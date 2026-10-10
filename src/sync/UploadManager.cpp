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
#include <sys/stat.h>
#include <dirent.h>
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
            info.romDir = sys.romDir;
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
    } else {
        createBody += ",\"parents\":[\"root\"]";
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
    // 1. Resolve Root "Roms" Folder on Google Drive ("root/Roms")
    // The DB setting is cleared whenever the Google account changes
    // (logout / new login), so trust it over the in-memory caches:
    // an id left over from a previous account would 404 every upload.
    {
        std::string configured = DatabaseManager::instance().getSetting("drive_roms_folder_id", "");
        if (configured != m_rootBackupFolderId) {
            m_rootBackupFolderId = configured;
            m_systemFolderCache.clear();
        }
    }
    if (m_rootBackupFolderId.empty()) {
        // Find or create "Roms" folder directly under root
        m_rootBackupFolderId = findOrCreateDriveFolder("Roms", "", token);
        if (!m_rootBackupFolderId.empty()) {
            DatabaseManager::instance().setSetting("drive_roms_folder_id", m_rootBackupFolderId);
        }
    }

    if (m_rootBackupFolderId.empty()) {
        Logger::error("UploadManager: Could not resolve root 'Roms' folder on Google Drive");
        return "";
    }

    // 2. Resolve System Subfolder inside "Roms" (e.g. root/Roms/GBA, root/Roms/FC, root/Roms/PS)
    // Ưu tiên romDir (ví dụ GBA, FC, SFC, PS...), fallback về systemCode
    std::string pathName = !game.romDir.empty() ? game.romDir : (!game.systemCode.empty() ? game.systemCode : "OTHER");
    auto it = m_systemFolderCache.find(pathName);
    if (it != m_systemFolderCache.end()) {
        return it->second;
    }

    // Tìm hoặc tự tạo mới thư mục hệ game nằm bên trong root/Roms
    std::string sysFolderId = findOrCreateDriveFolder(pathName, m_rootBackupFolderId, token);
    if (!sysFolderId.empty()) {
        m_systemFolderCache[pathName] = sysFolderId;
        Logger::info("UploadManager: Destination folder ready at root/Roms/" + pathName + " (ID: " + sysFolderId + ")");
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

    if (AuthManager::instance().getValidAccessToken().empty()) {
        Logger::error("Cannot upload: Google login session expired");
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = UploadState::FAILED;
        m_progress.errorMessage = "Phiên đăng nhập Google đã hết hạn. Vào mục Đồng bộ & Thẻ nhớ, đăng nhập lại rồi sao lưu tiếp.";
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
        if (m_uploadQueue.empty()) {
            m_progress.errorMessage = "Chưa có game nào trên thẻ để sao lưu.";
        }
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

    if (AuthManager::instance().getValidAccessToken().empty()) {
        Logger::error("Cannot upload: Google login session expired");
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = UploadState::FAILED;
        m_progress.errorMessage = "Phiên đăng nhập Google đã hết hạn. Vào mục Đồng bộ & Thẻ nhớ, đăng nhập lại rồi sao lưu tiếp.";
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
                    info.romDir = sys.romDir;
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
        if (m_uploadQueue.empty()) {
            m_progress.errorMessage = "Game đã chọn không còn trên thẻ nhớ.";
        }
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

        bool stopBatch = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (success) {
                m_progress.gamesUploaded++;
            } else {
                m_progress.gamesFailed++;
                if (!m_lastUploadError.empty()) {
                    m_progress.errorMessage = m_lastUploadError;
                }
                stopBatch = m_lastErrorIsAuth;
            }
        }
        if (stopBatch) {
            // Token rejected: every remaining game would fail the same
            // way, so stop here and tell the user to log in again.
            Logger::error("UploadManager: Stopping batch, Google login session is invalid");
            std::lock_guard<std::mutex> lock(m_mutex);
            m_progress.state = UploadState::FAILED;
            m_progress.errorMessage = "Phiên đăng nhập Google đã hết hạn. Vào mục Đồng bộ & Thẻ nhớ, đăng nhập lại rồi sao lưu tiếp.";
            break;
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

bool UploadManager::uploadOneFile(const std::string& localPath, const std::string& driveName, const std::string& folderId, const std::string& token) {
    m_lastUploadedFileId.clear();

    // Open local file and determine file size
    std::ifstream file(localPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        m_lastUploadError = "Không mở được file trên thẻ: " + driveName;
        Logger::error("UploadManager: Cannot open local file: " + localPath);
        return false;
    }

    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    if (fileSize <= 0) {
        m_lastUploadError = "File rỗng, không sao lưu được: " + driveName;
        Logger::error("UploadManager: Empty file, skipping: " + localPath);
        return false;
    }

    // Determine MIME type
    std::string mimeType = "application/octet-stream";
    size_t dotPos = driveName.find_last_of('.');
    if (dotPos != std::string::npos) {
        std::string ext = driveName.substr(dotPos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == "nes" || ext == "fds") mimeType = "application/x-nes";
        else if (ext == "snes" || ext == "smc" || ext == "sfc" || ext == "swc") mimeType = "application/x-snes";
        else if (ext == "gba") mimeType = "application/x-gba";
        else if (ext == "gb" || ext == "gbc") mimeType = "application/x-gameboy";
        else if (ext == "nds") mimeType = "application/x-nds";
        else if (ext == "iso" || ext == "bin" || ext == "img") mimeType = "application/x-iso9660-image";
        else if (ext == "zip" || ext == "7z") mimeType = "application/zip";
    }

    // Step A: Initiate Google Drive Resumable Upload Session.
    // On a 401 the token may have died mid-batch: refresh once and retry.
    std::string initUrl = "https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable&supportsAllDrives=true";
    std::string metadataJson = "{\"name\":\"" + driveName + "\",\"parents\":[\"" + folderId + "\"]}";
    std::string useToken = token;
    std::string sessionUrl;
    std::string initResponse;
    long initHttpCode = 0;
    CURLcode initTransport = CURLE_OK;

    for (int attempt = 0; attempt < 2 && sessionUrl.empty(); ++attempt) {
        std::vector<std::string> initHeaders = {
            "Authorization: Bearer " + useToken,
            "Content-Type: application/json; charset=UTF-8",
            "X-Upload-Content-Type: " + mimeType,
            "X-Upload-Content-Length: " + std::to_string(fileSize)
        };

        CURL* curlInit = curl_easy_init();
        if (!curlInit) {
            m_lastUploadError = "Không khởi tạo được kết nối mạng.";
            Logger::error("UploadManager: curl_easy_init failed");
            return false;
        }

        struct curl_slist* initHeaderList = nullptr;
        for (const auto& h : initHeaders) {
            initHeaderList = curl_slist_append(initHeaderList, h.c_str());
        }

        initResponse.clear();
        std::string initHeaderResponse;
        initHttpCode = 0;

        curl_easy_setopt(curlInit, CURLOPT_URL, initUrl.c_str());
        curl_easy_setopt(curlInit, CURLOPT_POST, 1L);
        curl_easy_setopt(curlInit, CURLOPT_POSTFIELDS, metadataJson.c_str());
        curl_easy_setopt(curlInit, CURLOPT_HTTPHEADER, initHeaderList);
        curl_easy_setopt(curlInit, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curlInit, CURLOPT_WRITEDATA, &initResponse);
        curl_easy_setopt(curlInit, CURLOPT_HEADERFUNCTION, headerCallback);
        curl_easy_setopt(curlInit, CURLOPT_HEADERDATA, &initHeaderResponse);
        curl_easy_setopt(curlInit, CURLOPT_TIMEOUT, 30L);
        // Khớp cấu hình với HttpClient (đang chạy tốt trên máy): bản curl
        // trên TrimUI không có gói CA mặc định nên bật kiểm chứng SSL là
        // rớt kết nối ngay (HTTP 0); ép thêm IPv4 và NOSIGNAL cho worker.
        curl_easy_setopt(curlInit, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curlInit, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curlInit, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curlInit, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);

        CURLcode initRes = curl_easy_perform(curlInit);
        initTransport = initRes;
        curl_easy_getinfo(curlInit, CURLINFO_RESPONSE_CODE, &initHttpCode);
        curl_easy_cleanup(curlInit);
        curl_slist_free_all(initHeaderList);

        if (initRes == CURLE_OK && (initHttpCode == 200 || initHttpCode == 201)) {
            // Extract Location URL from response headers
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
            break;
        }

        if (initHttpCode == 401 && attempt == 0 && AuthManager::instance().refreshAccessTokenNow()) {
            std::string fresh = AuthManager::instance().getValidAccessToken();
            if (!fresh.empty() && fresh != useToken) {
                Logger::info("UploadManager: Token refreshed after 401, retrying upload init");
                useToken = fresh;
                continue;
            }
        }
        break;
    }

    if (sessionUrl.empty()) {
        if (initHttpCode == 401) {
            m_lastErrorIsAuth = true;
            m_lastUploadError = "Google từ chối token đăng nhập (đã hết hạn). Đăng nhập lại rồi sao lưu tiếp.";
        } else if (initHttpCode > 0) {
            m_lastUploadError = "Google từ chối sao lưu " + driveName + " (mã lỗi " + std::to_string(initHttpCode) + ").";
        } else {
            m_lastUploadError = "Không kết nối được Google Drive. Kiểm tra mạng rồi thử lại.";
        }
        Logger::error("UploadManager: Failed to initiate resumable upload session (HTTP " + std::to_string(initHttpCode) + ", curl " + std::to_string((int)initTransport) + " " + curl_easy_strerror(initTransport) + "): " + initResponse);
        return false;
    }

    // Step B: Stream upload the file chunks via PUT
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
    // Xem ghi chú ở bước mở phiên: SSL/IPv4/NOSIGNAL khớp HttpClient.
    curl_easy_setopt(curlUpload, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curlUpload, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curlUpload, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curlUpload, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);

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
        m_lastUploadError = "Mất kết nối khi đang sao lưu " + driveName + ".";
        Logger::error("UploadManager: Stream upload failed: " + std::string(curl_easy_strerror(uploadRes)));
        return false;
    }

    if (uploadHttpCode == 200 || uploadHttpCode == 201) {
        m_lastUploadedFileId = JsonHelper::extractString(uploadResponse, "id");
        Logger::info("UploadManager: Successfully uploaded " + driveName + " (Drive ID: " + m_lastUploadedFileId + ")");
        return true;
    }

    if (uploadHttpCode == 401) {
        m_lastErrorIsAuth = true;
        m_lastUploadError = "Google từ chối token đăng nhập (đã hết hạn). Đăng nhập lại rồi sao lưu tiếp.";
    } else {
        m_lastUploadError = "Google từ chối sao lưu " + driveName + " (mã lỗi " + std::to_string(uploadHttpCode) + ").";
    }
    Logger::error("UploadManager: Upload failed with HTTP " + std::to_string(uploadHttpCode) + ": " + uploadResponse.substr(0, 200));
    return false;
}

bool UploadManager::uploadSingleGame(const LocalGameInfo& game) {
    m_lastUploadError.clear();
    m_lastErrorIsAuth = false;

    // 1. Get OAuth token
    std::string accessToken = AuthManager::instance().getValidAccessToken();
    if (accessToken.empty()) {
        m_lastUploadError = "Phiên đăng nhập Google đã hết hạn. Đăng nhập lại rồi sao lưu tiếp.";
        m_lastErrorIsAuth = true;
        Logger::error("UploadManager: No personal access token available");
        return false;
    }

    // 2. Resolve destination folder on Google Drive (Roms/<system>)
    std::string folderId = getTargetFolderForGame(game, accessToken);
    if (folderId.empty()) {
        m_lastUploadError = "Không tạo được thư mục đích trên Drive cho hệ " + game.systemCode + ". Kiểm tra mạng rồi thử lại.";
        Logger::error("UploadManager: No Drive folder available for system " + game.systemCode);
        return false;
    }

    // 3. Local path must still exist on the SD card
    struct stat pathStat;
    if (::stat(game.localPath.c_str(), &pathStat) != 0) {
        m_lastUploadError = "Không thấy game trên thẻ nhớ: " + game.filename;
        Logger::error("UploadManager: Local path missing: " + game.localPath);
        return false;
    }

    bool success = false;
    std::string uploadedId;

    if (S_ISDIR(pathStat.st_mode)) {
        // Folder game (e.g. PS1 bin/cue in its own folder): mirror it into
        // a same-name folder on Drive and push every file inside.
        std::string subFolderId = findOrCreateDriveFolder(game.filename, folderId, accessToken);
        if (subFolderId.empty()) {
            m_lastUploadError = "Không tạo được thư mục game trên Drive: " + game.filename;
            Logger::error("UploadManager: Cannot create Drive folder for game folder " + game.filename);
            return false;
        }

        DIR* dir = opendir(game.localPath.c_str());
        if (!dir) {
            m_lastUploadError = "Không mở được thư mục game trên thẻ: " + game.filename;
            return false;
        }

        int filesDone = 0;
        bool allOk = true;
        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') continue;
            std::string childPath = game.localPath + "/" + entry->d_name;
            struct stat childStat;
            if (::stat(childPath.c_str(), &childStat) != 0 || !S_ISREG(childStat.st_mode)) continue;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_progress.filename = entry->d_name;
            }
            if (uploadOneFile(childPath, entry->d_name, subFolderId, accessToken)) {
                filesDone++;
            } else {
                allOk = false;
                if (m_lastErrorIsAuth || m_cancelRequested) break;
            }
        }
        closedir(dir);

        if (filesDone == 0) {
            if (m_lastUploadError.empty()) {
                m_lastUploadError = "Thư mục game không có file nào để sao lưu: " + game.filename;
            }
            return false;
        }
        success = allOk;
        uploadedId = subFolderId;
    } else {
        success = uploadOneFile(game.localPath, game.filename, folderId, accessToken);
        uploadedId = m_lastUploadedFileId;
    }

    if (success && !uploadedId.empty()) {
        GameRecord gameRec;
        if (DatabaseManager::instance().getGameById(game.gameId, gameRec)) {
            gameRec.cloudFileId = uploadedId;
            DatabaseManager::instance().upsertGame(gameRec);
        }
    }
    return success;
}

UploadManager::~UploadManager() {
    shutdown();
}

} // namespace RomCloud
