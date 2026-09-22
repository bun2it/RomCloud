#include "UploadManager.h"
#include "../logging/Logger.h"
#include "../config/AppConfig.h"
#include "../filesystem/FileSystemManager.h"
#include "../auth/AuthManager.h"
#include "../network/HttpClient.h"

#include <fstream>
#include <sstream>
#include <curl/curl.h>

namespace RomCloud {

static size_t uploadReadCallback(void* ptr, size_t size, size_t nmemb, void* stream) {
    std::ifstream* file = static_cast<std::ifstream*>(stream);
    if (!file || file->eof()) return 0;
    file->read(static_cast<char*>(ptr), size * nmemb);
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
            info.needsUpload = game.cloudFileId.empty();

            if (info.needsUpload) {
                // Check if file actually exists locally
                if (!info.localPath.empty() && FileSystemManager::instance().fileExists(info.localPath)) {
                    localOnly.push_back(info);
                }
            }
        }
    }

    Logger::info("UploadManager: Found " + std::to_string(localOnly.size()) + " local games not on cloud");
    return localOnly;
}

std::string UploadManager::findDriveFolderForSystem(const std::string& systemCode) {
    // Try to find the folder ID from database settings
    // In a full implementation, this would query Drive API for matching folder
    std::string folderId = DatabaseManager::instance().getSetting("drive_folder_id", "");
    return folderId;
}

void UploadManager::startReverseSync() {
    if (m_isRunning) {
        Logger::warn("Upload already in progress");
        return;
    }

    if (!AuthManager::instance().isLinked()) {
        Logger::error("Cannot upload: not linked to Google Drive");
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = UploadState::FAILED;
        m_progress.errorMessage = "Not linked to Google Drive. Connect in Settings first.";
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
    if (m_progress.state == UploadState::UPLOADING) {
        m_progress.state = UploadState::COMPLETED;
        m_isRunning = false;

        std::string msg = "Upload complete: " + std::to_string(m_progress.gamesUploaded) +
                         " succeeded, " + std::to_string(m_progress.gamesFailed) + " failed";
        Logger::info("UploadManager: " + msg);

        if (onComplete) {
            onComplete(m_progress.gamesFailed == 0, msg);
        }
    }
}

bool UploadManager::uploadSingleGame(const LocalGameInfo& game) {
    // Get OAuth token
    std::string accessToken = AuthManager::instance().getValidAccessToken();
    if (accessToken.empty()) {
        Logger::error("UploadManager: No access token available");
        return false;
    }

    // Get folder ID
    std::string folderId = findDriveFolderForSystem(game.systemCode);
    if (folderId.empty()) {
        Logger::error("UploadManager: No Drive folder ID configured");
        return false;
    }

    // Open the file
    std::ifstream file(game.localPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        Logger::error("UploadManager: Cannot open file: " + game.localPath);
        return false;
    }

    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    // Determine MIME type based on extension
    std::string mimeType = "application/octet-stream";
    size_t dotPos = game.filename.find_last_of('.');
    if (dotPos != std::string::npos) {
        std::string ext = game.filename.substr(dotPos + 1);
        if (ext == "nes") mimeType = "application/x-nes";
        else if (ext == "snes" || ext == "smc") mimeType = "application/x-snes";
        else if (ext == "gba") mimeType = "application/x-gba";
        else if (ext == "nds") mimeType = "application/x-nds";
        else if (ext == "iso" || ext == "bin") mimeType = "application/x-iso9660-image";
        else if (ext == "zip" || ext == "7z") mimeType = "application/zip";
        else if (ext == "nes" || ext == "fds") mimeType = "application/x-fds";
        else if (ext == "sfc" || ext == "swc") mimeType = "application/x-snes";
    }

    // Build multipart metadata
    std::string metadata = "{"
        "\"name\":\"" + game.filename + "\","
        "\"parents\":[\"" + folderId + "\"]"
        "}";

    // Build multipart boundary
    std::string boundary = "boundary_" + std::to_string(time(nullptr));
    std::string contentType = "multipart/related; boundary=" + boundary;

    // Calculate total size for progress
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress.totalBytes = fileSize;
    m_progress.bytesUploaded = 0;

    // Simple upload: use Google Drive API multipart upload
    CURL* curl = curl_easy_init();
    if (!curl) {
        Logger::error("UploadManager: curl_easy_init failed");
        return false;
    }

    std::string response;
    std::string headerResponse;
    long httpCode = 0;

    std::string uploadUrl = "https://www.googleapis.com/upload/drive/v3/files?uploadType=multipart";

    // Set up headers
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + accessToken).c_str());
    headers = curl_slist_append(headers, ("Content-Type: " + contentType).c_str());

    // Build multipart body manually
    std::string body;
    body += "--" + boundary + "\r\n";
    body += "Content-Type: application/json\r\n\r\n";
    body += metadata + "\r\n";
    body += "--" + boundary + "\r\n";
    body += "Content-Type: " + mimeType + "\r\n\r\n";

    size_t bodySize = body.size();
    curl_off_t totalSize = bodySize + fileSize + boundary.size() + 6; // closing boundary

    curl_easy_setopt(curl, CURLOPT_URL, uploadUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_PIPEWAIT, 1L);

    // For simplicity, use a simpler approach: read file to memory and upload
    // Note: For large files, this should use chunked upload
    file.close();
    std::ifstream fileRead(game.localPath, std::ios::binary);
    std::string fileData((std::istreambuf_iterator<char>(fileRead)), std::istreambuf_iterator<char>());
    fileRead.close();

    body += fileData;
    body += "\r\n--" + boundary + "--\r\n";

    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headerResponse);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        Logger::error("UploadManager: curl failed: " + std::string(curl_easy_strerror(res)));
        return false;
    }

    if (httpCode >= 200 && httpCode < 300) {
        Logger::info("UploadManager: Successfully uploaded " + game.filename);

        // Parse response to get file ID
        std::string fileId = "";
        size_t idPos = response.find("\"id\":\"");
        if (idPos != std::string::npos) {
            idPos += 6;
            size_t idEnd = response.find("\"", idPos);
            if (idEnd != std::string::npos) {
                fileId = response.substr(idPos, idEnd - idPos);
            }
        }

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
        Logger::error("UploadManager: Upload failed with HTTP " + std::to_string(httpCode) + ": " + response.substr(0, 200));
        return false;
    }
}

UploadManager::~UploadManager() {
    shutdown();
}

} // namespace RomCloud
