#pragma once
// ============================================================================
// RomCloud MarketManager — giá thị trường live (free, không key):
// - Xăng/dầu: VietFuel (vùng 1/2, fallback checkgia vùng 1)
// - Vàng: vang.today SJC + DOJI HN/HCM theo miền
// - USD: checkgia Vietcombank
// Vùng giá theo tỉnh đã chọn ở mục Thời tiết (mặc định Hà Nội).
// ============================================================================
#include <string>
#include <vector>
#include <cstdint>
#include <mutex>

namespace RomCloud {

struct MarketQuote {
    bool ok = false;
    std::string value;  // đã format ("27.080", "143.500.000")
    std::string unit;   // "đ/lít", "đ/lượng", "đ"
    std::string sub;    // dòng phụ ("Mua ...", "Vùng 1", "Vietcombank")
    std::string delta;  // "▲ 5,7%" / "▼ 0,1%" / "" (▲▼ có trong font)
    bool up = true;
    std::string updated; // "08:50 03/10" hoặc ""
};

struct MarketData {
    MarketQuote xang, dau, vang, usd;
    std::string area; // "Hà Nội • Vùng 1"
    int64_t fetchedAt = 0;
};

class MarketManager {
public:
    static MarketManager& instance();
    MarketData data() const;
    // Chạy trong BackgroundTask (sync, 4 HTTP nối tiếp).
    bool fetch(const std::string& province);
    // Vùng giá xăng/dầu theo tỉnh (1/2). Xấp xỉ cấp tỉnh.
    static int fuelZone(const std::string& province);
    // Nhãn "Hà Nội • Vùng 1" (tính local, không cần mạng).
    static std::string areaFor(const std::string& province);
    // Miền Nam (dùng giá DOJI HCM) hay còn lại (DOJI HN).
    static bool isSouth(const std::string& province);

private:
    MarketManager() = default;
    mutable std::mutex m_mtx;
    MarketData m_data;
};

} // namespace RomCloud
