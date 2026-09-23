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
mkdir -p "$STAGING_DIR/Apps/RomCloud/assets/fonts"
mkdir -p "$STAGING_DIR/Apps/RomCloud/assets/icons"
mkdir -p "$STAGING_DIR/Apps/RomCloud/config"

# Copy essential runtime files
cp config.json "$STAGING_DIR/Apps/RomCloud/"
cp icon.png "$STAGING_DIR/Apps/RomCloud/"
cp launch.sh "$STAGING_DIR/Apps/RomCloud/"
cp bin/RomCloud "$STAGING_DIR/Apps/RomCloud/bin/"
cp assets/fonts/font.ttf "$STAGING_DIR/Apps/RomCloud/assets/fonts/"
cp assets/icons/*.png "$STAGING_DIR/Apps/RomCloud/assets/icons/"
cp config/settings.json "$STAGING_DIR/Apps/RomCloud/config/"

# Ensure execution permissions
chmod +x "$STAGING_DIR/Apps/RomCloud/launch.sh" "$STAGING_DIR/Apps/RomCloud/bin/RomCloud"

# Create zip
mkdir -p "$DIST_DIR"
rm -f "$DIST_DIR/$ZIP_NAME"
cd "$STAGING_DIR"
zip -r "$DIST_DIR/$ZIP_NAME" Apps
cd "$SCRIPT_DIR"
rm -rf "$STAGING_DIR"

echo "=== Release Package Created Successfully: dist/$ZIP_NAME ==="
ls -lh "$DIST_DIR/$ZIP_NAME"
