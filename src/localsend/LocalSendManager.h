// LocalSendManager.h
// Singleton điều phối toàn bộ LocalSend P2P: discovery, receive, send.
// Bắt đầu chạy khi user vào UIState::LOCALSEND_HOME, dừng khi thoát ra menu
// khác.

#pragma once

#include "LocalSendProtocol.h"
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace RomCloud {

struct LsPendingPrepare {
  int clientFd = -1;
  std::string sessionId;
  std::string fileId;
  std::string fileToken;
  std::string fromAlias;
  std::string fromIp;
  LsFileMeta file;
  std::chrono::steady_clock::time_point startTime;

  enum State { WAITING_USER = 0, APPROVED = 1, REJECTED = 2, TIMEOUT = 3 };
  State state = WAITING_USER;
  std::string savePath;
};

class LocalSendManager {
public:
  static LocalSendManager &instance();

  // Lifecycle: gọi từ UIManager khi vào/ra state LOCALSEND_*
  // start(): khởi động discovery + transfer thread. Idempotent.
  // stop(): dừng thread, dọn pending. Idempotent.
  bool start();
  void stop();

  bool isRunning() const { return m_running; }

  // ----- Fingerprint (persist qua các lần khởi động) --------------
  const std::string &fingerprint() const { return m_selfFingerprint; }
  const std::string &alias() const { return m_selfAlias; }
  const std::string &ownIp() const { return m_ownIp; }
  void setAlias(const std::string &a) { m_selfAlias = a; }

  // ----- Build JSON announce/info (public cho helper anon namespace) -
  std::string buildDeviceInfoJson(bool announce) const;

  // ----- Known devices (cho UI danh sách) -------------------------
  std::vector<LsDeviceInfo> knownDevices(); // copy, thread-safe
  void clearKnownDevices();
  // Quét chủ động: burst multicast + dọn thiết bị cũ (gọi khi vào màn Send /
  // bấm Y)
  void refreshDiscovery();
  void pruneStaleDevices(int64_t maxAgeMs = 20000);

  // ----- Target folder picker (ưu tiên 1 trong path resolution) ---
  void setTargetFolder(const std::string &folder); // "Roms/GBA/" hoặc "" = auto
  std::string currentTargetFolder() const;
  // Override đích cho 1 pending cụ thể (dialog incoming chọn tay).
  // folderRel: "" = giữ nguyên auto, hoặc "Roms/GBA/", "Inbox/", ...
  // Trả về savedPath mới (rỗng nếu session không tồn tại / path unsafe).
  std::string setPendingTargetFolder(const std::string &sessionId,
                                     const std::string &folderRel);

  // ----- Pending upload requests ----------------------------------
  // UI subscribe: callback khi có request mới → render modal A/B.
  void setOnUserPrompt(std::function<void(const LsUploadRequest &)> cb);
  // UI subscribe: callback khi có progress update (~mỗi 500ms).
  void setOnProgress(std::function<void(const LsUploadRequest &)> cb);
  // UI subscribe: callback khi upload xong/thất bại.
  void setOnComplete(std::function<void(const LsUploadRequest &)> cb);

  // User actions: gọi từ UIManager khi bấm A (đồng ý) hoặc B (từ chối).
  void approveUpload(const std::string &sessionId);
  void approveUploadWithPath(const std::string &sessionId, const std::string &savePath);
  void rejectUpload(const std::string &sessionId);
  void pruneStalePrepares();

  // ----- Sender API (RomCloud TrimUI gửi file sang TrimUI khác) -----
  // Gửi 1 file. absPath phải nằm trong /mnt/SDCARD/.
  // Trả về sessionId (rỗng nếu lỗi).
  std::string sendFile(const std::string &absPath, const LsDeviceInfo &target);

  // Gửi 1 file với metadata đầy đủ (kèm game meta nếu là ROM từ DB).
  // Dùng cho UI game picker: truyền được game title, system, cover path tới
  // receiver.
  std::string sendFileMeta(const LsFileMeta &meta, const std::string &absPath,
                           const LsDeviceInfo &target);
  // Bản async (non-blocking): spawn thread nền rồi trả về ngay sessionId tạm.
  // UI nên dùng bản này để màn PROGRESS hiển thị live (bản sync block UI).
  std::string sendFileMetaAsync(const LsFileMeta &meta,
                                const std::string &absPath,
                                const LsDeviceInfo &target);

  // Lấy progress các file đang/đã gửi (cho UI sender screen).
  std::vector<LsSendProgress> sendProgresses();
  // Lấy snapshot các upload đang/đã nhận (cho UI progress screen).
  std::vector<LsUploadRequest> receiveProgresses();
  // Xóa các tác vụ đã xong hoặc lỗi khỏi danh sách progress
  void clearFinishedTasks();

  // ----- Internal: HTTP routing -----------------------------------
  // Được gọi từ TransferLoop thread (private thread). Mỗi accept → spawn 1
  // detached thread. Trả về true nếu caller cần đóng clientFd ngay.
  bool handleHttpClient(int clientFd);

private:
  LocalSendManager() = default;
  ~LocalSendManager();

  // Vòng lặp chạy trong thread riêng (POSIX socket trực tiếp).
  void discoveryLoop();
  void transferLoop();

  // UDP discovery
  bool setupUdpSocket();
  void sendMulticastAnnounce();
  void sendUnicastAnnounce(const std::string &ip, int port,
                           bool announce = false);
  void handleDiscoveryPacket(const char *json, size_t len,
                             const std::string &fromIp);
  void addOrUpdateKnownDevice(const LsDeviceInfo &dev);

  // TCP transfer
  bool setupTcpSocket();
  bool readHttpRequest(int fd, std::string &method, std::string &path,
                       std::map<std::string, std::string> &headers,
                       std::string &body);
  void sendJsonResponse(int fd, int statusCode, const std::string &jsonBody);
  void sendEmptyResponse(int fd, int statusCode);

  // Endpoint handlers (spec §4)
  void handleInfo(int fd);
  void handleRegister(int fd, const std::string &body);
  bool handlePrepareUpload(int fd, const std::string &body,
                           const std::string &fromIp);
  void handleFileUpload(int fd, const std::string &query,
                        const std::map<std::string, std::string> &headers,
                        const std::string &initialBody);
  void handleCancel(int fd, const std::string &query);

  // Path resolution (3 tầng ưu tiên)
  std::string resolveTargetPath(const LsFileMeta &file);

  // Heuristic mapping extension → folder (fallback khi không có relativePath).
  std::string heuristicMap(const std::string &fileName);

  // Post-process: extract + organize + rescan (chạy detached thread).
  void postProcessUpload(const LsUploadRequest &req);

  // Sender: gọi HTTP/HTTPS client tới target IP.
  bool httpPostJson(const std::string &protocol, const std::string &ip, int port,
                    const std::string &path, const std::string &jsonBody,
                    std::string &respBody, int timeoutSec = 10);
  bool httpPostBinary(const std::string &protocol, const std::string &ip, int port,
                      const std::string &path, const std::string &filePath,
                      LsSendProgress &prog);

  // ----- State -----------------------------------------------------
  std::atomic<bool> m_running{false};
  std::thread m_discoveryThread;
  std::thread m_transferThread;
  int m_udpFd = -1;
  int m_tcpFd = -1;

  // Self info
  std::string m_selfAlias;
  std::string m_selfFingerprint;
  std::string m_selfModel = "TrimUI Brick Pro";
  std::string m_ownIp;

  // Known devices
  std::mutex m_knownMutex;
  std::vector<LsDeviceInfo> m_knownDevices;

  // Target folder (user set trong UI)
  mutable std::mutex m_targetMutex;
  std::string m_targetFolder; // "Roms/GBA/" hoặc ""

  // Pending upload requests
  std::mutex m_pendingMutex;
  std::vector<LsUploadRequest>
      m_pending; // vector thay map vì sessionId có thể trùng pattern

  // Pending prepare requests (held HTTP response awaiting user approval)
  std::mutex m_prepareMutex;
  std::vector<LsPendingPrepare> m_preparing;

  // UI callbacks
  std::mutex m_cbMutex;
  std::function<void(const LsUploadRequest &)> m_onUserPrompt;
  std::function<void(const LsUploadRequest &)> m_onProgress;
  std::function<void(const LsUploadRequest &)> m_onComplete;

  // Send progresses (cho sender UI)
  std::mutex m_sendMutex;
  std::vector<std::shared_ptr<LsSendProgress>> m_sends;

  LocalSendManager(const LocalSendManager &) = delete;
  LocalSendManager &operator=(const LocalSendManager &) = delete;
};

} // namespace RomCloud