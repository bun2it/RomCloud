#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Building RomCloud for PC Simulator ==="
mkdir -p bin

# 1. Detect Compiler (zig c++, g++, clang++)
CXX_CMD=""
MSYS2_PREFIX="C:/msys64/mingw64"

if command -v zig &>/dev/null; then
    CXX_CMD="zig c++"
elif [ -f "$MSYS2_PREFIX/bin/g++.exe" ]; then
    CXX_CMD="$MSYS2_PREFIX/bin/g++.exe"
elif command -v g++ &>/dev/null; then
    CXX_CMD="g++"
elif command -v clang++ &>/dev/null; then
    CXX_CMD="clang++"
else
    echo "ERROR: No C++ compiler found (zig, g++, clang++). Please install one or configure PATH." >&2
    exit 1
fi

echo "Using compiler: $CXX_CMD"

# 2. Platform specific flags
OS_NAME="$(uname -s 2>/dev/null || echo "Windows")"
EXTRA_CFLAGS=""
EXTRA_LIBS=""
OUTPUT_BIN="bin/RomCloud"

case "$OS_NAME" in
    MINGW*|MSYS*|CYGWIN*|Windows*)
        OUTPUT_BIN="bin/RomCloud.exe"
        EXTRA_CFLAGS="-include src/common/PlatformCompat.h -Isrc/compat"
        if [ -d "$MSYS2_PREFIX/include/SDL2" ]; then
            EXTRA_CFLAGS="$EXTRA_CFLAGS -I$MSYS2_PREFIX/include/SDL2 -I$MSYS2_PREFIX/include"
        fi
        EXTRA_LIBS="-L$MSYS2_PREFIX/lib -lmingw32 -lSDL2main -lSDL2 -mconsole -lSDL2_image -lSDL2_ttf -lsqlite3 -lcurl -lssl -lcrypto -lws2_32 -lwinmm -lgdi32 -luser32 -lshell32 -lpthread -lm"
        ;;
    Darwin*)
        OUTPUT_BIN="bin/RomCloud_mac"
        if [ -d "/opt/homebrew/include" ]; then
            EXTRA_CFLAGS="-I/opt/homebrew/include -I/opt/homebrew/include/SDL2"
            EXTRA_LIBS="-L/opt/homebrew/lib"
        elif [ -d "/usr/local/include" ]; then
            EXTRA_CFLAGS="-I/usr/local/include -I/usr/local/include/SDL2"
            EXTRA_LIBS="-L/usr/local/lib"
        fi
        EXTRA_LIBS="$EXTRA_LIBS -lSDL2 -lSDL2_image -lSDL2_ttf -lsqlite3 -lcurl -lssl -lcrypto -lpthread -lm"
        ;;
    *)
        # Linux
        OUTPUT_BIN="bin/RomCloud"
        EXTRA_LIBS="-lSDL2 -lSDL2_image -lSDL2_ttf -lsqlite3 -lcurl -lssl -lcrypto -lpthread -ldl -lm"
        ;;
esac

echo "Target binary: $OUTPUT_BIN"

# nsgif is strict C99 (uses `restrict`) — compile as C, not C++.
CC_CMD="cc"
case "$CXX_CMD" in
    "zig c++") CC_CMD="zig cc" ;;
    *g++*) CC_CMD="${CXX_CMD%g++*}gcc" ;;
    *clang++*) CC_CMD="${CXX_CMD%clang++*}clang" ;;
esac
mkdir -p build
$CC_CMD -O2 -Isrc/browser/nsgif -c src/browser/nsgif/gif.c -o build/nsgif_gif.o
$CC_CMD -O2 -Isrc/browser/nsgif -c src/browser/nsgif/lzw.c -o build/nsgif_lzw.o
$CC_CMD -O2 -Isrc/browser/duktape -c src/browser/duktape/duktape.c -o build/duktape.o

$CXX_CMD -std=c++17 -O2 -Wall -Wextra \
    -DPC_SIMULATOR_MODE \
    $EXTRA_CFLAGS \
    -Isrc \
    src/main.cpp \
    src/app/Application.cpp \
    src/ui/UIManager.cpp \
    src/ui/UiRenderer.cpp \
    src/ui/UiStrings.cpp \
    src/ui/SearchInputModal.cpp \
    src/ui/DialogManager.cpp \
    src/ui/CoverManager.cpp \
    src/ui/BoxartScraper.cpp \
    src/ui/QrRenderer.cpp \
    src/ui/qrcodegen.cpp \
    src/ui/ExplorerInput.cpp \
    src/ui/ExplorerRender.cpp \
    src/ui/ExplorerRenderKb.cpp \
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
    src/ui/ExplorerSync.cpp \
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
    $EXTRA_LIBS \
    -o "$OUTPUT_BIN"

echo "=== Build Successful: $OUTPUT_BIN ==="
ls -lh "$OUTPUT_BIN" 2>/dev/null || echo "Binary ready."
