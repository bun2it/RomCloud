#include "DownloadManager.h"
#include "../network/HttpClient.h"
#include "../auth/AuthManager.h"
#include "../filesystem/FileSystemManager.h"
#include "../config/AppConfig.h"
#include "../ui/BoxartScraper.h"
#include "../utils/HashHelper.h"
#include "../logging/Logger.h"

#include <curl/curl.h>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <sys/stat.h>
#include <deque>

namespace RomCloud {

DownloadManager& DownloadManager::instance() {
    static DownloadManager instance;
    return instance;
}

DownloadManager::~DownloadManager() {
    shutdown();
}

bool DownloadManager::init() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress = DownloadProgress();
    m_progress.state = DownloadState::IDLE;
    Logger::info("DownloadManager initialized.");
    return true;
}

void DownloadManager::shutdown() {
    cancelDownload();
}

bool DownloadManager::isDownloading() const {
    return m_isRunning;
}

DownloadProgress DownloadManager::getProgress() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_progress;
}

void DownloadManager::resetProgress() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress = DownloadProgress();
    m_progress.state = DownloadState::IDLE;
}

// ---- Queue Management ----

bool DownloadManager::addToQueue(const GameRecord& game, const SystemRecord& sys) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Don't add duplicates
    for (const auto& item : m_queue) {
        if (item.game.id == game.id) return false;
    }
    m_queue.push_back({game, sys});
    Logger::info("Queued for download: " + game.title + " (Queue size: " + std::to_string(m_queue.size()) + ")");
    return true;
}

bool DownloadManager::removeFromQueue(int64_t gameId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
        if (it->game.id == gameId) {
            Logger::info("Removed from queue: " + it->game.title);
            m_queue.erase(it);
            return true;
        }
    }
    return false;
}

bool DownloadManager::isInQueue(int64_t gameId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& item : m_queue) {
        if (item.game.id == gameId) return true;
    }
    return false;
}

std::deque<QueueItem> DownloadManager::getQueue() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue;
}

int DownloadManager::queueSize() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_queue.size());
}

void DownloadManager::clearQueue() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.clear();
    Logger::info("Download queue cleared.");
}

void DownloadManager::processNextInQueue() {
    if (m_isRunning) return;
    QueueItem next;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) return;
        next = m_queue.front();
        m_queue.pop_front();
    }
    Logger::info("Auto-starting next queued download: " + next.game.title);
    startDownload(next.game, next.sys);
}

void DownloadManager::cancelDownload() {
    m_cancelRequested = true;
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    m_isRunning = false;

    // Clean up partial file
    if (!m_tempFilePath.empty()) {
        FileSystemManager::instance().removeFile(m_tempFilePath);
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_progress.state != DownloadState::COMPLETED) {
        m_progress.state = DownloadState::CANCELLED;
    }
}

int DownloadManager::xferCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    (void)ultotal;
    (void)ulnow;
    auto* self = static_cast<DownloadManager*>(clientp);
    if (!self) return 0;

    if (self->m_cancelRequested) {
        return 1; // Abort CURL transfer
    }

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

bool DownloadManager::startDownload(const GameRecord& game, const SystemRecord& sys) {
    if (m_isRunning) {
        Logger::warn("A download is already in progress, ignoring new download request.");
        return false;
    }

    if (!AuthManager::instance().isLinked()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = DownloadState::FAILED;
        m_progress.errorMessage = "Not connected to Google Drive. Link account in Settings.";
        Logger::warn(m_progress.errorMessage);
        return false;
    }

    if (game.cloudFileId.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = DownloadState::FAILED;
        m_progress.errorMessage = "Game does not have a valid Google Drive file ID.";
        Logger::error(m_progress.errorMessage);
        return false;
    }

    // Disk space check: required = game size + 50MB safety buffer
    uint64_t requiredSpace = game.sizeBytes + (50ULL * 1024ULL * 1024ULL);
    auto diskSpace = FileSystemManager::instance().getDiskSpace(AppConfig::instance().getRomsDir());
    if (diskSpace.availableBytes < requiredSpace && diskSpace.availableBytes > 0) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = DownloadState::FAILED;
        m_progress.errorMessage = "Insufficient SD Card space: Need " +
                                  FileSystemManager::instance().formatBytes(requiredSpace) +
                                  ", Available: " +
                                  FileSystemManager::instance().formatBytes(diskSpace.availableBytes);
        Logger::error(m_progress.errorMessage);
        return false;
    }

    m_activeGame = game;
    m_activeSystem = sys;

    // Temporary path: /mnt/SDCARD/Apps/RomCloud/temp/<filename>.part
    m_tempFilePath = AppConfig::instance().getTempDir() + "/" + game.filename + ".part";

    // Final destination: /mnt/SDCARD/Roms/<sys.romDir>/<filename>
    std::string sysRomDir = AppConfig::instance().getRomsDir() + "/" + sys.romDir;
    FileSystemManager::instance().createDirectoryRecursive(sysRomDir);
    m_finalFilePath = sysRomDir + "/" + game.filename;

    m_cancelRequested = false;
    m_isRunning = true;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress = DownloadProgress();
        m_progress.state = DownloadState::INITIALIZING;
        m_progress.gameId = game.id;
        m_progress.gameTitle = game.title;
        m_progress.systemCode = sys.code;
        m_progress.filename = game.filename;
        m_progress.totalBytes = game.sizeBytes;
    }

    Logger::info("Starting on-demand download for: " + game.title + " (" + game.filename + ")");

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    m_workerThread = std::thread(&DownloadManager::runDownloadWorker, this);
    return true;
}

void DownloadManager::runDownloadWorker() {
    std::string token = AuthManager::instance().getValidAccessToken();
    std::string url;
    if (!token.empty()) {
        // Authenticated: use Drive API
        url = "https://www.googleapis.com/drive/v3/files/" + m_activeGame.cloudFileId + "?alt=media";
    } else if (!m_activeGame.cloudFileId.empty()) {
        // Public folder: use uc endpoint with confirm bypass
        url = "https://drive.google.com/uc?export=download&confirm=t&id=" + m_activeGame.cloudFileId;
    } else {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = DownloadState::FAILED;
        m_progress.errorMessage = "Missing cloud file identifier.";
        Logger::error(m_progress.errorMessage);
        m_isRunning = false;
        return;
    }

    FILE* fp = fopen(m_tempFilePath.c_str(), "wb");
    if (!fp) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = DownloadState::FAILED;
        m_progress.errorMessage = "Cannot create temporary file: " + m_tempFilePath;
        Logger::error(m_progress.errorMessage);
        m_isRunning = false;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = DownloadState::DOWNLOADING;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        FileSystemManager::instance().removeFile(m_tempFilePath);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = DownloadState::FAILED;
        m_progress.errorMessage = "Failed to initialize CURL handle.";
        m_isRunning = false;
        return;
    }

    struct curl_slist* headers = nullptr;
    if (!token.empty()) {
        std::string authHeader = "Authorization: Bearer " + token;
        headers = curl_slist_append(headers, authHeader.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "RomCloud-TrimUI-BrickPro/1.0");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L); // 1 KB/s limit
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);    // 30 seconds
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_off_t speedBytesPerSec = 0;
    curl_easy_getinfo(curl, CURLINFO_SPEED_DOWNLOAD_T, &speedBytesPerSec);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (m_cancelRequested) {
        FileSystemManager::instance().removeFile(m_tempFilePath);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = DownloadState::CANCELLED;
        m_isRunning = false;
        Logger::info("Download cancelled by user.");
        return;
    }

    // --- Google Drive virus-scan confirmation page detection ---
    // If Drive returns a small HTML page (< 64KB) with a confirm token,
    // we must re-download using the confirm=t URL parameter.
    {
        struct stat st;
        long fileSize = 0;
        if (stat(m_tempFilePath.c_str(), &st) == 0) fileSize = st.st_size;

        if (httpCode == 200 && fileSize > 0 && fileSize < 128 * 1024 &&
            m_activeGame.sizeBytes > 256 * 1024) {
            // Suspect it's an HTML confirmation page, not the real file.
            std::string htmlContent;
            FILE* checkFp = fopen(m_tempFilePath.c_str(), "r");
            bool isHtml = false;
            if (checkFp) {
                char buf[8192] = {0};
                size_t n = fread(buf, 1, sizeof(buf) - 1, checkFp);
                fclose(checkFp);
                htmlContent = std::string(buf, n);
                isHtml = (htmlContent.find("<!DOCTYPE") != std::string::npos ||
                          htmlContent.find("<html") != std::string::npos ||
                          htmlContent.find("virus scan") != std::string::npos ||
                          htmlContent.find("Download anyway") != std::string::npos ||
                          htmlContent.find("drive.usercontent.google.com") != std::string::npos);
            }
            if (isHtml) {
                Logger::info("Detected Drive virus-scan warning page. Extracting bypass form tokens...");
                FileSystemManager::instance().removeFile(m_tempFilePath);

                // Extract action URL
                std::string formAction = "https://drive.usercontent.google.com/download";
                size_t actPos = htmlContent.find("action=\"");
                if (actPos != std::string::npos) {
                    actPos += 8;
                    size_t endAct = htmlContent.find('\"', actPos);
                    if (endAct != std::string::npos) {
                        formAction = htmlContent.substr(actPos, endAct - actPos);
                    }
                }

                // Extract uuid token
                std::string uuid;
                size_t uuidPos = htmlContent.find("name=\"uuid\" value=\"");
                if (uuidPos != std::string::npos) {
                    uuidPos += 19;
                    size_t endUuid = htmlContent.find('\"', uuidPos);
                    if (endUuid != std::string::npos) {
                        uuid = htmlContent.substr(uuidPos, endUuid - uuidPos);
                    }
                }

                std::string bypassUrl = formAction + "?id=" + m_activeGame.cloudFileId + "&export=download&confirm=t";
                if (!uuid.empty()) {
                    bypassUrl += "&uuid=" + uuid;
                }
                Logger::info("Retrying download with real direct URL: " + bypassUrl);

                FILE* fp2 = fopen(m_tempFilePath.c_str(), "wb");
                if (!fp2) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_progress.state = DownloadState::FAILED;
                    m_progress.errorMessage = "Cannot reopen temp file for retry download.";
                    m_isRunning = false;
                    return;
                }

                CURL* curl2 = curl_easy_init();
                if (!curl2) { fclose(fp2); m_isRunning = false; return; }
                curl_easy_setopt(curl2, CURLOPT_URL, bypassUrl.c_str());
                curl_easy_setopt(curl2, CURLOPT_COOKIEFILE, "");
                curl_easy_setopt(curl2, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
                curl_easy_setopt(curl2, CURLOPT_WRITEFUNCTION, fwrite);
                curl_easy_setopt(curl2, CURLOPT_WRITEDATA, fp2);
                curl_easy_setopt(curl2, CURLOPT_NOPROGRESS, 0L);
                curl_easy_setopt(curl2, CURLOPT_XFERINFOFUNCTION, xferCallback);
                curl_easy_setopt(curl2, CURLOPT_XFERINFODATA, this);
                curl_easy_setopt(curl2, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(curl2, CURLOPT_SSL_VERIFYPEER, 0L);
                curl_easy_setopt(curl2, CURLOPT_SSL_VERIFYHOST, 0L);
                curl_easy_setopt(curl2, CURLOPT_CONNECTTIMEOUT, 15);
                curl_easy_setopt(curl2, CURLOPT_LOW_SPEED_LIMIT, 1024L);
                curl_easy_setopt(curl2, CURLOPT_LOW_SPEED_TIME, 30L);
                res = curl_easy_perform(curl2);
                curl_easy_getinfo(curl2, CURLINFO_RESPONSE_CODE, &httpCode);
                curl_easy_cleanup(curl2);
                fclose(fp2);
                Logger::info("Retry download complete: HTTP " + std::to_string(httpCode));
            }
        }
    }
    // --- end virus scan bypass ---

    if (res != CURLE_OK || httpCode != 200) {
        FileSystemManager::instance().removeFile(m_tempFilePath);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = DownloadState::FAILED;
        m_progress.errorMessage = "Download failed: " + std::string(curl_easy_strerror(res)) + " (HTTP " + std::to_string(httpCode) + ")";
        Logger::error(m_progress.errorMessage);
        m_isRunning = false;
        return;
    }

    // Checksum verification
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = DownloadState::VERIFYING;
    }

    Logger::info("Download completed successfully, calculating MD5 checksum...");
    std::string computedMd5 = HashHelper::computeFileMd5(m_tempFilePath);

    if (!m_activeGame.checksumSha256.empty() && !computedMd5.empty()) {
        std::string expected = m_activeGame.checksumSha256;
        std::string actual = computedMd5;
        std::transform(expected.begin(), expected.end(), expected.begin(), ::tolower);
        std::transform(actual.begin(), actual.end(), actual.begin(), ::tolower);

        if (expected != actual) {
            FileSystemManager::instance().removeFile(m_tempFilePath);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_progress.state = DownloadState::FAILED;
            m_progress.errorMessage = "Checksum mismatch: Expected " + expected + ", Got " + actual;
            Logger::error(m_progress.errorMessage);
            m_isRunning = false;
            return;
        }
        Logger::info("MD5 Checksum verified match: " + actual);
    }

    // Ensure destination directory exists
    size_t lastSlash = m_finalFilePath.find_last_of('/');
    if (lastSlash != std::string::npos) {
        FileSystemManager::instance().createDirectoryRecursive(m_finalFilePath.substr(0, lastSlash));
    }

    // Move file to final destination
    if (std::rename(m_tempFilePath.c_str(), m_finalFilePath.c_str()) != 0) {
        // Fallback: Copy and remove temp file
        FILE* srcFp = fopen(m_tempFilePath.c_str(), "rb");
        FILE* dstFp = fopen(m_finalFilePath.c_str(), "wb");
        bool copyOk = false;
        if (srcFp && dstFp) {
            char copyBuf[65536];
            size_t bytes;
            copyOk = true;
            while ((bytes = fread(copyBuf, 1, sizeof(copyBuf), srcFp)) > 0) {
                if (fwrite(copyBuf, 1, bytes, dstFp) != bytes) {
                    copyOk = false;
                    break;
                }
            }
        }
        if (srcFp) fclose(srcFp);
        if (dstFp) fclose(dstFp);

        FileSystemManager::instance().removeFile(m_tempFilePath);

        if (!copyOk) {
            FileSystemManager::instance().removeFile(m_finalFilePath);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_progress.state = DownloadState::FAILED;
            m_progress.errorMessage = "Failed to place file into: " + m_finalFilePath;
            Logger::error(m_progress.errorMessage);
            m_isRunning = false;
            return;
        }
    }

    // Update SQLite database to LOCAL state
    DatabaseManager::instance().updateGameLocalState(m_activeGame.id, GameState::LOCAL, m_finalFilePath);
    Logger::info("ROM installed locally at: " + m_finalFilePath + ". State transitioned to LOCAL.");

    // Auto-scrape boxart cover art if not present
    std::string scrapedCover;
    if (BoxartScraper::instance().scrapeCover(m_activeGame, m_activeSystem, scrapedCover)) {
        DatabaseManager::instance().updateGameCover(m_activeGame.id, scrapedCover);
        Logger::info("Auto-scraped boxart for game: " + m_activeGame.title + " -> " + scrapedCover);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = DownloadState::COMPLETED;
        m_progress.speedKBps = speedBytesPerSec / 1024.0;
    }

    m_isRunning = false;

    // Auto-start next item in queue if any
    // (Uses a small delay to let UIManager pick up COMPLETED state first)
    // processNextInQueue() is called from UIManager::update() after it detects COMPLETED
}

} // namespace RomCloud
