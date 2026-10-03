#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <csetjmp>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/uinput.h>

#include "toojpeg.h"

namespace GameCast {

static const int HTTP_PORT = 8090;
static const char* PID_FILE = "/tmp/gamecast.pid";
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_downscale{true};
static std::atomic<int> g_serverFd{-1};
static void sigHandler(int sig);

// Virtual Gamepad using /dev/uinput
class VirtualGamepad {
public:
    VirtualGamepad() : m_fd(-1) {}
    ~VirtualGamepad() { destroy(); }

    bool isInitialized() const { return m_fd >= 0; }

    bool init() {
        m_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (m_fd < 0) {
            std::cerr << "[GameCast] Failed to open /dev/uinput" << std::endl;
            return false;
        }

        ioctl(m_fd, UI_SET_EVBIT, EV_KEY);
        ioctl(m_fd, UI_SET_EVBIT, EV_SYN);

        const int buttons[] = {
            BTN_A, BTN_B, BTN_X, BTN_Y,
            BTN_START, BTN_SELECT,
            BTN_TL, BTN_TR, // L1, R1
            BTN_TL2, BTN_TR2, // L2, R2
            BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT
        };
        for (int b : buttons) {
            ioctl(m_fd, UI_SET_KEYBIT, b);
        }

        struct uinput_setup usetup;
        std::memset(&usetup, 0, sizeof(usetup));
        usetup.id.bustype = BUS_USB;
        usetup.id.vendor = 0x1234;
        usetup.id.product = 0x5678;
        std::strcpy(usetup.name, "TrimUI GameCast Remote Pad");

        ioctl(m_fd, UI_DEV_SETUP, &usetup);
        ioctl(m_fd, UI_DEV_CREATE);

        std::cout << "[GameCast] Virtual Gamepad created via /dev/uinput" << std::endl;
        return true;
    }

    void emit(int code, int val) {
        if (m_fd < 0) return;
        struct input_event ev[2];
        std::memset(ev, 0, sizeof(ev));

        ev[0].type = EV_KEY;
        ev[0].code = code;
        ev[0].value = val;

        ev[1].type = EV_SYN;
        ev[1].code = SYN_REPORT;
        ev[1].value = 0;

        write(m_fd, ev, sizeof(ev));
    }

    void destroy() {
        if (m_fd >= 0) {
            ioctl(m_fd, UI_DEV_DESTROY);
            close(m_fd);
            m_fd = -1;
        }
    }

private:
    int m_fd;
};

static VirtualGamepad g_gamepad;

// Frame buffer sync
struct FrameData {
    std::vector<uint8_t> jpeg;
    uint64_t frameNumber = 0;
};

static std::mutex g_frameMutex;
static std::condition_variable g_frameCv;
static FrameData g_currentFrame;
static std::atomic<int> g_viewerCount{0};
static std::atomic<double> g_currentFps{0.0};

// Web Player HTML
static const char* HTML_PAGE = R"rawhtml(<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
<title>TrimUI Brick Pro - GameCast Ultra Low Latency</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; user-select: none; }
  body {
    background: #000;
    color: #e2e8f0;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    overflow: hidden;
    width: 100vw;
    height: 100vh;
    display: flex;
    flex-direction: column;
  }
  #header {
    height: 48px;
    background: rgba(15, 23, 42, 0.9);
    backdrop-filter: blur(10px);
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 0 16px;
    border-bottom: 1px solid rgba(255,255,255,0.08);
    z-index: 10;
  }
  .brand {
    font-weight: 700;
    font-size: 15px;
    letter-spacing: 0.5px;
    color: #38bdf8;
    display: flex;
    align-items: center;
    gap: 10px;
  }
  .badge {
    background: #166534;
    color: #4ade80;
    font-size: 11px;
    padding: 2px 8px;
    border-radius: 9999px;
    font-weight: 600;
  }
  .actions {
    display: flex;
    gap: 8px;
  }
  .btn {
    border: none;
    padding: 6px 12px;
    border-radius: 6px;
    font-size: 13px;
    font-weight: 600;
    cursor: pointer;
    transition: opacity 0.15s;
  }
  .btn:active { opacity: 0.8; }
  .btn-blue { background: #0284c7; color: #fff; }
  .btn-gray { background: #334155; color: #f1f5f9; }
  .btn-red { background: #dc2626; color: #fff; }

  #stage {
    flex: 1;
    position: relative;
    width: 100vw;
    height: calc(100vh - 48px);
    display: flex;
    align-items: center;
    justify-content: center;
    background: #000;
  }
  #screen {
    max-width: 100%;
    max-height: 100%;
    aspect-ratio: 4 / 3;
    object-fit: contain;
    box-shadow: 0 10px 40px rgba(0,0,0,0.8);
    image-rendering: -webkit-optimize-contrast;
    image-rendering: pixelated;
    image-rendering: crisp-edges;
  }
  #hud {
    position: fixed;
    bottom: 12px;
    left: 50%;
    transform: translateX(-50%);
    background: rgba(15, 23, 42, 0.75);
    backdrop-filter: blur(8px);
    padding: 6px 16px;
    border-radius: 20px;
    font-size: 12px;
    color: #94a3b8;
    display: flex;
    gap: 16px;
    pointer-events: none;
    border: 1px solid rgba(255,255,255,0.06);
    z-index: 20;
  }
  .key { color: #f8fafc; font-weight: 700; background: #334155; padding: 1px 6px; border-radius: 4px; }
  :fullscreen #header, :-webkit-full-screen #header { display: none; }
  :fullscreen #stage, :-webkit-full-screen #stage { height: 100vh; }
</style>
</head>
<body>
<div id="header">
  <div class="brand">
    <span>🎮 TrimUI Brick Pro</span>
    <span class="badge" id="liveBadge">LIVE 60FPS</span>
  </div>
  <div class="actions">
    <button class="btn btn-blue" id="btnRes" onclick="toggleRes()">⚡ Đổi 512p/1024p</button>
    <button class="btn btn-gray" onclick="toggleFullscreen()">⛶ Toàn màn hình</button>
    <button class="btn btn-red" onclick="stopCast()">⏹ Tắt Stream</button>
  </div>
</div>

<div id="stage" onclick="toggleFullscreen()">
  <img id="screen" src="/stream" alt="TrimUI Screen Stream" onerror="setTimeout(() => { this.src = '/stream?t=' + Date.now(); }, 1000)">
</div>

<div id="hud">
  <span>WASD/Mũi tên: <span class="key">D-Pad</span></span>
  <span>J/K: <span class="key">A / B</span></span>
  <span>U/I: <span class="key">X / Y</span></span>
  <span>Enter/Space: <span class="key">Start / Select</span></span>
  <span>Cắm tay cầm USB/Bluetooth để chơi</span>
</div>

<script>
function toggleFullscreen() {
  if (!document.fullscreenElement && !document.webkitFullscreenElement) {
    if (document.documentElement.requestFullscreen) {
      document.documentElement.requestFullscreen();
    } else if (document.documentElement.webkitRequestFullscreen) {
      document.documentElement.webkitRequestFullscreen();
    }
  } else {
    if (document.exitFullscreen) {
      document.exitFullscreen();
    }
  }
}

function toggleRes() {
  fetch('/api/resolution', { method: 'POST' })
    .then(r => r.json())
    .then(d => {
      document.getElementById('btnRes').innerText = d.downscale ? '⚡ Đang 512p (Mượt)' : '⚡ Đang 1024p (HD)';
    }).catch(() => {});
}

function stopCast() {
  if (confirm('Bạn có muốn tắt hẳn GameCast daemon trên Brick?')) {
    fetch('/api/stop', { method: 'POST' }).then(() => {
      alert('Đã tắt GameCast.');
      window.location.reload();
    }).catch(() => {});
  }
}

// Input injection to /api/input
const keyMap = {
  'ArrowUp': 'UP', 'KeyW': 'UP',
  'ArrowDown': 'DOWN', 'KeyS': 'DOWN',
  'ArrowLeft': 'LEFT', 'KeyA': 'LEFT',
  'ArrowRight': 'RIGHT', 'KeyD': 'RIGHT',
  'KeyJ': 'A', 'KeyK': 'B',
  'KeyU': 'X', 'KeyI': 'Y',
  'Enter': 'START', 'Space': 'SELECT',
  'KeyQ': 'L1', 'KeyE': 'R1'
};

const pressedKeys = new Set();
function sendKey(btn, down) {
  fetch('/api/input', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ btn: btn, down: down ? 1 : 0 })
  }).catch(() => {});
}

window.addEventListener('keydown', (e) => {
  if (keyMap[e.code] && !pressedKeys.has(e.code)) {
    pressedKeys.add(e.code);
    sendKey(keyMap[e.code], true);
  }
});

window.addEventListener('keyup', (e) => {
  if (keyMap[e.code]) {
    pressedKeys.delete(e.code);
    sendKey(keyMap[e.code], false);
  }
});

// HTML5 Gamepad API loop
let lastGamepadState = {};
function pollGamepad() {
  const gamepads = navigator.getGamepads ? navigator.getGamepads() : [];
  for (let gp of gamepads) {
    if (!gp) continue;
    const mapping = [
      { idx: 0, btn: 'B' }, { idx: 1, btn: 'A' }, { idx: 2, btn: 'Y' }, { idx: 3, btn: 'X' },
      { idx: 4, btn: 'L1' }, { idx: 5, btn: 'R1' }, { idx: 8, btn: 'SELECT' }, { idx: 9, btn: 'START' },
      { idx: 12, btn: 'UP' }, { idx: 13, btn: 'DOWN' }, { idx: 14, btn: 'LEFT' }, { idx: 15, btn: 'RIGHT' }
    ];
    for (let m of mapping) {
      const isDown = gp.buttons[m.idx] && gp.buttons[m.idx].pressed;
      const key = gp.index + '_' + m.btn;
      if (isDown !== lastGamepadState[key]) {
        lastGamepadState[key] = isDown;
        sendKey(m.btn, isDown);
      }
    }
  }
  requestAnimationFrame(pollGamepad);
}
window.addEventListener('gamepadconnected', () => {
  pollGamepad();
});

// Live Stats HUD Updater
setInterval(async () => {
  try {
    const res = await fetch('/api/status');
    const data = await res.json();
    if (data.fps !== undefined) {
      const badge = document.getElementById('liveBadge');
      badge.textContent = 'LIVE ' + data.fps + ' FPS';
      badge.style.background = data.fps >= 25 ? '#166534' : '#854d0e';
    }
  } catch(e) {}
}, 1000);
</script>
</body>
</html>
)rawhtml";

static thread_local uint8_t* g_tlDestPtr = nullptr;
static thread_local uint8_t* g_tlDestEnd = nullptr;

static void tooJpegWriteCallback(unsigned char byte) {
    if (g_tlDestPtr < g_tlDestEnd) {
        *g_tlDestPtr++ = byte;
    }
}

static bool encodeJpeg(const uint8_t* rgbData, int width, int height, int quality, std::vector<uint8_t>& outJpeg) {
    size_t maxCap = static_cast<size_t>(width * height * 2 + 2048);
    if (outJpeg.size() < maxCap) {
        outJpeg.resize(maxCap);
    }
    g_tlDestPtr = outJpeg.data();
    g_tlDestEnd = outJpeg.data() + maxCap;
    bool ok = ::TooJpeg::writeJpeg(tooJpegWriteCallback, rgbData, (unsigned short)width, (unsigned short)height, true, (unsigned char)quality, true);
    size_t written = g_tlDestPtr - outJpeg.data();
    outJpeg.resize(written);
    g_tlDestPtr = nullptr;
    g_tlDestEnd = nullptr;
    return ok;
}

// Frame Capture Thread
static void captureWorker() {
    int fb_fd = open("/dev/fb0", O_RDONLY);
    if (fb_fd < 0) {
        std::cerr << "[GameCast] Error opening /dev/fb0" << std::endl;
        g_running = false;
        return;
    }

    struct fb_var_screeninfo vinfo;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo)) {
        close(fb_fd);
        g_running = false;
        return;
    }

    struct fb_fix_screeninfo finfo;
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo)) {
        close(fb_fd);
        g_running = false;
        return;
    }

    size_t mappedSize = finfo.smem_len;
    if (mappedSize < static_cast<size_t>(vinfo.xres * vinfo.yres * 4)) {
        mappedSize = static_cast<size_t>(vinfo.xres * vinfo.yres * 4);
    }

    uint8_t* fbp = (uint8_t*)mmap(0, mappedSize, PROT_READ, MAP_SHARED, fb_fd, 0);
    if (fbp == MAP_FAILED) {
        close(fb_fd);
        g_running = false;
        return;
    }

    int srcW = vinfo.xres;
    int srcH = vinfo.yres;

    std::vector<uint8_t> rgbBuffer;
    std::vector<uint8_t> jpegBuffer;

    auto lastFpsTime = std::chrono::steady_clock::now();
    int frameCount = 0;
    uint64_t frameSeq = 0;

    std::cout << "[GameCast] Ultra-low latency capture started (" << srcW << "x" << srcH 
              << ", buffer: " << mappedSize / 1024 << "KB)" << std::endl;

    while (g_running) {
        // IDLE SLEEP: If no client is watching the stream, do NOT burn CPU compressing frames!
        // This frees 100% of CPU for emulators when daemon is idling in background.
        if (g_viewerCount.load() <= 0) {
            int fnFd = open("/sys/class/gpio/gpio243/value", O_RDONLY);
            if (fnFd >= 0) {
                char val = 0;
                if (read(fnFd, &val, 1) > 0 && val == '0') {
                    close(fnFd);
                    sigHandler(SIGTERM);
                    break;
                }
                close(fnFd);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        auto frameStart = std::chrono::steady_clock::now();

        // Dynamically re-check vscreeninfo to track active buffer panning (RetroArch double/triple buffering)
        if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
            srcW = vinfo.xres;
            srcH = vinfo.yres;
        }

        bool downscale = g_downscale.load();
        int targetW = downscale ? (srcW / 2) : srcW;
        int targetH = downscale ? (srcH / 2) : srcH;
        int quality = 50; // Optimized compression: ~12KB frames for sub-5ms Wi-Fi delivery

        if (rgbBuffer.size() != static_cast<size_t>(targetW * targetH * 3)) {
            rgbBuffer.resize(targetW * targetH * 3);
        }

        // Calculate active display offset using line_length and yoffset
        size_t bpp = (vinfo.bits_per_pixel > 0) ? (vinfo.bits_per_pixel / 8) : 4;
        size_t lineLength = (finfo.line_length > 0) ? finfo.line_length : (srcW * bpp);
        size_t activeOffset = ((size_t)vinfo.yoffset * lineLength) + ((size_t)vinfo.xoffset * bpp);

        if (activeOffset + ((size_t)srcH * lineLength) > mappedSize) {
            activeOffset = 0; // Guard against out-of-bounds
        }

        const uint8_t* activeFb = fbp + activeOffset;
        uint8_t* dst = rgbBuffer.data();

        if (downscale) {
            for (int y = 0; y < targetH; ++y) {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(activeFb + (y * 2) * lineLength);
                for (int x = 0; x < targetW; ++x) {
                    uint32_t p = row[x * 2];
                    *dst++ = (p >> 16) & 0xFF; // R
                    *dst++ = (p >> 8) & 0xFF;  // G
                    *dst++ = p & 0xFF;         // B
                }
            }
        } else {
            for (int y = 0; y < targetH; ++y) {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(activeFb + y * lineLength);
                for (int x = 0; x < targetW; ++x) {
                    uint32_t p = row[x];
                    *dst++ = (p >> 16) & 0xFF; // R
                    *dst++ = (p >> 8) & 0xFF;  // G
                    *dst++ = p & 0xFF;         // B
                }
            }
        }

        // Fast JPEG Compress
        if (encodeJpeg(rgbBuffer.data(), targetW, targetH, quality, jpegBuffer)) {
            std::lock_guard<std::mutex> lock(g_frameMutex);
            g_currentFrame.jpeg = std::move(jpegBuffer);
            g_currentFrame.frameNumber = ++frameSeq;
            g_frameCv.notify_all();
        }

        frameCount++;
        static bool s_fnWasOn = false;
        if ((frameCount % 30) == 0) {
            int fnFd = open("/sys/class/gpio/gpio243/value", O_RDONLY);
            if (fnFd >= 0) {
                char val = 0;
                if (read(fnFd, &val, 1) > 0) {
                    if (val == '1') {
                        s_fnWasOn = true;
                    } else if (val == '0' && s_fnWasOn) {
                        std::cout << "[GameCast] Hardware FN switch turned OFF -> Terminating daemon..." << std::endl;
                        close(fnFd);
                        sigHandler(SIGTERM);
                    }
                }
                close(fnFd);
            }
        }
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<double> elapsed = now - lastFpsTime;
        if (elapsed.count() >= 1.0) {
            g_currentFps = frameCount / elapsed.count();
            frameCount = 0;
            lastFpsTime = now;
        }

        // Throttle: 60 FPS for downscale (~16ms), ~25 FPS for native (~40ms)
        // 512x384 downscale runs at ultra-smooth 60 FPS with minimal CPU load
        auto frameDuration = std::chrono::duration_cast<std::chrono::milliseconds>(now - frameStart).count();
        int targetMs = downscale ? 16 : 40;
        if (frameDuration < targetMs) {
            std::this_thread::sleep_for(std::chrono::milliseconds(targetMs - frameDuration));
        }
    }

    munmap(fbp, mappedSize);
    close(fb_fd);
}

static void sigHandler(int) {
    g_running = false;
    g_frameCv.notify_all();
    int sfd = g_serverFd.exchange(-1);
    if (sfd >= 0) {
        close(sfd);
    }
    g_gamepad.destroy();
    unlink(PID_FILE);
    _exit(0);
}

// Handle HTTP Client Connection
static void handleClient(int clientFd) {
    char reqBuf[2048];
    int n = recv(clientFd, reqBuf, sizeof(reqBuf) - 1, 0);
    if (n <= 0) {
        close(clientFd);
        return;
    }
    reqBuf[n] = '\0';

    std::string req(reqBuf);

    // Route: GET / or GET /cast or HEAD -> HTML Page
    if (req.find("GET / ") == 0 || req.find("GET /cast") == 0 || req.find("HEAD /") == 0) {
        std::string html = HTML_PAGE;
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/html; charset=utf-8\r\n"
                          "Content-Length: " + std::to_string(html.size()) + "\r\n"
                          "Connection: close\r\n\r\n";
        if (req.find("HEAD ") != 0) {
            res += html;
        }
        send(clientFd, res.c_str(), res.size(), 0);
        close(clientFd);
        return;
    }

    // Route: GET /api/status -> JSON
    if (req.find("GET /api/status") == 0) {
        std::string json = "{\"running\":true,\"fps\":" + std::to_string(static_cast<int>(g_currentFps.load())) +
                           ",\"viewers\":" + std::to_string(g_viewerCount.load()) +
                           ",\"downscale\":" + (g_downscale.load() ? "true" : "false") + "}\r\n";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.size()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.size(), 0);
        close(clientFd);
        return;
    }

    // Route: POST /api/resolution -> Live on-the-fly resolution toggle
    if (req.find("POST /api/resolution") == 0) {
        if (req.find("mode=native") != std::string::npos) {
            g_downscale = false;
        } else if (req.find("mode=downscale") != std::string::npos) {
            g_downscale = true;
        } else {
            g_downscale = !g_downscale.load();
        }
        std::string json = "{\"ok\":true,\"downscale\":" + std::string(g_downscale.load() ? "true" : "false") + "}\r\n";
        std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.size(), 0);
        close(clientFd);
        return;
    }

    // Route: POST /api/stop -> Stop daemon cleanly
    if (req.find("POST /api/stop") == 0) {
        std::string res = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
        send(clientFd, res.c_str(), res.size(), 0);
        close(clientFd);
        sigHandler(SIGTERM);
        return;
    }

    // Route: POST /api/input -> Parse button and inject to uinput
    if (req.find("POST /api/input") == 0) {
        size_t bodyPos = req.find("\r\n\r\n");
        if (bodyPos != std::string::npos) {
            std::string body = req.substr(bodyPos + 4);
            int isDown = (body.find("\"down\":1") != std::string::npos || body.find("\"down\": 1") != std::string::npos) ? 1 : 0;
            
            int btnCode = -1;
            if (body.find("\"A\"") != std::string::npos) btnCode = BTN_A;
            else if (body.find("\"B\"") != std::string::npos) btnCode = BTN_B;
            else if (body.find("\"X\"") != std::string::npos) btnCode = BTN_X;
            else if (body.find("\"Y\"") != std::string::npos) btnCode = BTN_Y;
            else if (body.find("\"START\"") != std::string::npos) btnCode = BTN_START;
            else if (body.find("\"SELECT\"") != std::string::npos) btnCode = BTN_SELECT;
            else if (body.find("\"UP\"") != std::string::npos) btnCode = BTN_DPAD_UP;
            else if (body.find("\"DOWN\"") != std::string::npos) btnCode = BTN_DPAD_DOWN;
            else if (body.find("\"LEFT\"") != std::string::npos) btnCode = BTN_DPAD_LEFT;
            else if (body.find("\"RIGHT\"") != std::string::npos) btnCode = BTN_DPAD_RIGHT;
            else if (body.find("\"L1\"") != std::string::npos) btnCode = BTN_TL;
            else if (body.find("\"R1\"") != std::string::npos) btnCode = BTN_TR;

            if (btnCode >= 0) {
                if (!g_gamepad.isInitialized()) {
                    g_gamepad.init();
                }
                g_gamepad.emit(btnCode, isDown);
            }
        }
        std::string res = "HTTP/1.1 200 OK\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
        send(clientFd, res.c_str(), res.size(), 0);
        close(clientFd);
        return;
    }

    // Route: GET /stream -> Ultra Low Latency MJPEG Stream
    if (req.find("GET /stream") == 0) {
        g_viewerCount++;

        // 1. Disable Nagle's algorithm for sub-10ms packet dispatch
        int one = 1;
        setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        // 2. Small 32KB send buffer: strictly 1-2 frames to prevent network buffer bloat and lag
        int sndBuf = 32768; // 32KB
        setsockopt(clientFd, SOL_SOCKET, SO_SNDBUF, &sndBuf, sizeof(sndBuf));

        std::string header = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                             "Cache-Control: no-cache, no-store, must-revalidate, max-age=0\r\n"
                             "Pragma: no-cache\r\n"
                             "Expires: 0\r\n"
                             "Access-Control-Allow-Origin: *\r\n"
                             "Connection: close\r\n\r\n";
        if (send(clientFd, header.c_str(), header.size(), MSG_NOSIGNAL) <= 0) {
            close(clientFd);
            g_viewerCount--;
            return;
        }

        uint64_t lastFrameNumber = 0;
        std::vector<uint8_t> sendPacket;
        sendPacket.reserve(131072);

        while (g_running) {
            std::vector<uint8_t> frameJpeg;
            {
                std::unique_lock<std::mutex> lock(g_frameMutex);
                g_frameCv.wait(lock, [&]() {
                    return !g_running || g_currentFrame.frameNumber > lastFrameNumber;
                });
                if (!g_running) break;
                frameJpeg = g_currentFrame.jpeg;
                lastFrameNumber = g_currentFrame.frameNumber;
            }

            if (frameJpeg.empty()) continue;

            // Non-blocking poll: check if client socket is ready to receive
            // If socket is congested/lagging, DROP this frame immediately so latency never accumulates!
            struct pollfd pfd;
            pfd.fd = clientFd;
            pfd.events = POLLOUT;
            int pret = poll(&pfd, 1, 4); // max 4ms wait
            if (pret <= 0 || !(pfd.revents & POLLOUT)) {
                continue; // Drop stale frame to preserve zero latency
            }

            // Single contiguous buffer send
            char partHeader[128];
            int phLen = std::snprintf(partHeader, sizeof(partHeader),
                                      "--frame\r\n"
                                      "Content-Type: image/jpeg\r\n"
                                      "Content-Length: %zu\r\n\r\n", frameJpeg.size());

            sendPacket.clear();
            sendPacket.insert(sendPacket.end(), partHeader, partHeader + phLen);
            sendPacket.insert(sendPacket.end(), frameJpeg.begin(), frameJpeg.end());
            sendPacket.push_back('\r');
            sendPacket.push_back('\n');

            // Send complete frame without truncation
            size_t totalBytes = sendPacket.size();
            size_t totalSent = 0;
            bool clientDead = false;

            while (totalSent < totalBytes && g_running) {
                ssize_t sent = send(clientFd, sendPacket.data() + totalSent, totalBytes - totalSent, MSG_NOSIGNAL);
                if (sent > 0) {
                    totalSent += sent;
                } else if (sent < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        struct pollfd spfd;
                        spfd.fd = clientFd;
                        spfd.events = POLLOUT;
                        int sret = poll(&spfd, 1, 50);
                        if (sret <= 0) {
                            clientDead = true;
                            break;
                        }
                        continue;
                    }
                    clientDead = true;
                    break;
                } else {
                    clientDead = true;
                    break;
                }
            }

            if (clientDead) {
                break; // Client disconnected
            }
        }

        g_viewerCount--;
        close(clientFd);
        return;
    }

    // 404 Not Found
    std::string res = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot Found";
    send(clientFd, res.c_str(), res.size(), 0);
    close(clientFd);
}

} // namespace GameCast

int main(int argc, char** argv) {
    signal(SIGINT, GameCast::sigHandler);
    signal(SIGTERM, GameCast::sigHandler);
    signal(SIGPIPE, SIG_IGN);

    bool downscale = true;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--native") {
            downscale = false;
        } else if (std::string(argv[i]) == "--downscale") {
            downscale = true;
        }
    }
    GameCast::g_downscale = downscale;

    // Write PID file
    FILE* pf = std::fopen(GameCast::PID_FILE, "w");
    if (pf) {
        std::fprintf(pf, "%d\n", getpid());
        std::fclose(pf);
    }

    // Start screen capture worker
    std::thread capThread(GameCast::captureWorker);

    // Setup HTTP Server Socket
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        std::cerr << "[GameCast] Failed to create socket" << std::endl;
        GameCast::g_running = false;
        capThread.join();
        return 1;
    }

    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(GameCast::HTTP_PORT);

    if (bind(serverFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[GameCast] Failed to bind port " << GameCast::HTTP_PORT << std::endl;
        close(serverFd);
        GameCast::g_running = false;
        capThread.join();
        return 1;
    }

    if (listen(serverFd, 10) < 0) {
        std::cerr << "[GameCast] Failed to listen on socket" << std::endl;
        close(serverFd);
        GameCast::g_running = false;
        capThread.join();
        return 1;
    }

    GameCast::g_serverFd = serverFd;

    std::cout << "==================================================" << std::endl;
    std::cout << "[GameCast] Server listening on port " << GameCast::HTTP_PORT << std::endl;
    std::cout << "[GameCast] Open on TV / Laptop browser: http://<IP>:" << GameCast::HTTP_PORT << "/cast" << std::endl;
    std::cout << "==================================================" << std::endl;

    while (GameCast::g_running) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(serverFd, (struct sockaddr*)&clientAddr, &clientLen);
        if (clientFd >= 0) {
            std::thread(GameCast::handleClient, clientFd).detach();
        }
    }

    close(serverFd);
    if (capThread.joinable()) capThread.join();
    GameCast::g_gamepad.destroy();
    unlink(GameCast::PID_FILE);

    std::cout << "[GameCast] Daemon exited cleanly." << std::endl;
    return 0;
}
