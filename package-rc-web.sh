#!/bin/bash
# package-rc-web.sh — đóng gói RC-Web thành 1 file zip duy nhất để phát
# hành qua OTA (UpdateManager::installRcWebZip) hoặc chép tay.
#
# Vào:  cây rc-web đã ráp bằng chrome/fetch-rc-web.sh
#       (mặc định $HOME/Downloads/rc-web)
# Ra:   dist/rc-web-<rc_web_version>.zip (entries ở gốc: app/, electron/,
#       syslibs/, weston/, launch.sh, version.txt, ...)
#
# version lấy từ version.json (trường rc_web_version) và được ghi đè vào
# version.txt trong gói — máy dùng file này để biết có bản mới không.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RCVER=$(grep '"rc_web_version"' version.json | head -n 1 | awk -F'"' '{print $4}')
if [ -z "$RCVER" ]; then
    echo "ERROR: no rc_web_version in version.json" >&2
    exit 1
fi

SRC="${1:-$HOME/Downloads/rc-web}"
if [ ! -x "$SRC/electron/electron" ] || [ ! -f "$SRC/launch.sh" ]; then
    echo "ERROR: $SRC is not a complete rc-web tree (missing electron/launch.sh)." >&2
    echo "Run ./chrome/fetch-rc-web.sh first." >&2
    exit 1
fi

# Đảm bảo version.txt trong gói khớp manifest (ghi đè, không tin bản cũ).
echo "$RCVER" > "$SRC/version.txt"
echo "version.txt = $RCVER"

DIST_DIR="$SCRIPT_DIR/dist"
ZIP_NAME="rc-web-${RCVER}.zip"
mkdir -p "$DIST_DIR"
rm -f "$DIST_DIR"/rc-web-*.zip

echo "=== Packaging RC-Web $RCVER ==="
# Loại rc-web.log (log phiên chạy trên máy, không phải runtime) và file
# zip/log tạm nếu ai đó để lẫn trong cây.
(cd "$SRC" && zip -qr "$DIST_DIR/$ZIP_NAME" . -x "*.log" ".DS_Store" "*/.DS_Store")

echo "=== RC-Web Package Created ==="
ls -lh "$DIST_DIR/$ZIP_NAME"
unzip -l "$DIST_DIR/$ZIP_NAME" | grep -E "electron/electron|launch.sh|version.txt|syslibs/|weston/bin/weston" | head -8

echo ""
echo "Upload asset này lên GitHub release (kèm RomCloud-vX.Y.Z.zip):"
echo "  gh release upload v<VERSION> $DIST_DIR/$ZIP_NAME"
echo "và đảm bảo version.json có:"
echo "  \"rc_web_version\": \"$RCVER\""
echo "  \"rc_web_url\": \"https://github.com/bun2it/RomCloud/releases/download/v<VERSION>/$ZIP_NAME\""
