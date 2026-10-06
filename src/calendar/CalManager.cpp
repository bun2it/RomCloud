#include "CalManager.h"
#include "../database/DatabaseManager.h"
#include "../network/HttpClient.h"
#include "../logging/Logger.h"

#include "../common/TimeZone.h"
#include <sqlite3.h>
#include <sstream>
#include <ctime>
#include <cstdio>
#include <mutex>
#include <cstdlib>
#include <algorithm>
#include <unordered_map>

namespace RomCloud {

CalManager& CalManager::instance() {
    static CalManager inst;
    return inst;
}

void CalManager::ensureTables() {
    DatabaseManager::instance().exec(
        "CREATE TABLE IF NOT EXISTS cal_sources ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL, url TEXT NOT NULL UNIQUE,"
        "last_refresh INTEGER NOT NULL DEFAULT 0);");
    DatabaseManager::instance().exec(
        "CREATE TABLE IF NOT EXISTS cal_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "source TEXT NOT NULL, uid TEXT NOT NULL,"
        "title TEXT NOT NULL, start_ts INTEGER NOT NULL,"
        "end_ts INTEGER NOT NULL, allday INTEGER NOT NULL DEFAULT 0,"
        "remind INTEGER NOT NULL DEFAULT 0);");
    // Migration DB cũ: thêm cột remind (bỏ qua lỗi nếu đã có).
    DatabaseManager::instance().exec(
        "ALTER TABLE cal_events ADD COLUMN remind INTEGER NOT NULL DEFAULT 0;");
    DatabaseManager::instance().exec(
        "CREATE INDEX IF NOT EXISTS idx_cal_events_start ON cal_events(start_ts);");
}

// Gỡ line folding RFC5545: dòng bắt đầu space/tab nối vào dòng trước
std::string CalManager::unfold(const std::string& ics) {
    std::string out;
    out.reserve(ics.size());
    std::istringstream ss(ics);
    std::string line;
    bool first = true;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!first && !line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            out += line.substr(1);
        } else {
            if (!first) out += '\n';
            out += line;
        }
        first = false;
    }
    return out;
}

// Lấy value của prop (bỏ qua tham số ;...): "DTSTART;TZID=X:val" -> "val"
std::string CalManager::prop(const std::string& vevent, const std::string& key) {
    size_t pos = vevent.find(key);
    while (pos != std::string::npos) {
        // key phải ở đầu dòng
        if (pos == 0 || vevent[pos - 1] == '\n') break;
        pos = vevent.find(key, pos + 1);
    }
    if (pos == std::string::npos) return "";
    size_t colon = vevent.find(':', pos);
    if (colon == std::string::npos) return "";
    size_t eol = vevent.find('\n', colon);
    std::string val = vevent.substr(colon + 1, eol == std::string::npos
                                                 ? std::string::npos
                                                 : eol - colon - 1);
    if (!val.empty() && val.back() == '\r') val.pop_back();
    return val;
}

// Lấy tham số của prop: propParam(vev,"DTSTART","TZID") -> "America/..."
std::string CalManager::propParam(const std::string& vevent, const std::string& key,
                                  const std::string& param) {
    size_t pos = vevent.find(key);
    while (pos != std::string::npos) {
        if (pos == 0 || vevent[pos - 1] == '\n') break;
        pos = vevent.find(key, pos + 1);
    }
    if (pos == std::string::npos) return "";
    size_t colon = vevent.find(':', pos);
    if (colon == std::string::npos) return "";
    std::string head = vevent.substr(pos, colon - pos);
    std::string needle = param + "=";
    size_t pp = head.find(needle);
    if (pp == std::string::npos) return "";
    size_t v0 = pp + needle.length();
    size_t v1 = head.find(';', v0);
    return head.substr(v0, v1 == std::string::npos ? std::string::npos : v1 - v0);
}


// mktime với TZID cụ thể (dùng zoneinfo hệ thống). KHÔNG setenv trực tiếp
// (race giữa các thread) — đi qua TimeZone::offsetFor có mutex chung.
int64_t CalManager::mktimeTz(struct tm& t, const std::string& tzid) {
    if (tzid.empty()) return (int64_t)mktime(&t);
    struct tm tc = t;
    int64_t approx = (int64_t)mktime(&tc); // coi như giờ máy trước
    long offTz = TimeZone::offsetFor(tzid, (std::time_t)approx);
    long offDev = TimeZone::offsetFor("", (std::time_t)approx);
    return approx + (offDev - offTz);
}

// "20261004" / "20261004T090000Z" / "20261004T090000" -> epoch local.
// TZID: đổi sang giờ local qua zoneinfo (VD America/Los_Angeles).
int64_t CalManager::parseDt(const std::string& val, bool& allDay,
                            const std::string& tzid) {
    allDay = false;
    if (val.size() < 8) return 0;
    struct tm t = {};
    t.tm_year = atoi(val.substr(0, 4).c_str()) - 1900;
    t.tm_mon = atoi(val.substr(4, 2).c_str()) - 1;
    t.tm_mday = atoi(val.substr(6, 2).c_str());
    t.tm_isdst = -1;
    if (val.size() == 8) {
        allDay = true;
        return mktimeTz(t, "");
    }
    size_t tp = val.find('T');
    if (tp == std::string::npos || val.size() < tp + 7) return 0;
    t.tm_hour = atoi(val.substr(tp + 1, 2).c_str());
    t.tm_min = atoi(val.substr(tp + 3, 2).c_str());
    t.tm_sec = (val.size() >= tp + 7) ? atoi(val.substr(tp + 5, 2).c_str()) : 0;
    bool utc = !val.empty() && val.back() == 'Z';
    if (utc) {
#if defined(__GLIBC__) || defined(__linux__)
        return (int64_t)timegm(&t);
#else
        // Fallback: mktime rồi trừ offset (gần đúng)
        time_t local = mktime(&t);
        struct tm* g = gmtime(&local);
        struct tm* l = localtime(&local);
        long off = (l->tm_hour - g->tm_hour) * 3600;
        return (int64_t)local - off;
#endif
    }
    return mktimeTz(t, tzid);
}

std::vector<CalManager::RawEvent> CalManager::expand(const RawEvent& e) {
    std::vector<CalManager::RawEvent> out;
    if (e.startTs <= 0) return out;
    if (e.rrule.empty()) {
        out.push_back(e);
        return out;
    }
    std::string freq;
    long interval = 1, count = -1;
    int64_t until = 0;
    std::vector<int> byday; // 0=CN..6=T7 (tm_wday)
    {
        std::stringstream ss(e.rrule);
        std::string tok;
        while (std::getline(ss, tok, ';')) {
            if (tok.rfind("FREQ=", 0) == 0) freq = tok.substr(5);
            else if (tok.rfind("INTERVAL=", 0) == 0) interval = atol(tok.substr(9).c_str());
            else if (tok.rfind("COUNT=", 0) == 0) count = atol(tok.substr(6).c_str());
            else if (tok.rfind("UNTIL=", 0) == 0) {
                bool ad = false;
                until = parseDt(tok.substr(6), ad);
            } else if (tok.rfind("BYDAY=", 0) == 0) {
                std::stringstream bs(tok.substr(6));
                std::string d;
                while (std::getline(bs, d, ',')) {
                    // Bỏ tiền tố số (VD 1MO tháng): chỉ lấy 2 chữ cuối
                    std::string dd = d.size() > 2 ? d.substr(d.size() - 2) : d;
                    if (dd == "SU") byday.push_back(0);
                    else if (dd == "MO") byday.push_back(1);
                    else if (dd == "TU") byday.push_back(2);
                    else if (dd == "WE") byday.push_back(3);
                    else if (dd == "TH") byday.push_back(4);
                    else if (dd == "FR") byday.push_back(5);
                    else if (dd == "SA") byday.push_back(6);
                }
            }
        }
    }
    if (interval < 1) interval = 1;
    int64_t horizon = (int64_t)std::time(nullptr) + 730L * 86400; // 2 năm
    int64_t dur = e.endTs > e.startTs ? e.endTs - e.startTs : 0;
    // Giờ:phút:giây gốc (giữ nguyên qua các instance)
    time_t es = (time_t)e.startTs;
    struct tm etm = *localtime(&es);
    auto emit = [&](int64_t ts, long& n) -> bool {
        if ((count >= 0 && n >= count) || ts > horizon) return false;
        if (until > 0 && ts > until) return false;
        if (ts < e.startTs) return true; // trước giờ bắt đầu: bỏ qua, tiếp tục
        RawEvent c = e;
        c.startTs = ts;
        c.endTs = ts + dur;
        c.rrule.clear();
        out.push_back(c);
        n++;
        return (count < 0 || n < count) && (int64_t)out.size() < 1000;
    };
    long n = 0;
    if (freq == "WEEKLY" && !byday.empty()) {
        // Lặp theo tuần: mỗi tuần sinh các ngày BYDAY
        std::sort(byday.begin(), byday.end());
        // Ngày đầu tuần (CN) chứa DTSTART
        struct tm wt = etm;
        wt.tm_hour = 0; wt.tm_min = 0; wt.tm_sec = 0;
        time_t weekStart = mktime(&wt);
        {
            struct tm* wlt = localtime(&weekStart);
            weekStart -= wlt->tm_wday * 86400L;
        }
        for (long w = 0; w < 150; ++w) { // ~3 năm, chặn bởi horizon/count
            for (int wd : byday) {
                int64_t day = (int64_t)weekStart + (w * 7 + wd) * 86400L;
                struct tm dt = *localtime((time_t*)&day);
                dt.tm_hour = etm.tm_hour;
                dt.tm_min = etm.tm_min;
                dt.tm_sec = etm.tm_sec;
                int64_t ts = (int64_t)mktime(&dt);
                if (!emit(ts, n)) {
                    if ((count >= 0 && n >= count) || ts > horizon) break;
                    continue;
                }
            }
            if ((count >= 0 && n >= count)) break;
            // Dừng sớm nếu tuần này đã vượt horizon và until
            int64_t probe = (int64_t)weekStart + (w * 7 + 7) * 86400L;
            if (probe > horizon && (until <= 0 || probe > until)) break;
            (void)interval; // WEEKLY+BYDAY: interval tuần (mở rộng sau nếu cần)
            if (interval > 1) {
                // Nhảy tuần theo interval: cộng thêm (interval-1) tuần
                weekStart += (interval - 1) * 7 * 86400L;
            }
        }
    } else {
        int64_t cur = e.startTs;
        while (n < 1000) {
            if (!emit(cur, n)) break;
            if (freq == "DAILY") {
                cur += interval * 86400L;
            } else if (freq == "WEEKLY") {
                cur += interval * 7 * 86400L;
            } else if (freq == "MONTHLY") {
                struct tm t = *localtime((time_t*)&cur);
                t.tm_mon += (int)interval;
                cur = (int64_t)mktime(&t);
            } else if (freq == "YEARLY") {
                struct tm t = *localtime((time_t*)&cur);
                t.tm_year += (int)interval;
                cur = (int64_t)mktime(&t);
            } else {
                break; // FREQ lạ: chỉ lấy 1 instance
            }
            if (cur <= e.startTs) break; // chống lặp vô hạn (DST...)
            if (cur > horizon) break;
        }
    }
    return out;
}

void CalManager::replaceSourceEvents(int64_t sourceId, const std::string& sourceName,
                                     const std::vector<RawEvent>& events) {
    DatabaseManager& db = DatabaseManager::instance();
    // Gộp 1 transaction: nhanh gấp chục lần, không khóa DB làm render lag
    db.beginTransaction();
    // Cách đơn giản an toàn: xóa theo source rồi insert lại
    // (dùng executeSimpleQuery vì DatabaseManager chưa có prepare public —
    // escape nháy đơn trong title/uid).
    auto esc = [](const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '\'') o += "''";
            else o += c;
        }
        return o;
    };
    // Giữ lựa chọn báo/không báo cũ theo uid (refresh không reset user).
    std::unordered_map<std::string, int> keepRemind;
    {
        char q[512];
        snprintf(q, sizeof(q),
                 "SELECT uid,remind FROM cal_events WHERE source='%s';",
                 esc(sourceName).c_str());
        db.query(q, [&](sqlite3_stmt* st) {
            const unsigned char* u = sqlite3_column_text(st, 0);
            keepRemind[u ? (const char*)u : ""] = sqlite3_column_int(st, 1);
        });
    }
    db.exec(("DELETE FROM cal_events WHERE source='" + esc(sourceName) + "';").c_str());
    (void)sourceId;
    for (const auto& e : events) {
        if (e.startTs <= 0 || e.title.empty()) continue;
        int rem = 0;
        auto it = keepRemind.find(e.uid);
        if (it != keepRemind.end()) rem = it->second;
        char sql[4096];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO cal_events(source,uid,title,start_ts,end_ts,allday,remind)"
                 " VALUES('%s','%s','%s',%lld,%lld,%d,%d);",
                 esc(sourceName).c_str(), esc(e.uid).c_str(), esc(e.title).c_str(),
                 (long long)e.startTs, (long long)e.endTs, e.allDay ? 1 : 0, rem);
        db.exec(sql);
    }
    db.commitTransaction();
}

int CalManager::importIcsText(const std::string& ics, const std::string& sourceName) {
    if (ics.empty() || sourceName.empty()) return -1;
    std::string body = unfold(ics);
    std::vector<RawEvent> all;
    size_t pos = 0;
    while ((pos = body.find("BEGIN:VEVENT", pos)) != std::string::npos) {
        size_t end = body.find("END:VEVENT", pos);
        if (end == std::string::npos) break;
        std::string vev = body.substr(pos, end - pos);
        RawEvent e;
        e.uid = prop(vev, "UID");
        e.title = prop(vev, "SUMMARY");
        // Unescape \, \; \n
        {
            std::string t;
            for (size_t i = 0; i < e.title.size(); ++i) {
                if (e.title[i] == '\\' && i + 1 < e.title.size()) {
                    char n = e.title[++i];
                    if (n == 'n' || n == 'N') t += '\n';
                    else if (n == ',') t += ',';
                    else if (n == ';') t += ';';
                    else t += n;
                } else t += e.title[i];
            }
            e.title = t;
        }
        bool adS = false, adE = false;
        std::string tzS = propParam(vev, "DTSTART", "TZID");
        std::string tzE = propParam(vev, "DTEND", "TZID");
        e.startTs = parseDt(prop(vev, "DTSTART"), adS, tzS);
        std::string dur = prop(vev, "DURATION");
        std::string dtend = prop(vev, "DTEND");
        if (!dtend.empty()) {
            e.endTs = parseDt(dtend, adE, tzE);
        } else if (!dur.empty() && dur[0] == 'P') {
            // DURATION đơn giản: PTnHnMnS / PnD
            long secs = 0;
            size_t tp = dur.find('T');
            std::string dp = tp == std::string::npos ? dur.substr(1) : dur.substr(1, tp - 1);
            // days trước T
            {
                size_t dd = dp.find('D');
                if (dd != std::string::npos) secs += atol(dp.substr(0, dd).c_str()) * 86400L;
            }
            if (tp != std::string::npos) {
                std::string tpart = dur.substr(tp + 1);
                // parse tuần tự (đủ cho ca thường gặp)
                long v = 0;
                for (char c : tpart) {
                    if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
                    else if (c == 'H') { secs += v * 3600; v = 0; }
                    else if (c == 'M') { secs += v * 60; v = 0; }
                    else if (c == 'S') { secs += v; v = 0; }
                }
            }
            e.endTs = e.startTs + secs;
        }
        e.allDay = adS;
        if (e.allDay && e.endTs <= e.startTs) e.endTs = e.startTs + 86400;
        e.rrule = prop(vev, "RRULE");
        auto expanded = expand(e);
        all.insert(all.end(), expanded.begin(), expanded.end());
        pos = end + 10;
    }
    // Ghi đè toàn bộ event của nguồn này
    replaceSourceEvents(0, sourceName, all);
    Logger::info("CalManager: imported " + std::to_string(all.size()) +
                 " events from '" + sourceName + "'");
    return (int)all.size();
}

int64_t CalManager::addUrlSource(const std::string& name, const std::string& url) {
    if (name.empty() || url.empty()) return -1;
    auto esc = [](const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '\'') o += "''";
            else o += c;
        }
        return o;
    };
    DatabaseManager::instance().exec(
        ("INSERT OR REPLACE INTO cal_sources(name,url,last_refresh)"
         " VALUES('" + esc(name) + "','" + esc(url) + "',0);").c_str());
    // Lấy id vừa insert
    // (đơn giản: query lại theo url unique)
    int64_t id = -1;
    // Dùng getSetting? Không — query trực tiếp qua sqlite handle riêng.
    // DatabaseManager không expose prepare; lấy id bằng cách liệt kê sources.
    for (const auto& s : sources()) {
        if (s.url == url) { id = s.id; break; }
    }
    return id;
}

bool CalManager::deleteSource(int64_t id) {
    std::string name;
    for (const auto& s : sources()) {
        if (s.id == id) { name = s.name; break; }
    }
    if (name.empty()) return false;
    auto esc = [](const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '\'') o += "''";
            else o += c;
        }
        return o;
    };
    DatabaseManager::instance().exec(
        ("DELETE FROM cal_events WHERE source='" + esc(name) + "';").c_str());
    char sql[128];
    snprintf(sql, sizeof(sql), "DELETE FROM cal_sources WHERE id=%lld;", (long long)id);
    DatabaseManager::instance().exec(sql);
    return true;
}

// Helper đọc rows: DatabaseManager có hàm query public nào không?
// Kiểm tra nhanh: dùng executeSimpleQuery cho write; read cần callback API.
// Xem DatabaseManager.h để bổ sung nếu thiếu.

namespace {
std::string sqlEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\'') o += "''";
        else o += c;
    }
    return o;
}
const char* colText(sqlite3_stmt* st, int c) {
    const unsigned char* t = sqlite3_column_text(st, c);
    return t ? (const char*)t : "";
}
}

std::vector<CalSource> CalManager::sources() {
    std::vector<CalSource> out;
    DatabaseManager::instance().query(
        "SELECT id,name,url,last_refresh FROM cal_sources ORDER BY name;",
        [&](sqlite3_stmt* st) {
            CalSource s;
            s.id = sqlite3_column_int64(st, 0);
            s.name = colText(st, 1);
            s.url = colText(st, 2);
            s.lastRefresh = sqlite3_column_int64(st, 3);
            out.push_back(s);
        });
    return out;
}

std::vector<CalEvent> CalManager::eventsForRange(int64_t fromTs, int64_t toTs) {
    std::vector<CalEvent> out;
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT id,source,uid,title,start_ts,end_ts,allday,remind FROM cal_events"
             " WHERE start_ts>=%lld AND start_ts<%lld ORDER BY start_ts LIMIT 500;",
             (long long)fromTs, (long long)toTs);
    DatabaseManager::instance().query(sql, [&](sqlite3_stmt* st) {
        CalEvent e;
        e.id = sqlite3_column_int64(st, 0);
        e.source = colText(st, 1);
        e.uid = colText(st, 2);
        e.title = colText(st, 3);
        e.startTs = sqlite3_column_int64(st, 4);
        e.endTs = sqlite3_column_int64(st, 5);
        e.allDay = sqlite3_column_int(st, 6) != 0;
        e.remind = sqlite3_column_int(st, 7) != 0;
        out.push_back(e);
    });
    return out;
}

std::vector<CalEvent> CalManager::upcoming(int limit) {
    if (limit <= 0 || limit > 100) limit = 10;
    int64_t now = (int64_t)std::time(nullptr);
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT id,source,uid,title,start_ts,end_ts,allday,remind FROM cal_events"
             " WHERE end_ts>=%lld ORDER BY start_ts LIMIT %d;",
             (long long)now, limit);
    std::vector<CalEvent> out;
    DatabaseManager::instance().query(sql, [&](sqlite3_stmt* st) {
        CalEvent e;
        e.id = sqlite3_column_int64(st, 0);
        e.source = colText(st, 1);
        e.uid = colText(st, 2);
        e.title = colText(st, 3);
        e.startTs = sqlite3_column_int64(st, 4);
        e.endTs = sqlite3_column_int64(st, 5);
        e.allDay = sqlite3_column_int(st, 6) != 0;
        e.remind = sqlite3_column_int(st, 7) != 0;
        out.push_back(e);
    });
    return out;
}

int64_t CalManager::addManual(const std::string& dateYmd, const std::string& timeHm,
                              const std::string& title) {
    if (title.empty() || dateYmd.size() < 10) return -1;
    struct tm t = {};
    t.tm_year = atoi(dateYmd.substr(0, 4).c_str()) - 1900;
    t.tm_mon = atoi(dateYmd.substr(5, 2).c_str()) - 1;
    t.tm_mday = atoi(dateYmd.substr(8, 2).c_str());
    t.tm_isdst = -1;
    int64_t start = 0, end = 0;
    bool allDay = timeHm.empty();
    if (allDay) {
        start = (int64_t)mktime(&t);
        end = start + 86400;
    } else {
        if (timeHm.size() < 5) return -1;
        t.tm_hour = atoi(timeHm.substr(0, 2).c_str());
        t.tm_min = atoi(timeHm.substr(3, 2).c_str());
        start = (int64_t)mktime(&t);
        end = start + 3600;
    }
    if (start <= 0) return -1;
    char sql[2048];
    snprintf(sql, sizeof(sql),
             "INSERT INTO cal_events(source,uid,title,start_ts,end_ts,allday,remind)"
             " VALUES('manual','manual-%lld','%s',%lld,%lld,%d,1);",
             (long long)std::time(nullptr), sqlEsc(title).c_str(),
             (long long)start, (long long)end, allDay ? 1 : 0);
    if (!DatabaseManager::instance().exec(sql)) return -1;
    return 0; // id không cần thiết cho manual
}

bool CalManager::deleteEvent(int64_t id) {
    char sql[128];
    snprintf(sql, sizeof(sql), "DELETE FROM cal_events WHERE id=%lld;", (long long)id);
    return DatabaseManager::instance().exec(sql);
}

bool CalManager::setRemind(int64_t id, bool on) {
    char sql[128];
    snprintf(sql, sizeof(sql), "UPDATE cal_events SET remind=%d WHERE id=%lld;",
             on ? 1 : 0, (long long)id);
    return DatabaseManager::instance().exec(sql);
}

int CalManager::refreshStale() {
    int64_t now = (int64_t)std::time(nullptr);
    int done = 0;
    for (const auto& s : sources()) {
        if (s.url.empty() || now - s.lastRefresh < 24 * 3600) continue;
        HttpResponse resp = HttpClient::instance().get(s.url);
        if (!resp.success || resp.body.empty() || resp.statusCode != 200) {
            Logger::warn("CalManager: refresh failed: " + s.name);
            continue;
        }
        int n = importIcsText(resp.body, s.name);
        if (n >= 0) {
            char sql[256];
            snprintf(sql, sizeof(sql),
                     "UPDATE cal_sources SET last_refresh=%lld WHERE id=%lld;",
                     (long long)now, (long long)s.id);
            DatabaseManager::instance().exec(sql);
            done++;
        }
    }
    return done;
}

int CalManager::refreshAll() {
    int64_t now = (int64_t)std::time(nullptr);
    int done = 0;
    for (const auto& s : sources()) {
        if (s.url.empty()) continue;
        HttpResponse resp = HttpClient::instance().get(s.url);
        if (!resp.success || resp.body.empty() || resp.statusCode != 200) {
            Logger::warn("CalManager: refresh failed: " + s.name);
            continue;
        }
        int n = importIcsText(resp.body, s.name);
        if (n >= 0) {
            char sql[256];
            snprintf(sql, sizeof(sql),
                     "UPDATE cal_sources SET last_refresh=%lld WHERE id=%lld;",
                     (long long)now, (long long)s.id);
            DatabaseManager::instance().exec(sql);
            done++;
        }
    }
    return done;
}

} // namespace RomCloud
