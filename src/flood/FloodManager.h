#pragma once
// RomCloud FloodManager — điểm ngập thường xuyên TP.HCM (offline).
// Data: assets/flood_points.json [{n:tên đường, w:phường, lvl:0 nhẹ/1 vừa/2 nặng, c:nguyên nhân}].
// Đợt 1: ~40 điểm tiêu biểu từ Sở Xây dựng (VnExpress/Thanh Niên 2026), mở rộng dần qua OTA.
#include <string>
#include <vector>

namespace RomCloud {

struct FloodPoint {
    std::string name;
    std::string ward;
    int lvl = 1;          // 0 = nhẹ, 1 = vừa, 2 = nặng (lịch sử)
    std::string cause;    // "mưa" | "triều" | "mưa+triều"
    double lat = 0, lon = 0; // WGS84 (Nominatim), 0 = chưa có
    bool hasGeo = false;
};

// Dữ liệu live (mưa Open-Meteo + triều Vũng Tàu), cache 2h.
struct FloodLive {
    bool ok = false;
    long fetchedAt = 0;
    double rainPast3h = 0;   // mm 3h qua (trạm HCMC)
    double rainNext3h = 0;   // mm 3h tới
    long tideHighEpoch = 0;  // đỉnh triều tiếp theo (UTC epoch)
    double tideHighLevel = 0;
    std::string summary;     // dòng header: "Mưa 3h 12mm • Triều 3.9m 23:59"
};

class FloodManager {
public:
    static FloodManager& instance();

    bool load();
    size_t count() const { return m_pts.size(); }
    const FloodPoint& at(size_t i) const { return m_pts[i]; }

    // Live: mưa + triều + risk/điểm. Chạy nền (blocking HTTP).
    // Hết mạng thì nấu lại từ cache cũ (summary gắn "cũ").
    bool refreshLive();
    bool loadLiveCache();
    bool liveFresh() const; // ok && < 2h
    const FloodLive& live() const { return m_live; }
    // Tier nguy cơ live: 2 = Cao, 1 = Chú ý, 0 = Thấp, -1 = chưa có live.
    int riskOf(size_t i) const;

private:
    FloodManager() = default;
    std::vector<FloodPoint> m_pts;
    FloodLive m_live;
    std::vector<int> m_risk;
    std::string liveCachePath() const;
    std::string rainCachePath() const;
};

} // namespace RomCloud
