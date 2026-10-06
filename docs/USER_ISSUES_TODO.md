# User-Reported Issues — TODO (chưa xử lý)

Nguồn: https://github.com/bun2it/Romcloud_Logs/issues — kiểm tra ngày 2026-10-06.
Trạng thái: **CHƯA SỬA** — chờ chỉ thị.

---

## 🔴 #34 — OTA v2.4.0 → v2.4.1 lỗi "Giải nén bản cập nhật thất bại"

- **Máy:** RC-2a7c3687 · v2.4.0 · Stock (PS)
- **Hiện tượng:** tải full-zip xong (~23s) → fail giải nén. Thử 2 lần, cả 2 đều fail.
- **Đã loại trừ:**
  - Zip release `RomCloud-v2.4.1.zip` OK (`unzip -t` sạch, layout `Apps/RomCloud/...`).
  - Cấu trúc/nén giống hệt v2.4.0 — máy này từng OTA 2.3.2 → 2.4.0 thành công.
- **Nghi vấn:** lỗi phía máy user (thẻ nhớ đầy → zip cụt, hoặc `unzip` trả exit 1 = warning bị coi là fail).
- **Vấn đề code:** [installFullZip](../src/ota/UpdateManager.cpp) (L845-855)
  - `2>/dev/null` nuốt hết stderr của `unzip` → không biết lý do.
  - Không check dung lượng trống trước khi tải.
  - Không so kích thước file tải về với `sizeBytes` (chỉ check < 1MB).

### Fix đề xuất
- [ ] `statvfs` appRoot trước khi tải: cần ≥ ~3× size zip, thiếu → báo "Thẻ nhớ không đủ dung lượng".
- [ ] So `downloadedSize` với `info.sizeBytes` (nếu > 0), lệch → báo tải lỗi.
- [ ] Ghi stderr `unzip` ra file tạm, log 10 dòng cuối khi fail.
- [ ] Chấp nhận exit code 1 nếu `staged` tồn tại và có `bin/RomCloud`.
- [ ] Fallback thứ 3: `bin/7zzs x` (đã có sẵn trong gói).
- [ ] Hỏi user dung lượng trống thẻ nhớ để xác nhận nguyên nhân.

---

## 🟠 #33 — GameCast daemon không khởi động

- **Máy:** RC-cfb54f60 · v2.4.0 · Stock (PS)
- **Hiện tượng:** bật 3 lần, đều "GameCast daemon failed to start" sau 1.2s.
- **Đã loại trừ:**
  - Thiếu thư viện: `gamecast_d` chỉ cần libc/libdl/libpthread.
  - Lỗi mở `/dev/fb0`: chỉ dừng capture thread, process vẫn sống (main kẹt ở `accept`).
- **Nghi vấn còn lại:** bind port 8090 fail (process khác giữ port), crash, hoặc không exec được (`nice`/quyền).
- **Vấn đề code:** [CastManager::start](../src/cast/CastManager.cpp) (L186) không đọc `/tmp/gamecast.log` → report không có lý do.

### Fix đề xuất
- [ ] Khi fail: đọc tail `/tmp/gamecast.log` → `Logger::warn` + toast lý do ngắn.
- [ ] Nếu bind fail: log process đang giữ port 8090 (quét `/proc/net/tcp`).
- [ ] Fallback chạy không qua `nice` nếu lần đầu fail.

---

## 🟡 Vấn đề phụ

- [ ] `[STATE] SET/BACK` log mức **ERROR** ([UIManager.cpp](../src/ui/UIManager.cpp) L361, L382) → hạ xuống debug. Đang làm nhiễu log và chiếm budget 3KB report.
- [ ] Worker [filter.ts](../deploy/issue-relay/src/filter.ts) L92 `replace(/\s+/g,' ')` gộp log thành 1 dòng → chỉ gộp space/tab, giữ `\n`. Cần `wrangler deploy` lại.
- [ ] Giờ máy user lệch ~1h so với GitHub (cả 2 máy) — kiểm tra timezone/NTP trên Stock, không gây lỗi.
