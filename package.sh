#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

VERSION=$(grep '"version"' version.json | head -n 1 | awk -F'"' '{print $4}')
if [ -z "$VERSION" ]; then
    VERSION="latest"
fi

echo "=== Packaging RomCloud v${VERSION} for TrimUI ==="

DIST_DIR="$SCRIPT_DIR/dist"
STAGING_DIR="$DIST_DIR/staging"
ZIP_NAME="RomCloud-v${VERSION}.zip"

# Release gate: zip phat hanh nen dong tu binary STRIPPED (RELEASE=1 ./build.sh).
# Chi canh bao, khong tu build lai de tranh doi behavior bat ngo.
if [ -f bin/RomCloud ]; then
    if file bin/RomCloud | grep -q "not stripped"; then
        echo "WARNING: bin/RomCloud is NOT stripped (dev build)." >&2
        echo "WARNING: run RELEASE=1 ./build.sh before packaging a release." >&2
    fi
else
    echo "ERROR: bin/RomCloud not found. Run ./build.sh first." >&2
    exit 1
fi

rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR/Apps/RomCloud/bin"
mkdir -p "$STAGING_DIR/Apps/RomCloud/lib"
mkdir -p "$STAGING_DIR/Apps/RomCloud/scripts"
mkdir -p "$STAGING_DIR/Apps/RomCloud/assets/fonts"
mkdir -p "$STAGING_DIR/Apps/RomCloud/assets/sounds"
mkdir -p "$STAGING_DIR/Apps/RomCloud/assets/icons"
mkdir -p "$STAGING_DIR/Apps/RomCloud/assets/apps_icons"
mkdir -p "$STAGING_DIR/Apps/RomCloud/assets/player_icons"
mkdir -p "$STAGING_DIR/Apps/RomCloud/assets/button_icons"
mkdir -p "$STAGING_DIR/Apps/RomCloud/assets/stock_keyboard"
mkdir -p "$STAGING_DIR/Apps/RomCloud/config"
mkdir -p "$STAGING_DIR/Apps/RomCloud/iptv"

# Copy essential runtime files
cp config.json "$STAGING_DIR/Apps/RomCloud/"
cp icon.png "$STAGING_DIR/Apps/RomCloud/icon.png"
cp -f iconsel.png "$STAGING_DIR/Apps/RomCloud/iconsel.png" 2>/dev/null || cp icon.png "$STAGING_DIR/Apps/RomCloud/iconsel.png"
cp -f icontop.png "$STAGING_DIR/Apps/RomCloud/icontop.png" 2>/dev/null || cp icon.png "$STAGING_DIR/Apps/RomCloud/icontop.png"
cp launch.sh "$STAGING_DIR/Apps/RomCloud/"
cp bin/RomCloud "$STAGING_DIR/Apps/RomCloud/bin/"
if [ -f bin/gamecast_d ]; then
    cp bin/gamecast_d "$STAGING_DIR/Apps/RomCloud/bin/"
    chmod +x "$STAGING_DIR/Apps/RomCloud/bin/gamecast_d"
fi
# Copy YouTube support (python3 + yt-dlp)
# yt-dlp-glibc (38MB) chi dong trong goi Full; goi Lite se tu tai qua
# mpv_bundle.zip khi phat YouTube lan dau (UpdateManager::checkAndInstallDependencies).
if [ -f bin/yt-dlp ]; then
    cp bin/yt-dlp "$STAGING_DIR/Apps/RomCloud/bin/"
    chmod +x "$STAGING_DIR/Apps/RomCloud/bin/yt-dlp"
fi
# Backend bung .zip/.rar/.7z trong File Explorer (ArchiveEngine)
if [ -f bin/7zzs ]; then
    cp bin/7zzs "$STAGING_DIR/Apps/RomCloud/bin/"
    chmod +x "$STAGING_DIR/Apps/RomCloud/bin/7zzs"
fi
if [ -d scripts ]; then
    cp -r scripts/* "$STAGING_DIR/Apps/RomCloud/scripts/"
    chmod +x "$STAGING_DIR/Apps/RomCloud/scripts/"*.sh 2>/dev/null || true
fi

cp assets/fonts/NotoSans-Regular.ttf "$STAGING_DIR/Apps/RomCloud/assets/fonts/"
# Font so dong ho (Rajdhani) + font flip-clock Fliqlo (gluqlo) — thieu la
# mat so fullscreen (rule single-zip v2.3.0: moi runtime doi phai co trong zip)
cp assets/fonts/Rajdhani-Bold.ttf "$STAGING_DIR/Apps/RomCloud/assets/fonts/"
cp assets/fonts/gluqlo.ttf "$STAGING_DIR/Apps/RomCloud/assets/fonts/"
# Chuong bao thuc (ClockStore beepStart qua aplay)
cp assets/sounds/alarm.wav "$STAGING_DIR/Apps/RomCloud/assets/sounds/"
cp assets/vn_wards.json "$STAGING_DIR/Apps/RomCloud/assets/" 2>/dev/null || true
cp assets/hcm_outline.json "$STAGING_DIR/Apps/RomCloud/assets/" 2>/dev/null || true
cp assets/icon.png "$STAGING_DIR/Apps/RomCloud/assets/" 2>/dev/null || true
cp assets/traffic_cams.json "$STAGING_DIR/Apps/RomCloud/assets/" 2>/dev/null || true
cp assets/flood_points.json "$STAGING_DIR/Apps/RomCloud/assets/" 2>/dev/null || true
# Bang tra MD5/CRC32 -> he may cho tinh nang Sap xep ROM (sinh boi
# tools/gen_md5db.py --download); thieu thi vong 2 tu bo qua
cp assets/md5systems.dat "$STAGING_DIR/Apps/RomCloud/assets/" 2>/dev/null || true
cp assets/fbneosets.dat "$STAGING_DIR/Apps/RomCloud/assets/" 2>/dev/null || true
mkdir -p "$STAGING_DIR/Apps/RomCloud/assets/weather_icons"
cp assets/weather_icons/*.png "$STAGING_DIR/Apps/RomCloud/assets/weather_icons/" 2>/dev/null || true
cp -r assets/stock_keyboard/* "$STAGING_DIR/Apps/RomCloud/assets/stock_keyboard/"
cp assets/icons/*.png "$STAGING_DIR/Apps/RomCloud/assets/icons/"
cp assets/apps_icons/*.png "$STAGING_DIR/Apps/RomCloud/assets/apps_icons/" 2>/dev/null || true
cp -r assets/player_icons/* "$STAGING_DIR/Apps/RomCloud/assets/player_icons/" 2>/dev/null || true
cp assets/button_icons/*.png "$STAGING_DIR/Apps/RomCloud/assets/button_icons/" 2>/dev/null || true
if [ -d assets/netsurf ]; then
    mkdir -p "$STAGING_DIR/Apps/RomCloud/assets/netsurf"
    cp -r assets/netsurf/* "$STAGING_DIR/Apps/RomCloud/assets/netsurf/"
fi
cp config/settings.json "$STAGING_DIR/Apps/RomCloud/config/"
if [ -d config ]; then
    cp -f config/*.conf "$STAGING_DIR/Apps/RomCloud/config/" 2>/dev/null || true
    # P0-5: YouTube Data API v3 key (chmod 600 for privacy)
    if [ -f config/youtube_api.key ]; then
        cp -f config/youtube_api.key "$STAGING_DIR/Apps/RomCloud/config/"
        chmod 600 "$STAGING_DIR/Apps/RomCloud/config/youtube_api.key"
    fi
fi
cp -f iptv/*.m3u iptv/*.m3u8 "$STAGING_DIR/Apps/RomCloud/iptv/" 2>/dev/null || true
cp -f iptv/sources.txt "$STAGING_DIR/Apps/RomCloud/iptv/" 2>/dev/null || true
cp -f iptv/iptv_manifest.json "$STAGING_DIR/Apps/RomCloud/iptv/" 2>/dev/null || true

# Ensure execution permissions
chmod +x "$STAGING_DIR/Apps/RomCloud/launch.sh" "$STAGING_DIR/Apps/RomCloud/bin/RomCloud"

make_zip() {
    local out="$1"
    shift
    if command -v zip &>/dev/null; then
        zip -r "$out" "$@"
    elif command -v tar.exe &>/dev/null; then
        tar.exe -a -cf "$out" "$@"
    elif command -v tar &>/dev/null; then
        tar -a -cf "$out" "$@"
    fi
}

# Release rule (.agents/rules/release_ota.md): MỖI VERSION CHỈ 1 FILE ZIP DUY NHẤT
# (RomCloud-vX.Y.Z.zip full). CẤM sinh thêm Lite/mpv_bundle/loose files.
# OTA full-zip (UpdateManager::installFullZip) dùng chính file này.
mkdir -p "$DIST_DIR"
rm -f "$DIST_DIR"/RomCloud-v*.zip "$DIST_DIR"/RomCloud-Lite-Installer.zip \
      "$DIST_DIR"/RomCloud-Install-To-Apps.zip "$DIST_DIR"/mpv_bundle.zip \
      "$DIST_DIR"/bundle-*.zip "$DIST_DIR"/RomCloud "$DIST_DIR"/icon.png \
      "$DIST_DIR"/launch.sh
cd "$SCRIPT_DIR"
if [ -f bin/mpv ]; then
    cp bin/mpv "$STAGING_DIR/Apps/RomCloud/bin/"
    chmod +x "$STAGING_DIR/Apps/RomCloud/bin/mpv"
fiif [ -f bin/yt-dlp-glibc ]; then
    cp bin/yt-dlp-glibc "$STAGING_DIR/Apps/RomCloud/bin/"
    chmod +x "$STAGING_DIR/Apps/RomCloud/bin/yt-dlp-glibc"
fi
if [ -d lib ]; then
    cp -P lib/*.so* "$STAGING_DIR/Apps/RomCloud/lib/" 2>/dev/null || true
fi

# Flavor marker cho OTA delta (máy Lite đọc để tự kéo completion).
echo "full" > "$STAGING_DIR/Apps/RomCloud/.ota_flavor"
cd "$STAGING_DIR"
make_zip "$DIST_DIR/$ZIP_NAME" Apps

cd "$SCRIPT_DIR"

# OTA lite/delta/completion (chỉ khi OTA_RELEASE=1, cần ota-manifest.json
# baseline trong repo). Không đụng luồng package thường.
if [ "$OTA_RELEASE" = "1" ]; then
    if [ -f "$SCRIPT_DIR/ota-manifest.json" ]; then
        python3 "$SCRIPT_DIR/tools/mkota.py" release "$STAGING_DIR" "$VERSION" \
            "$SCRIPT_DIR/ota-manifest.json" "$DIST_DIR"
    else
        echo "WARNING: thieu ota-manifest.json baseline, bo qua goi delta/lite." >&2
    fi
fi
rm -rf "$STAGING_DIR"

echo "=== Release Package Created Successfully (single zip) ==="
ls -lh "$DIST_DIR/$ZIP_NAME"
