#pragma once
#include <string>
#include <vector>
#include <list>
#include <memory>
#include <mutex>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <pthread.h>
#include "../network/HttpClient.h"

namespace RomCloud {

// HTML element types
enum class HtmlElementType {
    TEXT,
    LINK,
    INPUT_TEXT,
    INPUT_PASSWORD,
    INPUT_CHECKBOX,
    BUTTON,
    FORM,
    DIV,
    IMAGE,
    LINE_BREAK,
};

// Represents a parsed HTML element.
// `children` uses std::list (NOT std::vector or std::deque) so that pointers
// to existing children stay valid across additional push_back calls.
// - std::vector would reallocate on growth, dangling m_focusable / stack
//   pointers captured during parseHtml (audit issue C2).
// - std::deque does not support incomplete types with libc++ when the deque
//   element type IS the containing struct itself (sizeof needed at
//   instantiation). std::list<HtmlElement> compiles fine and gives the same
//   pointer-stability guarantee.
struct HtmlElement {
    HtmlElementType type = HtmlElementType::TEXT;
    std::string text;
    std::string href;      // for links / form action (FORM)
    std::string name;     // for form inputs (also: form method for FORM,
                            //             button type for BUTTON)
    std::string value;    // current value of input
    std::string placeholder;
    std::string src;      // for images
    bool checked = false;
    int x = 0, y = 0;
    int width = 0, height = 0;
    bool focused = false;
    int tabIndex = -1;
    std::list<HtmlElement> children;  // list -> stable pointers, OK with libc++

    // Phase 3 — audit M5: denormalized form context. Every element knows
    // which enclosing <form> it belongs to (if any). At submit time, we
    // walk the tree, gather inputs sharing formAction, and POST them.
    // Using denormalized strings (rather than a parent pointer) keeps the
    // layout/render passes simple — they don't need to chase pointers.
    std::string formAction;
    std::string formMethod = "GET";
};

// Heap-allocated fetch result. Worker thread fills it, then transfers
// ownership to the main thread via HtmlRenderer::enqueueFetchResult().
// This replaces the old static `s_url` + direct call pattern that had
// a race when loadUrl() was called twice in quick succession (audit C1)
// and a data race when worker-thread code touched main-thread state directly (audit C3).
struct PendingFetch {
    std::string url;
    std::string body;
    std::string error;
    bool success = false;
};

// HTML Renderer - Simple browser for Wi-Fi portal and basic pages
class HtmlRenderer {
public:
    static HtmlRenderer& instance();

    // Initialize with screen dimensions
    void init(SDL_Renderer* renderer, TTF_Font* font);

    // Load and parse HTML content
    bool loadHtml(const std::string& html);
    bool loadUrl(const std::string& url);

    // Navigation
    void goBack();
    bool canGoBack() const;
    std::string currentUrl() const { return m_currentUrl; }

    // Input handling
    void handleInput(int button);
    void typeCharacter(char c);
    void deleteCharacter();

    // Page scrolling (Phase 2 — audit M2). Public so the UI layer can wire
    // shoulder buttons (L1/R1) without poking at the renderer internals.
    void pageUp(int delta = 80);
    void pageDown(int delta = 80);

    // Render
    void render();

    // Async fetch polling (call in render loop on main thread).
    // Picks up the PendingFetch posted by the worker thread (if any),
    // processes it on the main thread, and updates DOM state.
    void pollFetch();

    // State
    bool isLoading() const { return m_loading; }
    bool hasError() const { return m_error; }
    std::string errorMessage() const { return m_errorMsg; }

    // Cleanup
    void stop();

    // Diagnostics: how many DOM elements are currently rendered. Used by the
    // "screen is blank" diagnostic to distinguish "fetched but parsed
    // nothing" from "fetched and parsed but render path is broken".
    size_t elementCount() const { return m_elements.size(); }

private:
    HtmlRenderer() = default;
    ~HtmlRenderer() = default;
    HtmlRenderer(const HtmlRenderer&) = delete;
    HtmlRenderer& operator=(const HtmlRenderer&) = delete;

    // HTML Parsing
    void parseHtml(const std::string& html);
    std::string extractTag(const std::string& html, size_t& pos, std::string& tagName, std::string& attributes);
    std::string getAttribute(const std::string& attrs, const std::string& name);
    std::string decodeHtmlEntities(const std::string& str);

    // Layout
    void layout();
    void layoutElement(HtmlElement& el, int& y, int x, int width);
    int textWidth(const std::string& text);

    // Rendering
    void renderElement(const HtmlElement& el);

    // Navigation
    void submitForm(const HtmlElement& form);

    // Focus management
    void focusNext();
    void focusPrev();
    void updateFocus();
    void ensureFocusVisible();

    // Helper rendering functions
    void drawRect(int x, int y, int w, int h, SDL_Color color, bool filled);
    void drawText(const std::string& text, int x, int y, SDL_Color color, TTF_Font* font, bool centered);

    // Internal: load a URL without pushing it onto the back/forward history.
    // Used by goBack() and refresh() so navigating history does not create
    // duplicate entries (audit issue C4).
    bool loadUrlNoHistory(const std::string& url);

    // Internal: called from the worker thread. Hands off ownership of
    // `result` to the main thread via m_pendingResult + mutex.
    // The renderer takes ownership and is responsible for deleting/freeing
    // the PendingFetch object.
    void enqueueFetchResult(PendingFetch* result);

    // Input field
    int m_inputIndex = -1;
    bool m_editingText = false;

    // State
    SDL_Renderer* m_renderer = nullptr;
    TTF_Font* m_font = nullptr;
    // list (not vector): pointers into m_elements stay valid across
    // push_back — fixes the dangling-pointer bug in m_focusable (audit C2).
    std::list<HtmlElement> m_elements;
    std::vector<HtmlElement*> m_focusable;
    int m_scrollY = 0;
    int m_maxScroll = 0;
    std::vector<std::string> m_history;
    size_t m_historyPos = 0;
    std::string m_currentUrl;
    bool m_loading = false;
    bool m_error = false;
    std::string m_errorMsg;

    // Worker -> main thread handoff. Mutex protects m_pendingResult only;
    // once the main thread has the shared_ptr, the PendingFetch object is
    // owned solely by the main thread (no concurrent access).
    std::mutex m_fetchMutex;
    std::shared_ptr<PendingFetch> m_pendingResult;

    // Scroll / layout constants
    static constexpr int LINE_HEIGHT = 36;
    static constexpr int PADDING = 16;
    static constexpr int SCREEN_W = 1024;
    static constexpr int SCREEN_H = 768;

    // Phase 2 — audit M1: respect the chrome reserved by drawAppFooter() and
    // the URL bar drawn by renderBrowserState(). Header is 64 px tall,
    // footer is 53 px tall (Y 715..768). Content lives in Y 64..715.
    static constexpr int HEADER_H = 64;
    static constexpr int FOOTER_H = 53;
    static constexpr int CONTENT_Y = HEADER_H;       // 64
    static constexpr int CONTENT_H = SCREEN_H - HEADER_H - FOOTER_H;  // 651
    static constexpr int CONTENT_W = 992;
};

} // namespace RomCloud
