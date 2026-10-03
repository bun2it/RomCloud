#!/bin/sh
# check_brick_game.sh - Chẩn đoán tại sao Brick không load game
# Cách dùng trên Brick: copy file này vào /mnt/SDCARD/ rồi chạy: sh /mnt/SDCARD/check_brick_game.sh
# Hoặc qua adb: adb push check_brick_game.sh /mnt/SDCARD/ && adb shell sh /mnt/SDCARD/check_brick_game.sh

echo "========== 1. DEVICE & OS =========="
cat /etc/trimui_device.txt 2>/dev/null && echo "(device code ^)"
uname -a
echo "FW version:"; cat /mnt/SDCARD/System/version.txt 2>/dev/null; cat /usr/trimui/version.txt 2>/dev/null
ls /mnt/SDCARD/ | head -n 50
echo ""
echo "OS type:"
[ -d /mnt/SDCARD/Emus ] && echo "- Stock/NextUI style: Emus/ TỒN TẠI" || echo "- Không thấy Emus/"
[ -d /mnt/SDCARD/Emu ] && echo "- Spruce style: Emu/ TỒN TẠI" || echo "- Không thấy Emu/ (singular)"
[ -d /mnt/SDCARD/.nextui ] && echo "- NextUI marker: CÓ" || echo "- NextUI marker: không"
[ -d /mnt/SDCARD/spruce ] && echo "- Spruce marker: CÓ" || echo "- Spruce marker: không"

echo ""
echo "========== 2. ROMS STRUCTURE =========="
ls /mnt/SDCARD/Roms/ 2>/dev/null || echo "!! KHÔNG CÓ /mnt/SDCARD/Roms/ -> ĐÂY LÀ LỖI #1"
for d in /mnt/SDCARD/Roms/*/; do
  [ -d "$d" ] || continue
  n=$(ls -1 "$d" 2>/dev/null | wc -l)
  echo "[$n files] $d"
  ls -1 "$d" 2>/dev/null | head -n 5 | sed 's/^/    /'
done

echo ""
echo "========== 3. EMULATOR / CORE =========="
echo "--- /mnt/SDCARD/Emus (Stock) ---"
ls /mnt/SDCARD/Emus/ 2>/dev/null | head -n 40 || echo "(không có)"
echo "--- /mnt/SDCARD/Emu (Spruce) ---"
ls /mnt/SDCARD/Emu/ 2>/dev/null | head -n 40 || echo "(không có)"
echo "--- RetroArch cores ---"
ls /mnt/SDCARD/RetroArch/.retroarch/cores/ 2>/dev/null | head -n 40
ls /mnt/SDCARD/System/cores/ 2>/dev/null | head -n 40
which retroarch; ls /usr/bin/retroarch* /usr/trimui/bin/* 2>/dev/null | head -n 20

echo ""
echo "========== 4. BIOS (nguyên nhân đen màn văng ra) =========="
for f in bios.bin scph1001.bin scph5500.bin scph5501.bin scph5502.bin neogeo.zip gba_bios.bin bios7.bin bios9.bin firmware.bin; do
  found=$(find /mnt/SDCARD -maxdepth 4 -iname "$f" 2>/dev/null | head -n 2)
  if [ -n "$found" ]; then echo "OK  $f -> $found"; else echo "MISS $f"; fi
done
echo "--- thư mục System/BIOS ---"
ls /mnt/SDCARD/System/bios/ 2>/dev/null | head -n 20
ls /mnt/SDCARD/RetroArch/.retroarch/system/ 2>/dev/null | head -n 20
ls /mnt/SDCARD/Bios/ 2>/dev/null | head -n 20

echo ""
echo "========== 5. FILE LẠ / SAI ĐUÔI =========="
echo "File 0KB (ROM lỗi, copy dở):"
find /mnt/SDCARD/Roms -type f -size 0 2>/dev/null | head -n 20
echo "File không có đuôi game quen thuộc:"
find /mnt/SDCARD/Roms -type f 2>/dev/null | grep -viE '\.(gba|gbc|gb|nes|fds|sfc|smc|md|bin|gen|smd|cue|iso|chd|pbp|m3u|cso|n64|z64|v64|nds|zip|7z|pce|a26|gdi|cdi|pak|p8|png|sms|gg|lnx|ws|wsc|mdf|img|toc|cbn)$' | head -n 20

echo ""
echo "========== 6. PS .bin lẻ thiếu .cue (lỗi phổ biến) =========="
for sys in PS SEGACD PCE SFC SS DC; do
  if [ -d "/mnt/SDCARD/Roms/$sys" ]; then
    echo "--- $sys ---"
    for b in /mnt/SDCARD/Roms/$sys/*.bin /mnt/SDCARD/Roms/$sys/*.BIN; do
      [ -f "$b" ] || continue
      cue="${b%.*}.cue"; cue2="${b%.*}.CUE"
      if [ ! -f "$cue" ] && [ ! -f "$cue2" ]; then echo "THIẾU .cue cho: $b"; fi
    done | head -n 10
  fi
done

echo ""
echo "========== 7. THẺ NHỚ & QUYỀN =========="
df -h /mnt/SDCARD | head -n 5
dmesg 2>/dev/null | grep -iE 'mmc|I/O error|FAT|exFAT|corrupt' | tail -n 20
echo "--- quyền thực thi Emu ---"
ls -l /mnt/SDCARD/Emus/*/*.sh 2>/dev/null | head -n 10
mount | grep -i sdcard | head -n 5

echo ""
echo "========== 8. LOG GẦN NHẤT =========="
echo "--- MainUI log ---"
ls -lt /mnt/SDCARD/*.log /mnt/SDCARD/Apps/*/logs/* /tmp/*.log 2>/dev/null | head -n 10
tail -n 50 /mnt/SDCARD/Apps/RomCloud/debug.log 2>/dev/null
dmesg 2>/dev/null | tail -n 30
echo ""
echo "XONG. Copy toàn bộ output này gửi lại để chẩn đoán."
