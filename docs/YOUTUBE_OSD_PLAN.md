# Kế hoạch update OSD YouTube (giống app YouTube thật)

> Ngày 2026-10-03. Trạng thái: CHƯA LÀM — duyệt rồi mới implement.
> PHẠM VI CỨNG: chỉ update UI OSD. CẤM đụng tiến trình phát video
> (resolve URL, mpv spawn, IPC control loop, quality switch) và logic
> search. Icon thiếu user bổ sung sau — code chừa sẵn chỗ cắm icon.

## 1. Mục tiêu trải nghiệm (giống app YouTube)

Bấm bất kỳ nút nào (trừ B/MENU thoát) → hiện full OSD 3 vùng trong ~3s,
không bấm thì tự ẩn để xem sạch:

- **Top bar:** `‹` + tên video (truncate) bên trái; chất lượng hiện tại
  (`720p`) bên phải.
- **Giữa:** flash icon to (128px): play/pause/tới/lùi/volume dùng icon PNG
  sẵn trong `assets/player_icons/`; các nút phụ (aspect/CC/tốc độ/chất
  lượng) hiện chữ to giữa màn hình như cũ.
- **Bottom bar:** thanh tiến trình (đỏ, có chấm vị trí) + `phút:giây hiện tại
  / tổng thời lượng` + gợi ý nút footer kiểu IPTV (`A Tạm dừng • ←/→ ±10s •
  Y Phụ đề • B Thoát`…).

## 2. Kỹ thuật

- Vẽ 3 vùng bằng mpv `overlay-add` ảnh raw BGRA (tái dùng pattern
  `showIPTVPlaybackFooter`/`showIPTVChannelOSD` — đã có sẵn, không vẽ SDL).
- Poll `time-pos`/`duration` qua IPC `mpv_youtube.sock` mỗi 1s (khi OSD
  đang hiện) để vẽ lại thanh tiến trình; OSD ẩn thì ngừng poll.
- Hiện OSD: mọi nút điều khiển gọi `showYouTubeOSD()` + reset timer 3s;
  hết timer gửi `overlay-remove`.
- Giữ nguyên mapping nút hiện tại (A, ←/→, L1/R1, UP/DOWN, X, Y, START,
  SELECT, B/MENU) — chỉ thay mặt hiển thị.
- Icon dùng: `play.png`, `pause.png`, `forward.png`, `rewind.png`,
  `vol_up.png`, `vol_down.png` (bản `.raw` 128x128 cho overlay).
  Nút phụ (aspect/CC/tốc độ/chất lượng) TẠM giữ text OSD; khi user bổ sung
  file icon mới (cùng thư mục, đặt tên `aspect.png`, `cc.png`,
  `speed.png`, `quality.png`) thì chỉ đổi tên file trong 1 map
  icon-name → không sửa logic.
- KHÔNG ĐỔI: câu lệnh resolve stream, tham số spawn mpv, vòng control loop,
  `switchYouTubeQuality`, search/results, mapping nút → hành động.

## 3. Các bước làm

1. Thêm `showYouTubeOSD(title, quality)` + `hideYouTubeOSD()` trong
   `IPTVManager` (copy pattern footer IPTV, đổi layout 3 vùng).
2. Thêm poll time-pos/duration trong loop `playYouTubeVideo` (1s/lần khi
   OSD hiện).
3. Móc mọi nhánh nút (A/LEFT/RIGHT/L1/R1/UP/DOWN/X/Y/START/SELECT) gọi
   show + reset timer; xóa các `show-text` rời rạc cũ.
4. Timer ẩn OSD sau 3s không bấm (dùng `m_overlayExpireTime` có sẵn).

## 4. Test (máy thật qua adb)

- Mở 1 video 720p: bấm A → OSD đủ 3 vùng, icon giữa đúng trạng thái.
- Thanh tiến trình chạy đúng giờ, hết video tự tắt.
- Tua ±10s/±60s, đổi chất lượng, phụ đề: OSD cập nhật, không kẹt overlay.
- Để yên 3s → OSD ẩn sạch, video vẫn chạy 60fps.
- Thoát giữa chừng (B) không sót overlay/pid.

## 5. Release

Gộp vào bản minor tiếp theo. Không đụng luồng resolve/stream.
