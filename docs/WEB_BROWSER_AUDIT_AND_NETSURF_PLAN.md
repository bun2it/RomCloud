# Plan: Audit `src/browser/` và Chuẩn Bị Sửa (Tham chiếu NetSurf)

> **Liên quan:**
> - Tài liệu gốc: [`NETSURF_INTEGRATION_PLAN.md`](./NETSURF_INTEGRATION_PLAN.md) — kiến trúc & UI integration.
> - Quy tắc dự án bắt buộc tuân thủ: [`.agents/rules/app_conventions_and_standards.md`](../.agents/rules/app_conventions_and_standards.md).
> - Nguồn NetSurf tham chiếu:
>   - Local source: `/Users/tai/Downloads/netsurf-all-3.11/`
>   - GitHub: `https://github.com/netsurf-browser/netsurf`
>   - Git canonical: `https://git.netsurf-browser.org/netsurf.git/`
>   - Quick build docs: `https://git.netsurf-browser.org/netsurf.git/plain/docs/quick-start.md`

---

## 1. Bối cảnh & Mục tiêu

Code `src/browser/` hiện tại là một **HTML renderer mini tự viết** dùng cho Wi-Fi portal và basic pages. Báo cáo audit trước đó phát hiện **4 bug nghiêm trọng** (data race, dangling pointer, history lặp, scroll chết) cùng **8 vấn đề quan trọng** (parse HTML cực tối giản, overdraw header, không có submission form, v.v.).

Tài liệu này:
1. Tổng hợp lại các phát hiện audit.
2. Đối chiếu từng bug với **cách NetSurf giải quyết** (pattern tham chiếu).
3. Đề xuất **lộ trình sửa theo từng phase**, tập trung vào fix trước, tích hợp NetSurf sau.

**Triết lý**: Giữ `HtmlRenderer` cho Wi-Fi portal (lightweight), nhưng fix bug + harden trước khi dùng; **chưa vội link NetSurf** vì binary size + dependency cost.

---

## 2. Tổng hợp Audit `src/browser/`

### 2.1. Files audit
| File | LOC | Build status |
|------|-----|--------------|
| `src/browser/BrowserManager.h` | 62 | ✓ có trong `build.sh:100` |
| `src/browser/BrowserManager.cpp` | 101 | ✓ có trong `build.sh:101` |
| `src/browser/HtmlRenderer.h` | 142 | n/a (header) |
| `src/browser/HtmlRenderer.cpp` | 548 | ✓ |
| `src/ui/UIManager.cpp` (lines 6930–7175) | ~245 | ✓ — UI integration |

### 2.2. Bảng tổng hợp bug

| ID | Mức | Vị trí | Mô tả ngắn |
|----|------|--------|-----------|
| **C1** | 🔴 Critical | `HtmlRenderer.cpp:59-66` | `s_url` static trong lambda → race condition khi gọi `loadUrl` liên tiếp |
| **C2** | 🔴 Critical | `HtmlRenderer.cpp:221, 250` | `m_focusable.push_back(&m_elements.back())` → dangling pointer khi `m_elements` realloc |
| **C3** | 🔴 Critical | `HtmlRenderer.cpp:71-89` | `onFetchComplete` chạy trên worker thread nhưng ghi vào non-atomic state → data race |
| **C4** | 🔴 Critical | `HtmlRenderer.cpp:96-102` | `goBack()` gọi `loadUrl()` → loadUrl push history lần nữa → history trùng lặp |
| **M1** | 🟠 Major | `HtmlRenderer.cpp:436-442` | `render()` vẽ full màn hình 0,0,1024,768 → đè header (Y=0..64) của UIManager |
| **M2** | 🟠 Major | toàn file | Có `m_scrollY` nhưng không có input bind scroll → scroll chết |
| **M3** | 🟠 Major | `HtmlRenderer.cpp:21-29` | `init()` không reset `m_inputIndex`, `m_maxScroll`, `m_loading`, `m_error`, `m_errorMsg` |
| **M4** | 🟠 Major | `HtmlRenderer.cpp:114,116,123,137,267,269` | `std::isspace(char)` với byte UTF-8 âm → UB |
| **M5** | 🟠 Major | `HtmlRenderer.cpp:432-434` | `submitForm()` rỗng — không implement POST form |
| **M6** | 🟠 Major | `BrowserManager.cpp:25` | Không validate URL scheme (`javascript:`, `file:`, v.v.) |
| **M7** | 🟠 Major | `HttpClient::get()` | Không giới hạn response size → OOM risk |
| **M8** | 🟠 Major | `parseHtml()` | Không nhận `<script>`, `<style>`, `<table>`, `<!-- -->`, attribute không quote |

Chi tiết đầy đủ đã có trong session trước, không lặp lại ở đây.

---

## 3. Tham chiếu NetSurf — Pattern Giải Quyết

NetSurf giải quyết cùng các vấn đề trên bằng các pattern đã mature qua 15 năm phát triển. Đây là tham chiếu cụ thể cho từng bug class:

### 3.1. Threading & Async Fetch → NetSurf: `netsurf/content/llcache.c` + `netsurf/content/fetch.c`

NetSurf có **3 lớp async rất rõ ràng**:
1. **Low-level cache (`llcache.c`)**: Lưu trữ raw HTTP response (header + body stream). Worker thread fetch qua libcurl.
2. **High-level cache (`hlcache.c`)**: Mapping URL → content handler.
3. **Browser window (`browser_window.c`)**: Nhận callback `bw->callback()` từ content layer — **callback chạy trên main thread** (NetSurf dùng `schedule.c` để đẩy event về main loop).

So sánh với code hiện tại của ta:
```cpp
// Hiện tại (HtmlRenderer.cpp:61-66): gọi trực tiếp từ worker thread, không qua main loop
pthread_create(&tid, nullptr, [](void*)->void* {
    HttpResponse resp = HttpClient::instance().get(s_url, {}, 15);
    HtmlRenderer::instance().onFetchComplete(resp);  // ← C3: race
    return nullptr;
}, nullptr);
```

**Pattern NetSurf áp dụng** — tham khảo `netsurf/frontends/framebuffer/schedule.c` (NetSurf dùng `nsfb_event` để post event về main loop, tương tự như SDL_PushEvent):
```c
// Pseudo-code từ NetSurf framebuffer
static void schedule_run(void)
{
    while (scheduled != NULL) {
        schedule_entry *entry = scheduled;
        scheduled = entry->next;
        entry->callback(entry->pw);  // ← luôn chạy trên main thread
    }
}
```

**→ Áp dụng fix cho C1 + C3**: Dùng `SDL_PushEvent(SDL_USEREVENT)` để đẩy kết quả về main thread; capture URL bằng heap pointer trong `event.user.data1` thay vì static `s_url`.

### 3.2. DOM Tree Storage (giải quyết C2 dangling pointer) → NetSurf: `libdom/`

NetSurf dùng **libdom** — W3C DOM implementation đầy đủ bằng C, với:
- `dom_node` là struct cố định (không phải vector<HtmlElement>).
- Duyệt cây qua `dom_node_get_first_child()`, `dom_node_get_next_sibling()` — **stable iteration** bất kể tree grow.
- Reference counting: mỗi node có refcount, free khi về 0.

Tham khảo file: `/Users/tai/Downloads/netsurf-all-3.11/libdom/include/dom/core/node.h` — định nghĩa `dom_node`, `dom_node_get_parent_node`, `dom_node_get_first_child`, v.v.

```c
/* libdom/include/dom/core/node.h (rút gọn) */
typedef struct dom_node dom_node;
dom_exception dom_node_get_first_child(dom_node *node, dom_node **result);
dom_exception dom_node_get_next_sibling(dom_node *node, dom_node **result);
dom_exception dom_node_get_parent_node(dom_node *node, dom_node **parent);
```

**→ Áp dụng fix cho C2**: Hai lựa chọn:
1. **Quick fix**: Đổi `std::vector<HtmlElement>` → `std::list<HtmlElement>` (stable iterator, nhưng tốn memory hơn).
2. **Đúng đắn**: Thay bằng linked list hoặc intrusive list tự viết, hoặc lưu `size_t index` thay vì pointer.

**Khuyến nghị**: Chọn option 1 (quick), vì số element trong Wi-Fi portal thường < 50, memory overhead không đáng kể.

### 3.3. Navigation & History → NetSurf: `netsurf/desktop/browser_history.c`

NetSurf quản lý history qua **`struct history`**: doubly-linked list các entry (mỗi entry là một entry back/forward).

Tham khảo file: `/Users/tai/Downloads/netsurf-all-3.11/netsurf/desktop/browser_history.c` (~700 LOC).

API chính:
```c
struct history_entry {
    struct nsurl *url;            // URL không có fragment
    char *title;                  // page title
    // ... children for frames, scroll position, etc.
};

nserror history_add(struct history *history, struct hlcache_handle *content);
nserror history_back(struct history *history, struct history_entry **entry);
nserror history_forward(struct history *history, struct history_entry **entry);
```

Quan trọng: **`history_add` chỉ được gọi khi user navigate tới URL mới** (qua button/link), không phải khi back/forward/reload.

Áp dụng vào code ta:
```cpp
// Hiện tại (HtmlRenderer.cpp:96-102):
void HtmlRenderer::goBack() {
    if (canGoBack()) {
        m_historyPos--;
        std::string url = m_history[m_historyPos];
        loadUrl(url);              // ← loadUrl cũng push history → duplicate
    }
}
```

**Pattern NetSurf**: Tách thành 2 hàm private:
```cpp
// private helper — không push history
bool loadUrlNoHistory(const std::string& url);

// public
bool loadUrl(const std::string& url) {           // ← push history
    pushHistory(url);
    return loadUrlNoHistory(url);
}
void goBack() {                                    // ← không push
    if (!canGoBack()) return;
    m_historyPos--;
    loadUrlNoHistory(m_history[m_historyPos]);
}
```

### 3.4. HTML Parsing & Tokenizer → NetSurf: `libhubbub`

Hiện tại `HtmlRenderer::parseHtml` (~90 LOC) chỉ hiểu được:
- Tag đơn giản, không có nested edge case
- Attribute có quote (`"` hoặc `'`)
- Không hiểu `<script>`, `<style>`, `<table>`, comments

NetSurf dùng **libhubbub** — full HTML5 tokenizer + tree builder, ~12 500 LOC tổng (`/Users/tai/Downloads/netsurf-all-3.11/libhubbub/src/`).

Đáng chú ý:
- `hubbub_parser_create()` → tạo parser instance.
- `hubbub_parser_parse_chunk(parser, buffer, len)` → push HTML chunk.
- `hubbub_parser_completed(parser)` → flush state cuối.
- Callback `token_handler` được invoke cho mỗi start tag / end tag / text / comment.

API reference: `/Users/tai/Downloads/netsurf-all-3.11/libhubbub/include/hubbub/parser.h`.

**Đánh giá cho giai đoạn này**:
- Link libhubbub + libdom + libcss vào RomCloud sẽ tốn ~5–10 MB binary (xem §3.7).
- Đối với Wi-Fi portal HTML đơn giản, **không cần libhubbub**.
- **Quyết định**: Ở phase fix, **chỉ vá các lỗ hổng parsing dễ** (comment, attribute không quote, bỏ qua `<script>`/`<style>` content), không link libhubbub.

### 3.5. Framebuffer Frontend → NetSurf: `netsurf/frontends/framebuffer/`

Đây là **frontend gần nhất** với kiểu SDL2 của RomCloud. NetSurf tách renderer backend qua abstract `plotter` interface (`netsurf/include/netsurf/plotters.h`):
```c
typedef struct {
    nserror (*clip)(const struct redraw_context *ctx, const struct rect *clip);
    nserror (*rectangle)(...);
    nserror (*line)(...);
    nserror (*polygon)(...);
    nserror (*path)(...);
    nserror (*text)(...);
    nserror (*bitmap)(...);
    nserror (*arc)(...);
    // ...
} plotter_table_t;
```

Mỗi frontend (gtk, framebuffer, monkey, amiga, riscos, v.v.) implement một `plotter_table_t`. Framebuffer frontend (`netsurf/frontends/framebuffer/framebuffer.c`, ~1000 LOC) implement plotter trên `libnsfb` (NetSurf's framebuffer lib, hỗ trợ SDL, X11, wayland, VNC).

**Tham khảo cho Phase 4** (nếu sau này tích hợp NetSurf thật):
- `netsurf/frontends/framebuffer/framebuffer.c` — plotters (~250 LOC mỗi plotter).
- `netsurf/frontends/framebuffer/gui.c` — main loop, ~1500 LOC.

### 3.6. URL Validation & Scheme Whitelisting → NetSurf: `netsurf/content/fetch.c`

NetSurf kiểm tra scheme ngay trong fetcher registration (`netsurf/content/fetchers/`). Mỗi scheme có fetcher riêng:
- `http_fetch.c` — http/https
- `file_fetch.c` — file
- `data_fetch.c` — data URI
- `about_fetch.c` — `about:` internal

Fetcher factory match scheme và reject không match:
```c
/* Pseudo */
static bool fetch_about_init(void)
{
    return fetch_add_fetcher("about",
        about_fetch_handler, /* ... */);
}
```

**Áp dụng cho M6** trong `BrowserManager::openUrl()`:
```cpp
bool BrowserManager::openUrl(const std::string& url) {
    // Whitelist scheme
    if (url.compare(0, 7, "http://") != 0 &&
        url.compare(0, 8, "https://") != 0) {
        m_errorMsg = "Chi ho tro http/https";
        m_state = BrowserState::ERROR;
        return false;
    }
    // ...
}
```

### 3.7. HTTP Safety (Size Limit) → NetSurf: `netsurf/content/llcache.c` + curl options

NetSurf dùng libcurl với:
```c
curl_easy_setopt(handle, CURLOPT_MAXFILESIZE_LARGE,
                 (curl_off_t)option_max_fetch_size);
```

`option_max_fetch_size` mặc định ~10 MB, có thể config qua NetSurf options.

**Áp dụng cho M7** trong `HttpClient`:
```cpp
// Trong HttpClient::get():
curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, 2 * 1024 * 1024L);  // 2 MB
```

## 4. Tham chiếu `libnsfb` (lựa chọn thay thế nếu không dùng SDL)

File: `/Users/tai/Downloads/netsurf-all-3.11/libnsfb/README`:
```
The following libraries may also be installed:
  +  SDL 1.2 (for the SDL surface)
  +  libxcb* (for the X11 surface)
  +  wayland-client (for wayland surface)
  +  libvncserver (for vnc surface)
```

libnsfb source: 300 KB, cung cấp plotter API tương tự SDL nhưng nhẹ hơn (compile-time chọn backend). **Chỉ tham khảo**, không có kế hoạch tích hợp trong giai đoạn này.

---

## 5. Kế hoạch Sửa (Phased)

### Phase 0 — Quick Wins (1–2 giờ) ✅ ưu tiên
| ID | Hành động | File | Effort |
|----|-----------|------|--------|
| **M3** | Reset full state trong `init()` | `HtmlRenderer.cpp:21-29` | 5 ph |
| **M4** | Cast `(unsigned char)` cho tất cả `std::isspace(html[pos])` | `HtmlRenderer.cpp` nhiều nơi | 10 ph |
| **N1–N4** | Cleanup dead code (NETSURF branch, redundant calls) | `BrowserManager.cpp` | 15 ph |
| **N7** | Gọi `updateFocus()` sau khi clear `m_editingText` | `HtmlRenderer.cpp:378-383` | 5 ph |
| **N20** | Không log form action URL nguyên văn (che credentials) | `HtmlRenderer.cpp:432-434` | 5 ph |

### Phase 1 — Critical Bug Fixes (3–5 giờ) 🚨 **PHẢI LÀM TRƯỚC**
| ID | Hành động | Pattern tham chiếu | File |
|----|----------|--------------------|------|
| **C1** | Capture URL vào lambda bằng `std::unique_ptr<std::string>` | xem §3.1 | `HtmlRenderer.cpp:59-66` |
| **C2** | Đổi `m_elements` → `std::list<HtmlElement>` (hoặc dùng `size_t index`) | xem §3.2 | `HtmlRenderer.h:120-121, .cpp:221, 250, 252, 275-276` |
| **C3** | Post fetch completion về main thread qua `SDL_PushEvent(SDL_USEREVENT)` | xem §3.1 | `HtmlRenderer.cpp:61-89` + `UIManager.cpp:6948` |
| **C4** | Tách `loadUrlNoHistory()` private + `loadUrl()` chỉ push history | xem §3.3 | `HtmlRenderer.cpp:96-102` |

### Phase 2 — UX/Stability (3–4 giờ) 🟠
| ID | Hành động | File |
|----|-----------|------|
| **M1** | Constrain `render()` vào content area Y=64..715, HEIGHT=651 | `HtmlRenderer.cpp:436-442` |
| **M2** | Bind L1/R1 (hoặc L2/R2) cho scroll page; auto-scroll focus vào viewport | `HtmlRenderer.cpp` + `UIManager.cpp:7070-7079` |
| **M6** | Whitelist http/https scheme trong `BrowserManager::openUrl` | `BrowserManager.cpp:25` |
| **M7** | Thêm `CURLOPT_MAXFILESIZE_LARGE = 2MB` trong `HttpClient` | `HttpClient.cpp` |
| **M8a** | Bỏ qua `<script>`, `<style>` content trong parser | `HtmlRenderer.cpp:189-280` |
| **M8b** | Hỗ trợ comment `<!-- ... -->` | `HtmlRenderer.cpp:189-280` |
| **M8c** | Hỗ trợ attribute không quote `<input type=text>` | `HtmlRenderer.cpp:142-145` |

### Phase 3 — Form Submission (2–3 giờ) 🟠
| ID | Hành động |
|----|-----------|
| **M5** | Implement `submitForm()` thật: dùng `HttpClient::postForm()` với `application/x-www-form-urlencoded` |
| | Cần update `HttpClient::postForm()` nếu chưa có |
| | Update `parseHtml()` để nhận `<form action method>` + thu thập `<input name value>` |
| | Bind A trên `<button type="submit">` để trigger `submitForm` |

### Phase 4 — NetSurf Integration (TƯƠNG LAI, không nằm trong audit fix này)
> Chỉ thực hiện khi:
> 1. Phase 0–3 ổn định.
> 2. Có bandwidth build NetSurf ARM64.
> 3. Có yêu cầu thực tế cho full browsing (Wi-Fi portal không cần).
>
> File tham chiếu:
> - Build script: `https://git.netsurf-browser.org/netsurf.git/plain/docs/env.sh`
> - Frontend template: `netsurf/frontends/framebuffer/` (~3000 LOC, gần SDL nhất).
> - Cần link: `libparserutils`, `libwapcaplet`, `libhubbub`, `libdom`, `libcss`, `libnsfb`.
> - Size ước tính: +5–10 MB binary, +15 MB libs.

---

## 6. Build & Deploy

Theo **AGENTS.md** section "Build & Deploy":
```bash
# Build
./build.sh

# Deploy
adb push bin/RomCloud /mnt/emmcrun0/RomCloud/RomCloud
adb push bin/gamecast_d /mnt/emmcrun0/RomCloud/gamecast_d   # nếu có
adb shell sync
```

Mỗi phase phải:
1. Build OK (không warning mới).
2. Smoke test trên thiết bị: `Wi-Fi → Captive portal → render form → submit → back`.

---

## 7. Verification Checklist (sau mỗi Phase)

- [ ] **C1 fix**: Spam URL 10 lần liên tiếp, log đúng URL mỗi lần (không có URL bị "lẫn").
- [ ] **C2 fix**: Trang có 50+ input → submit → không crash, không có `valgrind`/ASan error.
- [ ] **C3 fix**: `ThreadSanitizer` pass; navigation 100 lần không race.
- [ ] **C4 fix**: Trang A → B → C → back → back → forward đúng trang C.
- [ ] **M1 fix**: Browser content không che header "WEB BROWSER" / footer hints.
- [ ] **M2 fix**: Trang dài 2000px scroll được bằng L1/R1.
- [ ] **M6 fix**: `openUrl("javascript:alert(1)")` bị reject.
- [ ] **M7 fix**: Response 100 MB bị abort sau 2 MB.
- [ ] **M8 fix**: Trang có `<script>alert(1)</script>` render không vỡ layout.

---

## 8. Câu hỏi mở (cần user xác nhận)

1. **Có cần tích hợp NetSurf thật không?** → ROM TrimUI Brick có ~512 MB RAM, binary NetSurf framebuffer build cho ARM64 khoảng ~8 MB + libs ~15 MB. Nếu chỉ dùng cho Wi-Fi portal, không cần.
2. **Form POST cần thiết không?** → Nếu portal chỉ dùng GET query, M5 có thể bỏ qua.
3. **HTTPS có cần không?** → Hiện `HttpClient` có support, nhưng cần `libssl` linked. Nếu portal HTTP thuần, không cần.
4. **VirtualKeyboard cho input field**: Hiện `HtmlRenderer::typeCharacter(char)` có nhưng UIManager không gọi. Cần wire-up.

---

## 9. Tóm tắt

| Hạng mục | Hiện trạng | Sau Phase 0–3 |
|----------|-----------|---------------|
| Critical bugs | 4 | 0 |
| Major issues | 8 | 0 |
| Build size | +5–8 KB (2 .cpp) | +5–10 KB |
| Binary impact | 0 | +0 (chỉ refactor) |
| Wi-Fi portal | Có thể crash trên URL nhanh | Ổn định |
| Full web browsing | Không | Không (NetSurf = future) |

**Đề xuất**: Triển khai Phase 0 + Phase 1 trong 1 commit, Phase 2 + Phase 3 trong commit tiếp theo. Phase 4 (NetSurf) để dành cho major release sau.