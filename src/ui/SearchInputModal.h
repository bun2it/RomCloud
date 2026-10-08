#pragma once
// ============================================================================
// SearchInputModal — reusable search-input popup (P0-6).
//
// Pops up over ANY screen that has a search input. Shows:
//   - Dim overlay over underlying state
//   - History list (vertical, top)
//   - VirtualKeyboard (bottom)
// Caller owns VkState (so keyboard state persists across popups) and history
// vector. Modal does not own state — just renders + routes input.
//
// Used by: YouTube HOME (search input), IPTV search (future), FileExplorer
// rename/create (future), LocalSend folder picker (future).
//
// Geometry (1024x768 TrimUI Brick):
//   Y=80      : Title strip ("TÌM KIẾM..." + current query)
//   Y=120..160: History pills row (horizontal, scrollable, 1 row × 40px)
//   Y=180..472: Keyboard (4 rows × 52px + action 52px, gaps 8px)
//   Y=472..715: dim (empty) — modal floats top, footer drawn by caller
//   Modal dim full screen (header stays visible via beginModalDim).
// ============================================================================

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <vector>

#include "VirtualKeyboard.h"

namespace RomCloud {

class UiRenderer;

class SearchInputModal {
public:
    // Modal owns no state; caller passes everything via Config.
    struct Config {
        UiRenderer* ui = nullptr;            // m_ui renderer for drawVirtualKeyboard
        VkState* vk = nullptr;               // caller's keyboard state (mutated)
        std::vector<std::string>* history = nullptr; // caller's history (re-ordered on pick)

        TTF_Font* fSmall = nullptr;          // shared fonts from UIManager
        TTF_Font* fMedium = nullptr;
        TTF_Font* fLarge = nullptr;

        const char* title = "TÌM KIẾM";      // modal title
        const char* placeholder = "Nhập từ khóa...";
        const char* actionLabels[5] = {
            "abc", "ABC", "Cách", "Xóa", "Tìm"
        };

        // Layout
        int modalX = 46;
        int modalW = 932;
        int historyY = 80;        // title strip Y
        int historyH = 440;       // (legacy, unused)
        int dividerY = 120;       // history pills row Y (40px tall)
        int keypadY = 180;
        int keypadH = 292;        // 4×52 + action 52 + gaps 8

        // Focus: 0=history pills row, 1=keyboard
        int focusMode = 1;        // default to keyboard (typing is primary)

        // History pills state (modal-local)
        int historySelected = 0;      // selected pill index
        int historyScrollOffset = 0;  // first visible pill index

        // Keyboard style (stride2 = legacy YT/TT layout, cols 0..9 even)
        int actionStride = 2;
        SDL_Color accent = {0, 140, 230, 255};
        SDL_Color accentEdge = {0, 180, 255, 255};

        // History actions
        bool allowPickHistory = true;   // A on history row → fill query
        bool allowDeleteHistoryItem = false; // X on history row → delete (optional)
    };

    enum class Result {
        None,           // no action (typing, navigating)
        Commit,         // user pressed A "Tìm" / START — query ready
        Cancel,         // user pressed B — close modal, no commit
        PickHistory,    // user picked a history item (query already filled)
    };

    // Xử lý input trong modal. Caller đã xử lý B-back chung cho state nền
    // (vd YOUTUBE_HOME) — modal chỉ lo input khi đang mở.
    // in: InputManager từ caller (chỉ đọc).
    // Mapping (giong stock): L1=Shift, R1=ABC/123, SELECT=Telex.
    // Returns signal cho caller (vd setState về RESULTS, hoặc đóng modal).
    static Result handleInput(const Config& cfg, bool aPressed,
                              bool bPressed, bool startPressed,
                              bool upPressed, bool downPressed,
                              bool leftPressed, bool rightPressed,
                              bool xPressed, bool yPressed,
                              bool l1Pressed, bool r1Pressed,
                              bool selectPressed = false);

    // Vẽ modal (gọi sau khi đã vẽ state nền). Tự gọi beginModalDim.
    static void render(const Config& cfg);

    // Lập cho click history → fill query + đẩy lên đầu history (LRU semantics)
    static void pickHistoryItem(Config& cfg, int historyIdx);

private:
    static void renderHistoryList(const Config& cfg);
    static void renderKeyboard(const Config& cfg);
    static void renderTitle(const Config& cfg);
    static int maxHistoryOffset(int historyCount);
};

} // namespace RomCloud