#!/bin/bash
# deploy-adb.sh — Deploy full-runtime lên máy test qua adb (không chỉ binary).
#
# Bài học: chỉ `adb push bin/*` khiến máy test lệch dần khỏi release
# (thiếu launch.sh/scripts/assets/lib mới -> lỗi tính năng động).
# Script này bung full-zip lên máy, GIỮ dữ liệu user (data/config/iptv),
# tương đương luồng installFullZip của máy.
#
# Usage: ./deploy-adb.sh [ZIP] [SERIAL]
#   ZIP    mặc định = dist/RomCloud-v*.zip mới nhất
#   SERIAL mã máy khi cắm nhiều máy (adb -s)
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

ZIP="${1:-$(ls -t dist/RomCloud-v*.zip 2>/dev/null | head -n 1)}"
SERIAL="$2"
if [ -n "$SERIAL" ]; then
    ADB="adb -s $SERIAL"
else
    ADB="adb"
fi

if [ -z "$ZIP" ] || [ ! -f "$ZIP" ]; then
    echo "ERROR: không tìm thấy zip. Chạy ./package.sh trước hoặc truyền path zip." >&2
    exit 1
fi
if ! $ADB get-state >/dev/null 2>&1; then
    echo "ERROR: không thấy máy adb." >&2
    exit 1
fi

APP_ROOT="/mnt/SDCARD/Apps/RomCloud"
STAGE="/mnt/SDCARD/.rc_deploy"
BAK="/mnt/SDCARD/.rc_deploy_bak"

echo "=== Deploy $(basename "$ZIP") qua adb ==="
$ADB push "$ZIP" "$STAGE.zip"

$ADB shell <<EOF
set -e
killall -9 RomCloud 2>/dev/null || true
rm -rf "$STAGE" "$BAK"
mkdir -p "$STAGE" "$BAK"
unzip -o -q "$STAGE.zip" 'Apps/RomCloud/*' -d "$STAGE"
# Giữ dữ liệu user (giống installFullZip: user thắng khi trùng)
for d in data config iptv; do
  [ -d "$APP_ROOT/\$d" ] && cp -a "$APP_ROOT/\$d" "$BAK/\$d"
done
cp -a "$STAGE/Apps/RomCloud/." "$APP_ROOT/"
for d in data config iptv; do
  [ -d "$BAK/\$d" ] && cp -a "$BAK/\$d/." "$APP_ROOT/\$d/"
done
chmod 755 "$APP_ROOT"/bin/* "$APP_ROOT"/launch.sh 2>/dev/null || true
chmod 755 "$APP_ROOT"/scripts/*.sh 2>/dev/null || true
sync
rm -rf "$STAGE" "$BAK" "$STAGE.zip"
echo DEPLOY_OK
grep -a -o '2\.5\.[0-9]' "$APP_ROOT/bin/RomCloud" | sort -u | tr '\n' ' '
echo
ls "$APP_ROOT"/launch.sh "$APP_ROOT"/scripts/youtube_search.sh "$APP_ROOT"/.ota_flavor
EOF
echo "=== Xong ==="
