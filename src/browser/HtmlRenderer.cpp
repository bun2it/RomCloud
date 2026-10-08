#include "HtmlRenderer.h"
#include "JsEngine.h"
#include "SvgRaster.h"
#include "netsurf/NetSurfEngine.h"
#include "../logging/Logger.h"
#include "../network/HttpClient.h"
#include "BrowserManager.h"
#include <cstring>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <functional>
#include <algorithm>
#include <unistd.h>  // access()
#include <pthread.h>
#include <unordered_map>
#include <SDL2/SDL_image.h>
// nsgif is C99 — wrap for C++ callers.
extern "C" {
#include "nsgif/nsgif.h"
}

// Button definitions (matching InputManager::Button)
#define BTN_UP 0
#define BTN_DOWN 1
#define BTN_A 4
#define BTN_B 5

namespace RomCloud {

// P3: bare-attribute test with token boundaries (so value="unchecked"
// does not match "checked", value="deselected" does not match "selected").
static bool hasBareAttr(const std::string& attrs, const std::string& name) {
    size_t p = 0;
    while ((p = attrs.find(name, p)) != std::string::npos) {
        bool leftOk = (p == 0) || (attrs[p - 1] == ' ');
        size_t e = p + name.size();
        bool rightOk = (e >= attrs.size()) || (attrs[e] == ' ') || (attrs[e] == '=');
        if (leftOk && rightOk) return true;
        p += name.size();
    }
    return false;
}

// P4: case-insensitive Content-Type lookup over captured headers.
static std::string contentTypeOf(const std::unordered_map<std::string, std::string>& headers) {
    for (const auto& kv : headers) {
        std::string k = kv.first;
        std::transform(k.begin(), k.end(), k.begin(), ::tolower);
        if (k == "content-type") return kv.second;
    }
    return std::string();
}

// Phase 3 — audit M5: free function so the recursion is well-formed (a
// lambda that calls itself can't be declared `auto` and still recurse).
// Walks `tree` (and its children) gathering <input> values whose
// formAction matches `action`.
static void collectFormInputs(
    const std::list<HtmlElement>& tree,
    const std::string& action,
    std::unordered_map<std::string, std::string>& formData) {
    for (const auto& el : tree) {
        if (el.formAction != action) continue;
        if (el.type == HtmlElementType::INPUT_CHECKBOX) {
            if (el.checked && !el.name.empty()) {
                formData[el.name] = el.value.empty() ? std::string("on") : el.value;
            }
        } else if (el.type == HtmlElementType::INPUT_TEXT ||
                   el.type == HtmlElementType::INPUT_PASSWORD ||
                   el.type == HtmlElementType::INPUT_HIDDEN) {
            if (!el.name.empty()) {
                formData[el.name] = el.value;
            }
        } else if (el.type == HtmlElementType::SELECT) {
            if (!el.name.empty()) {
                std::string val;
                int idx = 0;
                for (const auto& ch : el.children) {
                    if (ch.tag != "option") continue;
                    if (idx == el.selected) {
                        std::string t;
                        std::function<void(const HtmlElement&)> gather =
                            [&](const HtmlElement& n) {
                                if (n.type == HtmlElementType::TEXT && !n.text.empty()) {
                                    if (!t.empty()) t += " ";
                                    t += n.text;
                                }
                                for (const auto& c : n.children) gather(c);
                            };
                        gather(ch);
                        val = ch.name.empty() ? t : ch.name;
                        break;
                    }
                    idx++;
                }
                formData[el.name] = val;
            }
        }
        collectFormInputs(el.children, action, formData);
    }
}

// P3: gather (value, display) of a SELECT's <option> children.
static void selectOptions(const HtmlElement& sel,
                          std::vector<std::pair<std::string, std::string>>& out) {
    for (const auto& ch : sel.children) {
        if (ch.tag != "option") continue;
        std::string t;
        std::function<void(const HtmlElement&)> gather = [&](const HtmlElement& n) {
            if (n.type == HtmlElementType::TEXT && !n.text.empty()) {
                if (!t.empty()) t += " ";
                t += n.text;
            }
            for (const auto& c : n.children) gather(c);
        };
        gather(ch);
        out.emplace_back(ch.name.empty() ? t : ch.name, t);
    }
}

HtmlRenderer& HtmlRenderer::instance() {
    static HtmlRenderer inst;
    return inst;
}

HtmlRenderer::~HtmlRenderer() = default;

// Browser-style request headers for page/asset fetches (Referer proves
// same-site navigation to anti-hotlink CDNs; UA/Accept live in HttpClient).
std::vector<std::string> HtmlRenderer::getBrowserHeaders(const std::string& referer) {
    std::vector<std::string> out;
    // Modern Mobile User-Agent so news sites (VnExpress, Thanh Nien) deliver
    // clean responsive 1-column mobile templates instead of heavy desktop grids.
    out.push_back("User-Agent: Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Mobile Safari/537.36");
    out.push_back("Accept: text/html,application/xhtml+xml,image/*,*/*;q=0.8");
    out.push_back("Accept-Language: vi-VN,vi;q=0.9,en-US;q=0.8,en;q=0.7");
    out.push_back("Sec-CH-UA-Mobile: ?1");
    if (!referer.empty()) out.push_back("Referer: " + referer);
    return out;
}

void HtmlRenderer::init(SDL_Renderer* renderer, TTF_Font* font,
                        const std::string& fontPath, int baseSize) {
    // Font/renderer changed => cached textures are stale.
    clearTextCache();
    clearImages();
    if (fontPath != m_fontPath) {
        for (auto& kv : m_fonts) {
            if (kv.second && kv.second != m_font) TTF_CloseFont(kv.second);
        }
        m_fonts.clear();
        m_fontPath = fontPath;
    }
    m_baseFontSize = baseSize > 0 ? baseSize : 30;
    static bool imgInitDone = false;
    if (!imgInitDone) {
        // P12: ask for everything the SDL2_image build can give (webp
        // included when compiled in; gif has no flag). Missing bits stay off.
        int got = IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP);
        // P10: log which decoders the device build actually has (tells
        // webp/gif support apart from network failures in debug.log).
        Logger::info(std::string("HtmlRenderer: IMG_Init png=") +
                     ((got & IMG_INIT_PNG) ? "yes" : "NO") + " jpg=" +
                     ((got & IMG_INIT_JPG) ? "yes" : "NO") + " webp=" +
                     ((got & IMG_INIT_WEBP) ? "yes" : "NO"));
        imgInitDone = true;
    }
    m_renderer = renderer;
    m_font = font;
    // Reset full state (Phase 0 — audit M3).
    // The previous init() left m_inputIndex, m_maxScroll, m_loading,
    // m_error, m_errorMsg and m_pendingResult carrying over from the
    // previous session, which could cause stale focus / "ghost loading"
    // overlays on re-entry.
    m_elements.clear();
    m_focusable.clear();
    m_history.clear();
    m_historyPos = 0;
    m_scrollY = 0;
    m_maxScroll = 0;
    m_inputIndex = -1;
    m_editingText = false;
    m_currentUrl.clear();
    m_loading = false;
    m_error = false;
    m_errorMsg.clear();
    {
        std::lock_guard<std::mutex> lock(m_fetchMutex);
        m_pendingResult.reset();
    }
    m_js.reset();
    m_jsEpoch++;
    m_scripts.clear();
    m_scriptSrcs.clear();
    {
        std::lock_guard<std::mutex> lock(m_jsMutex);
        m_jsQueue.clear();
        m_jsReady.clear();
        m_jsInFlight = 0;
    }
    clearCss();
    m_dirty = true;
}

bool HtmlRenderer::loadHtml(const std::string& html) {
    m_rawHtml = html;
    if (BrowserManager::instance().settings().fullEngine) {
        // P16: FULL engine owns this page — the LITE DOM is never rendered,
        // so skip its parse/layout/image/JS pipeline entirely. (The synchronous
        // parse was freezing the UI on big pages even in FULL mode.)
        // Still drop per-page LITE state so nothing stale lingers.
        NetSurfEngine::instance().loadHtml(html, m_currentUrl);
        m_js.reset();
        m_jsEpoch++;
        m_scripts.clear();
        m_scriptSrcs.clear();
        {
            std::lock_guard<std::mutex> lock(m_jsMutex);
            m_jsQueue.clear();
            m_jsReady.clear();
            m_jsInFlight = 0;
        }
        clearFind();
        clearCss();
        m_elements.clear();
        m_focusable.clear();
        m_scrollY = 0;
        m_inputIndex = -1;
        m_editingText = false;
        m_loading = false;
        markDirty();
        return true;
    }
    m_elements.clear();
    m_focusable.clear();
    // New document: drop the old JS heap (proxies would dangle) and bump
    // the epoch so in-flight timers/XHR callbacks self-discard.
    m_js.reset();
    m_jsEpoch++;
    m_scripts.clear();
    m_scriptSrcs.clear();
    {
        std::lock_guard<std::mutex> lock(m_jsMutex);
        m_jsQueue.clear();
        m_jsReady.clear();
        m_jsInFlight = 0;
    }
    clearFind();  // P15.2: hits point into the old tree — never dangle
    clearCss();  // P9b: stylesheets are per-page, never leak across loads
    parseHtml(html);
    layout();
    updateFocus();
    markDirty();
    queueImages();
    pumpCss();
    runJs();
    return true;
}

void HtmlRenderer::refreshAfterSettings() {
    if (BrowserManager::instance().settings().fullEngine) {
        // P16: FULL mode owns the page — re-layout from cached raw HTML.
        // (The LITE tree may be empty; never touch it here.)
        if (!m_rawHtml.empty()) {
            NetSurfEngine::instance().loadHtml(m_rawHtml, m_currentUrl);
        }
        return;
    }
    if (m_elements.empty() && !m_rawHtml.empty()) {
        // P16: just switched FULL -> LITE — the LITE tree was never built.
        // Rebuild it from the cached HTML (no page refetch; CSS re-queues).
        loadHtml(m_rawHtml);
        return;
    }
    applyStyles();
    layout();
    updateFocus();
    queueImages();
    runJs();
    markDirty();
}

bool HtmlRenderer::loadUrl(const std::string& url) {
    // m_historyPos is the INDEX of the current page (P15.4 fixed the old
    // off-by-one where pos=size made the first Back a silent reload).
    // Truncate any forward entries so the new page becomes the new tip.
    if (m_historyPos + 1 < m_history.size()) {
        m_history.resize(m_historyPos + 1);
    }
    m_history.push_back(url);
    m_historyPos = m_history.size() - 1;

    return loadUrlNoHistory(url);
}

bool HtmlRenderer::loadUrlNoHistory(const std::string& url) {
    // Internal: fetch a URL without modifying history. Used by goBack()
    // and refresh() so history stays consistent (audit C4).
    m_loading = true;
    m_error = false;
    m_errorMsg.clear();
    m_currentUrl = url;
    markDirty();

    // Heap-allocate the fetch payload so each worker thread has its own
    // copy of the URL (Phase 1 — audit C1: previous code used a static
    // `s_url` that got overwritten if loadUrl() was called twice in
    // quick succession, making both fetches use the second URL).
    PendingFetch* payload = new PendingFetch();
    payload->url = url;

    pthread_t tid;
    int rc = pthread_create(&tid, nullptr, [](void* arg) -> void* {
        std::unique_ptr<PendingFetch> owned(static_cast<PendingFetch*>(arg));
        HttpResponse resp = HttpClient::instance().get(
            owned->url, HtmlRenderer::getBrowserHeaders(), 15);
        owned->success = resp.success;
        owned->body = std::move(resp.body);
        owned->error = std::move(resp.error);
        owned->contentType = contentTypeOf(resp.headers);
        owned->effectiveUrl = std::move(resp.effectiveUrl);

        // Hand ownership over to the main thread. The main thread will
        // process the result in pollFetch(). This fixes the cross-thread
        // data race where the worker was directly mutating main-thread
        // state (audit C3).
        HtmlRenderer::instance().enqueueFetchResult(owned.release());
        return nullptr;
    }, payload);
    if (rc != 0) {
        // pthread_create failed — clean up and surface the error.
        delete payload;
        m_loading = false;
        m_error = true;
        m_errorMsg = "Failed to spawn fetch thread";
        Logger::error("HtmlRenderer: pthread_create failed (rc=" + std::to_string(rc) + ")");
        return false;
    }
    pthread_detach(tid);

    return true;
}

void HtmlRenderer::enqueueFetchResult(PendingFetch* result) {
    // Called from the worker thread.
    // Atomically hand ownership of `result` to the main thread.
    // Any previously-queued result is overwritten (acceptable: it was
    // for a stale URL the user already navigated away from). The mutex
    // protects m_pendingResult only; after the swap the PendingFetch is
    // owned exclusively by the main thread, which then reads its fields
    // without any synchronization.
    std::lock_guard<std::mutex> lock(m_fetchMutex);
    m_pendingResult.reset(result);
}

void HtmlRenderer::pollFetch() {
    // Called from the main thread (UIManager render loop).
    // Take ownership of the PendingFetch (if any) posted by the worker.
    std::shared_ptr<PendingFetch> result;
    {
        std::lock_guard<std::mutex> lock(m_fetchMutex);
        result = std::move(m_pendingResult);
        m_pendingResult.reset();
    }
    if (!result) {
        return;
    }

    // Discard stale results: the user may have navigated to a different
    // URL while this fetch was in flight, in which case the result no
    // longer reflects what they want to see.
    if (result->url != m_currentUrl) {
        Logger::info("HtmlRenderer: discarding stale fetch result for: " + result->url);
        return;
    }

    if (!result->success) {
        m_error = true;
        m_errorMsg = result->error.empty() ? "Khong the tai trang" : result->error;
        m_loading = false;
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
        std::string html = "<html><body>"
            "<h1>Khong the tai trang</h1>"
            "<p>URL: " + m_currentUrl + "</p>"
            "<p>Loi: " + m_errorMsg + "</p>"
            "</body></html>";
        loadHtml(html);
        // Mirror the openUrl() error path: show the error in the page area
        // and let the UIManager render the "A: retry / Y: edit" overlay on
        // top. The user explicitly asked for this UX so they can recover
        // from a typo or a flaky captive portal without leaving the screen.
        BrowserManager::instance().setError(m_errorMsg);
    } else {
        // P4: follow the final URL after redirect chains (portal logins
        // bounce through several URLs before landing).
        if (!result->effectiveUrl.empty()) {
            m_currentUrl = result->effectiveUrl;
        }
        // P4: content-type guard — never parse images/binaries as HTML.
        // Empty/absent type stays lenient (portals with quirky servers).
        std::string ct = result->contentType;
        std::transform(ct.begin(), ct.end(), ct.begin(), ::tolower);
        bool isHtml = ct.empty() || ct.find("html") != std::string::npos ||
                      ct.find("text/") == 0 || ct.find("text ") == 0 ||
                      ct.find("xml") != std::string::npos ||
                      ct.find("xhtml") != std::string::npos;
        if (!isHtml) {
            m_error = true;
            m_errorMsg = "Trang không phải HTML (" + result->contentType + ")";
            m_loading = false;
            loadHtml("<html><body><h1>Không mở được</h1><p>URL trả về " +
                     result->contentType + ", không phải trang web.</p></body></html>");
            BrowserManager::instance().setError(m_errorMsg);
            return;
        }
        m_loading = false;
        loadHtml(result->body);
        if (BrowserManager::instance().settings().fullEngine) {
            // P16: FULL engine lays out on a worker thread — NetSurfEngine::poll()
            // flips LOADING -> RENDERING when the tree lands, so the spinner
            // stays visible instead of flashing a blank page.
        } else {
            BrowserManager::instance().setState(BrowserState::RENDERING);
        }
    }
}

void HtmlRenderer::injectFetchForTest(const std::string& url, bool success,
                                     const std::string& body,
                                     const std::string& contentType,
                                     const std::string& effectiveUrl) {
    // Simulate an in-flight fetch so the stale-result guard passes.
    m_currentUrl = url;
    m_loading = true;
    m_error = false;
    PendingFetch* p = new PendingFetch();
    p->url = url;
    p->success = success;
    p->body = body;
    p->contentType = contentType;
    p->effectiveUrl = effectiveUrl;
    enqueueFetchResult(p);
}

void HtmlRenderer::goBack() {
    if (canGoBack()) {
        m_historyPos--;
        std::string url = m_history[m_historyPos];
        // Use loadUrlNoHistory so back-navigation does not push the
        // historical URL back onto the history stack (Phase 1 — audit C4).
        loadUrlNoHistory(url);
    }
}

void HtmlRenderer::goForward() {
    if (canGoForward()) {
        m_historyPos++;
        loadUrlNoHistory(m_history[m_historyPos]);
    }
}

bool HtmlRenderer::canGoForward() const {
    return m_historyPos + 1 < m_history.size();
}

bool HtmlRenderer::canGoBack() const {
    return m_historyPos > 0;
}

std::string HtmlRenderer::extractTag(const std::string& html, size_t& pos, std::string& tagName, std::string& attributes) {
    tagName.clear();
    attributes.clear();
    if (pos >= html.size() || html[pos] != '<') return "";
    size_t start = pos;
    pos++;
    // NOTE: isspace() is called with (unsigned char) cast to avoid undefined
    // behaviour on bytes >= 0x80 (UTF-8 multi-byte sequences). Audit M4.
    while (pos < html.size() && std::isspace(static_cast<unsigned char>(html[pos]))) pos++;
    // P2: closing tags `</h3>` start with '/'. The name loop below treats '/'
    // as a delimiter, so consume the slash here and re-add it — otherwise
    // every close tag parses as "" and the stack NEVER pops (whole page
    // nests into the first <a>, blank screen on vnexpress).
    bool closing = false;
    if (pos < html.size() && html[pos] == '/') {
        closing = true;
        pos++;
    }
    size_t nameStart = pos;
    while (pos < html.size() && !std::isspace(static_cast<unsigned char>(html[pos])) && html[pos] != '>' && html[pos] != '/') pos++;
    tagName = html.substr(nameStart, pos - nameStart);
    std::transform(tagName.begin(), tagName.end(), tagName.begin(), ::tolower);
    if (closing) tagName = "/" + tagName;
    while (pos < html.size() && html[pos] != '>') {
        while (pos < html.size() && std::isspace(static_cast<unsigned char>(html[pos]))) pos++;
        if (pos >= html.size() || html[pos] == '>') break;
        size_t attrStart = pos;
        while (pos < html.size() && !std::isspace(static_cast<unsigned char>(html[pos])) && html[pos] != '=' && html[pos] != '>' && html[pos] != '/') pos++;
        std::string attrName = html.substr(attrStart, pos - attrStart);
        while (pos < html.size() && std::isspace(static_cast<unsigned char>(html[pos]))) pos++;
        std::string value;
        if (pos < html.size() && html[pos] == '=') {
            pos++;
            while (pos < html.size() && std::isspace(static_cast<unsigned char>(html[pos]))) pos++;
            char quote = 0;
            if (pos < html.size() && (html[pos] == '"' || html[pos] == '\'')) {
                quote = html[pos];
                pos++;
            }
            size_t valueStart = pos;
            while (pos < html.size()) {
                if (quote && html[pos] == quote) break;
                if (!quote && (std::isspace(static_cast<unsigned char>(html[pos])) || html[pos] == '>' || html[pos] == '/')) break;
                pos++;
            }
            value = html.substr(valueStart, pos - valueStart);
            if (quote && pos < html.size()) pos++;
        }
        // P3: keep valueless attrs (selected/checked/disabled) as bare
        // names so hasBareAttr() works; valued attrs stay name="value"
        // for getAttribute().
        if (!attrName.empty() && attrName != "/") {
            if (!attributes.empty()) attributes += " ";
            if (!value.empty()) attributes += attrName + "=\"" + value + "\"";
            else attributes += attrName;
        }
        if (pos < html.size() && html[pos] == '/') {
            pos++;
            if (pos < html.size() && html[pos] == '>') {
                pos++;
                return "/";
            }
        }
    }
    if (pos < html.size() && html[pos] == '>') pos++;
    return html.substr(start, pos - start);
}

std::string HtmlRenderer::getAttribute(const std::string& attrs, const std::string& name) {
    std::string search = name + "=\"";
    size_t pos = attrs.find(search);
    if (pos == std::string::npos) {
        search = name + "='";
        pos = attrs.find(search);
    }
    if (pos == std::string::npos) return "";
    pos += search.size();
    size_t end = pos;
    while (end < attrs.size() && attrs[end] != '"' && attrs[end] != '\'') end++;
    return attrs.substr(pos, end - pos);
}

std::string HtmlRenderer::decodeHtmlEntities(const std::string& str) {
    std::string result = str;
    struct { const char* entity; const char* rep; } entities[] = {
        {"&nbsp;", " "}, {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
        {"&quot;", "\""}, {"&apos;", "'"}, {"&#39;", "'"},
        {"&hellip;", "…"}, {"&mdash;", "—"}, {"&ndash;", "–"},
        {"&ldquo;", "\u201c"}, {"&rdquo;", "\u201d"},
        {"&lsquo;", "\u2018"}, {"&rsquo;", "\u2019"},
        {"&laquo;", "«"}, {"&raquo;", "»"}, {"&middot;", "·"},
        {"&times;", "×"}, {"&copy;", "©"}, {"&reg;", "®"},
    };
    for (auto& e : entities) {
        size_t pos = 0;
        size_t elen = strlen(e.entity);
        size_t rlen = strlen(e.rep);
        while ((pos = result.find(e.entity, pos)) != std::string::npos) {
            result.replace(pos, elen, e.rep);
            pos += rlen;
        }
    }
    // P2: numeric entities &#123; and &#x1F; (vnexpress uses &#273; for đ).
    {
        std::string out;
        out.reserve(result.size());
        for (size_t i = 0; i < result.size();) {
            if (result[i] == '&' && i + 1 < result.size() && result[i + 1] == '#') {
                size_t j = i + 2;
                bool hex = false;
                if (j < result.size() && (result[j] == 'x' || result[j] == 'X')) {
                    hex = true;
                    j++;
                }
                size_t numStart = j;
                while (j < result.size() &&
                       (hex ? isxdigit(static_cast<unsigned char>(result[j]))
                            : isdigit(static_cast<unsigned char>(result[j])))) {
                    j++;
                }
                if (j > numStart && j < result.size() && result[j] == ';') {
                    std::string num = result.substr(numStart, j - numStart);
                    unsigned code = 0;
                    try {
                        code = (unsigned)std::stoul(num, nullptr, hex ? 16 : 10);
                    } catch (...) { code = 0; }
                    if (code > 0 && code < 0x110000) {
                        appendUtf8(out, code);
                        i = j + 1;
                        continue;
                    }
                }
            }
            out.push_back(result[i]);
            i++;
        }
        result.swap(out);
    }
    return result;
}

void HtmlRenderer::appendUtf8(std::string& out, unsigned code) {
    if (code < 0x80) {
        out.push_back((char)code);
    } else if (code < 0x800) {
        out.push_back((char)(0xC0 | (code >> 6)));
        out.push_back((char)(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back((char)(0xE0 | (code >> 12)));
        out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (code & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (code >> 18)));
        out.push_back((char)(0x80 | ((code >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (code & 0x3F)));
    }
}

// Split text into words (whitespace), then greedy-fill lines by pixel width.
// A single over-long word (URL) is hard-broken UTF-8-safe.
std::vector<std::string> HtmlRenderer::wrapText(const std::string& text, int maxWidth, int px) {
    std::vector<std::string> lines;
    if (text.empty() || maxWidth <= 0) return lines;
    // Tokenize on ASCII whitespace runs.
    std::vector<std::string> words;
    {
        size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) i++;
            if (i >= text.size()) break;
            size_t j = i;
            while (j < text.size() && !std::isspace(static_cast<unsigned char>(text[j]))) j++;
            words.push_back(text.substr(i, j - i));
            i = j;
        }
    }
    auto hardBreak = [&](const std::string& w) {
        // Break an over-long word into fitting chunks (UTF-8 char safe).
        size_t i = 0;
        std::string cur;
        while (i < w.size()) {
            unsigned char c = static_cast<unsigned char>(w[i]);
            size_t n = 1;
            if ((c & 0x80) == 0) n = 1;
            else if ((c & 0xE0) == 0xC0) n = 2;
            else if ((c & 0xF0) == 0xE0) n = 3;
            else if ((c & 0xF8) == 0xF0) n = 4;
            if (i + n > w.size()) n = w.size() - i;
            std::string trial = cur + w.substr(i, n);
            if (textWidth(trial, px) <= maxWidth || cur.empty()) {
                cur = trial;
                i += n;
            } else {
                lines.push_back(cur);
                cur.clear();
            }
        }
        if (!cur.empty()) lines.push_back(cur);
    };
    std::string cur;
    for (const auto& w : words) {
        if (textWidth(w, px) > maxWidth) {
            if (!cur.empty()) { lines.push_back(cur); cur.clear(); }
            hardBreak(w);
            continue;
        }
        std::string trial = cur.empty() ? w : cur + " " + w;
        if (textWidth(trial, px) <= maxWidth) {
            cur = trial;
        } else {
            if (!cur.empty()) lines.push_back(cur);
            cur = w;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

static std::string cssTrim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

static std::vector<std::pair<std::string, std::string>> parseDeclBlock(const std::string& block) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    while (i < block.size()) {
        size_t semi = block.find(';', i);
        std::string one = cssTrim(block.substr(i, semi == std::string::npos
                                                      ? std::string::npos
                                                      : semi - i));
        if (semi == std::string::npos) i = block.size();
        else i = semi + 1;
        if (one.empty()) continue;
        size_t colon = one.find(':');
        if (colon == std::string::npos) continue;
        std::string prop = cssTrim(one.substr(0, colon));
        std::string val = cssTrim(one.substr(colon + 1));
        std::transform(prop.begin(), prop.end(), prop.begin(), ::tolower);
        if (!prop.empty() && !val.empty()) out.emplace_back(prop, val);
    }
    return out;
}

void HtmlRenderer::parseViewportMeta(const std::string& content) {
    m_viewport.hasMeta = true;
    size_t i = 0;
    while (i < content.size()) {
        size_t next = content.find_first_of(",;", i);
        std::string token = (next == std::string::npos) ? content.substr(i) : content.substr(i, next - i);
        i = (next == std::string::npos) ? content.size() : next + 1;

        while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.erase(token.begin());
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.pop_back();
        if (token.empty()) continue;

        size_t eq = token.find('=');
        if (eq == std::string::npos) continue;
        std::string key = token.substr(0, eq);
        std::string val = token.substr(eq + 1);

        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        std::transform(val.begin(), val.end(), val.begin(), ::tolower);

        if (key == "width") {
            if (val == "device-width") {
                m_viewport.isDeviceWidth = true;
            } else {
                int w = std::atoi(val.c_str());
                if (w > 0) m_viewport.width = w;
            }
        } else if (key == "initial-scale") {
            double s = std::atof(val.c_str());
            if (s > 0.0) m_viewport.initialScale = (float)s;
        } else if (key == "maximum-scale") {
            double s = std::atof(val.c_str());
            if (s > 0.0) m_viewport.maximumScale = (float)s;
        } else if (key == "user-scalable") {
            if (val == "no" || val == "0") m_viewport.userScalable = false;
        }
    }
}

int HtmlRenderer::effectiveViewportWidth() const {
    if (m_viewport.hasMeta) {
        if (m_viewport.isDeviceWidth) {
            return 480; // Standard Mobile CSS Viewport (triggers @media (max-width: 768px))
        }
        if (m_viewport.width > 0) {
            return std::min(m_viewport.width, 680);
        }
    }
    // Mobile responsive fallback: evaluate CSS @media at 480px mobile breakpoint
    return 480;
}

bool HtmlRenderer::evaluateMediaQuery(const std::string& query, int viewportW) {
    if (query.empty()) return true;
    std::string q = query;
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);

    // Skip print / speech media blocks
    if (q.find("print") != std::string::npos || q.find("speech") != std::string::npos) {
        return false;
    }

    auto extractPx = [](const std::string& str, const std::string& key) -> int {
        size_t pos = str.find(key);
        if (pos == std::string::npos) return -1;
        pos += key.size();
        while (pos < str.size() && (str[pos] == ' ' || str[pos] == ':' || str[pos] == '\t')) pos++;
        if (pos >= str.size()) return -1;
        char* endPtr = nullptr;
        double val = std::strtod(&str[pos], &endPtr);
        if (endPtr == &str[pos]) return -1;
        std::string unit;
        while (endPtr && *endPtr && *endPtr != ' ' && *endPtr != ')' && *endPtr != ';') {
            unit.push_back(::tolower(*endPtr));
            endPtr++;
        }
        if (unit == "rem" || unit == "em") {
            return (int)(val * 16.0);
        }
        return (int)val;
    };

    int minW = extractPx(q, "min-width");
    if (minW > 0 && viewportW < minW) {
        return false;
    }

    int maxW = extractPx(q, "max-width");
    if (maxW > 0 && viewportW > maxW) {
        return false;
    }

    return true;
}

void HtmlRenderer::parseStylesheet(const std::string& css) {
    // 1. Strip /* comments */.
    std::string s;
    s.reserve(css.size());
    for (size_t i = 0; i < css.size();) {
        if (i + 1 < css.size() && css[i] == '/' && css[i + 1] == '*') {
            size_t e = css.find("*/", i + 2);
            if (e == std::string::npos) break;
            i = e + 2;
        } else {
            s.push_back(css[i]);
            i++;
        }
    }
    // 2. Inline @media blocks whose query matches the effective viewport.
    int vpW = effectiveViewportWidth();
    std::string flat;
    flat.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s.compare(i, 6, "@media") == 0) {
            size_t brace = s.find('{', i);
            if (brace == std::string::npos) break;
            std::string condition = cssTrim(s.substr(i + 6, brace - i - 6));
            int depth = 0;
            size_t j = brace;
            for (; j < s.size(); j++) {
                if (s[j] == '{') depth++;
                else if (s[j] == '}') {
                    depth--;
                    if (depth == 0) break;
                }
            }
            if (j < s.size()) {
                if (evaluateMediaQuery(condition, vpW)) {
                    flat.append(s.substr(brace + 1, j - brace - 1));
                    flat.push_back(' ');
                }
                i = j + 1;
                continue;
            }
            break;
        }
        flat.push_back(s[i]);
        i++;
    }
    // 3. Split top-level rules on '}'.
    size_t i = 0;
    while (i < flat.size()) {
        size_t close = flat.find('}', i);
        std::string chunk = cssTrim(flat.substr(i, close == std::string::npos
                                                       ? std::string::npos
                                                       : close - i));
        if (close == std::string::npos) i = flat.size();
        else i = close + 1;
        if (chunk.empty()) continue;
        size_t open = chunk.find('{');
        if (open == std::string::npos) continue;
        std::string selText = cssTrim(chunk.substr(0, open));
        if (selText.empty() || selText[0] == '@') continue;  // @font-face etc.
        auto decls = parseDeclBlock(chunk.substr(open + 1));
        if (decls.empty()) continue;
        CssRule rule;
        rule.order = m_cssOrder++;
        rule.decls = std::move(decls);
        // Split selector list on ','.
        size_t p = 0;
        bool ruleOk = false;
        while (p <= selText.size()) {
            size_t comma = selText.find(',', p);
            std::string one = cssTrim(selText.substr(p, comma == std::string::npos
                                                              ? std::string::npos
                                                              : comma - p));
            if (comma == std::string::npos) p = selText.size() + 1;
            else p = comma + 1;
            if (one.empty()) continue;
            // Split descendant chain on whitespace.
            CssSelector sel;
            size_t q = 0;
            bool bad = false;
            while (q < one.size()) {
                while (q < one.size() &&
                       std::isspace(static_cast<unsigned char>(one[q]))) q++;
                if (q >= one.size()) break;
                size_t r = q;
                while (r < one.size() &&
                       !std::isspace(static_cast<unsigned char>(one[r]))) r++;
                std::string tok = one.substr(q, r - q);
                q = r;
                if (tok.find(':') != std::string::npos) {
                    bad = true;  // pseudo-class — not supported, drop selector
                    break;
                }
                CssCompound c;
                size_t k = 0;
                if (!tok.empty() && tok[0] != '#' && tok[0] != '.') {
                    size_t e = k;
                    while (e < tok.size() && tok[e] != '#' && tok[e] != '.' &&
                           tok[e] != '[') e++;
                    c.tag = tok.substr(k, e - k);
                    std::transform(c.tag.begin(), c.tag.end(), c.tag.begin(), ::tolower);
                    if (c.tag == "*") c.tag.clear();
                    k = e;
                }
                while (k < tok.size()) {
                    if (tok[k] == '#') {
                        size_t e = k + 1;
                        while (e < tok.size() && tok[e] != '.' && tok[e] != '[') e++;
                        c.id = tok.substr(k + 1, e - k - 1);
                        sel.specificity += 10000;
                        k = e;
                    } else if (tok[k] == '.') {
                        size_t e = k + 1;
                        while (e < tok.size() && tok[e] != '#' && tok[e] != '.' &&
                               tok[e] != '[') e++;
                        c.classes.push_back(tok.substr(k + 1, e - k - 1));
                        sel.specificity += 100;
                        k = e;
                    } else {
                        break;  // [attr] — stop, keep what we parsed
                    }
                }
                if (!c.tag.empty()) sel.specificity += 1;
                sel.parts.push_back(std::move(c));
            }
            if (!bad && !sel.parts.empty()) {
                rule.selectors.push_back(std::move(sel));
                ruleOk = true;
            }
        }
        if (ruleOk) m_cssRules.push_back(std::move(rule));
    }
}

static bool matchCompound(const CssCompound& c, const HtmlElement& el) {
    if (!c.tag.empty() && c.tag != el.tag) return false;
    if (!c.id.empty() && c.id != el.elemId) return false;
    if (!c.classes.empty()) {
        // Tokenize element class list on whitespace.
        for (const auto& want : c.classes) {
            bool found = false;
            size_t i = 0;
            while (i < el.cls.size()) {
                while (i < el.cls.size() &&
                       std::isspace(static_cast<unsigned char>(el.cls[i]))) i++;
                if (i >= el.cls.size()) break;
                size_t j = i;
                while (j < el.cls.size() &&
                       !std::isspace(static_cast<unsigned char>(el.cls[j]))) j++;
                if (el.cls.compare(i, j - i, want) == 0) {
                    found = true;
                    break;
                }
                i = j;
            }
            if (!found) return false;
        }
    }
    return true;
}

bool HtmlRenderer::matchSelector(const CssSelector& sel, const HtmlElement& el,
                                 const std::vector<const HtmlElement*>& ancestors) const {
    if (sel.parts.empty()) return false;
    if (!matchCompound(sel.parts.back(), el)) return false;
    int pi = (int)sel.parts.size() - 2;
    for (auto it = ancestors.rbegin(); it != ancestors.rend() && pi >= 0; ++it) {
        if (matchCompound(sel.parts[(size_t)pi], **it)) pi--;
    }
    return pi < 0;
}

int HtmlRenderer::parsePx(const std::string& v) {
    // Leading integer only, px/pt/unitless. Relative units (em/rem/%)
    // and bare floats (line-height ratios) are NOT pixels — ignore.
    size_t i = 0;
    while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) i++;
    bool neg = false;
    if (i < v.size() && (v[i] == '-' || v[i] == '+')) {
        neg = (v[i] == '-');
        i++;
    }
    int n = 0;
    bool any = false;
    while (i < v.size() && v[i] >= '0' && v[i] <= '9') {
        n = n * 10 + (v[i] - '0');
        any = true;
        i++;
    }
    if (!any) return 0;
    // Unit check: allow end/space/px/pt — reject em/rem/%/vw/….
    while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) i++;
    if (i < v.size() && v[i] != 'p') {
        // 'p' starts px/pt; anything else (e, r, %, v, .) is relative.
        return 0;
    }
    if (i < v.size()) {
        if (v.compare(i, 2, "px") != 0 && v.compare(i, 2, "pt") != 0) return 0;
    }
    return neg ? -n : n;
}

bool HtmlRenderer::parseColor(const std::string& v, SDL_Color& out) {
    std::string s = cssTrim(v);
    std::string low = s;
    std::transform(low.begin(), low.end(), low.begin(), ::tolower);
    if (low == "transparent") return false;
    static const std::unordered_map<std::string, unsigned> named = {
        {"white", 0xFFFFFF}, {"black", 0x000000}, {"red", 0xFF0000},
        {"green", 0x008000}, {"blue", 0x0000FF}, {"yellow", 0xFFFF00},
        {"orange", 0xFFA500}, {"gray", 0x808080}, {"grey", 0x808080},
        {"silver", 0xC0C0C0}, {"cyan", 0x00FFFF}, {"magenta", 0xFF00FF},
        {"lime", 0x00FF00}, {"maroon", 0x800000}, {"navy", 0x000080},
        {"olive", 0x808000}, {"purple", 0x800080}, {"teal", 0x008080},
    };
    auto nit = named.find(low);
    if (nit != named.end()) {
        unsigned c = nit->second;
        out = {(uint8_t)(c >> 16), (uint8_t)((c >> 8) & 0xFF),
               (uint8_t)(c & 0xFF), 255};
        return true;
    }
    if (!s.empty() && s[0] == '#') {
        std::string h = s.substr(1);
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        unsigned r = 0, g = 0, b = 0;
        if (h.size() == 3) {
            int v0 = hex(h[0]), v1 = hex(h[1]), v2 = hex(h[2]);
            if (v0 < 0 || v1 < 0 || v2 < 0) return false;
            r = (unsigned)(v0 * 17);
            g = (unsigned)(v1 * 17);
            b = (unsigned)(v2 * 17);
        } else if (h.size() >= 6) {
            int v[6];
            for (int k = 0; k < 6; k++) {
                v[k] = hex(h[(size_t)k]);
                if (v[k] < 0) return false;
            }
            r = (unsigned)(v[0] * 16 + v[1]);
            g = (unsigned)(v[2] * 16 + v[3]);
            b = (unsigned)(v[4] * 16 + v[5]);
        } else {
            return false;
        }
        out = {(uint8_t)r, (uint8_t)g, (uint8_t)b, 255};
        return true;
    }
    if (low.compare(0, 4, "rgb(") == 0 || low.compare(0, 5, "rgba(") == 0) {
        size_t o = low.find('(');
        size_t c = low.find(')', o);
        if (o == std::string::npos || c == std::string::npos) return false;
        std::string inner = low.substr(o + 1, c - o - 1);
        for (char& ch : inner) {
            if (ch == ',' || ch == '/') ch = ' ';
        }
        std::stringstream ss(inner);
        int r = -1, g = -1, b = -1;
        double a = 1.0;
        ss >> r >> g >> b >> a;
        if (r < 0 || g < 0 || b < 0) return false;
        if (a <= 0.0) return false;  // fully transparent — ignore
        auto clamp = [](int x) -> uint8_t {
            if (x < 0) return 0;
            if (x > 255) return 255;
            return (uint8_t)x;
        };
        out = {clamp(r), clamp(g), clamp(b), 255};
        return true;
    }
    return false;
}

void HtmlRenderer::parseHtml(const std::string& html) {
    size_t pos = 0;
    std::vector<HtmlElement*> stack;
    // Phase 3 — audit M5: separate stack of currently-open <form> ancestors
    // so any nested element (input, button, div, ...) gets its formAction /
    // formMethod denormalized at parse time. Cheaper than chasing parent
    // pointers at submit time.
    std::vector<HtmlElement*> formStack;
    HtmlElement* current = nullptr;
    std::string tagName, attrs;
    int tabIndex = 0;

    auto applyFormContext = [&](HtmlElement& el) {
        if (!formStack.empty()) {
            el.formAction = formStack.back()->href;
            el.formMethod = formStack.back()->name.empty() ? "GET" : formStack.back()->name;
            // Defer to tolower so "POST" / "Post" / "post" all work.
            std::transform(el.formMethod.begin(), el.formMethod.end(),
                           el.formMethod.begin(), ::tolower);
        }
    };

    // P2: skip UTF-8 BOM if present.
    if (html.size() >= 3 && static_cast<unsigned char>(html[0]) == 0xEF &&
        static_cast<unsigned char>(html[1]) == 0xBB &&
        static_cast<unsigned char>(html[2]) == 0xBF) {
        pos = 3;
    }
    // P9b: fresh page theme capture.
    m_bodyInline.clear();
    m_bodyCls.clear();
    // Viewport meta is per-page.
    m_viewport = ViewportConfig{};

    // P2: text style stack (parallel to element stack) + <li> depth, so
    // headlines/bold survive nesting (vnexpress: <a><h3>title</h3></a>).
    struct TextStyle { int lineH = LINE_HEIGHT; bool bold = false; };
    std::vector<TextStyle> styleStack;
    int listDepth = 0;
    auto curStyle = [&]() -> TextStyle {
        return styleStack.empty() ? TextStyle{} : styleStack.back();
    };
    auto styleFor = [&](const std::string& tag) -> TextStyle {
        TextStyle s = curStyle();
        if (tag == "h1") { s.lineH = 58; s.bold = true; }
        else if (tag == "h2") { s.lineH = 52; s.bold = true; }
        else if (tag == "h3") { s.lineH = 46; s.bold = true; }
        else if (tag == "h4" || tag == "h5" || tag == "h6") { s.lineH = 40; s.bold = true; }
        else if (tag == "b" || tag == "strong") { s.bold = true; }
        return s;
    };
    // Pop element stack + keep style/list/form stacks in sync.
    auto popOne = [&]() {
        if (stack.empty()) return;
        HtmlElement* top = stack.back();
        stack.pop_back();
        if (!styleStack.empty()) styleStack.pop_back();
        if (top->tag == "li" && listDepth > 0) listDepth--;
        if (top->type == HtmlElementType::FORM && !formStack.empty() &&
            formStack.back() == top) {
            formStack.pop_back();
        }
    };

    while (pos < html.size()) {
        // Phase 2 — audit M8b: skip HTML comments `<!-- ... -->`.
        if (html.compare(pos, 4, "<!--") == 0) {
            size_t end = html.find("-->", pos + 4);
            if (end == std::string::npos) pos = html.size();
            else pos = end + 3;
            continue;
        }
        // P1b: skip DOCTYPE / declarations `<!...>`, `<?...?>`.
        if (html.compare(pos, 2, "<!") == 0) {
            std::string dummy1, dummy2;
            extractTag(html, pos, dummy1, dummy2);
            continue;
        }
        if (html.compare(pos, 2, "<?") == 0) {
            size_t end = html.find("?>", pos + 2);
            if (end == std::string::npos) pos = html.size();
            else pos = end + 2;
            continue;
        }
        if (html[pos] == '<') {
            std::string rawTag = extractTag(html, pos, tagName, attrs);
            if (tagName.empty()) continue;

            // Phase 2 — audit M8a: <script> blocks are skipped by layout —
            // otherwise the parser would tokenize JS as text and pollute
            // the DOM. Bodies are CAPTURED for the JS engine (setting js);
            // <svg> is skipped (P5 covers raster <img> only).
            if (tagName == "script" || tagName == "svg") {
                bool isScript = (tagName == "script");
                std::string endTag = "</" + tagName + ">";
                size_t end = html.find(endTag, pos);
                std::string body;
                if (end == std::string::npos) {
                    body = html.substr(pos);
                    pos = html.size();
                } else {
                    body = html.substr(pos, end - pos);
                    pos = end + endTag.size();
                }
                if (isScript) {
                    std::string src = getAttribute(attrs, "src");
                    if (!src.empty()) {
                        if (m_scriptSrcs.size() < kMaxJsFiles) {
                            m_scriptSrcs.push_back(src);
                        }
                    } else if (m_scripts.size() < 32 && body.size() < 128 * 1024) {
                        m_scripts.push_back(body);
                    }
                }
                continue;
            }
            if (tagName == "style") {
                size_t end = html.find("</style>", pos);
                std::string css = (end == std::string::npos)
                                      ? html.substr(pos)
                                      : html.substr(pos, end - pos);
                {
                    std::lock_guard<std::mutex> lock(m_cssMutex);
                    if (m_styleSheets.size() < kMaxCssSheets) {
                        m_styleSheets.push_back(css);
                    }
                }
                if (end == std::string::npos) pos = html.size();
                else pos = end + 8;
                continue;
            }

            // P1b: closing tags never create elements. Pop the stack until
            // the matching opener (mismatch recovery for real-world HTML
            // like vnexpress where void tags/typos unbalance the tree).
            if (!tagName.empty() && tagName[0] == '/') {
                std::string target = tagName.substr(1);
                while (!stack.empty()) {
                    bool match = (stack.back()->tag == target);
                    popOne();  // also syncs formStack/style/listDepth
                    if (match) break;
                }
                current = stack.empty() ? nullptr : stack.back();
                continue;
            }

            // Viewport meta (per-page; drives media queries + layout width).
            if (tagName == "meta") {
                std::string mname = getAttribute(attrs, "name");
                std::transform(mname.begin(), mname.end(), mname.begin(), ::tolower);
                if (mname == "viewport") {
                    parseViewportMeta(getAttribute(attrs, "content"));
                }
            }

            // P1b: void elements never push the stack. <br> keeps its break;
            // <input> keeps the existing widget handling; <img> becomes an
            // IMAGE node (rendered as placeholder until P5 loads it);
            // <link rel=stylesheet> queues async CSS fetch (P9);
            // <source> feeds its parent <video>/<audio> (P13);
            // everything else (meta/base/embed/...) is dropped.
            if (tagName == "br" || tagName == "hr" || tagName == "img" ||
                tagName == "input" || tagName == "meta" || tagName == "link" ||
                tagName == "base" || tagName == "area" || tagName == "col" ||
                tagName == "embed" || tagName == "source" || tagName == "track" ||
                tagName == "wbr" || tagName == "param" || rawTag == "/") {
                HtmlElement el;
                el.type = HtmlElementType::TEXT;
                if (tagName == "br") {
                    el.type = HtmlElementType::LINE_BREAK;
                    el.tag = "br";
                    if (current) current->children.push_back(el);
                    else m_elements.push_back(el);
                } else if (tagName == "img") {
                    el.type = HtmlElementType::IMAGE;
                    el.tag = "img";
                    // P9: selector matching for CSS image sizing.
                    el.cls = getAttribute(attrs, "class");
                    el.elemId = getAttribute(attrs, "id");
                    el.inlineCss = getAttribute(attrs, "style");
                    el.src = getAttribute(attrs, "src");
                    // P5: lazy-load pattern (vnexpress): src is a 1px
                    // data: GIF placeholder, real URL lives in data-src.
                    if (el.src.empty() || el.src.compare(0, 5, "data:") == 0) {
                        el.src = getAttribute(attrs, "data-src");
                    }
                    // P12: more lazy variants — data-original, then the
                    // first URL of srcset ("u1 220w, u2 480w").
                    if (el.src.empty() || el.src.compare(0, 5, "data:") == 0) {
                        el.src = getAttribute(attrs, "data-original");
                    }
                    if (el.src.empty() || el.src.compare(0, 5, "data:") == 0) {
                        std::string ss = getAttribute(attrs, "srcset");
                        if (!ss.empty()) {
                            size_t comma = ss.find(',');
                            std::string first = (comma == std::string::npos)
                                                    ? ss
                                                    : ss.substr(0, comma);
                            first = cssTrim(first);
                            size_t sp = first.find(' ');
                            if (sp != std::string::npos) first.resize(sp);
                            el.src = first;
                        }
                    }
                    el.text = getAttribute(attrs, "alt");
                    // P5b: reserve space from width/height or intrinsicsize
                    // ("220x132") so loaded images don't shift the layout.
                    auto parseDim = [&](const std::string& s) -> int {
                        int v = 0;
                        for (char c : s) {
                            if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
                            else if (v > 0) break;
                        }
                        return v;
                    };
                    el.imgW = parseDim(getAttribute(attrs, "width"));
                    el.imgH = parseDim(getAttribute(attrs, "height"));
                    if ((el.imgW <= 0 || el.imgH <= 0)) {
                        std::string is = getAttribute(attrs, "intrinsicsize");
                        size_t xpos = is.find('x');
                        if (xpos != std::string::npos) {
                            int w = parseDim(is.substr(0, xpos));
                            int h = parseDim(is.substr(xpos + 1));
                            if (w > 0 && h > 0) {
                                el.imgW = w;
                                el.imgH = h;
                            }
                        }
                    }
                    if (current) current->children.push_back(el);
                    else m_elements.push_back(el);
                } else if (tagName == "link") {
                    // P9: queue external stylesheets for async fetch.
                    // P9e: external sheets use their own budget (m_extCssCount),
                    // never the shared m_styleSheets cap — inline <style> blocks
                    // fill that vector during parse and must not starve <link>.
                    std::string rel = getAttribute(attrs, "rel");
                    std::transform(rel.begin(), rel.end(), rel.begin(), ::tolower);
                    if (rel.find("stylesheet") != std::string::npos) {
                        std::string href = getAttribute(attrs, "href");
                        if (!href.empty()) {
                            std::lock_guard<std::mutex> lock(m_cssMutex);
                            if (m_extCssCount < (int)kMaxCssSheets) {
                                m_cssQueue.push_back(href);
                                m_extCssCount++;
                            }
                        }
                    }
                } else if (tagName == "source") {
                    // P13: first <source> wins for a parent <video>/<audio>.
                    if (current && current->type == HtmlElementType::MEDIA &&
                        current->href.empty()) {
                        current->href = getAttribute(attrs, "src");
                    }
                } else if (tagName == "input") {
                    std::string type = getAttribute(attrs, "type");
                    if (type.empty()) type = "text";
                    std::transform(type.begin(), type.end(), type.begin(), ::tolower);
                    // P3: full type mapping. submit/button/reset/image render
                    // as buttons; hidden keeps data but no widget; radio
                    // degrades to checkbox (no group exclusivity).
                    if (type == "password") el.type = HtmlElementType::INPUT_PASSWORD;
                    else if (type == "checkbox" || type == "radio") el.type = HtmlElementType::INPUT_CHECKBOX;
                    else if (type == "submit" || type == "button" || type == "reset" ||
                             type == "image") {
                        el.type = HtmlElementType::BUTTON;
                        el.name = type;
                        el.text = getAttribute(attrs, "value");
                        if (el.text.empty()) el.text = "Submit";
                        el.tabIndex = tabIndex++;
                    } else if (type == "hidden") {
                        el.type = HtmlElementType::INPUT_HIDDEN;
                    } else {
                        el.type = HtmlElementType::INPUT_TEXT;
                    }
                    el.name = (el.type == HtmlElementType::BUTTON) ? el.name
                                                                  : getAttribute(attrs, "name");
                    if (el.type == HtmlElementType::INPUT_TEXT ||
                        el.type == HtmlElementType::INPUT_PASSWORD) {
                        el.value = getAttribute(attrs, "value");
                    } else if (el.type == HtmlElementType::INPUT_HIDDEN) {
                        el.name = getAttribute(attrs, "name");
                        el.value = getAttribute(attrs, "value");
                    } else if (el.type == HtmlElementType::INPUT_CHECKBOX) {
                        el.name = getAttribute(attrs, "name");
                        el.value = getAttribute(attrs, "value");
                        // P3: pre-checked box (<input checked>).
                        if (hasBareAttr(attrs, "checked")) el.checked = true;
                    }
                    // P3: maxlength (0 = default cap in setFocusedInputValue).
                    {
                        std::string ml = getAttribute(attrs, "maxlength");
                        if (!ml.empty()) {
                            try { el.maxLen = std::stoi(ml); } catch (...) { el.maxLen = 0; }
                            if (el.maxLen < 0) el.maxLen = 0;
                        }
                    }
                    el.placeholder = getAttribute(attrs, "placeholder");
                    // P9: selector matching inputs for form widgets.
                    el.cls = getAttribute(attrs, "class");
                    el.elemId = getAttribute(attrs, "id");
                    el.inlineCss = getAttribute(attrs, "style");
                    if (el.type != HtmlElementType::BUTTON) el.tabIndex = tabIndex++;
                    // Capture a stable pointer to the element we just pushed.
                    // Phase 1 — audit C2: with std::list, this pointer stays
                    // valid across subsequent pushes. The old code unconditionally
                    // took &m_elements.back(), which was wrong when the element
                    // was pushed into a nested parent's `children` container.
                    // NOTE: m_focusable is cleared and rebuilt by layout() anyway,
                    // so this initial push is overwritten — but the pointer is
                    // now correct for any defensive consumer.
                    HtmlElement* added = nullptr;
                    if (current) {
                        current->children.push_back(el);
                        added = &current->children.back();
                    } else {
                        m_elements.push_back(el);
                        added = &m_elements.back();
                    }
                    applyFormContext(*added);
                    // P3: hidden inputs carry form data but are never focusable.
                    if (el.type != HtmlElementType::INPUT_HIDDEN) {
                        m_focusable.push_back(added);
                    }
                }
                continue;
            }

            // Structural tags (html/head/body/meta/title/link/script/style)
            // carry no visible content — skip them so they don't become
            // phantom TEXT wrappers. (P1b: closing tags already returned
            // above, so only openers reach here.)
            // P9b: capture body/html style for the page theme (no tree node).
            // <html> comes first (fallback), <body> overwrites when set.
            if (tagName == "body" || tagName == "html") {
                std::string st = getAttribute(attrs, "style");
                std::string cl = getAttribute(attrs, "class");
                if (tagName == "body") {
                    if (!st.empty()) m_bodyInline = st;
                    if (!cl.empty()) m_bodyCls = cl;
                } else {
                    if (m_bodyInline.empty() && !st.empty()) m_bodyInline = st;
                    if (m_bodyCls.empty() && !cl.empty()) m_bodyCls = cl;
                }
            }
            if (tagName != "html" && tagName != "head" && tagName != "body" &&
                tagName != "script" && tagName != "style" && tagName != "meta" &&
                tagName != "title" && tagName != "link" && tagName != "base") {

                // P1b: auto-close. Real pages (vnexpress: 189 <a> vs 183 </a>)
                // leave inline tags unclosed and nest <a> inside <a> (through
                // spans/divs). Browsers implicitly close the outer one; we pop
                // until the matching ancestor, like a close tag would.
                // Without this, one unclosed <a> swallows the rest of the page
                // and the screen stays blank.
                if (tagName == "a" || tagName == "p" || tagName == "button" ||
                    tagName == "li" || tagName == "form" || tagName == "option" ||
                    tagName == "select" || tagName == "tr" || tagName == "td" ||
                    tagName == "th" ||
                    (tagName.size() == 2 && tagName[0] == 'h' &&
                     tagName[1] >= '1' && tagName[1] <= '6')) {
                    bool found = false;
                    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                        if ((*it)->tag == tagName) { found = true; break; }
                    }
                    if (found) {
                        while (!stack.empty()) {
                            bool match = (stack.back()->tag == tagName);
                            popOne();
                            if (match) break;
                        }
                        current = stack.empty() ? nullptr : stack.back();
                    }
                }

                HtmlElement el;
                el.tag = tagName;
                TextStyle ns = styleFor(tagName);
                el.lineH = ns.lineH;
                el.bold = ns.bold;
                // P9: selector matching inputs.
                el.cls = getAttribute(attrs, "class");
                el.elemId = getAttribute(attrs, "id");
                el.inlineCss = getAttribute(attrs, "style");
                if (tagName == "a") {
                    el.type = HtmlElementType::LINK;
                    el.href = getAttribute(attrs, "href");
                    el.tabIndex = tabIndex++;
                } else if (tagName == "button") {
                    el.type = HtmlElementType::BUTTON;
                    // Phase 3 — audit M5: capture the button's `type`
                    // attribute ("submit", "reset", or "" for default).
                    // Used by handleInput() to decide whether A submits
                    // the form.
                    el.name = getAttribute(attrs, "type");
                    el.tabIndex = tabIndex++;
                } else if (tagName == "form") {
                    el.type = HtmlElementType::FORM;
                    el.href = getAttribute(attrs, "action");
                    el.name = getAttribute(attrs, "method");
                } else if (tagName == "select") {
                    // P3: <select name> — options are child <option> wrappers;
                    // A cycles selected, submit sends the chosen value.
                    el.type = HtmlElementType::SELECT;
                    el.name = getAttribute(attrs, "name");
                    el.tabIndex = tabIndex++;
                } else if (tagName == "table") {
                    // P13: grid container (rows laid out side-by-side).
                    el.type = HtmlElementType::TABLE;
                } else if (tagName == "tr") {
                    el.type = HtmlElementType::TABLE_ROW;
                } else if (tagName == "td" || tagName == "th") {
                    el.type = HtmlElementType::TABLE_CELL;
                    if (tagName == "th") el.bold = true;
                    // P13b: spans (clamped — broken pages use 100+).
                    try {
                        int cs = std::stoi(getAttribute(attrs, "colspan"));
                        if (cs >= 1 && cs <= 10) el.colspan = cs;
                    } catch (...) {}
                    try {
                        int rs = std::stoi(getAttribute(attrs, "rowspan"));
                        if (rs >= 1 && rs <= 10) el.rowspan = rs;
                    } catch (...) {}
                } else if (tagName == "iframe" || tagName == "video" ||
                           tagName == "audio") {
                    // P13: embedded media — bordered box, A plays.
                    el.type = HtmlElementType::MEDIA;
                    el.href = getAttribute(attrs, "src");
                    el.poster = getAttribute(attrs, "poster");
                    el.name = tagName;  // remember which tag for labeling
                    el.tabIndex = tabIndex++;
                } else if (tagName == "textarea") {
                    // P3: <textarea name> behaves like a text input; inner
                    // text is moved into value by the post-parse fixup below.
                    el.type = HtmlElementType::INPUT_TEXT;
                    el.name = getAttribute(attrs, "name");
                    {
                        std::string ml = getAttribute(attrs, "maxlength");
                        if (!ml.empty()) {
                            try { el.maxLen = std::stoi(ml); } catch (...) { el.maxLen = 0; }
                            if (el.maxLen < 0) el.maxLen = 0;
                        }
                    }
                    el.placeholder = getAttribute(attrs, "placeholder");
                    el.tabIndex = tabIndex++;
                } else if (tagName == "option") {
                    // P3: option value kept in `name`; display text is the
                    // child text node. Parent SELECT gathers these at layout.
                    el.type = HtmlElementType::TEXT;
                    el.name = getAttribute(attrs, "value");
                    if (hasBareAttr(attrs, "selected")) {
                        el.checked = true;  // marker: preselected option
                    }
                } else {
                    el.type = HtmlElementType::TEXT;
                }
                // Legacy presentational align="..." (Wikipedia/forums).
                {
                    std::string al = getAttribute(attrs, "align");
                    if (!al.empty()) {
                        std::transform(al.begin(), al.end(), al.begin(), ::tolower);
                        if (al.find("center") != std::string::npos) {
                            el.textAlign = TextAlign::CENTER;
                        } else if (al.find("right") != std::string::npos) {
                            el.textAlign = TextAlign::RIGHT;
                        } else if (al.find("left") != std::string::npos ||
                                   al.find("justify") != std::string::npos) {
                            el.textAlign = TextAlign::LEFT;
                        }
                        el.centered = (el.textAlign == TextAlign::CENTER);
                        el.hasExplicitAlign = true;
                    }
                }

                HtmlElement* added = nullptr;
                if (current) {
                    current->children.push_back(el);
                    added = &current->children.back();
                } else {
                    m_elements.push_back(el);
                    added = &m_elements.back();
                }
                applyFormContext(*added);
                stack.push_back(added);
                styleStack.push_back(ns);
                if (tagName == "li") listDepth++;
                current = added;

                // Phase 3 — audit M5: track open <form> ancestors so
                // subsequent inputs/buttons/divs inherit the form context.
                if (added->type == HtmlElementType::FORM) {
                    formStack.push_back(added);
                }
            }
        } else {
            size_t textStart = pos;
            while (pos < html.size() && html[pos] != '<') pos++;
            std::string text = html.substr(textStart, pos - textStart);
            size_t start = 0;
            while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) start++;
            size_t end = text.size();
            while (end > start && std::isspace(static_cast<unsigned char>(text[end-1]))) end--;
            text = text.substr(start, end - start);
            if (!text.empty()) {
                HtmlElement el;
                el.type = HtmlElementType::TEXT;
                el.text = decodeHtmlEntities(text);
                // P2: strip stray CR, inherit headline/bold style, bullet for <li>.
                {
                    std::string noCr;
                    noCr.reserve(el.text.size());
                    for (char c : el.text) if (c != '\r') noCr.push_back(c);
                    el.text.swap(noCr);
                }
                TextStyle ts = curStyle();
                el.lineH = ts.lineH;
                el.bold = ts.bold;
                if (listDepth > 0 && current && current->tag == "li" &&
                    current->children.empty()) {
                    el.text = "• " + el.text;
                }
                HtmlElement* added = nullptr;
                if (current) {
                    current->children.push_back(el);
                    added = &current->children.back();
                } else {
                    m_elements.push_back(el);
                    added = &m_elements.back();
                }
                applyFormContext(*added);
            }
        }
    }

    // P3: post-parse fixups over the finished tree.
    std::function<void(std::list<HtmlElement>&)> fixup =
        [&](std::list<HtmlElement>& els) {
            for (auto& el : els) {
                // <textarea>: move inner text into value so it renders and
                // edits as one field instead of doubling as static text.
                if (el.type == HtmlElementType::INPUT_TEXT && el.tag == "textarea") {
                    std::string combined;
                    for (auto& ch : el.children) {
                        if (ch.type == HtmlElementType::TEXT && !ch.text.empty()) {
                            if (!combined.empty()) combined += " ";
                            combined += ch.text;
                            ch.text.clear();
                            ch.lines.clear();
                        }
                    }
                    if (!combined.empty() && el.value.empty()) el.value = combined;
                }
                // <select>: honor preselected <option>.
                if (el.type == HtmlElementType::SELECT) {
                    int idx = 0, sel = 0;
                    for (auto& ch : el.children) {
                        if (ch.tag == "option") {
                            if (ch.checked) sel = idx;
                            idx++;
                        }
                    }
                    el.selected = sel;
                }
                fixup(el.children);
            }
        };
    fixup(m_elements);

    // P9: parse newly captured stylesheets, then compute cascade.
    {
        std::lock_guard<std::mutex> lock(m_cssMutex);
        while (m_sheetsParsed < (int)m_styleSheets.size()) {
            parseStylesheet(m_styleSheets[(size_t)m_sheetsParsed]);
            m_sheetsParsed++;
        }
    }
    applyStyles();
}

int HtmlRenderer::textWidth(const std::string& text, int px) {
    TTF_Font* f = fontFor(px);
    if (!f || text.empty()) return 0;
    int w = 0;
    TTF_SizeUTF8(f, text.c_str(), &w, nullptr);
    return w;
}

int HtmlRenderer::cssPx(int v) const {
    int pct = BrowserManager::instance().settings().fontScalePct;
    if (pct != 100 && pct != 120 && pct != 150) pct = 120;
    int s = v * pct / 100;
    if (s < 15) s = 15;
    if (s > 44) s = 44;
    return s;
}

TTF_Font* HtmlRenderer::fontFor(int px) {    // P9: sized font from the same file, cached. Falls back to base font.
    if (px <= 0 || m_fontPath.empty()) return m_font;
    if (px < 12) px = 12;
    if (px > 48) px = 48;
    auto it = m_fonts.find(px);
    if (it != m_fonts.end()) return it->second ? it->second : m_font;
    TTF_Font* f = TTF_OpenFont(m_fontPath.c_str(), px);
    m_fonts[px] = f;  // may be null — cached miss avoids re-open attempts
    return f ? f : m_font;
}

void HtmlRenderer::layout() {
    m_focusable.clear();
    // Phase 2 — audit M1: layout begins at CONTENT_Y (just below the header
    // chrome) instead of PADDING. m_maxScroll is bounded by CONTENT_H so
    // pageUp/pageDown never reveal the empty footer area.
    m_maxScroll = CONTENT_Y + PADDING;
    int y = CONTENT_Y + PADDING;

    // Fill the 1024x768 screen width cleanly with comfortable 20px side margins
    const int sideMargin = 20;
    int layoutW = m_readerActive ? 840 : (SCREEN_W - sideMargin * 2);
    int startX = m_readerActive ? ((SCREEN_W - layoutW) / 2) : sideMargin;

    for (auto& el : m_elements) {
        layoutElement(el, y, startX, layoutW);
    }
    m_maxScroll = y + PADDING - (CONTENT_Y + CONTENT_H);
    if (m_maxScroll < 0) m_maxScroll = 0;
}

void HtmlRenderer::layoutElement(HtmlElement& el, int& y, int x, int width) {
    // P9: display:none — element + subtree occupy nothing, never focused.
    if (el.hide) return;
    if (el.type == HtmlElementType::LINE_BREAK) {
        y += LINE_HEIGHT / 2;
        return;
    }
    if (el.type == HtmlElementType::TEXT) {
        // Phase 2 — audit fix: do NOT return early here. The parser builds
        // `<p>`/`<div>`/`<li>`/etc. as TEXT-type wrappers with the actual
        // text pushed into their `children` (a single TEXT child). If we
        // return before iterating children, the inner text element keeps
        // its default y=-1, h=-1, and renderElement() clips it out as
        // "above the content area" — leaving the screen blank even though
        // the HTML parsed fine. Recurse first, then claim the line for
        // the wrapper itself if it has any text of its own.
        // P14.2: wrapper margins wrap the whole block (children + own text).
        // P-GEMINI: only ul/ol/blockquote indent (no exponential cascade);
        // empty containers take no space (margin collapse).
        int childX = x;
        int childW = width;
        if (el.tag == "ul" || el.tag == "ol" || el.tag == "blockquote") {
            childX = x + 16;
            childW = width - 16;
        }
        // Container margin collapse: only leaf blocks with actual text or specific tags
        // claim vertical margin. Raw container div cascades never accumulate phantom empty gaps.
        bool hasVisibleContent = !el.text.empty() || el.tag == "hr";
        if (!hasVisibleContent) {
            for (const auto& ch : el.children) {
                if (!ch.hide && (!ch.text.empty() || !ch.children.empty() ||
                                 ch.type == HtmlElementType::IMAGE ||
                                 ch.type == HtmlElementType::BUTTON ||
                                 ch.type == HtmlElementType::LINK)) {
                    hasVisibleContent = true;
                    break;
                }
            }
        }
        if (!hasVisibleContent) return; // Completely empty container — 0px footprint

        bool isBlockTag = (el.tag == "p" || el.tag == "hr" ||
                           (el.tag.size() == 2 && el.tag[0] == 'h'));
        if (isBlockTag) y += std::min(el.marginTop, 16);
        for (auto& child : el.children) {
            layoutElement(child, y, childX, childW);
        }
        if (el.text.empty()) {
            if (isBlockTag) y += std::min(el.marginBottom, 16);
            return;
        }
        // P2: pixel word-wrap (vnexpress lines otherwise overflow 992px).
        // P9: wrap + measure with the CSS font size.
        int px = fontPx(el);
        int lh = el.lineH > 0 ? el.lineH : LINE_HEIGHT;
        el.availW = width;
        el.lines = wrapText(el.text, width, px);
        if (el.lines.empty()) return;
        el.x = x; el.y = y;
        el.width = 0;
        for (const auto& ln : el.lines) {
            el.width = std::max(el.width, textWidth(ln, px));
        }
        el.height = (int)el.lines.size() * lh;
        y += el.height + std::min(el.marginBottom, 16);
        // Moderate spacing for paragraphs/headings only if no CSS marginBottom
        if (el.marginBottom <= 0) {
            if (el.tag == "p") y += 4;
            else if (el.tag.size() == 2 && el.tag[0] == 'h' &&
                     el.tag[1] >= '1' && el.tag[1] <= '6') y += 4;
        }
        return;
    }
    if (el.type == HtmlElementType::LINK || el.type == HtmlElementType::BUTTON ||
        el.type == HtmlElementType::INPUT_TEXT || el.type == HtmlElementType::INPUT_PASSWORD ||
        el.type == HtmlElementType::INPUT_CHECKBOX) {
        // Recurse into children preserving x and width (no artificial indent).
        // yStart anchors wrapper links at the TOP of their content so focus
        // scroll lands on the visible headline, not blank space below it.
        int yStart = y;
        for (auto& child : el.children) {
            layoutElement(child, y, x, width);
        }
        // Links/buttons whose text lives in children don't claim an extra blank line,
        // but DO adopt the bounding box of their children so focus glow covers the card!
        if ((el.type == HtmlElementType::LINK || el.type == HtmlElementType::BUTTON) &&
            el.text.empty() && !el.children.empty()) {
            if (el.children.size() == 1 && el.children.front().type == HtmlElementType::IMAGE) {
                // Snug bounding box wrapping the single image cleanly
                const auto& childImg = el.children.front();
                el.x = childImg.x;
                el.y = childImg.y;
                el.width = childImg.width;
                el.height = childImg.height;
            } else {
                el.x = x; el.y = yStart;
                el.width = width;
                el.height = std::max(28, y - yStart);
            }
            el.availW = width;
            m_focusable.push_back(&el);
            return;
        }
        if (el.type == HtmlElementType::LINK && !el.text.empty()) {
            int px = fontPx(el);
            int lh = el.lineH > 0 ? el.lineH : LINE_HEIGHT;
            el.lines = wrapText(el.text, width, px);
            y += std::min(el.marginTop, 16);
            el.x = x; el.y = y;
            el.width = width;
            el.height = std::max((int)el.lines.size() * lh, lh);
            el.availW = width;
            m_focusable.push_back(&el);
            y += el.height + std::min(el.marginBottom, 16);
            return;
        }
        std::string displayText = el.text;
        if (el.type == HtmlElementType::INPUT_TEXT || el.type == HtmlElementType::INPUT_PASSWORD) {
            displayText = el.value.empty() ? el.placeholder : el.value;
            if (el.type == HtmlElementType::INPUT_PASSWORD) {
                displayText = std::string(displayText.size(), '*');
            }
        }
        y += std::min(el.marginTop, 16);
        el.x = x; el.y = y;
        int px = fontPx(el);
        el.availW = width;
        el.width = textWidth(displayText, px) + PADDING * 2;
        el.height = LINE_HEIGHT + 8;
        if (el.type == HtmlElementType::INPUT_TEXT || el.type == HtmlElementType::INPUT_PASSWORD) {
            el.width = std::min(el.width, (int)CONTENT_W - PADDING * 2);
        }
        m_focusable.push_back(&el);
        y += el.height + std::min(el.marginBottom, 16);
        return;
    }
    if (el.type == HtmlElementType::IMAGE) {
        // Settings: images off take no space at all.
        if (!BrowserManager::instance().settings().images) {
            el.x = x; el.y = y;
            el.width = 0; el.height = 0;
            return;
        }
        // P5: adopt cached dimensions when loaded; else the parsed reserve
        // (intrinsicsize/width/height); else the placeholder box.
        // Animated GIFs use frame 0's dimensions.
        std::string full = resolveUrl(el.src);
        int tw = 0, th = 0;
        auto git = m_gifAnims.find(full);
        if (git != m_gifAnims.end()) {
            tw = git->second.w;
            th = git->second.h;
        } else {
            auto it = m_imgTex.find(full);
            if (it != m_imgTex.end()) {
                tw = it->second.w;
                th = it->second.h;
            }
        }
        // Respect explicit CSS/HTML sizing first; fall back to texture dimensions
        int dw = (el.imgW > 0) ? el.imgW : tw;
        int dh = (el.imgH > 0) ? el.imgH : th;
        if (el.imgW > 0 && el.imgH <= 0 && tw > 0 && th > 0) {
            dh = (int)((double)th * el.imgW / tw);
        } else if (el.imgH > 0 && el.imgW <= 0 && tw > 0 && th > 0) {
            dw = (int)((double)tw * el.imgH / th);
        }
        if (dw > 0 && dh > 0) {
            // P10: tracking pixels (1x1 blank gifs) take no space at all.
            if (dw <= 8 && dh <= 8) {
                el.x = x; el.y = y;
                el.width = 0; el.height = 0;
                return;
            }
            int w = std::min(dw, width);
            int h = (int)((double)dh * w / dw);
            if (h > kImgMaxH) {
                h = kImgMaxH;
                w = (int)((double)dw * h / dh);
            }
            int imgX = (w >= 300 && w < width) ? (x + (width - w) / 2) : x;
            y += el.marginTop;
            el.x = imgX; el.y = y;
            el.width = w; el.height = h;
            y += h + 8 + el.marginBottom;
        } else {
            int pw = std::min(width, 400);
            int ph = pw * 9 / 16;
            if (ph < 120) ph = 120;
            y += el.marginTop;
            el.x = x; el.y = y;
            el.width = pw; el.height = ph;
            y += ph + 8 + el.marginBottom;
        }
        return;
    }
    if (el.type == HtmlElementType::INPUT_HIDDEN) {
        // P3: form data only — occupies no space, never focused.
        el.x = x; el.y = y;
        el.width = 0; el.height = 0;
        return;
    }
    if (el.type == HtmlElementType::SELECT) {
        // P3: dropdown box sized to the widest option (stable while cycling).
        std::vector<std::pair<std::string, std::string>> opts;
        selectOptions(el, opts);
        int px = fontPx(el);
        int w = textWidth("...", px);
        for (const auto& o : opts) {
            w = std::max(w, textWidth(o.second, px));
        }
        y += el.marginTop;
        el.x = x; el.y = y;
        el.width = std::min(w + PADDING * 2 + 24, (int)CONTENT_W - PADDING * 2);
        el.height = LINE_HEIGHT + 8;
        m_focusable.push_back(&el);
        y += LINE_HEIGHT + 8 + el.marginBottom;
        return;
    }
    if (el.type == HtmlElementType::TABLE) {
        // P13: simple grid. Rows are direct TABLE_ROW children (or inside
        // thead/tbody/tfoot wrappers); cells split the row width evenly.
        // P13b: colspan/rowspan via an occupancy grid (clamped 1..10).
        y += el.marginTop;
        std::vector<HtmlElement*> rows;
        std::function<void(HtmlElement&)> gather = [&](HtmlElement& n) {
            for (auto& c : n.children) {
                if (c.type == HtmlElementType::TABLE_ROW) {
                    rows.push_back(&c);
                } else if (c.type == HtmlElementType::TEXT &&
                           (c.tag == "thead" || c.tag == "tbody" ||
                            c.tag == "tfoot")) {
                    gather(c);
                }
            }
        };
        gather(el);
        // Placement: grid[r][c] = occupying cell (or nullptr).
        struct Place {
            HtmlElement* cell = nullptr;
            int r = 0, c0 = 0, cs = 1, rs = 1;
        };
        std::vector<Place> places;
        std::vector<std::vector<HtmlElement*>> grid;
        size_t ncols = 1;
        for (size_t ri = 0; ri < rows.size(); ri++) {
            if (grid.size() <= ri) grid.emplace_back();
            size_t c = 0;
            for (auto& ch : rows[ri]->children) {
                if (ch.type != HtmlElementType::TABLE_CELL) continue;
                while (c < grid[ri].size() && grid[ri][c] != nullptr) c++;
                int cs = std::max(1, std::min(10, ch.colspan));
                int rs = std::max(1, std::min(10, ch.rowspan));
                while (grid.size() <= ri + (size_t)rs - 1) grid.emplace_back();
                for (int dr = 0; dr < rs; dr++) {
                    if (grid[ri + dr].size() < c + (size_t)cs) {
                        grid[ri + dr].resize(c + (size_t)cs, nullptr);
                    }
                    for (int dc = 0; dc < cs; dc++) {
                        if (grid[ri + dr][c + (size_t)dc] == nullptr) {
                            grid[ri + dr][c + (size_t)dc] = &ch;
                        }
                    }
                }
                places.push_back({&ch, (int)ri, (int)c, cs, rs});
                c += (size_t)cs;
            }
            size_t rowCols = 0;
            for (auto* g : grid[ri]) {
                (void)g;
                rowCols++;
            }
            ncols = std::max(ncols, rowCols);
        }
        const int gap = 4;
        int colW = ncols ? (width - (int)(ncols - 1) * gap) / (int)ncols : width;
        if (colW < 40) colW = 40;
        el.x = x;
        el.y = y;
        el.width = width;
        // Measure content heights (children laid once here for measuring;
        // focusables pushed during measuring are truncated right after).
        std::vector<int> rowH(grid.size(), 0);
        std::vector<char> rowUsed(grid.size(), 0);
        std::vector<int> contentH(places.size(), 0);
        int px = fontPx(el);
        auto measureCell = [&](HtmlElement* cell, int cx, int cw) -> int {
            int cy = 0;
            if (!cell->text.empty()) {
                cell->lines = wrapText(cell->text, cw - 8, px);
                cy += (int)cell->lines.size() *
                      (cell->lineH > 0 ? cell->lineH : LINE_HEIGHT);
            }
            for (auto& ch : cell->children) {
                layoutElement(ch, cy, cx + 4, cw - 8);
            }
            return cy;
        };
        size_t f0 = m_focusable.size();
        for (size_t pi = 0; pi < places.size(); pi++) {
            Place& p = places[pi];
            int cw = p.cs * colW + (p.cs - 1) * gap;
            int cx = x + p.c0 * (colW + gap);
            contentH[pi] = measureCell(p.cell, cx, cw);
            if (p.rs == 1) {
                rowH[(size_t)p.r] = std::max(rowH[(size_t)p.r], contentH[pi]);
                rowUsed[(size_t)p.r] = 1;
            }
        }
        m_focusable.resize(f0);  // drop measuring-pass duplicates
        // Grow rows to fit row-spanning cells (deficit to the last row).
        for (size_t pi = 0; pi < places.size(); pi++) {
            const Place& p = places[pi];
            if (p.rs <= 1) continue;
            int have = 0;
            for (int dr = 0; dr < p.rs && (size_t)(p.r + dr) < rowH.size(); dr++) {
                have += rowH[(size_t)(p.r + dr)];
                if (dr) have += gap;
            }
            if (contentH[pi] > have && (size_t)(p.r + p.rs - 1) < rowH.size()) {
                rowH[(size_t)(p.r + p.rs - 1)] += contentH[pi] - have;
            }
            for (int dr = 0; dr < p.rs && (size_t)(p.r + dr) < rowUsed.size(); dr++) {
                rowUsed[(size_t)(p.r + dr)] = 1;
            }
        }
        if (rowH.empty()) rowH.push_back(0);
        // Row Y offsets (fully empty rows take a small gap, like before).
        std::vector<int> rowY(grid.size(), 0);
        {
            int ty = y;
            for (size_t r = 0; r < grid.size(); r++) {
                rowY[r] = ty;
                ty += rowUsed[r] ? rowH[r] + gap : 8;
            }
            el.height = ty - y;
            y = ty + el.marginBottom;
        }
        // Final boxes + single real content layout at final positions.
        for (size_t pi = 0; pi < places.size(); pi++) {
            Place& p = places[pi];
            int cw = p.cs * colW + (p.cs - 1) * gap;
            int cx = x + p.c0 * (colW + gap);
            int chh = 0;
            for (int dr = 0; dr < p.rs && (size_t)(p.r + dr) < rowH.size(); dr++) {
                if (!rowUsed[(size_t)(p.r + dr)]) continue;
                chh += rowH[(size_t)(p.r + dr)];
                if (dr) chh += gap;
            }
            int cy = rowY[(size_t)p.r];
            if (!p.cell->text.empty()) {
                p.cell->lines = wrapText(p.cell->text, cw - 8, px);
                p.cell->x = cx + 4;
                cy += (int)p.cell->lines.size() *
                      (p.cell->lineH > 0 ? p.cell->lineH : LINE_HEIGHT);
            }
            for (auto& chh2 : p.cell->children) {
                layoutElement(chh2, cy, cx + 4, cw - 8);
            }
            p.cell->x = cx;
            p.cell->y = rowY[(size_t)p.r];
            p.cell->width = cw;
            p.cell->height = chh;
        }
        return;
    }
    if (el.type == HtmlElementType::MEDIA) {
        // P13: embedded video box (16:9-ish, capped), focusable for A-play.
        y += el.marginTop;
        el.x = x; el.y = y;
        el.width = width;
        el.height = std::min(240, width * 9 / 16);
        if (el.height < 120) el.height = 120;
        m_focusable.push_back(&el);
        y += el.height + 8 + el.marginBottom;
        return;
    }
    int childY = y;
    for (auto& child : el.children) {
        layoutElement(child, childY, x, width);
    }
    if (!el.text.empty()) {
        el.x = x; el.y = y;
        el.width = textWidth(el.text);
        el.height = LINE_HEIGHT;
        y += LINE_HEIGHT;
    }
    y = childY;
}

void HtmlRenderer::updateFocus() {
    for (auto* el : m_focusable) {
        el->focused = false;
    }
    if (m_inputIndex >= 0 && m_inputIndex < (int)m_focusable.size()) {
        m_focusable[m_inputIndex]->focused = true;
    } else if (!m_focusable.empty()) {
        m_inputIndex = 0;
        m_focusable[0]->focused = true;
    }
    // Phase 2 — audit M2: scroll the focused element into the viewport.
    // Without this the user can press DOWN past the visible area and lose
    // track of which input they're on. ensureFocusVisible() adjusts
    // m_scrollY just enough to bring el.y, its children inside the content
    // area (CONTENT_Y..CONTENT_Y + CONTENT_H).
    ensureFocusVisible();
    markDirty();
}

void HtmlRenderer::focusNext() {
    if (m_focusable.empty()) return;
    m_editingText = false;
    m_inputIndex = (m_inputIndex + 1) % m_focusable.size();
    updateFocus();
}

void HtmlRenderer::focusPrev() {
    if (m_focusable.empty()) return;
    m_editingText = false;
    m_inputIndex = (m_inputIndex - 1 + m_focusable.size()) % m_focusable.size();
    updateFocus();
}

// Viewport-as-card block model: each top-level DOM node is one block.
// Only links/inputs/buttons/selects/checkboxes/media are events — plain
// text and images are never focusable and never highlighted.
static bool treeContainsNode(const HtmlElement* tree, const HtmlElement* target) {
    if (tree == target) return true;
    for (const auto& c : tree->children) {
        if (treeContainsNode(&c, target)) return true;
    }
    return false;
}

int HtmlRenderer::blockOf(int focusIdx) const {
    if (focusIdx < 0 || focusIdx >= (int)m_focusable.size()) return -1;
    const HtmlElement* target = m_focusable[(size_t)focusIdx];
    int bi = 0;
    for (const auto& top : m_elements) {
        if (treeContainsNode(&top, target)) return bi;
        bi++;
    }
    return -1;
}

std::vector<int> HtmlRenderer::blockMembers(int block) const {
    std::vector<int> out;
    if (block < 0) return out;
    int bi = 0;
    const HtmlElement* root = nullptr;
    for (const auto& top : m_elements) {
        if (bi == block) {
            root = &top;
            break;
        }
        bi++;
    }
    if (!root) return out;
    for (size_t i = 0; i < m_focusable.size(); i++) {
        if (treeContainsNode(root, m_focusable[i])) out.push_back((int)i);
    }
    return out;
}

void HtmlRenderer::focusInBlockStep(int dir) {
    if (m_focusable.empty()) return;
    m_editingText = false;
    int block = blockOf(m_inputIndex);
    std::vector<int> members = blockMembers(block);
    if (members.empty()) {
        // Current position has no block (or empty page) — fall back global.
        if (dir > 0) focusNext();
        else focusPrev();
        return;
    }
    size_t pos = 0;
    for (size_t i = 0; i < members.size(); i++) {
        if (members[i] == m_inputIndex) {
            pos = i;
            break;
        }
    }
    pos = (pos + members.size() + (dir > 0 ? 1 : members.size() - 1)) % members.size();
    m_inputIndex = members[pos];
    updateFocus();
}

void HtmlRenderer::focusBlockStep(int dir) {
    if (m_focusable.empty()) return;
    m_editingText = false;
    // Count blocks (top-level nodes).
    int nblocks = 0;
    for (size_t i = 0; i < m_elements.size(); i++) nblocks++;
    if (nblocks == 0) return;
    int cur = blockOf(m_inputIndex);
    // From "nowhere" (-1): DOWN starts at the first block, UP at the last.
    if (cur < 0) cur = (dir > 0) ? nblocks - 1 : 0;
    for (int step = 0; step < nblocks; step++) {
        cur = (cur + nblocks + dir) % nblocks;
        std::vector<int> members = blockMembers(cur);
        if (!members.empty()) {
            m_inputIndex = members[0];
            updateFocus();
            return;
        }
    }
    // No block with events (shouldn't happen) — keep position.
}

void HtmlRenderer::pageUp(int delta) {
    m_scrollY -= delta;
    if (m_scrollY < 0) m_scrollY = 0;
    // Viewport drives: focus follows to the nearest visible widget instead
    // of yanking the viewport back (old ensureFocusVisible made L1/R1 look
    // dead when focus sat at the top of a long page).
    focusNearestVisible();
    markDirty();
}

void HtmlRenderer::pageDown(int delta) {
    m_scrollY += delta;
    if (m_scrollY > m_maxScroll) m_scrollY = m_maxScroll;
    focusNearestVisible();
    markDirty();
}

void HtmlRenderer::focusNearestVisible() {
    if (m_focusable.empty()) return;
    int top = m_scrollY + CONTENT_Y;
    int best = -1;
    for (size_t i = 0; i < m_focusable.size(); i++) {
        const HtmlElement* e = m_focusable[i];
        if (e->y + e->height >= top) {
            best = (int)i;  // first overlapping viewport, else first below it
            break;
        }
    }
    if (best < 0) best = (int)m_focusable.size() - 1;
    for (auto* el : m_focusable) el->focused = false;
    m_inputIndex = best;
    m_focusable[best]->focused = true;
    markDirty();
}

// Read-scroll (dpad UP/DOWN): move the page line by line and highlight the
// nearest actionable event — the element keeps its own look plus an event
// cue (link underline, input border). Never yanks the viewport.
void HtmlRenderer::scrollRead(int delta) {
    if (m_focusable.empty()) {
        m_scrollY += delta;
        if (m_scrollY < 0) m_scrollY = 0;
        if (m_scrollY > m_maxScroll) m_scrollY = m_maxScroll;
        markDirty();
        return;
    }
    m_editingText = false;
    m_scrollY += delta;
    if (m_scrollY < 0) m_scrollY = 0;
    if (m_scrollY > m_maxScroll) m_scrollY = m_maxScroll;
    focusNearestVisible();
}

void HtmlRenderer::ensureFocusVisible() {
    if (m_inputIndex < 0 || m_inputIndex >= (int)m_focusable.size()) return;
    const HtmlElement* el = m_focusable[m_inputIndex];
    // P1: el->y is layout-space; viewport in layout-space is
    // [m_scrollY + CONTENT_Y, m_scrollY + CONTENT_Y + CONTENT_H].
    int top = m_scrollY + CONTENT_Y;
    int bottom = m_scrollY + CONTENT_Y + CONTENT_H;

    if (el->y < top) {
        m_scrollY -= (top - el->y);
    } else if (el->y + el->height > bottom) {
        m_scrollY += (el->y + el->height - bottom);
    }
    if (m_scrollY < 0) m_scrollY = 0;
    if (m_scrollY > m_maxScroll) m_scrollY = m_maxScroll;
    // P1: no layout() re-derivation — render subtracts scroll at draw time.
}

void HtmlRenderer::handleInput(int button) {
    if (m_editingText) {
        if (button == BTN_B) {
            // Cancel edit mode: hide the virtual keyboard and refocus the
            // current input so the cursor / highlight state is consistent
            // (Phase 0 — audit N7).
            m_editingText = false;
            updateFocus();
        }
        return;
    }
    switch (button) {
        case BTN_UP:
            if (!m_focusable.empty()) {
                if (m_inputIndex > 0) {
                    m_inputIndex--;
                } else {
                    m_scrollY -= 48;
                    if (m_scrollY < 0) m_scrollY = 0;
                }
                updateFocus();
                ensureFocusVisible();
                markDirty();
            } else {
                m_scrollY -= 48;
                if (m_scrollY < 0) m_scrollY = 0;
                markDirty();
            }
            break;
        case BTN_DOWN:
            if (!m_focusable.empty()) {
                if (m_inputIndex + 1 < (int)m_focusable.size()) {
                    m_inputIndex++;
                } else {
                    m_scrollY += 48;
                    if (m_scrollY > m_maxScroll) m_scrollY = m_maxScroll;
                }
                updateFocus();
                ensureFocusVisible();
                markDirty();
            } else {
                m_scrollY += 48;
                if (m_scrollY > m_maxScroll) m_scrollY = m_maxScroll;
                markDirty();
            }
            break;
        case 2:  // LEFT: nhảy nhanh lùi 4 mục hoặc cuộn trang
            if (!m_focusable.empty() && m_inputIndex > 0) {
                m_inputIndex = std::max(0, m_inputIndex - 4);
                updateFocus();
                ensureFocusVisible();
                markDirty();
            } else {
                pageUp(250);
            }
            break;
        case 3:  // RIGHT: nhảy nhanh tới 4 mục hoặc cuộn trang
            if (!m_focusable.empty() && m_inputIndex + 1 < (int)m_focusable.size()) {
                m_inputIndex = std::min((int)m_focusable.size() - 1, m_inputIndex + 4);
                updateFocus();
                ensureFocusVisible();
                markDirty();
            } else {
                pageDown(250);
            }
            break;
        case BTN_A:
            activateFocused();
            break;
        case BTN_B:
            if (canGoBack()) goBack();
            break;
    }
}

void HtmlRenderer::typeCharacter(char c) {
    if (m_inputIndex >= 0 && m_inputIndex < (int)m_focusable.size()) {
        HtmlElement* el = m_focusable[m_inputIndex];
        if (el->type == HtmlElementType::INPUT_TEXT || el->type == HtmlElementType::INPUT_PASSWORD) {
            el->value += c;
            markDirty();
        }
    }
}

void HtmlRenderer::deleteCharacter() {
    if (m_inputIndex >= 0 && m_inputIndex < (int)m_focusable.size()) {
        HtmlElement* el = m_focusable[m_inputIndex];
        if (el->type == HtmlElementType::INPUT_TEXT || el->type == HtmlElementType::INPUT_PASSWORD) {
            if (!el->value.empty()) el->value.pop_back();
            markDirty();
        }
    }
}

std::string HtmlRenderer::focusedInputValue() const {
    // Phase 1 — audit P1-2c: cho UIManager đọc giá trị hiện tại của field
    // đang focus (nếu là INPUT_TEXT/INPUT_PASSWORD) để mirror vào
    // VkState.query khi mới vào edit mode. Trả "" nếu không có field
    // focus hoặc element không phải text input (link, button, checkbox).
    if (m_inputIndex < 0 || m_inputIndex >= (int)m_focusable.size()) {
        return std::string();
    }
    const HtmlElement* el = m_focusable[m_inputIndex];
    if (el->type == HtmlElementType::INPUT_TEXT || el->type == HtmlElementType::INPUT_PASSWORD) {
        return el->value;
    }
    return std::string();
}

void HtmlRenderer::setFocusedInputValue(const std::string& v) {
    // Phase 1 — audit P1-2c: ghi đè giá trị field từ VkState.query (sau khi
    // VirtualKeyboard Telex transform xong). Trước audit, HtmlRenderer chỉ
    // có typeCharacter(char) append từng ký tự — không thể mirror cả một
    // query đã qua Telex transformation. setFocusedInputValue giải quyết
    // bằng cách ghi đè nguyên xi. 256 char cap là đủ cho mọi Wi-Fi portal
    // thực tế (username/password hiếm khi quá 64 char).
    if (m_inputIndex < 0 || m_inputIndex >= (int)m_focusable.size()) return;
    HtmlElement* el = m_focusable[m_inputIndex];
    if (el->type != HtmlElementType::INPUT_TEXT && el->type != HtmlElementType::INPUT_PASSWORD) return;
    // P3: honor maxlength (0 = default 256 cap).
    size_t cap = el->maxLen > 0 ? (size_t)el->maxLen : 256;
    el->value = v.size() > cap ? v.substr(0, cap) : v;
    markDirty();
}

int HtmlRenderer::focusedInputMaxLen() const {
    // P3: honor the focused field's maxlength (0 = default 256 cap).
    if (m_inputIndex >= 0 && m_inputIndex < (int)m_focusable.size()) {
        const HtmlElement* el = m_focusable[m_inputIndex];
        if ((el->type == HtmlElementType::INPUT_TEXT ||
             el->type == HtmlElementType::INPUT_PASSWORD) &&
            el->maxLen > 0) {
            return el->maxLen;
        }
    }
    return 256;
}

void HtmlRenderer::submitForm(const HtmlElement& form) {
    // Phase 3 — audit M5: real form submission.
    //
    // Form actions can carry credentials in their query strings (e.g.
    // "https://x/?token=…"), so we never log form.href verbatim. The
    // presence/absence of an action is enough to confirm the intent.
    Logger::info(std::string("Form submission (action present: ") +
                 (form.formAction.empty() ? "no" : "yes") + ")");

    // 1. Pick the action URL. The `form` parameter may not be a FORM element
    // (it might be a button), so we look at formAction (set by parseHtml
    // for every element inside a form). If neither is set, we have nothing
    // to POST to and the form was malformed.
    std::string action = form.formAction;
    if (action.empty()) {
        Logger::error("HtmlRenderer::submitForm: no form action URL");
        return;
    }
    // Phase 2 — audit M6: also enforce the http(s) whitelist at submit
    // time. A page can try to redirect to javascript:, file:, etc. via
    // <form action="...">. We refuse non-http(s) actions outright.
    if (action.substr(0, 7) != "http://" && action.substr(0, 8) != "https://") {
        Logger::error("HtmlRenderer::submitForm: refused non-http(s) action");
        return;
    }
    std::string method = form.formMethod.empty() ? std::string("GET") : form.formMethod;
    std::transform(method.begin(), method.end(), method.begin(), ::tolower);

    // 2. Collect inputs that share this form's action. parseHtml
    // denormalizes formAction onto every element, so a single pass over
    // m_elements (and their children) gathers the right fields.
    std::unordered_map<std::string, std::string> formData;
    collectFormInputs(m_elements, action, formData);

    // 3. Resolve relative action URLs against the current page URL.
    std::string resolvedAction = action;
    if (action[0] == '/' || action.find("://") == std::string::npos) {
        std::string base = m_currentUrl;
        size_t schemeEnd = base.find("://");
        if (schemeEnd != std::string::npos) {
            size_t pathStart = base.find('/', schemeEnd + 3);
            if (pathStart != std::string::npos) {
                base = base.substr(0, pathStart);
            }
        }
        if (action[0] == '/') {
            resolvedAction = base + action;
        } else {
            size_t lastSlash = m_currentUrl.rfind('/');
            if (lastSlash != std::string::npos) {
                resolvedAction = m_currentUrl.substr(0, lastSlash + 1) + action;
            } else {
                resolvedAction = base + "/" + action;
            }
        }
    }

    // 4. Dispatch asynchronously (P1 vnexpress plan): the old code called
    // HttpClient directly on the main thread and blocked the UI up to the
    // 15s timeout. Now the worker thread fetches and pollFetch() renders
    // the result, exactly like loadUrl().
    std::string targetUrl;
    if (method == "post") {
        targetUrl = resolvedAction;
    } else {
        // GET (or any non-POST): append query string and fetch.
        std::string url = resolvedAction;
        url += (url.find('?') == std::string::npos) ? "?" : "&";
        bool first = true;
        for (const auto& kv : formData) {
            if (!first) url += "&";
            first = false;
            url += HttpClient::instance().urlEncode(kv.first) + "=" +
                   HttpClient::instance().urlEncode(kv.second);
        }
        targetUrl = url;
    }

    fetchAsync(targetUrl, method, formData);
}

// P17: shared async fetch for form submits (LITE submitForm + FULL engine).
// Pushes history, flips to loading, and lets pollFetch() render the result.
void HtmlRenderer::fetchAsync(const std::string& targetUrl,
                              const std::string& method,
                              const std::unordered_map<std::string, std::string>& formData) {
    if (m_historyPos + 1 < m_history.size()) {
        m_history.resize(m_historyPos + 1);
    }
    m_history.push_back(targetUrl);
    m_historyPos = m_history.size() - 1;
    m_currentUrl = targetUrl;
    m_loading = true;
    m_error = false;
    m_errorMsg.clear();
    markDirty();

    struct FormFetch {
        std::string url;
        std::string method;
        std::unordered_map<std::string, std::string> data;
    };
    FormFetch* payload = new FormFetch{targetUrl, method, formData};
    pthread_t tid;
    int rc = pthread_create(&tid, nullptr, [](void* arg) -> void* {
        std::unique_ptr<FormFetch> owned(static_cast<FormFetch*>(arg));
        HttpResponse resp;
        if (owned->method == "post") {
            resp = HttpClient::instance().postForm(
                owned->url, owned->data,
                HtmlRenderer::getBrowserHeaders(HtmlRenderer::instance().currentUrl()));
        } else {
            resp = HttpClient::instance().get(
                owned->url,
                HtmlRenderer::getBrowserHeaders(HtmlRenderer::instance().currentUrl()), 15);
        }
        PendingFetch* out = new PendingFetch();
        out->url = owned->url;
        out->success = resp.success;
        out->body = std::move(resp.body);
        out->error = std::move(resp.error);
        out->contentType = contentTypeOf(resp.headers);
        out->effectiveUrl = std::move(resp.effectiveUrl);
        HtmlRenderer::instance().enqueueFetchResult(out);
        return nullptr;
    }, payload);
    if (rc != 0) {
        delete payload;
        m_loading = false;
        m_error = true;
        m_errorMsg = "Form submit failed (no thread)";
        Logger::error("HtmlRenderer::submitForm: pthread_create failed");
        loadHtml("<html><body><h1>Khong the gui form</h1></body></html>");
    }
}

void HtmlRenderer::render() {
    // NOTE: no dirty-gate here. UIManager paints the full-screen background
    // every frame before calling us, so skipping the DOM walk would leave
    // the content area black (flicker on scroll). Text textures are cached
    // and off-viewport nodes are skipped, so a full walk is just blits.
    m_dirty = false;
    // P5/P9: upload arrived images + apply arrived stylesheets (cheap when
    // idle — mutex + empty checks only).
    pumpImages();
    pumpCss();
    // JS timers/XHR/src-scripts pump (no-op without a live engine).
    pollJs();
    // Phase 2 — audit M1: only paint the background inside the content area
    // (between the 64 px header and the 53 px footer). The UIManager draws
    // the header and footer separately, so filling the full screen would
    // briefly flash black across them every frame.
    // P9b: paint the site's own background (white news pages stay white).
    drawRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, m_pageBg, true);
    // Clip page elements to the content viewport: partially-visible rows
    // (sy < CONTENT_Y when scrolled) must not paint over the header/URL
    // bar or the footer chrome.
    if (m_renderer) {
        SDL_Rect clip = {0, CONTENT_Y, SCREEN_W, CONTENT_H};
        SDL_RenderSetClipRect(m_renderer, &clip);
    }
    for (auto& el : m_elements) {
        renderElement(el, false);
    }
    if (m_renderer) {
        SDL_RenderSetClipRect(m_renderer, nullptr);
    }
}

void HtmlRenderer::renderElement(const HtmlElement& el, bool linkHot) {
    // P9: display:none subtree draws nothing (layout gave it no space).
    if (el.hide) return;
    // Phase 2 — audit M1: clip to the content area, not the full screen.
    // Elements that fall under the header/footer chrome are skipped.
    // NOTE: do NOT early-return here. The tree is built as nested
    // wrappers (e.g. <html> → <head>/<body> → <p> → text), and the
    // layout pass only sets y/height for elements that own text.
    // Intermediate wrappers keep y=0, h=0 from the struct default —
    // if we returned on that, we'd never recurse into the children
    // that actually carry the text. So we only skip the element's OWN
    // draw call; the children always get a chance to draw.
    // P1: el.y is layout-space; screen row = el.y - m_scrollY.
    // Event cue: text inside a focused link renders hot (cyan + underline);
    // inputs keep their border highlight. Plain text/images never hot.
    bool hot = linkHot || (el.type == HtmlElementType::LINK && el.focused);
    bool childHot = linkHot || (el.type == HtmlElementType::LINK && el.focused);
    int sy = el.y - m_scrollY;
    bool inView = (sy + el.height >= CONTENT_Y && sy <= CONTENT_Y + CONTENT_H);
    if (inView) {
        switch (el.type) {
        case HtmlElementType::TEXT: {
            int lh = el.lineH > 0 ? el.lineH : LINE_HEIGHT;
            int px = fontPx(el);
            TTF_Font* f = fontFor(px);
            // P9: CSS color wins; else theme-aware defaults (dark ink on
            // white pages, light ink on dark pages). Hot link text = cyan.
            SDL_Color tc = inkColor();
            if (hot) tc = SDL_Color{0, 180, 216, 255};
            else if (el.hasColor) tc = el.color;
            else if (lh > LINE_HEIGHT) tc = m_pageLight ? SDL_Color{0, 0, 0, 255}
                                                        : SDL_Color{255, 255, 255, 255};
            else if (el.bold) tc = inkBoldColor();
            if (f) {
                // P9: background highlight behind the text block.
                // P13b: background-image wins over the flat color.
                if (el.hasBg && !el.lines.empty()) {
                    int bh = (int)el.lines.size() * lh;
                    if (!drawBgImageBox(el.bgImage, el.x - 4, sy - 2, el.width + 8, bh + 4)) {
                        drawRect(el.x - 4, sy - 2, el.width + 8, bh + 4, el.bgColor, true);
                    }
                }
                // P15.2: current find hit gets a marker background.
                if (!m_findHits.empty() && m_findIndex >= 0 &&
                    m_findHits[(size_t)m_findIndex] == &el && !el.lines.empty()) {
                    int bh = (int)el.lines.size() * lh;
                    SDL_Color hl = m_pageLight ? SDL_Color{250, 204, 21, 255}
                                               : SDL_Color{0, 90, 130, 255};
                    drawRect(el.x - 4, sy - 2, el.width + 8, bh + 4, hl, true);
                }
                auto drawLine = [&](const std::string& ln, int ly) {
                    int lw = textWidth(ln, px);
                    int dx = el.x;
                    if (el.centered && el.availW > 0) {
                        dx = el.x + (el.availW - lw) / 2;
                        if (dx < el.x) dx = el.x;
                    } else if (el.textAlign == TextAlign::RIGHT && el.availW > 0) {
                        dx = el.x + el.availW - lw;
                        if (dx < el.x) dx = el.x;
                    }
                    drawText(ln, dx, ly, tc, f, false, px);
                    // P2: fake-bold double-draw (textures cached, ~free).
                    if (el.bold) drawText(ln, dx + 1, ly, tc, f, false, px);
                    // Event cue: focused hyperlink underlines its text.
                    if (hot) drawRect(dx, ly + lh - 5, lw > 0 ? lw : el.width, 2, tc, true);
                };
                if (el.lines.empty()) {
                    if (!el.text.empty()) drawLine(el.text, sy);
                } else {
                    for (size_t i = 0; i < el.lines.size(); i++) {
                        drawLine(el.lines[i], sy + (int)i * lh);
                    }
                }
            }
            break;
        }
        case HtmlElementType::LINK: {
            // Focus glow box: visually highlights the whole link or card clearly
            if (el.focused) {
                SDL_Color fbg = {30, 58, 138, 70};
                SDL_Color fedge = {0, 180, 216, 255};
                drawRect(el.x - 4, sy - 2, el.width + 8, el.height + 4, fbg, true);
                drawRect(el.x - 4, sy - 2, el.width + 8, el.height + 4, fedge, false);
            }
            // P9: CSS color when present (focus highlight still wins).
            SDL_Color color = el.focused ? SDL_Color{0, 180, 216, 255}
                              : el.hasColor ? el.color
                                            : linkBaseColor();
            if (m_font) {
                int px = fontPx(el);
                int lh = el.lineH > 0 ? el.lineH : LINE_HEIGHT;
                if (!el.lines.empty()) {
                    for (size_t i = 0; i < el.lines.size(); i++) {
                        int ly = sy + (int)i * lh;
                        drawText(el.lines[i], el.x, ly, color, m_font, false, px);
                        if (el.focused) {
                            int lw = textWidth(el.lines[i], px);
                            drawRect(el.x, ly + lh - 5, lw > 0 ? lw : el.width, 2, color, true);
                        }
                    }
                } else if (!el.text.empty()) {
                    drawText(el.text, el.x, sy, color, m_font, false, px);
                    // Event cue: underline a focused link's own text.
                    if (el.focused) {
                        int lw = textWidth(el.text, px);
                        drawRect(el.x, sy + LINE_HEIGHT - 5, lw > 0 ? lw : el.width, 2,
                                 color, true);
                    }
                }
            }
            break;
        }
        case HtmlElementType::INPUT_TEXT:
        case HtmlElementType::INPUT_PASSWORD: {
            SDL_Color bgColor = widgetBg(el.focused);
            SDL_Color borderColor = widgetEdge(el.focused);
            SDL_Color textColor = widgetInk();
            drawRect(el.x, sy, el.width, el.height, bgColor, true);
            drawRect(el.x, sy, el.width, el.height, borderColor, false);
            std::string displayText = el.value.empty() ? el.placeholder : el.value;
            if (el.type == HtmlElementType::INPUT_PASSWORD) {
                displayText = std::string(displayText.size(), '*');
            }
            if (displayText.empty()) displayText = " ";
            SDL_Color placeholderColor = widgetDim();
            int ipx = fontPx(el);
            if (m_font) drawText(displayText, el.x + PADDING, sy + 6,
                el.value.empty() ? placeholderColor : textColor, m_font, false, ipx);
            break;
        }
        case HtmlElementType::INPUT_CHECKBOX: {
            SDL_Color bgColor = widgetBg(el.focused);
            SDL_Color borderColor = widgetEdge(el.focused);
            int boxSize = 24;
            drawRect(el.x, sy + 4, boxSize, boxSize, bgColor, true);
            drawRect(el.x, sy + 4, boxSize, boxSize, borderColor, false);
            if (el.checked && m_font) drawText("X", el.x, sy + 2, SDL_Color{0, 180, 216, 255}, m_font, false);
            if (m_font) drawText(el.text, el.x + boxSize + 8, sy + 4, widgetInk(), m_font, false);
            break;
        }
        case HtmlElementType::BUTTON: {
            SDL_Color bgColor = el.focused ? SDL_Color{0, 100, 150, 255} : widgetBg(false);
            SDL_Color borderColor = widgetEdge(el.focused);
            drawRect(el.x, sy, el.width, el.height, bgColor, true);
            drawRect(el.x, sy, el.width, el.height, borderColor, false);
            SDL_Color btnInk = (el.focused || !m_pageLight) ? SDL_Color{255, 255, 255, 255}
                                                            : widgetInk();
            if (m_font) drawText(el.text, el.x + el.width / 2, sy + 8, btnInk, m_font, true);
            break;
        }
        case HtmlElementType::IMAGE: {
            // P5: real texture when loaded, placeholder + alt while pending.
            // Animated GIFs pick the frame by wall clock (main thread only).
            // Settings off: layout gave zero size — draw nothing.
            if (!BrowserManager::instance().settings().images) break;
            bool drawn = false;
            if (!el.src.empty() && el.width > 0 && el.height > 0) {
                std::string full = resolveUrl(el.src);
                auto git = m_gifAnims.find(full);
                if (git != m_gifAnims.end() && git->second.frames.size() > 1 &&
                    git->second.totalCs > 0) {
                    uint32_t t = (uint32_t)(SDL_GetTicks() / 10) % git->second.totalCs;
                    const GifFrameTex* pick = &git->second.frames[0];
                    for (const auto& fr : git->second.frames) {
                        if (t < fr.delayCs || fr.delayCs == UINT32_MAX) {
                            pick = &fr;
                            break;
                        }
                        t -= fr.delayCs;
                    }
                    if (pick->tex) {
                        SDL_Rect dst = {el.x, sy, el.width, el.height};
                        SDL_RenderCopy(m_renderer, pick->tex, nullptr, &dst);
                        drawn = true;
                    }
                } else {
                    auto it = m_imgTex.find(full);
                    if (it != m_imgTex.end() && it->second.tex) {
                        SDL_Rect dst = {el.x, sy, el.width, el.height};
                        SDL_RenderCopy(m_renderer, it->second.tex, nullptr, &dst);
                        drawn = true;
                    }
                }
            }
            if (!drawn && el.width > 0 && el.height > 0) {
                drawRect(el.x, sy, el.width, el.height, phBoxBg(), true);
                drawRect(el.x, sy, el.width, el.height, phBoxEdge(), false);
                std::string cap = "[Ảnh]";
                if (m_font) drawText(cap, el.x + el.width / 2, sy + el.height / 2,
                                     widgetDim(), m_font, true);
            }
            break;
        }
        case HtmlElementType::INPUT_HIDDEN:
            break;  // P3: never drawn
        case HtmlElementType::TABLE_CELL: {
            // P13: bordered grid cell; children render at their own coords.
            // P13b: background-image fills the cell before the flat fill.
            if (drawBgImageBox(el.bgImage, el.x, sy, el.width, el.height)) {
                drawRect(el.x, sy, el.width, el.height, phBoxEdge(), false);
            } else {
                drawRect(el.x, sy, el.width, el.height, phBoxBg(), true);
                drawRect(el.x, sy, el.width, el.height, phBoxEdge(), false);
            }
            if (!el.lines.empty() && m_font) {
                int lh = el.lineH > 0 ? el.lineH : LINE_HEIGHT;
                int px = fontPx(el);
                SDL_Color tc = el.hasColor ? el.color : inkColor();
                for (size_t i = 0; i < el.lines.size(); i++) {
                    drawText(el.lines[i], el.x + 4, sy + 2 + (int)i * lh, tc,
                             m_font, false, px);
                    if (el.bold) {
                        drawText(el.lines[i], el.x + 5, sy + 2 + (int)i * lh, tc,
                                 m_font, false, px);
                    }
                }
            }
            break;
        }
        case HtmlElementType::MEDIA: {
            // P13: embedded video box (focus highlight = ready to play).
            SDL_Color bgColor = el.focused ? SDL_Color{30, 40, 60, 255} : phBoxBg();
            SDL_Color borderColor = el.focused ? SDL_Color{0, 180, 216, 255} : phBoxEdge();
            drawRect(el.x, sy, el.width, el.height, bgColor, true);

            // Draw poster frame if available
            bool hasPoster = false;
            if (!el.poster.empty()) {
                std::string fullPoster = resolveUrl(el.poster);
                auto pit = m_imgTex.find(fullPoster);
                if (pit != m_imgTex.end() && pit->second.tex) {
                    SDL_Rect dst{el.x, sy, el.width, el.height};
                    SDL_RenderCopy(m_renderer, pit->second.tex, nullptr, &dst);
                    // Dim overlay so focus edge & play indicator pop
                    drawRect(el.x, sy, el.width, el.height, {0, 0, 0, 70}, true);
                    hasPoster = true;
                }
            }

            drawRect(el.x, sy, el.width, el.height, borderColor, false);

            // Play badge in center
            int cx = el.x + el.width / 2;
            int cy = sy + el.height / 2;
            drawRect(cx - 24, cy - 24, 48, 48, {0, 0, 0, 180}, true);
            drawRect(cx - 24, cy - 24, 48, 48, el.focused ? SDL_Color{0, 180, 216, 255} : SDL_Color{255, 255, 255, 180}, false);
            if (m_font) {
                drawText("▶", cx, cy - 12, el.focused ? SDL_Color{0, 180, 216, 255} : SDL_Color{255, 255, 255, 255}, m_font, true);
            }

            // Subtitle label if no poster loaded
            if (!hasPoster) {
                std::string label = "[Video]";
                std::string url = el.href;
                size_t se = url.find("://");
                if (se != std::string::npos) {
                    size_t hs = se + 3;
                    size_t pe = url.find('/', hs);
                    label = "[Video] " + url.substr(hs, pe == std::string::npos
                                                              ? std::string::npos
                                                              : pe - hs);
                } else if (!url.empty() && url[0] == '/') {
                    label = "[Video] " + url;
                }
                if (m_font) {
                    drawText(label, cx, cy + 28,
                             el.focused ? SDL_Color{0, 180, 216, 255} : widgetDim(),
                             m_font, true);
                }
            }
            break;
        }
        default:
            if (!el.text.empty() && m_font) {
                int px = fontPx(el);
                drawText(el.text, el.x, sy, inkColor(), m_font, false, px);
            }
            break;
    }
    }  // if (inView)
    for (const auto& child : el.children) {
        renderElement(child, childHot);
    }
}

void HtmlRenderer::stop() {
    m_elements.clear();
    m_focusable.clear();
    clearFind();
    m_js.reset();
    m_jsEpoch++;
    m_scripts.clear();
    m_scriptSrcs.clear();
    {
        std::lock_guard<std::mutex> lock(m_jsMutex);
        m_jsQueue.clear();
        m_jsReady.clear();
        m_jsInFlight = 0;
    }
    m_history.clear();
    m_historyPos = 0;
    m_currentUrl.clear();
    m_scrollY = 0;
    m_inputIndex = -1;  // -1 = no focus (matches init(); was 0)
    m_editingText = false;
    m_loading = false;
    m_error = false;
    m_errorMsg.clear();
    clearTextCache();
    clearImages();
    clearCss();
    m_dirty = true;
}

void HtmlRenderer::applyDecls(
    HtmlElement& el,
    const std::vector<std::pair<std::string, std::string>>& decls,
    bool isInline) {
    (void)isInline;  // priority handled by call order (inline applied last)
    for (const auto& d : decls) {
        const std::string& prop = d.first;
        const std::string& val = d.second;
        std::string low = val;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (prop == "display") {
            el.hide = (low.find("none") != std::string::npos);
        } else if (prop == "visibility") {
            el.hide = (low.find("hidden") != std::string::npos);
        } else if (prop == "color") {
            SDL_Color c;
            if (parseColor(val, c)) {
                el.color = c;
                el.hasColor = true;
            }
        } else if (prop == "background-color" || prop == "background") {
            // P13b: background: may carry both a color and url(...) — take
            // each part separately (parseColor fails on the combined value).
            size_t upos = low.find("url(");
            if (upos != std::string::npos) {
                size_t ue = low.find(')', upos + 4);
                if (ue != std::string::npos) {
                    std::string u = cssTrim(val.substr(upos + 4, ue - upos - 4));
                    if (!u.empty() && (u.front() == '"' || u.front() == '\'')) {
                        u = u.substr(1, u.size() > 1 ? u.size() - 2 : 0);
                    }
                    if (!u.empty() && u.compare(0, 5, "data:") != 0) {
                        el.bgImage = u;  // resolved at layout (needs base URL)
                    }
                }
            }
            SDL_Color c;
            if (parseColor(val, c)) {
                el.bgColor = c;
                el.hasBg = true;
            }
        } else if (prop == "background-image") {
            // P13b: url(...) only; gradients/data: ignored.
            size_t upos = low.find("url(");
            if (upos != std::string::npos) {
                size_t ue = low.find(')', upos + 4);
                if (ue != std::string::npos) {
                    std::string u = cssTrim(val.substr(upos + 4, ue - upos - 4));
                    if (!u.empty() && (u.front() == '"' || u.front() == '\'')) {
                        u = u.substr(1, u.size() > 1 ? u.size() - 2 : 0);
                    }
                    if (!u.empty() && u.compare(0, 5, "data:") != 0 &&
                        low.find("gradient") == std::string::npos) {
                        el.bgImage = u;  // resolved at layout (needs base URL)
                    }
                }
            }
        } else if (prop == "font-size") {
            int px = parsePx(val);
            if (px > 0) el.fontSize = px;
        } else if (prop == "font") {
            // P14.1: shorthand "italic bold 14px/1.5 arial" — take style,
            // weight and size; ignore family/line-height.
            std::string tok;
            std::stringstream ss(low);
            while (ss >> tok) {
                if (tok == "italic" || tok == "oblique") {
                    continue;  // no italic face — skip
                } else if (tok == "bold" || tok == "700" || tok == "800" ||
                           tok == "900" || tok == "bolder") {
                    el.bold = true;
                } else if (tok == "normal" || tok == "400" || tok == "lighter") {
                    el.bold = false;
                } else {
                    // size may carry line-height: "14px/1.5"
                    size_t slash = tok.find('/');
                    std::string sizePart =
                        (slash == std::string::npos) ? tok : tok.substr(0, slash);
                    int px = parsePx(sizePart);
                    if (px > 0) {
                        el.fontSize = px;
                        break;  // family names follow — done
                    }
                }
            }
        } else if (prop == "font-weight") {
            if (low == "bold" || low == "700" || low == "800" || low == "900" ||
                low == "bolder") {
                el.bold = true;
            } else if (low == "normal" || low == "400" || low == "lighter") {
                el.bold = false;
            }
        } else if (prop == "text-align") {
            if (low.find("center") != std::string::npos) {
                el.textAlign = TextAlign::CENTER;
                el.centered = true;
                el.hasExplicitAlign = true;
            } else if (low.find("right") != std::string::npos) {
                el.textAlign = TextAlign::RIGHT;
                el.centered = false;
                el.hasExplicitAlign = true;
            } else if (low.find("left") != std::string::npos) {
                el.textAlign = TextAlign::LEFT;
                el.centered = false;
                el.hasExplicitAlign = true;
            }
        } else if (prop == "width" || prop == "max-width") {
            int px = parsePx(val);
            if (px > 0) el.imgW = px;
        } else if (prop == "height" || prop == "max-height") {
            int px = parsePx(val);
            if (px > 0) el.imgH = px;
        } else if (prop == "margin-top" || prop == "padding-top") {
            int v = std::min(24, std::max(0, parsePx(val)));
            if (prop[0] == 'm') el.marginTop = v;
            else el.marginTop += v / 2;  // P14.2: padding counts half (inner)
        } else if (prop == "margin-bottom" || prop == "padding-bottom") {
            int v = std::min(24, std::max(0, parsePx(val)));
            if (prop[0] == 'm') el.marginBottom = v;
            else el.marginBottom += v / 2;
        } else if (prop == "padding") {
            // P14.2: uniform padding counts half (inner spacing, clamped to prevent gap balloons).
            int v = std::min(24, std::max(0, parsePx(val)));
            el.marginTop += v / 2;
            el.marginBottom += v / 2;
        } else if (prop == "line-height") {
            // P14.2: px wins outright; bare number multiplies font size.
            std::string t = cssTrim(val);
            if (t.find("px") != std::string::npos || t.find("pt") != std::string::npos) {
                int px = parsePx(t);
                if (px > 0) el.lineHPx = px;
            } else {
                try {
                    float f = std::stof(t);
                    if (f > 0.5f && f < 5.0f) el.lineHNum = f;
                } catch (...) {}
            }
        }
    }
}

void HtmlRenderer::applyStyles() {
    // Reset all hide flags first: CSS display:none re-applies below, and
    // reader-mode hides must not leak across toggles/pages.
    std::function<void(std::list<HtmlElement>&)> unhide =
        [&](std::list<HtmlElement>& els) {
            for (auto& el : els) {
                el.hide = false;
                unhide(el.children);
            }
        };
    unhide(m_elements);
    // Settings: CSS off means tag styles only (P2) + white default theme.
    if (!BrowserManager::instance().settings().css) {
        updatePageTheme();
        applyReaderMode();
        return;
    }
    struct Inh {
        bool hasColor = false;
        SDL_Color color = {0, 0, 0, 255};
        int fontSize = 0;
        bool bold = false;
        bool centered = false;
        TextAlign textAlign = TextAlign::LEFT;
        int lineHPx = 0;   // P14.2: line-height inherits like font-size
        float lineHNum = 0;
    };
    std::vector<const HtmlElement*> ancestors;
    std::function<void(std::list<HtmlElement>&, Inh)> walk =
        [&](std::list<HtmlElement>& els, Inh inh) {
            for (auto& el : els) {
                // Collect matching rules, weakest first (inline wins last).
                struct Hit {
                    int spec;
                    int order;
                    const CssRule* rule;
                };
                std::vector<Hit> hits;
                for (const auto& rule : m_cssRules) {
                    int best = -1;
                    for (const auto& sel : rule.selectors) {
                        if (matchSelector(sel, el, ancestors)) {
                            best = std::max(best, sel.specificity);
                        }
                    }
                    if (best >= 0) hits.push_back({best, rule.order, &rule});
                }
                std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
                    if (a.spec != b.spec) return a.spec < b.spec;
                    return a.order < b.order;
                });
                for (const auto& h : hits) applyDecls(el, h.rule->decls, false);
                if (!el.inlineCss.empty()) {
                    applyDecls(el, parseDeclBlock(el.inlineCss), true);
                }
                // Inherit typography into children (CSS inheritance).
                if (!el.hasColor && inh.hasColor) {
                    el.color = inh.color;
                    el.hasColor = true;
                }
                if (el.fontSize <= 0) el.fontSize = inh.fontSize;
                if (el.lineHPx <= 0) el.lineHPx = inh.lineHPx;
                if (el.lineHNum <= 0) el.lineHNum = inh.lineHNum;
                el.bold = el.bold || inh.bold;
                // Only inherit text alignment if this element has no explicit alignment
                if (!el.hasExplicitAlign) {
                    el.textAlign = inh.textAlign;
                    el.centered = (el.textAlign == TextAlign::CENTER);
                }
                if (el.fontSize > 0) {
                    el.lineH = std::max(20, cssPx(el.fontSize) * 14 / 10);
                }
                // P14.2: explicit line-height wins over the derived one.
                if (el.lineHPx > 0) {
                    int lh = cssPx(el.lineHPx);
                    if (lh < 16) lh = 16;
                    if (lh > 90) lh = 90;
                    el.lineH = lh;
                } else if (el.lineHNum > 0 && el.fontSize > 0) {
                    el.lineH = std::max(16, (int)(cssPx(el.fontSize) * el.lineHNum));
                }
                Inh child{el.hasColor, el.color, el.fontSize, el.bold,
                            el.centered, el.textAlign, el.lineHPx, el.lineHNum};
                ancestors.push_back(&el);
                walk(el.children, child);
                ancestors.pop_back();
            }
        };
    walk(m_elements, Inh{});
    updatePageTheme();
    applyReaderMode();
}

void HtmlRenderer::updatePageTheme() {
    // P9b: body background wins, else <html>. No background anywhere means
    // the browser-default white canvas (vnexpress mobile ships none).
    m_pageBg = {255, 255, 255, 255};
    m_pageLight = true;
    HtmlElement body;
    body.tag = "body";
    body.cls = m_bodyCls;
    struct Hit {
        int spec;
        int order;
        const CssRule* rule;
    };
    std::vector<Hit> hits;
    std::vector<const HtmlElement*> noAnc;
    for (const auto& rule : m_cssRules) {
        int best = -1;
        for (const auto& sel : rule.selectors) {
            // Only single-part body/html selectors (no ancestor context).
            if (sel.parts.size() == 1 &&
                (sel.parts[0].tag == "body" || sel.parts[0].tag == "html") &&
                matchSelector(sel, body, noAnc)) {
                best = std::max(best, sel.specificity);
            }
        }
        if (best >= 0) hits.push_back({best, rule.order, &rule});
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.spec != b.spec) return a.spec < b.spec;
        return a.order < b.order;
    });
    for (const auto& h : hits) applyDecls(body, h.rule->decls, false);
    if (!m_bodyInline.empty()) {
        applyDecls(body, parseDeclBlock(m_bodyInline), true);
    }
    if (body.hasBg) {
        m_pageBg = body.bgColor;
        int lum = (body.bgColor.r * 299 + body.bgColor.g * 587 +
                   body.bgColor.b * 114) / 1000;
        m_pageLight = lum > 128;
    }
}

void HtmlRenderer::applyReaderMode() {
    // P11: article pages (vnexpress id=article_detail) render as clean
    // reader view: keep the article ancestor chain, hide sibling chrome.
    // Portal/form pages (password fields) are never touched.
    m_readerActive = false;
    if (!BrowserManager::instance().settings().article) return;
    // Reset reader hides from a previous pass (display:none re-applies
    // through the normal cascade afterwards — applyStyles runs first).
    // NOTE: caller applyStyles() already cleared all hide flags at entry.
    std::function<bool(const HtmlElement*)> hasPassword = [&](const HtmlElement* el) -> bool {
        if (el->type == HtmlElementType::INPUT_PASSWORD) return true;
        for (const auto& c : el->children) {
            if (hasPassword(&c)) return true;
        }
        return false;
    };
    for (const auto& el : m_elements) {
        if (hasPassword(&el)) return;  // login/portal page — leave alone
    }
    // Find article root: id="article_detail" wins outright. Otherwise the
    // first <article> that actually looks like a main story (long h1 or
    // fck_detail body) — related-item boxes (homepage lists) must NOT
    // trigger reader mode or the whole homepage would collapse into one box.
    const HtmlElement* root = nullptr;
    std::function<void(const std::list<HtmlElement>&)> findId =
        [&](const std::list<HtmlElement>& els) {
            for (const auto& el : els) {
                if (!root && el.elemId == "article_detail") root = &el;
                findId(el.children);
            }
        };
    findId(m_elements);
    std::function<bool(const HtmlElement&)> looksArticle =
        [&](const HtmlElement& el) -> bool {
            if (el.cls.find("fck_detail") != std::string::npos) return true;
            if (el.tag == "h1") {
                size_t len = 0;
                std::function<void(const HtmlElement&)> gather =
                    [&](const HtmlElement& n) {
                        len += n.text.size();
                        for (const auto& c : n.children) gather(c);
                    };
                gather(el);
                if (len > 20) return true;
            }
            for (const auto& c : el.children) {
                if (looksArticle(c)) return true;
            }
            return false;
        };
    if (!root) {
        std::function<void(const std::list<HtmlElement>&)> findTag =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (!root && el.tag == "article" && looksArticle(el)) root = &el;
                    findTag(el.children);
                }
            };
        findTag(m_elements);
    }
    // No article story on this page (homepage/portal) — leave everything.
    if (!root) return;
    m_readerActive = true;
    std::function<bool(const HtmlElement*, const HtmlElement*)> contains =
        [&](const HtmlElement* tree, const HtmlElement* target) -> bool {
            if (tree == target) return true;
            for (const auto& c : tree->children) {
                if (contains(&c, target)) return true;
            }
            return false;
        };
    std::function<void(HtmlElement&)> hideAll = [&](HtmlElement& el) {
        el.hide = true;
        for (auto& c : el.children) hideAll(c);
    };
    if (root) {
        // Keep the ancestor chain plus the article's whole subtree; hide
        // everything else (sibling chrome at every level).
        std::function<void(std::list<HtmlElement>&)> keep =
            [&](std::list<HtmlElement>& els) {
                for (auto& el : els) {
                    if (&el == root) {
                        continue;  // subtree stays fully visible
                    } else if (contains(&el, root)) {
                        keep(el.children);
                    } else {
                        hideAll(el);
                    }
                }
            };
        keep(m_elements);
    }
    // Junk-class pass inside the kept article (never on portals — guarded
    // above — never over headings, and never over the keep-chain itself).
    auto junky = [](const HtmlElement& el) -> bool {
        std::string s = el.cls + " " + el.elemId + " " + el.tag;
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        static const char* kJunk[] = {"comment", "related", "sidebar",
                                      "advertise", "breadcrumb", "share",
                                      "social", "newsletter", "popup",
                                      "modal", "banner", "widget", nullptr};
        for (int i = 0; kJunk[i]; i++) {
            if (s.find(kJunk[i]) != std::string::npos) return true;
        }
        // footer/header/nav/menu only when they carry no heading content
        static const char* kChrome[] = {"footer", "header", "nav", "menu", nullptr};
        for (int i = 0; kChrome[i]; i++) {
            if (s.find(kChrome[i]) == std::string::npos) continue;
            // keep if it contains a headline (title lives in headers)
            std::function<bool(const HtmlElement&)> hasHead =
                [&](const HtmlElement& n) -> bool {
                    if ((n.tag == "h1" || n.tag == "h2") && !n.children.empty()) return true;
                    for (const auto& c : n.children) {
                        if (hasHead(c)) return true;
                    }
                    return false;
                };
            if (!hasHead(el)) return true;
        }
        return false;
    };
    std::function<void(std::list<HtmlElement>&)> sweep =
        [&](std::list<HtmlElement>& els) {
            for (auto& el : els) {
                if (el.hide) continue;  // hidden with its subtree already
                // Chain nodes (root + ancestors) are never junked — the
                // whole article would vanish with them. Descend only.
                if (&el == root || contains(&el, root)) {
                    sweep(el.children);
                } else if (junky(el)) {
                    hideAll(el);
                } else {
                    sweep(el.children);
                }
            }
        };
    sweep(m_elements);
}

void HtmlRenderer::pumpCss() {
    // Settings: CSS off disables external stylesheets entirely.
    if (!BrowserManager::instance().settings().css) return;
    bool applied = false;
    {
        std::lock_guard<std::mutex> lock(m_cssMutex);
        for (auto& body : m_cssReady) {
            // P9e: external budget was enforced at queue time (m_extCssCount),
            // so no m_styleSheets.size() cap here — it would reintroduce the
            // starvation once inline <style> blocks fill the vector.
            if (body.size() <= kMaxCssBytes) {
                m_styleSheets.push_back(body);
                parseStylesheet(body);
                applied = true;
            }
        }
        m_cssReady.clear();
        if (!m_cssFetchEnabled) return;  // P9d: offline deterministic mode
        while (!m_cssQueue.empty() && m_cssInFlight < kMaxCssInFlight) {
            std::string url = resolveUrl(m_cssQueue.front());
            m_cssQueue.erase(m_cssQueue.begin());
            // P9e: this sheet will never arrive — free its budget slot.
            if (url.empty()) { m_extCssCount--; continue; }
            m_cssInFlight++;
            std::string* payload = new std::string(url);
            pthread_t tid;
            int rc = pthread_create(&tid, nullptr, [](void* arg) -> void* {
                std::unique_ptr<std::string> owned(static_cast<std::string*>(arg));
                HttpResponse resp = HttpClient::instance().get(
                    *owned, HtmlRenderer::getBrowserHeaders(
                                HtmlRenderer::instance().currentUrl()), 15);
                HtmlRenderer& self = HtmlRenderer::instance();
                {
                    std::lock_guard<std::mutex> lock(self.m_cssMutex);
                    if (resp.success && !resp.body.empty() &&
                        resp.body.size() <= kMaxCssBytes) {
                        self.m_cssReady.push_back(std::move(resp.body));
                    }
                    self.m_cssInFlight--;
                }
                return nullptr;
            }, payload);
            if (rc != 0) {
                delete payload;
                m_cssInFlight--;
                m_extCssCount--;  // P9e: never launched — free the slot
            } else {
                pthread_detach(tid);
            }
        }
    }
    if (applied) {
        if (BrowserManager::instance().settings().fullEngine) {
            std::string combinedCss;
            for (const auto& s : m_styleSheets) {
                combinedCss.append(s);
                combinedCss.push_back('\n');
            }
            NetSurfEngine::instance().loadHtml(m_rawHtml, m_currentUrl, combinedCss);
        } else {
            applyStyles();
            layout();
            updateFocus();
            markDirty();
        }
    }
}

void HtmlRenderer::clearCss() {
    std::lock_guard<std::mutex> lock(m_cssMutex);
    m_cssRules.clear();
    m_cssOrder = 0;
    m_sheetsParsed = 0;
    m_styleSheets.clear();
    m_cssQueue.clear();
    m_extCssCount = 0;  // P9e: external budget resets with the page
    m_cssReady.clear();
    m_cssInFlight = 0;
}

// ---- JavaScript (setting js, Duktape bridge) ----

HtmlElement* HtmlRenderer::jsFindById(const std::string& id) {
    if (id.empty()) return nullptr;
    HtmlElement* out = nullptr;
    std::function<void(std::list<HtmlElement>&)> walk =
        [&](std::list<HtmlElement>& els) {
            for (auto& el : els) {
                if (!out && el.elemId == id) out = &el;
                walk(el.children);
            }
        };
    walk(m_elements);
    return out;
}

HtmlElement* HtmlRenderer::jsQueryFirst(const std::string& sel) {
    // Single-compound selectors only (#id, .class, tag, tag.class).
    std::string s = sel;
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    if (s.empty() || s.find(' ') != std::string::npos) return nullptr;
    std::string tag, id;
    std::vector<std::string> classes;
    size_t k = 0;
    if (!s.empty() && s[0] != '#' && s[0] != '.') {
        size_t e = 0;
        while (e < s.size() && s[e] != '#' && s[e] != '.') e++;
        tag = s.substr(0, e);
        std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
        k = e;
    }
    while (k < s.size()) {
        if (s[k] == '#') {
            size_t e = k + 1;
            while (e < s.size() && s[e] != '.' && s[e] != '#') e++;
            id = s.substr(k + 1, e - k - 1);
            k = e;
        } else if (s[k] == '.') {
            size_t e = k + 1;
            while (e < s.size() && s[e] != '.' && s[e] != '#') e++;
            classes.push_back(s.substr(k + 1, e - k - 1));
            k = e;
        } else {
            break;
        }
    }
    HtmlElement* out = nullptr;
    std::function<void(std::list<HtmlElement>&)> walk =
        [&](std::list<HtmlElement>& els) {
            for (auto& el : els) {
                if (!out) {
                    bool ok = true;
                    if (!tag.empty() && tag != el.tag) ok = false;
                    if (ok && !id.empty() && id != el.elemId) ok = false;
                    if (ok) {
                        for (const auto& want : classes) {
                            bool found = false;
                            size_t i = 0;
                            while (i < el.cls.size()) {
                                while (i < el.cls.size() &&
                                       std::isspace((unsigned char)el.cls[i])) i++;
                                if (i >= el.cls.size()) break;
                                size_t j = i;
                                while (j < el.cls.size() &&
                                       !std::isspace((unsigned char)el.cls[j])) j++;
                                if (el.cls.compare(i, j - i, want) == 0) {
                                    found = true;
                                    break;
                                }
                                i = j;
                            }
                            if (!found) {
                                ok = false;
                                break;
                            }
                        }
                    }
                    if (ok) out = &el;
                }
                walk(el.children);
            }
        };
    walk(m_elements);
    return out;
}

void HtmlRenderer::jsSubmit(HtmlElement* el) {
    if (!el) return;
    // A form element submits directly; anything else submits when it has a
    // form context (same rule as the A key).
    if (el->type == HtmlElementType::FORM || !el->formAction.empty()) {
        submitForm(*el);
    }
}

void HtmlRenderer::activateFocused() {
    // Shared A-key action: dpad input and JS click() behave identically.
    if (m_inputIndex < 0 || m_inputIndex >= (int)m_focusable.size()) return;
    HtmlElement* el = m_focusable[m_inputIndex];
    if (el->type == HtmlElementType::INPUT_TEXT || el->type == HtmlElementType::INPUT_PASSWORD) {
        m_editingText = true;
    } else if (el->type == HtmlElementType::LINK && !el->href.empty()) {
        if (el->href.substr(0, 7) == "http://" || el->href.substr(0, 8) == "https://") {
            loadUrl(el->href);
        } else {
            std::string base = m_currentUrl;
            size_t lastSlash = base.rfind('/');
            if (lastSlash != std::string::npos) base = base.substr(0, lastSlash + 1);
            loadUrl(base + el->href);
        }
    } else if (el->type == HtmlElementType::BUTTON) {
        // Phase 3 — audit M5: A on a button submits its form.
        if (!el->formAction.empty()) {
            submitForm(*el);
        }
    } else if (el->type == HtmlElementType::INPUT_CHECKBOX) {
        // P3: toggle (was a no-op — checkbox could never change).
        el->checked = !el->checked;
        markDirty();
    } else if (el->type == HtmlElementType::SELECT) {
        // P3: cycle options.
        std::vector<std::pair<std::string, std::string>> opts;
        selectOptions(*el, opts);
        if (!opts.empty()) {
            el->selected = (el->selected + 1) % (int)opts.size();
        }
        markDirty();
    } else if (el->type == HtmlElementType::MEDIA) {
        // P13: queue a play request (UIManager + MpvPlayer own it).
        std::string target = resolveUrl(el->href);
        if (!target.empty()) {
            m_mediaRequest = target;
        }
        markDirty();
    }
}

void HtmlRenderer::jsActivate(HtmlElement* el) {
    if (!el) return;
    // Focus it first when focusable so highlight + edit state stay coherent.
    for (size_t i = 0; i < m_focusable.size(); i++) {
        if (m_focusable[i] == el) {
            m_inputIndex = (int)i;
            updateFocus();
            break;
        }
    }
    // Non-focusables (plain divs/spans) still get their A-action when the
    // element itself is actionable (link/button/input...).
    int saved = m_inputIndex;
    bool pushed = false;
    if (el->type == HtmlElementType::LINK || el->type == HtmlElementType::BUTTON ||
        el->type == HtmlElementType::INPUT_TEXT || el->type == HtmlElementType::INPUT_PASSWORD ||
        el->type == HtmlElementType::INPUT_CHECKBOX || el->type == HtmlElementType::SELECT ||
        el->type == HtmlElementType::MEDIA) {
        // Temporarily point focus at el if it isn't already there.
        bool found = false;
        for (size_t i = 0; i < m_focusable.size(); i++) {
            if (m_focusable[i] == el) {
                found = true;
                break;
            }
        }
        if (!found) {
            // Not focusable (e.g. display quirk) — act on a scratch copy of
            // the A-branch by direct dispatch.
            if (el->type == HtmlElementType::LINK && !el->href.empty()) {
                std::string href = el->href;
                if (href.compare(0, 7, "http://") == 0 || href.compare(0, 8, "https://") == 0) {
                    loadUrl(href);
                } else {
                    std::string base = m_currentUrl;
                    size_t lastSlash = base.rfind('/');
                    if (lastSlash != std::string::npos) base = base.substr(0, lastSlash + 1);
                    loadUrl(base + href);
                }
            } else if ((el->type == HtmlElementType::BUTTON ||
                        el->type == HtmlElementType::FORM) &&
                       !el->formAction.empty()) {
                submitForm(*el);
            } else if (el->type == HtmlElementType::INPUT_CHECKBOX) {
                el->checked = !el->checked;
                markDirty();
            }
            return;
        }
        pushed = true;
    }
    (void)pushed;
    (void)saved;
    activateFocused();
}

void HtmlRenderer::runJs() {
    if (!BrowserManager::instance().settings().js) return;
    if (m_scripts.empty() && m_scriptSrcs.empty()) return;
    if (!m_js || m_js->epoch() != m_jsEpoch) {
        m_js.reset(new JsEngine(this, m_jsEpoch, m_currentUrl));
    }
    bool mutated = false;
    size_t ran = 0;
    for (const auto& body : m_scripts) {
        if (ran++ >= 32) break;
        if (m_js->runScript(body, "inline")) mutated = true;
    }
    // Queue external scripts for async fetch (cap like stylesheets).
    {
        std::lock_guard<std::mutex> lock(m_jsMutex);
        for (const auto& src : m_scriptSrcs) {
            if (m_jsQueue.size() >= kMaxJsFiles) break;
            std::string url = (src.find("://") == std::string::npos) ? resolveUrl(src) : src;
            if (url.empty()) continue;
            if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) {
                continue;
            }
            // Filter out known heavy ad/analytics/tracker scripts that cause heap exhaustion
            if (url.find("google-analytics") != std::string::npos ||
                url.find("googletagmanager") != std::string::npos ||
                url.find("doubleclick") != std::string::npos ||
                url.find("facebook.net") != std::string::npos ||
                url.find("clarity.ms") != std::string::npos ||
                url.find("/ads") != std::string::npos) {
                continue;
            }
            m_jsQueue.push_back(url);
        }
    }
    if (mutated) {
        layout();
        updateFocus();
        markDirty();
    }
}

void HtmlRenderer::pollJs() {
    // Fetch queued external scripts (main thread spawns, workers fetch).
    {
        std::lock_guard<std::mutex> lock(m_jsMutex);
        while (!m_jsQueue.empty() && m_jsInFlight < kMaxJsInFlight) {
            std::string url = m_jsQueue.front();
            m_jsQueue.erase(m_jsQueue.begin());
            m_jsInFlight++;
            std::string* payload = new std::string(url);
            pthread_t tid;
            int rc = pthread_create(&tid, nullptr, [](void* arg) -> void* {
                std::unique_ptr<std::string> owned(static_cast<std::string*>(arg));
                HttpResponse resp = HttpClient::instance().get(
                    *owned, HtmlRenderer::getBrowserHeaders(
                                HtmlRenderer::instance().currentUrl()), 15);
                HtmlRenderer& self = HtmlRenderer::instance();
                {
                    std::lock_guard<std::mutex> lock(self.m_jsMutex);
                    if (resp.success && !resp.body.empty() &&
                        resp.body.size() <= kMaxJsBytes) {
                        self.m_jsReady.emplace_back(*owned, std::move(resp.body));
                    }
                    self.m_jsInFlight--;
                }
                return nullptr;
            }, payload);
            if (rc != 0) {
                delete payload;
                m_jsInFlight--;
            } else {
                pthread_detach(tid);
            }
        }
    }
    // Run arrived scripts + timers on the main thread.
    bool arrived = false;
    {
        std::lock_guard<std::mutex> lock(m_jsMutex);
        arrived = !m_jsReady.empty();
    }
    if (arrived) {
        if (!m_js || m_js->epoch() != m_jsEpoch) {
            m_js.reset(new JsEngine(this, m_jsEpoch, m_currentUrl));
        }
        if (BrowserManager::instance().settings().js) {
            std::vector<std::pair<std::string, std::string>> batch;
            {
                std::lock_guard<std::mutex> lock(m_jsMutex);
                batch.swap(m_jsReady);
            }
            for (const auto& item : batch) {
                m_js->runScript(item.second, item.first);
            }
        } else {
            std::lock_guard<std::mutex> lock(m_jsMutex);
            m_jsReady.clear();
        }
        layout();
        updateFocus();
        markDirty();
    }
    if (m_js && m_js->epoch() == m_jsEpoch) {
        if (m_js->poll(SDL_GetTicks())) {
            layout();
            updateFocus();
            markDirty();
        }
    }
}

void HtmlRenderer::appendCssForTest(const std::string& css) {
    {
        std::lock_guard<std::mutex> lock(m_cssMutex);
        if (m_styleSheets.size() >= kMaxCssSheets) return;
        m_styleSheets.push_back(css);
    }
    parseStylesheet(css);
    applyStyles();
    layout();
    updateFocus();
    markDirty();
}

// ---- P15.2: in-page find ----

static std::string foldAscii(const std::string& s) {
    std::string o = s;
    for (char& c : o) {
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    }
    return o;
}

int HtmlRenderer::findMatches(const std::string& query) {
    clearFind();
    std::string q = foldAscii(query);
    if (q.empty()) return 0;
    std::function<void(std::list<HtmlElement>&)> walk =
        [&](std::list<HtmlElement>& els) {
            for (auto& el : els) {
                if (!el.hide && el.type == HtmlElementType::TEXT && !el.text.empty() &&
                    foldAscii(el.text).find(q) != std::string::npos) {
                    m_findHits.push_back(&el);
                }
                walk(el.children);
            }
        };
    walk(m_elements);
    if (!m_findHits.empty()) {
        m_findIndex = 0;
        // Jump to first hit immediately.
        HtmlElement* el = m_findHits[0];
        m_scrollY = el->y - CONTENT_Y - CONTENT_H / 3;
        if (m_scrollY < 0) m_scrollY = 0;
        if (m_scrollY > m_maxScroll) m_scrollY = m_maxScroll;
        markDirty();
    }
    return (int)m_findHits.size();
}

bool HtmlRenderer::findNext() {
    if (m_findHits.empty()) return false;
    m_findIndex = (m_findIndex + 1) % (int)m_findHits.size();
    HtmlElement* el = m_findHits[(size_t)m_findIndex];
    m_scrollY = el->y - CONTENT_Y - CONTENT_H / 3;
    if (m_scrollY < 0) m_scrollY = 0;
    if (m_scrollY > m_maxScroll) m_scrollY = m_maxScroll;
    markDirty();
    return true;
}

void HtmlRenderer::clearFind() {
    m_findHits.clear();
    m_findIndex = -1;
    markDirty();
}

void HtmlRenderer::drawRect(int x, int y, int w, int h, SDL_Color color, bool filled) {
    if (m_renderer) {
        SDL_SetRenderDrawBlendMode(m_renderer, color.a < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
        SDL_Rect rect = {x, y, w, h};
        if (filled) {
            SDL_RenderFillRect(m_renderer, &rect);
        } else {
            SDL_RenderDrawRect(m_renderer, &rect);
        }
    }
}

void HtmlRenderer::clearTextCache() {
    for (auto& kv : m_textCache) {
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    }
    m_textCache.clear();
    m_texTick = 0;
}

void HtmlRenderer::drawText(const std::string& text, int x, int y, SDL_Color color, TTF_Font* font, bool centered, int px) {
    TTF_Font* f = px > 0 ? fontFor(px) : font;
    if (!m_renderer || !f || text.empty()) return;
    // P1: texture cache — one entry per (text, color, size). Single family,
    // so the font pointer is not part of the key (cache is cleared on
    // init/stop when fonts may change).
    std::string key;
    key.reserve(text.size() + 7);
    key.append(text);
    key.push_back('\x01');
    key.push_back((char)color.r);
    key.push_back((char)color.g);
    key.push_back((char)color.b);
    key.push_back((char)color.a);
    key.push_back((char)(px & 0xFF));
    key.push_back((char)((px >> 8) & 0xFF));
    auto it = m_textCache.find(key);
    if (it == m_textCache.end()) {
        if (m_textCache.size() >= kMaxCachedTexts) {
            // Simple cap: drop everything and start over. Page text is
            // mostly static, so the working set re-populates in ~1 frame.
            clearTextCache();
        }
        SDL_Surface* surface = TTF_RenderUTF8_Blended(f, text.c_str(), color);
        if (!surface) return;
        SDL_Texture* texture = SDL_CreateTextureFromSurface(m_renderer, surface);
        int tw = surface->w, th = surface->h;
        SDL_FreeSurface(surface);
        if (!texture) return;
        CachedText ct;
        ct.tex = texture;
        ct.w = tw;
        ct.h = th;
        ct.lastUsed = ++m_texTick;
        it = m_textCache.emplace(std::move(key), ct).first;
    } else {
        it->second.lastUsed = ++m_texTick;
    }
    int tw = it->second.w, th = it->second.h;
    int drawX = centered ? x - tw / 2 : x;
    SDL_Rect dst = {drawX, y, tw, th};
    SDL_RenderCopy(m_renderer, it->second.tex, nullptr, &dst);
}

bool HtmlRenderer::drawBgImageBox(const std::string& url, int x, int y, int w, int h) {
    // P13b: decorative background — stretched, no aspect keep, skipped when
    // missing/failed. Returns false so the caller falls back to bg color.
    if (url.empty() || w <= 0 || h <= 0 || !m_renderer) return false;
    auto it = m_imgTex.find(url);
    if (it == m_imgTex.end() || !it->second.tex) return false;
    SDL_Rect dst = {x, y, w, h};
    SDL_RenderCopy(m_renderer, it->second.tex, nullptr, &dst);
    return true;
}

// ---- P5/P9c: async images ----

std::string HtmlRenderer::resolveUrl(const std::string& ref) const {
    if (ref.empty()) return std::string();
    if (ref.compare(0, 7, "http://") == 0 || ref.compare(0, 8, "https://") == 0) {
        return ref;
    }
    if (ref.compare(0, 2, "//") == 0) {
        std::string scheme = "https:";
        size_t se = m_currentUrl.find("://");
        if (se != std::string::npos) scheme = m_currentUrl.substr(0, se + 1);
        return scheme + ref;
    }
    std::string base = m_currentUrl;
    size_t se = base.find("://");
    std::string origin;
    if (se != std::string::npos) {
        size_t ps = base.find('/', se + 3);
        origin = (ps == std::string::npos) ? base : base.substr(0, ps);
    }
    if (!ref.empty() && ref[0] == '/') return origin + ref;
    size_t ls = base.rfind('/');
    std::string dir = (ls == std::string::npos) ? origin : base.substr(0, ls + 1);
    return dir + ref;
}

void HtmlRenderer::downscaleSurface(SDL_Surface*& surf, int maxW, int maxH) {
    if (!surf || (surf->w <= maxW && surf->h <= maxH)) return;
    double f = std::min((double)maxW / surf->w, (double)maxH / surf->h);
    int dw = std::max(1, (int)(surf->w * f));
    int dh = std::max(1, (int)(surf->h * f));
    SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, dw, dh, 32, SDL_PIXELFORMAT_RGBA32);
    if (!dst) return;
    if (SDL_BlitScaled(surf, nullptr, dst, nullptr) != 0) {
        SDL_FreeSurface(dst);
        return;
    }
    SDL_FreeSurface(surf);
    surf = dst;
}

void HtmlRenderer::ensureImage(const std::string& url) {
    // Settings: images off means no fetch, no space, no placeholder.
    if (url.empty() || !m_renderer || !imageUrlSupported(url)) return;
    if (!BrowserManager::instance().settings().images) return;
    {
        std::lock_guard<std::mutex> lock(m_imgMutex);
        if (m_imgTex.count(url) > 0) return;
        if (m_imgFailed.count(url) > 0) return;  // P9c: never refetch fails
        for (const auto& w : m_imgWaiting) {
            if (w == url) return;
        }
        for (const auto& r : m_imgReady) {
            if (r.url == url) return;
        }
        if (m_imgInFlight >= kMaxImgInFlight) {
            m_imgWaiting.push_back(url);
            return;
        }
        m_imgInFlight++;
    }
    startImageWorker(url);
}

bool HtmlRenderer::imageUrlSupported(const std::string& url) {
    // Strip query, compare extension case-insensitively.
    std::string path = url;
    size_t q = path.find('?');
    if (q != std::string::npos) path.resize(q);
    size_t h = path.find('#');
    if (h != std::string::npos) path.resize(h);
    if (path.size() >= 4) {
        std::string ext = path.substr(path.size() - 4);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        // SVG is decoded by the NanoSVG path when the svg setting is on;
        // the worker checks content + setting, so let it through to fetch.
        if (ext == ".svg") return true;
    }
    return true;
}

void HtmlRenderer::markImageFailed(const std::string& url) {
    std::lock_guard<std::mutex> lock(m_imgMutex);
    if (m_imgFailed.size() >= kMaxImgFailed) {
        m_imgFailed.clear();
    }
    m_imgFailed[url] = true;
}

struct ImgWorkerTask {
    std::string url;
    std::string referer;
    uint64_t epoch = 0;
};

void HtmlRenderer::startImageWorker(const std::string& url) {
    ImgWorkerTask* task = new ImgWorkerTask{url, m_currentUrl, m_imgEpoch};
    pthread_t tid;
    int rc = pthread_create(&tid, nullptr, [](void* arg) -> void* {
        std::unique_ptr<ImgWorkerTask> task(static_cast<ImgWorkerTask*>(arg));
        std::string url = task->url;
        std::vector<std::string> imgHeaders = HtmlRenderer::getBrowserHeaders(task->referer);
        imgHeaders.push_back("Accept: image/jpeg,image/png,image/*;q=0.8,*/*;q=0.5");
        HttpResponse resp = HttpClient::instance().get(url, imgHeaders, 15);
        SDL_Surface* surf = nullptr;
        // SVG raster (setting svg on): NanoSVG in-house, else skip without
        // burning a negative-cache entry meant for real failures.
        bool svgDone = false;
        std::string lowUrl = url;
        std::transform(lowUrl.begin(), lowUrl.end(), lowUrl.begin(), ::tolower);
        bool looksSvg = lowUrl.size() >= 4 &&
                        lowUrl.compare(lowUrl.size() - 4, 4, ".svg") == 0;
        if (!looksSvg && resp.success && resp.body.size() > 5 &&
            resp.body.compare(0, 5, "<?xml") == 0 &&
            resp.body.find("<svg") != std::string::npos) {
            looksSvg = true;
        }
        if (looksSvg) {
            svgDone = true;
            if (BrowserManager::instance().settings().svg && resp.success &&
                !resp.body.empty()) {
                std::vector<uint8_t> rgba;
                int sw = 0, sh = 0;
                if (svgRasterize(reinterpret_cast<const uint8_t*>(resp.body.data()),
                                 resp.body.size(), 480, rgba, sw, sh) &&
                    !rgba.empty()) {
                    surf = SDL_CreateRGBSurfaceWithFormatFrom(
                        rgba.data(), sw, sh, 32, sw * 4, SDL_PIXELFORMAT_RGBA32);
                    if (surf) {
                        SDL_Surface* owned = SDL_ConvertSurfaceFormat(
                            surf, SDL_PIXELFORMAT_RGBA32, 0);
                        SDL_FreeSurface(surf);
                        surf = owned;
                    }
                    if (surf) {
                        Logger::info("HtmlRenderer: svg ok " + url.substr(0, 80));
                    }
                }
            }
        }
        // GIF animation (setting gifAnim on): full nsgif decode into the
        // gif queue; otherwise first frame via SDL_image like statics.
        bool gifDone = false;
        bool gifOk = false;
        if (!svgDone && resp.success && resp.body.size() > 6 &&
            resp.body.compare(0, 6, "GIF8") == 0) {
            bool wantAnim = BrowserManager::instance().settings().gifAnim;
            std::vector<HtmlRenderer::GifFrameSurf> frames;
            if (HtmlRenderer::decodeGifFrames(
                    reinterpret_cast<const uint8_t*>(resp.body.data()),
                    resp.body.size(), frames, CONTENT_W - PADDING * 2,
                    HtmlRenderer::kImgMaxH) && !frames.empty()) {
                if (wantAnim && frames.size() > 1) {
                    gifDone = true;
                    HtmlRenderer& self = HtmlRenderer::instance();
                    std::lock_guard<std::mutex> lock(self.m_imgMutex);
                    HtmlRenderer::ReadyGif rg;
                    rg.url = url;
                    rg.frames = std::move(frames);
                    self.m_gifReady.push_back(std::move(rg));
                    Logger::info("HtmlRenderer: gif anim ok " + url.substr(0, 80));
                    gifOk = true;
                } else {
                    // gifAnim is OFF (or single-frame GIF): take frame 0 as a static surface!
                    gifDone = true;
                    surf = frames[0].surf;
                    frames[0].surf = nullptr;
                    for (size_t fi = 1; fi < frames.size(); fi++) {
                        if (frames[fi].surf) SDL_FreeSurface(frames[fi].surf);
                    }
                    frames.clear();
                    Logger::info("HtmlRenderer: gif static frame 0 ok " + url.substr(0, 80));
                }
            }
        }
        if (!gifDone) {
            if (resp.success && !resp.body.empty()) {
                SDL_RWops* rw = SDL_RWFromConstMem(resp.body.data(), (int)resp.body.size());
                if (rw) {
                    surf = IMG_Load_RW(rw, 1);
                }
            }
            // P10: one log line per image outcome (fetch vs decode faults).
            if (surf) {
                Logger::info("HtmlRenderer: image ok " + url.substr(0, 80));
            } else if (resp.success) {
                const char* err = IMG_GetError();
                Logger::warn("HtmlRenderer: image decode failed " + url.substr(0, 80) +
                             " (" + std::to_string(resp.body.size()) + " bytes): " +
                             (err ? err : "unknown"));
            } else if (!resp.success) {
                Logger::warn("HtmlRenderer: image fetch failed " + url.substr(0, 80));
            }
        }
        if (surf) {
            HtmlRenderer::downscaleSurface(surf, CONTENT_W - PADDING * 2, HtmlRenderer::kImgMaxH);
        }
        HtmlRenderer& self = HtmlRenderer::instance();
        std::string next;
        {
            std::lock_guard<std::mutex> lock(self.m_imgMutex);
            if (task->epoch == self.m_imgEpoch) {
                if (surf) {
                    self.m_imgReady.push_back(ReadyImage{url, surf});
                    surf = nullptr;  // ownership transferred to m_imgReady
                } else if (!gifOk) {
                    if (self.m_imgFailed.size() >= kMaxImgFailed) {
                        self.m_imgFailed.clear();
                    }
                    self.m_imgFailed[url] = true;
                }
            }
            self.m_imgInFlight--;
            if (self.m_imgInFlight < 0) self.m_imgInFlight = 0;
            if (task->epoch == self.m_imgEpoch && !self.m_imgWaiting.empty() && self.m_imgInFlight < kMaxImgInFlight) {
                next = self.m_imgWaiting.front();
                self.m_imgWaiting.pop_front();
                self.m_imgInFlight++;
            }
        }
        if (surf) SDL_FreeSurface(surf);
        if (!next.empty()) self.startImageWorker(next);
        return nullptr;
    }, task);
    if (rc != 0) {
        delete task;
        std::lock_guard<std::mutex> lock(m_imgMutex);
        m_imgInFlight--;
        if (m_imgInFlight < 0) m_imgInFlight = 0;
    } else {
        pthread_detach(tid);
    }
}

void HtmlRenderer::pumpImages() {
    if (!m_renderer) return;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_imgMutex);
        for (auto& r : m_imgReady) {
            if (m_imgTex.count(r.url) == 0 && r.surface) {
                SDL_Texture* tex = SDL_CreateTextureFromSurface(m_renderer, r.surface);
                if (tex) {
                    int tw = r.surface->w, th = r.surface->h;
                    if (m_imgTex.size() >= kMaxImgCached) {
                        // Evict one arbitrary entry (FIFO-ish via begin()).
                        auto it = m_imgTex.begin();
                        if (it->second.tex) SDL_DestroyTexture(it->second.tex);
                        m_imgTex.erase(it);
                    }
                    m_imgTex[r.url] = ImageTex{tex, tw, th};
                    changed = true;
                }
            }
            if (r.surface) SDL_FreeSurface(r.surface);
        }
        m_imgReady.clear();
        // GIF animations: upload every frame, record delays.
        for (auto& g : m_gifReady) {
            auto it = m_gifAnims.find(g.url);
            if (it == m_gifAnims.end() && !g.frames.empty()) {
                GifAnim anim;
                anim.w = g.frames[0].surf ? g.frames[0].surf->w : 0;
                anim.h = g.frames[0].surf ? g.frames[0].surf->h : 0;
                bool allOk = anim.w > 0 && anim.h > 0;
                for (auto& f : g.frames) {
                    SDL_Texture* tex = nullptr;
                    if (f.surf) {
                        tex = SDL_CreateTextureFromSurface(m_renderer, f.surf);
                    }
                    if (!tex) {
                        allOk = false;
                        break;
                    }
                    anim.frames.push_back({tex, f.delayCs});
                    anim.totalCs += (f.delayCs == UINT32_MAX) ? 0 : f.delayCs;
                }
                for (auto& f : g.frames) {
                    if (f.surf) SDL_FreeSurface(f.surf);
                }
                g.frames.clear();
                if (allOk && anim.frames.size() > 1) {
                    if (anim.totalCs == 0) anim.totalCs = (uint32_t)anim.frames.size() * 10;
                    if (m_gifAnims.size() >= kMaxGifCached) {
                        auto ev = m_gifAnims.begin();
                        for (auto& ef : ev->second.frames) {
                            if (ef.tex) SDL_DestroyTexture(ef.tex);
                        }
                        m_gifAnims.erase(ev);
                    }
                    m_gifAnims[g.url] = std::move(anim);
                    changed = true;
                } else {
                    for (auto& ef : anim.frames) {
                        if (ef.tex) SDL_DestroyTexture(ef.tex);
                    }
                }
            } else {
                for (auto& f : g.frames) {
                    if (f.surf) SDL_FreeSurface(f.surf);
                }
                g.frames.clear();
            }
        }
        m_gifReady.clear();
    }
    if (changed) {
        layout();  // adopt real image dimensions, then redraw
        markDirty();
    }
}

void HtmlRenderer::queueImages() {
    // Walk the fresh tree once, starting fetches for unseen IMAGE nodes.
    // P13b: background-images resolve here (base URL is final by now).
    std::function<void(std::list<HtmlElement>&)> walk =
        [&](std::list<HtmlElement>& els) {
            for (auto& el : els) {
                if (el.type == HtmlElementType::IMAGE && !el.src.empty()) {
                    ensureImage(resolveUrl(el.src));
                }
                if (el.type == HtmlElementType::MEDIA && !el.poster.empty()) {
                    ensureImage(resolveUrl(el.poster));
                }
                if (!el.bgImage.empty()) {
                    if (el.bgImage.find("://") == std::string::npos) {
                        el.bgImage = resolveUrl(el.bgImage);
                    }
                    if (!el.bgImage.empty()) ensureImage(el.bgImage);
                }
                walk(el.children);
            }
        };
    walk(m_elements);
}

void HtmlRenderer::clearImages() {
    std::lock_guard<std::mutex> lock(m_imgMutex);
    m_imgEpoch++;  // Invalidate any background image fetch threads from previous page
    for (auto& kv : m_imgTex) {
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    }
    m_imgTex.clear();
    for (auto& kv : m_gifAnims) {
        for (auto& f : kv.second.frames) {
            if (f.tex) SDL_DestroyTexture(f.tex);
        }
    }
    m_gifAnims.clear();
    m_imgFailed.clear();
    for (auto& r : m_imgReady) {
        if (r.surface) SDL_FreeSurface(r.surface);
    }
    m_imgReady.clear();
    for (auto& g : m_gifReady) {
        for (auto& f : g.frames) {
            if (f.surf) SDL_FreeSurface(f.surf);
        }
    }
    m_gifReady.clear();
    m_imgWaiting.clear();
    // Do not force m_imgInFlight = 0 so running workers cleanly decrement down to 0
}

size_t HtmlRenderer::imagePendingCount() {
    std::lock_guard<std::mutex> lock(m_imgMutex);
    return m_imgWaiting.size() + (size_t)m_imgInFlight + m_imgReady.size();
}

bool HtmlRenderer::decodeImageForTest(const std::string& url,
                                     const std::vector<uint8_t>& bytes) {
    if (url.empty() || bytes.empty()) return false;
    SDL_RWops* rw = SDL_RWFromConstMem(bytes.data(), (int)bytes.size());
    if (!rw) return false;
    SDL_Surface* surf = IMG_Load_RW(rw, 1);
    if (!surf) return false;
    downscaleSurface(surf, CONTENT_W - PADDING * 2, kImgMaxH);
    {
        std::lock_guard<std::mutex> lock(m_imgMutex);
        auto it = m_imgTex.find(url);
        if (it != m_imgTex.end() && it->second.tex) SDL_DestroyTexture(it->second.tex);
        SDL_Texture* tex = nullptr;
        if (m_renderer) tex = SDL_CreateTextureFromSurface(m_renderer, surf);
        // Headless (null renderer): record dimensions with null texture so
        // layout sizing is still testable.
        int tw = surf->w, th = surf->h;
        m_imgTex[url] = ImageTex{tex, tw, th};
    }
    SDL_FreeSurface(surf);
    return true;
}

// ---- GIF animation via vendored nsgif (setting gifAnim) ----

namespace {
struct NsgifBitmap {
    int w = 0;
    int h = 0;
    // uint32_t storage: nsgif writes 32-bit pixels and ARM strict-alignment
    // builds fault on unaligned word access (SIGBUS on image-heavy pages).
    std::vector<uint32_t> pixels;  // RGBA8888, row-major, no padding
};

nsgif_bitmap_t* gifCreate(int width, int height) {
    if (width <= 0 || height <= 0 || width > 2048 || height > 2048) return nullptr;
    NsgifBitmap* b = new (std::nothrow) NsgifBitmap();
    if (!b) return nullptr;
    b->w = width;
    b->h = height;
    try {
        b->pixels.assign((size_t)width * (size_t)height, 0);
    } catch (...) {
        delete b;
        return nullptr;
    }
    return b;
}

void gifDestroy(nsgif_bitmap_t* bitmap) {
    delete static_cast<NsgifBitmap*>(bitmap);
}

uint8_t* gifBuffer(nsgif_bitmap_t* bitmap) {
    NsgifBitmap* b = static_cast<NsgifBitmap*>(bitmap);
    return b ? reinterpret_cast<uint8_t*>(b->pixels.data()) : nullptr;
}

const nsgif_bitmap_cb_vt kGifVt = {gifCreate, gifDestroy, gifBuffer,
                                   nullptr, nullptr, nullptr};
}  // namespace

// Decode all unique frames (cap 10). Surfaces are RGBA32, pre-scaled.
bool HtmlRenderer::decodeGifFrames(const uint8_t* data, size_t len,
                                  std::vector<HtmlRenderer::GifFrameSurf>& out,
                                  int maxW, int maxH) {
    out.clear();
    if (!data || len < 6) return false;
    nsgif_t* gif = nullptr;
    if (nsgif_create(&kGifVt, NSGIF_BITMAP_FMT_R8G8B8A8, &gif) != NSGIF_OK || !gif) {
        return false;
    }
    bool ok = false;
    if (nsgif_data_scan(gif, len, data) == NSGIF_OK) {
        nsgif_data_complete(gif);
        uint32_t total = 0;
        bool seen[256] = {false};  // nsgif loops forever — keep unique frames
        for (int i = 0; i < 10; i++) {
            nsgif_rect_t area;
            uint32_t delay = 0, fnew = 0;
            nsgif_error err = nsgif_frame_prepare(gif, &area, &delay, &fnew);
            if (err != NSGIF_OK) break;
            if (fnew < 256 && seen[fnew]) break;
            if (fnew < 256) seen[fnew] = true;
            nsgif_bitmap_t* bm = nullptr;
            if (nsgif_frame_decode(gif, fnew, &bm) != NSGIF_OK || !bm) break;
            NsgifBitmap* b = static_cast<NsgifBitmap*>(bm);
            if (b->w <= 0 || b->h <= 0) break;
            // Clamp wild delays (hyper-flash ads) to [4cs, 10s].
            if (delay < 4 && delay != NSGIF_INFINITE) delay = 4;
            if (delay > 1000 && delay != NSGIF_INFINITE) delay = 1000;
            SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
                reinterpret_cast<uint8_t*>(b->pixels.data()), b->w, b->h, 32,
                b->w * 4, SDL_PIXELFORMAT_RGBA32);
            if (!surf) break;
            SDL_Surface* owned = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_RGBA32, 0);
            SDL_FreeSurface(surf);
            if (!owned) break;
            SDL_Surface* scaled = owned;
            downscaleSurface(scaled, maxW, maxH);
            out.push_back({scaled, delay});
            total += (delay == NSGIF_INFINITE) ? 0 : delay;
            ok = true;
            if (delay == NSGIF_INFINITE) break;
            (void)total;
        }
    }
    nsgif_destroy(gif);
    if (!ok) {
        for (auto& f : out) {
            if (f.surf) SDL_FreeSurface(f.surf);
        }
        out.clear();
    }
    return ok && !out.empty();
}

bool HtmlRenderer::decodeGifForTest(const std::string& url,
                                    const std::vector<uint8_t>& bytes) {
    if (url.empty() || bytes.empty()) return false;
    std::vector<GifFrameSurf> frames;
    if (!decodeGifFrames(bytes.data(), bytes.size(), frames,
                         CONTENT_W - PADDING * 2, kImgMaxH) ||
        frames.empty()) {
        return false;
    }
    GifAnim anim;
    anim.w = frames[0].surf ? frames[0].surf->w : 0;
    anim.h = frames[0].surf ? frames[0].surf->h : 0;
    for (auto& f : frames) {
        SDL_Texture* tex = nullptr;
        if (m_renderer && f.surf) {
            tex = SDL_CreateTextureFromSurface(m_renderer, f.surf);
        }
        anim.frames.push_back({tex, f.delayCs});
        if (f.delayCs != UINT32_MAX) anim.totalCs += f.delayCs;
        if (f.surf) SDL_FreeSurface(f.surf);
    }
    if (anim.totalCs == 0) anim.totalCs = (uint32_t)anim.frames.size() * 10;
    std::lock_guard<std::mutex> lock(m_imgMutex);
    m_gifAnims[url] = std::move(anim);
    return true;
}

} // namespace RomCloud
