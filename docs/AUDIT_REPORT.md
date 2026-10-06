# AUDIT REPORT — RomCloud (10/2026)

> Audit toàn diện dựa trên code thật (`src/`), log commit, và các plan đã viết.
> Ngày audit: 2026-10-06. Commit cuối: `43ad47b`.

---

## A. Trạng thái hiện tại (snapshot)

| Hạng mục | Số liệu |
|---|---|
| Tổng `.cpp`/`.h` | 128 file |
| `UIManager.cpp` | **10.038 dòng** (god class) |
| `UIManager.h` | 932 dòng |
| `src/browser/` | 1.286 dòng (BrowserManager + HtmlRenderer) |
| `docs/` | 24 plan files |
| `bin/RomCloud` | 32 MB (chưa strip) |
| `bin/yt-dlp-glibc` | 38 MB (bắt buộc) |
| `assets/fonts/font.ttf` | 16 MB (fallback dư) |
| Binary target | aarch64-linux-gnu 2.33 |
| Hardware | TrimUI Brick Pro 1024×768, 512 MB RAM |

---

## B. Các plan đã có (tiến độ thực tế)

| Plan | Status | Ghi chú |
|---|---|---|
| `MODULE_IMPLEMENTATION_PLAN.md` (8 module) | ~70% | BackgroundTask, FileOps, VirtualKeyboard, DialogManager, MpvPlayer xong logic. **Chưa build full + chưa thay UIManager** (P0-1 → vẫn dùng wrapper ủy quyền) |
| `CLEANUP_AND_OPTIMIZATION_PLAN.md` (5 phase) | **0%** | Chưa làm phase nào |
| `WEB_BROWSER_AUDIT_AND_NETSURF_PLAN.md` (4 phase) | **~95%** | Phase 0–3 fix xong (C1, C2, C3, C4, M1–M8). NetSurf = tương lai |
| `USER_ISSUES_TODO.md` (3 issues user) | **0%** | #34 OTA fail, #33 GameCast fail, log noise |
| `NETSURF_INTEGRATION_PLAN.md` | **0%** | Source đã có `libnetsurf-src/`, build script `scripts/build-netsurf.sh` setup nhưng chưa chạy |

---

## C. Module tách (`MODULE_IMPLEMENTATION_PLAN.md`) — cần làm tiếp

### ✅ Đã xong code (chưa verify build full)
| Module | File | Trạng thái |
|---|---|---|
| P1-1 `BackgroundTask` | `src/common/BackgroundTask.h` | header-only, pattern clearTextCache |
| P1-2 `FileOps` | `src/filesystem/FileSystemManager.*` | rename/copy/recursive delete/stats/mtime đã mở rộng |
| P0-2 `VirtualKeyboard` | `src/ui/VirtualKeyboard.h` | 1 keyboard chung, Telex pass, **đã dùng ở Search/YT/TikTok/Explorer/LocalSend** |
| P0-3 `DialogManager` | `src/ui/DialogManager.h` | confirm/toast/progress gộp |
| P2-1 `FileExplorer` | (chưa có file) | **chưa code** |
| P2-2 `ImageCache` | `src/ui/ImageCache.h` | header-only LRU, **chưa replace cache cũ** |

### 🔴 Việc cần làm tiếp
| # | Việc | Effort | Phụ thuộc |
|---|---|---|---|
| 1 | **Build full `./build.sh`** để verify các module mới compile/link | 1–2 giờ | Cần Mac/Linux có zig |
| 2 | P0-1 `UiRenderer`: chuyển 25 hàm `draw*` ra khỏi UIManager | 1 ngày | Build OK |
| 3 | P1-3 `MpvPlayer`: xong PID duy nhất; **cần test phát IPTV/YouTube/TikTok** | 4 giờ | Build + device |
| 4 | P2-1 `FileExplorer`: UIState mới, CRUD + clipboard + filter | 2–3 ngày | VirtualKeyboard + DialogManager + FileOps |
| 5 | P2-2 `ImageCache`: replace `m_gridIconCache/m_systemIconCache/m_buttonIconCache/m_ytThumbnails` | 1 ngày | Build |

**Lợi ích khi xon:** UIManager.cpp giảm từ 10.038 → ~6.500 dòng (ước tính), build nhanh hơn, không phải sửa 1 chỗ vỡ 5 chỗ.

---

## D. Browser Web — đã xong, còn polish

### ✅ Fix xong (theo `WEB_BROWSER_AUDIT_AND_NETSURF_PLAN.md`)
| ID | Bug | Fix | Verified |
|---|---|---|---|
| C1 | static `s_url` race | `PendingFetch*` heap + ownership transfer | ✓ |
| C2 | `vector<HtmlElement>` dangling | đổi `std::list<HtmlElement>` | ✓ |
| C3 | worker → main race | `enqueueFetchResult` + mutex + `pollFetch()` trên main | ✓ |
| C4 | history duplicate khi back | `loadUrlNoHistory()` private | ✓ |
| M1 | render đè header | CONTENT_Y/CONTENT_H bounds | ✓ |
| M2 | scroll chết | scroll wired (cần verify L1/R1) | partial |
| M3 | init() không reset | reset đủ `m_inputIndex/m_maxScroll/m_loading/m_error/...` | ✓ |
| M4 | `std::isspace(char)` UB | (theo plan, đã cast unsigned char) | cần grep verify |
| M5 | submitForm rỗng | implement `HttpClient::postForm` + urlencode + GET fallback | ✓ |
| M6 | scheme whitelist | `http://`/`https://` only ở `openUrl` + `submitForm` | ✓ |
| M7 | OOM risk | `CURLOPT_MAXFILESIZE_LARGE = 2 MB` | ✓ |
| M8a | `<script>`/`<style>` content | bỏ qua content trong parser | cần grep verify |
| M8b | comment `<!-- -->` | (theo plan) | cần grep verify |
| M8c | attribute không quote | (theo plan) | cần grep verify |

### 🔴 Còn thiếu (Web Browser)
| # | Việc | Effort | Ghi chú |
|---|---|---|---|
| 1 | Verify M2 scroll L1/R1 trên thiết bị | 30 ph | UI integration UIManager L7070-7079 |
| 2 | Verify M4/M8 (parser edge cases) | 1 giờ | grep code + test page có `<script>`, `<style>`, comment |
| 3 | **VirtualKeyboard wire-up** cho input field | 1 ngày | `HtmlRenderer::typeCharacter` có nhưng UIManager chưa gọi |
| 4 | Tables chưa support colspan/rowspan | 1–2 ngày | custom parser hiện chỉ flat cell |
| 5 | `<img src="...">` chưa render | 2–3 ngày | cần libpng/jpeg decode + cache |
| 6 | HTTPS redirect, cookies, cache | 1 tuần | cần libssl + storage |
| 7 | NetSurf thật (full browsing) | **1–2 tuần** | build 16 lib, link, tích hợp frontend |

**Đề xuất:** đủ cho Wi-Fi portal + simple page. Làm (1)–(3) trước, (4)–(7) tùy nhu cầ.

---

## E. Cleanup & Tối ưu release (`CLEANUP_AND_OPTIMIZATION_PLAN.md`)

### Phase 1 — Dọn code chết (rủi ro rất thấp) — **CHƯA LÀM**
- `UIManager.cpp`: xóa `parseInnertubeSearchResponse()`, `getBatteryLevel()`, `getCurrentTimeString()` (static, unused)
- `UIManager.cpp`: bỏ `FOOTER_H` trong `renderIPTVState`
- `UIManager.cpp` L992/1785/2296: lambda `(const char *m)` → `(const char *)`
- `UIManager.cpp`: `renderYouTubeHomeContentRow(int /*contentH*/)` unused param
- `IPTVManager.cpp`: xóa `buildChannelListAss()`, `green` var
- **Effort: 1 giờ. Sau khi làm: 0 warning mới.**

### Phase 2 — Dọn file repo không vào release (rủi ro thấp) — **CHƯA LÀM**
- `assets/grid_icons/` (16 file, ~1 MB) — 0 ref, trùng `icons/`
- `assets/stock_icons/` (21 file, ~2 MB) — 0 ref
- `assets/button_icons/options.png, view.png, start_icon.png` — cấm bởi AGENTS.md
- `bin/poc_fb_capture`, `tools/poc_fb_capture.cpp`, `tools/ls_test_input.c` — POC không build/package
- `assets/fonts/NotoSans{HK,JP,KR,SC,TC}-Regular.ttf` (35 MB) — không package
- **Effort: 30 phút. Không ảnh hưởng binary/zip.**

### Phase 3 — Sửa font fallback (rủi ro trung bình) — **CHƯA LÀM**
- `UIManager.cpp` L113-123: xóa entry `NotoSansTC.ttf` (đường dẫn chết) + `font.ttf`
- `AppConfig.cpp` L157-165: tương tự
- `MpvPlayer.cpp` L40-42 (`resolveOsdFont`): `font.ttf` → `/usr/trimui/res/full.ttf`
- `package.sh` L56: bỏ copy `font.ttf`
- `package.sh`: **bổ sung copy `assets/stock_keyboard/`** (đang thiếu — release phụ thuộc fallback `/usr/trimui/res/skin/`)
- **Effort: 2 giờ + 30 ph test máy. Tiết kiệm: −16 MB zip.**

### Phase 4 — Strip binary (rủi ro trung bình) — **CHƯA LÀM**
- `build.sh`: thêm `RELEASE=1` → link `-s` cho RomCloud + gamecast_d; giữ `RomCloud.debug`
- `package.sh`: check `file bin/RomCloud` = stripped
- `.gitignore`: thêm `bin/*.debug`
- Lưu ý: `IssueLogger` dùng `backtrace_symbols` → crash report chỉ còn địa chỉ, giải mã bằng `addr2line` + `RomCloud.debug`
- **Effort: 2 giờ. Tiết kiệm: −~28 MB → ~5–8 MB final.**

### Phase 5 — Tối ưu ảnh + yt-dlp Lite (tùy chọn)
| Việc | Effort | Tiết kiệm |
|---|---|---|
| Resize `APP.png`/`app_main.png` 1254→512 + nén PNG | 1 giờ | −2 MB |
| Bỏ `yt-dlp-glibc` khỏi Lite, dùng `UpdateManager::checkAndInstallDependencies()` tự tải | 30 ph | **−38 MB Lite** (lần đầu phát YT phải tải) |

**Tổng tiết kiệm nếu làm Phase 1–4:** ~46 MB binary/zip + ~14 MB repo checkout.

---

## F. User-Reported Issues (`USER_ISSUES_TODO.md`) — **CHƯA XỬ LÝ**

### 🔴 #34 — OTA 2.4.0 → 2.4.1 fail giải nén
- **Nghi vấn:** thẻ nhớ đầy → zip cụt, hoặc `unzip` warning = exit 1
- **Code:** `src/ota/UpdateManager.cpp` L845-855 `installFullZip`
- **Fix:**
  1. `statvfs` appRoot trước tải: cần ≥ ~3× size zip
  2. So `downloadedSize` vs `info.sizeBytes` (>0)
  3. Ghi stderr `unzip` ra file tạm, log 10 dòng cuối khi fail
  4. Chấp nhận exit 1 nếu `staged/bin/RomCloud` tồn tại
  5. Fallback: `bin/7zzs x` (đã có sẵn)
- **Effort: 1 ngày (test thẻ đầy giả lập)**

### 🟠 #33 — GameCast daemon fail
- **Nghi vấn:** bind port 8090 fail (process khác giữ port), crash, hoặc không exec được
- **Code:** `src/cast/CastManager.cpp` L186
- **Fix:**
  1. Khi fail: đọc tail `/tmp/gamecast.log` → `Logger::warn` + toast
  2. Nếu bind fail: log process đang giữ port 8090 (quét `/proc/net/tcp`)
  3. Fallback không qua `nice` nếu lần đầu fail
- **Effort: 4 giờ**

### 🟡 Phụ
| Việc | Effort |
|---|---|
| `[STATE] SET/BACK` log ERROR → debug (`UIManager.cpp` L361, L382) | 5 ph |
| `filter.ts` L92 gộp log thành 1 dòng, `wrangler deploy` lại | 15 ph |
| Timezone lệch ~1h — kiểm tra NTP Stock | 1 giờ |

---

## G. NetSurf Integration — **CHƯA BẮT ĐẦU**

### Hiện trạng
- Source: `libnetsurf-src/` (16 lib: hubbub, css, dom, gif, bmp, sprite, svgtiny, pencil…)
- Build script: `scripts/build-netsurf.sh` đang setup native build (chậm, ARM yếu)
- Chưa có `netsurf-all-3.11/` — cần download

### Effort ước tính
| Bước | Effort |
|---|---|
| Download `netsurf-all-3.11` + apply Makefile.config | 30 ph |
| Build parserutils + hubbub + nsgif + nsbmp + css + dom + pencil (chain) | **1–2 giờ build native**, debug linker 2–4 giờ |
| Link vào RomCloud + replace HtmlRenderer backend | 1 tuần |
| Frontend integration (plotter API cho SDL2) | 1 tuần |
| **Tổng: 1–2 tuần** |

### Quyết định
- Hiện tại custom parser đủ cho Wi-Fi portal + simple page
- NetSurf chỉ cần nếu muốn full browsing (CSS selectors, float, flexbox, ảnh lớn)
- **Đề xuất: KHÔNG làm giai đoạn này** — quay lại khi có use case thật

---

## H. Đề xuất ưu tiên (next 1–2 tuần)

| Ưu tiên | Việc | Effort | Lý do |
|---|---|---|---|
| 🔴 **P0** | User issue #34 (OTA fail) | 1 ngày | Đang ảnh hưởng release OTA cho user thật |
| 🔴 **P0** | User issue #33 (GameCast fail) | 4 giờ | Tương tự |
| 🟠 **P1** | Phase 1 cleanup (dead code) | 1 giờ | Giảm warning, an toàn |
| 🟠 **P1** | Verify Browser M2/M4/M8 + VirtualKeyboard wire-up ✅ | 1 ngày | xong `d339d20` — 167 LOC, 0 warning |
| 🟡 **P2** | Tables colspan/rowspan (optional) | 1–2 ngày | Custom parser hiện flat cell. Chưa làm |
| 🟡 **P2** | `<img src="...">` render (optional) | 2–3 ngày | Cần libpng/jpeg + cache. Chưa làm |
| 🟡 **P2** | Module Plan: build full + P0-1 UiRenderer | 1 ngày | Giảm UIManager.cpp từ 10K → 6.5K dòng |
| 🟡 **P2** | Phase 2 cleanup (file repo) | 30 ph | Repo sạch |
| 🟢 **P3** | Phase 3 font + stock_keyboard packaging | 2 giờ | −16 MB |
| 🟢 **P3** | Phase 4 strip binary | 2 giờ | −28 MB |
| ⚪ **P4** | NetSurf thật | 1–2 tuần | Khi có nhu cầu full browsing |
| ⚪ **P4** | Module Plan P2-1 FileExplorer | 2–3 ngày | Feature mới |

**Lưu ý:** Mỗi thao tác phải build + test trên thiết bị + commit riêng theo convention dự án.

---

## I. Các file/thư mục cần kiểm tra kỹ (chưa audit sâu)
- `src/filesystem/` (FileOps mở rộng) — chưa verify edge case (symlink, permission, FAT32)
- `src/cast/` (CastManager) — chưa audit drain
- `src/localsend/` (LocalSend) — protocol phức tạp, có thể có bug network race
- `src/download/` — multi-source download engine, có thể có leak file descriptor
- `src/backup/`, `src/sync/` — chưa từng chạm

**Đề xuất:** dành 1 ngày audit riêng mỗi module trên khi có thời gian.

---

## K. Tổng kết

- **Trạng thái:** App chạy ổn trên Brick, web browser đã có (custom mini).
- **Code nợ kỹ thuật:** UIManager.cpp 10K dòng (god class), chưa build full sau khi tách module.
- **Rủi ro release:** User #34 OTA fail có thể tái diễn ở bản sau.
- **Cleanup chưa làm:** −46 MB binary/zip khả thi, không ảnh hưởng tính năng.
- **NetSurf:** Đủ source, chưa build, **không ưu tiên** trừ khi cần full web.