#!/bin/sh
# deploy_and_test_browser.sh
# One-shot deploy + launch + log capture for the WEB BROWSER fix.
#
# Usage:  ./scripts/deploy_and_test_browser.sh
#
# What it does:
#   1. Pushes the new RomCloud binary
#   2. Pushes the shared libs we control (libcurl, libsqlite3, libjpeg,
#      libcrypto, libssl, libSDL2_image, libSDL2_ttf) into App's lib/ dir
#      — only the ones the TrimUI system lib/ doesn't already have.
#   3. Kills any running instance
#   4. Launches via the TrimUI launch.sh (NOT am start, which doesn't exist
#      on the device)
#   5. Tails the on-device log so you can see HTTP/Browser entries in real
#      time

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
    echo "[deploy] FATAL: no device connected."
    exit 1
fi

APP_ROOT="/mnt/SDCARD/Apps/RomCloud"
BIN_LOCAL="bin/RomCloud"

echo "[deploy] Killing existing RomCloud..."
adb shell "killall RomCloud 2>/dev/null" || true
sleep 1

echo "[deploy] Pushing binary ($BIN_LOCAL, $(wc -c < $BIN_LOCAL) bytes)..."
adb push "$BIN_LOCAL" "$APP_ROOT/bin/RomCloud"
adb shell "chmod +x $APP_ROOT/bin/RomCloud && sync"

echo "[deploy] Pushing shared libs we own (libcurl, libsqlite3, libjpeg, libsdl2_*)..."
# Only push libs that DON'T exist on the system to avoid clobbering working
# versions. The Brick's /mnt/SDCARD/Apps/RomCloud/lib/ already has libssl,
# libcrypto, libav*, libSDL2 — we only ship the ones that are missing.
for lib in libcurl libsqlite3 libjpeg libSDL2_image libSDL2_ttf libpng; do
    for f in sysroot/lib/${lib}.so*; do
        if [ -f "$f" ]; then
            echo "  + $f"
            adb push "$f" "$APP_ROOT/lib/" 2>&1 | tail -1
        fi
    done
done

adb shell "sync"

echo "[deploy] Verifying libs on device..."
for lib in libcurl.so.4 libsqlite3.so.0 libSDL2_image-1.2.so.0 libSDL2_ttf-2.0.so.0; do
    echo -n "  $lib: "
    adb shell "ls $APP_ROOT/lib/$lib 2>/dev/null || echo MISSING"
done

echo ""
echo "[deploy] Launching via $APP_ROOT/launch.sh (NOT am start)..."
echo "[deploy] This will block; press Ctrl+C when you want to stop tailing log."
echo "[deploy] ---"
adb shell "cd $APP_ROOT && ./launch.sh > /tmp/romcloud.stdout 2>&1 &"
sleep 3

echo "[deploy] Process running? (should print RomCloud line)"
adb shell "ps | grep -E 'RomCloud|gamecast' | head -5" || true
echo ""
echo "[deploy] === Tailing log (Ctrl+C to stop) ==="
adb shell "tail -f $APP_ROOT/RomCloud.log 2>/dev/null || tail -f /tmp/romcloud.stdout"