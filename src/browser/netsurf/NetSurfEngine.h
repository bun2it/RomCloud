#pragma once
#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "NetSurfBridge.h"
#include "NetSurfLayout.h"
#include "NetSurfRenderer.h"

namespace RomCloud {

class NetSurfEngine {
public:
    static NetSurfEngine& instance();

    void init(SDL_Renderer* renderer, TTF_Font* font,
              const std::string& fontPath = "", int baseFontSize = 24);

    // Layout and render HTML with NetSurf pipeline.
    // P16: loadHtml() only QUEUES the work — hubbub/libcss parse + layout
    // run on a worker thread so big pages don't freeze the UI. poll() (called
    // from render()) adopts the finished RenderBox tree on the main thread.
    bool loadHtml(const std::string& html, const std::string& url = "",
                  const std::string& extraCss = "");

    // Main thread: adopt a finished async layout, if any. Called from
    // render(); safe to call every frame.
    void poll();

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
};

} // namespace RomCloud
