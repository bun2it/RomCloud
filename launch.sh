#!/bin/sh
cd "$(dirname "$0")"

# Set dynamic library search path (local lib, system SD lib, system usr lib)
export LD_LIBRARY_PATH="$(dirname "$0")/lib:/mnt/SDCARD/System/lib:/usr/lib:$LD_LIBRARY_PATH"

# Display orientation check for TrimUI Brick Pro / Smart Pro
if [ -f /etc/trimui_device.txt ]; then
    read -r Current_device </etc/trimui_device.txt
    if [ "$Current_device" = "tsps" ]; then
        echo 1 >/sys/class/drm/card0-DSI-1/rotate 2>/dev/null
        echo 1 >/sys/class/drm/card0-DSI-1/force_rotate 2>/dev/null
    fi
fi

# Clean up any leftover temporary files from prior sessions
rm -f /tmp/romcloud_*.tmp 2>/dev/null

# Ensure port 8080 is freed if previous instance did not exit cleanly
fuser -k 8080/tcp 2>/dev/null || true

install_pending_ota() {
    NEW_BIN="./bin/RomCloud.new"
    TARGET_BIN="./bin/RomCloud"
    HELPER_SCRIPT="./bin/ota_install.sh"

    # Remove any stale helper script
    rm -f "$HELPER_SCRIPT" 2>/dev/null

    # Check for pending official icon update
    if [ -f "./icon.png.new" ]; then
        echo "[RomCloud OTA] Found pending icon update..."
        cp -f "./icon.png.new" "./icon.png" 2>/dev/null
        cp -f "./icon.png.new" "./assets/apps_icons/APP.png" 2>/dev/null
        cp -f "./icon.png.new" "./assets/icon.png" 2>/dev/null
        rm -f "./icon.png.new"
        sync
        echo "[RomCloud OTA] App icon updated successfully."
    fi

    # Check for pending mpv & codecs media bundle
    if [ -f "./mpv_bundle.zip" ]; then
        echo "[RomCloud OTA] Found mpv_bundle.zip, extracting media bundle..."
        unzip -o "./mpv_bundle.zip" -d . 2>/dev/null || busybox unzip -o "./mpv_bundle.zip" -d . 2>/dev/null
        rm -f "./mpv_bundle.zip"
        chmod +x ./bin/* 2>/dev/null
        sync
        echo "[RomCloud OTA] Media player bundle installed successfully."
    fi

    if [ -f "$NEW_BIN" ]; then
        echo "[RomCloud OTA] Found pending update file: $NEW_BIN"
        
        # Verify downloaded binary size (must be >= 1MB)
        NEW_SIZE=$(wc -c < "$NEW_BIN" 2>/dev/null || echo 0)
        if [ "$NEW_SIZE" -lt 1000000 ]; then
            echo "[RomCloud OTA] Error: Downloaded file size ($NEW_SIZE bytes) is too small, discarding."
            rm -f "$NEW_BIN"
            return 1
        fi

        # Allow kernel to completely release file handles and flush dirty buffers on FAT32
        sleep 1

        # FAT32 safe replacement:
        # Step 1: Remove old target binary (avoid EBUSY on rename/move)
        rm -f "$TARGET_BIN"
        sync

        # Step 2: Copy new binary to target path
        cp -f "$NEW_BIN" "$TARGET_BIN"
        sync

        # Step 3: Verify target binary size
        TARGET_SIZE=$(wc -c < "$TARGET_BIN" 2>/dev/null || echo 0)
        if [ "$TARGET_SIZE" -ge 1000000 ]; then
            chmod +x "$TARGET_BIN" 2>/dev/null
            rm -f "$NEW_BIN"
            sync
            echo "[RomCloud OTA] Successfully installed new binary ($TARGET_SIZE bytes)!"
            return 0
        else
            echo "[RomCloud OTA] Installation verification failed ($TARGET_SIZE bytes), keeping backup."
            return 1
        fi
    fi
    return 0
}

# Check and install any pending OTA update before starting
install_pending_ota

# Ensure binary is executable
chmod +x ./bin/RomCloud 2>/dev/null

# Execution loop supporting in-app restart after OTA update (exit code 42)
while true; do
    ./bin/RomCloud "$PWD"
    EXIT_CODE=$?
    if [ $EXIT_CODE -eq 42 ]; then
        echo "[RomCloud OTA] Restart requested (exit code 42). Installing update..."
        sleep 1
        install_pending_ota
        sleep 1
        continue
    fi
    break
done

# Sync file systems to SD card before returning to TrimUI MainUI
sync
