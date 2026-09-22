#include "UpdateManager.h"
#include "../network/HttpClient.h"
#include "../network/JsonHelper.h"
#include "../logging/Logger.h"
#include "../config/AppConfig.h"
#include "../filesystem/FileSystemManager.h"

#include <curl/curl.h>
#include <sys/stat.h>
#include <unistd.h>
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

    // 1. Try version.json manifest with cache-buster timestamp (immune to GitHub API rate limits and CDN caching)
    std::string manifestUrl = std::string(VERSION_MANIFEST_URL) + "?t=" + std::to_string(std::time(nullptr));
    std::vector<std::string> manifestHeaders = {
        "User-Agent: RomCloud-OTA/1.0 (TrimUI Brick Pro)",
        "Cache-Control: no-cache, no-store, must-revalidate",
        "Pragma: no-cache"
    };
    HttpResponse mResp = HttpClient::instance().get(manifestUrl, manifestHeaders);
    
    std::string remoteVer = "";
    std::string changelog = "";
    std::string relDate = "";
    std::string binUrl = "";

    if (mResp.success && !mResp.body.empty() && mResp.statusCode == 200) {
        remoteVer = JsonHelper::extractString(mResp.body, "version");
        binUrl = JsonHelper::extractString(mResp.body, "binary_url");
        if (binUrl.empty()) binUrl = JsonHelper::extractString(mResp.body, "download_url");
        changelog = JsonHelper::extractString(mResp.body, "changelog");
        relDate = JsonHelper::extractString(mResp.body, "release_date");
    }

    // 2. Fallback to GitHub Releases API if manifest was empty
    if (remoteVer.empty()) {
        Logger::info("Checking GitHub Releases API as fallback...");
        std::string apiEndpoint = "https://api.github.com/repos/" + std::string(GITHUB_REPO) + "/releases/latest";
        HttpResponse resp = HttpClient::instance().get(apiEndpoint, headers);
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

                // Try to get download URL from release assets
                auto assets = JsonHelper::extractArrayObjects(resp.body, "assets");
                for (const auto& asset : assets) {
                    std::string name = JsonHelper::extractString(asset, "name");
                    if (name == "RomCloud" || name == "RomCloud.bin") {
                        binUrl = JsonHelper::extractString(asset, "browser_download_url");
                        break;
                    }
                }
                if (binUrl.empty()) {
                    binUrl = "https://github.com/" + std::string(GITHUB_REPO) + "/releases/download/" + tag + "/RomCloud";
                }
            }
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

    Logger::info("OTA: Downloading to " + newBinPath);
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
    Logger::info("OTA: Downloaded file size: " + std::to_string(newSize) + " bytes");
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

    // Ensure all downloaded data is committed to SD card
    sync();

    // Try to replace the binary
    // On FAT32 SD card (TrimUI), we cannot rename/replace a running binary
    // Solution: Use a helper script to do the replacement on next boot

    std::string helperScript = binDir + "/ota_install.sh";
    std::string installCmd = "mv -f '" + newBinPath + "' '" + finalBinPath + "' && chmod +x '" + finalBinPath + "'";

    // Write helper script that will be executed by launch.sh or manually
    FILE* scriptFile = fopen(helperScript.c_str(), "w");
    if (scriptFile) {
        fprintf(scriptFile, "#!/bin/sh\n");
        fprintf(scriptFile, "if [ -f '%s' ]; then\n", newBinPath.c_str());
        fprintf(scriptFile, "    mv -f '%s' '%s' 2>/dev/null\n", newBinPath.c_str(), finalBinPath.c_str());
        fprintf(scriptFile, "fi\n");
        fprintf(scriptFile, "chmod +x '%s' 2>/dev/null\n", finalBinPath.c_str());
        fprintf(scriptFile, "rm -f '%s' 2>/dev/null\n", helperScript.c_str());
        fprintf(scriptFile, "echo 'OTA install complete'\n");
        fclose(scriptFile);
        chmod(helperScript.c_str(), 0755);
        Logger::info("OTA: Created install script: " + helperScript);
    }

    // Also try direct replacement (works on ext4, may fail on FAT32)
    bool replaced = false;
    std::string oldBinPath = binDir + "/RomCloud.old";
    FileSystemManager::instance().removeFile(oldBinPath);

    if (rename(finalBinPath.c_str(), oldBinPath.c_str()) == 0) {
        if (rename(newBinPath.c_str(), finalBinPath.c_str()) == 0) {
            chmod(finalBinPath.c_str(), 0755);
            FileSystemManager::instance().removeFile(oldBinPath);
            replaced = true;
            FileSystemManager::instance().removeFile(helperScript);
            Logger::info("OTA: Direct binary replacement succeeded (ext4 filesystem).");
        } else {
            rename(oldBinPath.c_str(), finalBinPath.c_str());
            Logger::warn("OTA: Direct replacement failed (FAT32 filesystem).");
        }
    } else {
        Logger::warn("OTA: Could not backup old binary (file may be locked).");
    }

    if (!replaced) {
        Logger::info("OTA: Binary replacement deferred. Install script created.");
        Logger::info("OTA: App will restart. Run: sh " + helperScript + " to complete installation.");
    }

    sync();

    Logger::info("OTA update ready! File size: " + std::to_string(newSize) + " bytes");
    Logger::info("OTA: Ready to restart. App will exit with code 42.");

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = UpdateState::COMPLETED;
        m_progress.progressPct = 100.0;
    }

    m_isRunning = false;
}

} // namespace RomCloud
