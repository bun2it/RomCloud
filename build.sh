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

# Release mode: RELEASE=1 ./build.sh
#   - Build nhu dev (giu debug info), copy ban debug sang bin/*.debug,
#   - roi strip binary chinh bang `zig objcopy --strip-all` (nhe ~5-8MB).
#   - Mac dinh (dev) GIU NGUYEN debug info de debug crash.
#   - bin/*.debug da co trong .gitignore, khong lot vao git/zip.
RELEASE="${RELEASE:-0}"

# Detect git commit hash for build-id tagging in debug.log
GIT_SHA="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "Build commit: $GIT_SHA"

# Ultralight SDK (PortalBrowser, nhánh portal-browser). Lấy từ /tmp/ulsdk
# (overlay SDK, không commit vào git). Thiếu là dừng và báo rõ.
ULSDK="${ULSDK:-/tmp/ulsdk}"
if [ ! -f "$ULSDK/bin/libWebCore.so" ]; then
    echo "ERROR: Ultralight SDK not found at $ULSDK (need bin/libWebCore.so)." >&2
    echo "HINT: tải ultralight-free-sdk-1.4.0-linux-arm64.7z từ nhánh base-sdk" >&2
    echo "      của ovsky/Ultralight-WebBrowser rồi giải nén vào $ULSDK." >&2
    exit 1
fi

$ZIG c++ \
    -target aarch64-linux-gnu.2.33 \
    -std=c++17 \
    -O3 \
    -Wall -Wextra \
    -Wno-error=date-time \
    -DGIT_COMMIT_HASH=\"$GIT_SHA\" \
    -Isrc \
    -I"$ULSDK/include" \
    -Isysroot/include \
    -Isysroot/include/SDL2 \
    src/main.cpp \
    src/app/Application.cpp \
    src/ui/UIManager.cpp \
    src/ui/UiRenderer.cpp \
    src/ui/UiStrings.cpp \
    src/ui/ExplorerSync.cpp \
    src/weather/WeatherManager.cpp \
    src/calendar/CalManager.cpp \
    src/ui/WeatherUI.cpp \
    src/clock/ClockStore.cpp \
    src/ui/ClockUI.cpp \
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
    src/localsend/LocalSendManager.cpp \
    src/cast/CastManager.cpp \
    src/browser/PortalBrowser.cpp \
    src/ui/PortalUI.cpp \
    -L"$ULSDK/bin" \
    -lUltralight \
    -lUltralightCore \
    -lWebCore \
    -Wl,-rpath,'$ORIGIN/../lib' \
    -Lsysroot/lib \
    -lSDL2 \
    -lSDL2_image \
    -lSDL2_ttf \
    -lsqlite3 \
    -lcurl \
    -lssl \
    -lcrypto \
    -lpthread \
    -ldl \
    -lm \
    -o bin/RomCloud

echo "=== Build Successful: bin/RomCloud ==="
ls -lh bin/RomCloud
file bin/RomCloud

if [ "$RELEASE" = "1" ]; then
    echo "=== Release mode: keeping debug copy + stripping ==="
    cp bin/RomCloud bin/RomCloud.debug
    "$ZIG" objcopy --strip-all bin/RomCloud bin/RomCloud.stripped
    mv bin/RomCloud.stripped bin/RomCloud
    ls -lh bin/RomCloud bin/RomCloud.debug
    file bin/RomCloud
fi

echo "=== Compiling GameCast Daemon (gamecast_d) ==="
$ZIG c++ \
    -target aarch64-linux-gnu.2.33 \
    -std=c++17 \
    -O3 \
    -Wall -Wextra \
    -Isrc \
    src/cast/toojpeg.cpp \
    src/cast/gamecast_d.cpp \
    -lpthread \
    -o bin/gamecast_d

echo "=== Build Successful: bin/gamecast_d ==="
ls -lh bin/gamecast_d
file bin/gamecast_d

if [ "$RELEASE" = "1" ]; then
    cp bin/gamecast_d bin/gamecast_d.debug
    "$ZIG" objcopy --strip-all bin/gamecast_d bin/gamecast_d.stripped
    mv bin/gamecast_d.stripped bin/gamecast_d
    ls -lh bin/gamecast_d bin/gamecast_d.debug
    file bin/gamecast_d
fi

