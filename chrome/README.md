# RC-Web — trình duyệt Chromium toàn màn hình của RomCloud

RC-Web là gói trình duyệt do **RomCloud tự đóng gói**: Electron chạy toàn màn hình trên Weston (DRM), tay cầm điều khiển qua gptokeyb2. RomCloud fork `launch.sh` của gói này, giám sát qua waitpid và đưa URL mới vào bằng CDP `:9222`. Thoát phiên (SELECT) là tiến trình con tự dọn weston/electron/gptokeyb và trả màn hình về.

Gói này KHÔNG phụ thuộc app của bên thứ ba nào trên máy người dùng, và không chứa file lấy từ gói đóng sẵn của bất kỳ app nào khác (xem `licenses/NOTICE.txt` sau khi ráp).

## Thành phần (tất cả lấy từ nơi phát hành gốc)

| Thành phần | Nguồn gốc |
|---|---|
| Electron 22.3.27 arm64 | electron/electron — GitHub releases chính thức |
| Thư viện hệ thống cho Electron (GTK/NSS/cups/pango...) | Debian bookworm arm64 (gói `.deb` gốc) |
| Weston 10 + Xwayland + seatd + thư viện đồ hoạ/input | Debian bookworm arm64 (gói `.deb` gốc) |
| gptokeyb2 + libinterpose (tay cầm → phím/chuột) | PortMaster-GUI — GitHub releases chính thức |
| gamecontrollerdb.txt | SDL_GameControllerDB — repo upstream |
| Font DejaVu | Debian (fonts-dejavu-core) |
| `launch.sh`, `browser.gptk`, `fonts.conf`, `app/` (Electron main + bàn phím ảo) | Code của RomCloud trong thư mục `chrome/` này |
| `ChromeBridge` (phía C++ trong app) | `src/browser/chrome/` |

## Ráp gói (trên Mac, một lần)

```sh
cd /Users/tai/RomCloud
./chrome/fetch-rc-web.sh            # ráp vào ~/Downloads/rc-web
```

Script tải đúng các nguồn ở bảng trên, gom thêm code của ta, và in ra bố cục cuối. Binary không commit vào git (chỉ commit phần code trong `chrome/` và `src/browser/chrome/`).

Đẩy lên máy:

```sh
adb push ~/Downloads/rc-web /mnt/SDCARD/Apps/RomCloud/rc-web
adb shell "chmod +x /mnt/SDCARD/Apps/RomCloud/rc-web/launch.sh /mnt/SDCARD/Apps/RomCloud/rc-web/electron/electron /mnt/SDCARD/Apps/RomCloud/rc-web/electron/chrome_crashpad_handler /mnt/SDCARD/Apps/RomCloud/rc-web/bin/gptokeyb2 /mnt/SDCARD/Apps/RomCloud/rc-web/weston/bin/*"
```

(`adb push` không giữ bit thực thi nên cần dòng chmod.)

## Cách chạy trên máy

- Trong app: menu WEB đang ở chế độ FULL → mở URL → RomCloud fork `rc-web/launch.sh`, hiện màn "Đang mở trình duyệt..." trong lúc weston/electron khởi động.
- Weston chiếm toàn màn hình; Electron mở trang đã chọn (UA mobile).
- **Tay cầm trong phiên:** D-pad di chuột, R1 bấm chuột, A = Enter, B = quay lại, X = trang chủ, Y = bàn phím ảo, L1 = phóng to (xoay vòng), L2/R2 cuộn trang.
- **START** (RomCloud giữ): mở ô sửa địa chỉ của app → nhập xong là deep-link vào phiên đang chạy.
- **SELECT** (RomCloud giữ): thoát phiên — weston/electron/gptokeyb được dọn sạch, RAM thu hồi toàn bộ, về lại menu của app.
- Gõ chữ trong trang (portal cafe, ô tìm kiếm...): bấm Y mở bàn phím ảo của RC-Web.
- Video/TikTok: xem bằng mpv của app, không test trong browser.

## Bố cục gói `rc-web/` trên máy

```
rc-web/
  launch.sh            # launcher của RomCloud (fork từ ChromeBridge)
  browser.gptk         # mapping tay cầm của RomCloud
  fonts.conf           # fontconfig: fonts/ đóng kèm + font hệ thống
  gamecontrollerdb.txt # cơ sở dữ liệu tay cầm cho gptokeyb2
  electron/            # Electron upstream (fetch tải, không commit)
  syslibs/             # .so hệ thống cho Electron (Debian)
  weston/              # cây Weston/Xwayland/seatd (Debian): bin/lib/share
  bin/                 # gptokeyb2 + libinterpose (PortMaster)
  fonts/               # DejaVu (Debian)
  app/                 # Electron main của RomCloud: main.js, preload.js,
                       # keyboard.html, package.json
  conf/                # tự sinh lúc chạy: profile/cache/tmp + weston.ini
  licenses/            # NOTICE + license của các thành phần
  rc-web.log           # log phiên gần nhất (ghi mới mỗi lần mở)
```

## Chạy thử trên máy tính trước (tùy chọn)

Electron trong gói là bản arm64 cho máy TrimUI, không chạy trên Mac. Việc kiểm tra cú pháp JS/shell làm ở mức `node --check` / `sh -n`; kiểm tra thật là trên máy (mục dưới).

## Đọc trace khi debug (gửi log)

Mỗi phiên duyệt đều để lại vết từng bước, mã hoá 3 lớp:

- **B** = bridge phía RomCloud (trong `debug.log` của app)
- **L** = launcher trên máy (trong `rc-web.log` cạnh gói)
- **J** = electron main (cũng trong `rc-web.log`)

Dòng trace có dạng `[WEBTRACE] B05 (+12345ms) ...`. Thứ tự một phiên khoẻ mạnh: `B00` (yêu cầu mở) → `B01` (kiểm gói) → `B03` (fork) → `B04/B05` (chờ CDP) và song song phía máy: `L00`→`L04` (seatd) → `L06/L07` (weston) → `L08` (Xwayland) → `L09` (tay cầm) → `L10` (electron chạy) → `J00`→`J02` (cửa sổ) → `J12/J13` (đang tải/trang đã lên). Thoát bình thường: `L11` + `B08` (thoát sạch) — nếu thấy `B08` kèm "tín hiệu 9" là máy hết RAM giết; `B09/B10` là RomCloud chủ động dừng mà con không chịu thoát.

Trong `rc-web.log` còn có các khối `SNAP` chụp RAM + tiến trình nặng nhất + cuối `dmesg` tại các mốc (vừa vào launcher, sau khi weston lên, trước/sau khi electron chạy) — kernel ghi rõ ở đó nếu nó giết tiến trình vì hết RAM.

Khi lỗi: gửi cả 2 file `debug.log` (của RomCloud) và `rc-web.log` (trong `Apps/RomCloud/rc-web`), kèm một câu mô tả đã bấm gì. Không cần chụp màn hình.

## Cần Tai đo khi test trên Brick

1. Thời gian từ lúc bấm mở web tới khi thấy trang (kỳ vọng dưới 30 giây).
2. Trang có ở lại không — nếu tự đóng thì gửi `debug.log` của RomCloud + `rc-web.log` trong `Apps/RomCloud/rc-web` (log phía ta nay ghi rõ phiên chết vì mã lỗi hay vì tín hiệu 9 = hết RAM).
3. Đọc một trang báo nặng: máy có giật/hết RAM không.
4. SELECT thoát: về menu sạch, không sót tiến trình (mở lại app bình thường).
5. START sửa địa chỉ trong phiên: trang mới có mở đúng không.
6. Bàn phím ảo (Y): gõ được vào ô đăng nhập của một trang portal.
