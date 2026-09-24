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

rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR/Apps/RomCloud/bin"
mkdir -p "$STAGING_DIR/Apps/RomCloud/lib"
mkdir -p "$STAGING_DIR/Apps/RomCloud/scripts"
mkdir -p "$STAGING_DIR/Apps/RomCloud/assets/fonts"
mkdir -p "$STAGING_DIR/Apps/RomCloud/assets/icons"
mkdir -p "$STAGING_DIR/Apps/RomCloud/assets/apps_icons"
mkdir -p "$STAGING_DIR/Apps/RomCloud/assets/player_icons"
mkdir -p "$STAGING_DIR/Apps/RomCloud/config"
mkdir -p "$STAGING_DIR/Apps/RomCloud/iptv"

# Copy essential runtime files
cp config.json "$STAGING_DIR/Apps/RomCloud/"
cp icon.png "$STAGING_DIR/Apps/RomCloud/icon.png"
cp -f iconsel.png "$STAGING_DIR/Apps/RomCloud/iconsel.png" 2>/dev/null || cp icon.png "$STAGING_DIR/Apps/RomCloud/iconsel.png"
cp -f icontop.png "$STAGING_DIR/Apps/RomCloud/icontop.png" 2>/dev/null || cp icon.png "$STAGING_DIR/Apps/RomCloud/icontop.png"
cp launch.sh "$STAGING_DIR/Apps/RomCloud/"
cp bin/RomCloud "$STAGING_DIR/Apps/RomCloud/bin/"

# Copy YouTube support (python3 + yt-dlp)
if [ -f bin/yt-dlp ]; then
    cp bin/yt-dlp "$STAGING_DIR/Apps/RomCloud/bin/"
    chmod +x "$STAGING_DIR/Apps/RomCloud/bin/yt-dlp"
fi
if [ -f bin/yt-dlp-glibc ]; then
    cp bin/yt-dlp-glibc "$STAGING_DIR/Apps/RomCloud/bin/"
    chmod +x "$STAGING_DIR/Apps/RomCloud/bin/yt-dlp-glibc"
fi
if [ -d scripts ]; then
    cp -r scripts/* "$STAGING_DIR/Apps/RomCloud/scripts/"
    chmod +x "$STAGING_DIR/Apps/RomCloud/scripts/"*.sh 2>/dev/null || true
fi

cp assets/fonts/font.ttf "$STAGING_DIR/Apps/RomCloud/assets/fonts/"
cp assets/icons/*.png "$STAGING_DIR/Apps/RomCloud/assets/icons/"
cp assets/apps_icons/*.png "$STAGING_DIR/Apps/RomCloud/assets/apps_icons/" 2>/dev/null || true
cp -r assets/player_icons/* "$STAGING_DIR/Apps/RomCloud/assets/player_icons/" 2>/dev/null || true
cp config/settings.json "$STAGING_DIR/Apps/RomCloud/config/"
if [ -f config/input.conf ]; then
    cp config/input.conf "$STAGING_DIR/Apps/RomCloud/config/"
fi
cp -f iptv/*.m3u iptv/*.m3u8 "$STAGING_DIR/Apps/RomCloud/iptv/" 2>/dev/null || true
cp -f iptv/sources.txt "$STAGING_DIR/Apps/RomCloud/iptv/" 2>/dev/null || true

# Ensure execution permissions
chmod +x "$STAGING_DIR/Apps/RomCloud/launch.sh" "$STAGING_DIR/Apps/RomCloud/bin/RomCloud"

# Create Lite Installer zip (no heavy mpv/lib bundles, only ~5MB, ideal for first-time copy)
mkdir -p "$DIST_DIR"
rm -f "$DIST_DIR/$ZIP_NAME" "$DIST_DIR/RomCloud-Lite-Installer.zip" "$DIST_DIR/mpv_bundle.zip"
cd "$STAGING_DIR"
zip -r "$DIST_DIR/RomCloud-Lite-Installer.zip" Apps

# Now add mpv and libraries for Full package
cd "$SCRIPT_DIR"
if [ -f bin/mpv ]; then
    cp bin/mpv "$STAGING_DIR/Apps/RomCloud/bin/"
    chmod +x "$STAGING_DIR/Apps/RomCloud/bin/mpv"
fi
if [ -d lib ]; then
    cp -P lib/*.so* "$STAGING_DIR/Apps/RomCloud/lib/" 2>/dev/null || true
fi

# Create Full package zip
cd "$STAGING_DIR"
zip -r "$DIST_DIR/$ZIP_NAME" Apps

# Create mpv_bundle.zip for OTA update delivery
cd "$STAGING_DIR/Apps/RomCloud"
zip -r -9 "$DIST_DIR/mpv_bundle.zip" bin/mpv lib bin/yt-dlp bin/yt-dlp-glibc scripts assets/player_icons assets/apps_icons/YOUTUBE.png
cd "$SCRIPT_DIR"
rm -rf "$STAGING_DIR"

cp -f bin/RomCloud "$DIST_DIR/RomCloud"
cp -f launch.sh "$DIST_DIR/launch.sh"
cp -f icon.png "$DIST_DIR/icon.png"

echo "=== Release Packages Created Successfully ==="
ls -lh "$DIST_DIR/RomCloud-Lite-Installer.zip" "$DIST_DIR/$ZIP_NAME" "$DIST_DIR/mpv_bundle.zip" "$DIST_DIR/RomCloud" "$DIST_DIR/icon.png"
