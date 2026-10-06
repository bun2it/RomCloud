#include "FloodManager.h"
#include "../config/AppConfig.h"
#include "../logging/Logger.h"
#include "../network/HttpClient.h"
#include "../tide/TideManager.h"

#include <ctime>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sys/stat.h>

namespace RomCloud {

FloodManager& FloodManager::instance() {
    static FloodManager inst;
    return inst;
}

bool FloodManager::load() {
    if (!m_pts.empty()) return true;
    std::string path = AppConfig::instance().getAssetsDir() + "/flood_points.json";
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        Logger::warn("FloodManager: missing " + path);
        return false;
    }
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    // Parser gọn cùng kiểu CameraManager (string scan, không thêm dep JSON).
    size_t pos = 0;
    auto getStr = [&](size_t from, size_t endPos, const char* key, std::string& out) -> size_t {
        std::string pat = std::string("\"") + key + "\"";
        size_t p = body.find(pat, from);
        if (p == std::string::npos || p >= endPos) return std::string::npos;
        size_t c0 = body.find(':', p);
        size_t q0 = body.find('"', c0);
        if (q0 == std::string::npos || q0 > endPos) return std::string::npos;
        size_t q = q0 + 1;
        std::string val;
        while (q < body.size()) {
            char ch = body[q];
            if (ch == '\\' && q + 1 < body.size()) {
                char nx = body[q + 1];
                if (nx == '"') val += '"';
                else if (nx == 'n') val += ' ';
                else if (nx == 'u') { q += 6; continue; }
                else { val += ch; val += nx; }
                q += 2;
            } else if (ch == '"') {
                break;
            } else {
                val += ch;
                q++;
            }
        }
        out = val;
        return q;
    };
    auto getInt = [&](size_t from, size_t endPos, const char* key, int& out) -> size_t {
        std::string pat = std::string("\"") + key + "\"";
        size_t p = body.find(pat, from);
        if (p == std::string::npos || p >= endPos) return std::string::npos;
        size_t c0 = body.find(':', p);
        if (c0 == std::string::npos || c0 > endPos) return std::string::npos;
        out = std::atoi(body.c_str() + c0 + 1);
        return c0 + 1;
    };
    while ((pos = body.find('{', pos)) != std::string::npos) {
        size_t end = body.find('}', pos);
        if (end == std::string::npos) break;
        FloodPoint pt;
        std::string n;
        size_t q = getStr(pos, end, "n", n);
        if (q == std::string::npos || n.empty()) { pos = end + 1; continue; }
        pt.name = n;
        getStr(q, end, "w", pt.ward);
        getStr(q, end, "c", pt.cause);
        int lvl = 1;
        if (getInt(q, end, "lvl", lvl) != std::string::npos)
            pt.lvl = (lvl < 0) ? 0 : (lvl > 2 ? 2 : lvl);
        if (pt.cause.empty()) pt.cause = "mưa";
        // Tọa độ WGS84 (optional, bản đồ chấm).
        {
            double la = 0, lo = 0;
            size_t pl = body.find("\"lat\"", pos);
            if (pl != std::string::npos && pl < end) {
                size_t cb = body.find(':', pl) + 1;
                la = std::atof(body.c_str() + cb);
            }
            size_t pn = body.find("\"lon\"", pos);
            if (pn != std::string::npos && pn < end) {
                size_t cb = body.find(':', pn) + 1;
                lo = std::atof(body.c_str() + cb);
            }
            if (la > 0 && lo > 0) { pt.lat = la; pt.lon = lo; pt.hasGeo = true; }
        }
        m_pts.push_back(std::move(pt));
        pos = end + 1;
    }
    Logger::info("FloodManager: loaded " + std::to_string(m_pts.size()) + " flood points");
    loadLiveCache();
    return !m_pts.empty();
}

// ---- Live: mưa (Open-Meteo, tọa độ HCMC cố định) + triều (TideManager) ----

std::string FloodManager::liveCachePath() const {
    return AppConfig::instance().getDataDir() + "/flood_live.json";
}

std::string FloodManager::rainCachePath() const {
    return AppConfig::instance().getDataDir() + "/rain_cache.json";
}

bool FloodManager::liveFresh() const {
    if (!m_live.ok) return false;
    long age = (long)std::time(nullptr) - m_live.fetchedAt;
    return age >= 0 && age < 7200; // 2h
}

// Quét mảng JSON đơn giản cùng kiểu parser gọn trong app.
static std::vector<std::string> scanStrArr(const std::string& j, const char* key, size_t from) {
    std::vector<std::string> o;
    std::string pat = std::string("\"") + key + "\"";
    size_t p = j.find(pat, from);
    if (p == std::string::npos) return o;
    size_t b = j.find('[', p);
    size_t e = j.find(']', b);
    if (b == std::string::npos || e == std::string::npos) return o;
    size_t q = b;
    while ((q = j.find('"', q)) != std::string::npos && q < e) {
        size_t q1 = j.find('"', q + 1);
        if (q1 == std::string::npos || q1 > e) break;
        o.push_back(j.substr(q + 1, q1 - q - 1));
        q = q1 + 1;
        if (o.size() > 2000) break;
    }
    return o;
}

static std::vector<double> scanNumArr(const std::string& j, const char* key, size_t from) {
    std::vector<double> o;
    std::string pat = std::string("\"") + key + "\"";
    size_t p = j.find(pat, from);
    if (p == std::string::npos) return o;
    size_t b = j.find('[', p);
    size_t e = j.find(']', b);
    if (b == std::string::npos || e == std::string::npos) return o;
    const char* c = j.c_str();
    size_t q = b;
    while (q < e) {
        while (q < e && !(c[q] == '-' || c[q] == '.' || (c[q] >= '0' && c[q] <= '9'))) q++;
        if (q >= e) break;
        o.push_back(std::atof(c + q));
        while (q < e && (c[q] == '-' || c[q] == '.' || c[q] == 'e' || c[q] == 'E' ||
                          c[q] == '+' || (c[q] >= '0' && c[q] <= '9'))) q++;
        if (o.size() > 2000) break;
    }
    return o;
}

// "2026-10-06T08:00" (UTC, Open-Meteo timezone=UTC) -> epoch.
static long parseHourEpoch(const std::string& s) {
    return TideManager::parseUtcEpoch(s);
}

static std::string fmtHCM(long epochUtc) {
    long loc = epochUtc + 7 * 3600;
    std::time_t t = (std::time_t)loc;
    struct tm* g = std::gmtime(&t);
    if (!g) return "--:--";
    char b[16];
    std::snprintf(b, sizeof(b), "%02d:%02d", g->tm_hour, g->tm_min);
    return b;
}

// Nấu risk + summary từ dữ liệu đã có (live mới hoặc cache cũ).
static void cookLive(FloodLive& lv, const std::vector<FloodPoint>& pts,
                     std::vector<int>& risk, double rp3, double rn3,
                     long tEp, double tLv, long at, bool stale) {
    lv.ok = true;
    lv.fetchedAt = at;
    lv.rainPast3h = rp3;
    lv.rainNext3h = rn3;
    lv.tideHighEpoch = tEp;
    lv.tideHighLevel = tLv;
    char sb[128];
    if (tEp > 0)
        std::snprintf(sb, sizeof(sb), "Mưa 3h %.0fmm • Triều %.1fm %s%s",
                      rp3 + rn3, tLv, fmtHCM(tEp).c_str(), stale ? " (cũ)" : "");
    else
        std::snprintf(sb, sizeof(sb), "Mưa 3h %.0fmm%s", rp3 + rn3,
                      stale ? " (cũ)" : "");
    lv.summary = sb;
    risk.assign(pts.size(), 0);
    long now = (long)std::time(nullptr);
    bool tHi = tEp > 0 && tLv >= 3.5 && (tEp - now) <= 86400;
    for (size_t i = 0; i < pts.size(); ++i) {
        const auto& pt = pts[i];
        bool hasRain = pt.cause.find("mưa") != std::string::npos;
        bool hasTide = pt.cause.find("triều") != std::string::npos;
        bool rWet = (rp3 >= 15 || rn3 >= 15);
        bool rHeavy = (rp3 >= 30 || rn3 >= 30 || rp3 + rn3 >= 40);
        int tier = 0;
        if (hasRain && rHeavy)
            tier = 2;
        else if (hasRain && rWet && hasTide && tHi)
            tier = 2;
        else if (hasTide && tHi && pt.lvl >= 2)
            tier = 2;
        else if (hasRain && rWet && pt.lvl >= 2)
            tier = 2;
        else if ((hasRain && rWet) || (hasTide && tHi) ||
                 (pt.lvl >= 2 && (rp3 >= 5 || rn3 >= 5)))
            tier = 1;
        risk[i] = tier;
    }
}

bool FloodManager::refreshLive() {
    if (m_pts.empty()) load();
    long now = (long)std::time(nullptr);
    // 1. Triều (tự fallback cache trong TideManager).
    TideManager::instance().refresh();
    long tEp = 0;
    double tLv = 0;
    if (TideManager::instance().hasData())
        TideManager::instance().nextHigh(now, tEp, tLv);
    // 2. Mưa HCMC (past_days=1 để có 3h qua, UTC để index theo epoch).
    double rp3 = 0, rn3 = 0;
    bool rainOk = false;
    {
        std::string url = "https://api.open-meteo.com/v1/forecast?latitude=10.7769&longitude=106.7009"
                          "&hourly=precipitation&past_days=1&forecast_days=2&timezone=UTC";
        HttpResponse resp = HttpClient::instance().get(url, {}, 20);
        std::string body;
        if (resp.success && !resp.body.empty() && resp.statusCode == 200) {
            body = resp.body;
            std::ofstream f(rainCachePath(), std::ios::trunc);
            if (f) f << body;
        } else {
            std::ifstream f(rainCachePath(), std::ios::binary);
            if (f) body.assign((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
        }
        size_t hp = body.find("\"hourly\"");
        if (hp != std::string::npos) {
            auto times = scanStrArr(body, "time", hp);
            auto prec = scanNumArr(body, "precipitation", hp);
            if (!times.empty() && times.size() == prec.size()) {
                int cur = -1;
                for (size_t i = 0; i < times.size(); ++i) {
                    if (parseHourEpoch(times[i]) <= now) cur = (int)i;
                    else break;
                }
                if (cur >= 0) {
                    for (int k = 0; k < 3; ++k) {
                        if (cur - k >= 0) rp3 += prec[cur - k];
                        if (cur + 1 + k < (int)prec.size()) rn3 += prec[cur + 1 + k];
                    }
                    rainOk = true;
                }
            }
        }
    }
    if (!rainOk && tEp == 0) {
        Logger::warn("FloodManager: live failed (no rain, no tide)");
        return loadLiveCache();
    }
    cookLive(m_live, m_pts, m_risk, rp3, rn3, tEp, tLv, now, false);
    // Persist meta để boot sau offline vẫn có.
    {
        std::ofstream f(liveCachePath(), std::ios::trunc);
        if (f) {
            f << "{\"at\":" << now << ",\"rp\":" << rp3 << ",\"rn\":" << rn3
              << ",\"te\":" << tEp << ",\"tl\":" << tLv << "}";
        }
    }
    Logger::info("FloodManager: live ok (" + m_live.summary + ")");
    return true;
}

bool FloodManager::loadLiveCache() {
    if (m_live.ok) return true;
    // Meta mới nhất (nếu có).
    long at = 0;
    double rp = 0, rn = 0, tl = 0;
    long te = 0;
    {
        std::ifstream f(liveCachePath(), std::ios::binary);
        if (f) {
            std::string b((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
            size_t p;
            p = b.find("\"at\"");  if (p != std::string::npos) at = std::atol(b.c_str() + b.find(':', p) + 1);
            p = b.find("\"rp\"");  if (p != std::string::npos) rp = std::atof(b.c_str() + b.find(':', p) + 1);
            p = b.find("\"rn\"");  if (p != std::string::npos) rn = std::atof(b.c_str() + b.find(':', p) + 1);
            p = b.find("\"te\"");  if (p != std::string::npos) te = std::atol(b.c_str() + b.find(':', p) + 1);
            p = b.find("\"tl\"");  if (p != std::string::npos) tl = std::atof(b.c_str() + b.find(':', p) + 1);
        }
    }
    if (at > 0 && !m_pts.empty()) {
        bool stale = ((long)std::time(nullptr) - at) >= 7200;
        cookLive(m_live, m_pts, m_risk, rp, rn, te, tl, at, stale);
        return true;
    }
    return false;
}

int FloodManager::riskOf(size_t i) const {
    if (!m_live.ok || i >= m_risk.size()) return -1;
    return m_risk[i];
}

} // namespace RomCloud
