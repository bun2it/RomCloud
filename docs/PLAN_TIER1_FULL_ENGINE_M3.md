# 📋 PLAN: Tier 1 — Sửa toàn bộ giới hạn của FULL engine (Ghi chú từ Agent M3)

> **Mục đích:** Lưu trữ bản kế hoạch chi tiết từ Agent M3 để đối chiếu, audit và triển khai kiến trúc **In-process NetSurfEngine** cho RomCloud Web Browser.

---

## 🎯 Mục tiêu tổng thể

| Hiện tại (dead code) | Sau khi sửa |
|----------------------|-------------|
| FULL dùng `NetSurfWorkerBridge` (fork daemon 19.3MB) | FULL dùng `NetSurfEngine` (in-process) + `NetSurfWorkerBridge` làm fallback |
| Không có JS | QuickJS embedded (~1MB) |
| CSS subset 17 properties | Mở rộng lên ~25 properties |
| Restart daemon mỗi navigate | In-process navigate, ~50ms |
| Cookie mất khi tắt | `-f file:...` → persist |
| RAM ~250MB | RAM ~100MB |

**Phạm vi thay đổi**: tổng cộng ước tính **~1.500-2.000 LOC** (chủ yếu là sửa, không phải viết mới).

---

## 📦 Phase P20 — Wire in-process FULL vào UIManager (3-5 ngày)

**Mục tiêu**: từ "FULL daemon" chuyển sang "FULL in-process" làm primary, daemon thành fallback.

### Công việc

**P20.1 — `BrowserManager::openUrl()` (FULL branch)** — `src/browser/BrowserManager.cpp:103-110`

Thay vì gọi `NetSurfWorkerBridge::instance().start(fixed)`, làm:
```cpp
if (m_settings.fullEngine) {
    Logger::info("BrowserManager: FULL engine (in-process) for " + fixed);
    NetSurfEngine::instance().clear();        // reset state cũ
    fetchAndLoadFull(fixed);                  // fetch HTTP → NetSurfEngine::loadHtml
    // KHÔNG spawn daemon. Nếu NetSurfEngine chết, fallback ở P20.4.
    m_state = BrowserState::RENDERING;
} else {
    HtmlRenderer::instance().loadUrl(fixed);
}
```

**P20.2 — `fetchAndLoadFull()`** (mới, ~50 LOC) — async fetch qua HttpClient thread riêng:
- Tạo worker thread tương tự `HtmlRenderer::loadUrl` (line 297-340) nhưng destination là `NetSurfEngine::instance().loadHtml(html, url)`
- Cap 1.5MB, timeout 30s (giống LITE)
- Error → `m_state = BrowserState::ERROR`, `m_errorMsg = "..."`
- Thành công → `m_state = BrowserState::RENDERING`, đợi layout xong ở `NetSurfEngine::poll()` (đã có sẵn)

**P20.3 — `UIManager.cpp:7430` render loop** — đổi FULL branch:

```cpp
// CŨ:
if (BrowserManager::instance().engineMode() == BrowserEngine::FULL) {
    NetSurfWorkerBridge::instance().render(m_renderer);
} else {
    HtmlRenderer::instance().render();
}

// MỚI:
if (BrowserManager::instance().engineMode() == BrowserEngine::FULL) {
    NetSurfEngine::instance().render();   // in-process: poll + draw
    // Có thể fallback về daemon nếu in-process fail — xem P20.4
} else {
    HtmlRenderer::instance().render();
}
```

**P20.4 — Fallback to daemon** (P20 chỉ làm in-process làm default; daemon là optional safety net):
- Thêm `m_useInProcessFull = true` flag (default true)
- Nếu `NetSurfEngine` không init được (libcss missing) → auto fallback `m_useInProcessFull = false` → dùng daemon như cũ
- Nếu user gặp crash lặp lại trên trang JS-heavy, có thể ép dùng daemon qua settings

**P20.5 — Settings UI** — `UIManager.cpp` (settings screen chỗ toggle Engine):
- Thêm mục "FULL engine: In-process / Daemon" (chỉ hiện khi fullEngine=true)
- Lưu vào `browser.cfg`: `inProcessFull=1` (default)
- `BrowserManager::loadSettings()` đọc thêm

**P20.6 — `BrowserManager::goBack()` / `refresh()`** — đổi tương tự P20.1, dùng in-process.

**P20.7 — Build flag** — `build.sh`:
- Mặc định `BUILD_LIBNETSURF=1` (đang là 0) — vì nếu không có libcss/libhubbub thì `NetSurfBridge::init()` silent fallback và in-process không hoạt động
- Verify `sysroot/lib/libcss.a`, `libhubbub.a`, `libdom.a`, `libparserutils.a`, `libwapcaplet.a` tồn tại

### Files thay đổi (P20)

| File | Thay đổi | LOC |
|------|----------|-----|
| `src/browser/BrowserManager.cpp` | Thêm `fetchAndLoadFull()`, đổi FULL branch trong openUrl/goBack/refresh | +80 / -10 |
| `src/browser/BrowserManager.h` | Thêm `m_useInProcessFull`, prototype `fetchAndLoadFull` | +10 |
| `src/ui/UIManager.cpp` | Đổi render loop (line 7430) + settings UI toggle | +30 / -10 |
| `build.sh` | Set `BUILD_LIBNETSURF=1` mặc định | +5 / -2 |
| `src/browser/netsurf/NetSurfWorkerBridge.cpp` | (Optional) đánh dấu fallback, không xóa | +20 comment |

### Verification P20

1. **Build**:
   ```bash
   BUILD_LIBNETSURF=1 ./build.sh
   ```
2. **Test host**:
   ```bash
   ./build_pc.sh && ./bin/test_browser   # phải pass hết 120+ tests
   ```
3. **Test on-device** (qua adb):
   ```bash
   adb push bin/RomCloud /mnt/SDCARD/Apps/RomCloud/bin/
   adb shell sync
   adb shell am start -n ...
   ```
4. **Smoke test**:
   - Mở `assets/netsurf/welcome.html` (default page khi boot) → phải render được qua `NetSurfEngine`
   - Mở `https://example.com` → phải parse + layout + render qua in-process, không còn fork daemon
   - Check `ps aux` trên máy: KHÔNG còn process `netsurf` con
   - Check `free -h`: RAM dùng giảm ~50-100MB
   - Check `dmesg` hoặc logcat: `NetSurfEngine: layout ok` thay vì `NetSurfWorkerBridge: netsurf ready`

### Rủi ro P20
- ⚠️ NetSurf libs chưa build → phải verify trước. Nếu build fail, fallback về daemon.
- ⚠️ Threading issue: `NetSurfEngine::poll()` chạy main thread, cần đảm bảo m_pendingTree được move đúng cách (đã có sẵn trong code, có mutex).
- ⚠️ Form editing: P17 API đã có sẵn, chỉ cần test trên máy.

---

## 📦 Phase P21 — Mở rộng CSS computed extraction (3-5 ngày)

**Mục tiêu**: từ 17 properties → 25 properties. Fix những gì user thấy ngay (line-height hardcode, text-decoration missing, font-style italic).

### Công việc

**P21.1 — Extend `NetSurfComputedStyle`** — `src/browser/netsurf/NetSurfBridge.h:22-58`

Thêm fields:
```cpp
float lineHeight = 1.35f;          // line-height multiplier
uint8_t fontStyle = 0;             // 0 = normal, 1 = italic
uint8_t textDecoration = 0;        // bitfield: 0x01=underline, 0x02=line-through
uint8_t textTransform = 0;         // 0=none, 1=uppercase, 2=lowercase, 3=capitalize
uint8_t verticalAlign = 0;         // 0=baseline, 1=top, 2=middle, 3=bottom
uint8_t position = 0;              // 0=static, 1=relative, 2=absolute, 3=fixed
int letterSpacingPx = 0;           // letter-spacing
uint8_t visibility = 0;            // 0=visible, 1=hidden
uint8_t overflow = 0;              // 0=visible, 1=hidden, 2=scroll
```

**P21.2 — Extract từ libcss** — `src/browser/netsurf/NetSurfBridge.cpp:920-1140`

Trong `computeStylesRecursive`, thêm các `css_computed_*` calls:
```cpp
// line-height (P21)
css_unit unit = CSS_UNIT_PX;
css_fixed val = 0;
uint8_t lh_type;
css_computed_line_height(computed, &lh_type, &val, &unit);
if (lh_type == CSS_LINE_HEIGHT_NUMBER) {
    style.lineHeight = val * 1.0f;  // multiplier
} else if (lh_type == CSS_LINE_HEIGHT_LENGTH && unit == CSS_UNIT_PX) {
    style.lineHeight = (float)val / (float)style.fontSizePx;
}
// font-style
style.fontStyle = css_computed_font_style(computed);  // 0=normal, 1=italic
// text-decoration
style.textDecoration = css_computed_text_decoration(computed);
// position
style.position = css_computed_position(computed);
// visibility
style.visibility = css_computed_visibility(computed);
// letter-spacing
css_computed_letter_spacing(computed, &val, &unit);
```

**P21.3 — Update `NetSurfLayout`** — `src/browser/netsurf/NetSurfLayout.cpp`

- Sửa line 196: dùng `style.lineHeight` thay vì hardcode `fontSize * 1.35f`
- Thêm position handling (chỉ relative đơn giản: offset top/left)
- Sửa text decoration: `if (style.textDecoration & 0x01) drawLine() dưới text`
- Sửa font-style: dùng `TTF_SetFontStyle(face, TTF_STYLE_ITALIC)` (cần load font italic riêng)

**P21.4 — Update `NetSurfRenderer`** — `src/browser/netsurf/NetSurfRenderer.cpp`

- Italic: nếu không có italic font thì dùng slant manual (transform text matrix 12° — dùng SDL không có sẵn, phải dùng `TTF_SetFontStyle` hoặc load font riêng)
- Underline: `SDL_RenderDrawLine(y = textBottom + 1)`
- Line-through: `SDL_RenderDrawLine(y = textMid)`
- Vertical-align middle (trong td/th): dịch box.y theo height

**P21.5 — Load italic font (nếu có)** — `src/browser/netsurf/NetSurfRenderer.cpp:23-30` (init)

- Nếu `/mnt/SDCARD/Assets/fonts/italic.ttf` tồn tại → load làm `m_fontItalic`
- Nếu không → fallback về roman (bỏ qua italic) — không crash

### Files thay đổi (P21)

| File | Thay đổi | LOC |
|------|----------|-----|
| `src/browser/netsurf/NetSurfBridge.h` | Thêm fields computed | +20 |
| `src/browser/netsurf/NetSurfBridge.cpp` | Extract thêm properties | +80 |
| `src/browser/netsurf/NetSurfLayout.cpp` | Apply line-height, position, decoration | +60 |
| `src/browser/netsurf/NetSurfRenderer.cpp` | Draw underline/line-through, italic handling | +50 |

### Verification P21

1. Test page có:
   ```html
   <p style="line-height: 2.0">Dòng cao gấp đôi</p>
   <p style="text-decoration: underline">Gạch chân</p>
   <p style="text-decoration: line-through">Gạch ngang</p>
   <p style="font-style: italic">Chữ nghiêng</p>
   <p style="text-transform: uppercase">in hoa</p>
   <p style="visibility: hidden">ẩn</p>
   <p style="position: relative; top: 20px">Dịch xuống</p>
   ```
2. Chạy test_browser, mở file test, assert từng cái render đúng

### Rủi ro P21
- ⚠️ libcss có API cho mỗi property cần check version. Nếu version cũ thiếu API → skip property đó, log warning.
- ⚠️ Italic font: nếu user không có italic.ttf → render roman. Không nguy hiểm.
- ⚠️ `position:absolute/fixed` chưa hỗ trợ trong plan này (chỉ relative). Absolute là Phase 2.

---

## 📦 Phase P22 — QuickJS JavaScript engine (5-7 ngày)

**Mục tiêu**: chạy JavaScript trong trang web. Không cần đầy đủ DOM, chỉ cần đủ để:
- Mở `addEventListener('click', ...)`
- Chạy `document.getElementById('x').textContent = '...'` → re-render
- Chạy `setTimeout` cho banner ads
- Chạy `document.querySelector('.btn').click()` synthetic events

### Công việc

**P22.1 — Vendor QuickJS** — copy `quickjs.c`, `quickjs.h`, `cutils.c`, `libregexp.c`, `libunicode.c` vào `src/browser/quickjs/`

- QuickJS 2024-04-26 release (~7000 LOC C)
- License: MIT (tương thích GPL của RomCloud)
- Thêm vào `CMakeLists.txt` build target

**P22.2 — Bridge QuickJS ↔ libdom** — `src/browser/quickjs/JsBridge.cpp` (mới, ~400 LOC)

```cpp
class JsBridge {
    JSRuntime* rt;
    JSContext* ctx;
    dom_document* doc;  // current libdom doc
    
    // Bindings exposed to JS:
    // - window.console.log/warn/error
    // - window.setTimeout / clearTimeout
    // - window.fetch(url)  → return Promise (basic)
    // - document.getElementById(id) → JsDomElement
    // - document.querySelector(sel) → JsDomElement
    // - document.querySelectorAll(sel) → JsDomElement[]
    // - document.createElement(tag) → JsDomElement
    // - document.body.appendChild(el)
    // - element.textContent = '...'
    // - element.innerHTML = '...' (parse + append)
    // - element.style.display = 'none'
    // - element.classList.add/remove/toggle
    // - element.click()  → trigger event
    // - element.addEventListener('click', fn)
    // - element.value (for input)
    // - location.href = '...'  → trigger navigation
    // - new Image(), img.src = '...', img.onload
};
```

**P22.3 — Hook vào `NetSurfEngine`** — `src/browser/netsurf/NetSurfEngine.cpp:188-217` (sau poll() thành công)

```cpp
void NetSurfEngine::runJs() {
    if (!m_initialized || !m_pendingOk) return;
    // 1. Serialize libdom DOM to JSON
    // 2. QuickJS context sees JSON tree as native JS objects
    // 3. Re-hydrate back to libdom on mutation
    //    (or: walk libdom directly, build JS objects with getters/setters
    //     that call libdom mutators on access)
}
```

**P22.4 — Trigger re-render khi DOM đổi** — `JsBridge` set a flag `m_dirty = true` khi bất kỳ setter nào được gọi. Main loop check `m_dirty` → re-layout + re-render.

**P22.5 — Settings toggle** — `BrowserSettings.js = true` đã có. Thêm mặc định bật trong FULL mode.

**P22.6 — `m_browserVk` JS interaction** — nếu JS gọi `input.focus()` từ page, chuyển `setEditing(true)`.

**P22.7 — Disable JS theo settings** — `m_settings.js` đã có. QuickJS chỉ khởi tạo nếu `js=true`.

### Files thêm/sửa (P22)

| File | Thay đổi | LOC |
|------|----------|-----|
| `src/browser/quickjs/*.c,h` | Vendor QuickJS | +7000 |
| `src/browser/quickjs/JsBridge.{h,cpp}` | Bridge mới | +500 |
| `src/browser/netsurf/NetSurfEngine.h/cpp` | Thêm `runJs()` hook | +50 |
| `src/ui/UIManager.cpp` | Gọi `runJs()` sau poll, settings UI thông báo JS on/off | +30 |
| `CMakeLists.txt` | Thêm quickjs vào build | +5 |

### Verification P22

1. Test page:
   ```html
   <button onclick="document.getElementById('out').textContent='Đã bấm!'">Bấm</button>
   <div id="out">Chưa bấm</div>
   <script>setTimeout(()=>{ document.getElementById('out').textContent='sau 2s'; }, 2000);</script>
   ```
2. Bấm nút A trên button → text phải đổi thành "Đã bấm!"
3. Đợi 2s → text đổi thành "sau 2s"
4. Test mở `https://example.com` (page có `<script>` minimal) — phải không crash

### Rủi ro P22
- ⚠️ QuickJS + libdom interaction phức tạp. Có thể dùng cách đơn giản hơn: clone DOM sang JS-side JSON, thao tác trên JSON, sau khi xong thì apply lại vào libdom. Tradeoff: chậm hơn nhưng an toàn.
- ⚠️ Memory: QuickJS mỗi page reset context. ~10MB overhead.
- ⚠️ Event loop: phải xử lý setTimeout không block main thread.
- ⚠️ Không hỗ trợ fetch(), Promise (advanced), module system (ESM). Giới hạn ở mức "đọc được trang tin tức".

---

## 📦 Phase P23 — Cookie persist + cleanup (2-3 ngày)

**Mục tiêu**: cookie không bị mất khi tắt máy.

### Công việc

**P23.1 — Switch daemon flag** — `src/browser/netsurf/NetSurfWorkerBridge.cpp:130-145` (initShm + spawn)

Nếu user chọn FULL=daemon, đổi `-f ram` → `-f file:/mnt/SDCARD/Apps/RomCloud/data/NetSurf/Choices` (mkdir -p trước).

**P23.2 — Cookie jar cho in-process** — `src/browser/HtmlRenderer.cpp` đã có Cookie jar (P4). Tái sử dụng cho FULL path.

Trong `fetchAndLoadFull()` (P20.2), pass `HtmlRenderer::getBrowserHeaders()` (đã có cookie) → fetch → loadHtml.

**P23.3 — Giữ 2 paths song song** — In-process dùng `HttpClient` (đã có cookie support), daemon dùng `Choices` file (cần mới).

### Verification P23
- Login vào 1 site → đóng app → mở lại → vẫn còn login session
- Cookie file `data/NetSurf/Choices` tồn tại sau khi app tắt

---

## 📦 Phase P24 — In-process navigation (không còn restart daemon) (2-3 ngày)

**Mục tiêu**: khi user click link hoặc back, không spawn lại daemon.

### Công việc

**P24.1 — Click link** — `NetSurfRenderer::navigate()` đã có A-button handler. Cần hook vào `NetSurfEngine`:
- Lấy `currentFocusedLink()` → `BrowserManager::openUrl(link)` → `fetchAndLoadFull(link)` (in-process) → `NetSurfEngine::loadHtml(html, link)`
- Daemon path: giữ nguyên nếu user chọn

**P24.2 — Back/Forward** — dùng `BrowserManager::m_history` (đã có sẵn). `goBack` → lấy URL cũ → `fetchAndLoadFull` lại. ~200ms thay vì 700ms.

**P24.3 — Form submit (P17 + M6)** — `NetSurfEngine::submitFocusedForm()` đã có sẵn (line 340-423). Chỉ cần test trên máy.

### Verification P24
- Click link trong welcome.html → load trang mới trong ~300ms (không còn fork)
- B back → trở về trang cũ trong ~300ms
- Form submit → POST/GET → render response

---

## 📦 Phase P25 — Tests + Polish (2-3 ngày)

**P25.1 — Mở rộng test_browser.cpp** — thêm cases:
- In-process render với renderer thật (không phải nullptr) → so sánh pixel output
- Form submit trong in-process
- CSS extension: line-height, text-decoration, italic
- QuickJS: click button → text changes, setTimeout fires

**P25.2 — On-device manual test** — test mỗi tính năng trên TrimUI Brick:
- Welcome page
- example.com
- vnexpress.net (lite)
- google.com (form)
- một page có JS minimal

**P25.3 — Update `docs/WEB_BROWSER_DONE.md`** — ghi log các P-number mới (P20-P25)

**P25.4 — Update OTA rules** — nếu in-process FULL không cần daemon, cần update `package.sh` để không còn ship `bin/netsurf` (tiết kiệm 20MB disk).

---

## 📊 Tổng kết plan

| Phase | Nội dung | Effort | Risk | Phụ thuộc |
|-------|----------|--------|------|-----------|
| **P20** | Wire in-process FULL | 3-5 ngày | Thấp (đã có tests pass) | - |
| **P21** | CSS extension | 3-5 ngày | Trung bình (libcss API) | P20 |
| **P22** | QuickJS bridge | 5-7 ngày | Cao (phức tạp nhất) | P20 |
| **P23** | Cookie persist | 2-3 ngày | Thấp | P20 |
| **P24** | Navigation perf | 2-3 ngày | Thấp | P20 |
| **P25** | Tests + polish | 2-3 ngày | Thấp | All |

**Tổng: 17-26 ngày (3.5-5 tuần)**. Có thể làm song song P21+P22+P23+P24 vì độc lập.

**Deliverables cuối cùng:**
- ✅ FULL engine chạy in-process, không cần daemon (hoặc daemon là fallback optional)
- ✅ Có JavaScript (QuickJS) — lần đầu tiên trong RomCloud
- ✅ CSS mở rộng: line-height, italic, underline, text-transform
- ✅ Cookie persist
- ✅ Navigation latency giảm 5-10x (700ms → 100ms)
- ✅ RAM giảm 50% (250MB → 100MB)
- ✅ Binary size giảm 20MB (nếu bỏ daemon)
- ✅ Tất cả 120+ test cũ pass + thêm ~30 test mới
