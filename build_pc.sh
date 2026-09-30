#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Building RomCloud for PC ==="
mkdir -p bin

# MSYS2 / MinGW Windows paths
MSYS2_PREFIX="C:/msys64/mingw64"
GXX="$MSYS2_PREFIX/bin/g++.exe"

# Check if MSYS2 g++ exists
if [ ! -f "$GXX" ]; then
    GXX="g++"
fi

echo "Using compiler: $GXX"

# SDL2 include and lib paths for MSYS2
SDL2_CFLAGS="-I$MSYS2_PREFIX/include/SDL2"
SDL2_LIBS="-L$MSYS2_PREFIX/lib"

echo "SDL2 flags: $SDL2_CFLAGS"

$GXX -std=c++17 -O2 -Wall -Wextra \
    -DPC_SIMULATOR_MODE \
    $SDL2_CFLAGS \
    -Isrc \
    src/main.cpp \
    src/app/Application.cpp \
    src/ui/UIManager.cpp \
    src/ui/UiRenderer.cpp \
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
    src/download/DownloadManager.cpp \
    src/ota/UpdateManager.cpp \
    src/input/InputManager.cpp \
    src/filesystem/FileSystemManager.cpp \
    src/platform/PlatformInfo.cpp \
    src/logging/Logger.cpp \
    src/logging/IssueLogger.cpp \
    src/iptv/IPTVManager.cpp \
    src/iptv/TikTokManager.cpp \
    src/media/MpvPlayer.cpp \
    src/config/AppConfig.cpp \
    src/database/DatabaseManager.cpp \
    src/database/RomIndexer.cpp \
    src/sync/UploadManager.cpp \
    src/backup/BackupManager.cpp \
    src/rom/RomDetector.cpp \
    src/rom/RomOrganizer.cpp \
    src/localsend/LocalSendManager.cpp \
    src/ui/ExplorerSync.cpp \
    $SDL2_LIBS \
    -lSDL2 -lSDL2main -lSDL2_image -lSDL2_ttf \
    -lsqlite3 -lcurl -lssl -lcrypto -lpthread -lm \
    -o bin/RomCloud.exe

echo "=== Build Successful: bin/RomCloud.exe ==="
ls -lh bin/RomCloud.exe 2>/dev/null || echo "Binary created"
