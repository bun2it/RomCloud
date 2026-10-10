#pragma once
#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <vector>
#include <utility>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "NetSurfBridge.h"
#include "NetSurfLayout.h"
#include "NetSurfRenderer.h"

namespace RomCloud {

class NetSurfJsBridge;  // P22: Duktape bridge for JS in NetSurf pages.

class NetSurfEngine {
public:
    static NetSurfEngine& instance();

    void init(SDL_Renderer* renderer, TTF_Font* font,
              const std::string& fontPath = "", int baseFontSize = 36);

    // Layout and render HTML with NetSurf pipeline.
    // P16: loadHtml() only QUEUES the work — hubbub/libcss parse + layout
    // run on a worker thread so big pages don't freeze the UI. poll() (called
    // from render()) adopts the finished RenderBox tree on the main thread.
    bool loadHtml(const std::string& html, const std::string& url = "",
                  const std::string& extraCss = "");

    // Main thread: adopt a finished async layout, if any. Called from
    // render(); safe to call every frame.
    void poll();

    // P22: extract <script> bodies + external src from raw HTML. Called by
    // loadHtml() before queueing layout. Visible to tests.
    void extractScriptsForTest(const std::string& html,
                               std::vector<std::string>& outInline,
                               std::vector<std::string>& outSrc);

    // P22: trigger a re-layout + re-render after JS mutated the DOM.
    // Spawns the same async worker as loadHtml(). Safe to call from main.
    void relayout();

    // P22: main-thread JS pump. Runs inline scripts after layout, fetches
    // external scripts, drains timers. Called from render() and poll().
    void pollJs();

    // P22: number of inline / external scripts extracted from current page.
    int inlineScriptCount() const { return (int)m_scripts.size(); }
    int externalScriptCount() const { return (int)m_scriptSrcs.size(); }

    // True while a layout job is in flight (page not drawable yet).

    // Render current frame
    void render();

    // D-Pad and Page navigation
    bool navigate(NavDirection dir);
    void scrollBy(int delta);
    int scrollY() const;
    int maxScroll() const;

    // Focused box and active link
    RenderBox* focusedBox() const;
    std::string currentFocusedLink() const;

    // Reset/clear
    void clear();

    // P20: in-process FULL input handler. Mirrors NetSurfWorkerBridge::
    // handleInput() but without the SHM mouse path — in-process has no
    // cursor mode, just 4-way D-Pad focus nav + scroll + form submit.
    // Returns true if the event was consumed. UIManager calls this when
    // fullEngine + inProcessFull are both true; otherwise it calls
    // NetSurfWorkerBridge::handleInput().
    bool handleInput();

    // P17: form editing — FULL-mode counterpart of HtmlRenderer's P1-2c API.
    // UIManager drives the VirtualKeyboard through these.
    bool isEditing() const { return m_editing; }
    void setEditing(bool on);
    // Focused text-like INPUT (text/password/textarea), else null.
    RenderBox* focusedInputBox() const;
    std::string focusedInputValue() const;
    void setFocusedInputValue(const std::string& v);
    int focusedInputMaxLen() const;
    // Checkbox/radio flip, select option cycle.
    void toggleFocusedCheck();
    void cycleFocusedSelect();
    std::string focusedSelectOption() const;
    // Submit the enclosing form of the focused button (async via pollFetch).
    bool submitFocusedForm();

    bool isInitialized() const { return m_initialized; }
    const std::string& currentUrl() const { return m_currentUrl; }
    const std::string& rawHtml() const { return m_rawHtml; }

private:
    NetSurfEngine() = default;
    ~NetSurfEngine() = default;
    NetSurfEngine(const NetSurfEngine&) = delete;
    NetSurfEngine& operator=(const NetSurfEngine&) = delete;

    // Worker entry point (static: pthread_create needs a free function).
    static void* layoutWorker(void* arg);

    // P23: fetch <link rel=stylesheet> trong worker thread (không block UI).
    // Trả về CSS gộp để nối vào extraCss trước khi layout.
    static std::string fetchExternalCss(const std::string& html,
                                        const std::string& baseUrl,
                                        uint64_t epoch, NetSurfEngine* eng);

    std::string m_currentUrl;
    std::string m_rawHtml;
    bool m_initialized = false;

    // P17: VK edit session flag (the per-box cursor flag lives on RenderBox).
    bool m_editing = false;

    // P16: async layout handoff. Worker fills m_pendingTree under
    // m_asyncMutex; the main thread adopts it in poll(). m_layoutEpoch
    // invalidates stale jobs when the user navigates mid-layout.
    std::mutex m_asyncMutex;
    std::unique_ptr<RenderBox> m_pendingTree;
    bool m_pendingReady = false;
    bool m_pendingOk = false;
    uint64_t m_pendingEpoch = 0;
    std::atomic<uint64_t> m_layoutEpoch{0};

    // P22: JavaScript bridge state. m_jsEpoch bumps on every clear() and
    // loadHtml() so an old bridge stops being valid for a new page. m_js
    // is created lazily by runJs() right after a successful layout. The
    // async fetch queue mirrors HtmlRenderer's m_jsReady/m_jsQueue.
    uint64_t m_jsEpoch = 0;
    std::unique_ptr<NetSurfJsBridge> m_js;
    std::vector<std::string> m_scripts;     // inline <script> bodies
    std::vector<std::string> m_scriptSrcs;  // <script src="..."> URLs
    std::vector<std::pair<std::string, std::string>> m_jsReady;  // (url, body) arrived
    std::vector<std::string> m_jsQueue;     // external src URLs awaiting fetch
    int m_jsInFlight = 0;
    static constexpr int kMaxJsFiles = 8;
    static constexpr int kMaxJsInFlight = 2;
    static constexpr size_t kMaxJsBytes = 256 * 1024;
    std::mutex m_jsMutex;

    // P22: bump layoutEpoch (cancel pending worker + queue fresh job). Used
    // by relayout() after a JS mutation to force a re-layout.
    void enqueueLayout(const std::string& extraCss = "");
};

} // namespace RomCloud
