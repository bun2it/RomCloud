#pragma once
// ============================================================================
// RomCloud WeatherManager — thời tiết Open-Meteo (miễn phí, không key) +
// chọn phường/xã VN (2 cấp, offline) + lịch tháng (thuần C++, offline).
// Icon thời tiết: PNG user bổ sung sau (assets/weather_icons/<code>.png),
// chưa có thì vẽ hình khối.
// ============================================================================
#include <string>
#include <vector>

namespace RomCloud {

struct WxDay {
    std::string date;   // yyyy-mm-dd
    int code = 0;       // WMO weather code
    double tmax = 0, tmin = 0;
};

struct WxNow {
    bool ok = false;
    double temp = 0, hum = 0, wind = 0;
    int code = 0;
    std::string time;
};

struct WxPlace {
    std::string province;
    std::string ward;
    double lat = 0, lon = 0;
    bool valid = false;
};

struct WxGeoCand {
    std::string name;
    std::string admin;
    double lat = 0, lon = 0;
};

class WeatherManager {
public:
    static WeatherManager& instance();

    // Danh sách tỉnh -> phường/xã (assets/vn_wards.json, offline)
    bool loadWards();
    size_t provinceCount() const { return m_provinces.size(); }
    const std::string& provinceName(size_t i) const;
    const std::vector<std::string>& wardsOf(size_t i) const;

    // Vị trí đã chọn (lưu DB settings)
    WxPlace place() const { return m_place; }
    void loadPlace();
    // Geocode "phường, tỉnh" -> ứng viên (online, Open-Meteo, không key)
    std::vector<WxGeoCand> geocode(const std::string& ward,
                                   const std::string& province);
    void selectPlace(const std::string& province, const std::string& ward,
                     double lat, double lon);

    // Thời tiết (online, cache offline trong data/wx_cache.json)
    bool fetch(); // dùng m_place.lat/lon
    bool loadCache(); // đọc cache khi offline
    WxNow now() const { return m_now; }
    const std::vector<WxDay>& days() const { return m_days; }
    long cacheAgeSec() const; // -1 = chưa có cache

    // WMO code -> tiếng Việt + tên icon
    static const char* wmoLabel(int code);
    static const char* wmoIcon(int code); // sun/cloud/rain/snow/storm/fog

private:
    WeatherManager() = default;
    std::vector<std::string> m_provinces;
    std::vector<std::vector<std::string>> m_wards;
    WxPlace m_place;
    WxNow m_now;
    std::vector<WxDay> m_days;

    static std::string urlEncode(const std::string& s);
    static double extractDouble(const std::string& json, const std::string& key,
                                double def = 0);
    static int extractInt(const std::string& json, const std::string& key,
                          int def = 0);
    // Mảng số trong "key":[...] bắt đầu từ vị trí pos (0 = tìm từ đầu)
    static std::vector<double> extractNumArray(const std::string& json,
                                              const std::string& key,
                                              size_t fromPos = 0);
    static std::vector<std::string> extractStrArray(const std::string& json,
                                                   const std::string& key,
                                                   size_t fromPos = 0);
    std::string cachePath() const;
};

} // namespace RomCloud
