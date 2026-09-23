#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

ZIG="/Users/tai/.gemini/antigravity-ide/brain/00d3b56c-d55c-4262-81a8-0cf5fe35825f/tools/zig-macos-aarch64-0.13.0/zig"
if [ ! -f "$ZIG" ] && command -v zig &>/dev/null; then
    ZIG="zig"
fi

echo "=== Compiling RomCloud for TrimUI Brick Pro (aarch64-linux-gnu.2.33) ==="
mkdir -p bin

$ZIG c++ \
    -target aarch64-linux-gnu.2.33 \
    -std=c++17 \
    -O3 \
    -Wall -Wextra \
    -Isrc \
    -Isysroot/include \
    -Isysroot/include/SDL2 \
    src/main.cpp \
    src/app/Application.cpp \
    src/ui/UIManager.cpp \
    src/ui/CoverManager.cpp \
    src/ui/BoxartScraper.cpp \
    src/ui/QrRenderer.cpp \
    src/ui/qrcodegen.cpp \
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
    src/config/AppConfig.cpp \
    src/database/DatabaseManager.cpp \
    src/database/RomIndexer.cpp \
    src/sync/UploadManager.cpp \
    src/backup/BackupManager.cpp \
    src/rom/RomDetector.cpp \
    src/rom/RomOrganizer.cpp \
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
