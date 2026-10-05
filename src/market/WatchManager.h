#pragma once
// ============================================================================
// RomCloud WatchManager — theo dõi crypto + chứng khoán VN (free, không key):
// - Crypto: CoinGecko simple/price (batch) + market_chart 30 ngày.
// - Cổ phiếu VN: VNDirect dchart history (resolution=D, giá nghìn đồng).
// Tối đa 4 mã (lưới 2x2), lưu DB "watch_symbols" (CSV in hoa).
// ============================================================================
#include <string>
#include <vector>
#include <mutex>

namespace RomCloud {

struct WatchItem {
    std::string symbol; // "BTC" (in hoa)
    std::string label;  // "Bitcoin" / "VCB"
    bool crypto = false;
    bool ok = false;
    std::string price; // "85.844" / "63,50"
    std::string unit;  // "USD" / "×1.000 VNĐ"
    std::string delta; // "▲ 0,7%" / "▼ 1,2%" (▲▼ có trong font)
    bool up = true;
    std::vector<float> hist; // giá đóng cửa (<=60), cuối = mới nhất
};

class WatchManager {
public:
    static WatchManager& instance();
    static const int kMaxCards = 4;
    // Đọc/ghi danh sách mã (chuẩn hóa UPPER, tối đa 4). Mặc định BTC,ETH,VCB,HPG.
    std::vector<std::string> symbols();
    void setSymbols(const std::vector<std::string>& v);
    // symbols + dữ liệu đã fetch (chưa có data -> ok=false).
    std::vector<WatchItem> items();
    // Chạy trong BackgroundTask: giá batch + chart từng mã.
    bool fetchAll();
    static bool isCrypto(const std::string& sym);
    static std::string cryptoId(const std::string& sym); // "" nếu không map được
    static std::string cryptoLabel(const std::string& sym);

private:
    WatchManager() = default;
    mutable std::mutex m_mtx;
    std::vector<WatchItem> m_items;
};

} // namespace RomCloud
