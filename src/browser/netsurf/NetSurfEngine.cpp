#include "NetSurfEngine.h"
#include "NetSurfJsBridge.h"
#include "../BrowserManager.h"
#include "../HtmlRenderer.h"
#include "../../input/InputManager.h"
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
    std::string url;  // P23: base để resolve <link> href tương đối
    std::string extraCss;
    uint64_t epoch;
};

// P23: resolve href tương đối về URL tuyệt đối.
static std::string resolveCssUrl(const std::string& base, const std::string& ref) {
    if (ref.empty()) return std::string();
    if (ref.compare(0, 7, "http://") == 0 || ref.compare(0, 8, "https://") == 0)
        return ref;
    if (ref.compare(0, 2, "//") == 0) {
        std::string scheme = "https:";
        size_t se = base.find("://");
        if (se != std::string::npos) scheme = base.substr(0, se + 1);
        return scheme + ref;
    }
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

std::string NetSurfEngine::fetchExternalCss(const std::string& html,
                                            const std::string& baseUrl,
                                            uint64_t epoch, NetSurfEngine* eng) {
    std::string out;
    if (html.empty() || baseUrl.empty()) return out;
    if (!BrowserManager::instance().settings().css) return out;
    const int kMaxSheets = 3;
    const size_t kMaxBytes = 256 * 1024;  // mỗi file, chống file rác khổng lồ
    size_t pos = 0;
    int fetched = 0;
    while (fetched < kMaxSheets && (pos = html.find("<link", pos)) != std::string::npos) {
        size_t end = html.find('>', pos);
        if (end == std::string::npos) break;
        std::string tag = html.substr(pos, end - pos);
        pos = end + 1;
        std::string low = tag;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (low.find("stylesheet") == std::string::npos) continue;
        size_t hp = low.find("href");
        if (hp == std::string::npos) continue;
        size_t q1 = tag.find('"', hp);
        char qc = '"';
        if (q1 == std::string::npos) { q1 = tag.find('\'', hp); qc = '\''; }
        if (q1 == std::string::npos) continue;
        size_t q2 = tag.find(qc, q1 + 1);
        if (q2 == std::string::npos) continue;
        std::string href = tag.substr(q1 + 1, q2 - q1 - 1);
        std::string url = resolveCssUrl(baseUrl, href);
        if (url.empty()) continue;
        if (epoch != eng->m_layoutEpoch.load()) break;  // user chuyển trang
        HttpResponse resp = HttpClient::instance().get(
            url, HtmlRenderer::getBrowserHeaders(baseUrl), 4);
        if (resp.success && !resp.body.empty() && resp.body.size() <= kMaxBytes) {
            if (!out.empty()) out.push_back('\n');
            out.append(resp.body);
            fetched++;
            Logger::info("NetSurfEngine: fetched CSS " + url + " (" +
                         std::to_string(resp.body.size()) + " bytes)");
        }
    }
    return out;
}

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
    // P22: invalidate any old JS bridge; the new bridge is built in runJs()
    // after the new layout is adopted. Drop pending external-script work too.
    m_jsEpoch++;
    {
        std::lock_guard<std::mutex> lock(m_jsMutex);
        m_js.reset();
        m_scripts.clear();
        m_scriptSrcs.clear();
        m_jsQueue.clear();
        m_jsReady.clear();
        m_jsInFlight = 0;
    }
    // Extract <script> bodies and external src URLs synchronously so we know
    // what to run right after the layout lands.
    extractScriptsForTest(html, m_scripts, m_scriptSrcs);
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
                 ", extraCss: " + std::to_string(extraCss.size()) + " bytes, " +
                 "scripts: " + std::to_string(m_scripts.size()) + " inline + " +
                 std::to_string(m_scriptSrcs.size()) + " external)");
    LayoutPayload* payload = new LayoutPayload{this, html, url, extraCss, epoch};
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
            // P23: fetch <link rel=stylesheet> ở worker (không block UI),
            // nối vào extraCss — bridge sẽ parse chung với <style> inline.
            // CSS ngoài đứng trước inline để đúng thứ tự cascade.
            std::string extCss = NetSurfEngine::fetchExternalCss(
                p->html, p->url, p->epoch, eng);
            std::string allCss = extCss;
            if (!p->extraCss.empty()) {
                if (!allCss.empty()) allCss.push_back('\n');
                allCss.append(p->extraCss);
            }
            // Viewport width = 984 (1024 - 40 margin)
            ok = NetSurfBridge::instance().layout(p->html, allCss, 984, tree, p->url);
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
        // P22: hook JS for the freshly laid-out page. Build the bridge on
        // demand and run inline scripts. External scripts go on a queue.
        if (!m_scripts.empty() || !m_scriptSrcs.empty()) {
            if (BrowserManager::instance().settings().js) {
                if (!m_js || m_js->epoch() != m_jsEpoch) {
                    m_js.reset(new NetSurfJsBridge(this, m_jsEpoch, m_currentUrl));
                }
                m_js->setRoot(NetSurfRenderer::instance().getRenderTree());
                bool mutated = false;
                int ran = 0;
                for (const auto& body : m_scripts) {
                    if (ran++ >= 32) break;
                    if (m_js->runScript(body, "inline")) mutated = true;
                }
                if (mutated) {
                    relayout();
                    return;
                }
                // Queue external scripts (cap like HtmlRenderer).
                {
                    std::lock_guard<std::mutex> lock(m_jsMutex);
                    for (const auto& src : m_scriptSrcs) {
                        if ((int)m_jsQueue.size() >= kMaxJsFiles) break;
                        if (src.empty()) continue;
                        if (src.compare(0, 7, "http://") != 0 &&
                            src.compare(0, 8, "https://") != 0) continue;
                        // Filter known-bad ad/tracker hosts (mirrors HtmlRenderer).
                        if (src.find("google-analytics") != std::string::npos ||
                            src.find("googletagmanager") != std::string::npos ||
                            src.find("doubleclick") != std::string::npos ||
                            src.find("facebook.net") != std::string::npos ||
                            src.find("clarity.ms") != std::string::npos) {
                            continue;
                        }
                        m_jsQueue.push_back(src);
                    }
                }
            }
        }
    } else {
        NetSurfRenderer::instance().reset();
        Logger::warn("NetSurfEngine: layout failed for " + m_currentUrl);
        BrowserManager::instance().setError("Không thể hiển thị trang web: lỗi dựng khung hình");
        return;
    }
    // Layout successful: flip from LOADING to RENDERING
    if (BrowserManager::instance().state() == BrowserState::LOADING) {
        BrowserManager::instance().setState(BrowserState::RENDERING);
    }
}

void NetSurfEngine::render() {
    if (!m_initialized) return;
    poll();  // adopt a finished async layout, if any
    pollJs();
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
    // P22: drop JS state.
    m_jsEpoch++;
    {
        std::lock_guard<std::mutex> lock(m_jsMutex);
        m_js.reset();
        m_scripts.clear();
        m_scriptSrcs.clear();
        m_jsQueue.clear();
        m_jsReady.clear();
        m_jsInFlight = 0;
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

bool NetSurfEngine::handleInput() {
    // FULL input handler (in-process only): keyboard/4-way-nav, no cursor
    // mode, no click-at-pixel. Returns true if the event was consumed.
    if (!m_initialized) return false;
    InputManager& input = InputManager::instance();

    // L1/R1: smooth page scroll (matches daemon's 120px step).
    if (input.isButtonJustPressed(Button::L1) || input.isButtonRepeat(Button::L1)) {
        scrollBy(-120);
        return true;
    }
    if (input.isButtonJustPressed(Button::R1) || input.isButtonRepeat(Button::R1)) {
        scrollBy(120);
        return true;
    }
    // L2/R2: large page jump (matches daemon's 350px step).
    if (input.isButtonJustPressed(Button::L2)) {
        scrollBy(-350);
        return true;
    }
    if (input.isButtonJustPressed(Button::R2)) {
        scrollBy(350);
        return true;
    }

    // D-Pad: 4-way focus navigation. The focus box is a regular box in
    // the layout tree (anchor / input / button), highlighted by the
    // renderer. Holding auto-repeats via the InputManager's repeat path.
    if (input.isButtonJustPressed(Button::UP))    { navigate(NavDirection::UP);    return true; }
    if (input.isButtonJustPressed(Button::DOWN))  { navigate(NavDirection::DOWN);  return true; }
    if (input.isButtonJustPressed(Button::LEFT))  { navigate(NavDirection::LEFT);  return true; }
    if (input.isButtonJustPressed(Button::RIGHT)) { navigate(NavDirection::RIGHT); return true; }

    // A: activate focused box.
    //   - <a href>  → BrowserManager::openUrl(href)
    //   - <input type=submit|button|image> / <button> → submitFocusedForm
    //   - <input type=text|password|textarea> → enter edit mode (UIManager
    //     shows the VirtualKeyboard overlay, mirroring P1-2c for LITE).
    //   - <input type=checkbox|radio> → toggleFocusedCheck
    //   - <select> → cycleFocusedSelect
    if (input.isButtonJustPressed(Button::A)) {
        RenderBox* fb = focusedBox();
        if (!fb) return false;
        if (fb->type == RenderBoxType::LINK) {
            std::string href = currentFocusedLink();
            if (!href.empty()) {
                Logger::info("NetSurfEngine: navigate to " + href);
                BrowserManager::instance().openUrl(href);
            }
            return true;
        }
        if (fb->type == RenderBoxType::INPUT) {
            const std::string& t = fb->inputType;
            if (t == "text" || t == "password" || t == "textarea") {
                m_editing = true;
                return true;
            }
            if (t == "checkbox" || t == "radio") {
                toggleFocusedCheck();
                return true;
            }
            if (t == "submit" || t == "button" || t == "image") {
                submitFocusedForm();
                return true;
            }
            if (t == "select") {
                cycleFocusedSelect();
                return true;
            }
        }
        if (fb->type == RenderBoxType::BUTTON) {
            submitFocusedForm();
            return true;
        }
        return false;  // Focus the box but no action
    }

    // SELECT: focus the first interactive box (mirror daemon's behaviour
    // when the user toggles to "Chế độ chuột" but stuck in the page).
    if (input.isButtonJustPressed(Button::SELECT)) {
        NetSurfRenderer::instance().focusFirst();
        return true;
    }

    // P24: X = forward. B (back) is handled at the UIManager layer because
    // it also closes the browser on the tail of the stack; X has no such
    // double-duty, so we keep it local to the engine. After goBack() the
    // user expects X to take them back to where they came from — the
    // forward stack lives in BrowserManager::m_history and is intact.
    if (input.isButtonJustPressed(Button::X)) {
        if (BrowserManager::instance().canGoForward()) {
            BrowserManager::instance().goForward();
            return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// P22: JavaScript bridge plumbing (Duktape) — extract scripts, run after
// layout, fetch external scripts, re-layout on DOM mutations.
// ---------------------------------------------------------------------------

void NetSurfEngine::extractScriptsForTest(const std::string& html,
                                          std::vector<std::string>& outInline,
                                          std::vector<std::string>& outSrc) {
    outInline.clear();
    outSrc.clear();
    if (html.empty()) return;
    size_t pos = 0;
    while ((pos = html.find("<script", pos)) != std::string::npos) {
        size_t end = html.find('>', pos);
        if (end == std::string::npos) break;
        std::string attrs = html.substr(pos, end - pos);
        std::string attrsLow = attrs;
        std::transform(attrsLow.begin(), attrsLow.end(), attrsLow.begin(), ::tolower);
        if (attrsLow.find("type=") != std::string::npos) {
            if (attrsLow.find("module") != std::string::npos ||
                attrsLow.find("json") != std::string::npos ||
                attrsLow.find("text/template") != std::string::npos) {
                pos = end + 1;
                continue;
            }
        }
        bool external = false;
        std::string src;
        size_t sp = attrsLow.find("src=");
        if (sp != std::string::npos) {
            char q = attrs[sp + 4];
            if (q == '"' || q == '\'') {
                size_t q2 = attrs.find(q, sp + 5);
                if (q2 != std::string::npos) {
                    src = attrs.substr(sp + 5, q2 - sp - 5);
                    external = true;
                }
            }
        }
        if (external) {
            outSrc.push_back(src);
        } else {
            size_t bodyStart = end + 1;
            size_t closePos = html.find("</script", bodyStart);
            if (closePos == std::string::npos) { pos = bodyStart; continue; }
            size_t bodyEnd = closePos;
            std::string body = html.substr(bodyStart, bodyEnd - bodyStart);
            size_t a = 0, b = body.size();
            while (a < b && std::isspace((unsigned char)body[a])) ++a;
            while (b > a && std::isspace((unsigned char)body[b - 1])) --b;
            body = body.substr(a, b - a);
            if (body.size() > 64 * 1024) body.resize(64 * 1024);
            if (!body.empty()) outInline.push_back(body);
            pos = closePos + 9;
            continue;
        }
        pos = end + 1;
    }
    if (outInline.size() > 32) outInline.resize(32);
    if (outSrc.size() > (size_t)kMaxJsFiles) outSrc.resize(kMaxJsFiles);
}

void NetSurfEngine::enqueueLayout(const std::string& extraCss) {
    uint64_t epoch = ++m_layoutEpoch;
    {
        std::lock_guard<std::mutex> lock(m_asyncMutex);
        m_pendingTree.reset();
        m_pendingReady = false;
    }
    if (m_rawHtml.empty()) return;
    LayoutPayload* payload =
        new LayoutPayload{this, m_rawHtml, m_currentUrl, extraCss, epoch};
    pthread_t tid;
    if (pthread_create(&tid, nullptr, layoutWorker, payload) != 0) {
        delete payload;
        Logger::error("NetSurfEngine: cannot spawn relayout thread");
        return;
    }
    pthread_detach(tid);
}

void NetSurfEngine::relayout() {
    Logger::info("NetSurfEngine: relayout triggered by JS mutation");
    enqueueLayout();
}

void NetSurfEngine::pollJs() {
    if (!m_initialized) return;
    // 1. Spawn external-script fetchers (mirrors HtmlRenderer::pollJs()).
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
                                NetSurfEngine::instance().currentUrl()), 15);
                NetSurfEngine& self = NetSurfEngine::instance();
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
    // 2. Run arrived external scripts on the main thread.
    bool arrived = false;
    {
        std::lock_guard<std::mutex> lock(m_jsMutex);
        arrived = !m_jsReady.empty();
    }
    if (arrived) {
        if (!m_js || m_js->epoch() != m_jsEpoch) {
            m_js.reset(new NetSurfJsBridge(this, m_jsEpoch, m_currentUrl));
        }
        m_js->setRoot(NetSurfRenderer::instance().getRenderTree());
        if (BrowserManager::instance().settings().js) {
            std::vector<std::pair<std::string, std::string>> batch;
            {
                std::lock_guard<std::mutex> lock(m_jsMutex);
                batch.swap(m_jsReady);
            }
            bool mutated = false;
            for (const auto& item : batch) {
                if (m_js->runScript(item.second, item.first)) mutated = true;
            }
            if (mutated) relayout();
        } else {
            std::lock_guard<std::mutex> lock(m_jsMutex);
            m_jsReady.clear();
        }
    }
    // 3. Pump timers + check dirty flag.
    if (m_js && m_js->epoch() == m_jsEpoch) {
        bool changed = m_js->poll(SDL_GetTicks());
        if (changed || m_js->dirty()) {
            m_js->clearDirty();
            relayout();
        }
    }
}

} // namespace RomCloud


