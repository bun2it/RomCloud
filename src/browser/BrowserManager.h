#pragma once
#include <string>
#include <memory>
#include <vector>
#include <atomic>

namespace RomCloud {

// Browser state
enum class BrowserState {
    IDLE,
    LOADING,
    RENDERING,
    ERROR,
};

// Browser engine mode: Lite for portal/fast browsing, Full for general web
enum class BrowserEngine {
    LITE,  // Fast, lightweight engine (default for Captive Portal)
    FULL   // Enhanced layout & CSS engine for general browsing
};

// Browser feature settings (web browser settings screen, SELECT in browser).
struct BrowserSettings {
    bool images = true;     // P5 raster images (implemented)
    bool css = true;        // P9 stylesheets (implemented)
    int fontScalePct = 120; // P15.3: 100/120/150 (implemented)
    bool gifAnim = false;   // animated GIF: off (1st frame only)
    bool video = false;     // in-page video: off (1st frame / poster only)
    bool svg = false;       // SVG raster
    bool js = false;        // JavaScript engine
    bool fullEngine = true; // true = FULL (NetSurf), false = LITE (HtmlRenderer)
    // Full Engine: Mặc định sử dụng NetSurf Native Daemon (NetSurfWorkerBridge)
    // để NetSurf tự xử lý layout 2D, CSS cascade và JavaScript qua framebuffer SHM.
    bool inProcessFull = false;
    bool article = true;    // P11 reader mode: keep article, hide chrome
};

// Browser Manager - unified interface for web browsing
class BrowserManager {
public:
    static BrowserManager& instance();

    // Initialization
    void init();

    // Start browsing
    bool openUrl(const std::string& url);
    void close();

    // Navigation
    void goBack();
    bool canGoBack() const;
    void goForward();
    bool canGoForward() const;
    void refresh();

    // Engine Mode
    BrowserEngine engineMode() const { return m_settings.fullEngine ? BrowserEngine::FULL : BrowserEngine::LITE; }
    void setEngineMode(BrowserEngine mode) {
        m_settings.fullEngine = (mode == BrowserEngine::FULL);
        saveSettings();
    }

    // P24: history test seams. Cursor-based history lets us goBack AND
    // goForward after the user opens several URLs. Used by test_browser.cpp
    // to verify the in-process navigation paths don't re-fork the daemon.
    size_t historySizeForTest() const { return m_history.size(); }
    size_t historyPosForTest() const { return m_historyPos; }
    // P24: test-only seam to push a URL onto the history cursor without
    // triggering the async fetch+layout side-effect (which would race the
    // test runner). Same logic as openUrl()'s history-cursor update but
    // skips the network/daemon/engine branch.
    void pushHistoryForTest(const std::string& url) {
        if (m_historyPos + 1 < m_history.size()) {
            m_history.resize(m_historyPos + 1);
        }
        m_history.push_back(url);
        m_historyPos = m_history.size() - 1;
        m_currentUrl = url;
    }
    // P24: test-only fetch-suppression. When set, openUrlNoHistory() will
    // update the cursor + currentUrl but skip the actual fetchAndLoadFull /
    // NetSurfWorkerBridge / HtmlRenderer::loadUrl branch. This keeps the
    // test offline and avoids log spam from real HTTP requests to
    // example.com during CI. Default false.
    void setTestFetchDisabled(bool v) { m_testFetchDisabled = v; }
    bool testFetchDisabled() const { return m_testFetchDisabled; }

    // State
    BrowserState state() const { return m_state; }
    void setState(BrowserState s) { m_state = s; }
    void setError(const std::string& msg) { m_errorMsg = msg; m_state = BrowserState::ERROR; }
    std::string currentUrl() const { return m_currentUrl; }
    std::string errorMessage() const { return m_errorMsg; }

    // Settings (persisted to browser.cfg under the data dir)
    BrowserSettings& settings() { return m_settings; }
    const BrowserSettings& settings() const { return m_settings; }
    void saveSettings();

    // P20: read-only session flag — true after in-process FULL init failed
    // and we've auto-flipped to daemon. Used by UIManager render loop to
    // route the right way without touching the user-controlled settings.
    bool testInProcessFailed() const { return m_inProcessFullFailed; }

    // Cleanup
    void stop();

    // Normalize a user-typed address: trim whitespace, prepend https://
    // when no scheme is present ("google.com" -> "https://google.com").
    static std::string normalizeUrl(const std::string& raw);

private:
    BrowserManager() = default;
    ~BrowserManager() = default;
    BrowserManager(const BrowserManager&) = delete;
    BrowserManager& operator=(const BrowserManager&) = delete;

    void loadSettings();
    std::string settingsPath() const;

    // P20: async fetch HTML and feed NetSurfEngine (in-process FULL).
    // Spawns a detached pthread so the UI stays responsive. On error,
    // sets m_state = BrowserState::ERROR with a user-readable message.
    // Mirrors HtmlRenderer::loadUrl's worker-thread pattern.
    void fetchAndLoadFull(const std::string& url);

    // P24: load a URL without modifying m_history / m_historyPos. Used by
    // goBack() / goForward() / refresh() so navigating history doesn't
    // push a new entry (mirrors HtmlRenderer::loadUrlNoHistory).
    void openUrlNoHistory(const std::string& url);

    // P20: switch to daemon fallback when in-process FULL init fails
    // (missing libcss/libhubbub). Called from UIManager at boot if
    // NetSurfEngine reports !isInitialized() after init().
    void fallbackToDaemon(const std::string& reason);

    BrowserState m_state = BrowserState::IDLE;
    std::string m_currentUrl;
    std::string m_errorMsg;
    BrowserSettings m_settings;
    // P24: cursor-based history. m_history[m_historyPos] is the current
    // page. openUrl() truncates forward entries (m_history.size()-1 ..
    // m_historyPos) before pushing the new URL. goBack() decrements
    // m_historyPos without mutating the vector; goForward() increments.
    std::vector<std::string> m_history;
    size_t m_historyPos = 0;
    // P20: true after we tried in-process FULL and it failed at init
    // time (auto-fallback to daemon for the rest of the session).
    bool m_inProcessFullFailed = false;
    // P24: test-only fetch-suppression (see setTestFetchDisabled). When
    // true, openUrlNoHistory() updates cursor/currentUrl but skips the
    // actual fetch+layout+engine side-effect. Lets test_browser.cpp
    // exercise the cursor logic offline.
    bool m_testFetchDisabled = false;
};

} // namespace RomCloud
