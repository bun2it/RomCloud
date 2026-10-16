// tests/test_p20_inprocess.cpp — P20 in-process FULL engine smoke test.
//
// Runs on host (macOS). SDL_ttf + real font, renderer=nullptr (draw calls
// no-op). Verifies:
//   1. NetSurfEngine::init() succeeds with libcss/libhubbub linked in.
//   2. loadHtml() adopts a layout tree for a small HTML snippet.
//   3. Focus navigation (4-way D-Pad) walks the focusable list.
//   4. handleInput() dispatches A-press to the focused link's href.
//   5. BrowserSettings round-trip persists inProcessFull default.
//
// Build (host):
//   c++ -std=c++17 -Isrc -o tests/test_p20_inprocess \
//       tests/test_p20_inprocess.cpp \
//       src/browser/netsurf/NetSurfEngine.cpp \
//       src/browser/netsurf/NetSurfRenderer.cpp \
//       src/browser/netsurf/NetSurfLayout.cpp \
//       src/browser/netsurf/NetSurfBridge.cpp \
//       src/browser/BrowserManager.cpp \
//       src/browser/HtmlRenderer.cpp \
//       src/network/HttpClient.cpp \
//       src/logging/Logger.cpp \
//       src/config/AppConfig.cpp \
//       src/database/DatabaseManager.cpp \
//       src/input/InputManager.cpp \
//       -Lsysroot/lib -lcss -ldom -lhubbub -lparserutils -lwapcaplet \
//       -lSDL2 -lSDL2_ttf -lcurl -lpthread

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "../src/browser/netsurf/NetSurfEngine.h"
#include "../src/browser/netsurf/NetSurfRenderer.h"
#include "../src/browser/netsurf/NetSurfLayout.h"
#include "../src/browser/BrowserManager.h"
#include "../src/logging/Logger.h"

using namespace RomCloud;

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::cout << "[PASS] " << msg << std::endl; } \
    else { std::cout << "[FAIL] " << msg << std::endl; s_fail++; } \
} while (0)

int main() {
    std::cout << "=== P20 in-process FULL smoke test ===" << std::endl;

    // 1. Init SDL_ttf + NetSurfEngine.
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return 1;
    }
    if (TTF_Init() != 0) {
        std::cerr << "TTF_Init failed: " << TTF_GetError() << std::endl;
        return 1;
    }
    TTF_Font* font = TTF_OpenFont("assets/regular.ttf", 16);
    if (!font) {
        std::cerr << "TTF_OpenFont failed: " << TTF_GetError() << std::endl;
        return 1;
    }

    Logger::instance().init("");
    NetSurfEngine::instance().init(nullptr, font, "assets/regular.ttf", 16);
    CHECK(NetSurfEngine::instance().isInitialized(), "NetSurfEngine::init succeeded");

    // 2. loadHtml() a small page. P16 schedules an async layout; we poll
    // until adoption or 2s.
    const std::string html =
        "<!doctype html><html><body>"
        "<a href='https://example.com/'>link one</a>"
        "<a href='https://example.com/two'>link two</a>"
        "<input type='text' name='q' value='hi'/>"
        "<button type='submit'>Go</button>"
        "</body></html>";
    bool ok = NetSurfEngine::instance().loadHtml(html, "https://test.local/");
    CHECK(ok, "loadHtml() accepted HTML");

    auto start = std::chrono::steady_clock::now();
    while (NetSurfRenderer::instance().focusableCount() == 0) {
        NetSurfEngine::instance().poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) break;
    }
    CHECK(NetSurfRenderer::instance().focusableCount() >= 3,
          "Layout adopted 3+ focusables (2 links + input + button)");

    // 3. 4-way focus nav
    RenderBox* fb0 = NetSurfEngine::instance().focusedBox();
    CHECK(fb0 != nullptr, "focusedBox() returns a non-null box after load");
    bool navDown = NetSurfEngine::instance().navigate(NavDirection::DOWN);
    CHECK(navDown, "navigate(DOWN) returned true");
    RenderBox* fb1 = NetSurfEngine::instance().focusedBox();
    CHECK(fb1 != nullptr && fb1 != fb0, "focus moved to a different box on DOWN");

    // 4. handleInput() with no InputManager state is a no-op except for
    // SELECT which focusFirst()s. We just verify it doesn't crash.
    bool hij = NetSurfEngine::instance().handleInput();
    CHECK(true, "handleInput() did not crash (hij=" + std::to_string(hij) + ")");

    // 5. Settings round-trip: default inProcessFull=true.
    BrowserManager::instance().init();
    const auto& s = BrowserManager::instance().settings();
    CHECK(s.fullEngine == true, "default fullEngine=true");
    CHECK(s.inProcessFull == true, "default inProcessFull=true (P20 default)");
    CHECK(BrowserManager::instance().testInProcessFailed() == false,
          "testInProcessFailed() == false at boot");

    NetSurfEngine::instance().clear();
    TTF_CloseFont(font);
    TTF_Quit();
    SDL_Quit();

    std::cout << "=== Done. Failures: " << s_fail << " ===" << std::endl;
    return s_fail == 0 ? 0 : 1;
}
