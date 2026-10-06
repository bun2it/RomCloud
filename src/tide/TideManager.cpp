#include "TideManager.h"
#include "../config/AppConfig.h"
#include "../logging/Logger.h"
#include "../network/HttpClient.h"

#include <ctime>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sys/stat.h>

namespace RomCloud {

TideManager& TideManager::instance() {
    static TideManager inst;
    return inst;
}

std::string TideManager::cachePath() const {
    return AppConfig::instance().getDataDir() + "/tide_cache.json";
}

long TideManager::parseUtcEpoch(const std::string& s) {
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) < 5)
        return 0;
    struct tm t = {};
    t.tm_year = Y - 1900;
    t.tm_mon = M - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min = m;
    t.tm_sec = sec;
#if defined(_WIN32)
    return (long)_mkgmtime(&t);
#else
    return (long)timegm(&t);
#endif
}

static bool parseExtremes(const std::string& body, std::vector<TideExtreme>& out) {
    size_t ap = body.find("\"extremes\"");
    if (ap == std::string::npos) return false;
    size_t ab = body.find('[', ap);
    size_t ae = body.rfind(']');
    if (ab == std::string::npos || ae == std::string::npos || ae <= ab) return false;
    std::vector<TideExtreme> v;
    size_t pos = ab;
    while (true) {
        size_t p = body.find("\"time\"", pos);
        if (p == std::string::npos || p >= ae) break;
        size_t c0 = body.find(':', p);
        size_t q0 = body.find('"', c0);
        size_t q1 = body.find('"', q0 + 1);
        if (q0 == std::string::npos || q1 == std::string::npos || q1 >= ae) break;
        std::string ts = body.substr(q0 + 1, q1 - q0 - 1);
        size_t pl = body.find("\"level\"", q1);
        if (pl == std::string::npos || pl >= ae) break;
        double lvl = std::atof(body.c_str() + body.find(':', pl) + 1);
        size_t ph = body.find("\"high\"", pl);
        bool high = false;
        if (ph != std::string::npos && ph < ae) {
            size_t cb = body.find(':', ph) + 1;
            high = body.compare(cb, 4, "true") == 0;
        }
        long ep = TideManager::parseUtcEpoch(ts);
        if (ep > 0) {
            TideExtreme e;
            e.epoch = ep;
            e.level = lvl;
            e.high = high;
            v.push_back(e);
        }
        pos = q1 + 1;
        if (v.size() > 200) break;
    }
    if (v.empty()) return false;
    out = std::move(v);
    return true;
}

bool TideManager::refresh() {
    // Trạm Vũng Tàu (gần HCMC nhất, reference station).
    std::string url = "https://api.openwaters.io/tides/extremes?latitude=10.34&longitude=107.08";
    HttpResponse resp = HttpClient::instance().get(url, {}, 20);
    if (resp.success && !resp.body.empty() && resp.statusCode == 200) {
        std::vector<TideExtreme> v;
        if (parseExtremes(resp.body, v)) {
            m_ext = std::move(v);
            std::ofstream f(cachePath(), std::ios::trunc);
            if (f) f << resp.body;
            Logger::info("TideManager: live ok, " + std::to_string(m_ext.size()) + " extremes");
            return true;
        }
        Logger::warn("TideManager: parse live failed, fallback cache");
    } else {
        Logger::warn("TideManager: fetch failed, fallback cache");
    }
    return loadCache();
}

bool TideManager::loadCache() {
    if (!m_ext.empty()) return true;
    std::ifstream f(cachePath(), std::ios::binary);
    if (!f) return false;
    std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (body.size() < 100) return false;
    std::vector<TideExtreme> v;
    if (!parseExtremes(body, v)) return false;
    m_ext = std::move(v);
    Logger::info("TideManager: loaded cache, " + std::to_string(m_ext.size()) + " extremes");
    return true;
}

long TideManager::cacheAgeSec() const {
    struct stat st;
    if (stat(cachePath().c_str(), &st) != 0) return -1;
    long age = (long)std::time(nullptr) - (long)st.st_mtime;
    return age < 0 ? 0 : age;
}

bool TideManager::nextHigh(long now, long &outEpoch, double &outLevel) const {
    for (const auto& e : m_ext) {
        if (e.high && e.epoch >= now - 1800) {
            outEpoch = e.epoch;
            outLevel = e.level;
            return true;
        }
    }
    return false;
}

double TideManager::maxHighNext24h(long now) const {
    double mx = -1;
    for (const auto& e : m_ext) {
        if (e.high && e.epoch > now && e.epoch <= now + 86400) {
            if (e.level > mx) mx = e.level;
        }
    }
    return mx;
}

} // namespace RomCloud
