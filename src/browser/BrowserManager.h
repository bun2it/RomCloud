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

    // State
    BrowserState state() const { return m_state; }
    void setState(BrowserState s) { m_state = s; }
    void setError(const std::string& msg) { m_errorMsg = msg; m_state = BrowserState::ERROR; }
    std::string currentUrl() const { return m_currentUrl; }
    std::string errorMessage() const { return m_errorMsg; }

    // Cleanup
    void stop();

private:
    BrowserManager() = default;
    ~BrowserManager() = default;
    BrowserManager(const BrowserManager&) = delete;
    BrowserManager& operator=(const BrowserManager&) = delete;

    BrowserState m_state = BrowserState::IDLE;
    std::string m_currentUrl;
    std::string m_errorMsg;
};

} // namespace RomCloud
