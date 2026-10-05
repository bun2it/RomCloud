# Kế hoạch tối ưu chức năng Dọn dẹp bộ nhớ đệm (Cleanup)

> Audit ngày 2026-10-03. Code hiện tại: `UIManager::cleanCache()`
> (`src/ui/UIManager.cpp:4254`), `getDirSize`/`removeDirContents` (`:4191`, `:4214`).
> Trạng thái: CHƯA LÀM — duyệt rồi mới implement.

## 1. Hiện trạng

Dọn: `cache/covers`, `cache/metadata`, `/tmp/yt_thumbs` + xóa ping cache
và thumb memory. Đếm/dọn bằng 2 hàm đệ quy tự viết.

## 2. Bug phải sửa (P0)

`stat()` đi xuyên symlink → đếm sai số, xóa nhầm file NGOÀI cache,
đệ quy vô hạn với symlink vòng (a↔b) → treo/crash.
- Đổi sang `lstat()` ở cả 2 hàm; gặp symlink thì bỏ qua
  (hoặc chỉ `unlink` link, không traverse).
- Check return của `unlink/rmdir` để số freed trung thực.

## 3. Mục dọn còn thiếu (P1)

| # | Nguồn rác | Vị trí | Ghi chú |
|---|-----------|--------|---------|
| 1 | File tải game dở `.part` | `data/temp/` | Có thể tới GB — món hời nhất |
| 2 | Log mpv mỗi lần play | `/tmp/mpv_*.log` | `/tmp` là RAM, ăn thẳng 1GB máy |
| 3 | Log cũ | `data/logs` (giữ file đang viết) | Logger chỉ rotate 512KB + 1 file `.old` |
| 4 | OTA dở dang | `.ota_tmp`, `.ota_backup`, `ota_update.zip` | Sót khi lỗi giữa chừng |
| 5 | Cover memory | `CoverManager`/`ImageCache` RAM | Xóa kèm để đồng bộ với xóa covers đĩa |

Không đụng: `data/library.db`, `config/` (token/key), `iptv/` (playlist,
yêu thích), socket `/tmp/*.sock`, `/tmp/gamecast.pid`, `/tmp/stay_awake`.

## 4. UX (P2)

- Hộp xác nhận trước khi dọn (nguyên tắc: thao tác nào cũng xác nhận).
- Chạy nền qua `BackgroundTask` + progress dialog có sẵn (hiện đang
  blocking UI thread — cache to là đơ máy).
- Hiển thị chi tiết freed theo nhóm (temp XXMB, covers YYMB...).
- Số `Chiếm dụng:` tính lại mỗi lần vào tab Cài đặt (hiện bị stale).

## 5. Test

- Symlink unit: link ra ngoài + link vòng → dọn không mất file ngoài,
  không treo.
- Giả lập `.part` 1GB + 5000 file covers → đo thời gian, không đơ UI.
- Trên máy thật qua adb: dọn xong check YouTube/IPTV/OTA vẫn chạy
  (không xóa nhầm lib/script/asset).

## 6. Release

Gộp vào bản minor tiếp theo (không cần patch riêng).
Sau release: cập nhật USER_MANUAL mục Dọn dẹp.
