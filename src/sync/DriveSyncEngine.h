#pragma once
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include "../database/DatabaseManager.h"

namespace RomCloud {

enum class SyncStatus {
    IDLE,
    CONNECTING,
    DISCOVERING_FOLDERS,
    SYNCING_FILES,
    COMPLETED,
    ERROR_OCCURRED
};

struct SyncProgress {
    SyncStatus status = SyncStatus::IDLE;
    std::string currentPlatform;
    int currentSystemIndex = 0;
    int totalSystems = 0;
    int cloudGamesFound = 0;
    int newGamesIndexed = 0;
    int updatedGames = 0;
    std::string errorMessage;
};

class DriveSyncEngine {
public:
    static DriveSyncEngine& instance();
    bool init();
    void shutdown();

    bool startSync();
    void cancelSync();
    bool isSyncing() const;

    SyncProgress getProgress() const;
    std::string getLastSyncTime() const;

private:
    DriveSyncEngine() = default;
    ~DriveSyncEngine();

    mutable std::mutex m_mutex;
    SyncProgress m_progress;
    std::thread m_workerThread;
    std::atomic<bool> m_cancelRequested{false};
    std::atomic<bool> m_isRunning{false};

    void runSyncWorker();
    void syncPublicFolder(const std::string& rootFolderId);
    std::string findRomCloudRootFolder(const std::string& token);
    std::unordered_map<std::string, std::string> discoverSystemFolders(const std::string& rootFolderId, const std::string& token);
    int syncFilesForSystem(const SystemRecord& system, const std::string& folderId, const std::string& token);
    std::string normalizeTitle(const std::string& filename);
    std::string matchFolderToSystemCode(const std::string& name);
};

} // namespace RomCloud
