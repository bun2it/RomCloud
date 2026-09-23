# RomCloud v2.0.2 - Hướng Dẫn Sử Dụng

## 📱 Tổng Quan

RomCloud là ứng dụng quản lý ROM cho máy TrimUI, hỗ trợ:
- **Stock PS / Brick Pro**
- **NextUI**
- **SpruceOS**

---

## 🎮 Tính Năng Chính

### 1. Quản Lý ROM
- Tự động quét ROM từ thẻ nhớ
- Tìm kiếm game nhanh
- Lọc theo hệ máy, trạng thái
- Đồng bộ với Google Drive

### 2. Đồng Bộ Cloud
- Kết nối Google Drive
- Tải ROM từ Drive về máy
- Upload ROM lên Drive
- Đồng bộ 2 chiều

### 3. Xem TV (IPTV)
- Hỗ trợ playlist M3U
- Tích hợp sẵn danh sách kênh Việt Nam
- Upload playlist qua Web Portal
- Yêu cầu: **ffmpeg** hoặc **mpv**

### 4. Cập Nhật OTA
- Tự động kiểm tra bản mới
- Tải & cài đặt tự động
- Hỗ trợ nhiều phiên bản OS

---

## 🎯 Hướng Dẫn Sử Dụng

### Khởi Động
1. Chạy RomCloud từ menu TrimUI
2. Đợi ứng dụng khởi động
3. Vào **CÀI ĐẶT** để cấu hình

### Cài Đặt Google Drive
1. Vào **CÀI ĐẶT**
2. Chọn **Kết nối Google Drive**
3. Mở trình duyệt: `http://<IP máy>:8080`
4. Đăng nhập Google → Ủy quyền
5. Hoàn tất!

### Đồng Bộ ROM
1. Vào **ĐỒNG BỘ**
2. Chọn **Quét thẻ nhớ** để cập nhật danh sách
3. Hoặc **Đồng bộ Drive** để đồng bộ với cloud

### Xem TV
1. Vào **XEM TV**
2. Chọn nhóm kênh ( ▲▼ )
3. Chọn kênh ( ▲▼◀▶ )
4. Nhấn **[A]** để phát
5. Nhấn **[B]** để thoát

#### Thêm Playlist M3U
1. Mở `http://<IP máy>:8080`
2. Vào tab **IPTV**
3. Upload file `.m3u` hoặc thêm URL
4. Restart app để cập nhật

### Cập Nhật OTA
1. Vào **CẬP NHẬT**
2. App tự động kiểm tra bản mới
3. Nhấn **Cập nhật ngay** nếu có bản mới
4. Đợi tải & cài đặt hoàn tất

---

## ⚙️ Cài Đặt Nâng Cao

### Chọn Phiên Bản OS
Nếu app không tự nhận diện đúng OS:
1. Vào **CÀI ĐẶT**
2. Chọn **Phiên bản OS**
3. Nhấn **[A]** để đổi:
   - **Auto** - Tự nhận diện
   - **Stock (PS)** - TrimUI Stock
   - **NextUI** - NextUI firmware
   - **SpruceOS** - SpruceOS/SpruceUI

### Web Portal
Truy cập: `http://<IP máy>:8080`

| Tab | Chức năng |
|-----|-----------|
| ROM | Quản lý game, tìm kiếm |
| Hàng đợi | Xem tiến trình tải |
| Đồng bộ | Cấu hình Cloud |
| IPTV | Upload playlist TV |
| OTA | Cập nhật ứng dụng |

---

## 🔧 Xử Lý Sự Cố

### TV không phát
- Cần cài **ffmpeg** hoặc **mpv**
- App sẽ tự cài khi cập nhật OTA
- Hoặc tải file sau vào thư mục `bin/`:
  - `ffmpeg` hoặc `mpv`

### Không kết nối được Drive
- Kiểm tra internet
- Đảm bảo đã ủy quyền đúng tài khoản

### ROM không hiển thị
- Kiểm tra thẻ nhớ có ROM không
- Thử **Quét lại** trong menu đồng bộ

---

## 📁 Cấu Trúc Thư Mục

```
/mnt/SDCARD/
├── Apps/RomCloud/          # Ứng dụng
│   ├── bin/                # Binary & tools
│   ├── config/             # Cấu hình
│   ├── data/               # Database
│   ├── cache/              # Ảnh bìa
│   ├── iptv/               # Playlist TV
│   └── assets/             # Icons, fonts
├── Roms/                   # ROM games
└── Imgs/                  # Ảnh bìa game
```

---

## 📋 Phiên Bản

| Phiên bản | Ngày | Thay đổi |
|-----------|------|----------|
| v2.0.2 | 2026-09-23 | Multi-OS, Auto dependencies |
| v2.0.1 | 2026-09-23 | IPTV, OTA mpv |
| v2.0.0 | 2026-09-21 | Phien bản đầu tiên |

---

## 🔗 Liên Kết

- **GitHub:** https://github.com/bun2it/RomCloud
- **Issues:** https://github.com/bun2it/RomCloud/issues

---

*RomCloud v2.0.2 - Quản lý ROM cho TrimUI*
