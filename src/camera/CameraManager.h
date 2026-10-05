#pragma once
// ============================================================================
// RomCloud CameraManager — camera giao thông (ảnh snapshot refresh).
// Data: assets/traffic_cams.json [{n:name, c:code, u:image_base}].
// Ảnh: curl + &t=timestamp về /tmp, UI tự refresh theo chu kỳ.
// ============================================================================
#include <string>
#include <vector>
#include <unordered_set>

namespace RomCloud {

struct TrafficCam {
    std::string name;
    std::string code;
    std::string url;   // JPEG snapshot (kind 0)
    int kind = 0;      // 0 = ảnh JPEG refresh, 1 = YouTube live
    std::string vid;   // youtube id khi kind == 1
};

class CameraManager {
public:
    static CameraManager& instance();

    bool load();
    size_t count() const { return m_cams.size(); }
    const TrafficCam& at(size_t i) const { return m_cams[i]; }

    // Tải snapshot cam i về /tmp (thêm &t chống cache). Trả về path file,
    // rỗng nếu lỗi. Chạy nền (blocking curl).
    std::string fetch(size_t i);
    static std::string snapPath(size_t i);

    // Yêu thích (giống IPTV: X đánh dấu, Y lọc chỉ hiện yêu thích)
    bool isFavorite(const std::string& key) const;
    void toggleFavorite(const std::string& key);
    std::string favKey(size_t i);

private:
    CameraManager() = default;
    std::vector<TrafficCam> m_cams;
    std::unordered_set<std::string> m_favs;
    bool m_favsLoaded = false;
    void loadFavs();
    void saveFavs();
    std::string favPath() const;
};

} // namespace RomCloud
