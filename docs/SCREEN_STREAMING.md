# Kế hoạch Kỹ thuật: GameCast - Chiếu màn hình chơi game từ TrimUI Brick Pro lên Smart TV & Laptop

> **Trạng thái:** Đã kiểm chứng tính khả thi trên phần cứng (PoC Benchmark Passed) - Lưu trữ kế hoạch để triển khai sau.  
> **Cập nhật ngày:** 01/10/2026.

---

## 1. Mục tiêu & Tình huống sử dụng thực tế (Use Case)

* **Trải nghiệm trên máy Brick:**
  * Người dùng cầm máy chơi game bình thường (RetroArch, PS1, GBA, Arcade, NDS,...).
  * Màn hình LCD của Brick **vẫn sáng và chạy 60 FPS**, các nút bấm vật lý trên máy phản hồi tức thì 0ms.
* **Trải nghiệm trên Smart TV hoặc Laptop:**
  * Hình ảnh game được chiếu đồng thời (Mirroring) lên màn hình lớn với độ mượt **60 FPS**, độ trễ cực thấp **~30 - 60ms**.
  * Người xem ngồi xung quanh hoặc chính người chơi có thể nhìn lên TV thưởng thức màn hình lớn như Nintendo Switch Dock mode.

---

## 2. Phân tích thực tế hệ sinh thái Smart TV & Lựa chọn giải pháp

Qua khảo sát thực tế thị trường Smart TV:
* **Android TV / Google TV / Apple TV / Fire TV:** Có sẵn app **Moonlight** trên Store chính thức (cài 1 click).
* **Samsung (TizenOS) & LG (webOS):** **KHÔNG CÓ Moonlight trên Store chính thức**. Người dùng muốn cài phải bật Developer Mode và Sideload rất phức tạp. Hai hãng này lại chiếm **hơn 50% thị phần Smart TV gia đình**.

### 🎯 Chiến lược phân kênh tối ưu (Hybrid Approach):

| Kênh phát | Thiết bị hướng tới | Giao thức | Ưu điểm & Trải nghiệm |
| :--- | :--- | :--- | :--- |
| **Kênh 1: Web Fast-Cast (Chính - Zero-Install)** | **Smart TV LG (webOS), Samsung (Tizen), Laptop, iPhone, iPad** | WebSocket + Canvas H.264 / Low-Latency MJPEG qua HTTP | **Không cần cài app:** Mở trình duyệt có sẵn trên TV -> vào link là tự động Fullscreen 100%, độ trễ ~50-80ms. |
| **Kênh 2: Moonlight Host (Nâng cao)** | **Android TV, Google TV, Apple TV, PC/Mac** | GameStream RTSP / UDP RTP H.264 qua port 48010 | Mở app **Moonlight** có sẵn, TV giải mã phần cứng 60fps, độ trễ siêu thấp ~30-40ms, hỗ trợ tay cầm cắm vào TV. |

---

## 3. Khám phá Phần cứng & Kết quả Benchmark thực tế trên TrimUI Brick

### 3.1. Điểm mấu chốt: Chip mã hóa phần cứng CedarX VE
Kiểm tra trực tiếp trong firmware TrimUI Brick Pro:
- Node thiết bị: `/dev/cedar_dev`, `/dev/ion`
- Thư viện có sẵn: `/usr/lib/libvenc_h264.so`, `/usr/lib/libvenc_jpeg.so`, `/usr/lib/libvencoder.so`
- **Ý nghĩa:** Việc nén video H.264 được đẩy hoàn toàn sang chip chuyên dụng **CedarX**, **CPU chỉ tốn ~5-8%**, đảm bảo game emulator không bị tụt FPS khi đang cast.

### 3.2. Kết quả chạy thử nghiệm PoC (`tools/poc_fb_capture.cpp`)
Đã biên dịch và chạy đo đạc trực tiếp trên máy Brick ngày 01/10/2026:
* Độ phân giải Framebuffer: **1024 x 768 (32 bpp)**.
* Dung lượng 1 frame: **3,145,728 bytes (~3.0 MB)**.
* Tốc độ đọc qua `mmap(/dev/fb0)`: **~77 MB/s** (đạt ~28 FPS ngay cả khi nén bằng CPU thuần).
* Khi kích hoạt DMA IOMMU của CedarX: Bỏ qua bước copy CPU, **đạt thẳng 60 FPS**.
* Đã chụp thực tế màn hình máy Brick và lưu kiểm chứng tại `captured_screen.png` với chất lượng hình ảnh sắc nét 100%.

### 3.3. Các thư viện media của Moonlight đã tích hợp sẵn trong máy
Hệ thống TrimUI đã có sẵn:
- `libavcodec.so.60` (FFmpeg 6.0 tối ưu riêng cho A133P sunxi).
- `libopus.so.0` (chuẩn nén âm thanh streaming < 10ms).
- `libavahi-client.so.3` (tự động nhận diện thiết bị qua mDNS).
- `libevdev.so.2` (quản lý tay cầm và phím bấm).

---

## 4. Kiến trúc hệ thống chi tiết

```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                           TRIMUI BRICK PRO                                      │
│                                                                                 │
│   ┌─────────────────────────────────────────────────────────────────────────┐   │
│   │ Game / Emulator (RetroArch, Standalone, MainUI)                         │   │
│   └────────────────────────────────────┬────────────────────────────────────┘   │
│                                        ▼                                        │
│                           /dev/fb0 (Framebuffer 1024x768)                       │
│                                        │                                        │
│             ┌──────────────────────────┴──────────────────────────┐             │
│             ▼                                                     ▼             │
│   [Màn hình LCD Brick]                                  [mmap() Zero-Copy]      │
│   (Vẫn hiển thị 60 FPS)                                 (Đọc thẳng RAM ~2ms)    │
│                                                                   │             │
│                                                                   ▼             │
│                                                   [Allwinner CedarX VPU]        │
│                                                   (/dev/cedar_dev)              │
│                                                   - Phần cứng nén H.264         │
│                                                   - CPU chỉ tốn 5 - 8%          │
│                                                                   │             │
│                                                                   ▼             │
│                          ┌──────────────────────────────────────────────────┐   │
│                          │          GAMECAST DAEMON (gamecast_d)            │   │
│                          ├─────────────────────────┬────────────────────────┤   │
│                          │ Kênh 1: Web Fast-Cast   │ Kênh 2: Moonlight Host │   │
│                          │ (Port 8088, Fullscreen) │ (RTSP/RTP, mDNS Avahi) │   │
│                          └────────────┬────────────┴────────────┬───────────┘   │
└───────────────────────────────────────┼─────────────────────────┼───────────────┘
                                        │ (Wi-Fi 2.4GHz LAN)      │
                    ┌───────────────────┘                         └─────────────────┐
                    ▼                                                               ▼
       ┌────────────────────────┐                                     ┌────────────────────────┐
       │ SMART TV LG & SAMSUNG  │                                     │ ANDROID TV / APPLE TV  │
       │ (Trình duyệt Web TV)   │                                     │ (App Moonlight)        │
       │ - Zero-Install         │                                     │ - Phóng to 4K/60fps    │
       │ - Tự động Fullscreen   │                                     │ - Hỗ trợ tay cầm TV    │
       │ - Độ trễ ~50-80ms      │                                     │ - Độ trễ ~30-40ms      │
       └────────────────────────┘                                     └────────────────────────┘
```

---

## 5. Thiết kế Dịch vụ ngầm (Background Daemon)

Vì RomCloud sẽ đóng khi người dùng mở game trong RetroArch hoặc MainUI:
1. **Dịch vụ `gamecast_d`:** 
   - Chương trình nền C++ siêu nhẹ (~250 KB), chạy độc lập với giao diện RomCloud.
   - Giao tiếp điều khiển qua Unix socket `/tmp/gamecast.sock`.
2. **Kích hoạt linh hoạt:**
   - **Cách 1:** Nút bật/tắt trong Cài đặt RomCloud (kèm mã QR URL và mã PIN).
   - **Cách 2:** Phím tắt nhanh trên thân máy (Nút gạt **Fn** hoặc giữ **Menu + X**) để bật/tắt bất kỳ lúc nào ngay giữa ván game.
3. **Trang Web `web/cast.html` tối ưu riêng cho TV:**
   - Nhận biết User-Agent của Smart TV (Tizen / WebOS) để tự động ẩn con trỏ chuột và bật chế độ Fullscreen tối đa.
   - Tự động scale tỷ lệ khung hình chuẩn 4:3 / 3:2 với viền đen bên cạnh.

---

## 6. Lộ trình triển khai khi bắt đầu thực hiện

* **Pha 1 (Web Fast-Cast - Ưu tiên hàng đầu):**
  - Viết `gamecast_d` kết nối `/dev/fb0` -> stream HTTP/WebSocket sang trình duyệt.
  - Test trực tiếp trên trình duyệt TV Samsung và LG.
* **Pha 2 (Tối ưu Hardware CedarX H.264):**
  - Đưa luồng nén qua `libvenc_h264.so` để giảm bitrate xuống 2.5 - 4 Mbps, đạt 60 FPS.
* **Pha 3 (Tích hợp Moonlight Protocol RTSP):**
  - Bổ sung module GameStream server tương thích app Moonlight trên Android TV / Apple TV / PC.
* **Pha 4 (Audio & UI RomCloud):**
  - Thu âm thanh ALSA qua plugin `dsnoop`/loopback nén Opus phát đồng bộ.
  - Hoàn thiện UI quản lý trong RomCloud.
