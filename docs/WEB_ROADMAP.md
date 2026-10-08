# Web Browser — Cái còn thiếu + plan phát triển

Ngày: 2026-10-07. Đã xong: P0–P15 (engine, CSS, reader, table, media, GIF,
Settings, block-nav, 4 web mẫu), batch Gemini (headers/viewport/media/align/
gaps/img-sizing). Test: 144+ PASS. File chi tiết: `WEB_BROWSER_DONE.md`.

## I. Còn thiếu (xếp theo giá trị/công sức)

### Ngay (nhỏ, làm trong ngày)
1. **P11.2 Tác giả + giờ đăng**: dòng byline dưới tiêu đề bài viết
   (`meta article:published_time`, `.author_mail`). ~2h.
2. **P13.3 Video poster**: `<video poster>` hiện poster như ảnh thay vì
   hộp xám. ~1h.
3. **YouTube → app native**: URL `youtube.com/watch?v=` trong browser hiện
   nút "Mở bằng app YouTube" (dùng sẵn `resolveYouTubeStreamUrl` +
   mpv). Không cần JS trong browser. ~4h.
4. **Border CSS**: `border:1px solid #eee` → viền ô (table đã có viền
   riêng, còn lại chưa). ~2h.

### Vừa (vài ngày)
5. **P14.4 Float**: `float:left/right` cho ảnh (báo hay đặt ảnh giữa bài,
   chữ chảy quanh). Hiện ảnh chiếm nguyên hàng → "lệch hình". ~2–3 ngày.
6. **colspan/rowspan**: table báo giá. Kèm log tần suất trước. ~1–2 ngày.
7. **`background-image` CSS**: hiện bỏ qua → banner trang trắng trơn.
   Tải như ảnh thường + vẽ nền (không cần co giãn chuẩn). ~1–2 ngày.

### Lớn (PoC đo trên máy, giữ nếu qua)
8. **SVG raster**: parse đã có libsvgtiny trong tree nhưng chỉ ra path —
   cần trình tô + build ARM + đo. Giá trị thấp (logo/icon). Chưa bắt đầu.
9. **JS engine**: Duktape + DOM bindings tối thiểu (form portal, nút
   "xem thêm"). Rất lớn, chưa bắt đầu. Không JS thì YouTube/TikTok
   trong browser mãi trắng.
10. **Engine đầy đủ** (NetSurf/litehtml): sysroot đã có libcss/dom/hubbub
    build sẵn, nhưng frontend + form + chi phí RAM chưa đo. Chưa bắt đầu.

## II. Nợ verify trên máy (cần cáp + tay test)
- GIF FPS/RAM thực tế khi bật animation (log `IMG_Init` xem webp=yes/no).
- Settings UI/SELECT, article mode, table, phát mp4 từ trang.
- 4 web mẫu mới (nhất là m.youtube/tiktok có vào được không).
- Báo cáo user đang dở: CSS vỡ layout/lệch ảnh còn lại sau batch Gemini.

## III. Thứ tự đề xuất
1 → 2 → 3 → 4 (xong trong 1–2 buổi) → 5 → 6 → 7 (khi rảnh) → 8/9/10
chỉ khi có số đo PoC. Song song: mỗi lần cắm cáp là verify hết nợ cũ.
