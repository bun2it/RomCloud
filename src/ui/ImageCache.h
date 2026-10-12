#pragma once
// RomCloud ImageCache — LRU cache dung chung cho moi SDL_Texture*.
// Gom: m_gridIconCache / m_systemIconCache / m_buttonIconCache (UiRenderer + UIManager),
// m_ytThumbnails (YouTube), CoverManager::m_cache về 1 cơ chế duy nhất.
// Header-only per MODULE_IMPLEMENTATION_PLAN P2-2. Khong phu thuoc UIManager.
#include <SDL2/SDL.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace RomCloud {

class ImageCache {
public:
    struct Entry {
        SDL_Texture* tex = nullptr;
        int w = 0;
        int h = 0;
        uint32_t lastUsed = 0;
    };

    explicit ImageCache(size_t maxItems = 128) : m_max(maxItems) {}

    void setMax(size_t n) { m_max = n; trim(); }
    size_t size() const { return m_items.size(); }
    bool empty() const { return m_items.empty(); }

    // Lấy texture, cập nhật lastUsed. Trả nullptr nếu không có.
    SDL_Texture* get(const std::string& key, int* outW = nullptr, int* outH = nullptr) {
        auto it = m_items.find(key);
        if (it == m_items.end() || !it->second.tex) return nullptr;
        it->second.lastUsed = SDL_GetTicks();
        if (outW) *outW = it->second.w;
        if (outH) *outH = it->second.h;
        return it->second.tex;
    }

    bool contains(const std::string& key) const {
        auto it = m_items.find(key);
        return it != m_items.end() && it->second.tex != nullptr;
    }

    // Chèn / thay thế. Tự destroy texture cũ trùng key, tự trim khi quá max.
    void put(const std::string& key, SDL_Texture* tex, int w = 0, int h = 0) {
        if (key.empty() || !tex) return;
        auto it = m_items.find(key);
        if (it != m_items.end()) {
            if (it->second.tex && it->second.tex != tex) SDL_DestroyTexture(it->second.tex);
            it->second = {tex, w, h, SDL_GetTicks()};
        } else {
            m_items.emplace(key, Entry{tex, w, h, SDL_GetTicks()});
        }
        trim();
    }

    // Giữ lại các key trong keep, xóa + destroy phần còn lại.
    // Trả số item đã xóa (dùng cho prune thumbnail vượt 24).
    size_t retainOnly(const std::unordered_set<std::string>& keep) {
        size_t removed = 0;
        for (auto it = m_items.begin(); it != m_items.end();) {
            if (keep.find(it->first) == keep.end()) {
                if (it->second.tex) SDL_DestroyTexture(it->second.tex);
                it = m_items.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

    void clear() {
        for (auto& kv : m_items) {
            if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
        }
        m_items.clear();
    }

private:
    void trim() {
        if (m_max == 0 || m_items.size() <= m_max) return;
        while (m_items.size() > m_max) {
            auto oldest = m_items.begin();
            for (auto it = m_items.begin(); it != m_items.end(); ++it) {
                if (it->second.lastUsed < oldest->second.lastUsed) oldest = it;
            }
            if (oldest->second.tex) SDL_DestroyTexture(oldest->second.tex);
            m_items.erase(oldest);
        }
    }

    std::unordered_map<std::string, Entry> m_items;
    size_t m_max = 128;
};

} // namespace RomCloud
