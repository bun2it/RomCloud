#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <cstdint>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include "NetSurfLayout.h"

namespace RomCloud {

enum class NavDirection {
    UP,
    DOWN,
    LEFT,
    RIGHT
};

class NetSurfRenderer {
public:
    static NetSurfRenderer& instance();

    // Initialize renderer with SDL contexts
    void init(SDL_Renderer* renderer, TTF_Font* font,
              const std::string& fontPath = "", int baseFontSize = 24);

    // Reset state & layout tree
    void reset();

    // Set new layout tree & compute scroll limits & rebuild focusables
    void setRenderTree(std::unique_ptr<RenderBox> root);
    RenderBox* getRenderTree() const { return m_root.get(); }

    // Focus & Interactive Navigation
    void rebuildFocusables();
    const std::vector<RenderBox*>& focusables() const { return m_focusables; }
    size_t focusableCount() const { return m_focusables.size(); }
    RenderBox* focusedBox() const;
    int focusIndex() const { return m_focusedIndex; }
    bool setFocusIndex(int index);
    void focusFirst();
    void focusLast();

    // 4-Way D-pad navigation
    bool navigate(NavDirection dir);
    bool navigateUp() { return navigate(NavDirection::UP); }
    bool navigateDown() { return navigate(NavDirection::DOWN); }
    bool navigateLeft() { return navigate(NavDirection::LEFT); }
    bool navigateRight() { return navigate(NavDirection::RIGHT); }

    // Scrolling (screen space 64..715 on 1024x768 screen)
    int scrollY() const { return m_scrollY; }
    void setScrollY(int sy);
    void scrollBy(int delta);
    int maxScroll() const { return m_maxScroll; }
    void ensureVisible(const RenderBox* box);

    // Image pipeline
    void pumpImages();
    bool decodeImageForTest(const std::string& url, const std::vector<uint8_t>& bytes);
    bool isImageLoaded(const std::string& url) const;
    size_t imageCachedCount() const { return m_textures.size(); }
    void queueImage(const std::string& url);

    // Rendering
    void render();
    void renderBox(const RenderBox* box, int scrollY);

    // Font resolution
    TTF_Font* fontFor(int px);

private:
    NetSurfRenderer() = default;
    ~NetSurfRenderer();
    NetSurfRenderer(const NetSurfRenderer&) = delete;
    NetSurfRenderer& operator=(const NetSurfRenderer&) = delete;

    void clearTextCache();
    void clearImageCache();
    void drawText(const std::string& text, int x, int y, SDL_Color color, int fontSizePx);
    // P17: form controls (text/password field, checkbox, button, select).
    void drawFormControl(const RenderBox* box, int sy);
    // Truncate to pixel width (UTF-8 safe); tail=true keeps the end.
    std::string fitText(const std::string& text, int maxW, int px, bool tail = false);

    SDL_Renderer* m_renderer = nullptr;
    TTF_Font* m_font = nullptr;
    std::string m_fontPath;
    int m_baseFontSize = 24;
    std::unordered_map<int, TTF_Font*> m_fonts;

    std::unique_ptr<RenderBox> m_root;
    std::vector<RenderBox*> m_focusables;
    int m_focusedIndex = -1;

    int m_scrollY = 0;
    int m_maxScroll = 0;
    int m_totalHeight = 0;

    // Image textures
    struct ImageEntry {
        SDL_Texture* tex = nullptr;
        int w = 0;
        int h = 0;
    };
    std::unordered_map<std::string, ImageEntry> m_textures;
    std::unordered_set<std::string> m_pendingImages;
    std::mutex m_imageMutex;

    struct DecodedSurface {
        std::string url;
        SDL_Surface* surface = nullptr;
        int w = 0;
        int h = 0;
    };
    std::vector<DecodedSurface> m_readySurfaces;

    // Text texture cache
    struct CachedText {
        SDL_Texture* tex = nullptr;
        int w = 0;
        int h = 0;
        uint32_t lastUsed = 0;
    };
    std::unordered_map<std::string, CachedText> m_textCache;
    uint32_t m_texTick = 0;
};

} // namespace RomCloud
