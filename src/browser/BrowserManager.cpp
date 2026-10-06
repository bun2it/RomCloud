#include "BrowserManager.h"
#include "HtmlRenderer.h"
#include "../logging/Logger.h"
#include <unistd.h>  // access()

namespace RomCloud {

BrowserManager& BrowserManager::instance() {
    static BrowserManager inst;
    return inst;
}

void BrowserManager::init() {
    HtmlRenderer::instance().init(nullptr, nullptr);
    m_state = BrowserState::IDLE;
    m_currentUrl.clear();
    m_errorMsg.clear();
}

bool BrowserManager::openUrl(const std::string& url) {
    Logger::info("BrowserManager: Opening URL: " + url);

    // Phase 2 — audit M6: whitelist the http/https schemes. This is a basic
    // browser for Wi-Fi portals; we don't want to honor file://, javascript:,
    // data:, ftp://, or anything else that could read local files or run
    // unexpected code. Anything else is rejected up-front with an error state.
    if (url.substr(0, 7) != "http://" && url.substr(0, 8) != "https://") {
        Logger::error("BrowserManager: refused non-http(s) URL: " + url);
        m_errorMsg = "Only http:// and https:// URLs are supported";
        m_state = BrowserState::ERROR;
        m_currentUrl.clear();
        return false;
    }

    m_currentUrl = url;
    m_errorMsg.clear();
    m_state = BrowserState::LOADING;
    if (HtmlRenderer::instance().loadUrl(url)) {
        m_state = BrowserState::RENDERING;
        return true;
    } else {
        m_errorMsg = HtmlRenderer::instance().errorMessage();
        // If the underlying error looks like a network-level failure (DNS,
        // connection refused, timeout, …) and /etc/resolv.conf is missing,
        // tell the user — that's almost always the cause on a minimal Linux
        // image like the TrimUI Brick stock firmware.
        bool looksNetworky =
            m_errorMsg.find("Could not resolve") != std::string::npos ||
            m_errorMsg.find("Couldn't resolve") != std::string::npos ||
            m_errorMsg.find("Couldn't connect") != std::string::npos ||
            m_errorMsg.find("Connection refused") != std::string::npos ||
            m_errorMsg.find("Timeout") != std::string::npos ||
            m_errorMsg.find("timed out") != std::string::npos;
        if (looksNetworky && access("/etc/resolv.conf", F_OK) != 0) {
            m_errorMsg += " — /etc/resolv.conf missing (no DNS)";
        }
        m_state = BrowserState::ERROR;
        return false;
    }
}

void BrowserManager::close() {
    stop();
    // Phase 0 — audit N2: stop() already transitions m_state back to IDLE,
    // so the explicit assignment here was redundant. close() only needs to
    // reset the URL/error fields that are owned by BrowserManager itself.
    m_currentUrl.clear();
    m_errorMsg.clear();
}

void BrowserManager::goBack() {
    // Phase 0 — audit N3: goBack triggers a new network fetch, so update
    // m_state to LOADING so the UI shows the loading overlay while the
    // previous page is being retrieved. (RENDERING will transition on the
    // next openUrl(); a more accurate transition could be wired through
    // HtmlRenderer's pollFetch() in a later pass.)
    //
    // Phase 0 — audit N4 cleanup: the previous NETSURF branch is gone; the
    // HTML_RENDERER is the only engine now, so the switch collapses.
    HtmlRenderer::instance().goBack();
    m_state = BrowserState::LOADING;
}

bool BrowserManager::canGoBack() const {
    return HtmlRenderer::instance().canGoBack();
}

void BrowserManager::refresh() {
    if (!m_currentUrl.empty()) {
        openUrl(m_currentUrl);
    }
}

void BrowserManager::stop() {
    // Phase 0 — audit N4 cleanup: only HtmlRenderer to stop now.
    HtmlRenderer::instance().stop();
    m_state = BrowserState::IDLE;
}

} // namespace RomCloud
