# HƯỚNG DẪN SỬ DỤNG ROMCLOUD — TRIMUI BRICK PRO
(RomCloud User Manual)

---

## 1. TỔNG QUAN HỆ THỐNG

RomCloud biến Google Drive của bạn thành **Kho lưu trữ ROM đám mây khổng lồ** cho máy chơi game cầm tay TrimUI Brick Pro.
* Bạn có thể lưu hàng trăm Gigabyte ROMs trên Google Drive.
* Thẻ nhớ TrimUI chỉ cần lưu metadata và ảnh bìa để bạn duyệt mượt mà ở tốc độ 60 FPS.
* Khi muốn chơi trò nào, bạn chỉ cần bấm nút `[A]` để tải riêng trò chơi đó về máy và chơi ngay. Khi chơi xong hoặc thẻ nhớ đầy, bạn có thể xóa file ROM trên thẻ nhớ bất cứ lúc nào với nút `[SELECT]`, thông tin trò chơi trên đám mây vẫn được lưu trữ nguyên vẹn!

---

## 2. CHUẨN BỊ THƯ MỤC TRÊN GOOGLE DRIVE

Tạo một thư mục mang tên `RomCloud` (hoặc `Roms`) trên tài khoản Google Drive của bạn.
Bên trong thư mục này, tạo các thư mục con tương ứng với từng hệ máy chơi game theo chuẩn TrimUI:

```text
Google Drive: My Drive/
└── RomCloud/
    ├── FC/                      <-- Nintendo Entertainment System / Famicom
    │   ├── Super Mario Bros.nes
    │   └── Contra.nes
    ├── SFC/                     <-- Super Nintendo / Super Famicom
    │   └── Chrono Trigger.sfc
    ├── GBA/                     <-- Game Boy Advance
    │   ├── Pokemon Emerald.gba
    │   └── Fire Emblem.gba
    ├── PS/                      <-- Sony PlayStation 1
    │   ├── Castlevania SOTN.chd
    │   └── Tekken 3.chd
    ├── PSP/                     <-- PlayStation Portable
    │   └── Monster Hunter Portable 3rd.iso
    └── ...                      <-- Hỗ trợ tất cả 24 hệ máy TrimUI
```

> **Ghi chú định dạng file:** RomCloud hỗ trợ tất cả các định dạng file phổ biến: `.nes`, `.sfc`, `.gba`, `.gbc`, `.iso`, `.chd`, `.zip`, `.7z`, v.v.

---

## 3. KẾT NỐI GOOGLE DRIVE LẦN ĐẦU TIÊN (OAUTH 2.0 DEVICE FLOW)

1. Đảm bảo TrimUI Brick Pro đã kết nối Wi-Fi.
2. Mở ứng dụng **RomCloud** từ màn hình Apps của TrimUI.
3. Màn hình liên kết tài khoản sẽ hiển thị một mã QR và mã kết nối 8 ký tự (User Code).
4. **Cách liên kết:**
   * Dùng điện thoại thông minh quét mã QR hiển thị trên màn hình TrimUI.
   * Hoặc mở trình duyệt trên máy tính/điện thoại: truy cập [google.com/device](https://www.google.com/device) và nhập mã 8 ký tự.
5. Nhấn **Allow / Cho phép** để cấp quyền đọc thư mục ROM trên Google Drive.
6. Ứng dụng TrimUI sẽ tự động phát hiện thành công, lưu Refresh Token bảo mật vào cơ sở dữ liệu và chuyển thẳng vào giao diện thư viện game!

---

## 4. HƯỚNG DẪN THAO TÁC TRÊN TAY CẦM TRIMUI

| Nút bấm | Chức năng | Mô tả chi tiết |
| :--- | :--- | :--- |
| **D-Pad Lên / Xuống** | Di chuyển danh sách game | Lướt chọn game trong hệ máy hiện tại |
| **D-Pad Trái / Phải** | Đổi hệ máy | Chuyển đổi giữa FC, SFC, GBA, PS, PSP, MD, Arcade... |
| **L1 / R1** | Lật trang nhanh | Cuộn nhanh qua 10 game một lần |
| **Nút [A]** | Tải về / Chơi ngay | • Nếu game ở trên Đám mây: Bắt đầu tải về máy.<br>• Nếu game đã có trên thẻ: Khởi động trình giả lập để chơi. |
| **Nút [B]** | Quay lại / Hủy bỏ | Hủy hộp thoại xác nhận hoặc thoát màn hình cài đặt |
| **Nút [X]** | Lọc thư viện | Chuyển đổi qua 3 chế độ xem:<br>• `ALL`: Toàn bộ game (Cả máy + Đám mây)<br>• `LOCAL ONLY`: Chỉ xem các game đã tải trên thẻ nhớ<br>• `CLOUD ONLY`: Chỉ xem các game còn trên đám mây |
| **Nút [Y]** | Đồng bộ Đám mây | Quét lại Google Drive, cập nhật danh sách game mới tải lên mà không làm mất game cục bộ |
| **Nút [SELECT]** | Xóa bộ nhớ đệm ROM | Xóa file game cục bộ để giải phóng dung lượng thẻ nhớ (vẫn giữ metadata trên đám mây) |
| **Nút [START]** | Cài đặt / Trợ giúp | Xem thông tin dung lượng thẻ nhớ, tài khoản, hủy liên kết Drive |
| **Nút [MENU]** | Thoát ứng dụng | Lưu trạng thái an toàn và quay về giao diện chính TrimUI |

---

## 5. CƠ CHẾ TẢI VỀ & BẢO TOÀN DỮ LIỆU

* **Kiểm tra dung lượng thông minh:** Trước khi tải bất kỳ file nào, RomCloud tự động kiểm tra dung lượng trống trên thẻ nhớ (`/mnt/SDCARD`). Nếu thẻ không đủ chỗ, máy sẽ thông báo rõ ràng thay vì làm crash hệ thống.
* **Tải an toàn qua file tạm (.part):** Mọi bản tải về được ghi tạm thời vào file `.part`. Sau khi tải xong 100%, hệ thống tự động kiểm tra mã băm MD5 với Google Drive. Nếu toàn vẹn, file sẽ được chuyển nguyên tử vào thư mục ROM chính xác của TrimUI (`/mnt/SDCARD/Roms/<SYSTEM>/`).
* **Trạng thái hiển thị rõ ràng:**
  * Biểu tượng màu **Xanh lam [CLOUD]**: Game lưu trên Google Drive.
  * Biểu tượng màu **Xanh lục [LOCAL]**: Game đã có trên máy, sẵn sàng chơi ngay mà không cần mạng.
