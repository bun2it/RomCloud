#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

ZIG=""
if command -v zig &>/dev/null; then
    ZIG="zig"
elif [ -f "C:/Users/Tai/zig013/zig-windows-x86_64-0.13.0/zig.exe" ]; then
    ZIG="C:/Users/Tai/zig013/zig-windows-x86_64-0.13.0/zig.exe"
elif [ -f "C:/zig/zig.exe" ]; then
    ZIG="C:/zig/zig.exe"
elif [ -f "/Users/tai/.gemini/antigravity-ide/brain/00d3b56c-d55c-4262-81a8-0cf5fe35825f/tools/zig-macos-aarch64-0.13.0/zig" ]; then
    ZIG="/Users/tai/.gemini/antigravity-ide/brain/00d3b56c-d55c-4262-81a8-0cf5fe35825f/tools/zig-macos-aarch64-0.13.0/zig"
fi
if [ -z "$ZIG" ]; then
    echo "ERROR: zig not found. Install zig or set PATH." >&2
    exit 1
fi

echo "=== Compiling RomCloud for TrimUI Brick Pro (aarch64-linux-gnu.2.33) ==="
mkdir -p bin

# NetSurf stack REMOVED (Chrome-only browser): no libnetsurf build.
# (Old BUILD_LIBNETSURF block + scripts/build-libnetsurf.py deleted.)

# Release mode: RELEASE=1 ./build.sh
#   - Build nhu dev (giu debug info), copy ban debug sang bin/*.debug,
#   - roi build lai voi -s (strip luc link) cho binary chinh (nhe ~4-5MB).
#   - (zig objcopy --strip-all chua ho tro target nay -> dung -s.)
#   - Mac dinh (dev) GIU NGUYEN debug info de debug crash.
#   - bin/*.debug da co trong .gitignore, khong lot vao git/zip.
#   - Co the truyen STRIPFLAG="-s" de strip ngay lan build dau.
RELEASE="${RELEASE:-0}"
STRIPFLAG="${STRIPFLAG:-}"

# Detect git commit hash for build-id tagging in debug.log
GIT_SHA="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "Build commit: $GIT_SHA"

mkdir -p build

$ZIG c++ \
    -target aarch64-linux-gnu.2.33 \
    -std=c++17 \
    -O3 \
    $STRIPFLAG \
    -Wall -Wextra \
    -Wno-error=date-time \
    -DGIT_COMMIT_HASH=\"$GIT_SHA\" \
    -Isrc \
    -Isysroot/include \
    -Isysroot/include/SDL2 \
    src/main.cpp \
    src/app/Application.cpp \
    src/ui/UIManager.cpp \
    src/ui/BrickPadUI.cpp \
    src/brickpad/BrickPadStore.cpp \
    src/brickpad/BrickPadBt.cpp \
    src/brickpad/BrickPadManager.cpp \
    src/ui/UiRenderer.cpp \
    src/ui/UiStrings.cpp \
    src/ui/ExplorerSync.cpp \
    src/weather/WeatherManager.cpp \
    src/calendar/CalManager.cpp \
    src/ui/WeatherUI.cpp \
    src/clock/ClockStore.cpp \
    src/ui/ClockUI.cpp \
    src/screensaver/SaverEngine.cpp \
    src/market/WatchManager.cpp \
    src/ui/WatchUI.cpp \
    src/market/MarketManager.cpp \
    src/ui/MarketUI.cpp \
    src/camera/CameraManager.cpp \
    src/ui/TrafficUI.cpp \
    src/wifi/WifiManager.cpp \
    src/ui/WifiUI.cpp \
    src/flood/FloodManager.cpp \
    src/tide/TideManager.cpp \
    src/ui/FloodUI.cpp \
    src/ui/ExplorerInput.cpp \
    src/ui/ExplorerRender.cpp \
    src/ui/ExplorerRenderKb.cpp \
    src/ui/DialogManager.cpp \
    src/ui/CoverManager.cpp \
    src/ui/SearchInputModal.cpp \
    src/ui/BoxartScraper.cpp \
    src/ui/QrRenderer.cpp \
    src/ui/qrcodegen.cpp \
    src/network/HttpClient.cpp \
    src/network/YoutubeInnertube.cpp \
    src/network/WebServer.cpp \
    src/auth/AuthManager.cpp \
    src/sync/DriveSyncEngine.cpp \
    src/sync/OneDriveSync.cpp \
    src/download/DownloadManager.cpp \
    src/ota/UpdateManager.cpp \
    src/input/InputManager.cpp \
    src/filesystem/FileSystemManager.cpp \
    src/platform/PlatformInfo.cpp \
    src/logging/Logger.cpp \
    src/logging/IssueLogger.cpp \
    src/diagnostics/DeviceIdentity.cpp \
    src/iptv/IPTVManager.cpp \
    src/media/MpvPlayer.cpp \
    src/config/AppConfig.cpp \
    src/database/DatabaseManager.cpp \
    src/database/RomIndexer.cpp \
    src/sync/UploadManager.cpp \
    src/backup/BackupManager.cpp \
    src/rom/RomDetector.cpp \
    src/rom/RomOrganizer.cpp \
    src/rom/Md5SystemDb.cpp \
    src/rom/FbneoCompatDb.cpp \
    src/rom/EmuLauncher.cpp \
    src/localsend/LocalSendManager.cpp \
    src/browser/chrome/ChromeBridge.cpp \
    -I/tmp/dbusarm/dev/usr/include/dbus-1.0 \
    -I/tmp/dbusarm/dev/usr/lib/aarch64-linux-gnu/dbus-1.0/include \
    /tmp/dbusarm/rt/lib/aarch64-linux-gnu/libdbus-1.so.3 \
    -Lsysroot/lib \
    -lSDL2 \
    -lSDL2_image \
    -lSDL2_ttf \
    -lsqlite3 \
    -lcurl \
    -lssl \
    -lcrypto \
    -lpthread \
    -lrt \
    -ldl \
    -lm \
    -o bin/RomCloud

echo "=== Build Successful: bin/RomCloud ==="
ls -lh bin/RomCloud
file bin/RomCloud

if [ "$RELEASE" = "1" ]; then
    echo "=== Release mode: keeping debug copy + rebuilding stripped (-s) ==="
    cp bin/RomCloud bin/RomCloud.debug
    STRIPFLAG="-s" RELEASE=0 "$SCRIPT_DIR/build.sh"
    ls -lh bin/RomCloud bin/RomCloud.debug
    file bin/RomCloud
fi

