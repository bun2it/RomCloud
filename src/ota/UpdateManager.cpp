#include "UpdateManager.h"
#include "../network/HttpClient.h"
#include "../network/JsonHelper.h"
#include "../logging/Logger.h"
#include "../config/AppConfig.h"
#include "../filesystem/FileSystemManager.h"

#include <curl/curl.h>
#include <sys/stat.h>
#include <sstream>
#include <vector>

namespace RomCloud {

UpdateManager& UpdateManager::instance() {
    static UpdateManager instance;
    return instance;
}

UpdateManager::~UpdateManager() {
    shutdown();
}

bool UpdateManager::init() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress.state = UpdateState::IDLE;
    m_hasUpdate = false;
    Logger::info("UpdateManager initialized. Current version: v" + std::string(APP_VERSION));
    return true;
}

void UpdateManager::shutdown() {
    cancelUpdate();
}

bool UpdateManager::isVersionNewer(const std::string& remote, const std::string& current) {
    std::string r = remote;
    std::string c = current;
    if (!r.empty() && (r.front() == 'v' || r.front() == 'V')) r.erase(0, 1);
    if (!c.empty() && (c.front() == 'v' || c.front() == 'V')) c.erase(0, 1);

    auto parseParts = [](const std::string& str) {
        std::vector<int> parts;
        std::stringstream ss(str);
        std::string item;
        while (std::getline(ss, item, '.')) {
            try {
                parts.push_back(std::stoi(item));
            } catch (...) {
                parts.push_back(0);
            }
        }
        while (parts.size() < 3) parts.push_back(0);
        return parts;
    };

    auto rParts = parseParts(r);
    auto cParts = parseParts(c);

    for (size_t i = 0; i < 3; ++i) {
        if (rParts[i] > cParts[i]) return true;
        if (rParts[i] < cParts[i]) return false;
    }
    return false;
}

bool UpdateManager::checkForUpdatesSync(UpdateInfo& outInfo) {
    Logger::info("Checking for OTA updates from GitHub Releases API...");

    std::vector<std::string> headers = {
        "User-Agent: RomCloud-OTA/1.0 (TrimUI Brick Pro)",
        "Accept: application/vnd.github.v3+json"
    };

    // 1. Try GitHub Releases API first (instant, real-time, no CDN cache delay)
    std::string apiEndpoint = "https://api.github.com/repos/" + std::string(GITHUB_REPO) + "/releases/latest";
    HttpResponse resp = HttpClient::instance().get(apiEndpoint, headers);
    
    std::string remoteVer = "";
    std::string changelog = "";
    std::string relDate = "";
    std::string binUrl = "";

    if (resp.success && !resp.body.empty() && resp.statusCode == 200) {
        std::string tag = JsonHelper::extractString(resp.body, "tag_name");
        if (!tag.empty()) {
            remoteVer = tag;
            if (remoteVer.front() == 'v' || remoteVer.front() == 'V') {
                remoteVer.erase(0, 1);
            }
            changelog = JsonHelper::extractString(resp.body, "body");
            relDate = JsonHelper::extractString(resp.body, "published_at");
            if (relDate.length() >= 10) relDate = relDate.substr(0, 10);
            binUrl = "https://raw.githubusercontent.com/" + std::string(GITHUB_REPO) + "/main/bin/RomCloud";
        }
    }

    // 2. Fallback to version.json manifest if API failed or rate-limited
    if (remoteVer.empty()) {
        Logger::info("Falling back to version.json manifest: " + std::string(VERSION_MANIFEST_URL));
        HttpResponse mResp = HttpClient::instance().get(VERSION_MANIFEST_URL, headers);
        if (mResp.success && !mResp.body.empty()) {
            remoteVer = JsonHelper::extractString(mResp.body, "version");
            binUrl = JsonHelper::extractString(mResp.body, "binary_url");
            if (binUrl.empty()) binUrl = JsonHelper::extractString(mResp.body, "download_url");
            changelog = JsonHelper::extractString(mResp.body, "changelog");
            relDate = JsonHelper::extractString(mResp.body, "release_date");
        }
    }

    if (remoteVer.empty()) {
        Logger::warn("OTA check failed to obtain remote version.");
        return false;
    }
    if (binUrl.empty()) {
        binUrl = "https://raw.githubusercontent.com/" + std::string(GITHUB_REPO) + "/main/bin/RomCloud";
    }

    outInfo.remoteVersion = remoteVer;
    outInfo.downloadUrl = binUrl;
    outInfo.changelog = changelog;
    outInfo.releaseDate = relDate;

    bool newer = isVersionNewer(remoteVer, APP_VERSION);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_latestInfo = outInfo;
        m_hasUpdate = newer;
        m_progress.newVersion = remoteVer;
        m_progress.state = newer ? UpdateState::UPDATE_AVAILABLE : UpdateState::UP_TO_DATE;
    }

    if (newer) {
        Logger::info("New OTA update available: v" + remoteVer + " (Current: v" + std::string(APP_VERSION) + ")");
    } else {
        Logger::info("RomCloud is up to date (v" + std::string(APP_VERSION) + ")");
    }

    return newer;
}

void UpdateManager::checkForUpdatesAsync(std::function<void(bool hasUpdate, const UpdateInfo& info)> callback) {
    std::thread([this, callback]() {
        UpdateInfo info;
        bool hasUpdate = checkForUpdatesSync(info);
        if (callback) {
            callback(hasUpdate, info);
        }
    }).detach();
}

UpdateProgress UpdateManager::getProgress() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_progress;
}

UpdateInfo UpdateManager::getLatestInfo() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_latestInfo;
}

int UpdateManager::xferCallback(void* clientp, int64_t dltotal, int64_t dlnow, int64_t ultotal, int64_t ulnow) {
    (void)ultotal;
    (void)ulnow;
    auto* self = static_cast<UpdateManager*>(clientp);
    if (!self) return 0;
    if (self->m_cancelRequested) return 1; // Abort download

    if (dlnow > 0) {
        std::lock_guard<std::mutex> lock(self->m_mutex);
        self->m_progress.bytesDownloaded = static_cast<uint64_t>(dlnow);
        if (dltotal > 0) {
            self->m_progress.totalBytes = static_cast<uint64_t>(dltotal);
            self->m_progress.progressPct = (static_cast<double>(dlnow) / static_cast<double>(dltotal)) * 100.0;
        }
    }
    return 0;
}

bool UpdateManager::startUpdate(const UpdateInfo& info) {
    if (m_isRunning) {
        Logger::warn("An OTA update is already in progress.");
        return false;
    }

    m_cancelRequested = false;
    m_isRunning = true;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress = UpdateProgress();
        m_progress.state = UpdateState::DOWNLOADING;
        m_progress.newVersion = info.remoteVersion;
    }

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    m_workerThread = std::thread(&UpdateManager::runDownloadWorker, this, info);
    return true;
}

void UpdateManager::cancelUpdate() {
    m_cancelRequested = true;
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    m_isRunning = false;
}

void UpdateManager::runDownloadWorker(UpdateInfo info) {
    Logger::info("Starting OTA update download: " + info.downloadUrl);

    std::string binDir = AppConfig::instance().getBinDir();
    std::string newBinPath = binDir + "/RomCloud.new";
    std::string finalBinPath = binDir + "/RomCloud";

    FILE* fp = fopen(newBinPath.c_str(), "wb");
    if (!fp) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = UpdateState::FAILED;
        m_progress.errorMessage = "Không thể tạo tập tin: " + newBinPath;
        Logger::error(m_progress.errorMessage);
        m_isRunning = false;
        return;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = UpdateState::FAILED;
        m_progress.errorMessage = "Không thể khởi tạo phiên làm việc CURL.";
        Logger::error(m_progress.errorMessage);
        m_isRunning = false;
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, info.downloadUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "RomCloud-TrimUI-BrickPro/1.0");
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L); // 5 min timeout
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (m_cancelRequested) {
        FileSystemManager::instance().removeFile(newBinPath);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = UpdateState::IDLE;
        m_isRunning = false;
        Logger::info("OTA update cancelled by user.");
        return;
    }

    if (res != CURLE_OK || httpCode < 200 || httpCode >= 300) {
        FileSystemManager::instance().removeFile(newBinPath);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = UpdateState::FAILED;
        m_progress.errorMessage = "Tải về thất bại (Mã phản hồi HTTP " + std::to_string(httpCode) + "): " + curl_easy_strerror(res);
        Logger::error(m_progress.errorMessage);
        m_isRunning = false;
        return;
    }

    // Verify downloaded binary: check minimum size (> 1MB)
    size_t newSize = FileSystemManager::instance().getFileSize(newBinPath);
    if (newSize < 1000000) {
        FileSystemManager::instance().removeFile(newBinPath);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = UpdateState::FAILED;
        m_progress.errorMessage = "Tập tin tải về không hợp lệ (kích thước quá nhỏ).";
        Logger::error(m_progress.errorMessage);
        m_isRunning = false;
        return;
    }

    // Make executable
    chmod(newBinPath.c_str(), 0755);

    // Atomic replace if supported or move
    std::string oldBinPath = binDir + "/RomCloud.old";
    FileSystemManager::instance().removeFile(oldBinPath);
    rename(finalBinPath.c_str(), oldBinPath.c_str());
    if (rename(newBinPath.c_str(), finalBinPath.c_str()) != 0) {
        // Fallback: restore old binary
        rename(oldBinPath.c_str(), finalBinPath.c_str());
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = UpdateState::FAILED;
        m_progress.errorMessage = "Không thể ghi đè tập tin nhị phân mới.";
        Logger::error(m_progress.errorMessage);
        m_isRunning = false;
        return;
    }
    chmod(finalBinPath.c_str(), 0755);

    Logger::info("OTA update installed successfully! Ready to restart as v" + info.remoteVersion);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = UpdateState::COMPLETED;
        m_progress.progressPct = 100.0;
    }

    m_isRunning = false;
}

} // namespace RomCloud
