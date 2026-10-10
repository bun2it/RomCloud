#pragma once
// NetSurfJsBridge — Duktape bridge for NetSurfEngine (P22). Parallel to
// src/browser/JsEngine.h but operates on RenderBox* instead of HtmlElement*.
//
// Scope (v1):
//   - console.log / warn / error
//   - setTimeout / clearTimeout / setInterval
//   - alert
//   - document.getElementById / querySelector
//   - element.textContent, innerHTML, value, style.display, classList
//   - element.click()  (triggers JS click listeners)
//   - element.addEventListener('click', fn)
//   - location.href
//
// Safety: one heap per page (epoch-checked); every script runs inside
// duk_pcall with a longjmp fatal handler. The renderer pointer + epoch
// travel in the global stash. External scripts are pulled by the engine
// (worker thread), and run here on the main thread in poll().
//
// What it does NOT do: fetch, Promise, ES modules, WebGL, getComputedStyle
// for arbitrary properties. That's all out-of-scope for the Brick.
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "../duktape/duktape.h"
}

namespace RomCloud {

class NetSurfEngine;
struct RenderBox;

struct NetSurfJsTimer {
    uint32_t id = 0;
    uint32_t dueMs = 0;
    uint32_t intervalMs = 0;  // 0 = one-shot
    bool repeat = false;
};

class NetSurfJsBridge {
public:
    NetSurfJsBridge(NetSurfEngine* engine, uint64_t epoch,
                    const std::string& pageUrl);
    ~NetSurfJsBridge();

    NetSurfJsBridge(const NetSurfJsBridge&) = delete;
    NetSurfJsBridge& operator=(const NetSurfJsBridge&) = delete;

    // Run one inline script body. Errors are logged, never thrown.
    bool runScript(const std::string& code, const std::string& name);

    // Main-thread pump: fires due timers. Returns true when the DOM may have
    // changed (caller relayouts + markDirty).
    bool poll(uint32_t nowMs);

    // Drain queued click handlers attached via addEventListener. Called by
    // NetSurfEngine when JS or the renderer fires a synthetic click on a box.
    // 'thisEl' may be null (e.g. window.alert flow).
    void fireClick(RenderBox* thisEl);

    uint64_t epoch() const { return m_epoch; }
    NetSurfEngine* engine() const { return m_engine; }

    // Owned by the bridge, accessed by the engine.
    std::mutex m_asyncMutex;
    std::vector<NetSurfJsTimer> m_timers;
    uint32_t m_nextId = 1;

private:
    // Globals registered once.
    void registerGlobals();

    // Find a RenderBox by id (linear walk; pages are small).
    static RenderBox* findById(RenderBox* root, const std::string& id);
    // Find first RenderBox by simple tag / .class selector.
    static RenderBox* findFirst(RenderBox* root, const std::string& sel);

    // Duktape native bindings (C linkage via static).
    static duk_ret_t fnLog(duk_context* ctx);
    static duk_ret_t fnAlert(duk_context* ctx);
    static duk_ret_t fnSetTimeout(duk_context* ctx);
    static duk_ret_t fnClearTimeout(duk_context* ctx);
    static duk_ret_t fnSetInterval(duk_context* ctx);
    static duk_ret_t fnGetElementById(duk_context* ctx);
    static duk_ret_t fnQuerySelector(duk_context* ctx);
    static duk_ret_t fnGetBody(duk_context* ctx);
    static duk_ret_t fnLocation(duk_context* ctx);  // getter only; setter on .href
    static duk_ret_t locationGetHref(duk_context* ctx);
    static duk_ret_t locationSetHref(duk_context* ctx);

    // Element proxy helpers.
    void pushElementProxy(RenderBox* el);
    static RenderBox* checkElement(duk_context* ctx, duk_idx_t idx, uint64_t* epochOut);
    static duk_ret_t elGetText(duk_context* ctx);
    static duk_ret_t elSetText(duk_context* ctx);
    static duk_ret_t elGetInnerHtml(duk_context* ctx);
    static duk_ret_t elSetInnerHtml(duk_context* ctx);
    static duk_ret_t elGetValue(duk_context* ctx);
    static duk_ret_t elSetValue(duk_context* ctx);
    static duk_ret_t elClick(duk_context* ctx);
    static duk_ret_t elAddEventListener(duk_context* ctx);
    static duk_ret_t elGetClass(duk_context* ctx);
    static duk_ret_t classListAdd(duk_context* ctx);
    static duk_ret_t classListRemove(duk_context* ctx);
    static duk_ret_t classListToggle(duk_context* ctx);
    static duk_ret_t classListContains(duk_context* ctx);
    static duk_ret_t styleGetDisplay(duk_context* ctx);
    static duk_ret_t styleSetDisplay(duk_context* ctx);

    // Duktape ↔ bridge resolution.
    static NetSurfJsBridge* engineOf(duk_context* ctx);

    // Housekeeping.
    void markDirty();

    duk_context* m_ctx = nullptr;
    NetSurfEngine* m_engine = nullptr;
    uint64_t m_epoch = 0;
    std::string m_pageUrl;

    // RenderBox* root bound at script-eval time. setRoot() is called by the
    // engine right before runScript() so DOM lookups see the latest tree.
    RenderBox* m_root = nullptr;
    friend class NetSurfEngine;  // for setRoot + setDirty flag

public:
    // Called by the engine right before runScript() / after setRenderTree().
    void setRoot(RenderBox* root) { m_root = root; }
    // Set by element setters; read+cleared by the engine's pollJs().
    bool dirty() const { return m_dirty; }
    void clearDirty() { m_dirty = false; }

private:
    bool m_dirty = false;
};

}  // namespace RomCloud
