# Plan: phần còn thiếu của Web Browser (sau P10)

Ngày: 2026-10-07. Chuẩn đo: vnexpress.net trang chủ + 1 bài viết trên Brick.
Mỗi phase: code + gate `tests/test_browser` + `build_pc.sh` + deploy device.

## A. Đọc bài viết (P11) — ưu tiên cao nhất
Trang chủ đã đọc được; bài viết là use-case chính còn lại.
- [ ] P11.1 Article extractor: khi URL là trang bài (có `<article>` hoặc
  `h1.title-detail + p.description + p.Normal`), render gọn: tiêu đề +
  sapo + thân bài, ẩn nav/footer/comments/related (giảm ~70% scroll rác).
- [ ] P11.2 Author/date line: hiện tên tác giả + giờ đăng (lấy từ
  `meta[property=article:published_time]` / `.author_mail`).
- [ ] Gate: fixture bài viết → body text > 2000 ký tự, comments/ads = 0 node.

## B. Ảnh đầy đủ (P12)
- [ ] P12.1 Kiểm tra log device `IMG_Init png=/jpg=` (đã log từ P10): nếu
  thiếu jpg trên bản Brick → rebuild sysroot SDL2_image kèm libjpeg.
- [ ] P12.2 WebP: nếu thiếu → bỏ qua URL `.webp` như svg (đỡ tốn worker);
  nếu có → giữ.
- [ ] P12.3 GIF: chỉ lấy frame đầu (có sẵn qua SDL_image nếu build gồm gif).
- [ ] P12.4 `srcset`/`data-original`: vnexpress dùng `data-src` (đã xong);
  thêm `srcset` (lấy URL đầu) + `data-original`.
- [ ] Gate: đếm ảnh thật render trên fixture > 20.

## C. Table + iframe (P13)
Trang chủ không có table nhưng bài viết/báo giá có.
- [ ] P13.1 `<table>/<tr>/<td>/<th>`: render lưới đơn (text wrap theo cột,
  viền xám). Chưa cần colspan/rowspan (hiếm) — log khi gặp để đo.
- [ ] P13.2 `<iframe>` (video nhúng): vẽ placeholder có viền + tiêu đề
  src (thay vì nuốt im lặng như hiện tại).
- [ ] P13.3 `<video poster>`: dùng poster như ảnh.
- [ ] Gate: fixture table 2x2 render đủ 4 ô; iframe chiếm đúng 1 box.

## D. CSS tiếp (P14, khi cần)
- [ ] P14.1 `font:` shorthand (hiện chỉ đọc `font-size` riêng) — vnexpress
  dùng nhiều (`font:400 14px arial`).
- [ ] P14.2 `padding` (hiện chỉ margin) + `border` cơ bản (viền xám ô).
- [ ] P14.3 `line-height` số/px (hiện suy từ font-size).
- [ ] P14.4 `float:left/right` → coi như block (hiện tại) + log tần suất;
  chỉ làm thật nếu bài viết vỡ layout.
- [ ] Không làm: flexbox/grid/position/animation (Brick không kham, đã chốt).

## E. Trải nghiệm (P15)
- [ ] P15.1 Reader indicator: khi phát hiện trang bài, footer thêm hint
  "Bài viết" + tự focus vào tiêu đề.
- [ ] P15.2 Tìm trong trang: Y gõ từ khóa → nhảy giữa các match (dùng VkState
  có sẵn + highlight).
- [ ] P15.3 Zoom chữ: L2/R2 (nếu map) hoặc SELECT+UP/DOWN đổi cssPx scale,
  lưu theo session.
- [ ] P15.4 Lịch sử duyệt back/forward thật (hiện chỉ back).

## F. Không làm (đã quyết)
JS engine, full NetSurf/litehtml, GIF động, SVG, video phát trong trang.

## Thứ tự đề xuất
P11 → P12 → P13 → P15.1 → P14/P15 còn lại khi rảnh.
