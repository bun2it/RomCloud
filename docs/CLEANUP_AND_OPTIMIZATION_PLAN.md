# Kế hoạch Dọn dẹp & Tối ưu Release RomCloud (chia Phase an toàn)

> Bản v2 — đã audit lại bằng grep toàn bộ `src/`, `scripts/`, `package.sh`.
> Mỗi phase độc lập, có điểm kiểm tra và cách rollback. Làm xong phase nào commit phase đó.

---

## 0. Đính chính so với plan v1 (QUAN TRỌNG)

| Nhận định v1 | Thực tế sau audit | Hệ quả |
| :--- | :--- | :--- |
| `bin/yt-dlp-glibc` (38MB) không dùng, xoá được | **ĐANG DÙNG.** `bin/yt-dlp` chỉ là wrapper 4KB gọi `yt-dlp-glibc`. `youtube_search.py get_url` cần nó để lấy stream URL khi **phát** video. Innertube chỉ dùng cho **tìm kiếm**. | **KHÔNG xoá.** Xem Phase 5 (tuỳ chọn tải theo nhu cầu). |
| `assets/icons/*` hệ máy trùng, code dùng `grid_icons/` | Ngược lại: `UiRenderer::drawIcon` load `assets/icons/`. `assets/grid_icons/` **không có tham chiếu nào** (file giống hệt). | Xoá `grid_icons/`, giữ `icons/`. |
| Warning lambda nằm ở `ExplorerSync.cpp` | Nằm ở `UIManager.cpp` (L992, L1785, L2296) — tham số `m` không dùng. | Sửa đúng file. |

Phát hiện thêm:
- `package.sh` **không copy** `assets/stock_keyboard/` → bản release đang dựa vào fallback `/usr/trimui/res/skin/`. Cần bổ sung (Phase 3).
- `assets/stock_icons/` không có tham chiếu nào trong code (chỉ là nguồn đã pull từ máy).
- `font.ttf` là fallback cuối trong `UIManager`, `AppConfig`, `MpvPlayer` (sau NotoSans-Regular và font hệ thống `/usr/trimui/res/full.ttf`).
- Fonts CJK (`NotoSansHK/JP/KR/SC/TC`) và `grid_icons/`, `tools/`, `bin/poc_fb_capture` **không nằm trong gói release** → xoá chỉ làm sạch repo, không giảm size zip.
- Code đang tham chiếu `NotoSansTC.ttf` nhưng file thật là `NotoSansTC-Regular.ttf` → đường dẫn chết.

### Dung lượng thực tế đóng gói hiện nay
| Thành phần | Size | Có trong zip? |
| :--- | :--- | :--- |
| `bin/RomCloud` (chưa strip) | 32 MB | Có |
| `bin/yt-dlp-glibc` | 38 MB | Có (bắt buộc) |
| `lib/` (ffmpeg, SDL, ass…) | 22 MB | Có (bản Full) |
| `assets/fonts/font.ttf` | 16 MB | Có |
| `bin/gamecast_d` (chưa strip) | 4 MB | Có |
| `assets/apps_icons/APP.png` + `app_main.png` | 2.2 MB | Có |

**Tiết kiệm thực tế khả thi:** strip (~28MB) + bỏ `font.ttf` (16MB) + nén icon (~2MB) ≈ **45MB** (chưa nén zip). Không phải 130→18MB như v1.

---

## Phase 1 — Dọn code chết C++ (rủi ro: RẤT THẤP)

Chỉ xoá thứ compiler báo "unused". Không đổi hành vi.

| File | Việc |
| :--- | :--- |
| `src/ui/UIManager.cpp` | Xoá `parseInnertubeSearchResponse()`, `getBatteryLevel()`, `getCurrentTimeString()` (static, unused). |
| `src/ui/UIManager.cpp` | Xoá biến `FOOTER_H` không dùng trong `renderIPTVState`. |
| `src/ui/UIManager.cpp` L992/1785/2296 | Lambda `(const char *m)` → `(const char *)`. |
| `src/ui/UIManager.cpp` | `renderYouTubeHomeContentRow(int contentTop, int /*contentH*/)`. |
| `src/iptv/IPTVManager.cpp` | Xoá `buildChannelListAss()` (ASS OSD cũ), biến `green`; kiểm tra `sanitizeAssText` còn caller khác không rồi mới xoá. |

**Kiểm tra:** `touch` 2 file rồi `./build.sh` → 0 warning từ các mục trên (zig cache file không đổi nên phải touch để thấy warning).
**Test máy:** mở IPTV (OSD 3 kênh), YouTube Home, header pin/giờ.
**Rollback:** `git checkout -- src/ui/UIManager.cpp src/iptv/IPTVManager.cpp`.

---

## Phase 2 — Dọn file repo không vào release (rủi ro: THẤP)

Không ảnh hưởng binary/zip. Chỉ làm sạch repo.

| Xoá | Lý do đã kiểm chứng |
| :--- | :--- |
| `assets/grid_icons/` (16 file) | 0 tham chiếu, trùng 100% `assets/icons/`. |
| `assets/stock_icons/` (21 file) | 0 tham chiếu. MENU/SELECT/START đã copy sang `button_icons/`. |
| `assets/button_icons/options.png`, `view.png`, `start_icon.png` | Legacy bị cấm (AGENTS.md rule 8). **Trước khi xoá:** grep `"options"`, `"view"`, `"start_icon"` trong `UiRenderer::drawButtonIcon` để chắc không còn map tới. |
| `bin/poc_fb_capture`, `tools/poc_fb_capture.cpp`, `tools/ls_test_input.c` | POC, không build, không package. |
| `assets/fonts/NotoSans{HK,JP,KR,SC,TC}-Regular.ttf` (35MB) | Không package. **Cần bạn xác nhận** (xem câu hỏi bên dưới). |

Giữ lại: `tools/check_app_crash.sh`, `tools/check_brick_game.sh` (tool debug hữu ích).
`src/logging/test_issue_logger.cpp`: không có trong `build.sh` → giữ hoặc chuyển vào `tests/`.

**Kiểm tra:** `./build.sh` + `./package.sh` vẫn chạy OK.
**Rollback:** `git checkout -- <path>` (file đã tracked).

---

## Phase 3 — Sửa font fallback & đóng gói (rủi ro: TRUNG BÌNH)

1. **Bỏ `font.ttf` (16MB) khỏi release:**
   - `UIManager.cpp` L113-123, `AppConfig.cpp` L157-165: xoá entry `NotoSansTC.ttf` (đường dẫn chết) và `font.ttf`. Thứ tự còn lại: NotoSans-Regular → `/usr/trimui/res/full.ttf` → `regular.ttf` → msyh.
   - `MpvPlayer.cpp` L40-42 (`resolveOsdFont`): fallback `font.ttf` → `/usr/trimui/res/full.ttf`.
   - `package.sh` L56: bỏ copy `font.ttf`, bỏ `2>/dev/null || true` ở NotoSans (để fail rõ nếu thiếu).
2. **Bổ sung `assets/stock_keyboard/` vào `package.sh`** (đang thiếu).
3. **Lưu ý người dùng cũ qua OTA:** `font.ttf` cũ vẫn nằm trên thẻ → vô hại. Không cần script xoá.

**Kiểm tra trên máy (bắt buộc):**
- Tạm đổi tên `NotoSans-Regular.ttf` → xác nhận app vẫn lên chữ bằng font hệ thống, rồi đổi lại.
- OSD mpv (YouTube/IPTV) hiển thị tiếng Việt đúng.
- Bàn phím ảo hiện đúng keycap stock khi chạy từ bản package.

**Rollback:** revert 4 file trên.

---

## Phase 4 — Strip binary release (rủi ro: TRUNG BÌNH)

1. `build.sh`: thêm biến `RELEASE=1` → khi bật, sau khi build:
   - giữ bản debug: `cp bin/RomCloud bin/RomCloud.debug`
   - link với `-s` (zig c++ hỗ trợ) cho `RomCloud` và `gamecast_d`.
   - Mặc định (dev) **vẫn giữ debug info** để debug crash.
2. `package.sh`: gọi `RELEASE=1 ./build.sh` hoặc kiểm tra `file bin/RomCloud` báo `stripped`, không thì cảnh báo.
3. `.gitignore`: thêm `bin/*.debug`.

**Kiểm tra:** `file bin/RomCloud` → `stripped`, size ~5–8MB; chạy trên máy: Home, IPTV, YouTube, LocalSend, GameCast, OTA check.
**Lưu ý:** Kiểm tra `IssueLogger` có dùng `backtrace_symbols` không — nếu có, log crash từ bản release sẽ chỉ còn địa chỉ; giải mã bằng `RomCloud.debug` + `addr2line`.
**Rollback:** bỏ flag `-s`.

---

## Phase 5 — Tối ưu assets ảnh & phân phối yt-dlp (rủi ro: TRUNG BÌNH — TUỲ CHỌN)

1. **`APP.png`, `app_main.png` (1254×1254, 1.1MB mỗi file):** resize 512×512 + nén PNG (`sips`/`pngquant`). Kiểm tra trước kích thước hiển thị thực tế trong `Application.cpp` L168/L193 và `UpdateManager.cpp` L750 (icon launcher TrimUI).
2. **yt-dlp-glibc (38MB) — tuỳ chọn:** bỏ khỏi gói **Lite**, vì code đã có cơ chế `UpdateManager::checkAndInstallDependencies()` tự tải khi phát YouTube lần đầu (`UIManager.cpp` L6803-6808) và đã có trong `mpv_bundle.zip`. Đổi lại: lần đầu xem YouTube phải chờ tải ~38MB và cần mạng.

**Kiểm tra:** cài mới bản Lite trên thẻ sạch → mở YouTube → xác nhận auto-repair tải đủ và phát được.

---

## Thứ tự & checklist

| Phase | Rủi ro | Ảnh hưởng size zip | Cần test máy |
| :--- | :--- | :--- | :--- |
| 1. Code chết | Rất thấp | ~0 | Nhẹ |
| 2. File repo | Thấp | 0 (chỉ repo, −~40MB checkout) | Không |
| 3. Font + package | Trung bình | −16MB | Có |
| 4. Strip | Trung bình | −~28MB | Có (đầy đủ) |
| 5. Ảnh + yt-dlp Lite | Trung bình | −2MB (−38MB Lite nếu chọn) | Có (cài mới) |

Sau mỗi phase: `./build.sh` → deploy → test → `git commit -m "cleanup(phaseN): ..."`.
