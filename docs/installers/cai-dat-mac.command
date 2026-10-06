#!/bin/bash
# RomCloud - Cai dat 1 cham cho Mac (double-click file nay)
# Tu tai adb + ban RomCloud moi nhat, day vao may qua cap USB.
cd "$(dirname "$0")"

echo "=========================================="
echo "  Cai dat RomCloud 1 cham (Mac)"
echo "=========================================="
echo "1. Cam TrimUI Brick (dang bat) vao Mac."
echo "2. Tren may Brick: de che do USB = ADB."
echo ""

if [ ! -x "./platform-tools/adb" ]; then
  echo "[*] Dang tai cong cu adb (chi 1 lan dau)..."
  mkdir -p ./platform-tools
  curl -L --progress-bar --fail -o /tmp/rc-pt.zip "https://dl.google.com/android/repository/platform-tools-latest-darwin.zip" || {
    echo "[!] Khong tai duoc adb. Kiem tra mang roi mo lai file nay."; read -p "Enter de thoat"; exit 1; }
  rm -rf /tmp/rc-pt ./platform-tools
  mkdir -p /tmp/rc-pt
  unzip -q -o /tmp/rc-pt.zip -d /tmp/rc-pt
  mkdir -p ./platform-tools
  mv /tmp/rc-pt/platform-tools/* ./platform-tools/
  rm -rf /tmp/rc-pt /tmp/rc-pt.zip
fi

echo "[*] Cho may Brick (bam Enter khi da cam cap)..."
read -r _
./platform-tools/adb kill-server >/dev/null 2>&1
echo "[*] Doi may hien len..."
./platform-tools/adb wait-for-device
echo "[*] Thay may roi."

echo "[*] Dang hoi ban RomCloud moi nhat..."
URL=$(curl -s "https://api.github.com/repos/bun2it/RomCloud/releases/latest" | python3 -c "import json,sys; d=json.load(sys.stdin); print([a['browser_download_url'] for a in d['assets'] if a['name'].endswith('.zip')][0])")
if [ -z "$URL" ]; then
  echo "[!] Khong lay duoc link tai. Kiem tra mang."; read -p "Enter de thoat"; exit 1
fi
echo "[*] Dang tai ban moi (~54MB)..."
curl -L --progress-bar --fail -o /tmp/romcloud.zip "$URL" || {
  echo "[!] Tai zip loi."; read -p "Enter de thoat"; exit 1; }

TOT=$(stat -f%z /tmp/romcloud.zip 2>/dev/null || stat -c%s /tmp/romcloud.zip)
TOTMB=$((TOT / 1048576))
echo "[*] Dang day vao the nho (tong ${TOTMB}MB)..."
./platform-tools/adb push /tmp/romcloud.zip /mnt/SDCARD/RomCloud-install.zip >/tmp/rc-push.log 2>&1 &
PPID=$!
while kill -0 $PPID 2>/dev/null; do
  SZ=$(./platform-tools/adb shell stat -c %s /mnt/SDCARD/RomCloud-install.zip 2>/dev/null | tr -d '\r')
  case "$SZ" in ''|*[!0-9]*) SZ=0 ;; esac
  PCT=$((SZ * 100 / TOT))
  printf "\r[*] Day file... %s/%sMB (%s%%)" "$((SZ / 1048576))" "$TOTMB" "$PCT"
  sleep 2
done
wait $PPID
PUSH_OK=$?
echo ""
if [ $PUSH_OK -ne 0 ]; then
  echo "[!] Day file loi. Thu rut cap cam lai."; read -p "Enter de thoat"; exit 1
fi

echo "[*] Dang bung + cap quyen..."
./platform-tools/adb shell 'cd /mnt/SDCARD && unzip -o -q RomCloud-install.zip && chmod +x Apps/RomCloud/launch.sh Apps/RomCloud/bin/RomCloud Apps/RomCloud/bin/gamecast_d && rm -f RomCloud-install.zip && sync && echo DONE'

echo ""
echo "=========================================="
echo "  XONG! Rut cap, mo RomCloud tren may."
echo "=========================================="
rm -f /tmp/romcloud.zip
read -p "Enter de thoat"
