#include "HtmlRenderer.h"
#include "../logging/Logger.h"
#include "../network/HttpClient.h"
#include "BrowserManager.h"
#include <cstring>
#include <functional>
#include <algorithm>
#include <unistd.h>  // access()
#include <pthread.h>
#include <unordered_map>

// Button definitions (matching InputManager::Button)
#define BTN_UP 0
#define BTN_DOWN 1
#define BTN_A 4
#define BTN_B 5

namespace RomCloud {

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
                   el.type == HtmlElementType::INPUT_PASSWORD) {
            if (!el.name.empty()) {
                formData[el.name] = el.value;
            }
        }
        collectFormInputs(el.children, action, formData);
    }
}

HtmlRenderer& HtmlRenderer::instance() {
    static HtmlRenderer inst;
    return inst;
}

void HtmlRenderer::init(SDL_Renderer* renderer, TTF_Font* font) {
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
}

bool HtmlRenderer::loadHtml(const std::string& html) {
    m_elements.clear();
    m_focusable.clear();
    parseHtml(html);
    layout();
    updateFocus();
    return true;
}

bool HtmlRenderer::loadUrl(const std::string& url) {
    // Add URL to back/forward history, then defer the actual fetch.
    // Phase 1 — audit C4 (history duplication): goBack() previously called
    // loadUrl() directly, which pushed the historical URL back onto the
    // history stack and grew it indefinitely when navigating back and forth.
    if (m_historyPos < m_history.size()) {
        // We are inside a forward history; truncate so the new entry
        // becomes the new tip.
        m_history.resize(m_historyPos);
    }
    m_history.push_back(url);
    m_historyPos = m_history.size();

    return loadUrlNoHistory(url);
}

bool HtmlRenderer::loadUrlNoHistory(const std::string& url) {
    // Internal: fetch a URL without modifying history. Used by goBack()
    // and refresh() so history stays consistent (audit C4).
    m_loading = true;
    m_error = false;
    m_errorMsg.clear();
    m_currentUrl = url;

    // Heap-allocate the fetch payload so each worker thread has its own
    // copy of the URL (Phase 1 — audit C1: previous code used a static
    // `s_url` that got overwritten if loadUrl() was called twice in
    // quick succession, making both fetches use the second URL).
    PendingFetch* payload = new PendingFetch();
    payload->url = url;

    pthread_t tid;
    int rc = pthread_create(&tid, nullptr, [](void* arg) -> void* {
        std::unique_ptr<PendingFetch> owned(static_cast<PendingFetch*>(arg));
        HttpResponse resp = HttpClient::instance().get(owned->url, {}, 15);
        owned->success = resp.success;
        owned->body = std::move(resp.body);
        owned->error = std::move(resp.error);

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
        m_loading = false;
        loadHtml(result->body);
        BrowserManager::instance().setState(BrowserState::RENDERING);
    }
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
    size_t nameStart = pos;
    while (pos < html.size() && !std::isspace(static_cast<unsigned char>(html[pos])) && html[pos] != '>' && html[pos] != '/') pos++;
    tagName = html.substr(nameStart, pos - nameStart);
    std::transform(tagName.begin(), tagName.end(), tagName.begin(), ::tolower);
    while (pos < html.size() && html[pos] != '>') {
        while (pos < html.size() && std::isspace(static_cast<unsigned char>(html[pos]))) pos++;
        if (pos >= html.size() || html[pos] == '>') break;
        size_t attrStart = pos;
        while (pos < html.size() && !std::isspace(static_cast<unsigned char>(html[pos])) && html[pos] != '=' && html[pos] != '>' && html[pos] != '/') pos++;
        std::string attrName = html.substr(attrStart, pos - attrStart);
        while (pos < html.size() && std::isspace(static_cast<unsigned char>(html[pos]))) pos++;
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
            std::string value = html.substr(valueStart, pos - valueStart);
            if (quote && pos < html.size()) pos++;
            if (!attrName.empty() && !value.empty()) {
                if (!attributes.empty()) attributes += " ";
                attributes += attrName + "=\"" + value + "\"";
            }
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
    struct { const char* entity; char ch; } entities[] = {
        {"&nbsp;", ' '}, {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
        {"&quot;", '"'}, {"&apos;", '\''}, {"&#39;", '\''}
    };
    for (auto& e : entities) {
        size_t pos = 0;
        while ((pos = result.find(e.entity, pos)) != std::string::npos) {
            result[pos] = e.ch;
            result.erase(pos + 1, strlen(e.entity) - 1);
        }
    }
    return result;
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

    while (pos < html.size()) {
        // Phase 2 — audit M8b: skip HTML comments `<!-- ... -->`.
        if (html.compare(pos, 4, "<!--") == 0) {
            size_t end = html.find("-->", pos + 4);
            if (end == std::string::npos) pos = html.size();
            else pos = end + 3;
            continue;
        }
        if (html[pos] == '<') {
            std::string rawTag = extractTag(html, pos, tagName, attrs);
            if (tagName.empty()) continue;

            // Phase 2 — audit M8a: <script> and <style> blocks. The opening
            // tag is skipped (already filtered below), but we also need to
            // skip the content until the matching closing tag — otherwise
            // the parser would tokenize JS/CSS as text and pollute the DOM.
            if (tagName == "script" || tagName == "style") {
                std::string endTag = "</" + tagName + ">";
                size_t end = html.find(endTag, pos);
                if (end == std::string::npos) pos = html.size();
                else pos = end + endTag.size();
                continue;
            }

            if (rawTag == "/" || tagName == "br" || tagName == "hr" || tagName == "img" || tagName == "input") {
                HtmlElement el;
                el.type = HtmlElementType::TEXT;
                if (tagName == "br") {
                    el.type = HtmlElementType::LINE_BREAK;
                    if (current) current->children.push_back(el);
                    else m_elements.push_back(el);
                } else if (tagName == "input") {
                    std::string type = getAttribute(attrs, "type");
                    if (type.empty()) type = "text";
                    if (type == "text") el.type = HtmlElementType::INPUT_TEXT;
                    else if (type == "password") el.type = HtmlElementType::INPUT_PASSWORD;
                    else if (type == "checkbox") el.type = HtmlElementType::INPUT_CHECKBOX;
                    else el.type = HtmlElementType::INPUT_TEXT;
                    el.name = getAttribute(attrs, "name");
                    el.value = getAttribute(attrs, "value");
                    el.placeholder = getAttribute(attrs, "placeholder");
                    el.tabIndex = tabIndex++;
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
                    m_focusable.push_back(added);
                }
                continue;
            }

            // Handle </form> before the opening-tag filter below, since
            // </form> IS in the filter (we want to pop formStack even when
            // we otherwise ignore the tag for tree building).
            if (tagName == "/form") {
                if (!formStack.empty()) formStack.pop_back();
            }

            // Exclude /form from tree insertion so it doesn't leak into
            // m_elements as a phantom TEXT node. The element-stack pop
            // below still runs for /form via the `tagName[0] == '/'`
            // branch (we don't exclude /form there), keeping the stack
            // balanced.
            if (tagName != "/form" &&
                tagName != "/html" && tagName != "/head" && tagName != "/body" &&
                tagName != "/script" && tagName != "/style" && tagName != "/meta" &&
                tagName != "/title" && tagName != "/link") {

                HtmlElement el;
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
                } else {
                    el.type = HtmlElementType::TEXT;
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
                current = added;

                // Phase 3 — audit M5: track open <form> ancestors so
                // subsequent inputs/buttons/divs inherit the form context.
                if (added->type == HtmlElementType::FORM) {
                    formStack.push_back(added);
                }
            }

            if (tagName[0] == '/') {
                if (!stack.empty()) {
                    stack.pop_back();
                    current = stack.empty() ? nullptr : stack.back();
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
}

int HtmlRenderer::textWidth(const std::string& text) {
    if (!m_font || text.empty()) return 0;
    int w = 0;
    TTF_SizeUTF8(m_font, text.c_str(), &w, nullptr);
    return w;
}

void HtmlRenderer::layout() {
    m_focusable.clear();
    // Phase 2 — audit M1: layout begins at CONTENT_Y (just below the header
    // chrome) instead of PADDING. m_maxScroll is bounded by CONTENT_H so
    // pageUp/pageDown never reveal the empty footer area.
    m_maxScroll = CONTENT_Y + PADDING;
    int y = CONTENT_Y + PADDING;
    for (auto& el : m_elements) {
        layoutElement(el, y, PADDING, CONTENT_W);
    }
    m_maxScroll = y + PADDING - (CONTENT_Y + CONTENT_H);
    if (m_maxScroll < 0) m_maxScroll = 0;
}

void HtmlRenderer::layoutElement(HtmlElement& el, int& y, int x, int width) {
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
        for (auto& child : el.children) {
            layoutElement(child, y, x + PADDING, width - PADDING * 2);
        }
        if (el.text.empty()) return;
        el.x = x; el.y = y - m_scrollY;
        el.width = textWidth(el.text);
        el.height = LINE_HEIGHT;
        y += LINE_HEIGHT;
        return;
    }
    if (el.type == HtmlElementType::LINK || el.type == HtmlElementType::BUTTON ||
        el.type == HtmlElementType::INPUT_TEXT || el.type == HtmlElementType::INPUT_PASSWORD ||
        el.type == HtmlElementType::INPUT_CHECKBOX) {
        std::string displayText = el.text;
        if (el.type == HtmlElementType::INPUT_TEXT || el.type == HtmlElementType::INPUT_PASSWORD) {
            displayText = el.value.empty() ? el.placeholder : el.value;
            if (el.type == HtmlElementType::INPUT_PASSWORD) {
                displayText = std::string(displayText.size(), '*');
            }
        }
        el.x = x; el.y = y - m_scrollY;
        el.width = textWidth(displayText) + PADDING * 2;
        el.height = LINE_HEIGHT + 8;
        if (el.type == HtmlElementType::INPUT_TEXT || el.type == HtmlElementType::INPUT_PASSWORD) {
            el.width = std::min(el.width, (int)CONTENT_W - PADDING * 2);
        }
        m_focusable.push_back(&el);
        y += LINE_HEIGHT + 8;
        return;
    }
    if (el.type == HtmlElementType::IMAGE) {
        el.x = x; el.y = y - m_scrollY;
        el.width = 200; el.height = 150;
        y += 160;
        return;
    }
    int childY = y;
    for (auto& child : el.children) {
        layoutElement(child, childY, x + PADDING, width - PADDING * 2);
    }
    if (!el.text.empty()) {
        el.x = x; el.y = y - m_scrollY;
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

void HtmlRenderer::pageUp(int delta) {
    m_scrollY -= delta;
    if (m_scrollY < 0) m_scrollY = 0;
    // Re-layout so el.y coordinates reflect the new scroll offset, then
    // re-anchor the focused row so it remains visible after the page jumps.
    layout();
    ensureFocusVisible();
}

void HtmlRenderer::pageDown(int delta) {
    m_scrollY += delta;
    if (m_scrollY > m_maxScroll) m_scrollY = m_maxScroll;
    layout();
    ensureFocusVisible();
}

void HtmlRenderer::ensureFocusVisible() {
    if (m_inputIndex < 0 || m_inputIndex >= (int)m_focusable.size()) return;
    const HtmlElement* el = m_focusable[m_inputIndex];
    int top = CONTENT_Y;
    int bottom = CONTENT_Y + CONTENT_H;

    // el.y is already in screen-space (layout sets el.y = layout_y - m_scrollY).
    // - If el.y < top: the element sits above the viewport — decrease
    //   m_scrollY so the content shifts down (user-perspective: scroll up).
    // - If el.y + el.height > bottom: element sits below the viewport —
    //   increase m_scrollY so the content shifts up (user-perspective:
    //   scroll down).
    if (el->y < top) {
        m_scrollY -= (top - el->y);
    } else if (el->y + el->height > bottom) {
        m_scrollY += (el->y + el->height - bottom);
    }
    if (m_scrollY < 0) m_scrollY = 0;
    if (m_scrollY > m_maxScroll) m_scrollY = m_maxScroll;
    // Re-derive el.y with the new scroll offset so renderElement picks the
    // right pixel row.
    layout();
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
            focusPrev();
            break;
        case BTN_DOWN:
            focusNext();
            break;
        case BTN_A:
            if (m_inputIndex >= 0 && m_inputIndex < (int)m_focusable.size()) {
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
                    // Both explicit <button type="submit"> and the default
                    // (button without an explicit type) submit. We just
                    // need the button to have a form context (formAction
                    // set during parseHtml).
                    if (!el->formAction.empty()) {
                        submitForm(*el);
                    }
                }
            }
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
        }
    }
}

void HtmlRenderer::deleteCharacter() {
    if (m_inputIndex >= 0 && m_inputIndex < (int)m_focusable.size()) {
        HtmlElement* el = m_focusable[m_inputIndex];
        if (el->type == HtmlElementType::INPUT_TEXT || el->type == HtmlElementType::INPUT_PASSWORD) {
            if (!el->value.empty()) el->value.pop_back();
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
    constexpr size_t kMaxFieldLen = 256;
    el->value = v.size() > kMaxFieldLen ? v.substr(0, kMaxFieldLen) : v;
}

int HtmlRenderer::focusedInputMaxLen() const {
    // Phase 1 — audit P1-2c: cap 256 cho mọi field. Có thể đọc `maxlength`
    // attribute sau nếu cần — cho giờ constant là đủ vì không có layout
    // test nào dùng field > 64 char.
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

    // 4. Dispatch.
    HttpResponse resp;
    if (method == "post") {
        resp = HttpClient::instance().postForm(resolvedAction, formData);
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
        resp = HttpClient::instance().get(url);
    }

    if (!resp.success) {
        // Mirror the openUrl() error path: show the error in the page area
        // and reset m_loading so the spinner doesn't hang.
        Logger::error("HtmlRenderer::submitForm: HTTP failed");
        m_error = true;
        m_errorMsg = resp.error.empty() ? "Form submit failed" : resp.error;
        m_loading = false;
        std::string html = "<html><body>"
            "<h1>Khong the gui form</h1>"
            "<p>Loi: " + m_errorMsg + "</p>"
            "</body></html>";
        loadHtml(html);
        return;
    }
    // 5. Render the response body in place of the current page.
    m_loading = false;
    loadHtml(resp.body);
}

void HtmlRenderer::render() {
    // Phase 2 — audit M1: only paint the background inside the content area
    // (between the 64 px header and the 53 px footer). The UIManager draws
    // the header and footer separately, so filling the full screen would
    // briefly flash black across them every frame.
    static int renderCallCount = 0;
    (void)renderCallCount;  // diagnostic counter, kept for future debugging
    drawRect(0, CONTENT_Y, SCREEN_W, CONTENT_H, {15, 15, 15, 255}, true);
    for (auto& el : m_elements) {
        renderElement(el);
    }
}

void HtmlRenderer::renderElement(const HtmlElement& el) {
    // Phase 2 — audit M1: clip to the content area, not the full screen.
    // Elements that fall under the header/footer chrome are skipped.
    // NOTE: do NOT early-return here. The tree is built as nested
    // wrappers (e.g. <html> → <head>/<body> → <p> → text), and the
    // layout pass only sets y/height for elements that own text.
    // Intermediate wrappers keep y=0, h=0 from the struct default —
    // if we returned on that, we'd never recurse into the children
    // that actually carry the text. So we only skip the element's OWN
    // draw call; the children always get a chance to draw.
    bool inView = (el.y + el.height >= CONTENT_Y && el.y <= CONTENT_Y + CONTENT_H);
    if (inView) {
        switch (el.type) {
        case HtmlElementType::TEXT:
            if (!el.text.empty() && m_font) {
                drawText(el.text, el.x, el.y, {200, 200, 200, 255}, m_font, false);
            }
            break;
        case HtmlElementType::LINK: {
            SDL_Color color = el.focused ? SDL_Color{0, 180, 216, 255} : SDL_Color{100, 150, 255, 255};
            if (m_font) drawText(el.text, el.x, el.y, color, m_font, false);
            break;
        }
        case HtmlElementType::INPUT_TEXT:
        case HtmlElementType::INPUT_PASSWORD: {
            SDL_Color bgColor = el.focused ? SDL_Color{30, 40, 60, 255} : SDL_Color{20, 25, 35, 255};
            SDL_Color borderColor = el.focused ? SDL_Color{0, 180, 216, 255} : SDL_Color{60, 70, 90, 255};
            SDL_Color textColor = {220, 220, 220, 255};
            drawRect(el.x, el.y, el.width, el.height, bgColor, true);
            drawRect(el.x, el.y, el.width, el.height, borderColor, false);
            std::string displayText = el.value.empty() ? el.placeholder : el.value;
            if (el.type == HtmlElementType::INPUT_PASSWORD) {
                displayText = std::string(displayText.size(), '*');
            }
            if (displayText.empty()) displayText = " ";
            SDL_Color placeholderColor = {100, 100, 100, 255};
            if (m_font) drawText(displayText, el.x + PADDING, el.y + 6,
                el.value.empty() ? placeholderColor : textColor, m_font, false);
            break;
        }
        case HtmlElementType::INPUT_CHECKBOX: {
            SDL_Color bgColor = el.focused ? SDL_Color{30, 40, 60, 255} : SDL_Color{20, 25, 35, 255};
            SDL_Color borderColor = el.focused ? SDL_Color{0, 180, 216, 255} : SDL_Color{60, 70, 90, 255};
            int boxSize = 24;
            drawRect(el.x, el.y + 4, boxSize, boxSize, bgColor, true);
            drawRect(el.x, el.y + 4, boxSize, boxSize, borderColor, false);
            if (el.checked && m_font) drawText("X", el.x, el.y + 2, SDL_Color{0, 180, 216, 255}, m_font, false);
            if (m_font) drawText(el.text, el.x + boxSize + 8, el.y + 4, {200, 200, 200, 255}, m_font, false);
            break;
        }
        case HtmlElementType::BUTTON: {
            SDL_Color bgColor = el.focused ? SDL_Color{0, 100, 150, 255} : SDL_Color{40, 50, 70, 255};
            SDL_Color borderColor = el.focused ? SDL_Color{0, 180, 216, 255} : SDL_Color{60, 80, 100, 255};
            drawRect(el.x, el.y, el.width, el.height, bgColor, true);
            drawRect(el.x, el.y, el.width, el.height, borderColor, false);
            if (m_font) drawText(el.text, el.x + el.width / 2, el.y + 8, {255, 255, 255, 255}, m_font, true);
            break;
        }
        case HtmlElementType::IMAGE:
            drawRect(el.x, el.y, el.width, el.height, {30, 30, 30, 255}, true);
            drawRect(el.x, el.y, el.width, el.height, {60, 60, 60, 255}, false);
            if (m_font) drawText("[Image]", el.x + el.width / 2, el.y + el.height / 2, {100, 100, 100, 255}, m_font, true);
            break;
        default:
            if (!el.text.empty() && m_font) {
                drawText(el.text, el.x, el.y, {180, 180, 180, 255}, m_font, false);
            }
            break;
    }
    }  // if (inView)
    for (const auto& child : el.children) {
        renderElement(child);
    }
}

void HtmlRenderer::stop() {
    m_elements.clear();
    m_focusable.clear();
    m_history.clear();
    m_historyPos = 0;
    m_currentUrl.clear();
    m_scrollY = 0;
    m_inputIndex = 0;
    m_editingText = false;
    m_loading = false;
    m_error = false;
    m_errorMsg.clear();
}

void HtmlRenderer::drawRect(int x, int y, int w, int h, SDL_Color color, bool filled) {
    if (m_renderer) {
        SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
        SDL_Rect rect = {x, y, w, h};
        SDL_RenderFillRect(m_renderer, filled ? &rect : nullptr);
        if (!filled) SDL_RenderDrawRect(m_renderer, &rect);
    }
}

void HtmlRenderer::drawText(const std::string& text, int x, int y, SDL_Color color, TTF_Font* font, bool centered) {
    if (!m_renderer || !font || text.empty()) return;
    SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!surface) return;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(m_renderer, surface);
    if (!texture) {
        SDL_FreeSurface(surface);
        return;
    }
    int tw = surface->w, th = surface->h;
    int drawX = centered ? x - tw / 2 : x;
    SDL_Rect dst = {drawX, y, tw, th};
    SDL_RenderCopy(m_renderer, texture, nullptr, &dst);
    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);
}

} // namespace RomCloud
