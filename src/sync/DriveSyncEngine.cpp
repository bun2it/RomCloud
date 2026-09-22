#include "DriveSyncEngine.h"
#include "../network/HttpClient.h"
#include "../network/JsonHelper.h"
#include "../auth/AuthManager.h"
#include "../logging/Logger.h"
#include <algorithm>
#include <chrono>

namespace RomCloud {

static const char* DRIVE_FILES_ENDPOINT = "https://www.googleapis.com/drive/v3/files";

DriveSyncEngine& DriveSyncEngine::instance() {
    static DriveSyncEngine instance;
    return instance;
}

DriveSyncEngine::~DriveSyncEngine() {
    shutdown();
}

bool DriveSyncEngine::init() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress.status = SyncStatus::IDLE;
    Logger::info("DriveSyncEngine initialized.");
    return true;
}

void DriveSyncEngine::shutdown() {
    cancelSync();
}

bool DriveSyncEngine::isSyncing() const {
    return m_isRunning;
}

SyncProgress DriveSyncEngine::getProgress() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_progress;
}

std::string DriveSyncEngine::getLastSyncTime() const {
    return DatabaseManager::instance().getSetting("last_cloud_sync_time", "Never");
}

void DriveSyncEngine::cancelSync() {
    m_cancelRequested = true;
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    m_isRunning = false;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_progress.status != SyncStatus::COMPLETED) {
        m_progress.status = SyncStatus::IDLE;
    }
}

bool DriveSyncEngine::startSync() {
    if (m_isRunning) {
        Logger::warn("Sync already in progress, ignoring start request.");
        return false;
    }

    if (!AuthManager::instance().isLinked()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.status = SyncStatus::ERROR_OCCURRED;
        m_progress.errorMessage = "Not connected to Google Drive. Please link account in Settings.";
        Logger::warn(m_progress.errorMessage);
        return false;
    }

    m_cancelRequested = false;
    m_isRunning = true;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress = SyncProgress();
        m_progress.status = SyncStatus::CONNECTING;
    }

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    m_workerThread = std::thread(&DriveSyncEngine::runSyncWorker, this);
    return true;
}

std::string DriveSyncEngine::normalizeTitle(const std::string& filename) {
    std::string base = filename;
    size_t lastDot = base.find_last_of('.');
    if (lastDot != std::string::npos) {
        base = base.substr(0, lastDot);
    }

    // Strip dump tags like (USA), [!], (v1.0), etc.
    std::string cleaned;
    bool inParen = false, inBracket = false;
    for (char c : base) {
        if (c == '(') inParen = true;
        else if (c == ')') { inParen = false; continue; }
        else if (c == '[') inBracket = true;
        else if (c == ']') { inBracket = false; continue; }
        else if (!inParen && !inBracket) {
            cleaned += c;
        }
    }

    // Trim trailing whitespace and dashes
    while (!cleaned.empty() && (cleaned.back() == ' ' || cleaned.back() == '-' || cleaned.back() == '_')) {
        cleaned.pop_back();
    }
    while (!cleaned.empty() && (cleaned.front() == ' ' || cleaned.front() == '-' || cleaned.front() == '_')) {
        cleaned.erase(0, 1);
    }

    return cleaned.empty() ? filename : cleaned;
}

std::string DriveSyncEngine::findRomCloudRootFolder(const std::string& token) {
    std::string query = "mimeType = 'application/vnd.google-apps.folder' and (name = 'RomCloud' or name = 'Roms') and trashed = false";
    std::string url = std::string(DRIVE_FILES_ENDPOINT) + "?q=" + HttpClient::instance().urlEncode(query) + "&fields=files(id,name)&pageSize=10";

    std::vector<std::string> headers = {
        "Authorization: Bearer " + token
    };

    HttpResponse resp = HttpClient::instance().get(url, headers);
    if (resp.success) {
        auto files = JsonHelper::extractArrayObjects(resp.body, "files");
        for (const auto& fileJson : files) {
            std::string id = JsonHelper::extractString(fileJson, "id");
            std::string name = JsonHelper::extractString(fileJson, "name");
            if (!id.empty()) {
                Logger::info("Found Google Drive Master ROM folder: '" + name + "' (ID: " + id + ")");
                return id;
            }
        }
    }
    Logger::info("No 'RomCloud' or 'Roms' dedicated root folder found. Scanning all Drive folders.");
    return "";
}

std::unordered_map<std::string, std::string> DriveSyncEngine::discoverSystemFolders(const std::string& rootFolderId, const std::string& token) {
    std::unordered_map<std::string, std::string> systemFolderMap;

    std::string pageToken;
    do {
        if (m_cancelRequested) break;

        std::string query;
        if (!rootFolderId.empty()) {
            query = "'" + rootFolderId + "' in parents and mimeType = 'application/vnd.google-apps.folder' and trashed = false";
        } else {
            query = "mimeType = 'application/vnd.google-apps.folder' and trashed = false";
        }

        std::string url = std::string(DRIVE_FILES_ENDPOINT) + "?q=" + HttpClient::instance().urlEncode(query) +
                          "&fields=nextPageToken,files(id,name)&pageSize=1000&supportsAllDrives=true&includeItemsFromAllDrives=true";
        if (!pageToken.empty()) {
            url += "&pageToken=" + HttpClient::instance().urlEncode(pageToken);
        }

        std::vector<std::string> headers = {
            "Authorization: Bearer " + token
        };

        HttpResponse resp = HttpClient::instance().get(url, headers);
        if (!resp.success) break;

        auto folders = JsonHelper::extractArrayObjects(resp.body, "files");
        for (const auto& fJson : folders) {
            std::string id = JsonHelper::extractString(fJson, "id");
            std::string name = JsonHelper::extractString(fJson, "name");

            // Convert name to uppercase for matching
            std::string upperName = name;
            std::transform(upperName.begin(), upperName.end(), upperName.begin(), ::toupper);

            if (!id.empty() && !upperName.empty()) {
                systemFolderMap[upperName] = id;
            }
        }
        pageToken = JsonHelper::extractString(resp.body, "nextPageToken");
    } while (!pageToken.empty() && !m_cancelRequested);

    return systemFolderMap;
}

int DriveSyncEngine::syncFilesForSystem(const SystemRecord& system, const std::string& folderId, const std::string& token) {
    int count = 0;
    std::string pageToken = "";

    // Parse supported extensions
    std::vector<std::string> extList;
    std::stringstream ss(system.extList);
    std::string ext;
    while (std::getline(ss, ext, ',')) {
        while (!ext.empty() && ext.front() == ' ') ext.erase(0, 1);
        while (!ext.empty() && ext.back() == ' ') ext.pop_back();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (!ext.empty()) extList.push_back(ext);
    }

    do {
        if (m_cancelRequested) break;

        std::string query = "'" + folderId + "' in parents and mimeType != 'application/vnd.google-apps.folder' and trashed = false";
        std::string url = std::string(DRIVE_FILES_ENDPOINT) + "?q=" + HttpClient::instance().urlEncode(query) +
                          "&fields=nextPageToken,files(id,name,size,md5Checksum,modifiedTime,mimeType)&pageSize=1000&supportsAllDrives=true&includeItemsFromAllDrives=true";
        if (!pageToken.empty()) {
            url += "&pageToken=" + HttpClient::instance().urlEncode(pageToken);
        }

        std::vector<std::string> headers = {
            "Authorization: Bearer " + token
        };

        HttpResponse resp = HttpClient::instance().get(url, headers);
        if (!resp.success) {
            Logger::error("Failed to fetch files for system " + system.code + ": " + resp.error);
            break;
        }

        auto fileObjects = JsonHelper::extractArrayObjects(resp.body, "files");
        for (const auto& fileJson : fileObjects) {
            if (m_cancelRequested) break;

            std::string fileId = JsonHelper::extractString(fileJson, "id");
            std::string filename = JsonHelper::extractString(fileJson, "name");
            uint64_t sizeBytes = JsonHelper::extractUInt64(fileJson, "size", 0);
            std::string md5 = JsonHelper::extractString(fileJson, "md5Checksum");
            std::string modTime = JsonHelper::extractString(fileJson, "modifiedTime");
            std::string mimeType = JsonHelper::extractString(fileJson, "mimeType");

            // Extension check
            size_t dotPos = filename.find_last_of('.');
            if (dotPos == std::string::npos) continue;
            std::string fileExt = filename.substr(dotPos + 1);
            std::transform(fileExt.begin(), fileExt.end(), fileExt.begin(), ::tolower);

            bool extMatch = false;
            for (const auto& validExt : extList) {
                if (fileExt == validExt) {
                    extMatch = true;
                    break;
                }
            }
            if (!extMatch && !extList.empty()) continue;

            // Check if game already indexed locally
            GameRecord existing;
            bool foundExisting = DatabaseManager::instance().getGameByFilename(system.id, filename, existing);

            if (foundExisting) {
                // Game already present (either local or cloud). Update cloud attributes while preserving local state!
                existing.cloudFileId = fileId;
                existing.driveModifiedTime = modTime;
                if (!md5.empty()) existing.checksumSha256 = md5;
                if (existing.sizeBytes == 0) existing.sizeBytes = sizeBytes;

                DatabaseManager::instance().upsertGame(existing);
                std::lock_guard<std::mutex> lock(m_mutex);
                m_progress.updatedGames++;
                m_progress.cloudGamesFound++;
            } else {
                // New game discovered on cloud!
                GameRecord newGame;
                newGame.cloudFileId = fileId;
                newGame.systemId = system.id;
                newGame.filename = filename;
                newGame.title = normalizeTitle(filename);
                newGame.sizeBytes = sizeBytes;
                newGame.mimeType = mimeType;
                newGame.driveModifiedTime = modTime;
                newGame.checksumSha256 = md5;
                newGame.localState = GameState::CLOUD; // Stored in cloud only

                DatabaseManager::instance().upsertGame(newGame);
                std::lock_guard<std::mutex> lock(m_mutex);
                m_progress.newGamesIndexed++;
                m_progress.cloudGamesFound++;
            }
            count++;
        }

        pageToken = JsonHelper::extractString(resp.body, "nextPageToken");
    } while (!pageToken.empty() && !m_cancelRequested);

    return count;
}

static std::string decodeDriveHexEscapes(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 3 < in.size() && in[i+1] == 'x') {
            int hexVal = 0;
            std::stringstream ss;
            ss << std::hex << in.substr(i + 2, 2);
            if (ss >> hexVal) {
                out += static_cast<char>(hexVal);
                i += 3;
                continue;
            }
        } else if (in[i] == '\\' && i + 1 < in.size() && in[i+1] == '/') {
            out += '/';
            i += 1;
            continue;
        } else if (in[i] == '\\' && i + 1 < in.size() && in[i+1] == '\\') {
            out += '\\';
            i += 1;
            continue;
        }
        out += in[i];
    }
    return out;
}

struct PublicDriveItem {
    std::string id;
    std::string name;
    std::string mime;
    uint64_t sizeBytes = 0;
    bool isFolder = false;
};

static std::string decodeHtmlEntities(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '&') {
            if (in.compare(i, 5, "&amp;") == 0) { out += '&'; i += 4; }
            else if (in.compare(i, 6, "&quot;") == 0) { out += '"'; i += 5; }
            else if (in.compare(i, 6, "&apos;") == 0) { out += '\''; i += 5; }
            else if (in.compare(i, 5, "&#39;") == 0) { out += '\''; i += 4; }
            else if (in.compare(i, 4, "&lt;") == 0) { out += '<'; i += 3; }
            else if (in.compare(i, 4, "&gt;") == 0) { out += '>'; i += 3; }
            else { out += in[i]; }
        } else {
            out += in[i];
        }
    }
    return out;
}

static std::vector<PublicDriveItem> fetchPublicFolderViaEmbeddedView(const std::string& folderId) {
    std::vector<PublicDriveItem> result;
    std::string url = "https://drive.google.com/embeddedfolderview?id=" + folderId;
    std::vector<std::string> headers = {
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
    };

    HttpResponse resp;
    for (int retry = 0; retry < 3; ++retry) {
        resp = HttpClient::instance().get(url, headers, 45);
        if (resp.success && !resp.body.empty()) break;
        Logger::warn("Embeddedfolderview attempt " + std::to_string(retry + 1) + " failed for folder " + folderId + ": " + resp.error + ", retrying...");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (!resp.success || resp.body.empty()) {
        Logger::warn("Failed to fetch embeddedfolderview for: " + folderId + " error: " + resp.error);
        return result;
    }

    const std::string entryMarker = "class=\"flip-entry\"";
    size_t pos = 0;
    while ((pos = resp.body.find(entryMarker, pos)) != std::string::npos) {
        size_t nextPos = resp.body.find(entryMarker, pos + entryMarker.size());
        size_t entryEnd = (nextPos != std::string::npos) ? nextPos : resp.body.size();
        std::string block = resp.body.substr(pos, entryEnd - pos);
        pos = nextPos;

        // Extract ID: id="entry-XXXX"
        std::string fileId;
        size_t idPos = block.find("id=\"entry-");
        if (idPos != std::string::npos) {
            size_t idStart = idPos + 10;
            size_t idEnd = block.find('\"', idStart);
            if (idEnd != std::string::npos) {
                fileId = block.substr(idStart, idEnd - idStart);
            }
        }

        // Extract Title: <div class="flip-entry-title">TITLE</div>
        std::string title;
        size_t titleMarker = block.find("class=\"flip-entry-title\"");
        if (titleMarker != std::string::npos) {
            size_t openTagEnd = block.find('>', titleMarker);
            if (openTagEnd != std::string::npos) {
                size_t closeTag = block.find('<', openTagEnd + 1);
                if (closeTag != std::string::npos) {
                    title = block.substr(openTagEnd + 1, closeTag - openTagEnd - 1);
                    title = decodeHtmlEntities(title);
                }
            }
        }

        if (fileId.empty() || title.empty()) {
            if (nextPos == std::string::npos) break;
            continue;
        }

        // Folders have href="/drive/folders/..." or class contains "Folder"
        bool isFolder = (block.find("/drive/folders/") != std::string::npos) ||
                        (block.find("Folder") != std::string::npos && block.find("drive-sprite-folder") != std::string::npos);

        PublicDriveItem item;
        item.id = fileId;
        item.name = title;
        item.isFolder = isFolder;
        item.mime = isFolder ? "application/vnd.google-apps.folder" : "application/octet-stream";
        item.sizeBytes = 0;
        result.push_back(item);

        if (nextPos == std::string::npos) break;
    }

    if (!result.empty()) {
        Logger::info("Embedded folderview successfully extracted " + std::to_string(result.size()) + " items for folder " + folderId);
    }
    return result;
}

// Scrape public Google Drive folder contents without OAuth or API key restrictions
static std::vector<PublicDriveItem> fetchPublicFolderViaHtml(const std::string& folderId) {
    // 1. Try embedded folderview first (retrieves full listing without 50-item viewport limits)
    auto embeddedItems = fetchPublicFolderViaEmbeddedView(folderId);
    if (!embeddedItems.empty()) {
        return embeddedItems;
    }

    // 2. Fallback to legacy _DRIVE_ivd parsing
    std::vector<PublicDriveItem> result;
    std::string url = "https://drive.google.com/drive/folders/" + folderId;
    std::vector<std::string> headers = {
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
    };

    HttpResponse resp = HttpClient::instance().get(url, headers);
    if (!resp.success || resp.body.empty()) {
        Logger::error("Failed to fetch Google Drive folder: " + folderId + " error: " + resp.error);
        return result;
    }

    size_t ivdPos = resp.body.find("window['_DRIVE_ivd']");
    if (ivdPos == std::string::npos) {
        Logger::warn("Could not find _DRIVE_ivd in Google Drive response for: " + folderId);
        return result;
    }

    size_t eqPos = resp.body.find('=', ivdPos);
    if (eqPos == std::string::npos) return result;

    size_t quoteStart = resp.body.find('\'', eqPos);
    if (quoteStart == std::string::npos) return result;
    size_t quoteEnd = resp.body.find('\'', quoteStart + 1);
    if (quoteEnd == std::string::npos) return result;

    std::string rawEscaped = resp.body.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
    std::string decoded = decodeDriveHexEscapes(rawEscaped);

    size_t pos = 0;
    while (pos < decoded.size()) {
        size_t itemStart = decoded.find("[\"", pos);
        if (itemStart == std::string::npos) break;

        size_t idStart = itemStart + 2;
        size_t idEnd = decoded.find('\"', idStart);
        if (idEnd == std::string::npos) break;
        std::string fileId = decoded.substr(idStart, idEnd - idStart);

        size_t afterId = decoded.find(']', idEnd);
        if (afterId == std::string::npos) break;

        size_t nameQuote1 = decoded.find('\"', afterId);
        if (nameQuote1 == std::string::npos) break;
        size_t nameQuote2 = decoded.find('\"', nameQuote1 + 1);
        if (nameQuote2 == std::string::npos) break;
        std::string name = decoded.substr(nameQuote1 + 1, nameQuote2 - nameQuote1 - 1);

        size_t mimeQuote1 = decoded.find('\"', nameQuote2 + 1);
        if (mimeQuote1 == std::string::npos) break;
        size_t mimeQuote2 = decoded.find('\"', mimeQuote1 + 1);
        if (mimeQuote2 == std::string::npos) break;
        std::string mime = decoded.substr(mimeQuote1 + 1, mimeQuote2 - mimeQuote1 - 1);

        PublicDriveItem item;
        item.id = fileId;
        item.name = name;
        item.mime = mime;
        item.isFolder = (mime.find("folder") != std::string::npos);
        result.push_back(item);

        pos = mimeQuote2 + 1;
    }

    return result;
}

static std::pair<std::vector<PublicDriveItem>, std::string> fetchFolderViaApi(
        const std::string& folderId, const std::string& apiKey, const std::string& token, const std::string& pageToken) {
    std::vector<PublicDriveItem> result;
    if (apiKey.empty() && token.empty()) return {result, ""};

    std::string url = "https://www.googleapis.com/drive/v3/files"
        "?q=%27" + folderId + "%27+in+parents+and+trashed%3Dfalse"
        "&fields=nextPageToken%2Cfiles(id%2Cname%2CmimeType%2Csize)"
        "&pageSize=1000"
        "&supportsAllDrives=true&includeItemsFromAllDrives=true";
    if (!apiKey.empty()) {
        url += "&key=" + apiKey;
    }
    if (!pageToken.empty()) {
        url += "&pageToken=" + pageToken;
    }

    std::vector<std::string> headers = {
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
    };
    if (!token.empty()) {
        headers.push_back("Authorization: Bearer " + token);
    }

    HttpResponse resp = HttpClient::instance().get(url, headers);
    if (!resp.success || resp.body.empty()) {
        Logger::warn("fetchFolderViaApi HTTP request failed: " + resp.error);
        return {result, ""};
    }
    if (resp.body.find("\"error\"") != std::string::npos &&
        resp.body.find("\"files\"") == std::string::npos) {
        Logger::warn("Google Drive API error for folder " + folderId + ": " + resp.body);
        return {result, ""};
    }

    auto fileObjects = JsonHelper::extractArrayObjects(resp.body, "files");
    for (const auto& fileJson : fileObjects) {
        std::string id = JsonHelper::extractString(fileJson, "id");
        std::string name = JsonHelper::extractString(fileJson, "name");
        std::string mime = JsonHelper::extractString(fileJson, "mimeType");
        uint64_t sz = JsonHelper::extractUInt64(fileJson, "size", 0);
        if (id.empty() || name.empty()) continue;
        PublicDriveItem item;
        item.id = id; item.name = name; item.mime = mime;
        item.sizeBytes = sz;
        item.isFolder = (mime.find("folder") != std::string::npos);
        result.push_back(item);
    }

    std::string nextToken = JsonHelper::extractString(resp.body, "nextPageToken");
    return {result, nextToken};
}

std::string DriveSyncEngine::matchFolderToSystemCode(const std::string& name) {
    std::string s = name;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    if (s.find("game boy advance") != std::string::npos || s.find("gba") != std::string::npos) return "GBA";
    if (s.find("game boy color") != std::string::npos || s.find("gbc") != std::string::npos) return "GBC";
    if (s.find("game boy") != std::string::npos || s == "gb") return "GB";
    if (s.find("super nintendo") != std::string::npos || s.find("snes") != std::string::npos || s.find("sfc") != std::string::npos) return "SFC";
    if (s.find("genesis") != std::string::npos || s.find("mega drive") != std::string::npos || s.find("megadrive") != std::string::npos || s == "md") return "MD";
    if (s.find("nintendo 64") != std::string::npos || s.find("n64") != std::string::npos) return "N64";
    if (s.find("nintendo ds") != std::string::npos || s.find("nds") != std::string::npos || s.find(" ds") != std::string::npos) return "NDS";
    if (s.find("playstation") != std::string::npos || s.find("ps1") != std::string::npos || s.find("psx") != std::string::npos) return "PS";
    if (s.find("psp") != std::string::npos) return "PSP";
    if (s.find("dreamcast") != std::string::npos || s.find("dc") != std::string::npos) return "DC";
    if (s.find("saturn") != std::string::npos) return "SS";
    if (s.find("game gear") != std::string::npos || s.find("gamegear") != std::string::npos || s == "gg") return "GG";
    if (s.find("master system") != std::string::npos || s == "ms" || s == "sms") return "MS";
    if (s.find("sega cd") != std::string::npos || s.find("segacd") != std::string::npos) return "SEGACD";
    if (s.find("neo geo") != std::string::npos || s.find("neogeo") != std::string::npos) return "NEOGEO";
    if (s.find("mame") != std::string::npos || s.find("arcade") != std::string::npos || s.find("fbneo") != std::string::npos) return "ARCADE";
    if (s.find("turbografx") != std::string::npos || s.find("pc engine") != std::string::npos || s.find("pce") != std::string::npos) return "PCE";
    if (s.find("2600") != std::string::npos || s.find("atari") != std::string::npos) return "ATARI2600";
    if ((s.find("famicom") != std::string::npos || s.find("nes") != std::string::npos || s == "fc") && s.find("genesis") == std::string::npos) return "FC";
    return "";
}

void DriveSyncEngine::syncPublicFolder(const std::string& rootFolderId) {
    Logger::info("Syncing public Google Drive master folder: " + rootFolderId);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.status = SyncStatus::DISCOVERING_FOLDERS;
    }

    auto dbSystems = DatabaseManager::instance().getSystems(false);
    std::unordered_map<std::string, SystemRecord> codeToSystem;
    for (const auto& s : dbSystems) {
        codeToSystem[s.code] = s;
    }

    std::string token = AuthManager::instance().getValidAccessToken();
    std::string apiKey = DatabaseManager::instance().getSetting("google_api_key", "");

    // Fetch items from root folder (try OAuth API only if token available, else embeddedfolderview parser)
    std::vector<PublicDriveItem> rootItems;
    if (!token.empty()) {
        std::string rootPageToken;
        do {
            auto pageResult = fetchFolderViaApi(rootFolderId, apiKey, token, rootPageToken);
            for (const auto& item : pageResult.first) rootItems.push_back(item);
            rootPageToken = pageResult.second;
        } while (!rootPageToken.empty() && !m_cancelRequested);
    }

    if (rootItems.empty()) {
        Logger::info("Using embedded folderview / HTML parser to discover public folder structure...");
        rootItems = fetchPublicFolderViaHtml(rootFolderId);
    }

    Logger::info("Fetched " + std::to_string(rootItems.size()) + " items in root Drive folder.");

    // Categorize: system subfolders vs direct files
    std::vector<std::pair<SystemRecord, PublicDriveItem>> subfolderJobs;
    std::vector<std::pair<SystemRecord, PublicDriveItem>> directFileJobs;

    for (const auto& it : rootItems) {
        std::string code = matchFolderToSystemCode(it.name);
        if (code.empty()) continue;
        auto sysIt = codeToSystem.find(code);
        if (sysIt == codeToSystem.end()) continue;
        if (it.isFolder) {
            subfolderJobs.push_back({sysIt->second, it});
        } else {
            directFileJobs.push_back({sysIt->second, it});
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.status = SyncStatus::SYNCING_FILES;
        m_progress.totalSystems = static_cast<int>(subfolderJobs.size() + (directFileJobs.empty() ? 0 : 1));
        m_progress.currentSystemIndex = 0;
    }

    DatabaseManager::instance().beginTransaction();

    // 1. Direct files in root (e.g. system full-set zip files)
    for (const auto& job : directFileJobs) {
        if (m_cancelRequested) break;
        if (job.second.name.empty() || job.second.name[0] == '.') continue;
        GameRecord g;
        g.systemId = job.first.id;
        g.filename = job.second.name;
        g.title = normalizeTitle(job.second.name);
        g.sizeBytes = job.second.sizeBytes;
        g.cloudFileId = job.second.id;
        g.localState = GameState::CLOUD;
        DatabaseManager::instance().upsertGame(g);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.cloudGamesFound++;
        m_progress.newGamesIndexed++;
    }

    // 2. Process each system subfolder
    int jobIdx = 0;
    for (const auto& job : subfolderJobs) {
        if (m_cancelRequested) break;
        jobIdx++;
        const SystemRecord& sys = job.first;
        const PublicDriveItem& folder = job.second;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_progress.currentPlatform = folder.name + " (" + sys.code + ")";
            m_progress.currentSystemIndex = jobIdx;
        }
        Logger::info("Syncing subfolder: " + folder.name + " (" + sys.code + ")...");

        std::vector<PublicDriveItem> allSystemItems;
        if (!token.empty()) {
            std::string pageToken;
            do {
                auto pageResult = fetchFolderViaApi(folder.id, apiKey, token, pageToken);
                for (const auto& it : pageResult.first) {
                    if (!it.isFolder) allSystemItems.push_back(it);
                }
                pageToken = pageResult.second;
            } while (!pageToken.empty() && !m_cancelRequested);
        }

        if (allSystemItems.empty()) {
            auto subItems = fetchPublicFolderViaHtml(folder.id);
            for (const auto& it : subItems) {
                if (!it.isFolder) allSystemItems.push_back(it);
            }
        }

        Logger::info("  Found " + std::to_string(allSystemItems.size()) + " files for " + sys.code);

        for (const auto& file : allSystemItems) {
            if (m_cancelRequested) break;
            if (file.name.empty() || file.name[0] == '.') continue;
            GameRecord g;
            g.systemId = sys.id;
            g.filename = file.name;
            g.title = normalizeTitle(file.name);
            g.sizeBytes = file.sizeBytes;
            g.cloudFileId = file.id;
            g.localState = GameState::CLOUD;
            DatabaseManager::instance().upsertGame(g);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_progress.cloudGamesFound++;
            m_progress.newGamesIndexed++;
        }
    }

    DatabaseManager::instance().commitTransaction();

    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M");
    DatabaseManager::instance().setSetting("last_cloud_sync_time", ss.str());

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_cancelRequested) {
            m_progress.status = SyncStatus::IDLE;
            Logger::info("Public folder sync cancelled by user.");
        } else {
            m_progress.status = SyncStatus::COMPLETED;
            Logger::info("Public folder sync complete! Total cloud games indexed: " + std::to_string(m_progress.cloudGamesFound));
        }
    }

    m_isRunning = false;
}




void DriveSyncEngine::runSyncWorker() {
    Logger::info("Google Drive Library Sync started...");

    std::string token = AuthManager::instance().getValidAccessToken();
    std::string publicFolderId = DatabaseManager::instance().getSetting("drive_folder_id", "");
    if (publicFolderId.empty()) {
        std::string url = DatabaseManager::instance().getSetting("drive_folder_url", "");
        size_t fPos = url.find("folders/");
        if (fPos != std::string::npos) {
            publicFolderId = url.substr(fPos + 8);
            size_t endPos = publicFolderId.find_first_of("?/#& ");
            if (endPos != std::string::npos) publicFolderId = publicFolderId.substr(0, endPos);
        }
    }

    if (!publicFolderId.empty()) {
        syncPublicFolder(publicFolderId);
        return;
    }

    if (token.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.status = SyncStatus::ERROR_OCCURRED;
        m_progress.errorMessage = "Chua ket noi Google Drive.";
        Logger::error(m_progress.errorMessage);
        m_isRunning = false;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.status = SyncStatus::DISCOVERING_FOLDERS;
    }

    std::string rootFolderId = findRomCloudRootFolder(token);
    auto folderMap = discoverSystemFolders(rootFolderId, token);
    Logger::info("Discovered " + std::to_string(folderMap.size()) + " platform folders on Google Drive.");

    auto systems = DatabaseManager::instance().getSystems(false);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.status = SyncStatus::SYNCING_FILES;
        m_progress.totalSystems = static_cast<int>(systems.size());
    }

    DatabaseManager::instance().beginTransaction();

    int totalSynced = 0;
    for (size_t i = 0; i < systems.size(); ++i) {
        if (m_cancelRequested) break;

        const auto& sys = systems[i];
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_progress.currentPlatform = sys.name + " (" + sys.code + ")";
            m_progress.currentSystemIndex = static_cast<int>(i) + 1;
        }

        std::string upperCode = sys.code;
        std::transform(upperCode.begin(), upperCode.end(), upperCode.begin(), ::toupper);

        auto it = folderMap.find(upperCode);
        if (it != folderMap.end()) {
            Logger::info("Syncing cloud ROMs for " + sys.code + " (Folder ID: " + it->second + ")...");
            int sysCount = syncFilesForSystem(sys, it->second, token);
            totalSynced += sysCount;
        }
    }

    DatabaseManager::instance().commitTransaction();

    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M");
    std::string timeStr = ss.str();

    DatabaseManager::instance().setSetting("last_cloud_sync_time", timeStr);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_cancelRequested) {
            m_progress.status = SyncStatus::IDLE;
            Logger::info("Sync operation was cancelled by user.");
        } else {
            m_progress.status = SyncStatus::COMPLETED;
            Logger::info("Google Drive Library Sync complete! Total cloud games indexed: " +
                         std::to_string(m_progress.cloudGamesFound) + " (New: " +
                         std::to_string(m_progress.newGamesIndexed) + ", Updated: " +
                         std::to_string(m_progress.updatedGames) + ")");
        }
    }

    m_isRunning = false;
}

} // namespace RomCloud
