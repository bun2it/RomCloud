#include "BrowserManager.h"
#include "HtmlRenderer.h"
#include "chrome/ChromeBridge.h"
#include "../network/HttpClient.h"
#include "../logging/Logger.h"
#include "../config/AppConfig.h"
#include "netsurf/NetSurfBridge.h"
#include "netsurf/NetSurfEngine.h"
#include "netsurf/NetSurfWorkerBridge.h"
#include <unistd.h>  // access()
#include <pthread.h>
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
        else if (k == "inProcessFull") m_settings.inProcessFull = boolean(v);
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
    f << "inProcessFull=" << (m_settings.inProcessFull ? 1 : 0) << "\n";
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

    // P24: cursor-based history. Truncate any forward entries (e.g. user
    // did goBack() and then opened a new page) so the new URL becomes the
    // new tail. This matches the LITE engine's pattern in
    // HtmlRenderer::loadUrl() and is what users expect from a Back/Forward
    // stack: any branch off the current page should forget the abandoned
    // future. After the push, m_historyPos points at the new entry.
    if (m_historyPos + 1 < m_history.size()) {
        m_history.resize(m_historyPos + 1);
    }
    if (m_history.empty() || m_history.back() != fixed) {
        m_history.push_back(fixed);
    }
    m_historyPos = m_history.size() - 1;

    if (m_settings.fullEngine) {
        // FULL engine = RC-Web handoff (thay NetSurf; code NetSurf giữ
        // riêng trong src/browser/netsurf, không route tới nữa).
        // Không có rc-web -> rớt về LITE để vẫn đọc được.
        Logger::info("BrowserManager: FULL engine (Chrome handoff) for " + fixed);
        if (!ChromeBridge::instance().openUrl(fixed)) {
            Logger::warn("BrowserManager: Chrome thiếu, fallback LITE");
            HtmlRenderer::instance().loadUrl(fixed);
        }
        return true;
    } else {
        HtmlRenderer::instance().loadUrl(fixed);
    }
    return true;
}

void BrowserManager::fetchAndLoadFull(const std::string& url) {
    // P20: detached worker thread fetches HTML via HttpClient, then hands
    // the body to NetSurfEngine::loadHtml which queues an async layout.
    // Mirrors HtmlRenderer::loadUrl's pattern (heap-allocated payload to
    // avoid the static-s url race the LITE engine fixed in audit C1).
    struct Payload {
        std::string url;
        bool success = false;
        std::string body;
        std::string error;
        std::string effectiveUrl;
        std::string contentType;
    };
    Payload* p = new Payload();
    p->url = url;
    m_state = BrowserState::LOADING;
    pthread_t tid;
    int rc = pthread_create(&tid, nullptr, [](void* arg) -> void* {
        std::unique_ptr<Payload> owned(static_cast<Payload*>(arg));
        // P20: cap 1.5MB body, 30s timeout, cookie+UA headers (same as
        // HtmlRenderer::loadUrl). 15 in the LITE call is the timeout
        // parameter (seconds), matching m_settings defaults elsewhere.
        HttpResponse resp = HttpClient::instance().get(
            owned->url, HtmlRenderer::getBrowserHeaders(), 15);
        owned->success = resp.success;
        owned->body = std::move(resp.body);
        owned->error = std::move(resp.error);
        owned->effectiveUrl = std::move(resp.effectiveUrl);
        owned->contentType = std::string();
        for (const auto& kv : resp.headers) {
            std::string k = kv.first;
            std::transform(k.begin(), k.end(), k.begin(), ::tolower);
            if (k == "content-type") {
                owned->contentType = kv.second;
                break;
            }
        }
        // Apply on the main thread via a small trampoline: write into
        // NetSurfEngine from here (it's already mutex-protected for the
        // rawHtml/currentUrl fields, and the layout job spawns its own
        // worker so there's no UI-thread blocking).
        if (!owned->success) {
            Logger::error("BrowserManager (FULL in-process): fetch failed: " +
                          owned->error);
            BrowserManager::instance().setError(
                "Tải trang thất bại: " + owned->error);
            return nullptr;
        }
        // Reject non-HTML (image/binary) — lenient on empty/missing type (same as HtmlRenderer P4).
        std::string ct = owned->contentType;
        std::transform(ct.begin(), ct.end(), ct.begin(), ::tolower);
        bool isHtml = ct.empty() ||
                      ct.find("html") != std::string::npos ||
                      ct.find("xml")  != std::string::npos ||
                      ct.find("text") != std::string::npos ||
                      ct.find("xhtml") != std::string::npos;
        if (!isHtml) {
            Logger::warn("BrowserManager (FULL in-process): non-HTML content-type: " + ct);
            BrowserManager::instance().setError(
                "Trang trả về không phải HTML (" + ct + ")");
            return nullptr;
        }
        // All good — feed the engine. Use effective URL (handles
        // redirects) as the base for relative <link>/<a>.
        Logger::info("BrowserManager (FULL in-process): " +
                     std::to_string(owned->body.size()) + "B from " +
                     owned->effectiveUrl);
        NetSurfEngine::instance().loadHtml(owned->body, owned->effectiveUrl);
        return nullptr;
    }, p);
    if (rc != 0) {
        delete p;
        m_state = BrowserState::ERROR;
        m_errorMsg = "Failed to spawn fetch thread";
        Logger::error("BrowserManager (FULL in-process): pthread_create rc=" +
                      std::to_string(rc));
        return;
    }
    pthread_detach(tid);
}

void BrowserManager::fallbackToDaemon(const std::string& reason) {
    // P20: called when NetSurfEngine init fails (libs missing on device,
    // libcss/libhubbub couldn't be loaded). Flip the in-process flag off
    // for the rest of the session so openUrl() routes to the daemon
    // binary path. Does NOT change m_settings.inProcessFull — that's
    // user-controlled; only this session flips.
    if (m_inProcessFullFailed) return;
    Logger::warn("BrowserManager: in-process FULL unavailable, falling back to daemon: " + reason);
    m_inProcessFullFailed = true;
}

std::string BrowserManager::normalizeUrl(const std::string& raw) {
    // Trim leading/trailing whitespace (VK can leave some).
    size_t b = raw.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = raw.find_last_not_of(" \t\r\n");
    std::string url = raw.substr(b, e - b + 1);
    // Common user habit: m.vnexpress.net was retired by VnExpress and fails DNS resolution
    size_t mvn = url.find("m.vnexpress.net");
    if (mvn != std::string::npos) {
        url.replace(mvn, 15, "vnexpress.net");
    }
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
    m_history.clear();
    // P24: reset the cursor alongside the vector so a fresh session starts
    // at position 0 of an empty history. If we forgot this, opening a URL
    // after close() would compute m_historyPos = size-1 = (size_t)-1 (an
    // underflow) on the very first push, leaving canGoBack() true for a
    // single-entry history.
    m_historyPos = 0;
    NetSurfWorkerBridge::instance().stop();
    NetSurfEngine::instance().clear();
}

void BrowserManager::goBack() {
    if (m_settings.fullEngine) {
        // P24: cursor-based back. We don't pop the current entry; we just
        // move the cursor left and re-load the now-current entry. The
        // forward stack (m_historyPos+1 .. m_history.size()-1) is preserved
        // so goForward() can return to where the user came from.
        if (canGoBack()) {
            m_historyPos--;
            std::string prev = m_history[m_historyPos];
            Logger::info("BrowserManager: Going back to " + prev);
            openUrlNoHistory(prev);
        }
    } else {
        HtmlRenderer::instance().goBack();
        m_state = BrowserState::LOADING;
    }
}

bool BrowserManager::canGoBack() const {
    if (m_settings.fullEngine) return m_historyPos > 0 && !m_history.empty();
    return HtmlRenderer::instance().canGoBack();
}

void BrowserManager::goForward() {
    if (m_settings.fullEngine) {
        // P24: cursor-based forward. Mirror of goBack() — only moves the
        // cursor; doesn't mutate the vector. The forward stack was created
        // by an earlier goBack() and is still intact.
        if (canGoForward()) {
            m_historyPos++;
            std::string next = m_history[m_historyPos];
            Logger::info("BrowserManager: Going forward to " + next);
            openUrlNoHistory(next);
        }
    } else {
        // LITE engine has its own history; delegate.
        HtmlRenderer::instance().goForward();
        m_state = BrowserState::LOADING;
    }
}

bool BrowserManager::canGoForward() const {
    if (m_settings.fullEngine) {
        return !m_history.empty() && m_historyPos + 1 < m_history.size();
    }
    return HtmlRenderer::instance().canGoForward();
}

void BrowserManager::openUrlNoHistory(const std::string& url) {
    // P24: load a URL without touching m_history / m_historyPos. Used by
    // goBack() / goForward() / refresh() so navigating history doesn't
    // push a duplicate entry. Mirrors HtmlRenderer::loadUrlNoHistory.
    // We still run the M6 whitelist + the in-process vs daemon split, but
    // skip the push-back-to-history step.
    std::string fixed = normalizeUrl(url);
    if (fixed.substr(0, 7) != "http://" && fixed.substr(0, 8) != "https://") {
        Logger::error("BrowserManager: refused non-http(s) URL: " + fixed);
        m_errorMsg = "Only http:// and https:// URLs are supported";
        m_state = BrowserState::ERROR;
        m_currentUrl.clear();
        return;
    }
    m_currentUrl = fixed;
    m_errorMsg.clear();
    m_state = BrowserState::LOADING;
    // P24: test-only fast path. When the test has disabled fetches, we
    // just acknowledge the URL (cursor + currentUrl already updated above)
    // and flip to RENDERING so the caller can observe a stable state.
    // No network, no daemon fork, no engine load. This keeps the P24
    // history-cursor tests fully offline.
    if (m_testFetchDisabled) {
        m_state = BrowserState::RENDERING;
        return;
    }
    if (m_settings.fullEngine) {
        if (m_settings.inProcessFull && !m_inProcessFullFailed) {
            Logger::info("BrowserManager: FULL engine (in-process) for " + fixed);
            NetSurfEngine::instance().clear();
            fetchAndLoadFull(fixed);
        } else {
            Logger::info("BrowserManager: FULL engine (daemon) for " + fixed);
            NetSurfWorkerBridge::instance().start(fixed);
            m_state = BrowserState::RENDERING;
        }
    } else {
        HtmlRenderer::instance().loadUrl(fixed);
    }
}

void BrowserManager::refresh() {
    if (!m_currentUrl.empty()) {
        // P24: openUrl() (with cursor logic) won't push a duplicate entry
        // because m_history.back() already equals m_currentUrl (we updated
        // it in openUrl/openUrlNoHistory). The truncate-forward branch is
        // also a no-op since we're sitting on the tail. So we can just
        // route through openUrl() and avoid duplicating an entry.
        openUrl(m_currentUrl);
    }
}

void BrowserManager::stop() {
    // P20: stop BOTH engines (in-process + daemon) — whichever was active
    // shuts down cleanly, the other one is a no-op.
    NetSurfWorkerBridge::instance().stop();
    HtmlRenderer::instance().stop();
    NetSurfEngine::instance().clear();
    m_state = BrowserState::IDLE;
}

} // namespace RomCloud
