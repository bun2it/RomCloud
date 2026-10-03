#!/bin/sh
# deploy_p0-5.sh - Deploy P0-5 YouTube API v3 to TrimUI Brick
#
# Prerequisites:
#   - Brick connected via USB (or WiFi ADB enabled)
#   - adb in $PATH
#   - bin/RomCloud already built (./build.sh)
#
# What this does:
#   1. Kill running RomCloud + gamecast_d
#   2. Push new binary + python script + API key
#   3. chmod +x binary
#   4. adb shell sync to flush SD card buffers
#   5. Quick smoke test: smart_search from python script on device
#
# Usage:
#   ./scripts/deploy_p0-5.sh                 # USB device
#   ./scripts/deploy_p0-5.sh 192.168.1.42    # WiFi ADB

set -e

DEVICE_ARG="$1"
if [ -n "$DEVICE_ARG" ]; then
    echo "[deploy] Connecting to WiFi ADB $DEVICE_ARG:5555"
    adb connect "$DEVICE_ARG:5555"
    sleep 1
fi

echo "[deploy] Verifying connection..."
adb devices
DEV_COUNT=$(adb devices | grep -c "device$" || true)
if [ "$DEV_COUNT" -lt 1 ]; then
    echo "[deploy] FATAL: no device connected. Plug USB and try again."
    exit 1
fi

APP_ROOT="/mnt/SDCARD/Apps/RomCloud"
BIN_LOCAL="bin/RomCloud"
SCRIPT_LOCAL="scripts/youtube_search.py"
KEY_LOCAL="config/youtube_api.key"

echo "[deploy] Killing existing RomCloud + gamecast_d..."
adb shell "killall RomCloud 2>/dev/null; killall gamecast_d 2>/dev/null" || true
sleep 1

echo "[deploy] Pushing binary ($BIN_LOCAL)..."
adb push "$BIN_LOCAL" "$APP_ROOT/bin/RomCloud"

echo "[deploy] Pushing python script ($SCRIPT_LOCAL)..."
adb push "$SCRIPT_LOCAL" "$APP_ROOT/scripts/youtube_search.py"

if [ -f "$KEY_LOCAL" ]; then
    echo "[deploy] Pushing API key ($KEY_LOCAL)..."
    adb push "$KEY_LOCAL" "$APP_ROOT/config/youtube_api.key"
fi

echo "[deploy] chmod +x binary + sync..."
adb shell "chmod +x $APP_ROOT/bin/RomCloud && sync"

echo "[deploy] Smoke test on device (API v3 smart_search)..."
adb shell "PATH=/mnt/SDCARD/System/bin:\$PATH LD_LIBRARY_PATH=/mnt/SDCARD/System/lib:\$LD_LIBRARY_PATH /mnt/SDCARD/System/bin/python3 $APP_ROOT/scripts/youtube_search.py smart 'Vat Vo Studio' 6"

echo "[deploy] Done. Launch RomCloud from TrimUI launcher."
echo "[deploy] Tip: watch P0.5 badge top-right when YouTube opens."