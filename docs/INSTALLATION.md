# HƯỚNG DẪN CÀI ĐẶT ROMCLOUD CHO TRIMUI BRICK PRO
(RomCloud Installation & Deployment Guide)

---

## 1. NGUYÊN TẮC BẢO VỆ FIRMWARE (CRITICAL ARCHITECTURE REQUIREMENT)

RomCloud được thiết kế theo kiến trúc **100% Firmware-Independent**:
* Ứng dụng hoạt động hoàn toàn bên trong thẻ nhớ microSD (`/mnt/SDCARD/Apps/RomCloud/`).
* **KHÔNG** can thiệp, ghi đè, sửa đổi bất kỳ file nào trong phân vùng hệ thống (`/rom`, `/overlay`, `/root`, `/usr`, `/etc`, `/mnt/UDISK`).
* **KHÔNG** can thiệp vào các trình giả lập gốc (`/mnt/SDCARD/Emus/`, `/mnt/SDCARD/RetroArch/`).
* Khi TrimUI cập nhật firmware chính thức (OTA hoặc thẻ nhớ), **RomCloud và toàn bộ cơ sở dữ liệu metadata ROM giữ nguyên 100% không bị mất hay hỏng**.

---

## 2. CẤU TRÚC THƯ MỤC CÀI ĐẶT

Thư mục ứng dụng RomCloud trên thẻ nhớ TrimUI Brick Pro:

```text
/mnt/SDCARD/
├── Apps/
│   └── RomCloud/
│       ├── config.json         <-- TrimUI MainUI App manifest
│       ├── icon.png            <-- Biểu tượng ứng dụng 300x300 PNG
│       ├── launch.sh           <-- Kịch bản khởi chạy tối ưu
│       ├── bin/
│       │   └── RomCloud        <-- File thực thi nhị phân ELF 64-bit C++17
│       ├── assets/
│       │   ├── fonts/
│       │   │   └── font.ttf    <-- Font hiển thị giao diện
│       │   └── boxarts/        <-- Cache ảnh bìa cục bộ
│       ├── config/
│       │   └── config.json     <-- Cấu hình OAuth & bộ đệm
│       ├── data/
│       │   └── library.db      <-- SQLite Database lưu trữ toàn bộ ROM metadata
│       ├── cache/
│       │   └── covers/         <-- Thumbnail ảnh bìa tải về
│       ├── logs/
│       │   └── romcloud.log    <-- Nhật ký hoạt động chi tiết
│       └── temp/               <-- Bộ đệm tải về theo block
└── Roms/                       <-- Thư mục ROM tiêu chuẩn của TrimUI
    ├── FC/
    ├── SFC/
    ├── GBA/
    ├── PS/
    └── ...
```

---

## 3. CÁCH CÀI ĐẶT LÊN THẺ NHỚ TRIMUI

### Cách 1: Copy trực tiếp qua Đầu đọc thẻ hoặc Chế độ USB Storage
1. Cắm thẻ nhớ TrimUI vào máy tính (hoặc bật chế độ USB Storage trên TrimUI).
2. Sao chép toàn bộ thư mục `RomCloud` vào đường dẫn `Apps/` trên thẻ nhớ (thành `Apps/RomCloud/`).
3. Đảm bảo file `launch.sh` và `bin/RomCloud` có quyền thực thi (`chmod +x`).
4. Rút thẻ nhớ an toàn, khởi động lại TrimUI. Biểu tượng **RomCloud** sẽ xuất hiện ngay trong mục **Apps** của màn hình chính TrimUI.

### Cách 2: Triển khai qua ADB
```bash
adb push RomCloud /mnt/SDCARD/Apps/
adb shell "chmod +x /mnt/SDCARD/Apps/RomCloud/launch.sh /mnt/SDCARD/Apps/RomCloud/bin/RomCloud"
```

---

## 4. TƯƠNG THÍCH FIRMWARE & TỰ ĐỘNG KHÔI PHỤC

* Nếu người dùng nâng cấp firmware TrimUI, TrimUI sẽ chỉ cập nhật các phân vùng `/rom` và kernel.
* Thư mục `Apps/RomCloud` trên `/mnt/SDCARD` hoàn toàn không bị chạm tới.
* Sau khi nâng cấp firmware, vào mục Apps trên TrimUI, RomCloud vẫn hoạt động ngay lập tức với đầy đủ tài khoản Google Drive đã liên kết và danh sách ROM đã đồng bộ.
