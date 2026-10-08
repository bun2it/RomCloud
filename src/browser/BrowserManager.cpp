#include "BrowserManager.h"
#include "HtmlRenderer.h"
#include "../logging/Logger.h"
#include "../config/AppConfig.h"
#include "netsurf/NetSurfBridge.h"
#include "netsurf/NetSurfEngine.h"
#include <unistd.h>  // access()
#include <fstream>
#include <sstream>

namespace RomCloud {

BrowserManager& BrowserManager::instance() {
    static BrowserManager inst;
    return inst;
}

void BrowserManager::init() {
    m_state = BrowserState::IDLE;
    m_currentUrl.clear();
    m_errorMsg.clear();
    loadSettings();

    // NetSurf Core Engine initialization
    NetSurfBridge::instance().init();
}

std::string BrowserManager::settingsPath() const {
    return AppConfig::instance().getDataDir() + "/browser.cfg";
}

void BrowserManager::loadSettings() {
    m_settings = BrowserSettings{};  // defaults first
    std::ifstream f(settingsPath());
    if (!f) return;
    std::string line;
    auto boolean = [](const std::string& v) {
        return v == "1" || v == "true" || v == "yes" || v == "on";
    };
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "images") m_settings.images = boolean(v);
        else if (k == "css") m_settings.css = boolean(v);
        else if (k == "fontScalePct") {
            try {
                int p = std::stoi(v);
                if (p == 100 || p == 120 || p == 150) m_settings.fontScalePct = p;
            } catch (...) {}
        } else if (k == "gifAnim") m_settings.gifAnim = boolean(v);
        else if (k == "video") m_settings.video = boolean(v);
        else if (k == "svg") m_settings.svg = boolean(v);
        else if (k == "js") m_settings.js = boolean(v);
        else if (k == "fullEngine") m_settings.fullEngine = boolean(v);
        else if (k == "article") m_settings.article = boolean(v);
    }
}

void BrowserManager::saveSettings() {
    std::ofstream f(settingsPath(), std::ios::trunc);
    if (!f) {
        Logger::warn("BrowserManager: cannot write " + settingsPath());
        return;
    }
    f << "images=" << (m_settings.images ? 1 : 0) << "\n";
    f << "css=" << (m_settings.css ? 1 : 0) << "\n";
    f << "fontScalePct=" << m_settings.fontScalePct << "\n";
    f << "gifAnim=" << (m_settings.gifAnim ? 1 : 0) << "\n";
    f << "video=" << (m_settings.video ? 1 : 0) << "\n";
    f << "svg=" << (m_settings.svg ? 1 : 0) << "\n";
    f << "js=" << (m_settings.js ? 1 : 0) << "\n";
    f << "fullEngine=" << (m_settings.fullEngine ? 1 : 0) << "\n";
    f << "article=" << (m_settings.article ? 1 : 0) << "\n";
}

bool BrowserManager::openUrl(const std::string& url) {
    std::string fixed = normalizeUrl(url);
    Logger::info("BrowserManager: Opening URL: " + fixed);

    // Phase 2 — audit M6: whitelist the http/https schemes. This is a basic
    // browser for Wi-Fi portals; we don't want to honor file://, javascript:,
    // data:, ftp://, or anything else that could read local files or run
    // unexpected code. Anything else is rejected up-front with an error state.
    if (fixed.substr(0, 7) != "http://" && fixed.substr(0, 8) != "https://") {
        Logger::error("BrowserManager: refused non-http(s) URL: " + fixed);
        m_errorMsg = "Only http:// and https:// URLs are supported";
        m_state = BrowserState::ERROR;
        m_currentUrl.clear();
        return false;
    }

    m_currentUrl = fixed;
    m_errorMsg.clear();
    m_state = BrowserState::LOADING;
    // Stay in LOADING until HtmlRenderer processes the fetch in pollFetch()
    // — loadUrl() only spawns a worker thread, it does not perform the
    // network request synchronously. Setting RENDERING here would make
    // the spinner overlay invisible because the state transition happens
    // in microseconds, not in the 100ms+ that the HTTP fetch takes.
    HtmlRenderer::instance().loadUrl(fixed);
    return true;
}

std::string BrowserManager::normalizeUrl(const std::string& raw) {
    // Trim leading/trailing whitespace (VK can leave some).
    size_t b = raw.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = raw.find_last_not_of(" \t\r\n");
    std::string url = raw.substr(b, e - b + 1);
    // Already has a scheme (http://, https://, or something the whitelist
    // will reject like file://) — leave it alone.
    if (url.find("://") != std::string::npos) return url;
    // No scheme: user typed "google.com" — assume https like every
    // mainstream browser's address bar.
    return "https://" + url;
}

void BrowserManager::close() {
    stop();
    m_currentUrl.clear();
    m_errorMsg.clear();
    NetSurfEngine::instance().clear();
}

void BrowserManager::goBack() {
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
    HtmlRenderer::instance().stop();
    NetSurfEngine::instance().clear();
    m_state = BrowserState::IDLE;
}

} // namespace RomCloud
