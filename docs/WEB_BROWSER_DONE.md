# Web Browser — Nhật ký đã làm (audit)

Ngày cập nhật: 2026-10-07. Tổng: **25 files, +3654/−318 dòng**, test **120 PASS**.

## P0 — Harness + fixture
- `tests/test_browser.cpp` (mới): chạy headless (SDL_ttf + font thật,
  renderer null/dummy), assert từng phase.
- `tests/fixtures/vnexpress.html` (264KB), `article.html` (348KB),
  `anim2.gif` (129 bytes, 2 frames do PIL tạo).
- Chuẩn pass mỗi phase: test pass + `build_pc.sh` pass.

## P1 — Perf core
- Texture cache chữ (`m_textCache`, cap 256), render lần 2 còn 0ms.
- Layout tách scroll (`el.y` layout-space, trừ `scrollY` lúc vẽ) —
  scroll không relayout.
- `submitForm` chuyển async (trước block main thread tới 15s).
- Sửa race C1/C3, history C4, `loadUrlNoHistory`.

## P1b — Parser vnexpress (phát sinh do test fail)
- `</tag>` từng parse thành rỗng → stack không bao giờ pop (trang trắng).
- Void tags không push stack; auto-close `a/p/li/h…`; match close đúng tên;
  bỏ `<svg>`; LINK recurse children (tiêu đề trong `<a><h3>`).

## P2 — Đọc được
- Wrap theo pixel + hard-break URL, `h1–h6` cỡ/đậm, `b/strong`,
  bullets `ul/li`, entities số (`&#273;`) + named (`&hellip;…`), strip BOM.

## P3 — Form
- Checkbox toggle bằng A (trước là no-op), `<select>` + preselect,
  `maxlength`, `hidden`, `textarea`, map `submit/button/reset/image`.

## P4 — Network
- Cookie jar file, gzip (`Accept-Encoding`), content-type guard
  (không parse ảnh/binary thành HTML), redirect → tracking effective URL.

## P5 — Ảnh
- Async SDL_image (4 luồng, LRU 24, cap 2MB/file, downscale theo trang),
  đọc `data-src` lazy-load, `intrinsicsize` đặt chỗ trước, placeholder.

## P5b/P10 — Chống flicker + theme
- Bỏ dirty-gate (UIManager tô đen mỗi frame gây flicker), chữ/texture theo
  theme trang (trắng/chữ đen như web thật), tracking-pixel 1x1 không chiếm
  chỗ, negative cache (không fetch lại ảnh lỗi — từng thấy `img_blank.gif`
  loop chiếm hết worker), log pipeline ảnh.

## P6 — Nghiệm thu: fixture 41ms + live fetch 349KB → 44 headlines.

## P7→P8 — Điều hướng kiểu YouTube + keyboard
- Dpad chọn event, UP ở widget đầu lên URL bar (viền cyan), A mở keyboard,
  TLD pills `.com/.net/.vn` (UP từ hàng phím), telex OFF khi gõ URL.
- `InputManager::isButtonRepeat` (400ms/70ms) cho A/X/Y toàn app.

## P8c — Settings trình duyệt (SELECT): Ảnh, CSS, Cỡ chữ 100/120/150,
## GIF, Video, SVG, JS, Engine, Chế độ bài viết — lưu `browser.cfg`.

## P9/P14 — CSS subset
- Tag/.class/#id/descendant, specificity, inline-style, `@media` inline,
  màu, font-size (+scale ×1.2 min 15px cho màn Brick), bold, center,
  display:none, margin/padding, line-height, `font:` shorthand.
- Fetch `<link rel=stylesheet>` async (2 luồng, cap 512KB/file, 8 sheets).
- Fixture vnexpress: 678 rules (inline).

## P11 — Reader mode: giữ chuỗi article, ẩn chrome/comments (giữ portal
## nguyên), header hiện BÀI VIẾT.

## P12 — Ảnh: `srcset`/`data-original`, xin decoder webp, log png/jpg/webp.

## P13 — Table grid + hộp video (A phát mp4 qua mpv, YouTube báo mở app).

## P15 — Lịch sử tới (X, sửa off-by-one của Back) + tìm trong trang
## (START, highlight, B xóa) + báo reader.

## GIF động (trong P8c): vendor nsgif (gif.c/lzw.c, stdlib-only), decode
## full frames + delays (dừng ở frame lặp), animate theo wall-clock,
## cap 10 frames/8 URLs. Build C riêng trong build.sh/build_pc.sh.

## Đợt block-nav + viewport audit (theo yêu cầu user)- Viewport đã đúng từ trước (Gemini): layout theo `effectiveViewportWidth()`,
  reader cột 760 giữa màn hình như app đọc mobile; media query theo width thực.
- Điều hướng theo khối: viewport = 1 card, mỗi top-level DOM là 1 block.
  TRÁI/PHẢI đi giữa các event trong cùng block (wrap), LÊN/XUỐNG nhảy tới
  block có event (bỏ block trống). UP ở event đầu vẫn lên URL bar.
- Link rỗng (chữ nằm ở con) neo focus ở ĐỈNH nội dung thay vì khoảng trống.
- Text/ảnh không bao giờ focusable/highlight (chỉ link/input/button/
  select/checkbox/media) — có gate test đi 12 stops.

## #6 colspan/rowspan + #7 background-image (theo yêu cầu user)
- Table: occupancy grid, span clamp 1..10, cột đều, hàng co theo nội dung,
  ô rowspan thiếu hụt dồn vào hàng cuối, đo 2 pass (truncate focus trong
  pass đo để không trùng focus).
- `background-image: url(...)` (+ trong `background:` shorthand): resolve
  ở queueImages, tải qua pipeline ảnh chung, vẽ stretch (decorative),
  fallback màu nền khi thiếu. Bỏ qua gradient/data:.
- Test: ô colspan rộng gấp ~2, rowspan cao hơn, bg url parse + queued.

## JS engine v1 — CHẠY RỒI (Duktape 2.7.0 vendor, `src/browser/duktape/`)
- Chạy script inline + `src` (async, cap 4 file) sau parse, lỗi isolate
  từng script (throw không vỡ trang).
- Bindings: console, setTimeout/setInterval/clear, location.href get/set,
  document.getElementById/querySelector (#id/.class/tag), input value,
  textContent/innerHTML, style.display, click(), form.submit(),
  XHR async (worker + callback, cap 512KB).
- An toàn: heap riêng mỗi trang (epoch check, proxy hết hạn thành null),
  timer/XHR cap, không chạy khi setting js tắt.
- Test: gán value, ẩn div, null-proxy không crash, throw isolate,
  setTimeout fire sau poll. Giới hạn thật: site React/Vue (YouTube,
  TikTok) cần V8 — Brick không kham, giữ nguyên khuyến nghị dùng app.

## Chưa làm (ghi trong Settings là "Sắp có")
JS engine (xong v1, còn mở rộng bindings), NetSurf/litehtml nguyên con,
phát video YouTube nhúng trong trang.

## SVG — XONG (NanoSVG, không phải tự viết)
- Chọn NanoSVG thay vì LunaSVG/PlutoSVG (1 header, Zlib, không phụ thuộc)
  và thay luôn bản raster tay (xóa draft): parse chuẩn hơn (cung tròn,
  transform đầy đủ), đỡ công maintain.
- `src/browser/SvgRaster.{h,cpp}` bọc NanoSVG: parse → raster RGBA theo
  target width giữ tỉ lệ, từ chối sheet trống/garbage.
- Nối pipeline ảnh: fetch như ảnh thường, raster ở worker khi setting svg
  bật, tắt thì fail-nhanh vào negative cache (không loop).
- Test: logo vnexpress 150x28 → 480x~90, pixels đầy, garbage từ chối.
SVG raster, JS engine, NetSurf/litehtml nguyên con, phát video YouTube
nhúng trong trang.

## Đợt audit Gemini (2026-10-07 chiều, đã verify + merge, test vẫn ALL PASS)
Tài liệu: `docs/NETSURF_VS_HTMLRENDERER_AUDIT.md`,
`docs/VIEWPORT_META_RESEARCH.md`. Gemini chỉ sửa `HtmlRenderer.h/.cpp`
(không đụng UIManager/mạng/UI khác).
- HTTP headers: thêm `Referer` trang hiện tại cho fetch ảnh/CSS/form
  (`getBrowserHeaders()`). Đính chính audit: `User-Agent`,
  `Accept`, `Accept-Language` đã có sẵn từ trước trong HttpClient —
  chỉ thiếu Referer.
- Text align đúng chuẩn: enum `TextAlign {LEFT,CENTER,RIGHT}` thay bool,
  con override được cha, đọc attr HTML `align`, render căn phải.
- Chống gap: container rỗng 0px (`hasVisibleContent`), margin block
  clamp 16px, chỉ `ul/ol/blockquote` thụt đầu dòng (hết cascade lệch).
- CSS image sizing: `width/height/max-width/max-height` → imgW/imgH.
- Viewport meta + `@media` đánh giá theo `effectiveViewportWidth()`
  (device-width/nhỏ nhất/980 fallback) thay vì inline mù.
- Tồn tại đã biết: `width` CSS gán vào imgW cho mọi element (vô hại —
  chỉ nhánh IMAGE đọc).

## Đợt read-scroll + sự cố file (2026-10-07 tối)
- Dpad lên/xuống = cuộn đọc từng hàng + highlight event gần nhất (link
  gạch chân cyan kể cả chữ trong thẻ con, input giữ viền sáng).
  Trái/phải đi trong khối, L1/R1 lật trang. A: link mở trang, input mở
  keyboard, button submit. Text/ảnh không bao giờ highlight.
- Sự cố: ghi nhầm path làm mất `HtmlRenderer.cpp` (chưa commit). Đã viết
  lại toàn bộ từ spec trong header + test suite (152 PASS) — là bằng
  chứng file đầy đủ. Bài học: commit sau mỗi phase, không dồn.
- Test cập nhật theo mô hình mới (scroll + nearest thay vì hop).

## Nợ device (chờ cáp/test máy)
- Verify scroll/click vnexpress + bài viết, GIF FPS/RAM, webp/gif decode
  thật (`IMG_Init` log), playback mpv từ trang, Settings UI/SELECT.
- Báo cáo user 2026-10-07: YouTube trắng trang (cần JS), CSS còn vỡ
  layout/lệch ảnh/gap nhiều → đang xử lý (P14 border/float đo tiếp).
