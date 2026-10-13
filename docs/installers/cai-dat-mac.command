#!/bin/bash
# RomCloud - Cai dat 1 cham cho Mac qua USB Storage (double-click file nay)
# Khong can driver, khong can adb: chep file thang vao the nho.
cd "$(dirname "$0")"

echo "=========================================="
echo "  Cai dat RomCloud 1 cham (Mac)"
echo "  (qua USB Storage - khong can driver/adb)"
echo "=========================================="
echo "1. Tren may Brick: mo app USB Storage (trong Apps he thong)."
echo "2. Cho the nho hien len Mac roi bam Enter."
echo ""
read -r _

echo "[*] Dang tim the nho (toi da 90s)..."
DRIVE=""
for i in $(seq 1 30); do
    for d in /Volumes/*; do
        [ -d "$d/Apps/RomCloud" ] && DRIVE="$d" && break 2
    done
    for d in /Volumes/*; do
        if [ -d "$d/Emus" ] && [ -d "$d/Roms" ]; then DRIVE="$d"; break 2; fi
    done
    sleep 3
done
if [ -z "$DRIVE" ]; then
    echo "[!] Khong thay the nho. Ban da mo app USB Storage tren Brick chua?"
    read -p "Enter de thoat"
    exit 1
fi
echo "[*] Thay the nho: $DRIVE (ok)"

echo "[*] Dang hoi ban RomCloud moi nhat (ban Core nhe de day nhanh)..."
URL=$(curl -s "https://api.github.com/repos/bun2it/RomCloud/releases/latest" | python3 -c "import json,sys; d=json.load(sys.stdin); z=[a for a in d['assets'] if a['name'].endswith('.zip') and not a['name'].startswith('rc-web-')]; core=[a['browser_download_url'] for a in z if 'ota-core-v' in a['name']]; lite=[a['browser_download_url'] for a in z if '-lite.zip' in a['name']]; full=[a['browser_download_url'] for a in z if 'RomCloud-v' in a['name'] and '-lite.zip' not in a['name']]; print((core+lite+full+[a['browser_download_url'] for a in z]+[''])[0])")
if [ -z "$URL" ]; then
    echo "[!] Khong lay duoc link tai. Kiem tra mang."
    read -p "Enter de thoat"
    exit 1
fi
echo "[*] Dang tai ban moi..."
curl -L --progress-bar --fail -o /tmp/romcloud.zip "$URL" || {
    echo "[!] Tai zip loi. Kiem tra mang."
    read -p "Enter de thoat"
    exit 1
}

echo "[*] Dang bung vao the nho (ghi de, giu nguyen game/save)..."
unzip -q -o /tmp/romcloud.zip -d "$DRIVE" || {
    echo "[!] Bung zip loi."
    read -p "Enter de thoat"
    exit 1
}
rm -f /tmp/romcloud.zip

echo "[*] Dang eject the nho (an toan truoc khi rut)..."
diskutil eject "$DRIVE" >/dev/null 2>&1 || diskutil unmountDisk "$DRIVE" >/dev/null 2>&1 || true

echo ""
echo "=========================================="
echo "  XONG! Thoat app USB Storage tren Brick,"
echo "  mo RomCloud tren may va dung."
echo "  (Neu cai ban Core: mo app lan dau co WiFi"
echo "  de may tu tai not phan con lai.)"
echo "  (Neu o USB chua bien mat: eject tay trong"
echo "  Finder roi thoat app tren Brick.)"
echo "=========================================="
read -p "Enter de thoat"
