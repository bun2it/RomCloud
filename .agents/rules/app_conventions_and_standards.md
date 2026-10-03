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
- **Tập trung / Highlight:** `UiTheme::FOCUS_BG` `{30, 58, 138, 255}`, viền `UiTheme::FOCUS_GLOW` `{59, 130, 246, 255}`.
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
