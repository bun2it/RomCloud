#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:/mingw64/bin:$PATH"

if [ ! -f "bin/RomCloud.exe" ] && [ ! -f "bin/RomCloud" ]; then
    echo "[ERROR] Simulator binary not found! Please run ./build_pc.sh first."
    exit 1
fi

mkdir -p pc_data
if [ ! -d "pc_data/assets/apps_icons" ]; then
    mkdir -p pc_data/assets
    cp -r assets/* pc_data/assets/ 2>/dev/null
fi

echo "=== Starting RomCloud PC Simulator [1024x768] ==="
if [ -f "bin/RomCloud.exe" ]; then
    ./bin/RomCloud.exe ./pc_data
elif [ -f "bin/RomCloud_mac" ]; then
    ./bin/RomCloud_mac ./pc_data
else
    ./bin/RomCloud ./pc_data
fi
