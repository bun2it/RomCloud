#pragma once
// JsEngine — Duktape-based JavaScript for portal/news pages (setting js).
// Scope (v1): inline + src scripts run in order at page load; console,
// timers, location, a minimal document (getElementById/querySelector,
// forms[].submit, input value, textContent, style.display, click),
// and async XMLHttpRequest. No layout engine hooks beyond relayout.
// Safety: one heap per page (epoch-checked); every script runs inside
// duk_pcall; timers/XHR capped. Without DOM the web is unusable, with
// full V8 the Brick is unusable — this is the middle path for portals.
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "duktape/duktape.h"
}

namespace RomCloud {

struct HtmlElement;  // HtmlRenderer.h (included by JsEngine.cpp)
class HtmlRenderer;

struct JsTimer {
    uint32_t id = 0;
    uint32_t dueMs = 0;
    uint32_t intervalMs = 0;  // 0 = one-shot (setTimeout)
    bool repeat = false;
};

struct JsXhrJob {
    uint32_t id = 0;
    std::string method;
    std::string url;
    std::string body;
    std::vector<std::string> headers;
    // Own mutex: the worker writes completion here; the job outlives the
    // engine (shared_ptr), so engine death can never dangle the worker.
    std::mutex mtx;
    bool done = false;
    bool success = false;
    int status = 0;
    std::string respBody;
    // P16: atomic — poll() reads this outside job->mtx while the worker
    // only ever writes done/status/body under the mutex. All writers are
    // main-thread, but the unlocked read was technically a data race.
    std::atomic<uint64_t> epoch{0};
};

class JsEngine {
public:
    JsEngine(HtmlRenderer* renderer, uint64_t epoch, const std::string& pageUrl);
    ~JsEngine();

    JsEngine(const JsEngine&) = delete;
    JsEngine& operator=(const JsEngine&) = delete;

    // Run one script body (errors logged, never thrown). Returns success.
    bool runScript(const std::string& code, const std::string& name);

    // Main-thread pump: due timers + finished XHR callbacks. Returns true
    // when the DOM may have changed (caller relayouts).
    bool poll(uint32_t nowMs);

    uint64_t epoch() const { return m_epoch; }

    // Timer/XHR storage (main thread only, except XHR completion flag
    // which the worker sets under m_asyncMutex).
    std::mutex m_asyncMutex;
    std::vector<JsTimer> m_timers;
    std::vector<std::shared_ptr<JsXhrJob>> m_xhrs;
    uint32_t m_nextId = 1;

    HtmlRenderer* renderer() const { return m_renderer; }

private:
    duk_context* m_ctx = nullptr;
    HtmlRenderer* m_renderer = nullptr;
    uint64_t m_epoch = 0;
    std::string m_pageUrl;

    void registerGlobals();
    static duk_ret_t fnLog(duk_context* ctx);
    static duk_ret_t fnSetTimeout(duk_context* ctx);
    static duk_ret_t fnClearTimeout(duk_context* ctx);
    static duk_ret_t fnSetInterval(duk_context* ctx);
    static duk_ret_t fnGetElementById(duk_context* ctx);
    static duk_ret_t fnQuerySelector(duk_context* ctx);
    static duk_ret_t fnAlert(duk_context* ctx);
    static duk_ret_t fnXhrCtor(duk_context* ctx);

    // Element proxy helpers (stack discipline documented inline).
    void pushElementProxy(HtmlElement* el);
    static HtmlElement* checkElement(duk_context* ctx, duk_idx_t idx, uint64_t* epochOut);
    static duk_ret_t elGetValue(duk_context* ctx);
    static duk_ret_t elSetValue(duk_context* ctx);
    static duk_ret_t elGetText(duk_context* ctx);
    static duk_ret_t elSetText(duk_context* ctx);
    static duk_ret_t elClick(duk_context* ctx);
    static duk_ret_t elSubmit(duk_context* ctx);
    static duk_ret_t elGetStyle(duk_context* ctx);
    static duk_ret_t styleGetDisplay(duk_context* ctx);
    static duk_ret_t styleSetDisplay(duk_context* ctx);
    static duk_ret_t xhrOpen(duk_context* ctx);
    static duk_ret_t xhrSend(duk_context* ctx);
    static duk_ret_t xhrSetHeader(duk_context* ctx);
    static duk_ret_t xhrAbort(duk_context* ctx);
    static std::shared_ptr<JsXhrJob> xhrJobOf(duk_context* ctx);

    static JsEngine* engineOf(duk_context* ctx);
    static HtmlRenderer* rendererOf(duk_context* ctx);
};

}  // namespace RomCloud
