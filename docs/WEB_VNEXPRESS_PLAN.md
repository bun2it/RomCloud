# Plan: Web Browser đạt chuẩn vnexpress.net

Mục tiêu: load trang chủ vnexpress.net đầy đủ, đọc được, scroll mượt trên Brick.
Chuẩn pass mỗi phase: code + `tests/test_browser` pass + `build_pc.sh` pass.
Trạng thái: **P0–P6 ALL PASS (54 checks), build PC pass.**

## P0 — Harness + fixture + baseline ✅ PASS
- `tests/fixtures/vnexpress.html`: snapshot trang chủ (~264KB, 0 table, 28 img).
- `tests/test_browser.cpp`: parse headless (SDL_ttf + font thật, renderer null),
  assert không crash + đo thời gian parse/layout.
- Baseline hiện tại để so sánh các phase sau.

## P1 — Perf core (mượt + nhanh) ✅ PASS
- Texture cache cho text (không tạo/hủy texture mỗi frame).
- Tách `layout_y` khỏi `scrollY` (scroll không relayout toàn cây).
- Viewport culling + dirty flag (chỉ render khi scrollY/DOM đổi).
- `submitForm` chuyển sang async như `loadUrl`.
- Test: assert scroll đổi không gọi `layout()` toàn cây quá 1 lần / frame render
  headless < 16ms trên fixture; assert form submit không block main thread.

## P2 — Đọc được vnexpress (text) ✅ PASS
- Word-wrap theo pixel (font thật), `h1–h6` cỡ lớn, `b/strong` + `i/em`,
  `ul/ol/li` bullet, entities số (`&#123;`/`&#x..;`) + named phổ biến,
  strip BOM + bỏ `meta` an toàn.
- Test: assert các `.title-news` trong fixture thành text đọc được (không tràn
  quá `CONTENT_W`, heading cao hơn body).

## P3 — Form đúng (portal) ✅ PASS
- Fix bug checkbox không toggle bằng A; `select` render options + chọn bằng A;
  tôn trọng `maxlength`; submit async.
- Test: assert toggle checkbox đổi state, select đổi value, submit không block.

## P4 — Network (portal khó + trang thật) ✅ PASS
- Cookie jar (curl `COOKIEFILE`), `Accept-Encoding: gzip`, content-type guard
  (không parse ảnh/binary thành HTML), giữ SSL verify OFF (portal tự ký).
- Test: assert cookie set/clear, body ảnh bị từ chối parse.

## P5 — Ảnh (vnexpress cần) ✅ PASS
- Parse `<img src>` (relative resolve), tải async SDL_image, downscale theo
  `CONTENT_W`, cache LRU, placeholder khi chờ, cap 2MB/file.
- Test: assert fixture có đúng 28 img element, placeholder trước — ảnh thật sau.

## P6 — Nghiệm thu vnexpress ✅ PASS

## P8b — TLD pills + P9 CSS engine ✅ PASS
- P8b: pill `.com/.net/.vn` trên đầu keyboard URL (UP từ hàng phím vào pill,
  A thêm hậu tố), L1/R1/SELECT về lại Hoa/ABC-123/Telex.
- P9: CSS subset (tag/.class/#id/descendant, specificity, inline-style,
  @media inline, màu, font-size, bold, center, display:none, margins, bg).
  Async fetch `<link rel=stylesheet>`, font-size scale ×1.6 min 18px cho
  màn Brick, texture cache theo cỡ chữ. Fixture: 678 rules.
- Dpad lên/xuống/trái/phải chọn event (link/input/button/select/checkbox),
  A/B action — UP ở widget đầu lên URL bar (viền cyan), A mở keyboard,
  DOWN về trang, START tải lại, L1/R1 lật trang.
- Keyboard URL (browser only): telex OFF mặc định, L1=`.com` R1=`.net`
  SELECT=`.vn` (qua `VirtualKeyboard::typeText`, tôn trọng maxLen).
- Repeat toàn app: `InputManager::isButtonRepeat` (400ms delay, 70ms rate)
  cho A/X/Y ở mọi keyboard (Explorer, Search, IPTV, YouTube, Browser,
  + 5 modal qua SearchInputModal). B/START/L1/R1/SELECT giữ edge.
- Fixture: parse < 1s, đủ headlines (đếm `title-news` > 0), đủ img, không crash.
- Live (nếu mạng): fetch thật + render 1 frame headless pass.
- `build_pc.sh` + full test suite pass.
