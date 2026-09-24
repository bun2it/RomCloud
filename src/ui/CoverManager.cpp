#include "CoverManager.h"
#include "../logging/Logger.h"
#include "../config/AppConfig.h"
#include "../platform/PlatformInfo.h"
#include <sys/stat.h>
#include <algorithm>

namespace RomCloud {

static bool fileExists(const std::string& path) {
    if (path.empty()) return false;
    struct stat st;
    return (stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode));
}

CoverManager& CoverManager::instance() {
    static CoverManager instance;
    return instance;
}

CoverManager::~CoverManager() {
    shutdown();
}

bool CoverManager::init(SDL_Renderer* renderer) {
    m_renderer = renderer;
    int flags = IMG_INIT_PNG | IMG_INIT_JPG;
    int initted = IMG_Init(flags);
    if ((initted & flags) != flags) {
        Logger::warn(std::string("IMG_Init warning: ") + IMG_GetError());
    }
    Logger::info("CoverManager initialized with SDL2_image support.");
    return true;
}

void CoverManager::shutdown() {
    clearCache();
    IMG_Quit();
}

void CoverManager::clearCache() {
    for (auto& pair : m_cache) {
        if (pair.second.texture) {
            SDL_DestroyTexture(pair.second.texture);
            pair.second.texture = nullptr;
        }
    }
    m_cache.clear();
}

std::string CoverManager::resolveCoverPath(const GameRecord& game, const SystemRecord& sys) {
    if (!game.coverPath.empty() && fileExists(game.coverPath)) {
        return game.coverPath;
    }

    std::string baseName = game.filename;
    size_t lastDot = baseName.find_last_of('.');
    std::string noExt = (lastDot != std::string::npos) ? baseName.substr(0, lastDot) : baseName;

    std::string imgsBase = AppConfig::instance().getImgsDir() + "/" + sys.code;
    std::vector<std::string> candidates = {
        imgsBase + "/" + noExt + ".png",
        imgsBase + "/" + noExt + ".jpg",
        imgsBase + "/" + noExt + ".jpeg",
        imgsBase + "/" + baseName + ".png",
        imgsBase + "/" + game.title + ".png",
        AppConfig::instance().getCacheDir() + "/covers/" + sys.code + "/" + noExt + ".png"
    };

    for (const auto& path : candidates) {
        if (fileExists(path)) {
            return path;
        }
    }

    return "";
}

void CoverManager::evictOldest() {
    if (m_cache.size() <= m_maxCacheSize) return;

    std::string oldestKey = "";
    uint32_t oldestTime = 0xFFFFFFFF;

    for (const auto& pair : m_cache) {
        if (pair.second.lastAccess < oldestTime) {
            oldestTime = pair.second.lastAccess;
            oldestKey = pair.first;
        }
    }

    if (!oldestKey.empty()) {
        auto it = m_cache.find(oldestKey);
        if (it != m_cache.end()) {
            if (it->second.texture) {
                SDL_DestroyTexture(it->second.texture);
            }
            m_cache.erase(it);
        }
    }
}

SDL_Texture* CoverManager::getCoverTexture(const GameRecord& game, const SystemRecord& sys, int& outW, int& outH) {
    if (!m_renderer) return nullptr;

    std::string key = sys.code + ":" + game.filename;
    uint32_t now = SDL_GetTicks();

    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        it->second.lastAccess = now;
        outW = it->second.width;
        outH = it->second.height;
        return it->second.texture;
    }

    std::string imgPath = resolveCoverPath(game, sys);
    if (imgPath.empty()) {
        return nullptr;
    }

    SDL_Surface* surface = IMG_Load(imgPath.c_str());
    if (!surface) {
        return nullptr;
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(m_renderer, surface);
    int w = surface->w;
    int h = surface->h;
    SDL_FreeSurface(surface);

    if (!texture) {
        return nullptr;
    }

    evictOldest();

    CachedTexture cached;
    cached.texture = texture;
    cached.width = w;
    cached.height = h;
    cached.lastAccess = now;
    m_cache[key] = cached;

    outW = w;
    outH = h;
    return texture;
}

void CoverManager::renderCoverBox(int x, int y, int w, int h, const GameRecord* game, const SystemRecord* sys, TTF_Font* font) {
    if (!m_renderer) return;

    int sx = PlatformInfo::instance().scaleX(x);
    int sy = PlatformInfo::instance().scaleY(y);
    int sw = PlatformInfo::instance().scaleW(w);
    int sh = PlatformInfo::instance().scaleH(h);

    // Drop shadow
    SDL_Rect shadowRect = {sx + 6, sy + 6, sw, sh};
    SDL_SetRenderDrawColor(m_renderer, 10, 13, 18, 160);
    SDL_RenderFillRect(m_renderer, &shadowRect);

    if (game && sys) {
        int imgW = 0, imgH = 0;
        SDL_Texture* tex = getCoverTexture(*game, *sys, imgW, imgH);
        if (tex && imgW > 0 && imgH > 0) {
            // Draw background frame
            SDL_Rect bgRect = {sx, sy, sw, sh};
            SDL_SetRenderDrawColor(m_renderer, 20, 24, 32, 255);
            SDL_RenderFillRect(m_renderer, &bgRect);

            // Compute aspect ratio preserving fit
            float scale = std::min((float)(sw - 8) / (float)imgW, (float)(sh - 8) / (float)imgH);
            int destW = (int)(imgW * scale);
            int destH = (int)(imgH * scale);
            int destX = sx + (sw - destW) / 2;
            int destY = sy + (sh - destH) / 2;

            SDL_Rect dstRect = {destX, destY, destW, destH};
            SDL_RenderCopy(m_renderer, tex, nullptr, &dstRect);

            // Outer border
            SDL_SetRenderDrawColor(m_renderer, 0, 180, 216, 255);
            SDL_RenderDrawRect(m_renderer, &bgRect);
            return;
        }
    }

    // Fallback procedural box art
    SDL_Rect bgRect = {sx, sy, sw, sh};
    SDL_SetRenderDrawColor(m_renderer, 24, 30, 42, 255);
    SDL_RenderFillRect(m_renderer, &bgRect);

    // Top system banner
    int bannerH = PlatformInfo::instance().scaleH(40);
    SDL_Rect bannerRect = {sx, sy, sw, bannerH};
    SDL_SetRenderDrawColor(m_renderer, 30, 58, 95, 255);
    SDL_RenderFillRect(m_renderer, &bannerRect);

    // Inner cartridge outline
    int padX = PlatformInfo::instance().scaleW(16);
    int padY = PlatformInfo::instance().scaleH(16);
    SDL_Rect innerBox = {sx + padX, sy + bannerH + padY, sw - 2 * padX, sh - bannerH - 2 * padY};
    SDL_SetRenderDrawColor(m_renderer, 35, 42, 56, 255);
    SDL_RenderFillRect(m_renderer, &innerBox);
    SDL_SetRenderDrawColor(m_renderer, 60, 72, 92, 255);
    SDL_RenderDrawRect(m_renderer, &innerBox);

    // Outer border
    SDL_SetRenderDrawColor(m_renderer, 0, 180, 216, 255);
    SDL_RenderDrawRect(m_renderer, &bgRect);

    // System text
    if (sys && font) {
        SDL_Color brandColor = {0, 180, 216, 255};
        SDL_Surface* brandSurf = TTF_RenderUTF8_Blended(font, sys->code.c_str(), brandColor);
        if (brandSurf) {
            SDL_Texture* brandTex = SDL_CreateTextureFromSurface(m_renderer, brandSurf);
            if (brandTex) {
                SDL_Rect brandDst = {sx + (sw - brandSurf->w) / 2, sy + (bannerH - brandSurf->h) / 2, brandSurf->w, brandSurf->h};
                SDL_RenderCopy(m_renderer, brandTex, nullptr, &brandDst);
                SDL_DestroyTexture(brandTex);
            }
            SDL_FreeSurface(brandSurf);
        }
    }

    // Game title text
    if (game && font) {
        std::string displayTitle = game->title;
        if (displayTitle.length() > 22) {
            displayTitle = displayTitle.substr(0, 19) + "...";
        }
        SDL_Color titleColor = {240, 245, 250, 255};
        SDL_Surface* titleSurf = TTF_RenderUTF8_Blended(font, displayTitle.c_str(), titleColor);
        if (titleSurf) {
            SDL_Texture* titleTex = SDL_CreateTextureFromSurface(m_renderer, titleSurf);
            if (titleTex) {
                SDL_Rect titleDst = {sx + (sw - titleSurf->w) / 2, sy + bannerH + (sh - bannerH - titleSurf->h) / 2, titleSurf->w, titleSurf->h};
                SDL_RenderCopy(m_renderer, titleTex, nullptr, &titleDst);
                SDL_DestroyTexture(titleTex);
            }
            SDL_FreeSurface(titleSurf);
        }
    }
}

} // namespace RomCloud
