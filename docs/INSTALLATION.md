# HƯỚNG DẪN CÀI ĐẶT ROMCLOUD CHO TRIMUI (BRICK PRO / SMART PRO)

Tài liệu hướng dẫn chi tiết dành cho người dùng lần đầu tiên cài đặt và sử dụng ứng dụng **RomCloud** trên máy TrimUI (hỗ trợ cả Stock OS, NextUI và SpruceOS).

---

## 1. Chọn phiên bản cài đặt phù hợp

Chúng tôi cung cấp 2 phương thức cài đặt trên trang [GitHub Releases](https://github.com/bun2it/RomCloud/releases/latest):

| Phiên bản | Dung lượng | Mô tả & Khuyên dùng |
| :--- | :--- | :--- |
| ⚡ **RomCloud-Lite-Installer.zip** | **~18 MB** (rất nhẹ) | **(Khuyên dùng)** Bộ cài đặt siêu nhanh. Chỉ cần giải nén copy vào thẻ nhớ. Khi mở app lần đầu trên TrimUI, app sẽ tự kiểm tra và tải các thư viện / video player qua OTA chỉ với 1 click! |
| 📦 **RomCloud-v2.0.7.zip** | **~29 MB** | **(Bản đầy đủ - Offline)** Đã tích hợp sẵn toàn bộ trình phát video MPV, bộ giải mã codecs và thư viện. Thích hợp nếu bạn không muốn hoặc chưa có kết nối Wi-Fi. |

---

## 2. Hướng dẫn cài đặt cho người dùng lần đầu (3 Bước Đơn Giản)

### Bước 1: Tải về bộ cài đặt
1. Truy cập vào [RomCloud Releases](https://github.com/bun2it/RomCloud/releases/latest).
2. Tải về tệp:
   * **`RomCloud-Lite-Installer.zip`** (Khuyên dùng) hoặc **`RomCloud-v2.0.7.zip`**.

### Bước 2: Sao chép vào thẻ nhớ SD
1. Kết nối thẻ nhớ MicroSD của máy TrimUI vào máy tính (bằng đầu đọc thẻ hoặc bật chế độ *USB Storage* trên máy TrimUI).
2. Giải nén tệp zip vừa tải. Bạn sẽ thấy thư mục `Apps`.
3. Kéo thả thư mục `Apps` vào **thư mục gốc (Root)** của thẻ nhớ:
   * Cấu trúc đường dẫn chuẩn trên thẻ nhớ sẽ là:
     ```text
     [Thẻ nhớ MicroSD]/
     └── Apps/
         └── RomCloud/
             ├── config.json
             ├── icon.png
             ├── launch.sh
             ├── bin/
             │   └── RomCloud
             ├── assets/
             ├── iptv/
             └── config/
     ```
4. Ngắt kết nối thẻ nhớ an toàn (Eject) và cắm lại vào máy TrimUI.

### Bước 3: Khởi chạy và kích hoạt lần đầu
1. Mở máy TrimUI, vào mục **Apps** (hoặc ứng dụng) trên màn hình chính:
   * Bạn sẽ thấy ngay biểu tượng logo **RomCloud** màu xanh neon nổi bật.
2. Bấm nút **A** để mở ứng dụng.
3. **Nếu dùng bản Lite:**
   * Ứng dụng sẽ tự động thông báo cài đặt các thành phần phụ trợ (Trình phát MPV & Codecs giải mã TV).
   * Bấm nút **A** để app tự tải và hoàn tất cài đặt tự động qua Wi-Fi.

---

## 3. Kết nối Google Drive & Thưởng thức ROMs

1. Kết nối máy TrimUI vào mạng **Wi-Fi** (trong Cài đặt của máy TrimUI).
2. Mở **RomCloud** -> vào mục **Cài Đặt** hoặc xem góc trên màn hình:
   * Màn hình sẽ hiển thị địa chỉ Web Portal: `http://<IP_MÁY_TRIMUI>:8080` (ví dụ: `http://192.168.1.50:8080`).
3. Mở trình duyệt trên máy tính hoặc điện thoại (cùng mạng Wi-Fi), truy cập địa chỉ trên.
4. Bấm nút **"Đăng nhập Google Drive"** để liên kết kho ROM của bạn:
   * App sẽ tự động đồng bộ danh sách game, tải ảnh bìa Boxart và cho phép bạn tải game về chơi ngay lập tức!

---

## 4. Quản lý xem TV Online (IPTV)

1. Truy cập vào địa chỉ Web Portal `http://<IP_MÁY_TRIMUI>:8080`.
2. Chuyển sang tab **"Quản Lý IPTV"**:
   * **Thêm từ link URL:** Dán đường dẫn link m3u/m3u8 và đặt tên nguồn (ví dụ: *Kênh Thể Thao*, *Kênh Tin Tức*).
   * **Upload file M3U:** Tải file danh sách kênh từ máy tính lên trực tiếp TrimUI.
   * **Xóa / Tải lại:** Quản lý xóa hoặc cập nhật nguồn kênh chỉ bằng 1 nút bấm.
3. Trên máy TrimUI, vào mục **XEM TV** để thưởng thức hàng trăm kênh truyền hình với khả năng tìm kiếm nhanh và phím **L1 / R1** sang trang cực nhanh!

---

## 5. Nguyên tắc an toàn & Bảo vệ Firmware

* RomCloud hoạt động độc lập 100% trong thư mục `/mnt/SDCARD/Apps/RomCloud`.
* Không can thiệp, không sửa đổi bất kỳ file hệ thống nào của TrimUI.
* Khi bạn nâng cấp firmware TrimUI, RomCloud và toàn bộ tài khoản, game và dữ liệu đã lưu **được giữ nguyên vẹn 100%**.
