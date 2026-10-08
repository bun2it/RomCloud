#pragma once
#include <string>
#include <vector>
#include <list>
#include <memory>
#include <mutex>
#include <cstdint>
#include <unordered_map>
#include <deque>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <pthread.h>
#include "../network/HttpClient.h"

namespace RomCloud {

class JsEngine;  // Duktape bridge (JsEngine.h); owned per page load.

// HTML element types
enum class HtmlElementType {
    TEXT,
    LINK,
    INPUT_TEXT,
    INPUT_PASSWORD,
    INPUT_CHECKBOX,
    INPUT_HIDDEN,  // P3: <input type=hidden> — form data only, never laid out
    SELECT,        // P3: <select> — A cycles options, submits selected value
    BUTTON,
    FORM,
    DIV,
    IMAGE,
    LINE_BREAK,
    TABLE,       // P13: <table> — grid layout of rows/cells
    TABLE_ROW,   // P13: <tr> (also thead/tbody transparently grouped)
    TABLE_CELL,  // P13: <td>/<th> — bordered box, text wrapped to column
    MEDIA,       // P13: <iframe>/<video>/<audio> — placeholder box, A plays
};

enum class TextAlign {
    LEFT,
    CENTER,
    RIGHT
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
    std::string tag;     // P1b: source tag name (no slash) for close-tag matching
    std::string text;
    // P9 (CSS): class/id for selector matching, raw inline style.
    std::string cls;
    std::string elemId;
    std::string inlineCss;
    // P2 (vnexpress plan): wrapped visual lines (pixel wrap at layout time).
    // Empty until layout() runs; render draws lines[i] at y + i*lineH.
    std::vector<std::string> lines;
    int lineH = 0;       // 0 = default LINE_HEIGHT; headings set taller
    bool bold = false;   // h1-h6, b, strong (fake-bold: double-draw, cached)
    // P9 (CSS computed style — set by applyStyles(), read by layout/render).
    bool hide = false;             // display:none (self + subtree skipped)
    int fontSize = 0;              // px, 0 = default base size
    bool centered = false;         // backward compatibility alias
    TextAlign textAlign = TextAlign::LEFT;
    bool hasExplicitAlign = false;
    bool hasColor = false;
    SDL_Color color = {0, 0, 0, 0};
    bool hasBg = false;
    SDL_Color bgColor = {0, 0, 0, 0};
    std::string bgImage;  // P13b: background-image URL (absolute, "" = none)
    int marginTop = 0, marginBottom = 0;
    int availW = 0;                // available width at layout (for centering)
    float lineHNum = 0;            // P14.2: line-height multiplier (0 = auto)
    int lineHPx = 0;               // P14.2: line-height in px (0 = auto)
    std::string href;      // for links / form action (FORM)
    std::string name;     // for form inputs (also: form method for FORM,
                            //             button type for BUTTON)
    std::string value;    // current value of input
    std::string placeholder;
    std::string src;      // for images
    std::string poster;   // for video/media poster thumbnail
    bool checked = false;
    int selected = 0;    // P3: SELECT — index into <option> children
    int maxLen = 0;      // P3: maxlength attr (0 = default 256)
    int colspan = 1;     // P13b: <td colspan> (clamped 1..10 at parse)
    int rowspan = 1;     // P13b: <td rowspan> (clamped 1..10 at parse)
    int x = 0, y = 0;
    // P1 (vnexpress plan): y is LAYOUT-space (independent of scroll).
    // Screen position = y - m_scrollY, computed at render/focus time so
    // scrolling never triggers a full relayout.
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
    int imgW = 0;        // P5b: parsed width/height reserve (intrinsicsize or
    int imgH = 0;        // width/height attrs) so images don't shift layout
};

// W3C Viewport Meta configuration (<meta name="viewport" content="...">)
struct ViewportConfig {
    bool hasMeta = false;
    bool isDeviceWidth = false;
    int width = 0;              // explicit width (e.g. 768, 980, 1024)
    float initialScale = 1.0f;
    float maximumScale = 1.0f;
    bool userScalable = true;
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
    std::string contentType;   // P4: raw Content-Type header (may be empty)
    std::string effectiveUrl;  // P4: final URL after redirects (may be empty)
    bool success = false;
};

// P9: parsed CSS rule. Specificity = ids*10000 + classes*100 + tags.
struct CssCompound {
    std::string tag;                 // empty = any tag
    std::string id;                  // empty = no id constraint
    std::vector<std::string> classes;
};
struct CssSelector {
    std::vector<CssCompound> parts;  // left-to-right (ancestor..target)
    int specificity = 0;
};
struct CssRule {
    std::vector<CssSelector> selectors;
    std::vector<std::pair<std::string, std::string>> decls;
    int order = 0;
};

// HTML Renderer - Simple browser for Wi-Fi portal and basic pages
class HtmlRenderer {
public:
    static HtmlRenderer& instance();

    // Initialize with screen dimensions. fontPath + baseSize enable sized
    // fonts for CSS font-size (P9); empty path = single font only.
    void init(SDL_Renderer* renderer, TTF_Font* font,
              const std::string& fontPath = "", int baseSize = 30);

    // Load and parse HTML content
    bool loadHtml(const std::string& html);
    bool loadUrl(const std::string& url);

    // Navigation
    void goBack();
    bool canGoBack() const;
    // P15.4: forward history (X key). loadUrl truncates forward entries,
    // so back-then-new-URL behaves like real browsers.
    void goForward();
    bool canGoForward() const;
    std::string currentUrl() const { return m_currentUrl; }
    size_t historySizeForTest() const { return m_history.size(); }
    size_t historyPosForTest() const { return m_historyPos; }

    // Input handling
    void handleInput(int button);
    void typeCharacter(char c);
    void deleteCharacter();

    // Phase 1 — audit P1-2c: API cho UIManager wire VirtualKeyboard vào
    // <input>/<input type=password> trong page. Trước audit, user focus
    // được input (UP/DOWN) rồi nhấn A để vào edit, nhưng không có cách
    // nào gõ ký tự — `m_editingText = true` chỉ thoát được bằng B. Giờ
    // UIManager gọi isEditing() để biết có nên hiện VK overlay, dùng
    // focusedInputValue()/setFocusedInputValue() để mirror giá trị
    // giữa VkState.query (do VirtualKeyboard Telex xử lý) và el.value
    // (do HtmlRenderer render).
    bool isEditing() const { return m_editingText; }
    std::string focusedInputValue() const;
    void setFocusedInputValue(const std::string& v);
    int focusedInputMaxLen() const;

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

    // P17: enqueue an async fetch (GET, or POST with form data) that flows
    // through pollFetch() exactly like loadUrl(). Used by HtmlRenderer's own
    // submitForm() and by NetSurfEngine for FULL-mode form submits.
    // targetUrl is pushed to history; method is "get" or "post".
    void fetchAsync(const std::string& targetUrl, const std::string& method,
                    const std::unordered_map<std::string, std::string>& formData);

    // Test seam (vnexpress plan P4): enqueue a synthetic fetch result so
    // content-type/redirect handling is testable without network.
    void injectFetchForTest(const std::string& url, bool success,
                            const std::string& body,
                            const std::string& contentType,
                            const std::string& effectiveUrl);

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
    // Test/plan hooks (vnexpress plan): read-only DOM access for assertions.
    const std::list<HtmlElement>& elements() const { return m_elements; }
    size_t focusableCount() const { return m_focusable.size(); }
    int scrollY() const { return m_scrollY; }
    int maxScroll() const { return m_maxScroll; }
    size_t textCacheSize() const { return m_textCache.size(); }
    bool dirty() const { return m_dirty; }
    const std::string& rawHtml() const { return m_rawHtml; }
    std::string resolveUrl(const std::string& ref) const;
    size_t imageCachedCount() const { return m_imgTex.size(); }
    size_t imagePendingCount();
    // Test seam P5: decode raw image bytes straight into the texture cache.
    bool decodeImageForTest(const std::string& url, const std::vector<uint8_t>& bytes);
    // Test seam GIF: decode (needs renderer for textures, dims either way).
    bool decodeGifForTest(const std::string& url, const std::vector<uint8_t>& bytes);
    size_t gifFrameCountForTest(const std::string& url) const {
        auto it = m_gifAnims.find(url);
        return it == m_gifAnims.end() ? 0 : it->second.frames.size();
    }
    // Test seam P9: append a stylesheet + re-apply (no network).
    void appendCssForTest(const std::string& css);
    size_t cssRuleCount() const { return m_cssRules.size(); }
    bool pageLightForTest() const { return m_pageLight; }
    // P15.1: reader indicator (header shows article mode).
    bool isReaderMode() const { return m_readerActive; }
    // Block-nav test seam: type of the currently focused node (-1 = none).
    int focusedTypeForTest() const {
        if (m_inputIndex < 0 || m_inputIndex >= (int)m_focusable.size()) return -1;
        return (int)m_focusable[(size_t)m_inputIndex]->type;
    }
    // P15.2: in-page find (case-insensitive ASCII fold; UTF-8 bytes kept).
    int findMatches(const std::string& query);
    bool findNext();  // jump to next hit (wraps), false when no hits
    void clearFind();
    int findCount() const { return (int)m_findHits.size(); }
    int findIndex() const { return m_findIndex; }
    // P13: media play requests (iframe/video A key). UIManager polls and
    // hands the URL to MpvPlayer; engine stays UI-agnostic.
    bool takeMediaRequest(std::string& out) {
        if (m_mediaRequest.empty()) return false;
        out = m_mediaRequest;
        m_mediaRequest.clear();
        return true;
    }
    // JavaScript bridge (setting js): per-page Duktape heap + DOM.
    uint64_t jsEpoch() const { return m_jsEpoch; }
    void markDirtyPublic() { markDirty(); }
    void pollJs();  // main-thread pump (timers, XHR, src scripts)
    HtmlElement* jsFindById(const std::string& id);
    HtmlElement* jsQueryFirst(const std::string& sel);
    void jsSubmit(HtmlElement* el);
    void jsActivate(HtmlElement* el);
    // Settings screen: re-run style + image pipelines after a toggle.
    void refreshAfterSettings();
    // P9d: kill-switch for <link> fetching (offline-deterministic tests).
    void setCssFetchEnabled(bool on) { m_cssFetchEnabled = on; }

    // W3C Viewport Meta integration
    const ViewportConfig& viewport() const { return m_viewport; }
    int effectiveViewportWidth() const;

    // Standard modern browser headers for compliant CDN / anti-hotlink fetching
    static std::vector<std::string> getBrowserHeaders(const std::string& referer = "");
    // Test seam: measure text the same way layout does (CSS-scaled font).
    int measureForTest(const std::string& text, int cssPx) {
        return textWidth(text, fontPxForTest(cssPx));
    }
    int fontPxForTest(int v) const { return cssPx(v <= 0 ? m_baseFontSize : v); }
    // P8: URL-bar focus integration (UIManager moves between URL bar and the
    // first page widget like YouTube's search input).
    int focusIndex() const { return m_inputIndex; }
    void focusFirst() {
        if (m_focusable.empty()) return;
        m_editingText = false;
        m_inputIndex = 0;
        updateFocus();
        markDirty();
    }

private:
    HtmlRenderer() = default;
    ~HtmlRenderer();  // out-of-line: JsEngine is incomplete in this header
    HtmlRenderer(const HtmlRenderer&) = delete;
    HtmlRenderer& operator=(const HtmlRenderer&) = delete;

    // HTML Parsing
    void parseHtml(const std::string& html);
    std::string extractTag(const std::string& html, size_t& pos, std::string& tagName, std::string& attributes);
    std::string getAttribute(const std::string& attrs, const std::string& name);
    std::string decodeHtmlEntities(const std::string& str);
    void appendUtf8(std::string& out, unsigned code);
    // P2: pixel word-wrap using the loaded font. Returns visual lines.
    std::vector<std::string> wrapText(const std::string& text, int maxWidth, int px = 0);

    // Layout
    void layout();
    void layoutElement(HtmlElement& el, int& y, int x, int width);
    TTF_Font* fontFor(int px);
    int fontPx(const HtmlElement& el) const {
        return cssPx(el.fontSize > 0 ? el.fontSize : m_baseFontSize);
    }
    // P15.3: readability scale from browser settings (100/120/150%).
    // Desktop CSS px would render tiny on the Brick's small panel, so a
    // floor/ceiling keeps body readable without poster-size headlines.
    int cssPx(int v) const;
    int textWidth(const std::string& text, int px = 0);

    // Rendering
    void renderElement(const HtmlElement& el, bool linkHot = false);

    // Navigation
    void submitForm(const HtmlElement& form);

    // Focus management
    void focusNext();
    void focusPrev();
    void updateFocus();
    void ensureFocusVisible();
    // Block navigation (viewport = card of blocks; dpad walks events inside
    // the current block, UP/DOWN jumps between blocks with events).
    int blockOf(int focusIdx) const;
    std::vector<int> blockMembers(int block) const;
    void focusInBlockStep(int dir);
    void focusBlockStep(int dir);
    void focusInBlockPrev() { focusInBlockStep(-1); }
    void focusInBlockNext() { focusInBlockStep(+1); }
    void focusBlockUp() { focusBlockStep(-1); }
    void focusBlockDown() { focusBlockStep(+1); }
    // Page scroll (L1/R1): viewport drives, focus follows to nearest visible
    // — never yanks back like ensureFocusVisible() would.
    void focusNearestVisible();
    // Read-scroll (dpad UP/DOWN): line-by-line page move + highlight the
    // nearest event (link underline, input border). A acts on the highlight.
    void scrollRead(int delta);

    // Helper rendering functions
    void drawRect(int x, int y, int w, int h, SDL_Color color, bool filled);
    void drawText(const std::string& text, int x, int y, SDL_Color color, TTF_Font* font, bool centered, int px = 0);
    // P13b: stretch a cached background-image over (x,y,w,h); false = none.
    bool drawBgImageBox(const std::string& url, int x, int y, int w, int h);

    // Internal: load a URL without pushing it onto the back/forward history.
    // Used by goBack() and refresh() so navigating history does not create
    // duplicate entries (audit issue C4).
    bool loadUrlNoHistory(const std::string& url);

    // Internal: called from the worker thread. Hands off ownership of
    // `result` to the main thread via m_pendingResult + mutex.
    // The renderer takes ownership and is responsible for deleting/freeing
    // the PendingFetch object.
    void enqueueFetchResult(PendingFetch* result);

    // P9: CSS engine (subset for news/portals — no float/flex/position).
    void parseStylesheet(const std::string& css);
    void applyStyles();
    bool matchSelector(const CssSelector& sel, const HtmlElement& el,
                       const std::vector<const HtmlElement*>& ancestors) const;
    void applyDecls(HtmlElement& el,
                    const std::vector<std::pair<std::string, std::string>>& decls,
                    bool isInline);
    static bool parseColor(const std::string& v, SDL_Color& out);
    static int parsePx(const std::string& v);
    void pumpCss();  // main thread: fetch queued <link> sheets, apply
    void clearCss();
    // P11 reader mode: keep the article, hide chrome/comments.
    void applyReaderMode();
    // JavaScript (setting js): script capture/run/poll + DOM helpers.
    void runJs();
    void activateFocused();  // shared A-key action (dpad + JS click())

    // Input field
    int m_inputIndex = -1;
    bool m_editingText = false;

    // State
    SDL_Renderer* m_renderer = nullptr;
    TTF_Font* m_font = nullptr;
    // P9: sized fonts for CSS font-size (same file, cached per px).
    std::string m_fontPath;
    int m_baseFontSize = 30;
    std::unordered_map<int, TTF_Font*> m_fonts;
    // list (not vector): pointers into m_elements stay valid across
    // push_back — fixes the dangling-pointer bug in m_focusable (audit C2).
    std::list<HtmlElement> m_elements;
    std::vector<HtmlElement*> m_focusable;
    int m_scrollY = 0;
    int m_maxScroll = 0;
    std::vector<std::string> m_history;
    size_t m_historyPos = 0;
    std::string m_currentUrl;
    std::string m_rawHtml;
    bool m_loading = false;
    bool m_error = false;
    std::string m_errorMsg;

    // Worker -> main thread handoff. Mutex protects m_pendingResult only;
    // once the main thread has the shared_ptr, the PendingFetch object is
    // owned solely by the main thread (no concurrent access).
    std::mutex m_fetchMutex;
    std::shared_ptr<PendingFetch> m_pendingResult;

    // P1 (vnexpress plan): text texture cache. drawText() created +
    // destroyed one SDL_Texture per node per frame — the #1 scroll bottleneck.
    // Key = text + color bytes (single font). Cleared on init()/stop().
    struct CachedText {
        SDL_Texture* tex = nullptr;
        int w = 0;
        int h = 0;
        uint64_t lastUsed = 0;
    };
    std::unordered_map<std::string, CachedText> m_textCache;
    uint64_t m_texTick = 0;
    static constexpr size_t kMaxCachedTexts = 256;

    // P1: dirty flag — render() skips the DOM walk when nothing changed
    // (no scroll, no DOM update, no focus move) since the frame is identical.
    bool m_dirty = true;
    void markDirty() { m_dirty = true; }
    void clearTextCache();

    // P5 (vnexpress plan): async raster images (PNG/JPEG via SDL_image).
    // Worker threads download + decode to SDL_Surface (CPU-only, thread
    // safe); the main thread uploads surfaces to textures in pumpImages().
    // Bounded: 4 concurrent fetches, 24 cached textures, 2MB/file (curl
    // cap), downscaled to content width / 400px height.
    struct ImageTex {
        SDL_Texture* tex = nullptr;
        int w = 0;
        int h = 0;
    };
    struct ReadyImage {
        std::string url;
        SDL_Surface* surface = nullptr;  // owned by main thread after handoff
    };
    // GIF animation (nsgif, setting gifAnim): decoded frame surfaces with
    // per-frame delays, uploaded to textures on the main thread.
    struct GifFrameSurf {
        SDL_Surface* surf = nullptr;
        uint32_t delayCs = 10;
    };
    struct GifFrameTex {
        SDL_Texture* tex = nullptr;
        uint32_t delayCs = 10;
    };
    struct GifAnim {
        std::vector<GifFrameTex> frames;
        int w = 0;
        int h = 0;
        uint32_t totalCs = 0;
    };
    struct ReadyGif {
        std::string url;
        std::vector<GifFrameSurf> frames;
    };
    // P13: pending media URL set by A on a MEDIA node, taken by UIManager.
    std::string m_mediaRequest;
    // P15.1: true when reader mode kept an article on the current page.
    bool m_readerActive = false;
    // P15.2: in-page find hits (TEXT node pointers, stable in list).
    std::vector<HtmlElement*> m_findHits;
    int m_findIndex = -1;
    std::unordered_map<std::string, ImageTex> m_imgTex;
    std::unordered_map<std::string, GifAnim> m_gifAnims;
    static constexpr size_t kMaxGifCached = 8;
    std::deque<std::string> m_imgWaiting;   // queued, no worker yet
    int m_imgInFlight = 0;
    uint64_t m_imgEpoch = 0;
    std::mutex m_imgMutex;
    std::vector<ReadyImage> m_imgReady;     // guarded by m_imgMutex
    std::vector<ReadyGif> m_gifReady;       // guarded by m_imgMutex
    // P9c: negative cache — URLs that failed fetch/decode are never
    // retried (device log showed img_blank.gif refetched in a loop,
    // hogging all 4 worker slots and hiding real images).
    std::unordered_map<std::string, bool> m_imgFailed;
    static constexpr int kMaxImgInFlight = 4;
    static constexpr size_t kMaxImgCached = 24;
    static constexpr size_t kMaxImgFailed = 128;
    static constexpr int kImgMaxH = 400;
    // P9c: vector formats need libraries the Brick build may lack (SVG
    // always, WebP sometimes) — don't waste workers/bandwidth on them.
    // (SVG now rasterized in-house via NanoSVG when the svg setting is on;
    // the worker below still gates on content + setting.)
    static bool imageUrlSupported(const std::string& url);
    void markImageFailed(const std::string& url);

    // P9: stylesheets (inline <style> captured at parse + fetched <link>).
    std::vector<CssRule> m_cssRules;
    int m_cssOrder = 0;
    int m_sheetsParsed = 0;  // how many m_styleSheets went through the parser
    // P9d: kill-switch for <link> fetching (tests run offline-deterministic;
    // queued URLs just accumulate). Default ON.
    bool m_cssFetchEnabled = true;
    // P9b: page theme from body/html background. Browsers default to a
    // WHITE canvas and vnexpress ships no body background (it assumes
    // white) — so white is our default too; dark pages opt in via CSS.
    // NOTE: body/html create no tree nodes (structural skip), so their raw
    // style/class are captured at parse into m_bodyInline/m_bodyCls.
    // W3C Viewport Meta state (<meta name="viewport" content="...">)
    ViewportConfig m_viewport;
    void parseViewportMeta(const std::string& content);
    static bool evaluateMediaQuery(const std::string& query, int viewportW);

    std::string m_bodyInline;
    std::string m_bodyCls;
    SDL_Color m_pageBg = {255, 255, 255, 255};
    bool m_pageLight = true;
    void updatePageTheme();
    SDL_Color inkColor() const {
        return m_pageLight ? SDL_Color{25, 25, 25, 255} : SDL_Color{200, 200, 200, 255};
    }
    SDL_Color inkBoldColor() const {
        return m_pageLight ? SDL_Color{0, 0, 0, 255} : SDL_Color{235, 235, 235, 255};
    }
    SDL_Color linkBaseColor() const {
        return m_pageLight ? SDL_Color{7, 109, 182, 255} : SDL_Color{100, 150, 255, 255};
    }
    // P10 audit: widgets/placeholders follow the page theme. Fixed dark
    // boxes on a white page read as "gray blobs" (the reported issue).
    SDL_Color widgetBg(bool focused) const {
        if (focused) return m_pageLight ? SDL_Color{215, 235, 245, 255}
                                        : SDL_Color{30, 40, 60, 255};
        return m_pageLight ? SDL_Color{255, 255, 255, 255} : SDL_Color{20, 25, 35, 255};
    }
    SDL_Color widgetEdge(bool focused) const {
        if (focused) return SDL_Color{0, 180, 216, 255};
        return m_pageLight ? SDL_Color{170, 170, 170, 255} : SDL_Color{60, 70, 90, 255};
    }
    SDL_Color widgetInk() const {
        return m_pageLight ? SDL_Color{20, 20, 20, 255} : SDL_Color{220, 220, 220, 255};
    }
    SDL_Color widgetDim() const {
        return m_pageLight ? SDL_Color{130, 130, 130, 255} : SDL_Color{100, 100, 100, 255};
    }
    SDL_Color phBoxBg() const {
        return m_pageLight ? SDL_Color{238, 238, 238, 255} : SDL_Color{30, 30, 30, 255};
    }
    SDL_Color phBoxEdge() const {
        return m_pageLight ? SDL_Color{205, 205, 205, 255} : SDL_Color{60, 60, 60, 255};
    }
    std::vector<std::string> m_styleSheets;  // raw CSS, guarded by m_cssMutex
    std::vector<std::string> m_cssQueue;    // pending <link> stylesheet URLs
    // P9e: external <link> sheets have their OWN budget (queued+fetched).
    // Inline <style> blocks fill m_styleSheets during parse — with a shared
    // cap, a page with 8 inline blocks (e.g. vnexpress) starved every <link>
    // and lost all its layout CSS. Guarded by m_cssMutex.
    int m_extCssCount = 0;
    int m_cssInFlight = 0;
    std::mutex m_cssMutex;
    std::vector<std::string> m_cssReady;    // fetched bodies, guarded
    // JavaScript state: per-page scripts + engine (epoch-checked).
    std::unique_ptr<JsEngine> m_js;
    uint64_t m_jsEpoch = 0;
    std::vector<std::string> m_scripts;     // inline bodies, document order
    std::vector<std::string> m_scriptSrcs;  // <script src>, document order
    std::vector<std::string> m_jsQueue;     // pending script URLs
    int m_jsInFlight = 0;
    std::mutex m_jsMutex;
    std::vector<std::pair<std::string, std::string>> m_jsReady;  // (url, body)
    static constexpr int kMaxJsInFlight = 2;
    static constexpr size_t kMaxJsBytes = 64 * 1024;
    static constexpr size_t kMaxJsFiles = 4;
    static constexpr int kMaxCssInFlight = 2;
    static constexpr size_t kMaxCssBytes = 512 * 1024;
    static constexpr size_t kMaxCssSheets = 8;

    void pumpImages();          // main thread: upload ready textures
    void queueImages();         // walk tree once per page, queue fetches
    void ensureImage(const std::string& url);  // queue or start fetch
    void startImageWorker(const std::string& url);  // slot already reserved
    // GIF animation decode (vendored nsgif, max 10 frames, pre-scaled).
    static bool decodeGifFrames(const uint8_t* data, size_t len,
                                std::vector<GifFrameSurf>& out, int maxW,
                                int maxH);
    void clearImages();
    static void downscaleSurface(SDL_Surface*& surf, int maxW, int maxH);

public:
    // Scroll / layout constants
    static constexpr int LINE_HEIGHT = 36;
    static constexpr int PADDING = 16;
    static constexpr int SCREEN_W = 1024;
    static constexpr int SCREEN_H = 768;

    // Chrome reserved heights:
    // Header: 64 px tall (Y 0..63)
    // URL bar: 48 px tall directly below header (Y 64..111)
    // Content area: Y 112..714 (height = 603 px)
    // Footer: 53 px tall at bottom (Y 715..767)
    static constexpr int HEADER_H = 64;
    static constexpr int URL_BAR_H = 48;
    static constexpr int FOOTER_H = 53;
    static constexpr int CONTENT_Y = HEADER_H + URL_BAR_H;            // 112
    static constexpr int CONTENT_H = SCREEN_H - CONTENT_Y - FOOTER_H; // 603
    static constexpr int CONTENT_W = 992;
};

} // namespace RomCloud
