#pragma once
#include <string>
#include <memory>
#include <vector>
#include <SDL2/SDL.h>
#include "NetSurfShm.h"
#include "../../input/InputManager.h"

namespace RomCloud {

class NetSurfWorkerBridge {
public:
    static NetSurfWorkerBridge& instance();

    // Start / Stop NetSurf daemon process
    bool start(const std::string& initialUrl = "https://google.com");
    void stop();
    bool isRunning() const;

    // Render viewport into SDL_Renderer
    void render(SDL_Renderer* renderer);

    // Input handlers from TrimUI controller
    bool handleInput();

    // Send low-level events
    void pushEvent(const NetSurfShmEvent& event);
    void sendClick(int x, int y, int mouseButton = NS_SHM_KEY_MOUSE_1);
    void sendMouseMove(int x, int y);

    // Modes & Cursor
    bool isCursorMode() const { return m_cursorMode; }

    // Scroll
    void scrollBy(int deltaY);

    // Form editing support
    bool isEditing() const { return m_editing; }
    void setEditing(bool on) { m_editing = on; }

    // Lazy-image shim: fetch + viết lại HTML cho daemon (không JS).
    // Trả về URL file:// đã xử lý, hoặc URL gốc khi không xử lý được.
    std::string prepareDaemonUrl(const std::string& url);

private:
    NetSurfWorkerBridge();
    ~NetSurfWorkerBridge();
    NetSurfWorkerBridge(const NetSurfWorkerBridge&) = delete;
    NetSurfWorkerBridge& operator=(const NetSurfWorkerBridge&) = delete;

    bool initShm();
    void cleanupShm();
    void drawVirtualCursor(SDL_Renderer* renderer, int screenX, int screenY);
    void drawTargetReticle(SDL_Renderer* renderer, int screenX, int screenY);

    pid_t m_childPid = -1;
    int m_shmFd = -1;
    size_t m_shmSize = 0;
    NetSurfShmHeader* m_shmHdr = nullptr;
    uint8_t* m_pixelBuffer = nullptr;

    SDL_Texture* m_viewportTexture = nullptr;
    SDL_Renderer* m_lastRenderer = nullptr;
    uint32_t m_lastRenderedSeq = 0;

    // Cursor & interaction state
    int m_cursorX = 512;
    int m_cursorY = 301;
    bool m_cursorMode = false; // false = scroll / viewport navigation (default), true = free pointer
    bool m_editing = false;
    std::string m_currentUrl;

    // Viewport layout constants
    static constexpr int VIEWPORT_X = 0;
    static constexpr int VIEWPORT_Y = 112;
    static constexpr int VIEWPORT_W = 1024;
    static constexpr int VIEWPORT_H = 603;
};

} // namespace RomCloud
