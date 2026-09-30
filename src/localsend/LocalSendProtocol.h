// LocalSendProtocol.h
// LocalSend v2.2 protocol implementation cho RomCloud (TrimUI Brick Pro).
// Spec: https://github.com/localsend/protocol (MIT)
// HTTP mode (không HTTPS), cổng 53317, multicast 224.0.0.167.
// Thiết kế nhẹ: < 200KB RAM, raw POSIX socket, JSON parse thủ công (không lib).

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace RomCloud {

// ----- Protocol constants (spec §1) -----------------------------------
namespace LocalSendProto {
constexpr int kPort = 53317;
constexpr const char* kMulticastAddr = "224.0.0.167";
constexpr const char* kVersion = "2.0";
constexpr const char* kDeviceType = "mobile";
constexpr const char* kProtocol = "http";
constexpr int kUploadChunkSize = 64 * 1024;
constexpr int kMaxKnownDevices = 32;
constexpr int kMaxPendingRequests = 16;
constexpr int kDiscoveryPingIntervalSec = 5;
constexpr int kDiscoveryRecvTimeoutMs = 500;
constexpr int kUploadApprovalTimeoutSec = 60;
} // namespace LocalSendProto

// ----- Device info (spec §3.1 announce) -------------------------------
struct LsDeviceInfo {
    std::string alias;
    std::string version;
    std::string deviceModel;
    std::string deviceType;
    std::string fingerprint;
    std::string ip;
    int port = LocalSendProto::kPort;
    std::string protocol = LocalSendProto::kProtocol;
    bool download = false;
    bool announce = true;
    int64_t lastSeenMs = 0;
};

// ----- File metadata (spec §4.1 prepare-upload) -----------------------
struct LsFileMeta {
    std::string id;
    std::string fileName;
    std::string relativePath;   // MỞ RỘNG: từ RomCloud sender, vd "Roms/GBA/"
    uint64_t size = 0;
    std::string fileType;
    std::string sha256;
    std::string preview;
    // ----- MỞ RỘNG RomCloud: nếu file là 1 game trong DB ------------
    // Cho phép sender truyền kèm metadata game (title, system, cover path)
    // → receiver render dialog duyệt với cover art + tên game đẹp.
    int64_t  gameId      = 0;
    int      systemId    = 0;
    std::string systemCode;   // "GBA", "PS1", ...
    std::string systemName;
    std::string gameTitle;    // display name (vd "Pokemon - Emerald Version (USA)")
    std::string coverPath;    // absolute path tới cover PNG/JPG trên sender
};

// ----- 1 request upload (pending state trong RAM) ---------------------
// Lưu ý: KHÔNG dùng std::atomic ở đây vì struct phải copy được
// (vector::push_back, lambda capture). Truy cập từ nhiều thread dùng mutex
// ở LocalSendManager (m_pendingMutex, m_sendMutex).
struct LsUploadRequest {
    std::string sessionId;
    std::string fileId;
    std::string fileToken;    // token trả cho sender (spec §4.1 response files[fileId]=token)
    std::string fromAlias;
    std::string fromIp;
    LsFileMeta file;
    std::string savedPath;

    enum State : uint8_t {
        PENDING = 0,
        APPROVED = 1,
        REJECTED = 2,
        RECEIVING = 3,
        DONE = 4,
        FAILED = 5
    };
    State state = PENDING;
    uint64_t receivedBytes = 0;
    uint32_t bytesPerSec = 0;
    int64_t lastUpdateMs = 0;
};

// ----- 1 request gửi file (sender side) ------------------------------
struct LsSendProgress {
    std::string sessionId;
    std::string toAlias;
    std::string toIp;
    std::string fileName;
    std::string absPath;
    uint64_t totalBytes = 0;
    uint64_t sentBytes = 0;
    uint32_t bytesPerSec = 0;
    enum State : uint8_t {
        QUEUED = 0,
        NEGOTIATING = 1,
        UPLOADING = 2,
        DONE = 3,
        FAILED = 4
    };
    State state = QUEUED;
    std::string errorMessage;
};

// ----- Helper JSON thủ công (không dùng lib) -------------------------
namespace LsJson {

bool getString(const std::string& json, const std::string& key, std::string& out);
bool getUint64(const std::string& json, const std::string& key, uint64_t& out);
bool getBool(const std::string& json, const std::string& key, bool& out);
bool getObject(const std::string& json, const std::string& key, std::string& out);
std::string escape(const std::string& s);

} // namespace LsJson

// ----- Helper chung ----------------------------------------------------
namespace LsUtil {

std::string makeUuid();
std::string randomHex(int n);
std::string basenameOf(const std::string& path);
std::string dirnameOf(const std::string& path);
std::string lowerExt(const std::string& path);
std::string getOwnIp(const std::string& iface = "wlan0");
bool mkdirRecursive(const std::string& path, mode_t mode = 0755);
uint64_t sdFreeBytes(const std::string& path = "/mnt/SDCARD");
std::string sha256OfFile(const std::string& path);
std::string humanSize(uint64_t bytes);
std::string humanSpeed(uint32_t bytesPerSec);
int64_t nowMs();
bool isPathSafe(const std::string& absPath);

} // namespace LsUtil

} // namespace RomCloud
