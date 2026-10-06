#pragma once
// RomCloud TideManager — triều cường trạm Vũng Tàu (live, miễn phí, không key).
// Feed: api.openwaters.io (TICON-4 harmonics, CC-BY-4.0, Asia/Ho_Chi_Minh).
// Cache data/tide_cache.json, TTL 2h (delay 1-2h chấp nhận được).
#include <string>
#include <vector>

namespace RomCloud {

struct TideExtreme {
    long epoch = 0;     // UTC epoch
    double level = 0;   // mét, datum LAT
    bool high = false;
};

class TideManager {
public:
    static TideManager& instance();

    // Fetch mới (nếu mạng ok) rồi cache; hết mạng thì dùng cache cũ.
    // Trả về true nếu có dữ liệu dùng được (mới hoặc cache).
    bool refresh();
    bool loadCache();
    long cacheAgeSec() const; // -1 = chưa có cache
    bool hasData() const { return !m_ext.empty(); }

    // Đỉnh triều cao tiếp theo (epochLevel ra UTC epoch + mét).
    bool nextHigh(long now, long &outEpoch, double &outLevel) const;
    // Đỉnh cao nhất trong 24h tới, -1 nếu không có.
    double maxHighNext24h(long now) const;

    static long parseUtcEpoch(const std::string& s); // "....T..:..:.." -> epoch

private:
    TideManager() = default;
    std::vector<TideExtreme> m_ext;
    std::string cachePath() const;
};

} // namespace RomCloud
