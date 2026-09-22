#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include "../database/DatabaseManager.h"

namespace RomCloud {

struct CachedTexture {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
    uint32_t lastAccess = 0;
};

class CoverManager {
public:
    static CoverManager& instance();
    bool init(SDL_Renderer* renderer);
    void shutdown();

    SDL_Texture* getCoverTexture(const GameRecord& game, const SystemRecord& sys, int& outW, int& outH);
    void clearCache();
    void renderCoverBox(int x, int y, int w, int h, const GameRecord* game, const SystemRecord* sys, TTF_Font* font);

private:
    CoverManager() = default;
    ~CoverManager();

    SDL_Renderer* m_renderer = nullptr;
    std::unordered_map<std::string, CachedTexture> m_cache;
    size_t m_maxCacheSize = 64;

    std::string resolveCoverPath(const GameRecord& game, const SystemRecord& sys);
    void evictOldest();
};

} // namespace RomCloud
