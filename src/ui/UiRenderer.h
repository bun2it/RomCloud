#pragma once
// RomCloud UiRenderer — primitive ve SDL2 dung chung, khong logic nghiep vu.
// UIManager / FileExplorer / Dialog chi goi m_ui.draw*(). Giu nguyen chu ky
// va hanh vi tu UIManager (scale theo PlatformInfo, cache text 256 item).
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "UiTheme.h"
#include "ImageCache.h"
#include "VirtualKeyboard.h"

namespace RomCloud {

class UiRenderer {
public:
    UiRenderer() = default;

    // Goi 1 lan sau khi co renderer + font. assetsDir vd AppConfig::getAssetsDir().
    void bind(SDL_Renderer* r, TTF_Font* small, TTF_Font* medium,
              TTF_Font* large, const std::string& assetsDir);
    void unbind();  // giai phong cache texture

    // ---- primitive co ban ----
    void drawText(const std::string& text, int x, int y, SDL_Color color,
                  TTF_Font* font, bool centered = false);
    // Vẽ nội dung số của 1 thẻ lật, căn giữa trong rect (x,y,w,h).
    // Kiến trúc gluqlo/Fliqlo: bản lề GIỮA THẺ (midY = y + h/2).
    // t01 < 0: tĩnh, vẽ đầy đủ số mới. Ngược lại 0..1 là tiến trình lật
    // (~320ms, fake-3D scale 2D): phase A (t<0.5) cánh NỬA TRÊN số cũ gập
    // cos(p*pi/2) về bản lề; phase B cánh NỬA DƯỚI số mới mở sin(p*pi/2).
    // Texture số cache sẵn (pre-render) nên blit mỗi frame rất rẻ.
    void drawFlipDigits(const std::string& newText, const std::string& oldText,
                        float t01, int x, int y, int w, int h,
                        TTF_Font* font, SDL_Color color);
    void drawRect(int x, int y, int w, int h, SDL_Color color, bool filled);
    void drawBorder(int x, int y, int w, int h, SDL_Color color, int thickness);
    void drawRoundedRect(int x, int y, int w, int h, int radius,
                         SDL_Color color, bool filled);
    void drawRoundedBorder(int x, int y, int w, int h, int radius,
                           SDL_Color color, int thickness);
    void drawRoundedTopBar(int x, int y, int w, int h, int radius,
                           SDL_Color color);
    void drawModalDialog(int x, int y, int w, int h, int radius,
                         SDL_Color bodyBg, SDL_Color titleBg, int titleH);
    void drawBadge(int x, int y, int w, int h, const std::string& text,
                   SDL_Color bg, SDL_Color fg);
    void drawIcon(const std::string& iconName, int x, int y, int w, int h);
    void drawPlayerIcon(const std::string& iconName, int x, int y, int w, int h);
    void drawButtonIcon(const std::string& button, int x, int y, int size);
    // Rộng icon theo button: L1R1/UPDOWN = 2 icon full-size cạnh nhau.
    static int buttonIconWidth(const std::string& button, int size);
    void drawGridIcon(const std::string& iconFile, int x, int y, int w, int h);

    // ---- do chu ----
    int textHeight(TTF_Font* font);
    int textWidth(const std::string& text, TTF_Font* font);
    std::string truncateToWidth(const std::string& text, TTF_Font* font, int maxPx);
    int pillWidth(const std::string& text, TTF_Font* font = nullptr);
    // Rong badge/button do theo pixel (w=0 -> tu do). Tra ve w sau clamp.
    int badgeWidth(const std::string& text, int h, TTF_Font* font = nullptr);
    int buttonWidth(const std::string& label, TTF_Font* font = nullptr);
    int badgeDualWidth(const std::string& label1, const std::string& label2, int h);

    // ---- widget ----
    void drawPill(int x, int y, int w, int h, const std::string& text,
                  bool active, TTF_Font* font = nullptr);
    void drawButton(int x, int y, int w, int h, const std::string& label,
                    bool focused, bool danger = false);
    void drawRow(int x, int y, int w, int h, bool focused, bool dim = false);
    int textYCentered(int y, int h, TTF_Font* font);
    void drawRowMainSub(int x, int y, int h, const std::string& main, TTF_Font* fMain,
                        const std::string& sub, TTF_Font* fSub,
                        int maxW = 0, int gap = 4);
    void drawTextRight(const std::string& text, int rightX, int y,
                       SDL_Color color, TTF_Font* font);
    int drawFooterHint(const std::string& button, const std::string& label, int x,
                       int barY, int barH, SDL_Color color, TTF_Font* font,
                       int iconSize = 30, int gap = 8);
    void drawFooterHintsCentered(
        const std::vector<std::pair<std::string, std::string>>& hints,
        int barY, int barH, SDL_Color color, TTF_Font* font,
        int iconSize = 30, int gap = 8, int hintGap = 28);
    void drawBadgeDual(int x, int y, int w, int h,
                       const std::string& btn1, const std::string& label1,
                       const std::string& btn2, const std::string& label2,
                       SDL_Color bg, SDL_Color fg);
    void drawInlineHintsCentered(const std::string& text, int centerX, int y,
                                 SDL_Color color, TTF_Font* font,
                                 int iconSize = 26, int gap = 6);

    // ---- theme helpers ----
    void drawAppBackground();
    void drawCard(int x, int y, int w, int h);
    void drawFocusRow(int x, int y, int w, int h);
    void drawAppHeader(const std::string& title, const std::string& sub = "");
    // Cum status phai header kieu stock: [wifi] [% pin + icon pin] [gio]
    void drawHeaderStatus();
    void drawPadIcon(UiTheme::PadBtn btn, int x, int y, int size);
    void drawAppFooter(const std::vector<UiTheme::FooterHint>& hints);
    void beginModalDim();

    // ---- virtual keyboard chung (P0-2: gom 6 ban phim ve tay) ----
    // Ve 4 hang phim QWERTY (VirtualKeyboard::lower/upperRows, symbolMode -> symbolRows) + hang action 5 o.
    // actionLabels: 5 nhan (vd {"abc","ABC","Cách","Xóa","Tìm"} — o 0/1 chi fallback khi withIcons=false); withIcons: ve icon nut L1/R1/X/Y/START.
    // actionStride: 1 = col 0..4 (Explorer/LsFolder/VirtualKeyboard::move), 2 = col 0..9 kieu YT/TT cu (col/2).
    void drawVirtualKeyboard(const VkState& vk, int x, int y, int cellW, int cellH,
                             int gapX, int gapY, SDL_Color accent, SDL_Color accentEdge,
                             const char* actionLabels[5], bool withIcons, bool rounded = true,
                             int actionStride = 1);

    void clearTextCache();
    // P2-2: cache anh LRU dung chung (logo/icon/button/thumb/cover).
    ImageCache& images() { return m_images; }
    SDL_Texture* getImage(const std::string& key, int* outW = nullptr, int* outH = nullptr);
    SDL_Texture* getOrLoadImage(const std::string& key, const std::string& path);
    void clearImages() { m_images.clear(); }

private:
    SDL_Renderer* m_r = nullptr;
    TTF_Font* m_fSmall = nullptr;
    TTF_Font* m_fMedium = nullptr;
    TTF_Font* m_fLarge = nullptr;
    std::string m_assetsDir;

    struct CachedText {
        SDL_Texture* texture = nullptr;
        int w = 0;
        int h = 0;
        uint32_t lastUsed = 0;
    };
    std::unordered_map<std::string, CachedText> m_textCache;
    // P2-2: 1 ImageCache LRU dung chung (thay 3 map roi grid/system/button).
    ImageCache m_images{256};
};

} // namespace RomCloud
