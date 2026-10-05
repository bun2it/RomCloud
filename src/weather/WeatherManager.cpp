#include "WeatherManager.h"
#include "../config/AppConfig.h"
#include "../database/DatabaseManager.h"
#include "../network/HttpClient.h"
#include "../network/JsonHelper.h"
#include "../logging/Logger.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <sys/stat.h>

namespace RomCloud {

WeatherManager& WeatherManager::instance() {
    static WeatherManager inst;
    return inst;
}

std::string WeatherManager::urlEncode(const std::string& s) {
    std::ostringstream out;
    out << std::hex << std::uppercase;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else if (c == ' ') {
            out << "%20";
        } else {
            out << '%' << std::setw(2) << std::setfill('0') << (int)c;
        }
    }
    return out.str();
}

double WeatherManager::extractDouble(const std::string& json, const std::string& key, double def) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return def;
    size_t colon = json.find(':', pos + pattern.length());
    if (colon == std::string::npos) return def;
    size_t start = colon + 1;
    while (start < json.length() && (json[start] == ' ' || json[start] == '"')) start++;
    if (start < json.length() && json[start] == 'n') return def; // null
    try {
        return std::stod(json.substr(start));
    } catch (...) { return def; }
}

int WeatherManager::extractInt(const std::string& json, const std::string& key, int def) {
    return (int)extractDouble(json, key, def);
}

static size_t findArray(const std::string& json, const std::string& key, size_t fromPos) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern, fromPos);
    if (pos == std::string::npos) return std::string::npos;
    size_t br = json.find('[', pos + pattern.length());
    return br;
}

std::vector<double> WeatherManager::extractNumArray(const std::string& json,
                                                   const std::string& key,
                                                   size_t fromPos) {
    std::vector<double> out;
    size_t br = findArray(json, key, fromPos);
    if (br == std::string::npos) return out;
    size_t end = json.find(']', br);
    if (end == std::string::npos) return out;
    std::string inner = json.substr(br + 1, end - br - 1);
    std::stringstream ss(inner);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        try { out.push_back(std::stod(tok)); } catch (...) {}
    }
    return out;
}

std::vector<std::string> WeatherManager::extractStrArray(const std::string& json,
                                                        const std::string& key,
                                                        size_t fromPos) {
    std::vector<std::string> out;
    size_t br = findArray(json, key, fromPos);
    if (br == std::string::npos) return out;
    size_t end = json.find(']', br);
    if (end == std::string::npos) return out;
    std::string inner = json.substr(br + 1, end - br - 1);
    std::stringstream ss(inner);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        size_t a = tok.find('"'), b = tok.rfind('"');
        if (a != std::string::npos && b != std::string::npos && b > a)
            out.push_back(tok.substr(a + 1, b - a - 1));
    }
    return out;
}

bool WeatherManager::loadWards() {
    if (!m_provinces.empty()) return true;
    std::string path = AppConfig::instance().getAssetsDir() + "/vn_wards.json";
    std::ifstream f(path);
    if (!f) {
        Logger::warn("WeatherManager: missing " + path);
        return false;
    }
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    // Format gọn: [{"p":"Hà Nội","w":["Ba Đình",...]},...]
    size_t pos = 0;
    static const std::vector<std::string> kEmpty;
    while ((pos = body.find("\"p\"", pos)) != std::string::npos) {
        size_t c0 = body.find(':', pos);
        size_t q0 = body.find('"', c0);
        size_t q1 = body.find('"', q0 + 1);
        if (q0 == std::string::npos || q1 == std::string::npos) break;
        std::string prov = body.substr(q0 + 1, q1 - q0 - 1);
        size_t wb = body.find('[', q1);
        size_t we = body.find(']', wb);
        if (wb == std::string::npos || we == std::string::npos) break;
        std::vector<std::string> wards;
        size_t q = wb;
        while ((q = body.find('"', q)) != std::string::npos && q < we) {
            size_t qe = body.find('"', q + 1);
            if (qe == std::string::npos || qe > we) break;
            wards.push_back(body.substr(q + 1, qe - q - 1));
            q = qe + 1;
        }
        m_provinces.push_back(prov);
        m_wards.push_back(std::move(wards));
        pos = we + 1;
    }
    Logger::info("WeatherManager: loaded " + std::to_string(m_provinces.size()) +
                 " provinces from vn_wards.json");
    return !m_provinces.empty();
}

const std::string& WeatherManager::provinceName(size_t i) const {
    static const std::string kEmpty;
    return i < m_provinces.size() ? m_provinces[i] : kEmpty;
}

const std::vector<std::string>& WeatherManager::wardsOf(size_t i) const {
    static const std::vector<std::string> kEmpty;
    return i < m_wards.size() ? m_wards[i] : kEmpty;
}

void WeatherManager::loadPlace() {
    DatabaseManager& db = DatabaseManager::instance();
    m_place.province = db.getSetting("wx_province", "");
    m_place.ward = db.getSetting("wx_ward", "");
    try {
        m_place.lat = std::stod(db.getSetting("wx_lat", "0"));
        m_place.lon = std::stod(db.getSetting("wx_lon", "0"));
    } catch (...) { m_place.lat = m_place.lon = 0; }
    m_place.valid = !m_place.ward.empty() && m_place.lat != 0;
}

void WeatherManager::selectPlace(const std::string& province, const std::string& ward,
                                 double lat, double lon) {
    m_place.province = province;
    m_place.ward = ward;
    m_place.lat = lat;
    m_place.lon = lon;
    m_place.valid = true;
    DatabaseManager& db = DatabaseManager::instance();
    db.setSetting("wx_province", province);
    db.setSetting("wx_ward", ward);
    db.setSetting("wx_lat", std::to_string(lat));
    db.setSetting("wx_lon", std::to_string(lon));
}

std::vector<WxGeoCand> WeatherManager::geocode(const std::string& ward,
                                              const std::string& province) {
    std::vector<WxGeoCand> out;
    // Thử "phường, tỉnh" trước; geocoder thường không biết phường mới
    // (sáp nhập 2025) -> rỗng thì fallback về tỉnh (luôn có).
    std::vector<std::string> queries = {ward + ", " + province, province, ward};
    for (const auto& qraw : queries) {
        std::string q = urlEncode(qraw);
        std::string url = "https://geocoding-api.open-meteo.com/v1/search?name=" + q +
                          "&count=5&language=vi&format=json";
        HttpResponse resp = HttpClient::instance().get(url);
        if (!resp.success || resp.body.empty() || resp.statusCode != 200) {
            Logger::warn("WeatherManager: geocode HTTP failed for " + qraw);
            continue;
        }
        auto objs = JsonHelper::extractArrayObjects(resp.body, "results");
        for (const auto& o : objs) {
            WxGeoCand c;
            c.name = JsonHelper::extractString(o, "name");
            c.admin = JsonHelper::extractString(o, "admin1");
            std::string country = JsonHelper::extractString(o, "country");
            if (!country.empty() && country != "Việt Nam" && country != "Vietnam")
                continue; // chỉ lấy trong VN
            c.lat = extractDouble(o, "latitude", 0);
            c.lon = extractDouble(o, "longitude", 0);
            if (c.lat == 0) continue;
            out.push_back(c);
        }
        if (!out.empty()) {
            if (&qraw != &queries[0])
                Logger::info("WeatherManager: geocode fallback dùng '" + qraw + "'");
            break;
        }
    }
    if (out.empty())
        Logger::warn("WeatherManager: geocode empty for " + ward + ", " + province);
    return out;
}

std::string WeatherManager::cachePath() const {
    return AppConfig::instance().getDataDir() + "/wx_cache.json";
}

bool WeatherManager::fetch() {
    if (!m_place.valid) return false;
    std::ostringstream url;
    url << "https://api.open-meteo.com/v1/forecast?latitude=" << m_place.lat
        << "&longitude=" << m_place.lon
        << "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
        << "&daily=weather_code,temperature_2m_max,temperature_2m_min"
        << "&timezone=Asia%2FHo_Chi_Minh&forecast_days=4";
    HttpResponse resp = HttpClient::instance().get(url.str());
    if (!resp.success || resp.body.empty() || resp.statusCode != 200) {
        Logger::warn("WeatherManager: forecast fetch failed");
        return false;
    }
    // current (object đầu tiên sau "current")
    size_t cp = resp.body.find("\"current\"");
    std::string cur = cp == std::string::npos ? "" : resp.body.substr(cp);
    m_now.ok = true;
    m_now.temp = extractDouble(cur, "temperature_2m", 0);
    m_now.hum = extractDouble(cur, "relative_humidity_2m", 0);
    m_now.wind = extractDouble(cur, "wind_speed_10m", 0);
    m_now.code = extractInt(cur, "weather_code", 0);
    m_now.time = JsonHelper::extractString(cur, "time");
    // daily
    size_t dp = resp.body.find("\"daily\"");
    std::string daily = dp == std::string::npos ? "" : resp.body.substr(dp);
    auto dates = extractStrArray(daily, "time", 0);
    auto codes = extractNumArray(daily, "weather_code", 0);
    size_t mxp = daily.find("\"temperature_2m_max\"");
    auto tmax = extractNumArray(daily, "temperature_2m_max", 0);
    auto tmin = extractNumArray(daily, "temperature_2m_min", 0);
    (void)mxp;
    m_days.clear();
    for (size_t i = 0; i < dates.size() && i < 4; ++i) {
        WxDay d;
        d.date = dates[i];
        d.code = i < codes.size() ? (int)codes[i] : 0;
        d.tmax = i < tmax.size() ? tmax[i] : 0;
        d.tmin = i < tmin.size() ? tmin[i] : 0;
        m_days.push_back(d);
    }
    // Cache offline
    std::ofstream f(cachePath(), std::ios::trunc);
    if (f) {
        f << resp.body;
        f << "\n<!--wx_cached_at=" << std::time(nullptr) << "-->";
    }
    Logger::info("WeatherManager: forecast ok for " + m_place.ward);
    return true;
}

bool WeatherManager::loadCache() {
    std::ifstream f(cachePath());
    if (!f) return false;
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    if (body.size() < 100) return false;
    size_t cp = body.find("\"current\"");
    if (cp == std::string::npos) return false;
    std::string cur = body.substr(cp);
    m_now.ok = true;
    m_now.temp = extractDouble(cur, "temperature_2m", 0);
    m_now.hum = extractDouble(cur, "relative_humidity_2m", 0);
    m_now.wind = extractDouble(cur, "wind_speed_10m", 0);
    m_now.code = extractInt(cur, "weather_code", 0);
    m_now.time = JsonHelper::extractString(cur, "time");
    size_t dp = body.find("\"daily\"");
    std::string daily = dp == std::string::npos ? "" : body.substr(dp);
    auto dates = extractStrArray(daily, "time", 0);
    auto codes = extractNumArray(daily, "weather_code", 0);
    auto tmax = extractNumArray(daily, "temperature_2m_max", 0);
    auto tmin = extractNumArray(daily, "temperature_2m_min", 0);
    m_days.clear();
    for (size_t i = 0; i < dates.size() && i < 4; ++i) {
        WxDay d;
        d.date = dates[i];
        d.code = i < codes.size() ? (int)codes[i] : 0;
        d.tmax = i < tmax.size() ? tmax[i] : 0;
        d.tmin = i < tmin.size() ? tmin[i] : 0;
        m_days.push_back(d);
    }
    return !m_days.empty();
}

long WeatherManager::cacheAgeSec() const {
    struct stat st;
    if (stat(cachePath().c_str(), &st) != 0) return -1;
    long age = (long)std::time(nullptr) - (long)st.st_mtime;
    return age < 0 ? 0 : age;
}

const char* WeatherManager::wmoLabel(int code) {
    if (code == 0) return "Trời quang";
    if (code == 1) return "Ít mây";
    if (code == 2) return "Mây rải rác";
    if (code == 3) return "Nhiều mây";
    if (code == 45 || code == 48) return "Sương mù";
    if ((code >= 51 && code <= 57) || (code >= 80 && code <= 82)) return "Mưa";
    if ((code >= 61 && code <= 67) || code == 95) return "Mưa to";
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "Tuyết";
    if (code == 96 || code == 99) return "Dông đá";
    return "Nhiều mây";
}

const char* WeatherManager::wmoIcon(int code) {
    // Khớp file thật trong assets/weather_icons/ (user bổ sung)
    if (code == 0 || code == 1) return "sun";
    if (code == 2) return "partly";
    if (code == 45 || code == 48) return "cloud";
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "cloud";
    if (code == 95 || code == 96 || code == 99) return "storm";
    if ((code >= 61 && code <= 67)) return "heavy_rain";
    if (code == 3) return "cloud";
    return "rain";
}

} // namespace RomCloud
