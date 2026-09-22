#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>
#include <chrono>
#include "../database/DatabaseManager.h"

namespace RomCloud {

enum class UploadState {
    IDLE,
    PREPARING,
    UPLOADING,
    COMPLETED,
    FAILED,
    CANCELLED
};

struct UploadProgress {
    UploadState state = UploadState::IDLE;
    int64_t gameId = 0;
    std::string gameTitle;
    std::string systemCode;
    std::string filename;
    uint64_t bytesUploaded = 0;
    uint64_t totalBytes = 0;
    double progressPct = 0.0;
    double speedKBps = 0.0;
    std::string errorMessage;

    // Batch upload stats
    int totalGames = 0;
    int gamesUploaded = 0;
    int gamesFailed = 0;
    int gamesSkipped = 0;
    int currentIndex = 0;
};

struct LocalGameInfo {
    int64_t gameId = 0;
    std::string filename;
    std::string title;
    uint64_t sizeBytes = 0;
    std::string localPath;
    int systemId = 0;
    std::string systemCode;
    std::string cloudFileId;  // empty if not on cloud
    bool needsUpload = false; // true if local but not on cloud
};

class UploadManager {
public:
    static UploadManager& instance();
    bool init();
    void shutdown();

    // Start reverse sync (upload all local games not on cloud)
    void startReverseSync();

    // Start upload of specific selected games
    void startUploadGames(const std::vector<int64_t>& gameIds);

    // Cancel current operation
    void cancel();

    // Get status
    bool isUploading() const;
    UploadProgress getProgress() const;
    std::vector<LocalGameInfo> getLocalOnlyGames() const;

    // Internal progress update from cURL callback
    void updateProgress(uint64_t bytesUploaded, uint64_t totalBytes);
    bool isCancelRequested() const { return m_cancelRequested.load(); }

    // Callbacks
    std::function<void(bool success, const std::string& message)> onComplete;

private:
    UploadManager() = default;
    ~UploadManager();
    UploadManager(const UploadManager&) = delete;
    UploadManager& operator=(const UploadManager&) = delete;

    void runWorker();
    bool uploadSingleGame(const LocalGameInfo& game);
    std::vector<LocalGameInfo> scanLocalOnlyGames();
    std::string findOrCreateDriveFolder(const std::string& folderName, const std::string& parentFolderId, const std::string& token);
    std::string getTargetFolderForGame(const LocalGameInfo& game, const std::string& token);

    mutable std::mutex m_mutex;
    std::thread m_workerThread;
    std::atomic<bool> m_cancelRequested{false};
    std::atomic<bool> m_isRunning{false};
    UploadProgress m_progress;

    // Speed calculation
    std::chrono::steady_clock::time_point m_lastSpeedTime;
    uint64_t m_lastSpeedBytes = 0;

    // Cache of system folder IDs on Drive (systemCode -> folderId)
    std::string m_rootBackupFolderId;
    std::unordered_map<std::string, std::string> m_systemFolderCache;

    // List of games to upload
    std::vector<LocalGameInfo> m_uploadQueue;
};

} // namespace RomCloud

