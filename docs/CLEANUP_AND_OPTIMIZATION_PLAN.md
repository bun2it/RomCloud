# Kế hoạch dọn dẹp Code thừa, Assets & Tối ưu dung lượng Release RomCloud

## 1. Mục tiêu
- **Giảm tối đa dung lượng release:** Cắt giảm từ ~130MB xuống còn **dưới 20MB** mà vẫn giữ trọn vẹn 100% tính năng, giao diện và font tiếng Việt.
- **Loại bỏ rác & code chết:** Dọn sạch các hàm không dùng (gây warning compiler), file thử nghiệm (POC), công cụ debug nội bộ.
- **Chuẩn hóa cấu trúc Assets:** Loại bỏ hoàn toàn icon trùng lặp, icon legacy bị cấm theo chuẩn TrimUI Brick, font CJK thừa nặng hàng chục MB.

---

## 2. Bảng phân tích chi tiết các thành phần cần dọn dẹp

### A. Nhóm Assets (Tiết kiệm ~65MB)
| Hạng mục | Vị trí hiện tại | Dung lượng | Tình trạng & Phương án xử lý |
| :--- | :--- | :--- | :--- |
| **Fonts CJK không dùng** | `assets/fonts/NotoSansHK/JP/KR/SC/TC` | **35.0 MB** | RomCloud là app giao diện Tiếng Việt/English trên TrimUI Brick. Brick đã có sẵn font hệ thống. Xóa 5 file font này khỏi repo. |
| **Font thừa `font.ttf`** | `assets/fonts/font.ttf` | **16.0 MB** | Font cũ chứa CJK khổng lồ. App đã dùng `NotoSans-Regular.ttf` (chỉ 197KB, đủ 100% dấu tiếng Việt). Bỏ `font.ttf`, chỉ giữ lại `NotoSans-Regular.ttf`. |
| **Icon app khổng lồ** | `assets/apps_icons/APP.png`, `app_main.png` | **2.2 MB** | 2 file PNG độ phân giải 1254x1254 chưa nén. Resize/optimize về 256x256 hoặc dùng `icon.png` (512x512) $\rightarrow$ giảm còn ~30KB (giảm 98%). |
| **Icons giả lập bị trùng 100%** | `assets/icons/ARCADE.png`, `FC.png`,... | **350 KB** | Trùng lặp hoàn toàn với `assets/grid_icons/`. Code app load từ `assets/grid_icons/`. Xóa bản copy trùng ở `assets/icons/`. |
| **Icon nút bấm legacy (bị cấm)** | `assets/button_icons/options.png`, `view.png`, `start_icon.png` | ~5 KB | Vi phạm Rule 8 trong `AGENTS.md` (đã thay bằng `MENU.png`, `SELECT.png`, `START.png`). Xóa bỏ. |
| **Stock icons thừa** | `assets/stock_icons/iotesting-key-*.png` | ~15 KB | 11 file icon test phần cứng pull từ skin Brick, code không hề sử dụng. Xóa bỏ. |

---

### B. Nhóm File Binary & Tool thử nghiệm (Tiết kiệm ~42MB)
| Hạng mục | Vị trí hiện tại | Dung lượng | Tình trạng & Phương án xử lý |
| :--- | :--- | :--- | :--- |
| **yt-dlp nhị phân glibc** | `bin/yt-dlp-glibc` | **38.0 MB** | File binary standalone cực nặng, gây crash kernel OOM khi chạy trên Brick 1GB RAM. YouTube hiện tại đã chuyển 100% sang Python3 Innertube / Data API siêu nhẹ. Bỏ khỏi gói release. |
| **Binary POC capture màn hình** | `bin/poc_fb_capture` | **3.6 MB** | Binary thử nghiệm framebuffer của tính năng GameCast cũ. Không sử dụng trong app. Xóa bỏ. |
| **Mã nguồn tool thử nghiệm** | `tools/poc_fb_capture.cpp`, `tools/ls_test_input.c`, `src/logging/test_issue_logger.cpp` | ~15 KB | Các file test tạm thời. Gom gọn hoặc loại khỏi build. |

---

### C. Nhóm Mã nguồn C++ (Code chết & Cảnh báo Compiler)
Dựa trên log biên dịch từ Clang/Zig:
1. **`src/ui/UIManager.cpp`:**
   - Xóa hàm tĩnh `parseInnertubeSearchResponse()` (trước đây thử parse JSON ở C++, giờ Python script đảm nhiệm).
   - Xóa hàm tĩnh `getBatteryLevel()` và `getCurrentTimeString()` (trùng lặp và không dùng, UI dùng từ `PlatformInfo`).
   - Xóa biến thừa `FOOTER_H` tại dòng 5755.
2. **`src/iptv/IPTVManager.cpp`:**
   - Xóa hàm tĩnh `buildChannelListAss()` (code thử nghiệm subtitle ASS cũ, không còn dùng).
   - Xóa biến không dùng `SDL_Color green`.
3. **`src/ui/ExplorerSync.cpp` & các lambda:**
   - Bỏ capture `this` không dùng trong các lambda để triệt tiêu toàn bộ warning compiler.

---

### D. Tối ưu hóa Build & Đóng gói (`build.sh` & `package.sh`)
1. **Strip Symbol nhị phân trong Release (`build.sh`):**
   - File `bin/RomCloud` hiện tại nặng **32 MB** vì chứa đầy đủ DWARF debug info (`with debug_info, not stripped`).
   - Thêm cờ `-Wl,-s` (strip symbols) khi build release $\rightarrow$ Dung lượng file thực thi giảm từ **32 MB xuống ~7 MB** (tiết kiệm ~25MB).
2. **Cập nhật script đóng gói (`package.sh`):**
   - Thay `cp assets/fonts/font.ttf` (16MB) $\rightarrow$ chỉ copy `assets/fonts/NotoSans-Regular.ttf` (197KB).
   - Bỏ copy `bin/yt-dlp-glibc` (38MB).
   - Bỏ copy các icon legacy, file test POC.
   - Gói ZIP Release cuối cùng sẽ giảm từ **~130MB $\rightarrow$ ~18MB** (nhẹ hơn hơn 7 lần, tải và giải nén cực nhanh qua OTA/SDCard).

---

## 3. Thứ tự các bước thực hiện an toàn
- **Bước 1:** Dọn dẹp code chết và các warning trong `src/ui/UIManager.cpp`, `src/iptv/IPTVManager.cpp`, `src/ui/ExplorerSync.cpp`.
- **Bước 2:** Xóa các assets thừa/trùng lặp (`assets/stock_icons/iotesting-*`, `assets/fonts/*.ttf` thừa, `assets/button_icons` legacy, `assets/icons/` trùng).
- **Bước 3:** Nén/tối ưu 2 file icon lớn `APP.png` và `app_main.png`.
- **Bước 4:** Cập nhật `build.sh` (thêm cờ strip cho release) và cập nhật `package.sh`.
- **Bước 5:** Chạy `./build.sh` kiểm tra biên dịch 0 warning, 0 error, sau đó chạy `./package.sh` để kiểm chứng dung lượng file zip hoàn thiện.
