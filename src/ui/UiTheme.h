#pragma once
// ============================================================================
// RomCloud Design Tokens — 10-Foot Leanback / Modern Dark Dashboard
// Single Source of Truth cho toàn app (màu, bo góc, geometry, footer, font).
// Logic nghiệp vụ không đổi — chỉ theme/layout dùng các token này.
// ============================================================================
#include <SDL2/SDL.h>

namespace RomCloud {
namespace UiTheme {

// ---- App geometry (Brick 1024x768) ----
inline constexpr int APP_W = 1024;
inline constexpr int APP_H = 768;

// ---- Global palette ----
inline constexpr SDL_Color BG_APP      = {11, 19, 30, 255};   // #0B131E
inline constexpr SDL_Color CARD_BG     = {15, 23, 42, 200};   // panel alpha ~200
inline constexpr SDL_Color CARD_SOLID  = {15, 23, 42, 255};   // khi cần nền đặc
inline constexpr SDL_Color CARD_BORDER = {51, 65, 85, 255};
inline constexpr SDL_Color FOCUS_BG    = {37, 99, 235, 255};  // #2563EB
// Selection highlight: FOCUS_BG trong suốt 25% (alpha 191) + blend.
// Mọi ô chọn (list/menu rows, cells) dùng drawHighlight(), hình chữ nhật
// highlight căn giữa dọc theo row/khối chứa nó.
inline constexpr SDL_Color FOCUS_BG_SOFT = {30, 58, 138, 191};
inline constexpr SDL_Color FOCUS_ALT   = {249, 115, 34, 255}; // #F97322
inline constexpr SDL_Color FOCUS_GLOW  = {96, 165, 250, 255};
inline constexpr SDL_Color TEXT_MAIN   = {255, 255, 255, 255};
inline constexpr SDL_Color TEXT_DIM    = {200, 210, 225, 255};
inline constexpr SDL_Color TEXT_SUB    = {140, 155, 175, 255};
inline constexpr SDL_Color TEXT_FAINT  = {100, 115, 135, 255};
inline constexpr SDL_Color ROW_BG      = {22, 28, 38, 255};
inline constexpr SDL_Color ROW_BG_ALT  = {16, 20, 28, 255};
inline constexpr SDL_Color ROW_BG_IPTV = {20, 26, 38, 255};
inline constexpr SDL_Color ACCENT_GREEN = {34, 197, 94, 255};
inline constexpr SDL_Color ACCENT_BLUE  = {59, 130, 246, 255};
inline constexpr SDL_Color ACCENT_GOLD  = {202, 138, 4, 255};
inline constexpr SDL_Color ACCENT_RED  = {239, 68, 68, 255};
inline constexpr SDL_Color ACCENT_CYAN = {0, 180, 216, 255};
inline constexpr SDL_Color DIM_OVERLAY = {0, 0, 0, 190};

inline constexpr int RADIUS_CARD = 12;
inline constexpr int RADIUS_ROW  = 10;
inline constexpr int RADIUS_BTN  = 10;
inline constexpr int RADIUS_MODAL = 16;

// ---- Pill / chip chuẩn mềm mại (phương án A: full-round h/2) ----
inline constexpr int PILL_H = 30;
inline constexpr int PILL_PAD_X = 16;
inline constexpr int PILL_GAP = 8;
inline constexpr int PILL_MIN_W = 64;
inline constexpr int PILL_MAX_W = 220;
inline constexpr int BTN_H = 36;
inline constexpr int BTN_MIN_W = 96;
inline constexpr SDL_Color PILL_BG        = {28, 36, 52, 255};
inline constexpr SDL_Color PILL_BG_ACTIVE = FOCUS_BG;
inline constexpr SDL_Color PILL_BORDER    = {51, 65, 85, 255};
inline constexpr SDL_Color PILL_TEXT_DIM  = {160, 175, 195, 255};
inline constexpr int GROUP_BAR_H = 44;

// ---- Header / footer ----
inline constexpr int HEADER_H = 64;
inline constexpr int SUB_H    = 48;
inline constexpr int FOOTER_Y = 715;
inline constexpr int FOOTER_H = 53;
inline constexpr SDL_Color FOOTER_BG   = {18, 22, 30, 255};
inline constexpr SDL_Color FOOTER_LINE = {40, 48, 62, 255};
inline constexpr int FOOTER_ICON = 26;
inline constexpr int FOOTER_GAP  = 8;
inline constexpr int FOOTER_HINT_GAP = 28;

// ---- Layout A: Master-Detail 65/35 trên 1024 ----
inline constexpr int LIST_W   = 666;
inline constexpr int DETAIL_X = 700;
inline constexpr int DETAIL_W = 300;
// ---- Layout B: form 1 cột căn giữa ----
inline constexpr int FORM_W = 680;

// ---- Font chính ----
inline const char *FONT_MAIN = "NotoSans-Regular.ttf";

// ---- Footer component ----
enum class PadBtn { A, B, X, Y, START, SELECT, MENU, DPAD, L1, R1, L1R1, L2, R2, PLAY, UPDOWN, AB, NONE };
struct FooterHint {
    PadBtn btn = PadBtn::NONE;
    const char *label = "";
    bool accent = false;
};

} // namespace UiTheme
} // namespace RomCloud
