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
if [ -d scripts ]; then
    cp -r scripts/* "$STAGING_DIR/Apps/RomCloud/scripts/"
    chmod +x "$STAGING_DIR/Apps/RomCloud/scripts/"*.sh 2>/dev/null || true
fi

cp assets/fonts/NotoSans-Regular.ttf "$STAGING_DIR/Apps/RomCloud/assets/fonts/"
cp -r assets/stock_keyboard/* "$STAGING_DIR/Apps/RomCloud/assets/stock_keyboard/"
cp assets/icons/*.png "$STAGING_DIR/Apps/RomCloud/assets/icons/"
cp assets/apps_icons/*.png "$STAGING_DIR/Apps/RomCloud/assets/apps_icons/" 2>/dev/null || true
cp -r assets/player_icons/* "$STAGING_DIR/Apps/RomCloud/assets/player_icons/" 2>/dev/null || true
cp assets/button_icons/*.png "$STAGING_DIR/Apps/RomCloud/assets/button_icons/" 2>/dev/null || true
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
fi
if [ -f bin/yt-dlp-glibc ]; then
    cp bin/yt-dlp-glibc "$STAGING_DIR/Apps/RomCloud/bin/"
    chmod +x "$STAGING_DIR/Apps/RomCloud/bin/yt-dlp-glibc"
fi
if [ -d lib ]; then
    cp -P lib/*.so* "$STAGING_DIR/Apps/RomCloud/lib/" 2>/dev/null || true
fi

cd "$STAGING_DIR"
make_zip "$DIST_DIR/$ZIP_NAME" Apps

cd "$SCRIPT_DIR"
rm -rf "$STAGING_DIR"

echo "=== Release Package Created Successfully (single zip) ==="
ls -lh "$DIST_DIR/$ZIP_NAME"
