#include "UploadManager.h"
#include "../logging/Logger.h"
#include "../config/AppConfig.h"
#include "../filesystem/FileSystemManager.h"
#include "../auth/AuthManager.h"
#include "../network/HttpClient.h"
#include "../network/JsonHelper.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <curl/curl.h>

namespace RomCloud {

static size_t streamReadCallback(char* ptr, size_t size, size_t nmemb, void* stream) {
    std::ifstream* file = static_cast<std::ifstream*>(stream);
    if (!file || file->eof()) return 0;
    file->read(ptr, size * nmemb);
    return file->gcount();
}

static size_t headerCallback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    std::string* header = static_cast<std::string*>(userdata);
    header->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

static size_t writeCallback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    std::string* response = static_cast<std::string*>(userdata);
    response->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

static int progressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal;
    (void)dlnow;
    UploadManager* self = static_cast<UploadManager*>(clientp);
    if (!self) return 0;
    if (self->isCancelRequested()) {
        return 1; // Aborts cURL transfer
    }
    self->updateProgress(static_cast<uint64_t>(ulnow), static_cast<uint64_t>(ultotal));
    return 0;
}

UploadManager& UploadManager::instance() {
    static UploadManager instance;
    return instance;
}

bool UploadManager::init() {
    Logger::info("UploadManager initialized");
    return true;
}

void UploadManager::shutdown() {
    cancel();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    Logger::info("UploadManager shutdown");
}

void UploadManager::cancel() {
    m_cancelRequested = true;
}

bool UploadManager::isUploading() const {
    return m_isRunning;
}

UploadProgress UploadManager::getProgress() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_progress;
}

std::vector<LocalGameInfo> UploadManager::getLocalOnlyGames() const {
    return m_uploadQueue;
}

void UploadManager::updateProgress(uint64_t bytesUploaded, uint64_t totalBytes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress.bytesUploaded = bytesUploaded;
    if (totalBytes > 0) {
        m_progress.totalBytes = totalBytes;
        m_progress.progressPct = (static_cast<double>(bytesUploaded) / static_cast<double>(totalBytes)) * 100.0;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastSpeedTime).count();
    if (elapsedMs >= 500) {
        if (bytesUploaded >= m_lastSpeedBytes && elapsedMs > 0) {
            uint64_t diffBytes = bytesUploaded - m_lastSpeedBytes;
            m_progress.speedKBps = (static_cast<double>(diffBytes) / 1024.0) / (static_cast<double>(elapsedMs) / 1000.0);
        }
        m_lastSpeedTime = now;
        m_lastSpeedBytes = bytesUploaded;
    }
}

std::vector<LocalGameInfo> UploadManager::scanLocalOnlyGames() {
    std::vector<LocalGameInfo> localOnly;

    // Get all systems
    auto systems = DatabaseManager::instance().getSystems(false);

    for (const auto& sys : systems) {
        // Get all games for this system
        auto games = DatabaseManager::instance().getGamesBySystem(sys.id, static_cast<int>(GameState::LOCAL));

        for (const auto& game : games) {
            LocalGameInfo info;
            info.gameId = game.id;
            info.filename = game.filename;
            info.title = game.title;
            info.sizeBytes = game.sizeBytes;
            info.localPath = game.localPath;
            info.systemId = sys.id;
            info.systemCode = sys.code;
            info.cloudFileId = game.cloudFileId;
            info.needsUpload = true;

            // Check if file actually exists locally on SD card
            if (!info.localPath.empty() && FileSystemManager::instance().fileExists(info.localPath)) {
                localOnly.push_back(info);
            }
        }
    }

    Logger::info("UploadManager: Found " + std::to_string(localOnly.size()) + " local games on SD card ready for backup");
    return localOnly;
}

std::string UploadManager::findOrCreateDriveFolder(const std::string& folderName, const std::string& parentFolderId, const std::string& token) {
    // 1. Search for existing folder
    std::string query;
    if (!parentFolderId.empty()) {
        query = "name = '" + folderName + "' and '" + parentFolderId + "' in parents and mimeType = 'application/vnd.google-apps.folder' and trashed = false";
    } else {
        query = "name = '" + folderName + "' and 'root' in parents and mimeType = 'application/vnd.google-apps.folder' and trashed = false";
    }

    std::string searchUrl = "https://www.googleapis.com/drive/v3/files?q=" + HttpClient::instance().urlEncode(query) +
                            "&fields=files(id,name)&pageSize=1&supportsAllDrives=true&includeItemsFromAllDrives=true";
    std::vector<std::string> headers = {
        "Authorization: Bearer " + token
    };

    HttpResponse resp = HttpClient::instance().get(searchUrl, headers);
    if (resp.success) {
        auto files = JsonHelper::extractArrayObjects(resp.body, "files");
        if (!files.empty()) {
            std::string folderId = JsonHelper::extractString(files[0], "id");
            if (!folderId.empty()) {
                Logger::info("UploadManager: Found existing Drive folder '" + folderName + "' (ID: " + folderId + ")");
                return folderId;
            }
        }
    }

    // 2. Not found, create folder
    std::string createUrl = "https://www.googleapis.com/drive/v3/files?supportsAllDrives=true";
    std::string createBody = "{\"name\":\"" + folderName + "\",\"mimeType\":\"application/vnd.google-apps.folder\"";
    if (!parentFolderId.empty()) {
        createBody += ",\"parents\":[\"" + parentFolderId + "\"]";
    }
    createBody += "}";

    std::vector<std::string> createHeaders = {
        "Authorization: Bearer " + token,
        "Content-Type: application/json; charset=UTF-8"
    };

    HttpResponse createResp = HttpClient::instance().post(createUrl, createBody, createHeaders);
    if (createResp.success && (createResp.statusCode == 200 || createResp.statusCode == 201)) {
        std::string folderId = JsonHelper::extractString(createResp.body, "id");
        Logger::info("UploadManager: Created new Drive folder '" + folderName + "' (ID: " + folderId + ")");
        return folderId;
    }

    Logger::error("UploadManager: Failed to create Drive folder '" + folderName + "': " + createResp.body);
    return "";
}

std::string UploadManager::getTargetFolderForGame(const LocalGameInfo& game, const std::string& token) {
    // 1. Resolve Root Backup Folder ("RomCloud_Backup")
    if (m_rootBackupFolderId.empty()) {
        std::string configured = DatabaseManager::instance().getSetting("drive_backup_folder_id", "");
        if (!configured.empty()) {
            m_rootBackupFolderId = configured;
        } else {
            m_rootBackupFolderId = findOrCreateDriveFolder("RomCloud_Backup", "", token);
            if (!m_rootBackupFolderId.empty()) {
                DatabaseManager::instance().setSetting("drive_backup_folder_id", m_rootBackupFolderId);
            }
        }
    }

    if (m_rootBackupFolderId.empty()) {
        Logger::error("UploadManager: Could not resolve root backup folder on Drive");
        return "";
    }

    // 2. Resolve System Subfolder (e.g. "GBA", "FC", "PS")
    std::string sysCode = game.systemCode.empty() ? "OTHER" : game.systemCode;
    auto it = m_systemFolderCache.find(sysCode);
    if (it != m_systemFolderCache.end()) {
        return it->second;
    }

    std::string sysFolderId = findOrCreateDriveFolder(sysCode, m_rootBackupFolderId, token);
    if (!sysFolderId.empty()) {
        m_systemFolderCache[sysCode] = sysFolderId;
        return sysFolderId;
    }

    return m_rootBackupFolderId;
}

void UploadManager::startReverseSync() {
    if (m_isRunning) {
        Logger::warn("Upload already in progress");
        return;
    }

    if (!AuthManager::instance().canUpload()) {
        Logger::error("Cannot upload: no personal Google Drive account linked");
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = UploadState::FAILED;
        m_progress.errorMessage = "Kho hiện tại là thư mục công khai (chỉ đọc).\nCần liên kết Drive cá nhân trong Cài đặt hoặc Web để sao lưu.";
        return;
    }

    m_cancelRequested = false;
    m_isRunning = true;
    m_uploadQueue = scanLocalOnlyGames();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress = UploadProgress();
        m_progress.state = UploadState::PREPARING;
        m_progress.totalGames = static_cast<int>(m_uploadQueue.size());
    }

    Logger::info("UploadManager: Starting reverse sync with " + std::to_string(m_uploadQueue.size()) + " games");

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    m_workerThread = std::thread([this]() {
        runWorker();
    });
}

void UploadManager::startUploadGames(const std::vector<int64_t>& gameIds) {
    if (m_isRunning) {
        Logger::warn("Upload already in progress");
        return;
    }

    if (!AuthManager::instance().canUpload()) {
        Logger::error("Cannot upload: no personal Google Drive account linked");
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = UploadState::FAILED;
        m_progress.errorMessage = "Kho hiện tại là thư mục công khai (chỉ đọc).\nCần liên kết Drive cá nhân trong Cài đặt hoặc Web để sao lưu.";
        return;
    }

    m_cancelRequested = false;
    m_isRunning = true;
    m_uploadQueue.clear();

    auto& db = DatabaseManager::instance();
    for (int64_t id : gameIds) {
        GameRecord g;
        if (db.getGameById(id, g)) {
            if (g.localState == GameState::LOCAL && !g.localPath.empty() && FileSystemManager::instance().fileExists(g.localPath)) {
                LocalGameInfo info;
                info.gameId = g.id;
                info.filename = g.filename;
                info.title = g.title;
                info.sizeBytes = g.sizeBytes;
                info.localPath = g.localPath;
                info.systemId = g.systemId;
                info.cloudFileId = g.cloudFileId;
                info.needsUpload = g.cloudFileId.empty();

                SystemRecord sys;
                if (db.getSystemById(g.systemId, sys)) {
                    info.systemCode = sys.code;
                }

                m_uploadQueue.push_back(info);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress = UploadProgress();
        m_progress.state = UploadState::PREPARING;
        m_progress.totalGames = static_cast<int>(m_uploadQueue.size());
    }

    Logger::info("UploadManager: Starting upload of " + std::to_string(m_uploadQueue.size()) + " selected games");

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    m_workerThread = std::thread([this]() {
        runWorker();
    });
}

void UploadManager::runWorker() {
    int total = static_cast<int>(m_uploadQueue.size());

    for (int i = 0; i < total; ++i) {
        if (m_cancelRequested) {
            Logger::info("UploadManager: Cancelled by user");
            std::lock_guard<std::mutex> lock(m_mutex);
            m_progress.state = UploadState::CANCELLED;
            break;
        }

        const auto& game = m_uploadQueue[i];

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_progress.state = UploadState::UPLOADING;
            m_progress.currentIndex = i + 1;
            m_progress.gameId = game.gameId;
            m_progress.gameTitle = game.title;
            m_progress.systemCode = game.systemCode;
            m_progress.filename = game.filename;
            m_progress.totalBytes = game.sizeBytes;
            m_progress.bytesUploaded = 0;
            m_progress.progressPct = 0.0;
            m_progress.speedKBps = 0.0;
        }

        Logger::info("UploadManager: Uploading " + game.filename + " (" + std::to_string(i + 1) + "/" + std::to_string(total) + ")");

        bool success = uploadSingleGame(game);

        std::lock_guard<std::mutex> lock(m_mutex);
        if (success) {
            m_progress.gamesUploaded++;
        } else {
            m_progress.gamesFailed++;
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_progress.state == UploadState::UPLOADING || m_progress.state == UploadState::PREPARING) {
        m_progress.state = UploadState::COMPLETED;
        m_isRunning = false;

        std::string msg = "Hoàn tất: " + std::to_string(m_progress.gamesUploaded) +
                         " thành công, " + std::to_string(m_progress.gamesFailed) + " thất bại";
        Logger::info("UploadManager: " + msg);

        if (onComplete) {
            onComplete(m_progress.gamesFailed == 0, msg);
        }
    } else {
        m_isRunning = false;
    }
}

bool UploadManager::uploadSingleGame(const LocalGameInfo& game) {
    // 1. Get OAuth token
    std::string accessToken = AuthManager::instance().getValidAccessToken();
    if (accessToken.empty()) {
        Logger::error("UploadManager: No personal access token available");
        return false;
    }

    // 2. Open local file and determine file size
    std::ifstream file(game.localPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        Logger::error("UploadManager: Cannot open local file: " + game.localPath);
        return false;
    }

    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    if (fileSize <= 0) {
        Logger::error("UploadManager: Empty file, skipping: " + game.localPath);
        return false;
    }

    // 3. Resolve destination folder on Google Drive
    std::string folderId = getTargetFolderForGame(game, accessToken);
    if (folderId.empty()) {
        Logger::error("UploadManager: No Drive folder available for system " + game.systemCode);
        return false;
    }

    // 4. Determine MIME type
    std::string mimeType = "application/octet-stream";
    size_t dotPos = game.filename.find_last_of('.');
    if (dotPos != std::string::npos) {
        std::string ext = game.filename.substr(dotPos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == "nes" || ext == "fds") mimeType = "application/x-nes";
        else if (ext == "snes" || ext == "smc" || ext == "sfc" || ext == "swc") mimeType = "application/x-snes";
        else if (ext == "gba") mimeType = "application/x-gba";
        else if (ext == "gb" || ext == "gbc") mimeType = "application/x-gameboy";
        else if (ext == "nds") mimeType = "application/x-nds";
        else if (ext == "iso" || ext == "bin" || ext == "img") mimeType = "application/x-iso9660-image";
        else if (ext == "chd") mimeType = "application/octet-stream";
        else if (ext == "pbp" || ext == "cso") mimeType = "application/octet-stream";
        else if (ext == "zip" || ext == "7z") mimeType = "application/zip";
    }

    // 5. Step A: Initiate Google Drive Resumable Upload Session
    std::string initUrl = "https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable&supportsAllDrives=true";
    std::string metadataJson = "{\"name\":\"" + game.filename + "\",\"parents\":[\"" + folderId + "\"]}";

    std::vector<std::string> initHeaders = {
        "Authorization: Bearer " + accessToken,
        "Content-Type: application/json; charset=UTF-8",
        "X-Upload-Content-Type: " + mimeType,
        "X-Upload-Content-Length: " + std::to_string(fileSize)
    };

    CURL* curlInit = curl_easy_init();
    if (!curlInit) {
        Logger::error("UploadManager: curl_easy_init failed");
        return false;
    }

    struct curl_slist* initHeaderList = nullptr;
    for (const auto& h : initHeaders) {
        initHeaderList = curl_slist_append(initHeaderList, h.c_str());
    }

    std::string initResponse;
    std::string initHeaderResponse;
    long initHttpCode = 0;

    curl_easy_setopt(curlInit, CURLOPT_URL, initUrl.c_str());
    curl_easy_setopt(curlInit, CURLOPT_POST, 1L);
    curl_easy_setopt(curlInit, CURLOPT_POSTFIELDS, metadataJson.c_str());
    curl_easy_setopt(curlInit, CURLOPT_HTTPHEADER, initHeaderList);
    curl_easy_setopt(curlInit, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curlInit, CURLOPT_WRITEDATA, &initResponse);
    curl_easy_setopt(curlInit, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curlInit, CURLOPT_HEADERDATA, &initHeaderResponse);
    curl_easy_setopt(curlInit, CURLOPT_TIMEOUT, 30L);

    CURLcode initRes = curl_easy_perform(curlInit);
    curl_easy_getinfo(curlInit, CURLINFO_RESPONSE_CODE, &initHttpCode);
    curl_easy_cleanup(curlInit);
    curl_slist_free_all(initHeaderList);

    if (initRes != CURLE_OK || (initHttpCode != 200 && initHttpCode != 201)) {
        Logger::error("UploadManager: Failed to initiate resumable upload session (HTTP " + std::to_string(initHttpCode) + "): " + initResponse);
        return false;
    }

    // Extract Location URL from response headers
    std::string sessionUrl;
    std::istringstream hStream(initHeaderResponse);
    std::string line;
    while (std::getline(hStream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string lowerLine = line;
        std::transform(lowerLine.begin(), lowerLine.end(), lowerLine.begin(), ::tolower);
        if (lowerLine.rfind("location: ", 0) == 0) {
            sessionUrl = line.substr(10);
            while (!sessionUrl.empty() && (sessionUrl.back() == ' ' || sessionUrl.back() == '\r' || sessionUrl.back() == '\n')) {
                sessionUrl.pop_back();
            }
            break;
        }
    }

    if (sessionUrl.empty()) {
        Logger::error("UploadManager: No Location header in Google Drive response");
        return false;
    }

    // 6. Step B: Stream upload the file chunks via PUT
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.bytesUploaded = 0;
        m_progress.totalBytes = static_cast<uint64_t>(fileSize);
        m_progress.progressPct = 0.0;
        m_progress.speedKBps = 0.0;
        m_lastSpeedTime = std::chrono::steady_clock::now();
        m_lastSpeedBytes = 0;
    }

    CURL* curlUpload = curl_easy_init();
    if (!curlUpload) return false;

    struct curl_slist* uploadHeaders = nullptr;
    uploadHeaders = curl_slist_append(uploadHeaders, ("Content-Type: " + mimeType).c_str());
    uploadHeaders = curl_slist_append(uploadHeaders, ("Content-Length: " + std::to_string(fileSize)).c_str());

    std::string uploadResponse;
    long uploadHttpCode = 0;

    curl_easy_setopt(curlUpload, CURLOPT_URL, sessionUrl.c_str());
    curl_easy_setopt(curlUpload, CURLOPT_UPLOAD, 1L); // Sends HTTP PUT with stream
    curl_easy_setopt(curlUpload, CURLOPT_READFUNCTION, streamReadCallback);
    curl_easy_setopt(curlUpload, CURLOPT_READDATA, &file);
    curl_easy_setopt(curlUpload, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(fileSize));
    curl_easy_setopt(curlUpload, CURLOPT_HTTPHEADER, uploadHeaders);
    curl_easy_setopt(curlUpload, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curlUpload, CURLOPT_WRITEDATA, &uploadResponse);
    curl_easy_setopt(curlUpload, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curlUpload, CURLOPT_XFERINFOFUNCTION, progressCallback);
    curl_easy_setopt(curlUpload, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curlUpload, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(curlUpload, CURLOPT_LOW_SPEED_TIME, 60L);

    CURLcode uploadRes = curl_easy_perform(curlUpload);
    curl_easy_getinfo(curlUpload, CURLINFO_RESPONSE_CODE, &uploadHttpCode);
    curl_easy_cleanup(curlUpload);
    curl_slist_free_all(uploadHeaders);
    file.close();

    if (m_cancelRequested) {
        Logger::info("UploadManager: Upload aborted by user");
        return false;
    }

    if (uploadRes != CURLE_OK) {
        Logger::error("UploadManager: Stream upload failed: " + std::string(curl_easy_strerror(uploadRes)));
        return false;
    }

    if (uploadHttpCode == 200 || uploadHttpCode == 201) {
        std::string fileId = JsonHelper::extractString(uploadResponse, "id");
        Logger::info("UploadManager: Successfully uploaded " + game.filename + " (Drive ID: " + fileId + ")");

        // Update database with cloud file ID
        if (!fileId.empty()) {
            GameRecord gameRec;
            if (DatabaseManager::instance().getGameById(game.gameId, gameRec)) {
                gameRec.cloudFileId = fileId;
                DatabaseManager::instance().upsertGame(gameRec);
            }
        }
        return true;
    } else {
        Logger::error("UploadManager: Upload failed with HTTP " + std::to_string(uploadHttpCode) + ": " + uploadResponse.substr(0, 200));
        return false;
    }
}

UploadManager::~UploadManager() {
    shutdown();
}

} // namespace RomCloud
