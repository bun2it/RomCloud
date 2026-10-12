#include "YoutubeInnertube.h"
#include "HttpClient.h"
#include "../logging/Logger.h"
#include <cctype>
#include <cstring>
#include <cstdlib>

namespace RomCloud {

// Public embedded key + WEB client (JSON nhe, field on dinh).
static const char* kSearchUrl =
    "https://www.youtube.com/youtubei/v1/search?key="
    "AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8&prettyPrint=false";

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

// Doc chuoi JSON tu vi tri sau dau '"' mo. Tra ve (gia tri, vi tri sau
// dau '"' dong) hoac ("", npos) khi loi.
static std::pair<std::string, size_t> readJsonString(const std::string& j,
                                                     size_t pos) {
    std::string out;
    size_t i = pos;
    while (i < j.size()) {
        char c = j[i];
        if (c == '"') return {out, i + 1};
        if (c == '\\' && i + 1 < j.size()) {
            char e = j[i + 1];
            if (e == '"') out += '"';
            else if (e == '\\') out += '\\';
            else if (e == '/') out += '/';
            else if (e == 'n') out += ' ';
            else if (e == 'r' || e == 't') out += ' ';
            else if (e == 'u' && i + 5 < j.size()) {
                unsigned cp = 0;
                bool ok = true;
                for (int k = 0; k < 4; ++k) {
                    char h = j[i + 2 + k];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                    else { ok = false; break; }
                }
                if (ok) {
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                    i += 6;
                    continue;
                } else {
                    out += ' ';
                }
                i += 2;
                continue;
            } else {
                out += ' ';
            }
            i += 2;
            continue;
        }
        out += c;
        ++i;
    }
    return {"", std::string::npos};
}

// Tim "\"key\":\"value\"" tu 'from', gioi han trong 'limit' ky tu.
static std::string findField(const std::string& j, const std::string& key,
                             size_t from, size_t limit) {
    std::string pat = "\"" + key + "\":\"";
    size_t p = j.find(pat, from);
    if (p == std::string::npos || p - from > limit) return std::string();
    auto r = readJsonString(j, p + pat.size());
    return r.first;
}

// "12:34" / "1:02:03" -> giay; "LIVE"/rong -> 0.
static int parseDuration(const std::string& s) {
    int parts[3] = {0, 0, 0};
    int n = 0, cur = 0;
    bool anyDigit = false, bad = false;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            anyDigit = true;
        } else if (c == ':') {
            if (n >= 3) { bad = true; break; }
            parts[n++] = cur;
            cur = 0;
        } else {
            bad = true;
            break;
        }
    }
    if (bad || !anyDigit) return 0;
    if (n == 0) return cur;                    // "57"
    if (n == 1) return parts[0] * 60 + cur;    // "12:34"
    if (n == 2) return parts[0] * 3600 + parts[1] * 60 + cur; // "1:02:03"
    return 0;
}

// "1,234 views" / "1.2M views" / "No views" -> so.
static long long parseViews(const std::string& s) {
    double v = 0;
    bool anyDigit = false, hasDot = false;
    double frac = 0.1;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            anyDigit = true;
            if (!hasDot) v = v * 10 + (c - '0');
            else { v += (c - '0') * frac; frac *= 0.1; }
        } else if (c == '.') {
            hasDot = true;
        } else if (c == ',') {
            continue;
        } else if (c == 'K' || c == 'k') {
            v *= 1000; break;
        } else if (c == 'M' || c == 'm') {
            v *= 1000000; break;
        } else if (c == 'B' || c == 'b') {
            v *= 1000000000; break;
        } else if (c == ' ') {
            continue;
        } else {
            break;
        }
    }
    return anyDigit ? (long long)v : 0;
}

static std::string cleanPipe(std::string s) {
    for (char& c : s) {
        if (c == '|' || c == '\n' || c == '\r') c = ' ';
    }
    // Gon khoang trang ke nhau (title YouTube hay thua khoang).
    std::string out;
    out.reserve(s.size());
    bool lastSpace = true; // bo khoang trang dau
    for (char c : s) {
        bool sp = (c == ' ' || c == '\t');
        if (sp && lastSpace) continue;
        out += c;
        lastSpace = sp;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// POST + parse videoRenderer (dung chung cho search va browse).
// Khai bao truoc de search() goi (dinh nghia o duoi).
static std::vector<std::string> postAndParse(const std::string& url,
                                             const std::string& body,
                                             int maxResults,
                                             const std::string& logTag,
                                             const std::string& queryKey = "");
// Chuan hoa de so khop ten kenh (khong dau, khong hoa/thuong, bo khoang).
static uint32_t utf8Decode(const std::string& s, size_t& i) {
    unsigned char c = s[i];
    if (c < 0x80) return s[i++];
    uint32_t cp = 0;
    size_t extra = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { ++i; return 0xFFFD; }
    if (i + extra >= s.size()) { i = s.size(); return 0xFFFD; }
    for (size_t k = 0; k < extra; ++k) {
        unsigned char d = s[++i];
        if ((d & 0xC0) != 0x80) { ++i; return 0xFFFD; }
        cp = (cp << 6) | (d & 0x3F);
    }
    ++i;
    return cp;
}

static char baseAscii(uint32_t cp) {
    if (cp < 0x80) {
        if (cp >= 'A' && cp <= 'Z') return (char)(cp + 32);
        if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) return (char)cp;
        return ' ';
    }
    // Latin-1 + Vietnamese.
    switch (cp) {
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5:
        case 0x102: case 0x103:
        case 0x1EA0: case 0x1EA1: case 0x1EA2: case 0x1EA3: case 0x1EA4:
        case 0x1EA5: case 0x1EA6: case 0x1EA7: case 0x1EA8: case 0x1EA9:
        case 0x1EAA: case 0x1EAB: case 0x1EAC: case 0x1EAD: case 0x1EAE:
        case 0x1EAF: case 0x1EB0: case 0x1EB1: case 0x1EB2: case 0x1EB3:
        case 0x1EB4: case 0x1EB5: case 0x1EB6: case 0x1EB7: return 'a';
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xE8: case 0xE9: case 0xEA: case 0xEB:
        case 0x1EB8: case 0x1EB9: case 0x1EBA: case 0x1EBB: case 0x1EBC:
        case 0x1EBD: case 0x1EBE: case 0x1EBF: case 0x1EC0: case 0x1EC1:
        case 0x1EC2: case 0x1EC3: case 0x1EC4: case 0x1EC5: case 0x1EC6:
        case 0x1EC7: return 'e';
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        case 0xEC: case 0xED: case 0xEE: case 0xEF:
        case 0x128: case 0x129:
        case 0x1EC8: case 0x1EC9: case 0x1ECA: case 0x1ECB: return 'i';
        case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6:
        case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6:
        case 0x1A0: case 0x1A1:
        case 0x1ECC: case 0x1ECD: case 0x1ECE: case 0x1ECF: case 0x1ED0:
        case 0x1ED1: case 0x1ED2: case 0x1ED3: case 0x1ED4: case 0x1ED5:
        case 0x1ED6: case 0x1ED7: case 0x1ED8: case 0x1ED9: case 0x1EDA:
        case 0x1EDB: case 0x1EDC: case 0x1EDD: case 0x1EDE: case 0x1EDF:
        case 0x1EE0: case 0x1EE1: case 0x1EE2: case 0x1EE3: return 'o';
        case 0xD9: case 0xDA: case 0xDB: case 0xDC:
        case 0xF9: case 0xFA: case 0xFB: case 0xFC:
        case 0x1AF: case 0x1B0: case 0x168: case 0x169:
        case 0x1EE4: case 0x1EE5: case 0x1EE6: case 0x1EE7: case 0x1EE8:
        case 0x1EE9: case 0x1EEA: case 0x1EEB: case 0x1EEC: case 0x1EED:
        case 0x1EEE: case 0x1EEF: case 0x1EF0: case 0x1EF1: return 'u';
        case 0xDD: case 0xFD: case 0xFF:
        case 0x1EF2: case 0x1EF3: case 0x1EF4: case 0x1EF5: case 0x1EF6:
        case 0x1EF7: case 0x1EF8: case 0x1EF9: return 'y';
        case 0xD0: case 0xF0: case 0x110: case 0x111: return 'd';
        case 0xD1: case 0xF1: return 'n';
        case 0xC7: case 0xE7: return 'c';
        default: return ' ';
    }
}

static std::string normKey(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        char c = baseAscii(utf8Decode(s, i));
        if (c != ' ') out += c;
    }
    return out;
}

std::vector<std::string> YoutubeInnertube::search(const std::string& query,
                                                 int maxResults,
                                                 const std::string& params) {
    // Tien to dac biet khong phai search thuong.
    if (query.size() >= 5 && query.compare(0, 5, "feed:") == 0) {
        std::string cat = query.substr(5);
        while (!cat.empty() && cat.front() == ' ') cat.erase(0, 1);
        while (!cat.empty() && cat.back() == ' ') cat.pop_back();
        return trending(cat, maxResults);
    }
    if (query.size() >= 8 && query.compare(0, 8, "channel:") == 0) {
        std::string cid = query.substr(8);
        while (!cid.empty() && cid.front() == ' ') cid.erase(0, 1);
        while (!cid.empty() && cid.back() == ' ') cid.pop_back();
        // RSS tra video moi nhat truoc; rong thi de duong cu (.py) lo.
        auto rss = channelVideos(cid, maxResults);
        if (!rss.empty())
            return rss;
        return {}; // duong cu .py lay video kenh
    }
    if (query.empty() || maxResults <= 0) return {};
    if (maxResults > 50) maxResults = 50;
    std::vector<std::string> out;

    std::string body =
        std::string("{\"context\":{\"client\":{\"clientName\":\"WEB\","
                    "\"clientVersion\":\"2.20241201.00.00\",\"hl\":\"en\","
                    "\"gl\":\"US\"}},\"query\":\"") +
        jsonEscape(query) + "\"";
    if (!params.empty())
        body += ",\"params\":\"" + params + "\"";
    body += "}";

    return postAndParse(kSearchUrl, body, maxResults,
                        "search '" + query + "'", normKey(query));
}

// POST + parse videoRenderer (dung chung cho search va browse).
static std::vector<std::string> postAndParse(
    const std::string& url, const std::string& body, int maxResults,
    const std::string& logTag, const std::string& queryKey) {
    std::vector<std::string> out;
    std::vector<std::string> headers = {
        "Content-Type: application/json",
        "User-Agent: Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0 Safari/537.36",
        "Accept-Language: en-US,en;q=0.9",
        "Origin: https://www.youtube.com"};
    HttpResponse r = HttpClient::instance().post(url, body, headers, 8);
    if (!r.success || r.body.empty() || r.statusCode != 200) {
        Logger::warn("[Innertube] " + logTag + " that bai (HTTP " +
                     std::to_string(r.statusCode) + ") -> fallback yt-dlp");
        return out;
    }

    const std::string& j = r.body;
    size_t pos = 0;
    // Hang kenh: chi hien khi tu khoa trung ten kenh (khong dau) — toi da
    // 1 hang, giong backend cu. Home/feed khong bao gio co hang kenh
    // (trending() da loc, day la van thu 2 cho search thuong).
    // queryKey rong (= browse/khong co query) thi bo qua kenh.
    int chanCount = 0;
    while ((int)out.size() < maxResults && chanCount < 3 && !queryKey.empty()) {
        size_t p = j.find("\"channelRenderer\":{", pos);
        if (p == std::string::npos) break;
        pos = p + 19;
        const size_t kWindow = 4000;
        std::string cid = findField(j, "channelId", pos, kWindow);
        std::string cname;
        {
            size_t tp = j.find("\"title\":{\"simpleText\":\"", pos);
            if (tp != std::string::npos && tp - pos < kWindow) {
                cname = readJsonString(
                            j, tp + strlen("\"title\":{\"simpleText\":\""))
                            .first;
            }
        }
        if (cname.empty() || cid.empty()) continue;
        // Trung khop: tu khoa nam trong ten kenh hoac nguoc lai.
        std::string tk = normKey(cname);
        bool match = false;
        if (!tk.empty()) {
            match = (tk.find(queryKey) != std::string::npos ||
                     queryKey.find(tk) != std::string::npos);
        }
        if (!match) continue;
        ++chanCount;
        // Avatar: thumbnail lon nhat (yt3, them https:).
        std::string avatar;
        {
            size_t tp = pos;
            std::string lastUrl;
            while ((tp = j.find("\"url\":\"//yt3.ggpht.com/", tp)) !=
                   std::string::npos &&
                   tp - pos < kWindow) {
                lastUrl = readJsonString(j, tp + 7).first;
                tp += 32;
            }
            if (lastUrl.rfind("//", 0) == 0)
                avatar = "https:" + lastUrl;
            else
                avatar = lastUrl;
        }
        // Subs that: videoCountText.simpleText ("5.2M subscribers").
        // Chu y: trong block co them accessibility o giua nen tim block
        // truoc roi moi tim simpleText (khong match cung chuoi).
        std::string subs;
        {
            size_t vb = j.find("\"videoCountText\":{", pos);
            if (vb != std::string::npos && vb - pos < kWindow) {
                subs = findField(j, "simpleText", vb,
                                 kWindow - (vb - pos));
            }
        }
        // Dinh dang: CHANNEL|name|id|avatar|subs.
        out.push_back("CHANNEL|" + cleanPipe(cname) + "|" + cid + "|" +
                      avatar + "|" + cleanPipe(subs));
        if (chanCount >= 1) break; // nhu backend cu: toi da 1 hang kenh
    }
    pos = 0;
    while ((int)out.size() < maxResults) {
        size_t p = j.find("\"videoRenderer\":{", pos);
        if (p == std::string::npos) break;
        pos = p + 17;
        const size_t kWindow = 6000; // 1 videoRenderer ~2-4KB

        std::string vid = findField(j, "videoId", pos, kWindow);
        if (vid.empty() || vid.size() > 16) continue;

        // Trung id thi bo (Up Next / playlist lap lai).
        bool dup = false;
        for (const auto& l : out) {
            if (l.size() > vid.size() && l.compare(0, vid.size(), vid) == 0 &&
                l[vid.size()] == '|') {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        // title nam trongruns dau tien ke tu videoRenderer.
        std::string title;
        {
            size_t tp = j.find("\"title\":{\"runs\":[{\"text\":\"", pos);
            if (tp != std::string::npos && tp - pos < kWindow) {
                title = readJsonString(
                            j, tp + strlen("\"title\":{\"runs\":[{\"text\":\""))
                            .first;
            }
        }
        if (title.empty()) continue;

        std::string len;
        {
            // lengthText co block rieng; khong lay simpleText lung tung
            // (viewCount/accessibility cung co simpleText).
            size_t lp = j.find("\"lengthText\":{", pos);
            if (lp != std::string::npos && lp - pos < kWindow) {
                len = findField(j, "simpleText", lp, kWindow - (lp - pos));
            }
        }
        int dur = parseDuration(len);
        std::string owner;
        {
            size_t op = j.find("\"ownerText\":{\"runs\":[{\"text\":\"", pos);
            if (op != std::string::npos && op - pos < kWindow) {
                owner = readJsonString(
                            j, op + strlen("\"ownerText\":{\"runs\":[{\"text\":\""))
                            .first;
            }
        }
        std::string views = "0";
        {
            // viewCountText xuat hien truoc ownerText trong block; quet ca
            // window roi lay so cuoi (so gan video nhat o cuoi block).
            size_t vp = pos;
            std::string last;
            while ((vp = j.find("\"viewCountText\":{\"simpleText\":\"", vp)) !=
                   std::string::npos &&
                   vp - pos < kWindow) {
                last = readJsonString(
                           j, vp + strlen("\"viewCountText\":{\"simpleText\":\""))
                           .first;
                vp += 32;
            }
            if (!last.empty())
                views = std::to_string(parseViews(last));
        }

        out.push_back(vid + "|" + cleanPipe(title) + "|" +
                      std::to_string(dur) + "|" + cleanPipe(owner) + "|" +
                      views);
    }

    Logger::info("[Innertube] " + logTag + ": tim thay " +
                 std::to_string(out.size()) + " ket qua");
    return out;
}

// Trending: khong co Data API key nen dung fallback search giong backend
// cu (.py): nhac -> "nhac viet hay nhat", con lai -> "thinh hanh viet nam".
// (Browse FEtrending that: YouTube doi visitorData, khong lam duoc.)
std::vector<std::string> YoutubeInnertube::trending(const std::string& categoryId,
                                                   int maxResults) {
    std::string cat = categoryId;
    while (!cat.empty() && cat.front() == ' ') cat.erase(0, 1);
    while (!cat.empty() && cat.back() == ' ') cat.pop_back();
    std::string q =
        (cat == "10") ? "nhac viet hay nhat" : "thinh hanh viet nam";
    // Feed thi truong: chi lay video, bo hang kenh (sai ngu canh trending).
    std::vector<std::string> out;
    for (const auto& l : search(q, maxResults)) {
        if (l.compare(0, 8, "CHANNEL|") != 0)
            out.push_back(l);
    }
    return out;
}

// --- Resolve stream qua player API (ANDROID client cho ti le OK cao) ---
static const char* kPlayerUrl =
    "https://www.youtube.com/youtubei/v1/player?key="
    "AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8&prettyPrint=false";

// Doc so nguyen sau "\"key\":" (itag, width, height).
static int findInt(const std::string& j, const std::string& key, size_t from,
                   size_t limit) {
    std::string pat = "\"" + key + "\":";
    size_t p = j.find(pat, from);
    if (p == std::string::npos || p - from > limit) return -1;
    p += pat.size();
    int v = 0;
    bool any = false;
    while (p < j.size() && j[p] >= '0' && j[p] <= '9') {
        v = v * 10 + (j[p] - '0');
        any = true;
        ++p;
    }
    return any ? v : -1;
}

std::string YoutubeInnertube::resolveStream(const std::string& videoId,
                                            const std::string& quality) {
    if (videoId.empty() || videoId.size() > 16) return std::string();
    int limit = (quality == "720") ? 720 : 360;

    std::string body =
        std::string("{\"context\":{\"client\":{\"clientName\":\"ANDROID\","
                    "\"clientVersion\":\"20.10.35\",\"androidSdkVersion\":30,"
                    "\"hl\":\"en\",\"gl\":\"US\"}},\"videoId\":\"") +
        videoId +
        std::string("\",\"playbackContext\":{\"contentPlaybackContext\":{"
                    "\"html5Preference\":\"HTML5_PREF_WANTS\"}}}");

    std::vector<std::string> headers = {
        "Content-Type: application/json",
        "User-Agent: com.google.android.youtube/20.10.35 (Linux; U; Android "
        "13; en_US; Brick) gzip",
        "Accept-Language: en-US,en;q=0.9",
        "Origin: https://www.youtube.com"};
    HttpResponse r =
        HttpClient::instance().post(kPlayerUrl, body, headers, 8);
    if (!r.success || r.body.empty() || r.statusCode != 200) {
        Logger::warn("[Innertube] resolve that bai (HTTP " +
                     std::to_string(r.statusCode) + ") -> fallback yt-dlp");
        return std::string();
    }

    const std::string& j = r.body;
    // playabilityStatus phai OK (LOGIN_REQUIRED/UNPLAYABLE -> fallback).
    size_t ps = j.find("\"playabilityStatus\":{");
    if (ps == std::string::npos) return std::string();
    std::string status = findField(j, "status", ps, 400);
    if (status != "OK") {
        Logger::warn("[Innertube] video khong play duoc (" + status +
                     ") -> fallback yt-dlp");
        return std::string();
    }

    // Quet formats[] (progressive, co the co url truc tiep). Live khong
    // co formats[] (chi adaptive) -> bo qua vong nay, xuong adaptive.
    size_t fp = j.find("\"formats\":[", ps);
    size_t arrEnd = std::string::npos;
    if (fp != std::string::npos) {
        arrEnd = j.find("\"adaptiveFormats\":[", fp);
        if (arrEnd == std::string::npos) arrEnd = fp + 200000;
    }

    struct Cand { int itag; int height; std::string url; bool avc; };
    Cand best{-1, -1, "", false};
    if (fp != std::string::npos) {
    size_t p = fp;
    while (p < arrEnd) {
        size_t ob = j.find('{', p);
        if (ob == std::string::npos || ob > arrEnd) break;
        // Timig ngoac dong cua object (bo qua string).
        size_t q = ob + 1, depth = 1;
        bool inS = false, esc = false;
        while (q < j.size() && depth > 0) {
            char c = j[q];
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inS = !inS;
            else if (!inS && c == '{') ++depth;
            else if (!inS && c == '}') --depth;
            ++q;
        }
        std::string blk = j.substr(ob, q - ob);
        p = q;
        int itag = findInt(blk, "itag", 0, blk.size());
        if (itag < 0) continue;
        // Chi lay URL truc tiep (bo signatureCipher).
        std::string url;
        {
            size_t up = blk.find("\"url\":\"");
            if (up != std::string::npos)
                url = readJsonString(blk, up + 7).first;
        }
        if (url.empty()) continue;
        int height = findInt(blk, "height", 0, blk.size());
        if (height < 0) {
            // qualityLabel "720p" fallback.
            std::string ql = findField(blk, "qualityLabel", 0, blk.size());
            if (ql.size() > 1 && ql.back() == 'p')
                height = atoi(ql.c_str());
        }
        if (height <= 0 || height > limit) continue;
        bool avc = blk.find("avc1") != std::string::npos;
        // Uu tien: cao nhat trong gioi han, avc truoc.
        if (height > best.height ||
            (height == best.height && avc && !best.avc)) {
            best = {itag, height, url, avc};
        }
        if ((int)(p - fp) > 100000) break; // an toan
    }
    } // if (fp != npos): live khong co formats[]

    if (best.itag < 0) {
        Logger::info("[Innertube] khong co progressive truc tiep");
    }
    // Adaptive (video-only + audio) neu cao hon progressive tot nhat.
    // VD quality=720: progressive chi co itag18 (360p) -> phai lay
    // adaptive 720p keo ve, khong thi mai mai chi duoc 360p.
    if (limit >= 360) {
        size_t ap = j.find("\"adaptiveFormats\":[", ps);
        if (ap != std::string::npos) {
            size_t aEnd = ap + 400000;
            if (aEnd > j.size()) aEnd = j.size();
            Cand bv{-1, -1, "", false};
            std::string ba;
            size_t p2 = ap;
            while (p2 < aEnd) {
                size_t ob = j.find('{', p2);
                if (ob == std::string::npos || ob > aEnd) break;
                size_t q = ob + 1, depth = 1;
                bool inS = false, esc = false;
                while (q < j.size() && depth > 0) {
                    char c = j[q];
                    if (esc) esc = false;
                    else if (c == '\\') esc = true;
                    else if (c == '"') inS = !inS;
                    else if (!inS && c == '{') ++depth;
                    else if (!inS && c == '}') --depth;
                    ++q;
                }
                std::string blk = j.substr(ob, q - ob);
                p2 = q;
                size_t up = blk.find("\"url\":\"");
                if (up == std::string::npos) continue;
                std::string u = readJsonString(blk, up + 7).first;
                if (u.empty()) continue;
                bool isAudio = blk.find("\"audioQuality\"") !=
                                   std::string::npos ||
                               blk.find("audio/mp4") != std::string::npos ||
                               blk.find("audio/webm") != std::string::npos;
                if (isAudio) {
                    if (ba.empty())
                        ba = u;
                    continue;
                }
                int h = findInt(blk, "height", 0, blk.size());
                if (h <= 0 || h > limit) continue;
                bool avc = blk.find("avc1") != std::string::npos;
                // Bo 60fps tren may yeu (720p60 nang gap doi, de yt-dlp
                // lo; chi lay <=30fps).
                if (h >= 720 && blk.find("\"fps\":60") != std::string::npos)
                    continue;
                if (h > bv.height ||
                    (h == bv.height && avc && !bv.avc)) {
                    int it = findInt(blk, "itag", 0, blk.size());
                    bv = {it, h, u, avc};
                }
            }
            if (bv.itag >= 0 && !ba.empty() && bv.height > best.height) {
                Logger::info("[Innertube] resolve adaptive " +
                             std::to_string(bv.height) + "p + audio truc tiep");
                return bv.url + "|" + ba;
            }
        }
    }
    if (best.itag < 0) {
        Logger::info("[Innertube] khong co URL truc tiep -> "
                     "fallback yt-dlp");
        return std::string();
    }
    Logger::info("[Innertube] resolve itag" + std::to_string(best.itag) +
                 " " + std::to_string(best.height) + "p truc tiep");
    return best.url;
}

std::vector<std::string> YoutubeInnertube::channelVideos(
    const std::string& channelId, int maxResults) {
    std::vector<std::string> out;
    if (channelId.empty() || maxResults <= 0) return out;
    if (maxResults > 30) maxResults = 30;
    // Chi nhan UC-id that (chong injection duong dan).
    for (char c : channelId) {
        if (!(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') &&
            !(c >= '0' && c <= '9') && c != '_' && c != '-') {
            return out;
        }
    }
    std::string url = "https://www.youtube.com/feeds/videos.xml?channel_id=" +
                      channelId;
    std::vector<std::string> headers = {
        "User-Agent: Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0 Safari/537.36"};
    HttpResponse r = HttpClient::instance().get(url, headers, 8);
    if (!r.success || r.body.empty() || r.statusCode != 200) {
        Logger::warn("[Innertube] RSS kenh that bai (HTTP " +
                     std::to_string(r.statusCode) + ")");
        return out;
    }
    const std::string& x = r.body;
    size_t pos = 0;
    while ((int)out.size() < maxResults) {
        size_t e = x.find("<entry>", pos);
        if (e == std::string::npos) break;
        size_t eEnd = x.find("</entry>", e);
        if (eEnd == std::string::npos) break;
        std::string entry = x.substr(e, eEnd - e);
        pos = eEnd + 8;
        auto tag = [&entry](const char* t) {
            std::string o = std::string("<") + t + ">";
            std::string c = std::string("</") + t + ">";
            size_t a = entry.find(o);
            if (a == std::string::npos) return std::string();
            a += o.size();
            size_t b = entry.find(c, a);
            if (b == std::string::npos) return std::string();
            return entry.substr(a, b - a);
        };
        std::string vid = tag("yt:videoId");
        std::string title = tag("title");
        if (vid.empty() || vid.size() > 16 || title.empty()) continue;
        // Ten kenh (author) de autoplay cung kenh + hien thi.
        std::string author;
        {
            size_t a0 = entry.find("<author>");
            if (a0 != std::string::npos) {
                size_t n0 = entry.find("<name>", a0);
                size_t n1 = (n0 == std::string::npos)
                                ? std::string::npos
                                : entry.find("</name>", n0);
                if (n0 != std::string::npos && n1 != std::string::npos)
                    author = entry.substr(n0 + 6, n1 - n0 - 6);
            }
        }
        // Giai HTML entities co ban (&amp; &quot; &#39;).
        std::string t;
        for (size_t i = 0; i < title.size(); ++i) {
            if (title[i] == '&') {
                if (title.compare(i, 5, "&amp;") == 0) { t += '&'; i += 4; }
                else if (title.compare(i, 6, "&quot;") == 0) { t += '"'; i += 5; }
                else if (title.compare(i, 5, "&#39;") == 0) { t += '\''; i += 4; }
                else if (title.compare(i, 4, "&lt;") == 0) { t += '<'; i += 3; }
                else if (title.compare(i, 4, "&gt;") == 0) { t += '>'; i += 3; }
                else t += title[i];
            } else {
                t += title[i];
            }
        }
        out.push_back(vid + "|" + cleanPipe(t) + "|0|" + cleanPipe(author) +
                      "|0");
    }
    Logger::info("[Innertube] RSS kenh: tim thay " +
                 std::to_string(out.size()) + " video moi nhat");
    return out;
}

std::string YoutubeInnertube::videoDescription(const std::string& videoId) {
    if (videoId.empty() || videoId.size() > 16) return std::string();
    std::string body =
        std::string("{\"context\":{\"client\":{\"clientName\":\"ANDROID\","
                    "\"clientVersion\":\"20.10.35\",\"androidSdkVersion\":30,"
                    "\"hl\":\"en\",\"gl\":\"US\"}},\"videoId\":\"") +
        videoId + "\"}";
    std::vector<std::string> headers = {
        "Content-Type: application/json",
        "User-Agent: com.google.android.youtube/20.10.35 (Linux; U; Android "
        "13; en_US; Brick) gzip"};
    HttpResponse r =
        HttpClient::instance().post(kPlayerUrl, body, headers, 8);
    if (!r.success || r.body.empty() || r.statusCode != 200)
        return std::string();
    const std::string& j = r.body;
    size_t vd = j.find("\"videoDetails\":{");
    if (vd == std::string::npos) return std::string();
    std::string desc = findField(j, "shortDescription", vd, 200000);
    // Gon: doi newline thanh khoang, cat ~600 ky tu (giu UTF-8 nguyen).
    for (char& c : desc) {
        if (c == '\n' || c == '\r' || c == '\t' || c == '|')
            c = ' ';
    }
    if (desc.size() > 600) {
        size_t cut = 600;
        while (cut > 500 && (desc[cut] & 0xC0) == 0x80)
            --cut; // khong cat giua ky tu UTF-8
        desc.resize(cut);
        desc += "...";
    }
    return desc;
}

} // namespace RomCloud
