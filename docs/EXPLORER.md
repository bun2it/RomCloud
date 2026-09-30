# Kế Hoạch: File Explorer Tối Ưu Tay Cầm cho RomCloud

## 1. Khảo sát hiện trạng (đã xong)

| Thành phần                              | Hiện có                                                                                                                                                                                                       | Tái dùng được                                              |
| --------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------- |
| `FileSystemManager` (`src/filesystem/`) | `listDirectory()` (sort dirs-first), `createDirectoryRecursive()`, `removeFile()` (chỉ file lẻ), `getFileSize()`, `getDiskSpace()`, `formatBytes()`                                                           | `listDirectory`, `createDirectoryRecursive`, `formatBytes` |
| `UIManager` + `UIState`                 | `CONFIRM_DELETE`, `CONFIRM_BATCH_DELETE` dialog, `renderLsFolderKeyboard()` (QWERTY như YouTube), `renderLocalSendFolderPicker()` (explorer 2 cột Master-Detail), toast, `drawRow`, `drawFooterHintsCentered` | Pattern dialog xác nhận, bàn phím ảo, layout 2 cột         |
| `TelexHelper`                           | Gõ tiếng Việt Telex cho bàn phím ảo                                                                                                                                                                           | Dùng nguyên cho Create/Rename                              |
| `InputManager`                          | Nút `UP/DOWN/LEFT/RIGHT, A/B/X/Y, L1/R1, START/SELECT` + hold-repeat                                                                                                                                          | Mapping tay cầm có sẵn                                     |
| Build                                   | `build.sh` (zig c++ aarch64, thêm file `.cpp` vào list là build được)                                                                                                                                         | —                                                          |

**Thiếu (phải viết mới):** rename file/folder, xóa đệ quy folder, copy/cut/paste (cả folder), clipboard 2 bước, worker thread + progress OSD, properties panel, filter rác.

## 2. Kiến trúc đề xuất

**Phương án A (khuyến nghị): Module mới + state mới, không phá code cũ**

- `src/filexplorer/FileExplorer.h/.cpp` — class `FileExplorer` độc lập: duyệt thư mục, clipboard, tác vụ nền. `UIManager` chỉ gọi `render()` + `handleInput()`.
- `FileSystemManager` chỉ thêm 4 hàm thuần filesystem: `renamePath()`, `removeRecursive()`, `copyRecursive()`, `getDirStats()` (đếm item + dung lượng). Không nhét logic UI/thread vào đây.
- `UIState::FILE_EXPLORER` mới + 2 sub-mode nội bộ: `BROWSER` và `KEYBOARD` (nhập tên), tái dùng `CONFIRM_DELETE` pattern cho popup xóa.

**Phương án B (loại): nhét hết vào UIManager** — file `UIManager.cpp` đã ~7395 dòng, thêm nữa sẽ khó bảo trì.

## 3. Kế hoạch chi tiết theo 3 nhóm yêu cầu

### Nhóm 1 — CRUD

1. `FileSystemManager::renamePath(old, new)` — wrapper `rename()`, kiểm tra trùng tên, trả lỗi.
2. `FileSystemManager::removeRecursive(path)` — duyệt `nftw`/đệ quy, xóa file trước rồi rmdir; đếm trước tổng số item để báo progress.
3. Create Folder: tái dùng bàn phím QWERTY của `renderLsFolderKeyboard()` + `suggestNewFolderName()` (NewFolder, NewFolder_2...), validate ký tự cấm `/`.
4. Popup xác nhận xóa: tái dùng `renderConfirmDeleteDialog()` — hiển thị tên + dung lượng + số item con, `[A] Xóa / [B] Hủy`.

### Nhóm 2 — Clipboard 2 bước (quan trọng nhất cho tay cầm)

```cpp
struct Clipboard { enum Op { NONE, COPY, CUT }; Op op; std::string srcPath; bool srcIsDir; };
```

1. `X` = Cut, `Y` = Copy (đánh dấu item, hiện badge "CUT/COPY" trên row + toast).
2. Điều hướng tới thư mục đích → `A (giữ)` hoặc nút Paste trên footer → `paste(destDir)`:
   - COPY: `copyRecursive(src, dest/basename)`; CUT: `rename()` nếu cùng ổ đĩa (nhanh), ngược lại copy-then-delete.
   - Chặn paste vào chính nó / vào con của chính nó (tránh vòng lặp vô hạn).
   - Xử lý cả file lẻ (`.gba/.zip/.mp4`) lẫn folder lớn (save game, ROM set).
3. `B` hoặc `SELECT` = hủy clipboard.

**Mapping nút đề xuất (không chuột):**
| Nút | Hành động |
|---|---|
| UP/DOWN | Di chuyển chọn |
| A | Vào folder / mở |
| B | Lùi folder cha |
| X / Y | Cut / Copy |
| R1 (hoặc START) | Paste |
| L1 | Menu thao tác (New folder / Rename / Delete / Properties / Filter) |
| SELECT | Hủy clipboard / đổi filter |

### Nhóm 3 — Tiện ích handheld

1. **Properties (Master-Detail):** cột phải hiện sẵn `formatBytes(size)`, `mtime` (thêm `getModTime()`), với folder thì `getDirStats()` đếm đệ quy (cache kết quả vì tốn I/O).
2. **Filter:** toggle `Ẩn file rác` (`.DS_Store, Thumbs.db, ._*`) + `Chỉ ROM/media` (whitelist `.gba/.gbc/.gb/.nes/.snes/.zip/.7z/.mp4/.mkv...`). Dùng `SELECT` hoặc L1-menu để đổi, lưu vào settings.
3. **Background task + Progress OSD:** `std::thread` worker + `std::atomic<uint64_t> done/total` + `atomic<bool> cancel`. UI poll mỗi frame vẽ progress bar (tái dùng `renderSyncOverlay`/`renderDownloadOverlay` pattern). `B` = hủy tác vụ. Tuyệt đối không chạy copy/xóa GB trên UI thread.

## 4. Files sẽ tạo/sửa (khi sang Act mode)

- **Mới:** `src/filexplorer/FileExplorer.h`, `src/filexplorer/FileExplorer.cpp`
- **Sửa:** `FileSystemManager.h/.cpp` (+4 hàm), `UIManager.h` (+state + fields), `UIManager.cpp` (+render/input), `UiStrings.h` (+chuỗi), `build.sh` (+2 file cpp), `initGridMenu()` (+icon Explorer).

## 5. Thứ tự triển khai + kiểm thử

1. Filesystem ops → 2. Browser UI + điều hướng → 3. CRUD + keyboard → 4. Clipboard → 5. Properties/Filter → 6. Worker thread + progress → build `./build.sh` mỗi bước.

Bạn đã chọn **Menu độc lập** — tôi sẽ đặt icon Explorer mới trong grid menu, đồng thời tách `FileExplorer` thành module riêng để sau này LocalSend picker có thể tái dùng.
