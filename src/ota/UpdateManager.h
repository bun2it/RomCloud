#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>

namespace RomCloud {

constexpr const char* APP_VERSION = "1.2.3";
constexpr const char* GITHUB_REPO = "bun2it/RomCloud";
constexpr const char* VERSION_MANIFEST_URL = "https://raw.githubusercontent.com/bun2it/RomCloud/main/version.json";

enum class UpdateState {
    IDLE,
    CHECKING,
    UPDATE_AVAILABLE,
    UP_TO_DATE,
    DOWNLOADING,
    VERIFYING,
    COMPLETED,
    FAILED
};

struct UpdateInfo {
    std::string remoteVersion;
    std::string downloadUrl;
    std::string changelog;
    std::string releaseDate;
    uint64_t sizeBytes = 0;
};

struct UpdateProgress {
    UpdateState state = UpdateState::IDLE;
    uint64_t bytesDownloaded = 0;
    uint64_t totalBytes = 0;
    double progressPct = 0.0;
    std::string errorMessage;
    std::string newVersion;
};

class UpdateManager {
public:
    static UpdateManager& instance();

    bool init();
    void shutdown();

    // Check for updates against GitHub version.json
    void checkForUpdatesAsync(std::function<void(bool hasUpdate, const UpdateInfo& info)> callback = nullptr);
    bool checkForUpdatesSync(UpdateInfo& outInfo);

    // Start OTA download and installation
    bool startUpdate(const UpdateInfo& info);
    void cancelUpdate();

    // State & progress
    UpdateProgress getProgress() const;
    bool isUpdateAvailable() const { return m_hasUpdate; }
    UpdateInfo getLatestInfo() const;
    std::string getCurrentVersion() const { return APP_VERSION; }

private:
    UpdateManager() = default;
    ~UpdateManager();

    mutable std::mutex m_mutex;
    std::atomic<bool> m_isRunning{false};
    std::atomic<bool> m_cancelRequested{false};
    std::atomic<bool> m_hasUpdate{false};

    UpdateInfo m_latestInfo;
    UpdateProgress m_progress;
    std::thread m_workerThread;

    void runDownloadWorker(UpdateInfo info);
    static int xferCallback(void* clientp, int64_t dltotal, int64_t dlnow, int64_t ultotal, int64_t ulnow);
    static bool isVersionNewer(const std::string& remote, const std::string& current);
};

} // namespace RomCloud
