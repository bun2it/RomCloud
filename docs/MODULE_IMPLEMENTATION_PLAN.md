# MODULE IMPLEMENTATION PLAN — Tách chức năng tái dùng thành module riêng lẻ

> Mục đích: app gọn nhẹ, không code thừa, không lặp lại, tối ưu tốc độ.
> Phạm vi: RomCloud (C++17, SDL2, TrimUI Brick Pro 1024x768, build `build.sh` bằng zig c++ aarch64).
> Ngày lập: 2026-09-30. Trạng thái: ĐANG TRIỂN KHAI (xong P1-1 BackgroundTask + P1-2 FileOps + P0-2 VirtualKeyboard + P0-3 DialogManager + P2-1 FileExplorer (logic header-only), chưa thay UIManager, chưa build full trên thiết bị).

---

## 1. Chẩn đoán hiện trạng (từ code thật)

- `src/ui/UIManager.cpp` ~6.676 dòng / ~351 KB + `UIManager.h` ~16 KB: mọi màn hình
  (Menu, Game, IPTV, YouTube, TikTok, LocalSend...) nhét render + input + logic
  nghiệp vụ vào 1 class.
- Hệ quả: code lặp, khó tái dùng cho File Explorer, build chậm, sửa 1 chỗ dễ vỡ chỗ khác.

### 1.1. Các cụm lặp lại đã xác định

| # | Cụm lặp lại | Bằng chứng trong code | Số nơi lặp | Module tách ra |
|---|---|---|---|---|
| 1 | Bàn phím ảo QWERTY + Telex | `m_iptvKbRow/Col`, `m_ytKbRow/Col/Shift`, `m_lsFolderKbRow/Col/Shift`, `renderLsFolderKeyboard()`, `TelexHelper.h` | 5+ | `VirtualKeyboard` |
| 2 | Confirm dialog + Toast + Progress overlay | `renderConfirmDeleteDialog()`, `renderConfirmBatchDeleteDialog()`, `showToast()/renderToast()`, `renderSyncOverlay()`, `renderDownloadOverlay()`, `renderUploadOverlay()`, `renderLocalSendProgress()` | 8+ | `DialogManager` |
| 3 | Primitive vẽ UI | ~25 hàm `draw*` (drawText/Rect/Badge/Pill/Button/Row/FooterHints/PadIcon/AppHeader/AppFooter...) | Mọi màn hình | `UiRenderer` |
| 4 | mpv fork + blocking loop + IPC | `IPTVManager.cpp`, `TikTokManager.cpp`, `playYouTubeVideo()`, `PLAN.md` | 3 | `MpvPlayer` |
| 5 | File ops thiếu | `FileSystemManager` chỉ có `listDirectory/createDirectoryRecursive/removeFile` — thiếu rename/copy/xóa đệ quy/stats | 2+ nơi cần | `FileOps` |
| 6 | List 2 cột Master-Detail + footer tay cầm | `renderLocalSendFolderPicker()`, `renderGameListState()`, `renderIPTVState()`, `renderLsRomsList/AppsList` | 4+ | `FileListView` |
| 7 | Thumbnail/cover cache rời rạc | `m_gridIconCache`, `m_systemIconCache`, `m_buttonIconCache`, `m_ytThumbnails`, `CoverManager` | 3 cơ chế | `ImageCache` |
| 8 | Tác vụ nặng chạy trên UI thread | Download/OTA/Sync/progress mỗi nơi 1 kiểu | 5+ | `BackgroundTask` |

### 1.2. Nguyên tắc tách

1. Module mới độc lập, không phụ thuộc UIManager; UIManager chỉ điều phối state + gọi module.
2. Tách dần theo P0 → P1 → P2, mỗi bước build + test được, app luôn chạy.
3. Không nhét UI/thread vào `FileSystemManager`; worker thread nằm ở `BackgroundTask`.
4. Giữ `UiTheme.h` + `UiStrings.h` nguyên (đã tốt — Single Source of Truth).

---

## 2. Danh mục module (8 module)

### P0-1. `UiRenderer` (vẽ primitive, không logic)
- **File mới:** `src/ui/UiRenderer.h`, `src/ui/UiRenderer.cpp`
- **Việc làm:**
  - Chuyển ~25 hàm `draw*` từ UIManager sang `UiRenderer` (giữ nguyên chữ ký).
  - UIManager giữ 1 instance `UiRenderer m_ui;`, gọi `m_ui.drawRow(...)`.
- **Xóa khỏi UIManager:** toàn bộ khối `draw*` (~1.500 dòng).
- **Kiểm chứng:** build `./build.sh`, boot app, duyệt menu/game/IPTV không vỡ layout.
- **Lợi ích:** mọi màn hình + File Explorer dùng chung 1 bộ vẽ, giảm ~1.500 dòng UIManager.

### P0-2. `VirtualKeyboard` (dùng ngay cho Create Folder / Rename)
- **Trang thai:** DONE phan LocalSend folder (VkState m_lsFolderVk thay m_lsFolderKbRow/Col/Shift + m_lsFolderRenameText); con lai Search/YouTube/IPTV/TikTok giu nguyen de lam tiep.
- **File mới:** `src/ui/VirtualKeyboard.h`, `src/ui/VirtualKeyboard.cpp`
- **API:** `struct VkState { string query; int row=0, col=0; bool shift=false, inResults=false; };`
  `handleInput(VkState&)` (D-pad di chuyển, A chọn, B thoát, X xóa, Y shift),
  `render(VkState&, x, y, w, h)` — tích hợp sẵn `TelexHelper::processTelex`.
- **Việc làm:** xóa 5 bộ `m_*KbRow/Col/Shift` trong UIManager, thay bằng 1 `VirtualKeyboard`.
- **Kiểm chứng:** gõ Telex ở Search + YouTube + IPTV + TikTok.
- **Lợi ích:** xóa ~800 dòng lặp, gõ tiếng Việt 1 nơi duy nhất.

### P0-3. `DialogManager` (confirm / toast / progress OSD)
- **File mới:** `src/ui/DialogManager.h`, `src/ui/DialogManager.cpp`
- **API:** `confirm(title, body, onOk)`, `toast(msg, color, durationMs)`,
  `progress(taskId, done, total, onCancel)`, `render()`, `handleInput()`.
- **Việc làm:** gom `renderConfirm*`, `showToast/renderToast`, `render*Overlay` về 1 nơi.
- **Kiểm chứng:** dialog xóa ROM + toast + progress download/OTA.
- **Lợi ích:** File Explorer dùng ngay popup xóa + progress copy GB mà không copy code.

### P1-1. `BackgroundTask` (worker thread chuẩn cho tác vụ nặng)
- **File mới:** `src/common/BackgroundTask.h` (header-only)
- **API:** `struct TaskProgress { atomic<uint64_t> done, total; atomic<bool> cancel, finished; string error; };`
  `run(fn)` (std::thread worker), `progress()`, `requestCancel()`.
- **Việc làm:** dùng chung cho copy/paste Explorer, DownloadManager, OTA, DriveSync,
  thumbnail download. UI poll mỗi frame vẽ progress (tái dùng pattern `renderSyncOverlay`).
- **Kiểm chứng:** copy folder vài GB không đơ UI; nút B hủy tác vụ.
- **Lợi ích:** chống đơ UI, 1 pattern hủy/tiến trình duy nhất.

### P1-2. `FileOps` (mở rộng FileSystemManager, thuần filesystem)
- **File sửa:** `src/filesystem/FileSystemManager.h`, `src/filesystem/FileSystemManager.cpp`
- **Thêm 5 hàm:** `renamePath(from, to)`, `removeRecursive(path, prog)`,
  `copyRecursive(src, dst, prog)`, `getDirStats(path)` (bytes/files/dirs),
  `getModTime(path)`.
- **Việc làm:** duyệt đệ quy (opendir/readdir), đếm trước tổng item để báo progress;
  chặn copy vào con của chính nó. Không nhét UI/thread vào đây.
- **Kiểm chứng:** test rename/copy/xóa đệ quy trên PC + thiết bị.
- **Lợi ích:** Explorer + LocalSend + Backup dùng chung.

### P1-3. `MpvPlayer` (gom 3 loop mpv thành 1)
- **File mới:** `src/media/MpvPlayer.h`, `src/media/MpvPlayer.cpp`
- **API:** `play(url, extraArgs)`, `sendCmd(json)` (vd `{"command":["seek","10","relative"]}`),
  `showText(text, ms)`, `pollExit()`, `stop()`.
- **Việc làm:** chuyển fork mpv + `--input-ipc-server` + blocking loop + IPC
  (loadfile/seek/show-text) từ IPTV/TikTok/YouTube về 1 nơi.
  Các manager chỉ còn cung cấp URL + mapping nút tay cầm.
- **Kiểm chứng:** phát IPTV/YouTube/TikTok, seek/volume/OSD bình thường.
- **Lợi ích:** xóa ~600 dòng copy-paste. Rủi ro trung bình → làm sau P0/P1-1/P1-2.

### P2-1. `FileExplorer` + `FileListView` (màn hình mới)
- **File mới:** `src/fileexplorer/FileExplorer.h/.cpp`, `src/ui/FileListView.h/.cpp`
- **Việc làm:**
  - `FileListView`: list 2 cột Master-Detail (trái entries, phải properties),
    footer tay cầm — tái dùng cho cả Explorer và LocalSend picker.
  - `FileExplorer`: duyệt thư mục + clipboard 2 bước (X=Cut, Y=Copy, R1/START=Paste,
    SELECT=hủy) + filter (ẩn `.DS_Store/Thumbs.db/._*`, whitelist ROM/media).
    Gọi `VirtualKeyboard` + `DialogManager` + `BackgroundTask` + `FileOps`, không viết lại.
  - `UIState::FILE_EXPLORER` mới + icon grid menu mới.
- **Mapping tay cầm:** UP/DOWN chọn, A vào folder, B lùi, X/Y Cut/Copy, R1 Paste,
  L1 menu (New folder/Rename/Delete/Properties/Filter), SELECT hủy clipboard.
- **Kiểm chứng:** đủ CRUD + clipboard file lẻ và folder lớn + properties + filter + progress.

### P2-2. `ImageCache` (gom cache ảnh)
- **File mới:** `src/ui/ImageCache.h` (header-only, LRU + `lastUsed`)
- **Việc làm:** gom `m_gridIconCache`, `m_systemIconCache`, `m_buttonIconCache`,
  `m_ytThumbnails`, cache trong `CoverManager` về 1 nơi; tái dùng pattern `clearTextCache`.
- **Kiểm chứng:** cuộn game list nhanh không leak, FPS 60.
- **Lợi ích:** 1 cơ chế cache, dễ giới hạn RAM trên handheld.

---

## 3. Thứ tự triển khai

| Bước | Module | Kiểm chứng |
|---|---|---|
| 1 | `UiRenderer` | build + boot/menu, duyệt menu/game/IPTV không vỡ layout |
| 2 | `VirtualKeyboard` | gõ Telex Search/YouTube/IPTV |
| 3 | `DialogManager` + `BackgroundTask` | xóa ROM + download progress + hủy |
| 4 | `FileOps` | rename/copy/xóa đệ quy |
| 5 | `FileExplorer` + `FileListView` | CRUD + clipboard tay cầm |
| 6 | `MpvPlayer`, `ImageCache` | phát 3 nguồn + FPS/cuộn list |

Mỗi bước: thêm file `.cpp` mới vào `build.sh` (lệnh `zig c++`), chạy `./build.sh`,
đo `ls -lh bin/RomCloud` + đếm dòng `UIManager.cpp` để xác nhận gọn đi.

## 4. Trade-off

- **Tách dần (khuyến nghị)** vs viết lại UIManager 1 lần: tách dần an toàn,
  app vẫn chạy sau mỗi bước; viết lại 1 lần rủi ro vỡ toàn bộ UI.
- **Header-only cho BackgroundTask/ImageCache** vs .cpp riêng: header-only gọn,
  build chậm hơn chút — chấp nhận được vì 2 module nhỏ.
- `MpvPlayer`/`ImageCache` để cuối vì chạm playback và render toàn app.

## 5. Checklist nghiệm thu

- [ ] Không còn hàm `draw*` trong UIManager (nằm ở `UiRenderer`).
- [x] 1 `VirtualKeyboard` cho mọi màn hình nhập liệu (xong logic `src/ui/VirtualKeyboard.h`, test Telex pass, chưa thay UIManager).
- [x] 1 `DialogManager` cho confirm/toast/progress (xong logic `src/ui/DialogManager.h`, chưa thay UIManager).
- [x] Copy/xóa GB chạy nền + progress + hủy được (`BackgroundTask` — xong code `src/common/BackgroundTask.h`, chưa build).
- [x] `FileOps` đủ rename/copy/xóa đệ quy/stats/mtime (xong code `FileSystemManager.*`, chưa build).
- [ ] Explorer CRUD + clipboard + filter + properties chạy bằng tay cầm, không chuột.
- [ ] IPTV/YouTube/TikTok phát qua `MpvPlayer` chung.
- [ ] `bin/RomCloud` build thành công qua `./build.sh`, app boot và duyệt menu bình thường.

## 6. Tham khảo

- `src/ui/UIManager.cpp` (~6.676 dòng) — nguồn các cụm lặp.
- `src/ui/UiTheme.h`, `src/ui/UiStrings.h` — giữ nguyên.
- `src/ui/TelexHelper.h` — dùng trong `VirtualKeyboard`.
- `src/filesystem/FileSystemManager.*` — mở rộng thành `FileOps`.
- `src/iptv/IPTVManager.cpp`, `src/iptv/TikTokManager.cpp`, `PLAN.md` — nguồn gom `MpvPlayer`.
- `src/ui/CoverManager.*`, `src/ui/BoxartScraper.cpp` — nguồn gom `ImageCache`.
- `build.sh` — thêm file `.cpp` mới vào lệnh `zig c++`.
