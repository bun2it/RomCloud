#include "NetSurfEngine.h"
#include "../BrowserManager.h"
#include "../HtmlRenderer.h"
#include "../../logging/Logger.h"
#include <pthread.h>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <cctype>
#include <chrono>

namespace RomCloud {

// P16: libcss/wapcaplet's default string-interning context is not
// thread-safe, so layout jobs are serialized even though every job owns its
// hubbub parser, DOM document and css select context.
static std::mutex s_layoutMutex;

struct LayoutPayload {
    NetSurfEngine* eng;
    std::string html;
    std::string extraCss;
    uint64_t epoch;
};

NetSurfEngine& NetSurfEngine::instance() {
    static NetSurfEngine inst;
    return inst;
}

void NetSurfEngine::init(SDL_Renderer* renderer, TTF_Font* font,
                         const std::string& fontPath, int baseFontSize) {
    NetSurfBridge::instance().init();
    NetSurfRenderer::instance().init(renderer, font, fontPath, baseFontSize);
    m_initialized = true;
    Logger::info("NetSurfEngine: initialized");
}

bool NetSurfEngine::loadHtml(const std::string& html, const std::string& url,
                             const std::string& extraCss) {
    m_rawHtml = html;
    m_currentUrl = url;
    m_editing = false;  // P17: new page drops any VK session
    // A new navigation invalidates any layout still in flight — the worker
    // epoch-checks before doing work and before handing the tree over.
    uint64_t epoch = ++m_layoutEpoch;
    {
        std::lock_guard<std::mutex> lock(m_asyncMutex);
        m_pendingTree.reset();
        m_pendingReady = false;
    }
    Logger::info("NetSurfEngine: queueing async layout (" +
                 std::to_string(html.size()) + " bytes, URL: " + url +
                 ", extraCss: " + std::to_string(extraCss.size()) + " bytes)");
    LayoutPayload* payload = new LayoutPayload{this, html, extraCss, epoch};
    pthread_t tid;
    if (pthread_create(&tid, nullptr, layoutWorker, payload) != 0) {
        delete payload;
        Logger::error("NetSurfEngine: cannot spawn layout thread");
        return false;
    }
    pthread_detach(tid);
    return true;
}

void* NetSurfEngine::layoutWorker(void* arg) {
    std::unique_ptr<LayoutPayload> p(static_cast<LayoutPayload*>(arg));
    NetSurfEngine* eng = p->eng;
    std::unique_ptr<RenderBox> tree;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(s_layoutMutex);
        // Don't burn CPU on a page the user already left.
        if (p->epoch == eng->m_layoutEpoch.load()) {
            auto t0 = std::chrono::steady_clock::now();
            // Viewport width = 984 (1024 - 40 margin)
            ok = NetSurfBridge::instance().layout(p->html, p->extraCss, 984, tree);
            ok = ok && tree != nullptr;
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            // Diagnostic: box/text counts tell a blank-but-ok layout apart
            // from a genuinely empty tree.
            int boxes = 0, textBoxes = 0, imgBoxes = 0;
            std::function<void(const RenderBox*)> count = [&](const RenderBox* b) {
                if (!b) return;
                boxes++;
                if (!b->lines.empty()) textBoxes++;
                if (b->type == RenderBoxType::IMAGE) imgBoxes++;
                for (auto& ch : b->children) count(ch.get());
            };
            if (tree) count(tree.get());
            Logger::info("NetSurfEngine: layout " + std::string(ok ? "ok" : "FAILED") +
                         " in " + std::to_string(ms) + "ms (html " +
                         std::to_string(p->html.size()) + "B, boxes " +
                         std::to_string(boxes) + ", text " + std::to_string(textBoxes) +
                         ", img " + std::to_string(imgBoxes) + ")");
        }
    }
    {
        std::lock_guard<std::mutex> lock(eng->m_asyncMutex);
        if (p->epoch == eng->m_layoutEpoch.load()) {
            eng->m_pendingTree = std::move(tree);
            eng->m_pendingOk = ok;
            eng->m_pendingEpoch = p->epoch;
            eng->m_pendingReady = true;
        }
        // else: stale navigation — drop the tree.
    }
    return nullptr;
}

void NetSurfEngine::poll() {
    std::unique_ptr<RenderBox> tree;
    bool ready = false, ok = false;
    uint64_t readyEpoch = 0;
    {
        std::lock_guard<std::mutex> lock(m_asyncMutex);
        if (m_pendingReady) {
            ready = true;
            ok = m_pendingOk;
            readyEpoch = m_pendingEpoch;
            tree = std::move(m_pendingTree);
            m_pendingReady = false;
        }
    }
    if (!ready) return;
    if (readyEpoch != m_layoutEpoch.load()) return;  // stale — drop
    if (ok && tree) {
        NetSurfRenderer::instance().setRenderTree(std::move(tree));
        Logger::info("NetSurfEngine: layout applied (maxScroll: " +
                     std::to_string(NetSurfRenderer::instance().maxScroll()) + "px)");
    } else {
        NetSurfRenderer::instance().reset();
        Logger::warn("NetSurfEngine: layout failed for " + m_currentUrl);
    }
    // The HTTP fetch already finished (HtmlRenderer::pollFetch); the page is
    // drawable now. Never override an ERROR state set after us (error page).
    if (BrowserManager::instance().state() == BrowserState::LOADING) {
        BrowserManager::instance().setState(BrowserState::RENDERING);
    }
}

void NetSurfEngine::render() {
    if (!m_initialized) return;
    poll();  // adopt a finished async layout, if any
    NetSurfRenderer::instance().render();
}

bool NetSurfEngine::navigate(NavDirection dir) {
    return NetSurfRenderer::instance().navigate(dir);
}

void NetSurfEngine::scrollBy(int delta) {
    NetSurfRenderer::instance().scrollBy(delta);
}

int NetSurfEngine::scrollY() const {
    return NetSurfRenderer::instance().scrollY();
}

int NetSurfEngine::maxScroll() const {
    return NetSurfRenderer::instance().maxScroll();
}

RenderBox* NetSurfEngine::focusedBox() const {
    return NetSurfRenderer::instance().focusedBox();
}

std::string NetSurfEngine::currentFocusedLink() const {
    RenderBox* b = focusedBox();
    if (b && !b->href.empty()) {
        return b->href;
    }
    return "";
}

void NetSurfEngine::clear() {
    ++m_layoutEpoch;  // invalidate any layout still in flight
    {
        std::lock_guard<std::mutex> lock(m_asyncMutex);
        m_pendingTree.reset();
        m_pendingReady = false;
    }
    m_editing = false;
    NetSurfRenderer::instance().reset();
    m_rawHtml.clear();
    m_currentUrl.clear();
}

// ---- P17: form editing ----

static void clearEditingFlags(RenderBox* root) {
    if (!root) return;
    root->editing = false;
    for (auto& ch : root->children) clearEditingFlags(ch.get());
}

static bool isTextLike(const RenderBox* b) {
    if (!b || b->type != RenderBoxType::INPUT) return false;
    const std::string& t = b->inputType;
    return t == "text" || t == "password" || t == "textarea";
}

RenderBox* NetSurfEngine::focusedInputBox() const {
    RenderBox* b = NetSurfRenderer::instance().focusedBox();
    return isTextLike(b) ? b : nullptr;
}

void NetSurfEngine::setEditing(bool on) {
    clearEditingFlags(NetSurfRenderer::instance().getRenderTree());
    m_editing = false;
    if (on) {
        RenderBox* b = focusedInputBox();
        if (b) {
            b->editing = true;
            m_editing = true;
        }
    }
}

std::string NetSurfEngine::focusedInputValue() const {
    RenderBox* b = focusedInputBox();
    return b ? b->value : std::string();
}

void NetSurfEngine::setFocusedInputValue(const std::string& v) {
    RenderBox* b = focusedInputBox();
    if (b) b->value = v;
}

int NetSurfEngine::focusedInputMaxLen() const {
    RenderBox* b = focusedInputBox();
    if (!b || b->maxLen <= 0) return 256;
    return b->maxLen;
}

void NetSurfEngine::toggleFocusedCheck() {
    RenderBox* b = NetSurfRenderer::instance().focusedBox();
    if (!b || b->type != RenderBoxType::INPUT) return;
    if (b->inputType == "checkbox" || b->inputType == "radio")
        b->checked = !b->checked;
}

void NetSurfEngine::cycleFocusedSelect() {
    RenderBox* b = NetSurfRenderer::instance().focusedBox();
    if (!b || b->type != RenderBoxType::INPUT) return;
    if (b->inputType != "select" || b->options.empty()) return;
    b->selected = (b->selected + 1) % (int)b->options.size();
    b->value = b->options[(size_t)b->selected];
}

std::string NetSurfEngine::focusedSelectOption() const {
    RenderBox* b = NetSurfRenderer::instance().focusedBox();
    if (!b || b->inputType != "select") return "";
    return b->value;
}

// Enclosing <form> box of b (null when the control sits outside any form).
static RenderBox* enclosingForm(RenderBox* b) {
    while (b && b->tagName != "form") b = b->parent;
    return b;
}

bool NetSurfEngine::submitFocusedForm() {
    RenderBox* fb = NetSurfRenderer::instance().focusedBox();
    if (!fb) return false;
    bool isButton = (fb->type == RenderBoxType::BUTTON);
    bool isSubmit = (fb->type == RenderBoxType::INPUT &&
                     (fb->inputType == "submit" || fb->inputType == "button" ||
                      fb->inputType == "image"));
    if (!isButton && !isSubmit) return false;

    RenderBox* form = enclosingForm(fb);
    std::string action = form ? form->formAction : "";
    std::string method = form ? form->formMethod : "GET";
    // Never log the action verbatim — it can carry credentials in the
    // query string (mirrors HtmlRenderer::submitForm).
    Logger::info(std::string("NetSurfEngine: form submission (action present: ") +
                 (action.empty() ? "no" : "yes") + ")");
    if (action.empty()) {
        Logger::error("NetSurfEngine::submitFocusedForm: no form action");
        return false;
    }
    // M6 whitelist (mirrors HtmlRenderer::submitForm): refuse non-http(s).
    if (action.substr(0, 7) != "http://" && action.substr(0, 8) != "https://") {
        Logger::error("NetSurfEngine::submitFocusedForm: refused non-http(s)");
        return false;
    }
    std::transform(method.begin(), method.end(), method.begin(), ::tolower);

    // Collect inputs belonging to this exact form (pointer identity).
    std::unordered_map<std::string, std::string> formData;
    std::function<void(RenderBox*)> walk = [&](RenderBox* b) {
        if (!b || b->phantom) return;
        if (b->type == RenderBoxType::INPUT && b != fb &&
            enclosingForm(b) == form) {
            const std::string& t = b->inputType;
            if (t == "checkbox" || t == "radio") {
                if (b->checked && !b->inputName.empty())
                    formData[b->inputName] = b->value.empty() ? "on" : b->value;
            } else if (t == "text" || t == "password" || t == "textarea" ||
                       t == "hidden" || t == "select") {
                if (!b->inputName.empty()) formData[b->inputName] = b->value;
            }
        }
        for (auto& ch : b->children) walk(ch.get());
    };
    walk(NetSurfRenderer::instance().getRenderTree());

    // Resolve relative actions (same rules as HtmlRenderer::submitForm).
    std::string resolvedAction = action;
    std::string cur = m_currentUrl;
    if (action[0] == '/' || action.find("://") == std::string::npos) {
        std::string base = cur;
        size_t schemeEnd = base.find("://");
        if (schemeEnd != std::string::npos) {
            size_t pathStart = base.find('/', schemeEnd + 3);
            if (pathStart != std::string::npos) base = base.substr(0, pathStart);
        }
        if (action[0] == '/') {
            resolvedAction = base + action;
        } else {
            size_t lastSlash = cur.rfind('/');
            if (lastSlash != std::string::npos)
                resolvedAction = cur.substr(0, lastSlash + 1) + action;
            else
                resolvedAction = base + "/" + action;
        }
    }

    std::string targetUrl;
    if (method == "post") {
        targetUrl = resolvedAction;
    } else {
        targetUrl = resolvedAction;
        targetUrl += (targetUrl.find('?') == std::string::npos) ? "?" : "&";
        bool first = true;
        for (const auto& kv : formData) {
            if (!first) targetUrl += "&";
            first = false;
            targetUrl += HttpClient::instance().urlEncode(kv.first) + "=" +
                         HttpClient::instance().urlEncode(kv.second);
        }
    }
    HtmlRenderer::instance().fetchAsync(targetUrl, method, formData);
    return true;
}

} // namespace RomCloud

