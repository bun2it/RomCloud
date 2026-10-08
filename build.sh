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

# Optional: build the NetSurf library stack (HTML5 parser, DOM, CSS engine).
# Skipped by default; set BUILD_LIBNETSURF=1 to enable. The libs end up in
# sysroot/lib/ (libcss.a, libhubbub.a, libdom.a, libparserutils.a,
# libwapcaplet.a, libnslog.a, libnsutils.a) and headers in sysroot/include/.
# See scripts/build-libnetsurf.py for the cross-compile recipe.
if [ "${BUILD_LIBNETSURF:-0}" = "1" ]; then
    if [ ! -e "libnetsurf-src" ]; then
        if [ -d "$HOME/Downloads/netsurf-all-3.11" ]; then
            ln -s "$HOME/Downloads/netsurf-all-3.11" libnetsurf-src
        elif [ -d "/Users/tai/Downloads/netsurf-all-3.11" ]; then
            ln -s "/Users/tai/Downloads/netsurf-all-3.11" libnetsurf-src
        else
            echo "WARNING: BUILD_LIBNETSURF=1 set but no libnetsurf source found."
            echo "        Expected ~/Downloads/netsurf-all-3.11/. Skipping."
        fi
    fi
    if [ -e "libnetsurf-src" ]; then
        echo "=== Building libnetsurf stack ==="
        python3 scripts/build-libnetsurf.py
    fi
fi

# Release mode: RELEASE=1 ./build.sh
#   - Build nhu dev (giu debug info), copy ban debug sang bin/*.debug,
#   - roi strip binary chinh bang `zig objcopy --strip-all` (nhe ~5-8MB).
#   - Mac dinh (dev) GIU NGUYEN debug info de debug crash.
#   - bin/*.debug da co trong .gitignore, khong lot vao git/zip.
RELEASE="${RELEASE:-0}"

# Detect git commit hash for build-id tagging in debug.log
GIT_SHA="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "Build commit: $GIT_SHA"

# nsgif is strict C99 (uses `restrict`) — compile as C, not C++.
mkdir -p build
$ZIG cc -target aarch64-linux-gnu.2.33 -O2 -Isrc/browser/nsgif \
    -c src/browser/nsgif/gif.c -o build/nsgif_gif.o
$ZIG cc -target aarch64-linux-gnu.2.33 -O2 -Isrc/browser/nsgif \
    -c src/browser/nsgif/lzw.c -o build/nsgif_lzw.o
# Duktape is C99 — same treatment.
$ZIG cc -target aarch64-linux-gnu.2.33 -O2 -Isrc/browser/duktape \
    -c src/browser/duktape/duktape.c -o build/duktape.o

$ZIG c++ \
    -target aarch64-linux-gnu.2.33 \
    -std=c++17 \
    -O3 \
    -Wall -Wextra \
    -Wno-error=date-time \
    -DGIT_COMMIT_HASH=\"$GIT_SHA\" \
    -Isrc \
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
    src/browser/HtmlRenderer.cpp \
    src/browser/BrowserManager.cpp \
    src/browser/JsEngine.cpp \
    src/browser/SvgRaster.cpp \
    src/browser/netsurf/NetSurfBridge.cpp \
    src/browser/netsurf/NetSurfLayout.cpp \
    src/browser/netsurf/NetSurfRenderer.cpp \
    src/browser/netsurf/NetSurfEngine.cpp \
    build/nsgif_gif.o \
    build/nsgif_lzw.o \
    build/duktape.o \
    src/cast/CastManager.cpp \
    -Lsysroot/lib \
    -lcss \
    -ldom \
    -lhubbub \
    -lparserutils \
    -lwapcaplet \
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

