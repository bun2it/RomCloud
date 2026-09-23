# RomCloud - AI Agent Development Guide

## 🎯 Mục tiêu
Tài liệu này hướng dẫn các AI agent tự động:
1. Đọc GitHub Issues hàng ngày
2. Phân tích và chẩn đoán lỗi
3. Tự động sửa lỗi
4. Đẩy phiên bản mới lên GitHub

---

## 📋 Cấu trúc Repository

```
RomCloud/
├── src/                    # Source code C++
│   ├── app/               # Application entry
│   ├── ui/                # UI rendering
│   ├── network/            # HTTP, WebServer
│   ├── database/           # SQLite operations
│   ├── sync/              # Drive sync
│   ├── download/           # ROM download
│   ├── ota/               # OTA updates
│   ├── auth/              # Google auth
│   ├── logging/           # Logger, IssueLogger
│   ├── config/            # AppConfig
│   ├── platform/           # Platform detection
│   ├── filesystem/         # File operations
│   ├── input/              # Gamepad input
│   ├── backup/            # Backup/Restore
│   └── rom/               # ROM detection/organize
├── assets/                 # Fonts, icons
├── config/                # Default config
├── build.sh               # Build script (zig c++)
├── launch.sh              # Launch script
├── version.json           # Version manifest
└── package.sh             # Package for release
```

---

## 🔄 Quy trình tự động của Agent

### Bước 1: Đọc Issues mới
```bash
# Liệt kê issues chưa đóng
gh issue list --repo bun2it/RomCloud --state open --limit 20

# Xem chi tiết issue
gh issue view <issue_number> --repo bun2it/RomCloud
```

### Bước 2: Phân tích lỗi
- Đọc `system_info` và `error_message` từ issue body
- Kiểm tra code liên quan trong `src/`
- Chạy build để xác nhận lỗi

### Bước 3: Sửa lỗi
1. Edit file(s) liên quan
2. Build: `./build.sh`
3. Test (nếu có thể)
4. Commit với format: `fix: <mô tả ngắn> #<issue_number>`

### Bước 4: Đẩy phiên bản mới
```bash
# 1. Tăng version trong src/ota/UpdateManager.h
#    constexpr const char* APP_VERSION = "X.Y.Z";

# 2. Cập nhật version.json
{
  "version": "X.Y.Z",
  "release_date": "YYYY-MM-DD",
  "binary_url": "https://github.com/bun2it/RomCloud/releases/download/vX.Y.Z/RomCloud",
  "changelog": "Mô tả thay đổi"
}

# 3. Build
./build.sh

# 4. Tạo release
gh release create vX.Y.Z --title "vX.Y.Z" --notes "<changelog>"

# 5. Upload binary
gh release upload vX.Y.Z bin/RomCloud
gh release upload vX.Y.Z launch.sh

# 6. Commit và push
git add -A
git commit -m "vX.Y.Z: <mô tả>"
git push origin main
```

### Bước 5: Đánh dấu đã sửa
```bash
# Đóng issue với comment
gh issue close <issue_number> --comment "Đã sửa trong vX.Y.Z"
```

---

## 📦 Build System

### Build Command
```bash
./build.sh
```
- Sử dụng Zig compiler cho aarch64-linux-gnu
- Output: `bin/RomCloud`

### Dependencies (trong sysroot/)
- SDL2, SDL2_image, SDL2_ttf
- sqlite3, curl, ssl, crypto

### Package cho Release
```bash
./package.sh
```
- Tạo file zip trong `dist/`
- Bao gồm: binary, launch.sh, fonts, icons, config

---

## 🔧 Các Module chính

### UIManager (src/ui/UIManager.cpp)
- Vẽ tất cả UI elements
- States: MENU, SYSTEM_SELECT, GAME_LIST, SETTINGS, OTA_UPDATE, etc.
- Key methods: `render*()`, `drawText()`, `drawRect()`, `drawBadge()`, `drawIcon()`

### PlatformInfo (src/platform/PlatformInfo.cpp)
- Detect display resolution, aspect ratio
- Scale UI cho màn hình khác nhau
- System info: CPU, RAM, storage

### OTA Update (src/ota/UpdateManager.cpp)
- Kiểm tra update từ version.json / GitHub Releases API
- Download binary, verify, install

### Logger (src/logging/Logger.cpp)
- Ghi log ra file
- Levels: DEBUG, INFO, WARN, ERROR

### IssueLogger (src/logging/IssueLogger.cpp)
- Tạo GitHub Issues tự động khi crash/error
- Cần GitHub token trong config/github_token

---

## 🎨 UI Strings

Tất cả text UI trong: `src/ui/UiStrings.h`
- Sửa text → build lại
- Format: `inline const char *NAME = "text";`

---

## 🔍 Debug Tips

### Xem log trên thiết bị
```bash
cat /mnt/SDCARD/Apps/RomCloud/logs/romcloud.log
```

### Check version hiện tại
```bash
strings /mnt/SDCARD/Apps/RomCloud/bin/RomCloud | grep "1\.[0-9]\.[0-9]"
```

### Test network
```bash
curl -I https://api.github.com/repos/bun2it/RomCloud/releases/latest
```

---

## 📝 Issue Format (từ RomCloud app)

Khi user báo lỗi từ app, issue body sẽ có format:

```markdown
## Lỗi được báo cáo từ thiết bị

### Mô tả lỗi
```
<error_message>
```

### Thông tin thiết bị
- **App Version:** vX.Y.Z
- **Device:** <device>
- **OS:** Linux <kernel>
- **Display:** <resolution>
- **RAM:** <free> / <total>
- **Storage:** <free> / <total>
- **Network:** <status>
```

---

## ⚙️ Configuration

### Cài đặt trên thiết bị
- Config: `/mnt/SDCARD/Apps/RomCloud/config/settings.json`
- Database: `/mnt/SDCARD/Apps/RomCloud/data/romcloud.db`

### GitHub Token (cho IssueLogger)
- File: `/mnt/SDCARD/Apps/RomCloud/config/github_token`
- Hoặc set trong database: `screenscraper_user`, `github_token`

---

## 🚀 Checklist khi fix Issue

- [ ] Đọc và hiểu issue
- [ ] Xác định file cần sửa
- [ ] Edit code
- [ ] Build: `./build.sh`
- [ ] Update version: `src/ota/UpdateManager.h` và `version.json`
- [ ] Test (nếu có thể)
- [ ] Commit: `fix: <mô tả> #<issue>`
- [ ] Push: `git push origin main`
- [ ] Tạo release: `gh release create vX.Y.Z ...`
- [ ] Upload binary: `gh release upload vX.Y.Z bin/RomCloud`
- [ ] Close issue: `gh issue close <num> --comment "Fixed in vX.Y.Z"`

---

## 📚 Tham khảo

- RomCloud Repo: https://github.com/bun2it/RomCloud
- Releases: https://github.com/bun2it/RomCloud/releases
- Issues: https://github.com/bun2it/RomCloud/issues

---

*Document này được tạo để hướng dẫn AI agent tự động fix bugs và maintain RomCloud.*
