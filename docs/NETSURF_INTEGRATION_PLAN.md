# Plan: Tích hợp Web Browser cho RomCloud

## 1. Context

TrimUI Brick Pro cần web browser để:
- **Wi-Fi captive portal login** (cần nhập thông tin trên web form)
- **General browsing**

**Approach**: Cả hai
- **Simple HTML Renderer**: Nhẹ, nhanh, cho portal và basic pages
- **NetSurf**: Đầy đủ cho general browsing với HTML/CSS/JS

## 2. Kiến trúc

```
┌─────────────────────────────────────┐
│ BrowserManager (unified interface)   │
├─────────────────────────────────────┤
│  ├─► HtmlRenderer (simple, SDL2)   │
│  └─► NetSurf (full browser)         │
└─────────────────────────────────────┘
```

### Khi nào dùng gì:
| Use case | Renderer |
|----------|----------|
| Wi-Fi Portal login | HtmlRenderer |
| Simple form/landing page | HtmlRenderer |
| Full web browsing | NetSurf |
| Complex pages with JS | NetSurf |

## 3. Các bước thực hiện

### Phase 1: Simple HTML Renderer (Ưu tiên cao)
Cho Wi-Fi portal và basic pages.

**Tạo files:**
- `src/browser/HtmlRenderer.h` - HTML parsing + SDL2 rendering
- `src/browser/HtmlRenderer.cpp` - Implementation

**Features:**
- Parse basic HTML tags: `<a>`, `<input>`, `<form>`, `<div>`, `<p>`, `<img>`
- CSS inline styling
- Virtual keyboard integration cho text input
- Button navigation (UP/DOWN/LEFT/RIGHT/A/B)
- Link following

### Phase 2: BrowserManager (Unified Interface)
```cpp
class BrowserManager {
    enum class Engine { HTML_RENDERER, NETSURF };
    Engine selectEngine(const std::string& url);
    void start(const std::string& url);
    void stop();
    void handleInput(const InputEvent& event);
    bool pollExited();
};
```

### Phase 3: NetSurf Integration (Phase 2 sau)
Build hoặc download prebuilt NetSurf ARM64.

### Phase 4: UI Integration
Sửa `src/ui/UIManager.cpp`:
- Thêm state `UIState::BROWSER`
- Render browser view
- Handle input

## 4. File Changes

### New Files:
| File | Description |
|------|-------------|
| `src/browser/HtmlRenderer.h` | Simple HTML renderer |
| `src/browser/HtmlRenderer.cpp` | Implementation |
| `src/browser/BrowserManager.h` | Unified browser interface |
| `src/browser/BrowserManager.cpp` | Implementation |
| `src/browser/VirtualKeyboard.h` | Browser-specific keyboard |

### Modified Files:
| File | Changes |
|------|---------|
| `src/ui/UIManager.cpp` | Thêm browser state/handler |
| `src/ui/UIManager.h` | Thêm browser methods |
| `src/ui/WifiUI.cpp` | Gọi BrowserManager cho portal |
| `build.sh` | Build thêm browser files |
| `package.sh` | Include browser assets |

## 5. Simple HTML Renderer Design

### HTML Tags Supported:
```
<a href="...">       - Links (navigable)
<input type="text">   - Text input
<input type="password"> - Password
<input type="checkbox"> - Checkbox
<button>            - Buttons
<form>              - Form submission
<div>, <p>, <span>  - Layout
<img src="...">     - Images (PNG/JPEG)
```

### Input Handling:
- D-PAD: Navigate between focusable elements
- A: Activate link/button, submit form
- B: Go back / close browser
- Keyboard: Type text in input fields

### Rendering:
- Parse HTML to internal DOM
- Layout engine đơn giản (top-to-bottom, left-to-right)
- Render bằng SDL2 (giống các UI elements khác)
- Scroll view cho long pages

## 6. Verification

1. **Build**: `cd ~/RomCloud && ./build.sh`
2. **Deploy**: `adb push bin/RomCloud ...`
3. **Test cases**:
   - Wi-Fi portal: Kết nối Wi-Fi → mở portal → nhập text → submit
   - Simple page: Mở URL → xem nội dung → click links
   - Navigation: Back button hoạt động

## 7. Open Questions

- [ ] NetSurf prebuilt ARM64 có sẵn ở đâu?
- [ ] Cần hỗ trợ HTTPS không? (libssl required)
- [ ] Cookie/session storage cần thiết không?
