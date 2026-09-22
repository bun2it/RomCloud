#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <deque>
#include <cstdint>
#include <curl/curl.h>
#include "../database/DatabaseManager.h"

namespace RomCloud {

enum class DownloadState {
    IDLE,
    INITIALIZING,
    DOWNLOADING,
    VERIFYING,
    COMPLETED,
    FAILED,
    CANCELLED
};

struct DownloadProgress {
    DownloadState state = DownloadState::IDLE;
    int64_t gameId = 0;
    std::string gameTitle;
    std::string systemCode;
    std::string filename;
    uint64_t bytesDownloaded = 0;
    uint64_t totalBytes = 0;
    double progressPct = 0.0;
    double speedKBps = 0.0;
    int etaSeconds = 0;
    std::string errorMessage;
    bool storageWarning = false;  // True if storage < 5%
    uint64_t storageAvailable = 0;
    uint64_t storageTotal = 0;
};

struct QueueItem {
    GameRecord game;
    SystemRecord sys;
};

class DownloadManager {
public:
    static DownloadManager& instance();
    bool init();
    void shutdown();

    // Single download (legacy / direct)
    bool startDownload(const GameRecord& game, const SystemRecord& sys);
    void cancelDownload();
    bool isDownloading() const;

    // Queue management
    bool addToQueue(const GameRecord& game, const SystemRecord& sys);
    bool removeFromQueue(int64_t gameId);
    bool isInQueue(int64_t gameId) const;
    std::deque<QueueItem> getQueue() const;
    int queueSize() const;
    void clearQueue();
    void processNextInQueue();

    DownloadProgress getProgress() const;
    void resetProgress();

private:
    DownloadManager() = default;
    ~DownloadManager();

    mutable std::mutex m_mutex;
    DownloadProgress m_progress;
    std::thread m_workerThread;
    std::atomic<bool> m_cancelRequested{false};
    std::atomic<bool> m_isRunning{false};

    GameRecord m_activeGame;
    SystemRecord m_activeSystem;
    std::string m_tempFilePath;
    std::string m_finalFilePath;

    // Download queue
    std::deque<QueueItem> m_queue;

    void runDownloadWorker();
    static int xferCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);
};

} // namespace RomCloud
