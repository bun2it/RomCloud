# FULL ENGINE — RC-Web (RomCloud tự đóng gói)

Cập nhật 2026-10-10: **RomCloud tự đóng gói trình duyệt Chromium, không còn phụ thuộc app trình duyệt của bên thứ ba trên máy người dùng.** Tài liệu này thay cho bản cũ (hướng giám sát app ngoài đã bỏ).

---

## Kiến trúc

```
RomCloud (FULL mode)
  └─ ChromeBridge::openUrl(url)
       ├─ ghi URL vào /tmp/rc-web-start-url + biến RC_WEB_URL
       ├─ fork: /bin/sh -c 'cd <appRoot>/rc-web && exec ./launch.sh'
       │    (session riêng, env đã lọc bỏ LD_* của RomCloud)
       ├─ RomCloud vẫn sống: state CHROME, poll tiến trình con
       ├─ weston (DRM, pixman) chiếm toàn màn hình, Xwayland :0
       ├─ Electron kiosk mở trang, điều khiển thêm qua CDP :9222
       └─ thoát (SELECT): SIGTERM script -> trap cleanup dọn
            weston/electron/gptokeyb -> CONT MainUI -> về menu
```

RomCloud chỉ giữ 2 nút trong phiên: **START** = sửa địa chỉ (editor của app -> CDP deep-link), **SELECT** = thoát phiên. Mọi nút trong trang do gptokeyb2 lo theo `browser.gptk` của RomCloud.

## Thành phần của gói rc-web

Chỉ gồm thành phần mã nguồn mở lấy thẳng từ nơi phát hành gốc, cộng code của chính RomCloud:

| Thành phần | Nguồn gốc |
|---|---|
| Electron 22.3.27 arm64 | electron/electron — GitHub releases chính thức |
| Thư viện hệ thống cho Electron (GTK/NSS/cups/pango...) | Debian bookworm arm64 (`.deb` gốc) |
| Weston 10 + Xwayland + seatd + lib đồ hoạ/input | Debian bookworm arm64 (`.deb` gốc) |
| gptokeyb2 + libinterpose (tay cầm → phím/chuột) | PortMaster-GUI — GitHub releases chính thức |
| gamecontrollerdb.txt | SDL_GameControllerDB upstream |
| Font DejaVu | Debian (fonts-dejavu-core) |
| `launch.sh`, `browser.gptk`, `fonts.conf`, `app/` | Code của RomCloud (`chrome/` trong repo) |
| `ChromeBridge` phía C++ | `src/browser/chrome/` |

Không có file nào lấy từ gói đóng sẵn của app khác. Ráp gói bằng `chrome/fetch-rc-web.sh` trên Mac (tải về `~/Downloads/rc-web`), binary không commit vào git.

## Chuẩn bị + cài lên máy

```sh
cd /Users/tai/RomCloud
./build.sh
./chrome/fetch-rc-web.sh
adb push ~/Downloads/rc-web /mnt/SDCARD/Apps/RomCloud/rc-web
adb shell "chmod +x /mnt/SDCARD/Apps/RomCloud/rc-web/launch.sh /mnt/SDCARD/Apps/RomCloud/rc-web/electron/electron /mnt/SDCARD/Apps/RomCloud/rc-web/electron/chrome_crashpad_handler /mnt/SDCARD/Apps/RomCloud/rc-web/bin/gptokeyb2 /mnt/SDCARD/Apps/RomCloud/rc-web/weston/bin/*"
adb push bin/RomCloud /mnt/SDCARD/Apps/RomCloud/bin/RomCloud
```

Thiếu gói `rc-web` trên máy thì app báo "Chưa cài RC-Web" và ở lại trình duyệt LITE — không crash.

## Tay cầm trong phiên (mapping của RomCloud)

D-pad di chuột, R1 bấm chuột, A = Enter, B = quay lại, X = trang chủ, Y = bàn phím ảo (gõ chữ trong trang, ví dụ portal cafe), L1 = phóng to xoay vòng (100→200%), L2/R2 cuộn trang. START sửa địa chỉ, SELECT thoát (RomCloud giữ).

## Bố cục `rc-web/` trên máy

```
rc-web/
  launch.sh            # launcher của RomCloud (ChromeBridge fork file này)
  browser.gptk         # mapping tay cầm của RomCloud
  fonts.conf           # fonts/ đóng kèm + font hệ thống
  gamecontrollerdb.txt # dữ liệu tay cầm cho gptokeyb2
  electron/            # Electron upstream
  syslibs/             # .so hệ thống cho Electron (Debian)
  weston/              # cây Weston/Xwayland/seatd (Debian): bin/lib/share
  bin/                 # gptokeyb2 + libinterpose (PortMaster)
  fonts/               # DejaVu
  app/                 # Electron main của RomCloud (main.js, preload.js, keyboard.html, package.json)
  conf/                # sinh lúc chạy: profile, cache, tmp, weston.ini
  licenses/            # NOTICE + license các thành phần
  version.txt          # version gói (YYYYMMDD) — OTA đối chiếu với rc_web_version
  rc-web.log           # log của phiên gần nhất
```

## OTA riêng cho RC-Web (chuẩn bị sẵn, chưa phát hành)

- Gói: `./package-rc-web.sh` → `dist/rc-web-<rc_web_version>.zip` (~200M,
  entries ở gốc). Upload kèm release GitHub.
- Manifest (`version.json`): `rc_web_version` + `rc_web_url`. Khi đổi nội
  dung rc-web thì bump `rc_web_version`, đóng gói + upload asset mới.
- Máy: màn Cài đặt → Cập nhật hiện dòng "Trình duyệt web: v<cũ> -> v<mới>",
  nhấn **X** để tải + cài (dừng phiên web trước, swap bằng rename, giữ
  `conf/profile/rc-history.json`, rollback bản cũ nếu bản mới lỗi).
  Không tự vá ở boot (gói nặng, chỉ cài khi user đồng ý).

## Khi có lỗi — đọc log theo thứ tự (WEBTRACE)

Cả 3 lớp đều ghi vết từng bước dạng `[WEBTRACE] <mã> (+ms) ...`:

- **B** (bridge C++, trong `debug.log`): B00 yêu cầu mở → B01 kiểm gói → B02/B03 fork → B04 nhịp chờ CDP 5s/lần → B05 sẵn sàng → B07 deep-link → B08 con thoát (kèm **mã thoát hoặc tín hiệu**: 9 = bị giết, thường là hết RAM) → B09/B10 dừng chủ động/leo thang SIGKILL.
- **L** (launcher, trong `Apps/RomCloud/rc-web/rc-web.log`): L00 khởi động → L01 dọn phiên cũ → L02 MainUI → L04 seatd → L05/L06/L07 weston + socket → L08 Xwayland → L09 gptokeyb → L10 electron chạy → L11 electron thoát (kèm mã) → L12 cleanup. Có thêm khối `SNAP` (RAM trống, tiến trình nặng nhất, đuôi `dmesg` — nơi kernel ghi khi giết tiến trình vì hết RAM) tại các mốc quan trọng.
- **J** (electron main, trong `rc-web.log`): J00 khởi động (kèm version) → J01/J02 cửa sổ chính + URL mở đầu → J10 cửa sổ được cấu hình → J12 bắt đầu tải → J13 trang lên (dom-ready) → J14 lỗi tải trang (mã lỗi) → J15 renderer chết (tự tải lại) → J20–J25 bàn phím ảo/phím chức năng → J30–J32 đóng hẳn.

Khi lỗi: gửi cả `debug.log` + `rc-web.log` và nói đã bấm gì — vết trace chỉ rõ phiên chết ở lớp nào, bước nào, sau bao lâu.

## Cần đo trên máy thật (chưa kiểm chứng)

1. Thời gian vào web (mục tiêu < 30s), trang có ở lại không (OOM?).
2. Đọc báo nặng: giật không, hết RAM không.
3. SELECT thoát sạch, không sót tiến trình, mở lại app bình thường.
4. START sửa địa chỉ -> deep-link mở đúng trang mới.
5. Bàn phím ảo (Y) gõ được ở trang đăng nhập/portal cafe.
6. Session sau thoát: mở lại web lần 2 có sạch như lần 1 không.

## Ghi chú kỹ thuật

- Tương thích glibc: Debian bookworm yêu cầu glibc >= 2.36; bộ `.so`/weston lấy từ đó là điểm cần xác nhận đầu tiên trên máy thật — nếu loader báo thiếu `GLIBC_2.xx` thì hạ nguồn Debian trong `fetch-rc-web.sh` (đổi `DEB_SUITE`/`DEB_MIRROR`) hoặc lấy bản cũ hơn.
- Weston chạy pixman (phần mềm) để né phụ thuộc driver GL của máy; Electron cũng tắt GPU. Video không test trong browser (xem bằng mpv của app).
- Phiên duyệt chạy trong session riêng (`setsid`), env lọc bỏ `LD_*` của RomCloud để Electron/Weston không nạp nhầm `.so` của app.
- Trong lúc weston đang boot mà trang có thể đã hiện, chỉ SELECT mới thoát được phiên — nút B bị vô hiệu ở màn chờ để tránh giết nhầm phiên đang chạy tốt.
