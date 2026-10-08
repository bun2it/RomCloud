#pragma once
#include <string>
#include <memory>

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
    bool fullEngine = true; // true = FULL (NetSurf W3C Core), false = LITE
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
    void refresh();

    // Engine Mode
    BrowserEngine engineMode() const { return m_settings.fullEngine ? BrowserEngine::FULL : BrowserEngine::LITE; }
    void setEngineMode(BrowserEngine mode) {
        m_settings.fullEngine = (mode == BrowserEngine::FULL);
        saveSettings();
    }

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

    BrowserState m_state = BrowserState::IDLE;
    std::string m_currentUrl;
    std::string m_errorMsg;
    BrowserSettings m_settings;
};

} // namespace RomCloud
