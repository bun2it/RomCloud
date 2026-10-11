# Weston bring-up cho RC-Web (TrimUI Brick Pro, stock OS)

> Trạng thái 2026-10-09: Weston headless + desktop-shell CHẠY ĐƯỢC
> (socket wayland-9 tạo thành công), Xwayland tạo được socket X0 nhưng
> client chưa connect được. Electron ozone-headless KHÔNG có trong build
> (strings chỉ thấy wayland/x11) → trap im lặng. Ozone-wayland đòi xdg_shell.
> Chưa xong: X11 client connect (nghi Xwayland chết sớm hoặc auth).

## Bài học xương máu (đọc trước khi test tay qua adb)

1. **Mọi daemon chết theo session adb.** `nohup`/`setsid`/`&` đều không cứu
   được — adbd dọn process khi shell thoát. Mọi test phải gói TRỌN trong
   1 lệnh duy nhất (dựng stack + test + in kết quả rồi mới thoát).
   Production không bị: RomCloud (sống dưới MainUI) fork + supervise
   con trực tiếp qua waitpid (mẫu MpvPlayer/ChromeBridge).
2. **Herodoc qua adb: LUÔN bọc single-quote ngoài cùng**, nếu không `$VAR`
   bị zsh expand NGAY TRÊN MAC (đã từng nhét cả `$PATH` của Mac vào script
   trên máy — lỗi `bad variable name`).
3. **KHÔNG tag lại input hệ thống** (`/etc/udev/rules.d`): đã từng làm liệt
   D-pad sau reboot (nghi `trimui_inputd` rối). File đó đã xóa vĩnh viễn.
   Weston headless + `[core] require-input=false` thì không cần input.
4. **Thiết bị reboot liên tục** (bệnh nguồn/cáp + panic kernel `sunxi_udc`
   khi bật USB mode). Đừng bật USB mode. Mọi thứ trong /tmp bay sau reboot.

## Recipe tối thiểu đã kiểm chứng (từng bước đều ra log đúng)

```sh
# 0. Mount squashfs Weston (PortMaster, dep ngoài — KHÔNG commit)
/mnt/SDCARD/Apps/RomCloud/rc-web/weston.squashfs -> /tmp/weston-check
mkdir -p /tmp/weston-check /tmp/rc-wrun /tmp/.X11-unix /tmp/share
chmod 700 /tmp/rc-wrun
mount -t squashfs .../weston.squashfs /tmp/weston-check
ln -sfn /tmp/weston-check/share /tmp/share

# 1. weston.ini của ta (KHÔNG dùng file của họ)
printf '[core]\nrequire-input=false\n' > /tmp/rc-weston.ini

# 2. Env (PATH chứa bin/ để tìm Xwayland!)
export PATH=/tmp/weston-check/bin:/usr/bin:/bin:/usr/sbin:/sbin
export LD_LIBRARY_PATH=/mnt/SDCARD/System/lib:/tmp/weston-check/lib_aarch64
export XDG_RUNTIME_DIR=/tmp/rc-wrun
export LIBSEAT_BACKEND=builtin        # khỏi seatd (hay chết)
export XKB_CONFIG_ROOT=/tmp/share/xkb
export WESTON_MODULE_MAP='headless-backend.so=.../libweston-14/headless-backend.so;desktop-shell.so=.../weston/desktop-shell.so;xwayland.so=.../libweston-14/xwayland.so;'
export WESTON_DATA_DIR=/tmp/weston-check/data
export WESTON_LIBEXEC_DIR=/tmp/weston-check/libexec
export WESTON_HEADLESS_WIDTH=1024 WESTON_HEADLESS_HEIGHT=768
export FONTCONFIG_FILE=/mnt/SDCARD/Apps/RomCloud/rc-web/fonts.conf

# 3. Chạy (headless backend = KHÔNG cướp màn hình; pixman = không cần GPU)
./wp_weston -c /tmp/rc-weston.ini -B headless --use-pixman \
    --shell=desktop-shell.so -i0 -Swayland-9 [--xwayland]
# Socket OK: /tmp/rc-wrun/wayland-9 ; X cần thêm /tmp/.X11-unix/X0
```

## Electron (đã kiểm chứng)

* Loader: cần closure 76 lib (Debian bullseye + kv runtime/lib).
  Xem `fetchsyslibs.py` (ngoài repo, đã chạy xong).
  (Ghi chú 2026-10: `compat_shim.c` đã bỏ — Electron chạy ổn không cần
  LD_PRELOAD trên glibc hiện tại.)
* `--version` chạy OK (= loader/libs/CPU ổn). V8 OK (`ELECTRON_RUN_AS_NODE=1`).
* Flags chắc chắn: `--no-sandbox` PHẢI nằm trên argv (JS appendSwitch quá muộn).
* Ozone: headless absent (trap im lặng), wayland cần xdg_shell,
  x11 cần Xwayland sống (đang dở: X0 tạo được nhưng client chưa connect được).

## Việc tiếp theo (khi máy ổn định nguồn)

1. Xwayland: vì sao X0 tạo mà không accept (Xwayland chết sớm? auth?
   --xwayland cần seatd thật thay vì builtin?).
2. Rồi mới tới: paint SHM, input HTTP, RAM đo đạc, supervision trong app.
