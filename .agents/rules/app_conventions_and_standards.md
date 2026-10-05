# RomCloud Architecture Rules & Verified Standards

Tài liệu này tổng hợp toàn bộ các quy định kiến trúc, tiêu chuẩn UI/UX, phím bấm và các tính năng ĐÃ ĐƯỢC KIỂM CHỨNG trên thiết bị TrimUI Brick.
Tất cả các Agent khi đọc hoặc chỉnh sửa mã nguồn BẮT BUỘC tuân thủ các quy tắc này. KHÔNG ĐƯỢC mất thời gian re-test lại những thứ đã được liệt kê là "Mặc định / Đã kiểm chứng" dưới đây.

---

## 1. Font chữ & Hiển thị Tiếng Việt (ĐÃ KIỂM CHỨNG - CẤM RETEST)

1. **Hỗ trợ Unicode:** Font mặc định của hệ thống TrimUI (`/usr/trimui/res/regular.ttf`) và các font được tải trong `UIManager` (`m_fontTitle`, `m_fontLarge`, `m_fontMedium`, `m_fontSmall`) đã hỗ trợ **100% đầy đủ bảng mã Unicode Tiếng Việt và các ký tự dấu thanh**.
2. **Không retest font:** Tuyệt đối không viết thêm script test font, không nghi ngờ font thiếu dấu khi thêm chuỗi tiếng Việt.
3. **Cơ chế render chuỗi:** Mọi chuỗi tiếng Việt UTF-8 đều render trực tiếp và chính xác qua `drawText()` hoặc `UiRenderer` (dùng `TTF_RenderUTF8_Blended`).
4. **Quy tắc cắt ngắn chuỗi (Truncation Rule):**
   - **TUYỆT ĐỐI KHÔNG** dùng `std::string::substr()` theo số lượng byte để cắt ngắn văn bản, vì tiếng Việt dùng 2–3 bytes cho mỗi ký tự có dấu. Việc cắt theo byte sẽ chém đứt ký tự UTF-8, gây lỗi vỡ font (`...`).
   - **BẮT BUỘC** dùng hàm đo độ rộng pixel đã có sẵn:
     ```cpp
     truncateToWidth(text, font, maxWidthPx);
     ```

---

## 2. Kích thước Màn hình & Layout Giao diện Chuẩn

Thiết bị TrimUI Brick sở hữu màn hình tỉ lệ 4:3 với độ phân giải phần cứng cố định **1024 x 768**.

1. **Hằng số kích thước:**
   - `UiTheme::APP_W = 1024`
   - `UiTheme::APP_H = 768`
   - `UiTheme::HEADER_H = 64` (Header bar từ Y = 0 đến 64)
   - `UiTheme::FOOTER_H = 53` (Footer bar từ Y = 715 đến 768)
   - **Content Area:** Y = 64 đến 715 (chiều cao khả dụng 651px)
2. **Header Bar chuẩn:**
   - Sử dụng `drawAppHeader(title, sub)` hoặc `drawHeaderStatus()`.
   - Vùng bên phải dành riêng 300px cho đồng hồ, trạng thái pin và kết nối mạng.
3. **Footer Bar chuẩn:**
   - Sử dụng `drawAppFooter({{UiTheme::PadBtn::..., "Nhãn"}, ...})`.
   - **Lưu ý tối quan trọng:** Hàm `drawAppFooter()` ĐÃ TỰ ĐỘNG VẼ NỀN VÀ ĐƯỜNG KẺ NGĂN CÁCH (`FOOTER_BG` + `FOOTER_LINE`). **Tuyệt đối KHÔNG tự viết `drawRect(0, 715, 1024, 53, ...)` thủ công đè lên footer.**

---

## 3. Màu sắc & Design System (`UiTheme`)

Sử dụng trực tiếp các hằng số màu tập trung trong `UiTheme`. Không hardcode màu RGB ngẫu nhiên khi đã có hằng số chuẩn:
- **Tập trung / Highlight:** `UiTheme::FOCUS_BG_SOFT` `{30, 58, 138, 191}` (trong suốt 25%) + blend cục bộ, viền `UiTheme::FOCUS_GLOW` `{59, 130, 246, 255}`. Chi tiết xem §9.7.
- **Card / Hộp:** `UiTheme::CARD_SOLID` `{18, 24, 34, 255}`, viền `UiTheme::CARD_BORDER` `{38, 48, 64, 255}`.
- **Hàng / Row:** `UiTheme::ROW_BG` `{22, 28, 38, 255}`.
- **Văn bản:** `UiTheme::TEXT_MAIN` `{255, 255, 255, 255}`, `UiTheme::TEXT_SUB` `{148, 163, 184, 255}`, `UiTheme::TEXT_DIM` `{100, 116, 139, 255}`.
- **Màu điểm nhấn:** `UiTheme::ACCENT_CYAN` `{0, 180, 216, 255}`, `UiTheme::ACCENT_GREEN` `{34, 197, 94, 255}`, `UiTheme::ACCENT_RED` `{239, 68, 68, 255}`.

---

## 4. Quy ước Phím bấm Chuẩn (Gamepad Mapping)

Mọi màn hình và tính năng phải tuân thủ chuẩn điều khiển vật lý sau:
- **Nút B (Back / Thoát):** Luôn có chức năng **Lùi lại / Thoát về menu trước / Hủy bỏ**. Tuyệt đối KHÔNG gán nút B làm nút xóa ký tự (Backspace) trong bàn phím ảo hoặc ô tìm kiếm (tránh gây kẹt người dùng không thể thoát màn hình).
- **Nút A (Confirm):** Chọn / Vào thư mục / Kích hoạt / Bắt đầu.
- **Nút X:** Hành động phụ (Cắt / Đổi nguồn / Bộ lọc / Tìm kiếm).
- **Nút Y:** Xóa ký tự (Backspace trong bàn phím ảo) / Chép / Tải lại (Refresh).
- **Nút START:** Bắt đầu / Xác nhận tìm kiếm / Lưu cấu hình.
- **Nút SELECT:** Menu phụ / Yêu thích.

---

## 5. Chuẩn Hàng Nút Cằm TrimUI Brick & Icon (Chin Buttons Standard)

1. **Thứ tự vật lý từ trái sang phải:** **MENU - SELECT - START**.
2. **Mã phím vật lý:**
   - Trái (`GUIDE` / Joy 8): `Button::MENU`.
   - Giữa (`BACK` / Joy 6): `Button::SELECT`.
   - Phải (`START` / Joy 7): `Button::START`.
   - **Tuyệt đối KHÔNG hoán đổi BACK và GUIDE.**
3. **Quy định Icon phím:**
   - Sử dụng các icon chuyên biệt: `assets/button_icons/MENU.png`, `SELECT.png`, `START.png`.
   - **Tuyệt đối KHÔNG sử dụng các icon generic legacy:** `options.png`, `view.png`, `start_icon.png`.
4. **Footer Hints:**
   - Đặt `SELECT` trước `START` trong danh sách nút để khớp đúng vị trí thị giác trên máy.

---

## 6. GameCast & Streaming (ĐÃ KIỂM CHỨNG)

1. **Cổng mạng (Port):** Luôn là cổng **8090** (URL: `http://<IP>:8090/cast`). Cấm đổi về 6666 vì Chrome/Edge chặn với mã lỗi `ERR_UNSAFE_PORT`.
2. **Độ phân giải & Tốc độ mặc định:**
   - Mặc định là **512x384 @ 60 FPS** (Turbo Mode, integer downscale 2x từ 1024x768).
   - Đã kiểm chứng thực tế trên chip Cortex-A53: 512x384 chỉ mất 15.8ms/frame -> đạt **60 FPS mượt mà**, gói tin ~10–12KB, không làm quá tải chip Wi-Fi XRadio XR829.
   - Không được để mặc định 1024x768 Native (vì CPU chỉ nén tối đa 14 FPS và frame 90KB gây nghẽn Wi-Fi, drop frame liên tục).
3. **Bộ đệm mạng (Anti-Buffer Bloat):**
   - Socket buffer cố định `SO_SNDBUF = 32768` (32KB). Tuyệt đối không tăng lên 256KB vì sẽ làm dồn 15-20 frame cũ trong nhân Linux, gây độ trễ tích lũy từ 300ms đến 1 giây.
4. **Không tạo tay cầm ảo khi khởi động:**
   - Cấm tự tiện gọi `VirtualGamepad::init()` (`/dev/uinput`) khi daemon vừa bật. Chỉ khởi tạo lazy khi có lệnh bấm phím từ web gửi về. Việc tạo uinput sớm sẽ cướp quyền Player 1 của tay cầm vật lý TrimUI và gây crash trình quản lý phím `trimui_inputd` khi vào game.
5. **Công tắc phần cứng FN:** GPIO 243. Gạt xuống = kill triệt để daemon ngầm.
6. **Ưu tiên CPU:** Khởi chạy daemon với `nice -n 10` để giả lập game luôn được ưu tiên 100% CPU.

---

## 7. Thẻ nhớ & Hệ Thống Tệp (`/mnt/SDCARD`)

1. Đường dẫn gốc thẻ nhớ luôn là: `/mnt/SDCARD`.
2. Đường dẫn ROM: `/mnt/SDCARD/Roms/<SYSTEM_CODE>/`.
3. Đường dẫn Giả lập: `/mnt/SDCARD/Emus/<SYSTEM_CODE>/`.
4. **Bảo vệ thẻ nhớ:** Luôn thực thi lệnh `sync` sau khi ghi file quan trọng hoặc cập nhật nhị phân để bảo vệ cấu trúc phân vùng FAT32/exFAT, tránh việc kernel Linux tự động khóa thẻ vào chế độ Read-Only khi máy bị tắt đột ngột.

---

## 8. Quy trình Biên dịch & Nạp vào thiết bị

1. **Biên dịch:**
   ```bash
   ./build.sh
   ```
   (Sử dụng công cụ Zig C++ với target `aarch64-linux-gnu.2.33`).
2. **Nạp vào máy TrimUI Brick:**
   ```bash
   adb push bin/RomCloud /mnt/SDCARD/Apps/RomCloud/bin/RomCloud
   adb push bin/gamecast_d /mnt/SDCARD/Apps/RomCloud/bin/gamecast_d
   adb shell "chmod +x /mnt/SDCARD/Apps/RomCloud/bin/RomCloud /mnt/SDCARD/Apps/RomCloud/bin/gamecast_d && sync"
   ```

---

## 9. Chuẩn căn Layout / Padding / Spacing (BẮT BUỘC)

Mọi màn hình mới/sửa đều tuân thủ token trong `src/ui/UiTheme.h`
(Single Source of Truth). Cấm hardcode số lẻ tẻ khi đã có token.

### 9.1. Khung màn hình (1024x768, 4:3)
- `APP_W = 1024`, `APP_H = 768`.
- Header: `HEADER_H = 64` (Y 0..64), vẽ bằng `drawAppHeader()` — gồm bar
  nền, line separator, tiêu đề cyan + cụm Wi-Fi/pin/giờ bên phải.
- Footer: `FOOTER_Y = 715`, `FOOTER_H = 53`, vẽ bằng `drawAppFooter()`
  (tự vẽ nền + line, cấm vẽ rect đè).
- Vùng content: Y = 64..715. Card full nội dung bắt đầu `contentTop = 124`,
  cách footer 16px (`contentH = 715 - contentTop - 16`).

### 9.2. Card & lề
- Lề ngoài card: **24px** hai bên (`x = 24`, rộng `1024 - 48`).
- 2 cột: trái `x = 24`, phải `x = 600` (rộng 400), gap giữa 16px.
- Bo góc: card `RADIUS_CARD = 12`, hàng `RADIUS_ROW = 10`,
  nút `RADIUS_BTN = 10`, modal `RADIUS_MODAL = 16`.
- Borderless là hướng thiết kế: card/panel mới KHÔNG vẽ viền
  (`drawRoundedBorder`) trừ khi user yêu cầu giữ; chỉ giữ nền khối
  để phân vùng.

### 9.3. Padding trong khối
- Tiêu đề khối: cách trái **20-28px**, cách đỉnh khối **12-18px**.
- Dòng nội dung: cách trái **20px** (dot trang trí ở **x+28**, chữ ở **x+42**).
- Dòng phải (số lượng, mã): right-align, cách mép phải card **20px**.
- Khoảng cách footer hints: `FOOTER_GAP = 8`, giữa các hint
  `FOOTER_HINT_GAP = 28`.

### 9.4. Spacing dòng & hàng
- Hàng list chuẩn: **44-48px** (text small), text cách nhau **26px** trong
  list đặc, tiêu đề cách dòng nội dung **28-34px**.
- Lịch tháng: `cellH = 34`, header ngày cách lưới **28-30px**,
  chấm sự kiện r=4 (đỏ) dưới số, dot hôm nay r=14-17 (cyan) sau số.
- Không để hàng cuối đè footer: tính `visibleRows` từ chiều cao còn lại,
  clamp scroll, scrollbar khi tràn.

### 9.5. Font (NotoSans-Regular, đã full Unicode Việt)
- Title 42 / Large 36 / Medium 30 / Small 24. Tiêu đề trang = Large cyan,
  tiêu đề khối = Medium trắng, nội dung = Small, chú thích = Small xám.
- Cấm `substr()` cắt chuỗi có dấu — luôn dùng
  `truncateToWidth(text, font, maxWidthPx)`.
- OSD phủ lên video (mpv overlay): chữ tiếng Việt CÓ dấu bình thường;
  hint dùng ký tự đã verify trong font (← → có; ◀ ▶ ❚ không có trong
  NotoSans — chỉ dùng qua mpv show-text có fontconfig fallback).

### 9.6. Quy tắc phủ mờ & popup
- Popup/modal phải `beginModalDim()` + `drawModalDialog()`, nút A chọn,
  B đóng, có hint `A Chọn • B Đóng` trong hộp.
- Toast là kênh báo trạng thái duy nhất (màu: xanh thành công,
  đỏ lỗi, vàng đang xử lý), không dùng dialog cho thông báo thường.

### 9.7. Highlight chọn (selection)
- Mọi ô chọn (list/menu rows, cells, radio rows) dùng `drawHighlight()`
  hoặc `drawFocusRow()`/`drawRow(focused)` — KHÔNG `drawRect` đặc.
- Màu fill `UiTheme::FOCUS_BG_SOFT` `{30, 58, 138, 191}` = trong suốt 25%,
  vẽ kèm `SDL_BLENDMODE_BLEND` cục bộ (bật trước, trả lại sau draw).
- Hình chữ nhật highlight phải **căn giữa dọc** theo row/khối chứa nó:
  slot cao H, highlight cao h → `y = rowY + (H - h) / 2`
  (VD row 46/highlight 40 → +3; row 34/highlight 32 → +1).

---

## 10. Hệ phân cấp Layout (nguyên tắc viewport > card > khối > cột/hàng)

Mô hình duy nhất cho mọi trang, từ ngoài vào trong:

```
Màn hình 1024x768
├── Header global (0..64) + Footer global (715..768) — drawAppHeader/drawAppFooter
└── Viewport: x 24..1000 (lề 24), y 124..699 (trên cách tab/title-zone 20px,
    dưới cách footer 16px). Trang không tab cũng giữ y=124 cho đồng nhất.
    └── Card (nền khối, borderless mặc định)
        └── Khối (section, CÓ hoặc KHÔNG nền/viền — dev quyết từng chỗ)
            └── Cột / Hàng nội dung
```

### 10.1. Padding (mép ngoài → vào trong)
- Viewport → Card: lề trái/phải **24px** (card x=24, rộng 976).
- Card → Khối/nội dung: padding trái/phải **20-28px** (chuẩn 24),
  padding trên **12-18px** (chuẩn: tiêu đề khối +12, nội dung đầu +16).
- Card → đáy viewport: **16px**. Khối cuối → đáy card: **12-16px**.

### 10.2. Khoảng cách GIỮA các khối (block gap)
- Hai khối dọc trong cùng card/viewport: **12px** (VD: clock card →
  notes card, ảnh camera → list).
- Hai card cột cạnh nhau: gap **16px** (VD: trái x=24 w=560,
  phải x=600 w=400).
- Tab pills → content: **20px** (pills Y 72..104, content Y 124).

### 10.3. Cột trong khối (column gap)
- 2 cột trong card/viewport: gap **16px** (tổng rộng chia theo tỉ lệ
  rồi trừ gap, VD 560/400).
- 4 cột dự báo / 7 cột lịch: chia đều `(rộng - 2*pad) / n`, không gap
  thêm (căn giữa từng ô).
- Cột icon + cột chữ: icon 24-36px, gap icon→chữ **12-16px**;
  chữ→số right-align: số cách mép phải khối **20px**.

### 10.4. Hàng trong khối (row gap / line step)
- Hàng chạm (list chọn): cao **44-48px**, KHÔNG gap (liền khối,
  highlight full-width trừ lề 8px mỗi bên).
- Hàng info (ngày/icon/số, ghi chú): step **26px** (chữ small 24px + 2).
- Hàng lịch: `cellH = 34`, header ngày cách lưới **28px**.
- Tiêu đề khối → hàng đầu: **28-34px**. Hàng text thường → hàng tiếp:
  **4-6px** nếu cùng nhóm, **12px** nếu khác nhóm.

### 10.5. Quy tắc tràn (overflow)
- List dài: tính `visible` từ chiều cao còn lại, clamp scroll, vẽ scrollbar
  khi tràn. Không bao giờ để hàng cuối đè footer (giữ đáy 16px).
- Card co giãn theo nội dung thì neo 1 đầu (top HOẶC bottom), đầu còn lại
  tự do — cấm căn giữa khoảng trống khiến icon/text "trôi" xa top.
- Text dài: truncate về max width (`truncateToWidth`), không wrap trừ
  trang đọc (text viewer / changelog).

---

## 11. Nguyên tắc audit Layout (checklist mọi màn)

### 11.1. Thứ tự audit
1. **Khung trước, chi tiết sau:** 1024x768, header 64, footer 53,
   content 124..699, lề card 24, đáy cách footer 16.
2. **Neo (chống trôi):** mỗi khối phải neo 1 đầu (top HOẶC đáy).
   Khối căn giữa khoảng trống còn lại là bug tiềm ẩn — nội dung nhảy
   khi data đổi (tháng 4/6 hàng, có/không mạng, fetch xong/chưa).
   Test 2 cực đoan: nội dung cao nhất và thấp nhất.
3. **Gap (đo bằng tọa độ code, không đo mắt):** block→block 12,
   cột→cột 16, tiêu đề→nội dung 28-34, text→text 26.
4. **Chồng lấn:** đáy khối A so đỉnh khối B — âm là đè. Kiểm tra lại
   sau mỗi lần đổi font size/icon size (textHeight đổi làm vỡ spacing).
5. **Tràn:** visibleRows tính từ chiều cao còn lại, clamp scroll,
   hàng cuối không đè footer. Text dấu luôn `truncateToWidth`.
6. **Nhất quán cross-screen:** cùng pattern ở các màn phải giống số.
7. **Thiết bị thật:** audit code bắt ~70%, còn lại xem trên máy
   (font render, icon thật, mạng chậm).

### 11.2. Quy tắc vertical-center
Center **có điều kiện**, không mặc định:
- **Center khi:** khối cao CỐ ĐỊNH + nội dung cao CỐ ĐỊNH
  (giờ trong card clock, trạng thái rỗng, chữ cạnh icon, số trong ô).
- **Neo khi:** nội dung co giãn (lưới tháng, list, có/không mạng).
- Gọn: **cố định + cố định thì center, còn lại neo.**

---

## 12. Nguyên tắc Navigation (B lui + DPAD không gian)

### 12.1. Nút B = lùi 1 cấp (back-stack)
- Mọi `setState()` (trừ EXIT, trừ lặp liên tiếp) push trang hiện tại vào
  stack (tối đa 30, `UIManager::m_stateHistory`).
- `goBack()` pop trang gần nhất (bỏ trùng/EXIT) và đi qua `setState`
  bình thường (giữ side-effect), có cờ `m_suppressPush` chống push ngược.
- Modal/keyboard/popup đóng trước (ở yên trang), rồi B tiếp mới lùi trang.
- Trang home của từng khu vực B thoát app; lưới launcher B không làm gì
  (về trang trước qua stack nếu có).
- Cấm B nhảy cố định về 1 trang (VD picker B về thẳng LocalSend) —
  phải `goBack()` để về đúng nơi đã vào.

### 12.2. DPAD điều hướng không gian (card > khối > content)
- Trang nhiều khối: DPAD di chuyển focus theo HƯỚNG HÌNH HỌC, không theo
  thứ tự tab cứng. Thuật toán: từ tâm khối hiện tại, xét các khối có tâm
  nằm trong nửa mặt phẳng hướng bấm (dung sai 1/3 kích thước); cùng hàng
  /cột phải GIAO NHAU VÙNG (card ngắn vẫn qua được card dài bên cạnh);
  chấm `score = kc_chính + 2*kc_phụ`, chọn nhỏ nhất.
  (Cấm chặn bằng khoảng cách tâm cứng như `abs > 200` — sai khi card
  co giãn chiều cao.)
- Trong khối có list: lên/xuống/trái/phải duyệt content (ngày, hàng,
  báo thức); tới biên khối thì nhảy sang khối liền kề gần nhất cùng hướng.
- Khối focus KHÔNG viền — chỉ nền sáng hơn một chút
  (VD `{30,38,54}` so với `{22,28,38}`). Rect nav và rect render phải
  chung 1 nguồn (card co giãn thì nav tính lại theo).
  Hàng/con trỏ trong khối: nền highlight + chữ trắng.
- Hành động (A/Y/X/START) theo khối đang focus, footer đổi hint theo focus.
- Rect khối hardcode phải khớp code render (`wxBlockRect`/`clkBlockRect`) —
  đổi layout render thì đổi rect nav cùng lúc.
