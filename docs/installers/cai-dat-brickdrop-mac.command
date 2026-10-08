#!/bin/bash
# BrickDrop - Cai dat 1 cham cho Mac (double-click file nay)
# Tu tai adb + ban BrickDrop moi nhat, day vao may qua cap USB.
cd "$(dirname "$0")"

echo "=========================================="
echo "  Cai dat BrickDrop 1 cham (Mac)"
echo "=========================================="
echo "1. Cam TrimUI Brick (dang bat) vao Mac."
echo "2. Tren may Brick: de che do USB = ADB."
echo ""

if [ ! -x "./platform-tools/adb" ]; then
  echo "[*] Dang tai cong cu adb (chi 1 lan dau)..."
  mkdir -p ./platform-tools
  curl -L --progress-bar --fail -o /tmp/bd-pt.zip "https://dl.google.com/android/repository/platform-tools-latest-darwin.zip" || {
    echo "[!] Khong tai duoc adb. Kiem tra mang roi mo lai file nay."; read -p "Enter de thoat"; exit 1; }
  rm -rf /tmp/bd-pt ./platform-tools
  mkdir -p /tmp/bd-pt
  unzip -q -o /tmp/bd-pt.zip -d /tmp/bd-pt
  mkdir -p ./platform-tools
  mv /tmp/bd-pt/platform-tools/* ./platform-tools/
  rm -rf /tmp/bd-pt /tmp/bd-pt.zip
fi

echo "[*] Cho may Brick (bam Enter khi da cam cap)..."
read -r _
./platform-tools/adb kill-server >/dev/null 2>&1
echo "[*] Doi may hien len..."
./platform-tools/adb wait-for-device
echo "[*] Thay may roi."

echo "[*] Dang hoi ban BrickDrop moi nhat..."
URL=$(curl -s "https://api.github.com/repos/bun2it/Brickdrop/releases/latest" | python3 -c "import json,sys; d=json.load(sys.stdin); print([a['browser_download_url'] for a in d['assets'] if a['name'].endswith('.zip')][0])")
if [ -z "$URL" ]; then
  echo "[!] Khong lay duoc link tai. Kiem tra mang."; read -p "Enter de thoat"; exit 1
fi
echo "[*] Dang tai ban moi (nhe, vai MB)..."
curl -L --progress-bar --fail -o /tmp/brickdrop.zip "$URL" || {
  echo "[!] Tai zip loi."; read -p "Enter de thoat"; exit 1; }

TOT=$(stat -f%z /tmp/brickdrop.zip 2>/dev/null || stat -c%s /tmp/brickdrop.zip)
TOTMB=$((TOT / 1048576))
echo "[*] Dang day vao the nho (tong ${TOTMB}MB)..."
./platform-tools/adb push /tmp/brickdrop.zip /mnt/SDCARD/BrickDrop-install.zip >/tmp/bd-push.log 2>&1 &
PPID=$!
while kill -0 $PPID 2>/dev/null; do
  SZ=$(./platform-tools/adb shell stat -c %s /mnt/SDCARD/BrickDrop-install.zip 2>/dev/null | tr -d '\r')
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
./platform-tools/adb shell 'cd /mnt/SDCARD && unzip -o -q BrickDrop-install.zip && chmod +x Apps/BrickDrop/launch.sh Apps/BrickDrop/bin/brickdrop && rm -f BrickDrop-install.zip && sync && echo DONE'

echo ""
echo "=========================================="
echo "  XONG! Rut cap, mo BrickDrop tren may."
echo "=========================================="
rm -f /tmp/brickdrop.zip
read -p "Enter de thoat"
