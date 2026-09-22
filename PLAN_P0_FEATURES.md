# RomCloud P0 Features Implementation Plan

## Context

RomCloud v1.1.1 đã hoàn thiện core features (Google Drive sync, on-demand download, OTA). P0 features được chọn để:
1. **Multi-Cloud Support** - Giảm dependency vào Google, mở rộng khả năng
2. **Settings Backup/Restore** - An toàn dữ liệu trước khi refactor
3. **Batch Operations** - Cải thiện UX cho việc quản lý game

---

## Implementation Order

### Phase 1: Settings Backup/Restore
**Rủi ro thấp, làm trước để có safety net trước khi refactor lớn**

### Phase 2: Batch Operations  
**Self-contained, không ảnh hưởng core logic**

### Phase 3: Multi-Cloud Provider Interface
**Foundation cho tương lai, cần refactoring cẩn thận**

---

## Phase 1: Settings Backup/Restore

### 1.1 Data to Export
| Data | Source | Priority |
|------|--------|----------|
| All settings | SQLite `settings` table | Required |
| Sync metadata | SQLite `sync_state` table | Required |
| System customizations | SQLite `systems` table | Required |
| Legacy config | `config/settings.json` | Optional |

### 1.2 New Files
```
src/backup/BackupManager.h
src/backup/BackupManager.cpp
```

### 1.3 BackupManager Interface
```cpp
class BackupManager {
    std::string exportToSdCard();           // Export to JSON
    bool importFromLastExport();            // Import from backup
    std::vector<std::string> listBackups();  // List available backups
    bool validateBackup(const std::string& path);
};
```

### 1.4 Export Format
```json
{
  "version": "1.1.1",
  "exported_at": "2026-09-22T14:30:00Z",
  "settings": {...},
  "systems": [...],
  "sync_state": [...]
}
```

### 1.5 Files to Modify
- `src/database/DatabaseManager.h/cpp` - Add methods to export/import data
- `src/ui/UIManager.h/cpp` - Add BACKUP/RESTORE rows in Settings screen
- `src/ui/UiStrings.h` - Add new strings: "Sao lưu", "Phục hồi", "Đang sao lưu...", etc.
- `src/ui/UiStrings.cpp` - Implement new strings

### 1.6 Settings Screen Changes
```
|------------------------------------------|
|  ☁️ Cloud Status: Đã kết nối (user@gmail.com)  |
|  📁 Folder: RomCloud                       |
|------------------------------------------|
|  🔧 [Sao lưu cài đặt]         →          |
|  🔧 [Phục hồi cài đặt]        →          |
|------------------------------------------|
```

### 1.7 Export Location
`/mnt/SDCARD/Apps/RomCloud/backups/romcloud_backup_YYYYMMDD_HHMMSS.json`

---

## Phase 2: Batch Operations

### 2.1 UIManager Changes

**Add to UIManager.h:**
```cpp
bool m_multiSelectMode = false;
std::vector<int64_t> m_selectedGameIds;
```

**Add to UIState enum:**
```cpp
CONFIRM_BATCH_DELETE,
```

### 2.2 Input Handling (UIManager.cpp)

Add L2 button toggle in GAME_LIST state:
```cpp
if (input.isButtonJustPressed(Button::L2)) {
    m_multiSelectMode = !m_multiSelectMode;
    if (!m_multiSelectMode) m_selectedGameIds.clear();
    showToast(m_multiSelectMode ? "Đã bật chọn nhiều" : "Đã tắt chọn nhiều", 2500);
}
```

### 2.3 Multi-Select Mode Controls
| Button | Action |
|--------|--------|
| L2 | Toggle multi-select mode |
| A | Toggle selection on current game |
| X | Batch delete (with confirmation) |
| Y | Add all selected to queue |
| B | Exit multi-select mode |

### 2.4 Rendering Changes

**Game List Row (add checkbox):**
```cpp
if (m_multiSelectMode) {
    bool isSelected = std::find(m_selectedGameIds.begin(), ...);
    // Draw checkbox [✓] or [ ]
}
```

**Header (add selection count):**
```cpp
if (m_multiSelectMode && !m_selectedGameIds.empty()) {
    drawBadge(200, 14, 150, 38, 
              std::to_string(m_selectedGameIds.size()) + " đã chọn", ...);
}
```

### 2.5 DatabaseManager Changes

**Add to DatabaseManager.h:**
```cpp
bool deleteGamesBulk(const std::vector<int64_t>& gameIds);
bool updateGamesLocalStateBulk(const std::vector<int64_t>& gameIds, GameState state);
```

**Implementation:**
```cpp
bool DatabaseManager::deleteGamesBulk(const std::vector<int64_t>& gameIds) {
    beginTransaction();
    for (int64_t id : gameIds) {
        markGameDeletedLocally(id);
    }
    commitTransaction();
}
```

### 2.6 Batch Confirmation Dialog
```
+--------------------------------------------------+
|              XÁC NHẬN XÓA NHIỀU GAME              |
|                                                  |
|  Bạn muốn xóa 15 game khỏi thẻ nhớ?              |
|                                                  |
|  [A] Xóa tất cả          [B] Hủy                 |
+--------------------------------------------------+
```

### 2.7 Files to Modify
- `src/ui/UIManager.h` - Add multi-select state
- `src/ui/UIManager.cpp` - Input handling, rendering, dialogs
- `src/database/DatabaseManager.h/cpp` - Bulk operations
- `src/ui/UiStrings.h/cpp` - New strings

---

## Phase 3: Multi-Cloud Provider Interface

### 3.1 Architecture

```
ICloudProvider (interface)
├── GoogleDriveProvider : ICloudProvider
├── OneDriveProvider : ICloudProvider  (future)
└── DropboxProvider : ICloudProvider   (future)

CloudProviderManager (factory singleton)
├── setActiveProvider(CloudType type)
├── getActiveProvider() -> ICloudProvider*
└── registerProvider(CloudType, ICloudProvider*)
```

### 3.2 ICloudProvider Interface
```cpp
class ICloudProvider {
    virtual CloudType getType() const = 0;
    virtual std::string getDisplayName() const = 0;
    virtual bool isConfigured() const = 0;
    virtual bool isAuthenticated() const = 0;
    virtual std::string getDownloadUrl(const std::string& fileId) = 0;
    virtual std::vector<CloudItem> listFolder(const std::string& folderId) = 0;
    virtual std::string matchFolderToSystemCode(const std::string& name) = 0;
};
```

### 3.3 CloudItem Structure
```cpp
struct CloudItem {
    std::string id;
    std::string name;
    std::string mimeType;
    uint64_t sizeBytes = 0;
    bool isFolder = false;
    std::string modifiedTime;
};
```

### 3.4 New Files
```
src/cloud/CloudTypes.h           # CloudType enum
src/cloud/ICloudProvider.h       # Abstract interface
src/cloud/CloudProviderManager.h  # Factory singleton
src/cloud/CloudProviderManager.cpp
src/cloud/GoogleDriveProvider.h   # Rename from DriveSyncEngine
src/cloud/GoogleDriveProvider.cpp # Refactor existing code
```

### 3.5 Critical Refactoring Points

**Before (hardcoded):**
```cpp
// DownloadManager.cpp L231
url = "https://www.googleapis.com/drive/v3/files/" + fileId + "?alt=media";
```

**After (abstracted):**
```cpp
// DownloadManager.cpp
url = CloudProviderManager::instance().getActiveProvider()
          ->getDownloadUrl(fileId);
```

**Before (Google-specific OAuth):**
```cpp
// AuthManager.cpp L19
static const char* DEVICE_AUTH_ENDPOINT = "https://oauth2.googleapis.com/device/code";
```

**After (via IAuthProvider):**
```cpp
// GoogleAuthProvider.cpp
const char* DEVICE_AUTH_ENDPOINT = "https://oauth2.googleapis.com/device/code";
```

### 3.6 Integration Points

| File | Changes |
|------|---------|
| `DownloadManager.cpp` | Use `ICloudProvider::getDownloadUrl()` |
| `AuthManager.cpp` | Delegate to `IAuthProvider` internally |
| `UIManager.cpp` | Add cloud provider selection in Settings |
| `Settings.cpp` | Add provider-specific config |

### 3.7 Settings UI Addition
```
|------------------------------------------|
|  ☁️ Cloud Provider: [Google Drive ▼]      |
|  📁 Folder: [Chọn thư mục]                |
|------------------------------------------|
```

### 3.8 Breaking Changes Mitigation
- Keep `DriveSyncEngine` as deprecated alias → `GoogleDriveProvider`
- Keep `AuthManager` as facade → delegates to `GoogleAuthProvider`
- No database schema changes
- Feature flag for new provider (disabled by default)

---

## Files Summary

### New Files (Phase 1)
1. `src/backup/BackupManager.h`
2. `src/backup/BackupManager.cpp`

### New Files (Phase 3)
3. `src/cloud/CloudTypes.h`
4. `src/cloud/ICloudProvider.h`
5. `src/cloud/CloudProviderManager.h`
6. `src/cloud/CloudProviderManager.cpp`
7. `src/cloud/GoogleDriveProvider.h` (rename/refactor)
8. `src/cloud/GoogleDriveProvider.cpp`

### Modified Files
| File | Phases | Changes |
|------|--------|---------|
| `src/ui/UIManager.h` | 2, 3 | Multi-select state, CONFIRM_BATCH_DELETE |
| `src/ui/UIManager.cpp` | 1, 2, 3 | Settings rows, batch UI, provider selection |
| `src/database/DatabaseManager.h` | 2 | Bulk delete methods |
| `src/database/DatabaseManager.cpp` | 2 | Bulk delete implementation |
| `src/ui/UiStrings.h` | 1, 2 | New strings |
| `src/download/DownloadManager.cpp` | 3 | Use ICloudProvider abstraction |

---

## Testing Approach

### Manual Testing Checklist
- [ ] Backup export creates valid JSON
- [ ] Backup import restores all settings
- [ ] Multi-select toggles with L2
- [ ] Batch add to queue processes sequentially
- [ ] Batch delete removes files from SD card
- [ ] Google Drive sync still works after refactoring
- [ ] UI renders correctly in multi-select mode

### Regression Testing
- Run existing sync flow with Google Drive
- Verify download queue still works
- Check OTA update still functions

---

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| Breaking Google Drive sync | Keep DriveSyncEngine as deprecated alias |
| Data loss during import | Validate backup JSON first, backup before overwrite |
| Memory issues during batch download | Sequential processing, one at a time |

---

## Timeline Estimate

| Phase | Complexity | Estimate |
|-------|------------|---------|
| Phase 1: Backup/Restore | Low | 2-3 days |
| Phase 2: Batch Operations | Medium | 3-4 days |
| Phase 3: Multi-Cloud | High | 5-7 days |
| **Total** | | **10-14 days** |
