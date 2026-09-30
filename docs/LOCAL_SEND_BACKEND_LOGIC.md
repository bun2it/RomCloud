# Plan: LocalSend Backend Logic

## Context

LocalSend hiện tại đã implement đúng chuẩn protocol với `/prepare-upload` endpoint, nhưng **flow nhận file có vấn đề**:

- **Hiện tại**: Server gửi response ngay trong `handlePrepareUpload()`, UI chỉ hiển thị dialog xem thông tin
- **�úng spec**: Server HOLD response, chờ user chọn folder và bấm Accept → mới gửi response với token

## Tóm tắt Flow Đúng

```
1. Sender → POST /prepare-upload (metadata)
2. Receiver → [HOLD] - không gửi response
3. UI → Modal hiển thị, user chọn folder, bấm [A] Accept
4. Receiver → Gửi HTTP 200 với {sessionId, files:{fileId: token}}
5. Sender → Nhận token → POST /upload (file data)
6. Receiver → Stream to disk
```

## Thay đổi cần thiết

### 1. LocalSendManager.h - Thêm data structure

```cpp
// Mở rộng: lưu fd để gửi response sau
struct LsPendingPrepare {
    int clientFd;                    // socket fd để gửi response
    std::string sessionId;
    std::string fileId;
    std::string fileToken;
    std::string fromAlias;
    LsFileMeta file;
    std::chrono::steady_clock::time_point startTime;  // timeout tracking

    enum State { WAITING_USER = 0, APPROVED = 1, REJECTED = 2, TIMEOUT = 3 };
    State state = WAITING_USER;
    std::string savePath;            // path user đã chọn
};
```

**Thêm methods:**
```cpp
// User actions: approve với path đã chọn
void approveUploadWithPath(const std::string& sessionId, const std::string& savePath);
void rejectUpload(const std::string& sessionId);

// Query
std::string getPendingSavePath(const std::string& sessionId) const;
```

### 2. LocalSendManager.cpp - Sửa handlePrepareUpload()

**Trước (hiện tại):**
```cpp
void LocalSendManager::handlePrepareUpload(int fd, ...) {
    // ... parse ...
    std::string target = resolveTargetPath(req.file);  // tự resolve!
    req.savedPath = target;
    req.fileToken = LsUtil::makeUuid();
    m_pending.push_back(req);

    // GỬI RESPONSE NGAY!
    sendJsonResponse(fd, 200, ss.str());

    // Rồi mới bắn callback cho UI
    if (m_onUserPrompt) m_onUserPrompt(req);
}
```

**Sau (cần sửa):**
```cpp
void LocalSendManager::handlePrepareUpload(int fd, ...) {
    // ... parse ...

    // Tạo pending prepare với fd để gửi response sau
    LsPendingPrepare pp;
    pp.clientFd = fd;
    pp.sessionId = LsUtil::makeUuid();
    pp.fileToken = LsUtil::makeUuid();
    pp.file = req.file;
    pp.fromAlias = req.fromAlias;
    pp.startTime = std::chrono::steady_clock::now();

    // Lưu vào m_preparing (khác với m_pending)
    {
        std::lock_guard<std::mutex> lock(m_prepareMutex);
        m_preparing.push_back(pp);
    }

    // Bắn callback cho UI - UI sẽ gọi approve/reject sau
    {
        std::lock_guard<std::mutex> lock(m_cbMutex);
        if (m_onUserPrompt) m_onUserPrompt(req);
    }

    // KHÔNG gửi response ở đây!
    // Response sẽ được gửi trong approveUploadWithPath()
}
```

### 3. Thêm approveUploadWithPath()

```cpp
void LocalSendManager::approveUploadWithPath(const std::string& sessionId,
                                              const std::string& savePath) {
    std::lock_guard<std::mutex> lock(m_prepareMutex);

    for (auto& pp : m_preparing) {
        if (pp.sessionId == sessionId && pp.state == LsPendingPrepare::WAITING_USER) {
            pp.state = LsPendingPrepare::APPROVED;
            pp.savePath = savePath;

            // Tạo LsUploadRequest để chuyển sang pending
            LsUploadRequest req;
            req.sessionId = pp.sessionId;
            req.fileId = pp.fileId;
            req.fileToken = pp.fileToken;
            req.fromAlias = pp.fromAlias;
            req.file = pp.file;
            req.savedPath = savePath;

            // Gửi response NGAY cho sender
            std::ostringstream ss;
            ss << "{\"sessionId\":\"" << req.sessionId << "\",\"files\":{"
               << "\"" << LsJson::escape(req.fileId) << "\":\""
               << req.fileToken << "\"}}";
            sendJsonResponse(pp.clientFd, 200, ss.str());

            // Chuyển sang m_pending để handleFileUpload() đọc
            {
                std::lock_guard<std::mutex> lock2(m_pendingMutex);
                m_pending.push_back(req);
            }

            return;
        }
    }
}
```

### 4. UIManager - Sửa flow

**Trước (hiện tại):**
- `handlePrepareUpload()` gửi response ngay → `m_onUserPrompt()` → `LOCALSEND_INCOMING`
- User bấm A → `approveUpload(sessionId)` → state = APPROVED

**Sau (cần sửa):**
- `handlePrepareUpload()` KHÔNG gửi response → `m_onUserPrompt()` → `LOCALSEND_INCOMING`
- User chọn folder → bấm A → `approveUploadWithPath(sessionId, savePath)`
- `LocalSendManager` gửi response + chuyển sang pending

### 5. Xử lý Timeout

Thêm background thread hoặc check trong `handleFileUpload()`:
- Nếu `m_preparing` timeout (60s) → gửi 408 + close fd + remove
- Cleanup thread: `pruneStalePrepares()` mỗi 5s

### 6. File cần sửa

| File | Thay đổi |
|------|----------|
| `src/localsend/LocalSendManager.h` | Thêm `LsPendingPrepare`, methods mới |
| `src/localsend/LocalSendManager.cpp` | Sửa `handlePrepareUpload()`, thêm `approveUploadWithPath()`, timeout cleanup |
| `src/ui/UIManager.cpp` | Sửa input handler gọi `approveUploadWithPath()` thay vì `approveUpload()` |

## Verification

1. Build: `cd /mnt/SDCARD && ./scripts/auto_fix.sh`
2. Test với LocalSend app trên PC:
   - Gửi file từ PC → TrimUI
   - Quan sát: Dialog xuất hiện TRƯỚC khi sender bắt đầu upload
   - Chọn folder → bấm Accept → sender bắt đầu upload
3. Test timeout: Không bấm gì 60s → sender nhận timeout error
4. Test reject: Bấm B → sender nhận rejected error

## Optional Enhancement (Phase 2)

Nếu muốn UI đẹp hơn như spec:
- Hiệu ứng "đang chờ" khi sender đã gửi prepare nhưng chưa Accept
- Countdown timeout hiển thị trên modal
- Animation khi nhận được response
