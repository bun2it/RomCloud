# Plan: RomCloud PC Simulator

## Context

Build system hiện tại dùng **Zig cross-compile** cho ARM Linux (TrimUI). Muốn thêm **native PC build** để test UI nhanh trên PC.

## Phân tích Code Hiện tại

### InputManager đã hỗ trợ Keyboard ✅
[InputManager.cpp](src/input/InputManager.cpp) đã có keyboard mapping đầy đủ:
- Arrow keys → D-pad
- Enter/A/Space → Button::A
- ESC/B/Backspace → Button::B
- X → Button::X, Y → Button::Y
- Q/E → L1/R1, TAB → SELECT, F1 → START

### SDL Window Creation
[Application.cpp:51-58](src/app/Application.cpp#L51-L58):
```cpp
m_window = SDL_CreateWindow(
    "RomCloud",
    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
    1024, 768,
    SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP
);
```

## Implementation - CHỈ 3 THAY ĐỔI

### 1. Sửa Application.cpp - Thêm flag và wrap window creation

```cpp
// Thêm define ở đầu file hoặc truyền qua compiler flag
#define PC_SIMULATOR_MODE

void Application::initSDL() {
    // ... SDL_Init ...

#ifdef PC_SIMULATOR_MODE
    // PC: Cửa sổ 1024x768 có thể resize
    m_window = SDL_CreateWindow(
        "RomCloud Simulator [1024x768]",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1024, 768,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    SDL_ShowCursor(SDL_ENABLE);  // Hiện cursor chuột
#else
    // TrimUI: Fullscreen
    m_window = SDL_CreateWindow(
        "RomCloud", 0, 0, 1024, 768,
        SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN
    );
    SDL_ShowCursor(SDL_DISABLE);
#endif
    // ... rest giữ nguyên ...
}
```

### 2. Tạo build script đơn giản

**`build_pc.sh`:**
```bash
#!/bin/bash
set -e
cd "$(dirname "$0")"

echo "=== Building RomCloud for PC ==="
mkdir -p bin

g++ -std=c++17 -O2 -Wall -Wextra \
    -DPC_SIMULATOR_MODE \
    -I/usr/local/include \
    -Isrc \
    src/main.cpp \
    src/app/Application.cpp \
    src/ui/UIManager.cpp \
    src/ui/UiRenderer.cpp \
    src/ui/CoverManager.cpp \
    src/ui/ExplorerInput.cpp \
    src/ui/ExplorerRender.cpp \
    src/ui/ExplorerRenderKb.cpp \
    src/network/HttpClient.cpp \
    src/network/WebServer.cpp \
    src/input/InputManager.cpp \
    src/filesystem/FileSystemManager.cpp \
    src/platform/PlatformInfo.cpp \
    src/logging/Logger.cpp \
    src/logging/IssueLogger.cpp \
    src/config/AppConfig.cpp \
    src/database/DatabaseManager.cpp \
    src/database/RomIndexer.cpp \
    src/localsend/LocalSendManager.cpp \
    -L/usr/local/lib \
    -lSDL2 -lSDL2_image -lSDL2_ttf \
    -lsqlite3 -lcurl -lssl -lcrypto -lpthread -lm \
    -o bin/RomCloud

echo "Done: bin/RomCloud"
```

### 3. Tạo thư mục test data

```bash
mkdir -p pc_data/mnt/SDCARD
mkdir -p pc_data/mnt/SDCARD/Roms/GBA
mkdir -p pc_data/mnt/SDCARD/Roms/NES
mkdir -p pc_data/mnt/SDCARD/Downloads
mkdir -p pc_data/mnt/SDCARD/.romcloud
mkdir -p pc_data/mnt/SDCARD/Covers
mkdir -p pc_data/mnt/SDCARD/assets/apps_icons
cp -r assets/fonts pc_data/mnt/SDCARD/
```

## Files cần tạo/sửa

| File | Action |
|------|--------|
| `.claude/settings.json` | Tạo mới - Custom commands |
| `build_pc.sh` | Tạo mới |
| `src/app/Application.cpp` | Sửa ~10 dòng |

## Custom Commands (Settings)

Thêm vào `.claude/settings.json`:

```json
{
  "slashCommands": {
    "build-pc": {
      "description": "Build RomCloud for PC simulator",
      "command": "chmod +x build_pc.sh && ./build_pc.sh && echo '\\n=== Running RomCloud ===' && mkdir -p pc_data && ./bin/RomCloud ./pc_data"
    },
    "adb": {
      "description": "Build ARM, push to TrimUI and run",
      "command": "./build.sh && adb push bin/RomCloud /mnt/SDCARD/Apps/RomCloud/ && adb shell 'cd /mnt/SDCARD/Apps/RomCloud && killall RomCloud 2>/dev/null; ./RomCloud /mnt/SDCARD &'"
    }
  }
}
```

**Cách dùng:**
- Gõ `/build-pc` → Build PC + chạy simulator
- Gõ `/adb` → Build ARM + push + chạy trên TrimUI

## Build Instructions

### macOS
```bash
brew install sdl2 sdl2_image sdl2_ttf
chmod +x build_pc.sh && ./build_pc.sh
mkdir -p pc_data && ./bin/RomCloud ./pc_data
```

### Linux
```bash
sudo apt install libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev libsqlite3-dev
chmod +x build_pc.sh && ./build_pc.sh
./bin/RomCloud ./pc_data
```

### Windows (MSYS2)
```bash
pacman -S mingw-w64-x86_64-SDL2 mingw-w64-x86_64-SDL2_image mingw-w64-x86_64-SDL2_ttf
./build_pc.sh
./bin/RomCloud.exe ./pc_data
```

## Keyboard Controls (đã có sẵn)

| Phím PC | Tay cầm TrimUI |
|---------|----------------|
| Arrow keys | D-pad |
| Enter | A (Chọn) |
| ESC | B (Lùi) |
| X | X button |
| Y | Y button |
| Q | L1 |
| E | R1 |
| TAB | SELECT |
| F1 | START |

## Verification

1. `mkdir -p bin && ./build_pc.sh` → build thành công
2. `./bin/RomCloud ./pc_data` → window 1024x768 hiện lên
3. Arrow keys di chuyển được trong UI
4. Enter bấm được menu
5. ESC thoát được ra
