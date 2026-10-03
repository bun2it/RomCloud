#!/bin/sh
# check_app_crash.sh - Bắt lỗi "Loading rồi văng ra" trên TrimUI Brick
# Cách dùng: sh /mnt/SDCARD/check_app_crash.sh "RetroHub"
# Nó sẽ chạy tay launch.sh của app đó và in ra lỗi thật (thay vì chỉ thấy Loading)

APP="$1"
if [ -z "$APP" ]; then
  echo "Dùng: sh $0 <TenApp>"
  echo "VD: sh $0 RetroHub"
  echo ""
  echo "Các app hiện có:"
  ls -1 /mnt/SDCARD/Apps/ 2>/dev/null
  ls -1 /mnt/SDCARD/App/ 2>/dev/null
  exit 1
fi

# Tìm folder app (Stock: Apps/ , Spruce: App/)
APPDIR=""
[ -d "/mnt/SDCARD/Apps/$APP" ] && APPDIR="/mnt/SDCARD/Apps/$APP"
[ -d "/mnt/SDCARD/App/$APP" ] && APPDIR="/mnt/SDCARD/App/$APP"

if [ -z "$APPDIR" ]; then
  echo "!! KHÔNG TÌM THẤY app $APP"
  echo "--- /mnt/SDCARD/Apps/ ---"; ls /mnt/SDCARD/Apps/ 2>/dev/null
  echo "--- /mnt/SDCARD/App/ ---"; ls /mnt/SDCARD/App/ 2>/dev/null
  exit 1
fi

echo "========== APP: $APPDIR =========="
echo "--- config.json ---"
cat "$APPDIR/config.json" 2>/dev/null || echo "(không có config.json)"
echo ""
echo "--- file launch ---"
ls -l "$APPDIR/" | head -n 20
echo ""
LAUNCH=$(cat "$APPDIR/config.json" 2>/dev/null | grep -o '"launch"[^,]*' | cut -d'"' -f4)
[ -z "$LAUNCH" ] && LAUNCH="launch.sh"
echo "launch file theo config: $LAUNCH"
ls -l "$APPDIR/$LAUNCH" 2>&1
echo ""
echo "--- quyền thực thi ---"
[ -x "$APPDIR/$LAUNCH" ] && echo "OK: có quyền +x" || echo "LỖI: MẤT quyền +x -> chmod +x ngay"

echo ""
echo "========== CHẠY TAY ĐỂ LẤY LỖI THẬT =========="
cd "$APPDIR" || exit 1
echo "--- nội dung $LAUNCH (20 dòng đầu) ---"
head -n 20 "$LAUNCH" 2>/dev/null
echo ""
echo "--- chạy thử, ghi exit code ---"
sh "$LAUNCH" 2>&1 | head -n 60
echo "EXIT CODE: $?  (139=segfault thiếu lib, 127=lệnh/file không tồn tại, 126=mất quyền)"

echo ""
echo "========== CHECK LIB THIẾU (lý do văng phổ biến nhất) =========="
# Tìm binary chính trong folder app
BIN=$(find "$APPDIR/bin" -type f 2>/dev/null | head -n 5)
[ -z "$BIN" ] && BIN=$(find "$APPDIR" -maxdepth 2 -type f -name "*etro*" -o -maxdepth 2 -type f -name "*ame*" 2>/dev/null | head -n 5)
echo "Binary nghi ngờ: $BIN"
for b in $BIN; do
  echo "--- ldd $b ---"
  ldd "$b" 2>&1 | head -n 30
done

echo ""
echo "========== SO SÁNH VỚI RomCloud (app chạy được) =========="
echo "--- RomCloud launch head ---"
head -n 15 /mnt/SDCARD/Apps/RomCloud/launch.sh 2>/dev/null
echo "--- lib của RomCloud ---"
ls /mnt/SDCARD/Apps/RomCloud/lib/ 2>/dev/null | head -n 20
echo "--- lib hệ thống ---"
ls /mnt/SDCARD/System/lib/ 2>/dev/null | head -n 20 || echo "!! MẤT /mnt/SDCARD/System/lib -> ĐÂY LÀ NGUYÊN NHÂN SẬP ALL"
ls /usr/lib/libSDL2* 2>/dev/null | head -n 5

echo ""
echo "========== RAM / DISK =========="
free 2>/dev/null | head -n 5; df -h /mnt/SDCARD 2>/dev/null | head -n 3
dmesg 2>/dev/null | tail -n 15
echo ""
echo "XONG. Copy toàn bộ output gửi lại."
