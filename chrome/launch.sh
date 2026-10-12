#!/bin/sh
# launch.sh — RC-Web của RomCloud: launcher chạy trên máy (trong rc-web/).
# RomCloud fork script này rồi giám sát bằng waitpid; thoát phiên bằng
# SIGTERM -> trap cleanup dọn weston/electron/gptokeyb, trả màn hình.
#
# Luồng: dừng MainUI (stock) -> seatd -> Weston DRM (pixman, Xwayland)
# -> chờ X11 sẵn sàng -> gptokeyb2 (tay cầm) -> Electron tiền cảnh.
#
# TRACE: mọi bước đều ghi 1 dòng [WEBTRACE] Lxx vào rc-web.log (cạnh
# script) kèm giờ máy. Mã bước: B=bridge trong debug.log, L=launcher ở
# đây, J=electron main. Khi lỗi, gửi cả debug.log + rc-web.log là thấy
# phiên chết ở bước nào, sau bao lâu, RAM còn bao nhiêu lúc đó.
cd "$(dirname "$0")" || exit 1
RC_DIR="$PWD"
LOG="$RC_DIR/rc-web.log"
: > "$LOG"
# Cờ lớp phụ của RomCloud (/tmp/rc-web-ui-mode) vòng đời theo phiên:
# xóa lúc mở + lúc dọn — Electron chết giữa chừng (file kẹt "kb"/
# "pointer") thì B lần sau vẫn thoát được, không rò phiên.
rm -f /tmp/rc-web-ui-mode
exec >>"$LOG" 2>&1

# ── Hàm trace + chụp trạng thái máy ──────────────────────────────────
# RC_WEB_DEBUG=1 mới chụp SNAP đầy đủ (free+ps+dmesg fork nhiều tiến
# trình, chậm trên CPU yếu). Mặc định chỉ ghi dòng trace nhẹ.
DEBUG="${RC_WEB_DEBUG:-0}"
T0=$(date +%s)
TR() {
  NOW=$(date +%s)
  echo "[WEBTRACE] $1 +$((NOW - T0))s $(date '+%H:%M:%S') $2"
}
SNAP() {
  [ "$DEBUG" = 1 ] || return 0
  # Ảnh chụp nhanh: RAM trống + tiến trình ăn RAM nhất + cuối dmesg
  # (kernel ghi rõ khi giết tiến trình vì hết RAM).
  echo "--- SNAP ($1) ---"
  if command -v free >/dev/null 2>&1; then free; else grep -E 'MemTotal|MemFree|MemAvailable' /proc/meminfo; fi
  ps -o pid,rss,comm 2>/dev/null | sort -k2 -rn 2>/dev/null | head -6
  dmesg 2>/dev/null | tail -8
  echo "--- /SNAP ---"
}

TR L00 "launch.sh bắt đầu, thư mục=$RC_DIR"
uname -a 2>/dev/null
SNAP "vừa vào launcher"
# set -x chi khi debug (RC_WEB_DEBUG=1): han che fork + ghi log tren
# the cham, bot vai giay boot. Mac dinh chi ghi dong trace.
if [ "$DEBUG" = 1 ]; then set -x; fi

WROOT="$RC_DIR/weston"
WLIB="$WROOT/lib/aarch64-linux-gnu"
RUN_DIR=/tmp/rc-web-run
mkdir -p "$RC_DIR/conf/profile" "$RUN_DIR"
chmod 700 "$RUN_DIR" 2>/dev/null
# Profile/cache/tmp của Electron sang tmpfs (/tmp = RAM): Chromium ghi
# LevelDB + cache file nhỏ liên tục, để trên thẻ SD (FAT) vừa chậm vừa
# dễ corrupt khi bị SIGKILL. Chỉ lịch sử địa chỉ là cần giữ lại qua
# phiên (khôi phục/lưu ở dưới + trong cleanup).
RUNDATA=/tmp/rc-web-data
mkdir -p "$RUNDATA/profile" "$RUNDATA/cache" "$RUNDATA/tmp"
if [ ! -f "$RUNDATA/profile/rc-history.json" ] && \
   [ -f "$RC_DIR/conf/profile/rc-history.json" ]; then
  cp -f "$RC_DIR/conf/profile/rc-history.json" \
        "$RUNDATA/profile/rc-history.json" 2>/dev/null
fi

PIDS=""
GOV_SAVE=""
MAINUI_STOPPED=0
SEATD_STARTED=0

cleanup() {
  TR L12 "cleanup: dọn weston/electron/gptokeyb, trả màn hình"
  rm -f /tmp/rc-web-ui-mode
  # Giữ lại duy nhất lịch sử địa chỉ (file nhỏ) về thẻ SD; profile còn
  # lại nằm trên tmpfs nên mất đi cũng không sao.
  if [ -f "$RUNDATA/profile/rc-history.json" ]; then
    cp -f "$RUNDATA/profile/rc-history.json" \
          "$RC_DIR/conf/profile/rc-history.json" 2>/dev/null
  fi
  # shellcheck disable=SC2086
  for p in $PIDS; do kill "$p" 2>/dev/null; done
  sleep 1
  killall -9 electron 2>/dev/null
  killall -9 Xwayland 2>/dev/null
  killall -9 weston 2>/dev/null
  killall -9 gptokeyb2 2>/dev/null
  if [ "$SEATD_STARTED" = 1 ]; then killall -9 seatd 2>/dev/null; fi
  if [ -n "$GOV_SAVE" ] && [ -w /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
    echo "$GOV_SAVE" > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null
  fi
  if [ "$MAINUI_STOPPED" = 1 ]; then killall -CONT MainUI 2>/dev/null; fi
  sync
  TR L12 "cleanup xong"
}
trap 'cleanup; exit 0' TERM INT

# Dọn phiên cũ nếu còn sót (thoát bất thường lần trước). Sach thi bo
# qua killall + sleep de tiet kiem ~2.5s boot.
if killall -0 electron 2>/dev/null || killall -0 weston 2>/dev/null || \
   killall -0 gptokeyb2 2>/dev/null || killall -0 Xwayland 2>/dev/null; then
  TR L01 "còn tiến trình cũ sót lại -> kill"
  killall -9 electron 2>/dev/null
  killall -9 gptokeyb2 2>/dev/null
  killall -9 weston 2>/dev/null
  killall -9 Xwayland 2>/dev/null
  sleep 1
  TR L01 "đã dọn tiến trình sót của phiên trước"
else
  TR L01 "không có tiến trình sót, bỏ qua dọn"
fi

# Stock OS: giữ MainUI đứng yên để nhường màn hình cho weston.
if [ -x /usr/trimui/bin/MainUI ] || [ -x /mnt/SDCARD/System/MainUI ]; then
  killall -STOP MainUI 2>/dev/null && MAINUI_STOPPED=1
  TR L02 "đã STOP MainUI (stock)"
else
  TR L02 "không thấy MainUI kiểu stock -> bỏ qua bước dừng"
fi

# CPU governor: tạm để performance cho mượt, cleanup trả lại.
GOV=/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
if [ -r "$GOV" ] && [ -w "$GOV" ]; then
  GOV_SAVE=$(cat "$GOV")
  echo performance > "$GOV" 2>/dev/null
  TR L03 "governor: $GOV_SAVE -> performance"
else
  TR L03 "không chỉnh được governor (bỏ qua)"
fi

# ── seatd (quản quyền /dev/dri + input cho weston) ──────────────────
export LIBSEAT_BACKEND=seatd
export SEATD_SOCK=/run/seatd.sock
# Socket sót (seatd cũ chết mà file còn): weston tưởng seatd sống sẽ treo
# chờ. Không có tiến trình seatd thì xóa socket đi làm lại từ đầu.
if [ -S "$SEATD_SOCK" ] && ! pidof seatd >/dev/null 2>&1; then
  rm -f "$SEATD_SOCK"
  TR L04 "xóa socket seatd sót (không có tiến trình seatd)"
fi
if [ -S "$SEATD_SOCK" ]; then
  TR L04 "seatd đã chạy sẵn (socket $SEATD_SOCK) -> dùng luôn"
elif [ -x "$WROOT/bin/seatd" ]; then
  "$WROOT/bin/seatd" &
  SEATD_STARTED=1
  i=0
  while [ ! -S "$SEATD_SOCK" ] && [ "$i" -lt 12 ]; do sleep 0.25; i=$((i+1)); done
  if [ -S "$SEATD_SOCK" ]; then
    TR L04 "seatd đã lên sau $((i * 250))ms"
  else
    export LIBSEAT_BACKEND=builtin
    TR L04 "seatd KHÔNG lên sau 3s -> LIBSEAT_BACKEND=builtin"
  fi
else
  export LIBSEAT_BACKEND=builtin
  TR L04 "không có bin/seatd trong gói -> LIBSEAT_BACKEND=builtin"
fi

# ── weston.ini (sinh tại chỗ vì đường dẫn tuỳ nơi cài) ───────────────
cat > "$RC_DIR/conf/weston.ini" <<EOF
[core]
require-input=false

[shell]
cursor-theme=default
cursor-size=48

[xwayland]
path=$WROOT/bin/Xwayland
EOF

# Thư mục module của weston/libweston (tên đổi theo version Debian).
MODDIR=""
for d in "$WLIB"/libweston-*; do [ -d "$d" ] && MODDIR="$d" && break; done
SHELLDIR="$WLIB/weston"
LIBEXEC=""
# Debian đặt helper (weston-keyboard, weston-desktop-shell) ngay trong
# thư mục lib đa kiến trúc; các layout khác để dự phòng.
for d in "$WLIB" "$WROOT/lib/weston" "$WROOT/libexec/weston"; do
  [ -e "$d/weston-desktop-shell" ] && LIBEXEC="$d" && break
done
TR L05 "module weston: MODDIR=$MODDIR SHELLDIR=$SHELLDIR LIBEXEC=$LIBEXEC"
ls "$MODDIR" "$SHELLDIR" 2>/dev/null

export XDG_RUNTIME_DIR="$RUN_DIR"
export WAYLAND_DISPLAY=wayland-1
export XKB_CONFIG_ROOT="$WROOT/share/X11/xkb"
export LD_LIBRARY_PATH="$WLIB:$WLIB/weston"
[ -n "$LIBEXEC" ] && export WESTON_LIBEXEC_DIR="$LIBEXEC"
if [ -n "$MODDIR" ]; then
  export WESTON_MODULE_MAP="drm-backend.so=$MODDIR/drm-backend.so;fbdev-backend.so=$MODDIR/fbdev-backend.so;kiosk-shell.so=$SHELLDIR/kiosk-shell.so;desktop-shell.so=$SHELLDIR/desktop-shell.so;xwayland.so=$MODDIR/xwayland.so"
fi

# ── Weston DRM chiếm toàn màn hình ───────────────────────────────────
# Xwayland cần thư mục chứa socket X11, máy không tự có sẵn.
mkdir -p /tmp/.X11-unix
chmod 1777 /tmp/.X11-unix 2>/dev/null || true

# Xwayland biên dịch sơ đồ phím bằng chương trình con xkbcomp, gọi theo
# đường dẫn cứng /usr/bin/xkbcomp và đọc dữ liệu ở /usr/share/X11/xkb.
# Máy không có sẵn hai thứ đó: trỏ symlink vào gói của mình (cần /usr ghi được).
XKB_OK=0
if [ -x "$WROOT/bin/xkbcomp" ] && ln -sf "$WROOT/bin/xkbcomp" /usr/bin/xkbcomp 2>/dev/null; then
  mkdir -p /usr/share/X11 2>/dev/null
  ln -sfn "$WROOT/share/X11/xkb" /usr/share/X11/xkb 2>/dev/null
  XKB_OK=1
fi
TR L05B "xkbcomp cho Xwayland: XKB_OK=$XKB_OK (1 = đã trỏ symlink vào /usr)"

# gdk-pixbuf trong Electron vẫn không đọc biến môi trường trỏ loader,
# nên trỏ thẳng thư mục loader + cache vào đường dẫn mặc định của thư viện.
PIXBUF_OK=0
PB_DIR=/usr/lib/aarch64-linux-gnu/gdk-pixbuf-2.0/2.10.0
if mkdir -p "$PB_DIR" 2>/dev/null; then
  ln -sfn "$RC_DIR/syslibs/pixbuf-loaders" "$PB_DIR/loaders" 2>/dev/null
  ln -sf "$RC_DIR/syslibs/pixbuf-loaders/loaders.cache" "$PB_DIR/loaders.cache" 2>/dev/null
  PIXBUF_OK=1
fi
TR L05C "gdk-pixbuf loader mac dinh: PIXBUF_OK=$PIXBUF_OK"

# Sinh lại loaders.cache ngay trên máy bằng công cụ của chính gdk-pixbuf
# (bản viết tay có thể lệch định dạng mà bộ đọc mục lục không chấp nhận).
PB_N=0
if [ -x "$RC_DIR/gdk-pixbuf-query-loaders" ]; then
  LD_LIBRARY_PATH="$RC_DIR/syslibs" "$RC_DIR/gdk-pixbuf-query-loaders" > "$RC_DIR/syslibs/pixbuf-loaders/loaders.cache" 2>/dev/null
  PB_N=$(grep -c libpixbufloader "$RC_DIR/syslibs/pixbuf-loaders/loaders.cache" || true)
fi
TR L05D "gdk-pixbuf loaders.cache sinh tren may: $PB_N loader"

# libinput của weston chỉ nhận thiết bị có thẻ ID_INPUT; bàn phím/chuột ảo
# của gptokeyb2 (uinput) chưa có thẻ nên bị bỏ qua. Cài rule để udev gán
# thẻ ngay khi gptokeyb2 tạo thiết bị (rootfs /etc ghi được).
UDEV_OK=0
UDEVADM="$(command -v udevadm 2>/dev/null)"
[ -x "$UDEVADM" ] || UDEVADM=/sbin/udevadm
[ -x "$UDEVADM" ] || UDEVADM=""
for UD in /etc/udev/rules.d /lib/udev/rules.d; do
  if mkdir -p "$UD" 2>/dev/null && printf '%s\n' 'KERNEL=="event*", ATTRS{name}=="Fake Keyboard Mouse", ENV{ID_INPUT}="1", ENV{ID_SEAT}="seat0", ENV{ID_INPUT_KEYBOARD}="1", ENV{ID_INPUT_MOUSE}="1", TAG+="seat"' 'KERNEL=="event*", ATTRS{name}=="Fake Absolute Mouse", ENV{ID_INPUT}="1", ENV{ID_SEAT}="seat0", ENV{ID_INPUT_MOUSE}="1", TAG+="seat"' > "$UD/99-rc-web-input.rules" 2>/dev/null; then
    UDEV_OK=1
  fi
done
if [ "$UDEV_OK" = 1 ] && [ -n "$UDEVADM" ]; then
  "$UDEVADM" control --reload-rules 2>/dev/null
fi
TR L05E "udev rule ban phim/chuot ao: UDEV_OK=$UDEV_OK UDEVADM=$UDEVADM"

# gptokeyb2 phải tạo thiết bị ảo TRƯỚC weston: weston/libinput chỉ đọc
# thẻ ID_INPUT lúc liệt kê thiết bị khi khởi động; thiết bị xuất hiện
# sau thời điểm đó (dù thẻ đã vào cơ sở dữ liệu udev) vẫn bị bỏ qua.
if [ -x "$RC_DIR/bin/gptokeyb2" ]; then
  SDL_GAMECONTROLLERCONFIG_FILE="$RC_DIR/gamecontrollerdb.txt" \
  LD_LIBRARY_PATH="$RC_DIR/bin" \
    "$RC_DIR/bin/gptokeyb2" electron -c "$RC_DIR/browser.gptk" &
  PIDS="$PIDS $!"
  TR L05F "gptokeyb2 đã chạy (trước weston), pid=$! (mapping browser.gptk)"
  if [ -n "$UDEVADM" ]; then
    "$UDEVADM" trigger --action=change --subsystem-match=input 2>/dev/null
    # Bo settle --timeout=2: weston khoi dong ~4s sau, udev kip ap rule.
    # Tiet kiem ~2s boot; thiet bi den muon thi weston nhan qua hot-plug.
  fi
  TR L05G "da kich change udev input (khong settle): UDEVADM=$UDEVADM"
  sleep 0.3
else
  TR L05F "CẢNH BÁO: thiếu bin/gptokeyb2 -> tay cầm không điều khiển được"
fi

# Weston 9 gọi backend bằng tên file module (drm-backend.so), không còn
# tên ngắn "drm". Thực đo trên Brick (2026-10-10, adb rc-web.log):
#  - drm-backend CHƯA BAO GIỜ lên được ("no drm device found" do thiếu
#    udevd; kể cả chỉ card cũng kẹt "/dev/tty1 is already in graphics
#    mode"). DRM chỉ còn là dự phòng.
#  - fbdev-backend lên trong ~250ms và HIỂN THỊ ĐƯỢC (chung /dev/fb0 với
#    RomCloud, ai vẽ sau thắng) -> thử fbdev TRƯỚC cho boot nhanh.
# last-be chỉ ghi khi DRM thắng (hiếm); cache khác thì xóa.
LASTBE=""
[ -f "$RC_DIR/conf/last-be" ] && LASTBE="$(cat "$RC_DIR/conf/last-be" 2>/dev/null)"
if [ "$LASTBE" != "drm-backend.so" ]; then
  rm -f "$RC_DIR/conf/last-be" 2>/dev/null
fi
WESTON_OK=0
for BE in fbdev-backend.so drm-backend.so; do
  # fbdev hoặc lên ngay hoặc hỏng; DRM cho 15s modeset/steal-master.
  if [ "$BE" = "drm-backend.so" ]; then MAXWAIT=60; else MAXWAIT=24; fi
  # --drm-device: weston liệt kê card qua udev (máy không có udevd) nên
  # phải chỉ thẳng card. --current-mode: giữ mode KMS hiện tại.
  if [ "$BE" = "drm-backend.so" ]; then
    BEARGS="--drm-device=card0 --current-mode"
  else
    BEARGS=""
  fi
  rm -f "$RUN_DIR/wayland-1" "$RUN_DIR/wayland-1.lock"
  "$WROOT/bin/weston" -c "$RC_DIR/conf/weston.ini" -B "$BE" --tty=1 \
    --shell=kiosk-shell.so --socket=wayland-1 \
    --idle-time=0 $BEARGS &
  WPID=$!
  PIDS="$PIDS $WPID"
  TR L06 "weston ($BE) đã chạy nền, pid=$WPID; chờ socket wayland-1 (tối đa $((MAXWAIT / 4))s)"
  i=0
  while [ ! -S "$RUN_DIR/wayland-1" ] && [ "$i" -lt "$MAXWAIT" ]; do
    kill -0 $WPID 2>/dev/null || break
    sleep 0.25; i=$((i+1))
  done
  if [ -S "$RUN_DIR/wayland-1" ]; then
    TR L07 "socket wayland-1 đã sẵn sàng (backend: $BE)"
    if [ "$BE" = "drm-backend.so" ]; then
      echo "$BE" > "$RC_DIR/conf/last-be" 2>/dev/null
    fi
    WESTON_OK=1
    break
  fi
  TR L07 "backend $BE không lên được, thử backend tiếp theo"
  kill $WPID 2>/dev/null
  wait $WPID 2>/dev/null
  PIDS=""
done

if [ "$WESTON_OK" != 1 ]; then
  TR L07 "LỖI: weston không tạo được socket wayland-1 (cả DRM lẫn fbdev)"
  SNAP "weston không lên"
  cleanup
  exit 1
fi
TR L07 "socket wayland-1 sẵn sàng sau $((i * 250))ms"
# Electron chạy thẳng trên Wayland (main.js đặt ozone-platform=wayland),
# không qua Xwayland nữa — lớp X11 của weston 9 lỗi assert khi app hiện cửa sổ.
TR L08 "bỏ qua Xwayland — Electron chạy Wayland native"
# weston/libinput bo qua thiet bi ao o buoc liet ke luc khoi dong (log
# chỉ thấy bàn phím thật dù thiết bị ảo có trước và thẻ đã đúng) -> phát
# lại sự kiện 'add' cho các thiết bị input để nó nhận theo đường
# hot-plug, đường mà các phiên trước đã thấy nó phản ứng.
if [ -n "$UDEVADM" ]; then
  "$UDEVADM" trigger --action=add --subsystem-match=input 2>/dev/null
  # Bo settle (ly do nhu L05G): weston da len, hot-plug xu ly tiep.
fi
TR L08B "da phat lai add input sau khi weston len (khong settle)"
SNAP "weston đã lên"

# ── Electron tiền cảnh (phiên kết thúc khi tiến trình này thoát) ────
export LD_LIBRARY_PATH="$RC_DIR/syslibs:$WLIB"
export FONTCONFIG_FILE="$RC_DIR/fonts.conf"
# GTK (Electron) giải mã ảnh PNG/.. qua gdk-pixbuf: trỏ loader đúng bản
export GDK_PIXBUF_MODULE_DIR="$RC_DIR/syslibs/pixbuf-loaders"
export GDK_PIXBUF_MODULEDIR="$RC_DIR/syslibs/pixbuf-loaders"
export GDK_PIXBUF_MODULE_FILE="$RC_DIR/syslibs/pixbuf-loaders/loaders.cache"
export XDG_DATA_DIRS="$RC_DIR/share:/usr/local/share:/usr/share"
export TMPDIR="$RUNDATA/tmp"
TR L10 "sắp chạy Electron: user-data=$RUNDATA/profile (tmpfs) URL đầu xem biến RC_WEB_URL"
SNAP "ngay trước khi chạy Electron"
"$RC_DIR/electron/electron" \
  --ozone-platform=wayland \
  --enable-features=UseOzonePlatform \
  --no-sandbox \
  --remote-debugging-port=9222 \
  --remote-debugging-address=127.0.0.1 \
  --user-data-dir="$RUNDATA/profile" \
  --disk-cache-dir="$RUNDATA/cache" \
  --disk-cache-size=33554432 \
  --media-cache-size=33554432 \
  --renderer-process-limit=1 \
  --autoplay-policy=user-gesture-required \
  --no-first-run \
  --disable-dev-shm-usage \
  --disable-gpu \
  --disable-background-networking \
  --disable-component-update \
  --disable-sync \
  --disable-default-apps \
  --disable-domain-reliability \
  --js-flags=--max-old-space-size=256 \
  "$RC_DIR/app" &
EPID=$!
PIDS="$PIDS $EPID"
# Ghi lại starttime + cmdline của electron để vòng chờ dưới phát hiện
# PID bị tái chế (hệ 1GB quay vòng PID nhanh): kill -0 đúng với tiến
# trình khác tên là treo vĩnh viễn. So khớp mỗi nhịp, lệch là thoát.
ESTART="$(cut -d' ' -f22 /proc/$EPID/stat 2>/dev/null)"
TR L10 "Electron đã chạy, pid=$EPID (starttime=$ESTART) -> chờ nó thoát"
# Chờ bằng vòng ngủ ngắn thay cho 'wait' thẳng: ash gác trap
# TERM khi đang wait nên RomCloud bấm B là launch.sh không dọn
# dẹp được, lần nào RomCloud cũng phải SIGKILL cả nhóm (giết
# cứng Electron giữa chừng, profile dễ lỗi -> web lúc lên lúc
# không). Ngủ ngắn từng nhịp thì trap có cơ hội chạy.
# Đồng thời phát hiện electron ZOMBIE (chết mà chưa reap): kill -0 vẫn
# đúng với zombie nên vòng cũ treo vĩnh viễn. Đọc state /proc, gặp Z
# (hoặc mất /proc) là thoát vòng để wait/cleanup chạy.
STATUS=0
while kill -0 $EPID 2>/dev/null; do
  ST=""
  if [ -r "/proc/$EPID/stat" ]; then
    ST="$(cut -d' ' -f3 /proc/$EPID/stat 2>/dev/null)"
  fi
  if [ "$ST" = "Z" ] || [ -z "$ST" ]; then
    TR L10 "Electron thành zombie/mất tích (pid=$EPID) -> thoát vòng chờ"
    break
  fi
  # PID tái chế: tiến trình khác chiếm số pid cũ (starttime/cmdline đổi).
  CURST="$(cut -d' ' -f22 /proc/$EPID/stat 2>/dev/null)"
  CURCMD="$(tr '\0' ' ' < /proc/$EPID/cmdline 2>/dev/null)"
  case "$CURCMD" in
    *electron*) ;;
    *) TR L10 "pid $EPID không còn là electron -> thoát vòng chờ"; break;;
  esac
  if [ -n "$ESTART" ] && [ -n "$CURST" ] && [ "$CURST" != "$ESTART" ]; then
    TR L10 "pid $EPID bị tái chế (starttime đổi) -> thoát vòng chờ"
    break
  fi
  sleep 0.5
done
wait $EPID
STATUS=$?
set +x
TR L11 "Electron THOÁT, mã thoát=$STATUS"
SNAP "ngay sau khi Electron thoát"
cleanup
exit $STATUS
