# Nghiên Cứu Chuyên Sâu: Viewport Meta Tag & Bố Cục Responsive Trên TrimUI Brick

## 1. Đặt Vấn Đề & Nhận Định Cốt Lõi

Trên các thiết bị cầm tay màn hình nhỏ như **TrimUI Brick** (màn hình 3.2 inch, độ phân giải 1024x768):
- Khi tải các trang web hiện đại (VnExpress, Wikipedia, Game Vui, Google, các trang tin tức), phần lớn các trang đều được thiết kế theo tư duy **Responsive Web Design (Mobile-First / Adaptive)**.
- Các trang web này dựa hoàn toàn vào thẻ meta viewport trong phần `<head>`:
  ```html
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  ```
- **Nếu browser engine bỏ qua hoặc không đọc thẻ này**:
  1. **Giả lập sai kích thước (Desktop Fallback 980px/1024px)**: Trình duyệt coi đây là trang web desktop cũ, áp dụng các bộ luật CSS desktop với các container rộng 1200px - 1440px.
  2. **Trộn lẫn và đè bẹp CSS `@media`**: Các file CSS hiện đại chứa cả khối `@media (max-width: 768px)` (cho mobile) lẫn `@media (min-width: 1200px)` (cho desktop màn hình lớn). Nếu không kiểm tra viewport width mà gộp bừa bãi, các luật CSS desktop sẽ ghi đè lên luật mobile, làm vỡ khung hình, tràn ngang màn hình, chữ nhỏ li ti hoặc bị lệch mất cân đối.
  3. **Không tận dụng được kích thước vùng vẽ thực tế**: Vùng vẽ web khả dụng thực tế của TrimUI Brick bị giới hạn bởi thanh Header và Footer, không phải toàn bộ 1024x768.

---

## 2. Thông Số Phần Cứng & Vùng Hiển Thị Khả Dụng Trên TrimUI Brick

### 2.1. Phân bổ không gian màn hình (Screen Layout Budget)
TrimUI Brick Pro có màn hình tỷ lệ 4:3 với độ phân giải vật lý **1024 x 768**:

```
Y = 0   ┌──────────────────────────────────────────────────────────┐
        │ HEADER BAR (Pin, WiFi, Đồng hồ, Title) - Cao 64 px       │
Y = 64  ├──────────────────────────────────────────────────────────┤
        │ URL ADDRESS BAR (Thanh địa chỉ web)   - Cao 48 px        │
Y = 112 ├──────────────────────────────────────────────────────────┤
        │                                                          │
        │                                                          │
        │       CONTENT VIEWPORT (Vùng nội dung trang web)         │
        │       Chiều rộng khả dụng (W): 992 px - 1024 px          │
        │       Chiều cao khả dụng  (H): 603 px                    │
        │       (Từ Y = 112 đến Y = 714)                           │
        │                                                          │
        │                                                          │
Y = 715 ├──────────────────────────────────────────────────────────┤
        │ FOOTER BAR (Các nút điều khiển D-PAD, A, B) - Cao 53 px  │
Y = 768 └──────────────────────────────────────────────────────────┘
```

* **Tổng độ phân giải**: `1024 x 768`
* **Vùng Chrome cố định**:
  * Header: `64 px`
  * URL Bar: `48 px`
  * Footer: `53 px`
  * **Tổng chiều cao Chrome**: `64 + 48 + 53 = 165 px`
* **Content Viewport chuẩn xác**:
  * `CONTENT_X = 0` (hoặc `PADDING = 16` ở hai bên)
  * `CONTENT_Y = 112`
  * `CONTENT_W = 1024` (chiều rộng tối đa) hoặc `992` (với lề 16px)
  * `CONTENT_H = 768 - 165 = 603 px`

### 2.2. Nghịch lý Mật độ điểm ảnh (DPI Paradox) trên màn hình 3.2 inch
* TrimUI Brick có độ phân giải **1024x768** nhưng kích thước đường chéo chỉ **3.2 inch**.
* Mật độ điểm ảnh lên tới **~400 PPI** (tương đương màn hình iPhone Retina).
* **Hậu quả nếu render thô 1:1 theo 1024px Desktop**:
  * Nếu một trang web render ở chiều rộng 1024px như màn hình máy tính bàn 24 inch thông thường, font chữ 14px sẽ có kích thước vật lý nhỏ hơn **1 milimet** trên màn hình thực tế, gây mỏi mắt và không thể đọc được.
  * Khi trang web nhận diện được Viewport di động, các thành phần giao diện sẽ tự động co về bố cục 1 cột (single-column card layout), cỡ chữ tiêu đề và nội dung được phóng lớn tương đối, hình ảnh chiếm vừa vặn chiều ngang màn hình.

---

## 3. Bản Chất Của Viewport Meta Tag (Theo Chuẩn W3C & WebKit)

Thẻ `<meta name="viewport">` chứa thuộc tính `content` gồm các cặp `key=value` phân tách bởi dấu phẩy:
```html
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
```

### Các chỉ thị chính:
1. **`width`**:
   * `device-width`: Báo cho engine dùng chiều rộng của thiết bị (ở đây là chiều rộng viewport khả dụng `CONTENT_W`).
   * Số cụ thể (ví dụ `width=768` hoặc `width=980`): Ép cứng chiều rộng viewport ảo của trang.
2. **`initial-scale`**:
   * Hệ số thu phóng ban đầu khi tải trang (thường là `1.0`).
   * Tỷ lệ giữa CSS pixel và physical pixel.
3. **`minimum-scale` / `maximum-scale`**:
   * Giới hạn mức độ zoom của người dùng.
4. **`user-scalable`**:
   * `yes` hoặc `no` (có cho phép zoom hay không).

### Khi KHÔNG có thẻ Viewport Meta (Desktop Legacy Fallback):
* Chuẩn Safari iOS và Google Chrome Mobile:
  * Nếu trang web không có thẻ `<meta name="viewport">`, trình duyệt gán mặc định:
    `viewport_width = 980px`.
  * Sau đó trang web được vẽ trên một canvas ảo rộng 980px rồi thu nhỏ lại để vừa màn hình.

---

## 4. Thực Trạng Hiện Tại Của Engine RomCloud

Qua kiểm tra mã nguồn `src/browser/HtmlRenderer.cpp` và `src/browser/HtmlRenderer.h`:

1. **Thẻ `<meta>` bị bỏ qua hoàn toàn trong Parser**:
   * Trong hàm `parseHtml()` (dòng 1466 và 1646):
     ```cpp
     if (tagName == "meta") { /* không làm gì, bị drop 100% */ }
     ```
   * Engine không trích xuất thông tin viewport, không lưu lại `width`, `initial-scale`.

2. **Xử lý `@media` CSS bị triệt tiêu điều kiện (Unconditional Flattening)**:
   * Tại dòng 652 trong `HtmlRenderer.cpp`:
     ```cpp
     // 2. Inline @media blocks (fixed 1024 viewport — conditions ignored).
     flat.append(s.substr(brace + 1, j - brace - 1));
     ```
   * Engine hiện tại **xóa sạch điều kiện `@media`** và nạp tất cả CSS vào chung một rổ!
   * **Hậu quả**:
     * Một trang web có:
       ```css
       .article { width: 100%; font-size: 16px; } /* Mobile */
       @media (min-width: 1200px) {
           .article { width: 1140px; margin-left: 200px; font-size: 13px; } /* Desktop Ultra-Wide */
       }
       ```
     * Engine gộp luôn khối `min-width: 1200px` vào, ghi đè lên Mobile rule. Kết quả: khung bài viết bị phình ra `1140px` (tràn khỏi màn hình 1024px của TrimUI), lề trái bị thụt vào `200px`, gây lệch và biến dạng layout!

3. **Chiều rộng Layout cố định**:
   * Tại `layout()`: `int layoutW = m_readerActive ? 760 : CONTENT_W;` (với `CONTENT_W = 992`).
   * Nếu không ở chế độ Reader Mode, mọi trang đều bị ép vào khung 992px bất kể trang web đó mong muốn hiển thị ở kích thước nào.

---

## 5. Giải Pháp Kiến Trúc Đề Xuất (Viewport Adaptation Plan)

Để giải quyết triệt để và mang lại trải nghiệm hiển thị cân đối, đẹp mắt nhất trên TrimUI Brick:

### Giai đoạn 1: Bổ sung Bộ Phân Tích Thẻ Viewport Meta (Viewport Parser)
* Thêm cấu trúc dữ liệu trong `HtmlRenderer.h`:
  ```cpp
  struct ViewportConfig {
      bool hasMeta = false;
      bool isDeviceWidth = false;
      int width = 0;             // Giá trị cụ thể nếu có (ví dụ 980, 1024)
      float initialScale = 1.0f;
      float maxScale = 1.0f;
      bool userScalable = true;
  };
  ViewportConfig m_viewport;
  ```
* Trong `parseHtml()`:
  * Khi gặp thẻ `<meta>`:
    * Kiểm tra `name="viewport"` (không phân biệt hoa thường).
    * Phân tích chuỗi `content`:
      * `width=device-width` -> `m_viewport.isDeviceWidth = true;`
      * `width=X` -> `m_viewport.width = atoi(X);`
      * `initial-scale=X` -> `m_viewport.initialScale = atof(X);`

### Giai đoạn 2: Tính toán Effective Viewport Width cho TrimUI Brick
* Xác định chiều rộng layout hiệu dụng (`effectiveWidth`):
  * **Trường hợp A: Trang có `width=device-width`** (Trang web Mobile / Responsive):
    * Gán `effectiveWidth = CONTENT_W` (992px hoặc 1024px).
    * Đánh dấu trang web ở chế độ **Mobile-Responsive Mode**.
  * **Trường hợp B: Trang có `width=X` cụ thể** (ví dụ `width=768`):
    * Dùng giá trị `X` làm chiều rộng tham chiếu layout.
  * **Trường hợp C: Trang không có thẻ Viewport Meta** (Trang Desktop truyền thống):
    * Gán `effectiveWidth = 980` (chuẩn Desktop Viewport fallback của WebKit).
    * Áp dụng scale tỷ lệ để trang 980px vừa khít với màn hình `CONTENT_W`.

### Giai đoạn 3: Đánh Giá Có Chọn Lọc CSS `@media` (Smart Media Query Filter)
Thay vì xóa bỏ vô điều kiện `@media`, bộ lọc CSS sẽ kiểm tra điều kiện chiều rộng:
* Parse các biểu thức cơ bản:
  * `(max-width: Npx)`: Chỉ kích hoạt nếu `effectiveWidth <= N`.
  * `(min-width: Npx)`: Chỉ kích hoạt nếu `effectiveWidth >= N`.
  * `screen and (max-width: Npx)`
* **Lợi ích ngay lập tức**:
  * Các khối CSS cho màn hình máy tính lớn (`min-width: 1200px`, `min-width: 1400px`) sẽ **bị loại bỏ hoàn toàn**!
  * Các khối CSS tối ưu cho thiết bị cầm tay (`max-width: 768px`, `max-width: 992px`, `max-width: 1024px`) sẽ được giữ lại và áp dụng đầy đủ.
  * Trang web sẽ hiển thị sạch sẽ, chữ to rõ ràng, hình ảnh và khung bài không bao giờ bị tràn sang hai bên.

### Giai đoạn 4: Đảm bảo Container Clamping
* Trong hàm `layoutElement()`:
  * Mọi phần tử khối có chiều rộng cố định vượt quá `layoutW` (ví dụ `width="1200"` trong thuộc tính HTML hoặc CSS inline) sẽ được giới hạn tự động:
    `el.width = std::min(el.width, layoutW);`
  * Tránh tình trạng một bảng tính hoặc một banner quảng cáo làm đẩy toàn bộ trang web lệch sang một bên.

---

## 6. Lộ Trình Triển Khai

| Bước | Hạng Mục | Mô Tả Kỹ Thuật | Trạng Thái |
| :--- | :--- | :--- | :--- |
| **P1** | **Viewport Parser** | Nhận diện thẻ `<meta name="viewport">` trong `parseHtml()` và lưu thông số vào `m_viewport`. | Sẵn sàng |
| **P2** | **Media Query Filter** | Cải tiến hàm xử lý CSS để lọc đúng các block `@media (max-width/min-width)` thay vì nạp tràn lan. | Sẵn sàng |
| **P3** | **Responsive Layout Scale** | Điều chỉnh `layoutW` và font scale thông minh dựa trên `isDeviceWidth`. | Sẵn sàng |
| **P4** | **Kiểm thử Thực tế** | Test với các trang web mẫu: VnExpress (mobile), Wikipedia (mobile), Game Vui, DuckDuckGo. | Sẵn sàng |
