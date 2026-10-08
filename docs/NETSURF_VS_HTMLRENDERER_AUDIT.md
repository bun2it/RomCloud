# Báo Cáo Audit Chuyên Sâu: So Sánh Engine Hiện Tại Với NetSurf Gốc & Kế Hoạch Khắc Phục

## 1. Tổng Quan Kiến Trúc (Architecture Audit)

Trong mã nguồn RomCloud hiện tại có hai thành phần liên quan đến trình duyệt web:
1. **Thư viện NetSurf chính thức trong sysroot (`sysroot/lib/`)**:
   - `libcss.a` (8.8 MB): CSS3 parsing & selection cascade engine hoàn chỉnh của NetSurf.
   - `libdom.a` (4.2 MB): W3C DOM Level 1-3 core & HTML element interfaces.
   - `libhubbub.a` (1.2 MB): HTML5 standard parser & tokenizer.
   - `libparserutils.a` (392 KB): Charset conversion & string utilities.
   - `libwapcaplet.a`: Interned string table.
   - **Tất cả đã được cross-compile sẵn cho ARM aarch64 (TrimUI Brick Pro)**.
2. **Custom HtmlRenderer Engine (`src/browser/HtmlRenderer.cpp`)**:
   - Render engine tự viết siêu nhẹ, giao tiếp trực tiếp với SDL2 và SDL2_ttf.
   - Được thiết kế ban đầu cho Wi-Fi Captive Portal login và các trang tra cứu nhẹ.
   - Hiện tại đang đảm nhiệm toàn bộ việc duyệt web thông thường (VnExpress, Wikipedia, Game Vui, Google...).

---

## 2. Bảng So Sánh Chi Tiết: HtmlRenderer vs NetSurf Gốc

| Tiêu Chí | NetSurf Gốc | HtmlRenderer Hiện Tại | Đánh Giá & Vấn Đề Hiện Tại |
| :--- | :--- | :--- | :--- |
| **HTTP Request Headers** | Gửi đầy đủ `User-Agent`, `Referer`, `Accept`, `Accept-Language`. | Gửi headers rỗng `{}` khi fetch trang, CSS và hình ảnh! | **Nghiêm trọng**: CDN ảnh (VnExpress, Wikimedia, Cloudflare) chặn 403 Forbidden do thiếu User-Agent/Referer. |
| **Nhận diện Mobile / Desktop** | Web server tự quyết định dựa trên User-Agent tiêu chuẩn. | Một số trang bị ép cưỡng bức mobile view gây lỗi load. | Cần để web tự xác định dựa trên User-Agent chuẩn. |
| **Text Alignment (`text-align`)** | Hỗ trợ `left`, `center`, `right`, `justify`. Thẻ con override được thẻ cha. Đọc cả thuộc tính HTML `align="..."`. | Chỉ có cờ boolean `centered`. Nếu cha center thì con bị ép center vĩnh viễn (không override left được). Bỏ qua HTML `align`. | **Lỗi hiển thị**: Text bị căn giữa lung tung, không align left đúng theo CSS của thẻ con. |
| **Xử lý Hình Ảnh (Image)** | Hỗ trợ CSS `width`, `height`, `max-width`, float, inline image. | Chỉ đọc HTML `width`/`height`. Bỏ qua CSS image sizing. Khi tải ảnh bị chặn bởi CDN. | **Lỗi hiển thị**: Ảnh không load được hoặc sắp xếp sai kích thước CSS. |
| **Khoảng Gap Trống (Spacing)** | CSS Margin Collapsing: `max(m1, m2)`. Thẻ rỗng không chiếm chỗ. | Mọi wrapper `div`, `p`, `h*` đều cộng dồn `marginTop`/`marginBottom` kể cả khi rỗng, tạo khoảng trắng khổng lồ. | **Lỗi hiển thị**: Xuất hiện nhiều khoảng gap trống bất thường giữa các dòng. |
| **CSS Media Queries** | Đánh giá đầy đủ media features (width, height, screen, orientation). | Chỉ mới hỗ trợ cơ bản min-width/max-width. | Đã cải thiện nhưng cần chuẩn hóa thêm. |
| **DOM Hierarchy** | W3C DOM chuẩn qua `libdom` + `libhubbub`. | Danh sách phẳng / cây lồng đơn giản với custom tokenizer. | Gặp khó khăn với cấu trúc HTML phức tạp. |

---

## 3. Phân Tích Chi Tiết 4 Lỗi Người Dùng Chỉ Ra

### 3.1. Hình ảnh chưa được load hoặc sắp xếp sai CSS
* **Nguyên nhân không load được**:
  Trong `startImageWorker()` (dòng 3044), lệnh fetch gọi:
  ```cpp
  HttpResponse resp = HttpClient::instance().get(url, {}, 15); // Headers rỗng!
  ```
  Hầu hết các trang báo (VnExpress, Dân Trí) và CDN (Akamai, Cloudflare) có cơ chế **Anti-Hotlinking & Bot Detection**:
  - Không có `User-Agent` hợp lệ -> Trả về `403 Forbidden`.
  - Không có `Referer` (chứng minh ảnh được gọi từ trang web đó) -> Trả về `403 Forbidden`.
  - Khi fetch thất bại, `markImageFailed` đưa URL vào blacklist, khiến ảnh **vĩnh viễn không bao giờ được load**.
* **Nguyên nhân sắp xếp sai CSS**:
  Trong `applyDecls()`, các thuộc tính CSS `width`, `height`, `max-width`, `min-width` bị bỏ qua không gán cho `imgW` / `imgH`, dẫn đến ảnh tải về bị vẽ theo kích thước gốc thay vì co theo CSS.

### 3.2. Text chưa được align đúng (chỗ center, chỗ align left...)
* **Nguyên nhân**:
  1. Trong `HtmlRenderer.h`, thuộc tính chỉ là boolean `bool centered = false;`.
  2. Trong hàm kế thừa CSS (`walk`):
     ```cpp
     if (!el.centered) el.centered = inh.centered;
     ```
     Nếu thẻ cha (ví dụ `<div style="text-align: center">` hoặc `header`) có `centered = true`, thì thẻ con dù có CSS `text-align: left` cũng bị câu lệnh trên **ép lại thành centered = true**! Con không bao giờ có thể căn trái nếu cha căn giữa!
  3. Thẻ HTML có thuộc tính cũ `align="center"`, `align="left"`, `align="right"` (phổ biến trên Wikipedia, diễn đàn, tin tức) bị bỏ qua 100%.

### 3.3. Vẫn còn những khoảng gap trống
* **Nguyên nhân**:
  1. Khi một thẻ `<p>` hoặc `<div>` chứa các thẻ con `<span>`, `<a>`: Mỗi thẻ con bị coi như một block độc lập, làm tăng `y` nhiều lần và cộng thêm khoảng cách dòng `LINE_HEIGHT`.
  2. Các thẻ wrapper rỗng (chỉ chứa thẻ ẩn hoặc comment): Vẫn tự động cộng `y += el.marginTop` và `y += 6;`.
  3. Thiếu cơ chế **Margin Collapse**: Khi 2 phần tử liền nhau đều có margin, chúng bị cộng dồn thay vì lấy giá trị lớn hơn.

### 3.4. Chế độ mobile cần để web tự xác định
* **Nguyên nhân**:
  Nếu trình duyệt tự ý can thiệp đổi URL thành mobile hoặc ép viewport dị biệt, server có thể trả về template giao diện bị lỗi, JavaScript không tương thích hoặc chuyển hướng vòng lặp.
* **Giải pháp**:
  - Gửi header `User-Agent` chuẩn của modern browser (Chrome Linux / Android).
  - Không can thiệp đổi URL gốc của trang.
  - Để server tự quyết định dựa trên chuẩn HTTP content negotiation.

---

## 4. Kế Hoạch Khắc Phục Cụ Thể (Action Plan)

1. **Sửa lỗi Fetch Headers**:
   - Tạo hàm `buildHttpHeaders(const std::string& referer)`:
     - `User-Agent: Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36`
     - `Referer: <current_url>` cho mọi request tải ảnh và CSS.
     - `Accept: text/html,application/xhtml+xml,image/*,*/*;q=0.8`
     - `Accept-Language: vi-VN,vi;q=0.9,en-US;q=0.8,en;q=0.7`
2. **Chuẩn hóa Text Alignment**:
   - Thêm enum `enum class TextAlign { DEFAULT, LEFT, CENTER, RIGHT };` vào `HtmlElement`.
   - Đọc cả thuộc tính HTML `align="..."` lẫn CSS `text-align`.
   - Cơ chế kế thừa: Thẻ con có khai báo explicit (`LEFT`, `RIGHT`, `CENTER`) sẽ override hoàn toàn thẻ cha.
   - Hàm `renderElement` căn chỉnh theo đúng `TextAlign` (LEFT vẽ ở x, CENTER căn giữa availW, RIGHT căn phải availW).
3. **Triệt tiêu các khoảng gap trống**:
   - Kiểm tra `hasVisibleContent()` trước khi cộng margin cho block element.
   - Ngăn chặn các container rỗng tích lũy margin dọc.
   - Thắt chặt khoảng cách của các thẻ inline và line break.
4. **Hỗ trợ CSS Image Sizing**:
   - Parse `width`, `height`, `max-width` từ CSS cho các phần tử ảnh.
   - Đảm bảo ảnh hiển thị đúng tỷ lệ khung hình CSS quy định.

---

## 5. Các Hạng Mục Tiếp Theo & Theo Dõi Agent Khác (Roadmap & Audit Queue)

Hiện tại Agent đối tác đang triển khai 2 hạng mục tiếp theo:

### Hạng mục 6: `colspan` / `rowspan`, `background-image` (Ước tính: 1–2 ngày mỗi mục)
* **Nội dung thực hiện của Agent khác**:
  * Bổ sung thuộc tính `colspan` và `rowspan` cho bảng `<table>` (`<td>`, `<th>`) để hiển thị đúng layout bảng biểu phức tạp.
  * Hỗ trợ thuộc tính CSS `background-image: url(...)` cho các phần tử container/banner.
* **Kế hoạch Audit khi hoàn thành**:
  * Kiểm tra parser nhận diện đúng cú pháp `colspan`, `rowspan` và tính toán lại tọa độ grid cell tránh chồng đè.
  * Kiểm thử fetch bất đồng bộ và cache surface của `background-image` không gây nghẽn UI main thread.

### Hạng mục 7: `SVG` / `JS` (Duktape) / Engine Đầy Đủ (PoC Đo Trên Máy Thực Tế, Đạt Mới Giữ)
* **Nội dung thực hiện của Agent khác**:
  * Khảo sát và tích hợp PoC hỗ trợ vector SVG và thực thi JavaScript cơ bản **sử dụng Duktape** (nhúng nhẹ, footprint RAM thấp cho embedded).
  * Đo đạc hiệu năng (CPU, RAM heap, Frame time) trực tiếp trên phần cứng TrimUI Brick Pro (ARM64 Allwinner A133p).
  * Tiêu chí: Nếu PoC mượt mà, đạt chuẩn tài nguyên thì giữ lại; nếu quá tải hoặc giật lag thì tinh giản.
* **Kế hoạch Audit khi hoàn thành**:
  * Kiểm tra tích hợp Duktape (overhead khởi tạo context, bộ nhớ heap, DOM binding sandbox).
  * Đo benchmark thực tế trên thiết bị vật lý qua ADB shell (`top`, `ps`, `/proc/meminfo`).
  * Kiểm tra memory leak, crash handler và độ ổn định của thread khi chạy script dài/vòng lặp.

---

> [!NOTE]
> **Trạng thái hiện tại**: Đang tạm dừng theo dõi. Chờ thông báo từ User khi Agent đối tác hoàn thành mục 6 và mục 7 để tiến hành Audit và kiểm thử tích hợp.
