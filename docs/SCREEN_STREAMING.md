# Plan: Dual-Screen Streaming - Brick ↔ Browser/PC

## Context

TrimUI Brick Pro có màn hình 3.5" 1024x768. Muốn tận dụng làm:
1. **Secondary Display** cho PC/Mac (giống Spacedesk)
2. **Game Streaming** - Cast gameplay lên browser trên PC

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                         TRIMUI BRICK                        │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐     │
│  │   Mode 1:   │    │   Mode 2:   │    │   Mode 3:   │     │
│  │  Secondary  │    │  Game Cast  │    │  Standalone │     │
│  │   Display   │◀───│   Browser   │───▶│   Normal    │     │
│  │  (Receive)  │    │   (Send)    │    │   Usage     │     │
│  └─────────────┘    └─────────────┘    └─────────────┘     │
│         │                  │                  │              │
│         └──────────────────┼──────────────────┘              │
│                            │                               │
│                    ┌────────▼────────┐                       │
│                    │  StreamHub     │                       │
│                    │  - MJPEG Server│                       │
│                    │  - UDP Receiver │                      │
│                    │  - WebSocket   │                       │
│                    └────────────────┘                       │
└─────────────────────────────────────────────────────────────┘
                            │
        ┌───────────────────┼───────────────────┐
        ▼                   ▼                   ▼
┌───────────────┐   ┌───────────────┐   ┌───────────────┐
│     PC        │   │    PC        │   │   Browser     │
│  ┌─────────┐  │   │  ┌─────────┐ │   │  ┌─────────┐ │
│  │ Stream  │──┼───│──│ Stream  │─┼───│──│ Display │ │
│  │ Content │  │   │  │ Gameplay│ │   │  │ Screen  │ │
│  └─────────┘  │   │  └─────────┘ │   │  └─────────┘ │
│   Keyboard ───┼───┼─── Keyboard ──┼───┼─────────────│
└───────────────┘   └───────────────┘   └─────────────┘
```

## Two Main Modes

### Mode 1: Secondary Display (PC → Brick)
**Use Case:** Dùng TrimUI làm màn hình phụ cho PC
- PC stream nội dung xuống TrimUI
- TrimUI chỉ hiển thị (nhận dữ liệu)
- Không cần input từ TrimUI

**Tech:**
- UDP/RTP stream từ PC
- TrimUI nhận và hiển thị frame
- Có thể dùng ffmpeg để encode

### Mode 2: Game Cast (Brick → Browser)
**Use Case:** Chơi game trên TrimUI, xem và điều khiển từ browser
- TrimUI đang chạy game/emulator
- Stream lên browser trên PC
- Keyboard input từ browser gửi về TrimUI

**Tech:**
- MJPEG/H264 streaming
- WebSocket cho input
- ~15fps, 100-200ms latency acceptable

### Mode 3: Standalone (Normal)
- RomCloud hoạt động bình thường
- Không có streaming

## Implementation

### 1. StreamHub - Central Controller

```cpp
// src/screen/StreamHub.h
enum class StreamMode {
    Off,
    SecondaryDisplay,  // PC → Brick
    GameCast          // Brick → Browser
};

class StreamHub {
public:
    static StreamHub& instance();
    
    void setMode(StreamMode mode);
    StreamMode getMode() const;
    
    // Mode 1: Receiver
    void startReceiving(int port);
    void stopReceiving();
    
    // Mode 2: Sender
    void startBroadcasting(int port);
    void stopBroadcasting();
    void captureFrame();
    
    // Shared
    void renderFrame();  // Gọi mỗi frame
    
private:
    StreamMode m_mode = StreamMode::Off;
    UDPServer* m_udpReceiver;
    MJPEGServer* m_mjpegServer;
    std::vector<uint8_t> m_frameBuffer;
};
```

### 2. Mode 1: Secondary Display Receiver

```
PC                                    TrimUI
 │                                      │
 │  ffmpeg -re -i input -c:v libx264   │
 │        -f mpegts udp://IP:5000       │
 │                                      │
 ├──────────────────────────────────────▶
 │        UDP Stream (H264/MJPEG)       │
 │                                      │
 │  TrimUI nhận → decode → SDL blit    │
 │  Hiển thị lên màn hình              │
 │                                      │
```

**Receiver Implementation:**
```cpp
void StreamHub::startReceiving(int port) {
    m_udpReceiver = new UDPServer(port);
    // Decode H264/MJPEG → SDL Texture
    // Blit lên fullscreen
}
```

### 3. Mode 2: Game Cast Streaming

**MJPEG Server cho Browser:**
```
Brick                                          Browser
 │                                               │
 │  SDL_RenderReadPixels() → JPEG               │
 │  ─────────────────────────────────────────── │
 │  HTTP/1.1 200 OK                             │
 │  Content-Type: multipart/x-mixed-replace      │
 │                                               │
 ├─────────────────────────────────────────────▶
 │           MJPEG Stream (~15fps)              │
 │                                               │
 │  ◀─── WebSocket: {key: "ArrowUp", down: true}│
 │        Keyboard Input                         │
 │                                               │
```

**Browser Client (web/cast.html):**
```html
<video id="screen" style="width:100%">
<script>
// MJPEG stream
const img = document.getElementById('screen');
img.src = 'http://IP:8080/stream';

// Keyboard input
document.addEventListener('keydown', (e) => {
    ws.send(JSON.stringify({
        key: e.code,
        pressed: true,
        timestamp: Date.now()
    }));
});
</script>
```

### 4. Mode Switching UI

**Trong RomCloud Settings:**
```
┌─────────────────────────────────────┐
│         Screen Streaming            │
├─────────────────────────────────────┤
│  [OFF] [Display] [Game Cast]        │
├─────────────────────────────────────┤
│  Mode: Game Cast                    │
│  URL: http://192.168.1.50:8080      │
│  ┌─────────────────────────────┐    │
│  │    [QR Code]               │    │
│  │  Scan để kết nối nhanh     │    │
│  └─────────────────────────────┘    │
│  FPS: 15  [▼] Quality: 70%  [▼]   │
│                                     │
│  Keyboard Layout: [▼]              │
│  - WASD + Space + E/F              │
│  - Arrow Keys + ZXC                │
│                                     │
└─────────────────────────────────────┘
```

### 5. Frame Sync Strategy

**Cho Mode 2 (Game Cast):**
```cpp
void StreamHub::captureFrame() {
    // 1. Render UI/Game bình thường
    // 2. Capture framebuffer
    SDL_RenderReadPixels(renderer, NULL, 
        SDL_PIXELFORMAT_RGB888, pixels);
    
    // 3. Encode JPEG
    encodeJPEG(pixels, width, height, quality);
    
    // 4. Gửi cho browser (non-blocking)
    m_mjpegServer->enqueueFrame(jpegData);
    
    // 5. Xử lý input từ browser
    while (auto cmd = m_wsServer->getInput()) {
        InputManager::instance().injectButton(cmd->button, cmd->pressed);
    }
}
```

## Files to Create

| File | Purpose |
|------|---------|
| `src/screen/StreamHub.h/cpp` | Central controller |
| `src/screen/UDPServer.h/cpp` | UDP receiver for Mode 1 |
| `src/screen/MJPEGServer.h/cpp` | MJPEG server for Mode 2 |
| `src/screen/JPEGEncoder.h/cpp` | JPEG encoding |
| `web/cast.html` | Browser client |
| `web/display.html` | Display mode (future) |

## Files to Modify

| File | Change |
|------|--------|
| `src/app/Application.cpp` | Integrate StreamHub in render loop |
| `src/config/AppConfig.cpp` | Add streaming settings |
| `src/ui/UIManager.cpp` | Add streaming settings UI |
| `src/input/InputManager.cpp` | Add `injectButton()` for remote input |

## Configuration

```json
// config.json
{
  "streaming": {
    "mode": "off",  // off | display | gamecast
    "port": 8080,
    "fps": 15,
    "quality": 70,
    "resolution": "full"  // full | half
  }
}
```

## User Flows

### Flow 1: Use Brick as Display
```
1. PC: Chạy ffmpeg stream
   ffmpeg -re -i video.mp4 -c:v libx264 -f mpegts udp://BRICK_IP:5000

2. Brick: Settings → Streaming → [Display]
   Hiển thị "Listening on port 5000..."

3. PC: Bắt đầu stream
   → Brick hiển thị nội dung

4. Tắt: Settings → Streaming → [Off]
```

### Flow 2: Cast Game to Browser
```
1. Brick: Chạy game/emulator bình thường

2. Brick: Settings → Streaming → [Game Cast]
   Hiển thị URL + QR Code

3. PC: Mở browser, vào URL hoặc quét QR
   → Thấy màn hình game

4. PC: Bấm keyboard
   → Game trên Brick phản hồi

5. Tắt: Settings → Streaming → [Off]
```

## Performance Targets

| Metric | Target |
|--------|--------|
| Latency | 100-200ms (acceptable for UI) |
| FPS | 10-15fps for streaming |
| CPU Usage | < 30% on ARM |
| Memory | < 50MB additional |

## Verification

### Mode 1 (Display):
1. Start streaming on PC
2. See content on Brick
3. Stop streaming → Brick returns to normal

### Mode 2 (Game Cast):
1. Open browser → See Brick screen
2. Press keyboard → Game responds
3. 15fps stable stream

## Future Enhancements

1. **Audio streaming** - Sync audio with video
2. **Touch input** - Mouse clicks → touch events
3. **Multi-viewer** - Multiple browsers can watch
4. **Recording** - Save stream to file
5. **H.264 encoding** - Lower bandwidth than MJPEG

## Phân tích Code Hiện tại

### WebServer đã tồn tại
[src/network/WebServer.cpp](src/network/WebServer.cpp) - HTTP server nhẹ dùng cho LocalSend

### UI Rendering
- SDL_Renderer → `SDL_RenderReadPixels()` để đọc framebuffer
- JPEG encode (có thể dùng `IMG_SaveJPG` từ SDL2_image)

### InputManager đã có keyboard mapping
[src/input/InputManager.cpp](src/input/InputManager.cpp):
- Arrow keys → D-pad
- Enter → A, ESC → B
- X → X, Y → Y

## Implementation

### 1. Tạo `ScreenStreamer` class

**`src/screen/ScreenStreamer.h`:**
```cpp
class ScreenStreamer {
public:
    static ScreenStreamer& instance();
    
    void start(int port = 8080);
    void stop();
    void captureAndStream();  // Gọi mỗi frame
    
    void setInputCallback(std::function<void(InputManager::Button)> callback);
    
private:
    int m_port;
    bool m_running;
    std::thread m_streamThread;
    int m_clientFd;
    std::vector<char> m_jpegBuffer;
};
```

### 2. MJPEG Streaming Protocol

HTTP response với multipart:
```
HTTP/1.1 200 OK
Content-Type: multipart/x-mixed-replace; boundary=frame
Connection: close
Cache-Control: no-cache

--frame
Content-Type: image/jpeg
Content-Length: 12345

[JPEG DATA]

--frame
Content-Type: image/jpeg
...
```

### 3. Browser Client (HTML/JS)

**`web/screen.html`** - Web page đơn giản:
```html
<img id="screen" style="width:100%">
<script>
  document.getElementById('screen').src = 'http://IP:8080/stream';
  
  // Keyboard forwarding
  document.addEventListener('keydown', (e) => {
    fetch('/input', {
      method: 'POST',
      body: JSON.stringify({key: e.key, pressed: true})
    });
  });
</script>
```

### 4. Integration

**Trong `Application.cpp`:**
```cpp
// Sau khi init xong
#ifdef ENABLE_SCREEN_STREAMING
    ScreenStreamer::instance().start(8080);
    ScreenStreamer::instance().setInputCallback([](InputManager::Button btn) {
        InputManager::instance().injectButton(btn, true);
    });
#endif

// Trong game loop
void Application::renderFrame() {
    // Render bình thường...
    m_uiManager->render();
    
#ifdef ENABLE_SCREEN_STREAMING
    ScreenStreamer::instance().captureAndStream();
#endif
}
```

### 5. Frame Capture Flow

```
1. SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGB888, pixels)
2. Convert RGB888 → JPEG (IMG_SaveJPG_RW với custom dest)
3. Nếu có client kết nối → send JPEG qua socket
4. Repeat ~10-15 FPS (không cần 60fps cho UI)
```

## Files cần tạo

| File | Mô tả |
|------|-------|
| `src/screen/ScreenStreamer.h` | Header |
| `src/screen/ScreenStreamer.cpp` | Implementation MJPEG server |
| `web/screen.html` | Browser client page |

## Files cần sửa

| File | Thay đổi |
|------|----------|
| `src/app/Application.cpp` | Gọi ScreenStreamer trong loop |
| `CMakeLists.txt` hoặc `build.sh` | Thêm source files |

## Configuration

**`AppConfig` thêm:**
```cpp
// Bật/tắt streaming
bool isScreenStreamingEnabled();
void setScreenStreamingEnabled(bool enabled);
int getScreenStreamingPort(); // default 8080
```

**File `config.json`:**
```json
{
  "screenStreaming": {
    "enabled": false,
    "port": 8080,
    "fps": 15
  }
}
```

## User Flow

```
1. User bật "Screen Streaming" trong Settings
2. RomCloud hiển thị IP: http://192.168.1.50:8080
3. User mở browser trên PC, nhập IP đó
4. Thấy màn hình TrimUI, điều khiển = keyboard
5. Muốn tắt → vào Settings tắt
```

## Performance Considerations

- **FPS:** 10-15fps đủ cho UI navigation
- **Resolution:** Có thể scale down 50% (512x384) để giảm bandwidth
- **JPEG Quality:** 60-70% để giảm size
- **Encoding:** Dùng SIMD jpeg encoder nếu cần

## Verification

1. Bật streaming, truy cập browser → thấy hình
2. Bấm keyboard trên PC → UI TrimUI phản hồi
3. Tắt streaming → browser mất kết nối
4. FPS ổn định 10-15fps

## Future Enhancements (Optional)

1. **Audio streaming** - Stream âm thanh
2. **Touch emulation** - Mouse click → touch events
3. **QR Code quick connect** - Hiện QR code chứa URL
4. **Multiple viewers** - Nhiều browser cùng xem
