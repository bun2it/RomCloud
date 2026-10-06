#include "IPTVManager.h"
#include "../media/MpvPlayer.h"
#include "../ui/UIManager.h"
#include "../logging/Logger.h"
#include "../network/HttpClient.h"
#include "../filesystem/FileSystemManager.h"
#include "../config/AppConfig.h"
#include "../input/InputManager.h"
#include "../platform/PlatformInfo.h"
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include <curl/curl.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include <SDL2/SDL.h>

namespace RomCloud {

namespace {
// Cat chuoi theo so ky tu UTF-8 (khong cat giua dau tieng Viet) cho OSD mpv
static std::string truncateUtf8Chars(const std::string &s, size_t maxChars) {
    size_t count = 0, i = 0;
    size_t cutPos = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if ((c & 0x80) == 0) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (count >= maxChars) { cutPos = i; break; }
        count++;
        i += len;
        cutPos = i;
        if (count == maxChars && i < s.size()) return s.substr(0, i) + "..";
    }
    if (count <= maxChars) return s;
    return s.substr(0, cutPos) + "..";
}
// Font OSD/sub mpv: dung chung MpvPlayer::resolveOsdFont
// (uu tien NotoSans-Regular full TV, fallback font he thong).
inline std::string resolveOsdFont(const std::string &appRoot) {
    return MpvPlayer::resolveOsdFont(appRoot);
}
}

IPTVManager& IPTVManager::instance() {
    static IPTVManager instance;
    return instance;
}

IPTVManager::IPTVManager() {
    std::string appDir = AppConfig::instance().getAppRoot();
    if (appDir.empty()) {
        appDir = "/mnt/SDCARD/Apps/RomCloud";
    }
    m_iptvDir = appDir + "/iptv";

    // Ensure directory exists
    FileSystemManager::instance().createDirectoryRecursive(m_iptvDir);

    Logger::info("IPTVManager: Initializing from " + m_iptvDir);
    loadPlaylists(m_iptvDir);

    // If no playlist exists, create default playlist
    if (m_channels.empty()) {
        Logger::info("No playlists found, creating default playlist...");
        createDefaultPlaylist(m_iptvDir + "/default.m3u");
        loadPlaylists(m_iptvDir);
    }

    // Tu dong refresh cac playlist URL bi stale (khong block startup neu loi mang)
    int refreshed = checkAndAutoRefresh();
    if (refreshed > 0) {
        Logger::info("IPTVManager: Auto-refreshed " + std::to_string(refreshed) + " URL playlist(s) on startup");
    }
}

IPTVManager::~IPTVManager() {
    stop();
}

static size_t curlWriteBufferCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    auto* s = static_cast<std::string*>(userp);
    if (s) {
        s->append(static_cast<char*>(contents), total);
    }
    return total;
}

bool IPTVManager::loadPlaylists(const std::string& directory) {
    std::string dirPath = directory.empty() ? m_iptvDir : directory;
    Logger::info("Loading IPTV playlists from: " + dirPath);

    // Reset all state
    m_playlists.clear();
    m_channels.clear();
    m_groups.clear();
    m_sources.clear();
    loadFavorites();
    loadSourcesMeta();

    if (!FileSystemManager::instance().directoryExists(dirPath)) {
        Logger::warn("IPTV directory not found: " + dirPath);
        return false;
    }

    DIR* dir = opendir(dirPath.c_str());
    if (!dir) return false;

    std::vector<std::string> playlistFiles;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string filename = entry->d_name;
        if (filename.length() >= 4) {
            size_t dot = filename.rfind('.');
            if (dot != std::string::npos) {
                std::string ext = filename.substr(dot);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".m3u" || ext == ".m3u8")
                    playlistFiles.push_back(filename);
            }
        }
    }
    closedir(dir);
    std::sort(playlistFiles.begin(), playlistFiles.end());

    int loaded = 0;
    for (const auto& filename : playlistFiles) {
        std::string path = dirPath + "/" + filename;

        // Xac dinh ten hien thi cho playlist nay
        std::string sourceName;
        auto it = m_sourcesMeta.find(filename);
        if (it != m_sourcesMeta.end() && !it->second.name.empty()) {
            sourceName = it->second.name;
        } else {
            if (filename == "default.m3u")      sourceName = "Mặc định";
            else if (filename == "vietnam.m3u") sourceName = "Việt Nam";
            else {
                sourceName = filename;
                size_t dot = sourceName.rfind('.');
                if (dot != std::string::npos) sourceName = sourceName.substr(0, dot);
                std::replace(sourceName.begin(), sourceName.end(), '_', ' ');
            }
            IPTVSource newSrc;
            newSrc.name = sourceName; newSrc.filename = filename; newSrc.type = "file";
            m_sourcesMeta[filename] = newSrc;
        }

        // Tao slug cho playlist ID
        std::string slug;
        for (char c : filename) {
            if (c == '.') break;
            slug += ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ? c :
                    ((c >= 'A' && c <= 'Z') ? static_cast<char>(::tolower(c)) : '_');
        }
        if (slug.empty()) slug = "pl" + std::to_string(loaded);

        // --- Parse vao Playlist object doc lap ---
        Playlist pl;
        pl.id         = slug;
        pl.name       = sourceName;
        pl.sourceFile = filename;
        pl.sourcePath = path;

        // Lay URL neu co trong meta
        if (it != m_sourcesMeta.end()) pl.sourceUrl = it->second.url;

        struct stat st;
        if (stat(path.c_str(), &st) == 0) pl.fileSize = static_cast<size_t>(st.st_size);

        if (parseM3UIntoPlaylist(path, pl)) {
            m_playlists.push_back(std::move(pl));

            IPTVSource src = m_sourcesMeta[filename];
            src.name = sourceName;
            src.filename = filename;
            src.channelCount = m_playlists.back().channelCount();
            src.fileSize = m_playlists.back().fileSize;
            m_sources.push_back(src);
            loaded++;
        }
    }

    saveSourcesMeta();

    // Rebuild flat backward-compat list tu m_playlists
    rebuildFlatChannelList();

    Logger::info("Loaded " + std::to_string(m_channels.size()) + " IPTV channels from " +
                 std::to_string(loaded) + " playlist(s)");
    return !m_playlists.empty();
}


// ---------------------------------------------------------------------------
// parseM3UIntoPlaylist — Parse file .m3u vao mot Playlist doc lap
// Khong cham vao m_channels hay m_groups toan cuc
// ---------------------------------------------------------------------------
bool IPTVManager::parseM3UIntoPlaylist(const std::string& filepath, Playlist& out) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        Logger::error("Cannot open M3U file: " + filepath);
        return false;
    }

    std::string line;
    std::string currentGroup = "Chung";
    std::string currentName;
    std::string currentLogo;

    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (line.rfind("#EXTINF:", 0) == 0) {
            currentName = extractName(line);
            std::string grp = extractGroup(line);
            currentGroup = grp.empty() ? "Chung" : grp;
            currentLogo  = extractLogo(line);
            if (currentName.empty()) currentName = "Kênh không tên";
            continue;
        }
        if (line[0] == '#') continue;

        // Trim whitespace
        std::string url = line;
        while (!url.empty() && (url.front() == ' ' || url.front() == '\t')) url.erase(0, 1);
        while (!url.empty() && (url.back()  == ' ' || url.back()  == '\t')) url.pop_back();

        if (!url.empty() && url.find("://") != std::string::npos) {
            PlaylistItem item;
            item.name       = currentName;
            item.url        = url;
            item.group      = currentGroup;
            item.logo       = currentLogo;
            item.isFavorite = isFavorite(currentName);

            size_t idx = out.channels.size();
            out.channels.push_back(std::move(item));
            out.groups[currentGroup].push_back(idx);
        }
    }
    file.close();
    return true;
}

// ---------------------------------------------------------------------------
// rebuildFlatChannelList — Gop toan bo m_playlists thanh m_channels
// (backward-compat cho UIManager hien tai)
// ---------------------------------------------------------------------------
void IPTVManager::rebuildFlatChannelList() {
    m_channels.clear();
    m_groups.clear();

    for (const auto& pl : m_playlists) {
        for (const auto& item : pl.channels) {
            IPTVChannel ch = IPTVChannel::fromItem(item, pl.name, pl.sourceFile);
            size_t globalIdx = m_channels.size();
            m_channels.push_back(std::move(ch));
            m_groups[m_channels.back().group].push_back(globalIdx);
        }
    }
    Logger::info("rebuildFlatChannelList: " + std::to_string(m_channels.size()) +
                 " channels from " + std::to_string(m_playlists.size()) + " playlists");
}

// ---------------------------------------------------------------------------
// parseM3UFile — Backward-compat: delegate sang parseM3UIntoPlaylist
// ---------------------------------------------------------------------------
bool IPTVManager::parseM3UFile(const std::string& filepath, const std::string& sourceName,
                                const std::string& filename, size_t* outChannelCount) {
    Playlist tmp;
    tmp.id = filename; tmp.name = sourceName; tmp.sourceFile = filename;
    if (!parseM3UIntoPlaylist(filepath, tmp)) return false;
    if (outChannelCount) *outChannelCount = tmp.channelCount();
    // Ghi thang vao flat list (can thiet cho cac caller cu)
    for (const auto& item : tmp.channels) {
        IPTVChannel ch = IPTVChannel::fromItem(item, sourceName, filename);
        size_t idx = m_channels.size();
        m_channels.push_back(std::move(ch));
        m_groups[m_channels.back().group].push_back(idx);
    }
    return true;
}

std::string IPTVManager::extractGroup(const std::string& line) {
    size_t pos = line.find("group-title=\"");
    if (pos != std::string::npos) {
        pos += 13;
        size_t end = line.find("\"", pos);
        if (end != std::string::npos) {
            return line.substr(pos, end - pos);
        }
    }
    return "";
}

std::string IPTVManager::extractName(const std::string& line) {
    size_t commaPos = line.rfind(',');
    if (commaPos != std::string::npos && commaPos < line.length() - 1) {
        return line.substr(commaPos + 1);
    }
    return "";
}

std::string IPTVManager::extractLogo(const std::string& line) {
    size_t pos = line.find("tvg-logo=\"");
    if (pos != std::string::npos) {
        pos += 10;
        size_t end = line.find("\"", pos);
        if (end != std::string::npos) {
            return line.substr(pos, end - pos);
        }
    }
    return "";
}

bool IPTVManager::isFavorite(const std::string& channelName) const {
    return m_favorites.find(channelName) != m_favorites.end();
}

void IPTVManager::toggleFavorite(const std::string& channelName) {
    if (channelName.empty()) return;
    auto it = m_favorites.find(channelName);
    bool nowFav = false;
    if (it != m_favorites.end()) {
        m_favorites.erase(it);
        nowFav = false;
    } else {
        m_favorites.insert(channelName);
        nowFav = true;
    }
    // Update existing channels in memory
    for (auto& chan : m_channels) {
        if (chan.name == channelName) {
            chan.isFavorite = nowFav;
        }
    }
    saveFavorites();
    Logger::info("Toggled favorite for '" + channelName + "': " + (nowFav ? "ADDED" : "REMOVED"));
}

void IPTVManager::loadFavorites() {
    m_favorites.clear();
    std::string favPath = m_iptvDir + "/favorites.txt";
    std::ifstream file(favPath);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] != '#') {
            m_favorites.insert(line);
        }
    }
    file.close();
    Logger::info("Loaded " + std::to_string(m_favorites.size()) + " favorite channels");
}

void IPTVManager::saveFavorites() {
    std::string favPath = m_iptvDir + "/favorites.txt";
    std::ofstream file(favPath);
    if (!file.is_open()) {
        Logger::error("Cannot open favorites file for writing: " + favPath);
        return;
    }
    for (const auto& fav : m_favorites) {
        file << fav << "\n";
    }
    file.close();
}

std::vector<IPTVChannel> IPTVManager::getFavoriteChannels() const {
    std::vector<IPTVChannel> result;
    for (const auto& chan : m_channels) {
        if (chan.isFavorite) {
            result.push_back(chan);
        }
    }
    return result;
}

std::vector<IPTVChannel> IPTVManager::getChannelsByGroup(const std::string& group) const {
    std::vector<IPTVChannel> result;
    auto it = m_groups.find(group);
    if (it != m_groups.end()) {
        for (size_t idx : it->second) {
            if (idx < m_channels.size()) {
                result.push_back(m_channels[idx]);
            }
        }
    }
    return result;
}

std::vector<std::string> IPTVManager::getGroups() const {
    std::vector<std::string> groups;
    for (const auto& pair : m_groups) {
        groups.push_back(pair.first);
    }
    std::sort(groups.begin(), groups.end());
    return groups;
}

std::vector<IPTVChannel> IPTVManager::search(const std::string& query) const {
    std::vector<IPTVChannel> result;
    std::string lowerQuery = query;
    std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);

    for (const auto& channel : m_channels) {
        std::string lowerName = channel.name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

        std::string lowerGroup = channel.group;
        std::transform(lowerGroup.begin(), lowerGroup.end(), lowerGroup.begin(), ::tolower);

        std::string lowerSource = channel.source;
        std::transform(lowerSource.begin(), lowerSource.end(), lowerSource.begin(), ::tolower);

        if (lowerName.find(lowerQuery) != std::string::npos ||
            lowerGroup.find(lowerQuery) != std::string::npos ||
            lowerSource.find(lowerQuery) != std::string::npos) {
            result.push_back(channel);
        }
    }
    return result;
}

void IPTVManager::loadSourcesMeta() {
    m_sourcesMeta.clear();
    std::string metaPath = m_iptvDir + "/sources.txt";
    std::ifstream file(metaPath);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        // format: filename|name|type|url|lastRefreshed|refreshIntervalHours
        std::stringstream ss(line);
        std::string fn, name, type, url, tsStr, intervalStr;
        if (std::getline(ss, fn, '|') && std::getline(ss, name, '|')) {
            std::getline(ss, type, '|');
            std::getline(ss, url, '|');
            std::getline(ss, tsStr, '|');
            std::getline(ss, intervalStr, '|');

            IPTVSource src;
            src.filename = fn;
            src.name = name;
            src.type = type.empty() ? "file" : type;
            src.url = url;
            src.lastRefreshed = tsStr.empty() ? 0 : static_cast<std::time_t>(std::stoll(tsStr));
            src.refreshIntervalHours = intervalStr.empty() ? 24 : std::stoi(intervalStr);
            m_sourcesMeta[fn] = src;
        }
    }
    file.close();
}


void IPTVManager::saveSourcesMeta() {
    std::string metaPath = m_iptvDir + "/sources.txt";
    std::ofstream file(metaPath);
    if (!file.is_open()) return;

    file << "# RomCloud IPTV Sources Metadata\n";
    file << "# filename|name|type|url|lastRefreshed|refreshIntervalHours\n";
    for (const auto& pair : m_sourcesMeta) {
        const auto& s = pair.second;
        file << s.filename << "|"
             << s.name << "|"
             << s.type << "|"
             << s.url << "|"
             << static_cast<long long>(s.lastRefreshed) << "|"
             << s.refreshIntervalHours << "\n";
    }
    file.close();
}


bool IPTVManager::addSourceFromUrl(const std::string& url, const std::string& customName, std::string& outError, std::string& outFilename, size_t& outChannelCount) {
    outChannelCount = 0;
    outFilename.clear();
    outError.clear();

    std::string cleanUrl = url;
    while (!cleanUrl.empty() && (cleanUrl.front() == ' ' || cleanUrl.front() == '\t')) cleanUrl.erase(0, 1);
    while (!cleanUrl.empty() && (cleanUrl.back() == ' ' || cleanUrl.back() == '\t')) cleanUrl.pop_back();

    if (cleanUrl.empty() || (cleanUrl.find("http://") != 0 && cleanUrl.find("https://") != 0)) {
        outError = "URL không hợp lệ. Phải bắt đầu bằng http:// hoặc https://";
        return false;
    }

    std::string displayName = customName;
    while (!displayName.empty() && (displayName.front() == ' ' || displayName.front() == '\t')) displayName.erase(0, 1);
    while (!displayName.empty() && (displayName.back() == ' ' || displayName.back() == '\t')) displayName.pop_back();

    if (displayName.empty()) {
        size_t lastSlash = cleanUrl.find_last_of("/\\");
        if (lastSlash != std::string::npos && lastSlash + 1 < cleanUrl.size()) {
            std::string cand = cleanUrl.substr(lastSlash + 1);
            size_t q = cand.find('?');
            if (q != std::string::npos) cand = cand.substr(0, q);
            if (cand.size() > 4) {
                size_t dot = cand.rfind('.');
                if (dot != std::string::npos) cand = cand.substr(0, dot);
                displayName = cand;
            }
        }
        if (displayName.empty()) {
            displayName = "Nguồn URL " + std::to_string(std::time(nullptr));
        }
    }

    std::string baseSlug;
    for (char ch : displayName) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            baseSlug += static_cast<char>(std::tolower(ch));
        } else if (ch == ' ' || ch == '-' || ch == '_') {
            if (!baseSlug.empty() && baseSlug.back() != '_') {
                baseSlug += '_';
            }
        }
    }
    if (baseSlug.empty()) baseSlug = "playlist";
    while (!baseSlug.empty() && baseSlug.back() == '_') baseSlug.pop_back();

    std::string targetFilename = baseSlug + ".m3u";
    int counter = 1;
    while (true) {
        std::string fullPath = m_iptvDir + "/" + targetFilename;
        auto it = m_sourcesMeta.find(targetFilename);
        if (it != m_sourcesMeta.end()) {
            if (it->second.url == cleanUrl) {
                break;
            }
        } else if (!FileSystemManager::instance().fileExists(fullPath)) {
            break;
        }
        targetFilename = baseSlug + "_" + std::to_string(counter++) + ".m3u";
    }

    Logger::info("IPTV: Fetching URL: " + cleanUrl + " -> " + targetFilename);

    CURL* curl = curl_easy_init();
    if (!curl) {
        outError = "Không thể khởi tạo CURL handle";
        return false;
    }

    std::string responseBody;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36");
    headers = curl_slist_append(headers, "Accept: */*");
    headers = curl_slist_append(headers, "Accept-Language: vi,en;q=0.9");

    curl_easy_setopt(curl, CURLOPT_URL, cleanUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteBufferCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    if (access("/etc/ssl/certs/ca-certificates.crt", F_OK) == 0) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");
    }

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        outError = "Lỗi kết nối tải URL: " + std::string(curl_easy_strerror(res));
        Logger::error("IPTV addSourceFromUrl error: " + outError);
        return false;
    }

    if (httpCode < 200 || httpCode >= 400) {
        outError = "Máy chủ URL trả về HTTP " + std::to_string(httpCode);
        Logger::error("IPTV addSourceFromUrl error: " + outError);
        return false;
    }

    if (responseBody.empty()) {
        outError = "Nội dung tải về rỗng";
        return false;
    }

    if (responseBody.find("<!DOCTYPE html") != std::string::npos ||
        responseBody.find("<html") != std::string::npos) {
        if (responseBody.find("#EXTM3U") == std::string::npos && responseBody.find("#EXTINF") == std::string::npos) {
            outError = "URL trả về trang web HTML, không phải file playlist M3U/M3U8 hợp lệ";
            return false;
        }
    }

    std::string outPath = m_iptvDir + "/" + targetFilename;
    std::ofstream out(outPath, std::ios::binary);
    if (!out.is_open()) {
        outError = "Không thể ghi file vào " + outPath;
        return false;
    }
    out.write(responseBody.data(), responseBody.size());
    out.close();
    sync();

    IPTVSource meta;
    meta.name = displayName;
    meta.filename = targetFilename;
    meta.type = "url";
    meta.url = cleanUrl;
    meta.fileSize = responseBody.size();
    meta.lastRefreshed = std::time(nullptr); // danh dau moi tai ve
    meta.refreshIntervalHours = 24;          // default: tu dong refresh moi 24 gio
    m_sourcesMeta[targetFilename] = meta;
    saveSourcesMeta();

    loadPlaylists(m_iptvDir);

    outFilename = targetFilename;
    for (const auto& s : m_sources) {
        if (s.filename == targetFilename) {
            outChannelCount = s.channelCount;
            break;
        }
    }

    Logger::info("IPTV: Successfully added source '" + displayName + "' (" + targetFilename + ") with " +
                 std::to_string(outChannelCount) + " channels");
    return true;
}

bool IPTVManager::addSourceFromFile(const std::string& filename, const std::string& customName) {
    if (filename.empty()) return false;
    std::string name = customName;
    if (name.empty()) {
        name = filename;
        size_t dot = name.rfind('.');
        if (dot != std::string::npos) name = name.substr(0, dot);
        std::replace(name.begin(), name.end(), '_', ' ');
    }

    IPTVSource meta;
    meta.name = name;
    meta.filename = filename;
    meta.type = "file";
    meta.url = "";
    m_sourcesMeta[filename] = meta;
    saveSourcesMeta();
    loadPlaylists(m_iptvDir);
    return true;
}

bool IPTVManager::deleteSource(const std::string& filename, std::string& outError) {
    outError.clear();
    std::string clean = filename;
    while (!clean.empty() && (clean.front() == ' ' || clean.front() == '\t')) clean.erase(0, 1);
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '\t')) clean.pop_back();

    // Strip path if provided (e.g. iptv/foo.m3u or full path)
    size_t lastSlash = clean.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        clean = clean.substr(lastSlash + 1);
    }

    if (clean.empty() || clean.find("..") != std::string::npos) {
        outError = "Tên file không hợp lệ";
        return false;
    }

    std::string filePath = m_iptvDir + "/" + clean;
    if (unlink(filePath.c_str()) != 0) {
        Logger::warn("IPTV: Could not unlink " + filePath + " or file already gone");
    }

    m_sourcesMeta.erase(clean);
    m_sourcesMeta.erase(filename);
    saveSourcesMeta();
    loadPlaylists(m_iptvDir);

    Logger::info("IPTV: Deleted source " + clean + ". Channels remaining: " + std::to_string(m_channels.size()));
    return true;
}

IPTVChannel* IPTVManager::getChannel(size_t index) {
    if (index < m_channels.size()) {
        return &m_channels[index];
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// refreshPlaylistFromUrl
// Tai lai noi dung tu URL nguon, ghi de file .m3u, cap nhat lastRefreshed
// ---------------------------------------------------------------------------
bool IPTVManager::refreshPlaylistFromUrl(const std::string& filename, std::string& outError) {
    outError.clear();
    auto it = m_sourcesMeta.find(filename);
    if (it == m_sourcesMeta.end() || it->second.type != "url" || it->second.url.empty()) {
        outError = "Playlist không có URL nguồn hoặc không phải loại URL";
        return false;
    }

    const std::string& url = it->second.url;
    Logger::info("IPTV: Refreshing playlist '" + filename + "' from URL: " + url);

    CURL* curl = curl_easy_init();
    if (!curl) {
        outError = "Không thể khởi tạo CURL";
        return false;
    }

    std::string responseBody;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers,
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36");
    headers = curl_slist_append(headers, "Accept: */*");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteBufferCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        outError = "Lỗi kết nối: " + std::string(curl_easy_strerror(res));
        Logger::error("IPTV refresh error [" + filename + "]: " + outError);
        return false;
    }
    if (httpCode < 200 || httpCode >= 400) {
        outError = "Máy chủ trả về HTTP " + std::to_string(httpCode);
        Logger::error("IPTV refresh error [" + filename + "]: " + outError);
        return false;
    }
    if (responseBody.empty() ||
        (responseBody.find("#EXTM3U") == std::string::npos &&
         responseBody.find("#EXTINF") == std::string::npos)) {
        outError = "Nội dung tải về không phải M3U hợp lệ";
        return false;
    }

    // Ghi de file playlist cu
    std::string outPath = m_iptvDir + "/" + filename;
    std::ofstream outFile(outPath, std::ios::binary);
    if (!outFile.is_open()) {
        outError = "Không thể ghi file: " + outPath;
        return false;
    }
    outFile.write(responseBody.data(), responseBody.size());
    outFile.close();
    sync();

    // Cap nhat metadata: lastRefreshed = now
    it->second.lastRefreshed = std::time(nullptr);
    it->second.fileSize = responseBody.size();
    saveSourcesMeta();

    // Reload tat ca playlist
    loadPlaylists(m_iptvDir);

    Logger::info("IPTV: Refreshed '" + filename + "' OK (" +
                 std::to_string(responseBody.size()) + " bytes, " +
                 std::to_string(m_channels.size()) + " channels total)");
    return true;
}

// ---------------------------------------------------------------------------
// checkAndAutoRefresh
// Goi sau loadPlaylists() luc startup, tu dong tai lai cac playlist stale
// Tra ve so playlist da duoc refresh
// ---------------------------------------------------------------------------
int IPTVManager::checkAndAutoRefresh() {
    int refreshed = 0;
    std::vector<std::string> staleFiles;

    for (const auto& pair : m_sourcesMeta) {
        if (pair.second.isStale()) {
            staleFiles.push_back(pair.first);
        }
    }

    if (staleFiles.empty()) {
        Logger::info("IPTV: All URL playlists are up to date (no auto-refresh needed)");
        return 0;
    }

    Logger::info("IPTV: Auto-refreshing " + std::to_string(staleFiles.size()) + " stale playlist(s)...");
    for (const auto& fn : staleFiles) {
        std::string err;
        if (refreshPlaylistFromUrl(fn, err)) {
            refreshed++;
            Logger::info("IPTV: Auto-refresh OK: " + fn);
        } else {
            Logger::warn("IPTV: Auto-refresh FAILED [" + fn + "]: " + err);
        }
    }
    return refreshed;
}

// ---------------------------------------------------------------------------
// setRefreshInterval
// ---------------------------------------------------------------------------
void IPTVManager::setRefreshInterval(const std::string& filename, int hours) {
    auto it = m_sourcesMeta.find(filename);
    if (it != m_sourcesMeta.end()) {
        it->second.refreshIntervalHours = hours;
        saveSourcesMeta();
        Logger::info("IPTV: Set refresh interval for '" + filename +
                     "' to " + std::to_string(hours) + "h");
    }
}

// ---------------------------------------------------------------------------
// getLastRefreshedStr
// ---------------------------------------------------------------------------
std::string IPTVManager::getLastRefreshedStr(const std::string& filename) const {
    auto it = m_sourcesMeta.find(filename);
    if (it != m_sourcesMeta.end()) {
        return it->second.lastRefreshedStr();
    }
    return "Không rõ";
}

// ---------------------------------------------------------------------------
// getStalePlaylistFiles
// ---------------------------------------------------------------------------
std::vector<std::string> IPTVManager::getStalePlaylistFiles() const {
    std::vector<std::string> result;
    for (const auto& pair : m_sourcesMeta) {
        if (pair.second.isStale()) result.push_back(pair.first);
    }
    return result;
}



bool IPTVManager::isMediaPlayerInstalled() const {
    std::string appRoot = AppConfig::instance().getAppRoot();
    if (appRoot.empty()) appRoot = "/mnt/SDCARD/Apps/RomCloud";
    std::string mpvPath = appRoot + "/bin/mpv";
    if (access(mpvPath.c_str(), X_OK) == 0) return true;

    // Check system player paths (Stock OS, SpruceOS, NextUI)
    std::string sdRoot = AppConfig::instance().getSdRoot();
    const std::string players[] = {
        sdRoot + "/System/bin/mpv",
        sdRoot + "/Emus/VIDEOS/mpv.sh",
        sdRoot + "/Emu/VIDEOS/mpv.sh",
        sdRoot + "/Emu/MEDIA/bin64/ffplay",
        sdRoot + "/Emu/MEDIA/bin32/ffplay",
        "/usr/trimui/bin/mpv",
        "/usr/bin/mpv",
        appRoot + "/bin/ffplay",
        sdRoot + "/System/bin/ffplay",
        "/usr/bin/ffplay"
    };
    for (const auto& p : players) {
        if (access(p.c_str(), X_OK) == 0) return true;
    }
    return false;
}

bool IPTVManager::ensureMediaPlayerAvailable() {
    if (isMediaPlayerInstalled()) return true;

    std::string appRoot = AppConfig::instance().getAppRoot();
    if (appRoot.empty()) appRoot = "/mnt/SDCARD/Apps/RomCloud";
    std::string mpvPath = appRoot + "/bin/mpv";

    Logger::warn("IPTV: Media player (mpv/codecs) missing on device. Starting auto-download...");
    std::string bundleUrl = "https://github.com/bun2it/RomCloud/releases/download/v2.0.3/mpv_bundle.zip";
    std::string bundlePath = appRoot + "/mpv_bundle.zip";

    FILE* fp = fopen(bundlePath.c_str(), "wb");
    if (!fp) {
        Logger::error("IPTV: Cannot create mpv_bundle.zip for writing");
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, bundleUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (res != CURLE_OK || httpCode < 200 || httpCode >= 300) {
        Logger::error("IPTV: Failed to download mpv_bundle.zip (HTTP " + std::to_string(httpCode) + "): " + curl_easy_strerror(res));
        unlink(bundlePath.c_str());
        return false;
    }

    Logger::info("IPTV: Unpacking mpv_bundle.zip to " + appRoot);
    std::string unpackCmd = "unzip -o '" + bundlePath + "' -d '" + appRoot + "' 2>/dev/null || busybox unzip -o '" + bundlePath + "' -d '" + appRoot + "' 2>/dev/null";
    system(unpackCmd.c_str());
    unlink(bundlePath.c_str());
    chmod(mpvPath.c_str(), 0755);
    sync();

    if (isMediaPlayerInstalled()) {
        Logger::info("IPTV: Media player bundle installed successfully!");
        return true;
    }

    Logger::error("IPTV: mpv bundle extracted but mpv binary is not accessible");
    return false;
}

static std::string resolveCappedHlsUrl(const std::string& url, int targetHeight = 720) {
    if (url.empty()) return url;
    if (url.find(".m3u") == std::string::npos && url.find(".mpd") == std::string::npos) {
        return url;
    }

    try {
        HttpResponse resp = HttpClient::instance().get(url, {
            "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
        }, 4);
        if (!resp.success || resp.body.empty() || resp.body.find("#EXTM3U") == std::string::npos) {
            return url;
        }

        if (resp.body.find("#EXT-X-STREAM-INF") == std::string::npos) {
            return url;
        }

        std::string effectiveUrl = !resp.effectiveUrl.empty() ? resp.effectiveUrl : url;

        std::istringstream stream(resp.body);
        std::string line;
        struct Variant {
            int width = 0;
            int height = 0;
            int bandwidth = 0;
            std::string uri;
        };
        std::vector<Variant> variants;
        Variant curVariant;
        bool hasVariant = false;

        while (std::getline(stream, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
            if (line.rfind("#EXT-X-STREAM-INF:", 0) == 0) {
                curVariant = Variant();
                std::string upperLine = line;
                for (char& c : upperLine) c = std::toupper((unsigned char)c);

                size_t resPos = upperLine.find("RESOLUTION=");
                if (resPos != std::string::npos) {
                    size_t xPos = upperLine.find_first_of("X", resPos + 11);
                    if (xPos != std::string::npos) {
                        curVariant.width = std::atoi(line.c_str() + resPos + 11);
                        curVariant.height = std::atoi(line.c_str() + xPos + 1);
                    }
                }
                size_t bwPos = upperLine.find("BANDWIDTH=");
                if (bwPos != std::string::npos) {
                    curVariant.bandwidth = std::atoi(line.c_str() + bwPos + 10);
                }
                hasVariant = true;
            } else if (hasVariant && !line.empty() && line[0] != '#') {
                curVariant.uri = line;
                variants.push_back(curVariant);
                hasVariant = false;
            }
        }

        if (variants.empty()) return url;

        // Select best variant where height <= targetHeight (720) or bandwidth <= 4200000
        const Variant* best = nullptr;
        for (const auto& v : variants) {
            bool matches = false;
            if (v.height > 0) {
                matches = (v.height <= targetHeight);
            } else if (v.bandwidth > 0) {
                matches = (v.bandwidth <= 4200000);
            }
            if (matches) {
                if (!best || v.height > best->height || (v.height == best->height && v.bandwidth > best->bandwidth)) {
                    best = &v;
                }
            }
        }

        // If none <= targetHeight, pick the lowest resolution available
        if (!best) {
            for (const auto& v : variants) {
                if (!best || (v.height > 0 && (best->height == 0 || v.height < best->height))) {
                    best = &v;
                }
            }
        }

        if (best && !best->uri.empty()) {
            std::string resolved = best->uri;
            if (resolved.find("http://") != 0 && resolved.find("https://") != 0) {
                size_t qPos = effectiveUrl.find('?');
                std::string baseUrlNoQuery = (qPos != std::string::npos) ? effectiveUrl.substr(0, qPos) : effectiveUrl;

                if (!resolved.empty() && resolved[0] == '/') {
                    size_t protoEnd = baseUrlNoQuery.find("://");
                    size_t hostEnd = (protoEnd != std::string::npos) ? baseUrlNoQuery.find('/', protoEnd + 3) : std::string::npos;
                    if (hostEnd != std::string::npos) {
                        resolved = baseUrlNoQuery.substr(0, hostEnd) + resolved;
                    } else {
                        resolved = baseUrlNoQuery + resolved;
                    }
                } else {
                    size_t lastSlash = baseUrlNoQuery.rfind('/');
                    if (lastSlash != std::string::npos) {
                        resolved = baseUrlNoQuery.substr(0, lastSlash + 1) + resolved;
                    }
                }

                if (qPos != std::string::npos && resolved.find('?') == std::string::npos) {
                    resolved += effectiveUrl.substr(qPos);
                }
            }
            Logger::info("[IPTV] Auto-capped stream to 720p direct variant (" + std::to_string(best->width) + "x" +
                         std::to_string(best->height) + "): " + resolved);
            return resolved;
        }
    } catch (...) {}

    return url;
}

static std::string escapeJsonString(const std::string& input) {
    std::string output;
    for (char c : input) {
        if (c == '"') output += "\\\"";
        else if (c == '\\') output += "\\\\";
        else if (c == '\n') output += "\\n";
        else if (c == '\r') output += "\\r";
        else if (c == '\t') output += "\\t";
        else output += c;
    }
    return output;
}

// (removed) sendMpvIpcOverSocket — uy thac MpvPlayer::sendCmd.

// ─────────────────────────────────────────────
// IPTV Non-Blocking Playback
// mpv runs fullscreen in its own window on top of the screen,
// SDL UI continues to render channel list in the bottom ~360px region.
// ─────────────────────────────────────────────

bool IPTVManager::isMpvPlaying() const {
    return MpvPlayer::instance().isPlaying();
}

bool IPTVManager::isIPTVPlaying() const {
    return MpvPlayer::instance().isPlaying();
}

bool IPTVManager::sendMpvIpcCommand(const std::string& jsonCmd, std::string* response, const std::string& sockPath) {
    // P1-3: dung chung MpvPlayer (giu tuong thich sock cu).
    std::string target = sockPath;
    if (target.empty()) {
        if (access("/tmp/mpv_iptv.sock", F_OK) == 0) target = "/tmp/mpv_iptv.sock";
        else if (access("/tmp/mpv_youtube.sock", F_OK) == 0) target = "/tmp/mpv_youtube.sock";
        else target = "/tmp/mpv_iptv.sock";
    }
    return MpvPlayer::instance().sendCmd(jsonCmd, response, target);
}

void IPTVManager::showOverlayIcon(const std::string& iconName, uint32_t durationMs) {
    // P1-3: uy thac overlay + hen gio xoa cho MpvPlayer.
    std::string appRoot = AppConfig::instance().getAppRoot();
    if (appRoot.empty()) appRoot = "/mnt/SDCARD/Apps/RomCloud";
    MpvPlayer::instance().showOverlayIcon(appRoot, iconName, durationMs);
    m_overlayExpireTime = SDL_GetTicks() + durationMs;
}

static std::string fetchYouTubeStreamUrl(const std::string& videoId, const std::string& quality) {
    if (videoId.empty()) return "";
    // Dùng chung resolve của UIManager: đủ cả 2 URL video|audio cho DASH
    // (bản cũ chỉ lấy dòng đầu = video-only nên lên 720p là mất tiếng) +
    // cache theo chất lượng để chuyển tức thì.
    return UIManager::instance().resolveYouTubeStreamUrl(videoId, quality);
}

void IPTVManager::setUpgradeUrl(const std::string& videoId, const std::string& url) {
    std::lock_guard<std::mutex> lk(m_ytUpgradeMtx);
    m_ytUpgradeVid = videoId;
    m_ytUpgradeUrl = url;
}

bool IPTVManager::takeUpgradeUrl(const std::string& videoId, std::string& out) {
    std::lock_guard<std::mutex> lk(m_ytUpgradeMtx);
    if (m_ytUpgradeVid != videoId || m_ytUpgradeUrl.empty()) return false;
    out = m_ytUpgradeUrl;
    m_ytUpgradeUrl.clear(); // lấy 1 lần
    return true;
}

// iptvDbg: ghi qua Logger chuẩn (gộp vào debug.log chính với category IPTV).
// Trước đây ghi /tmp/iptv_debug.log riêng; giờ gộp 1 file để dễ debug.
static void iptvDbg(const std::string& msg) {
    RC_LOG_INFO(IPTV, msg);
}

// ---------------------------------------------------------------------------
// YouTube OSD 3 vùng (giống app YouTube) — CHỈ UI, không đụng phát/search.
// Top bar (overlay id 1): tên video + chất lượng.
// Bottom bar (overlay id 2): thanh tiến trình + giờ + gợi ý nút.
// Giữa màn hình tái dùng showOverlayIcon/show-text có sẵn.
// Icon tương lai (aspect/cc/speed/quality) chỉ cần thêm file png,
// không sửa logic.
// ---------------------------------------------------------------------------
static TTF_Font* ytOsdFont(const std::string& appRoot, int px, TTF_Font* &slot) {
    if (slot) return slot;
    if (TTF_Init() == -1) return nullptr;
    slot = TTF_OpenFont(MpvPlayer::resolveOsdFont(appRoot).c_str(), px);
    return slot;
}

static std::string ytTruncUtf8(TTF_Font* font, const std::string& s, int maxPx) {
    if (!font || s.empty()) return s;
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        size_t len = 1;
        if ((c & 0x80) == 0) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else { i++; continue; }
        std::string trial = out + s.substr(i, len);
        int w = 0, h = 0;
        if (TTF_SizeUTF8(font, trial.c_str(), &w, &h) != 0) break;
        if (w > maxPx) break;
        out = trial;
        i += len;
    }
    return out;
}

static void ytFillRect(std::vector<uint8_t>& cv, int CW, int CH,
                       int x, int y, int w, int h,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > CW) w = CW - x;
    if (y + h > CH) h = CH - y;
    if (w <= 0 || h <= 0) return;
    int stride = CW * 4;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) {
            size_t i = size_t(yy) * stride + xx * 4;
            cv[i+0] = b; cv[i+1] = g; cv[i+2] = r; cv[i+3] = a;
        }
}

static void ytFillCircle(std::vector<uint8_t>& cv, int CW, int CH,
                         int cx, int cy, int rad,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int yy = cy - rad; yy <= cy + rad; yy++)
        for (int xx = cx - rad; xx <= cx + rad; xx++) {
            int dx = xx - cx, dy = yy - cy;
            if (dx * dx + dy * dy > rad * rad) continue;
            if (xx < 0 || yy < 0 || xx >= CW || yy >= CH) continue;
            size_t i = size_t(yy) * (CW * 4) + xx * 4;
            cv[i+0] = b; cv[i+1] = g; cv[i+2] = r; cv[i+3] = a;
        }
}

static void ytBlitText(std::vector<uint8_t>& cv, int CW, int CH, TTF_Font* font,
                       const std::string& text, SDL_Color color, int x, int y) {
    if (text.empty() || !font) return;
    SDL_Surface* s = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!s) return;
    int stride = CW * 4;
    SDL_LockSurface(s);
    for (int yy = 0; yy < s->h; yy++) {
        if (y + yy < 0 || y + yy >= CH) continue;
        for (int xx = 0; xx < s->w; xx++) {
            if (x + xx < 0 || x + xx >= CW) continue;
            uint32_t px = ((uint32_t*)s->pixels)[yy * (s->pitch / 4) + xx];
            uint8_t a = (px >> 24) & 0xFF;
            if (a < 16) continue;
            size_t i = size_t(y + yy) * stride + (x + xx) * 4;
            uint8_t sr = (px >> 16) & 0xFF, sg = (px >> 8) & 0xFF, sb = px & 0xFF;
            uint16_t aa = a, inv = 255 - aa;
            cv[i+0] = (uint8_t)((sb * aa + cv[i+0] * inv) / 255);
            cv[i+1] = (uint8_t)((sg * aa + cv[i+1] * inv) / 255);
            cv[i+2] = (uint8_t)((sr * aa + cv[i+2] * inv) / 255);
            if (cv[i+3] < a) cv[i+3] = a;
        }
    }
    SDL_UnlockSurface(s);
    SDL_FreeSurface(s);
}

static std::string ytFmtTime(double sec) {
    if (sec < 0) sec = 0;
    long s = (long)sec;
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld:%02ld", s / 60, s % 60);
    return buf;
}

// spawnMpvForUrl: P1-3 uy thac MpvPlayer (giu API cu cho switch kênh).
pid_t IPTVManager::spawnMpvForUrl(const std::string& url) {
    if (url.empty()) return -1;
    MpvPlayer& p = MpvPlayer::instance();
    if (!p.play(url, {"--video-align-y=0", "--video-align-x=0",
                      "--demuxer-max-bytes=32M", "--demuxer-readahead-secs=8"},
                "/tmp/mpv_iptv.sock"))
        return -1;
    return p.pid();
}
static void killMpvPidBlocking(pid_t pid) {
    if (pid <= 0) return;
    // P1-3: uy thac MpvPlayer (quit/TERM/KILL). Giu ten ham de khoi sua call-site.
    MpvPlayer::instance().stop();
    unlink("/tmp/mpv_iptv.sock");
}

// switchIPTVChannelByIndex: RESTART mpv sach cho kenh moi (thay vi loadfile).
bool IPTVManager::switchIPTVChannelByIndex(size_t idx) {
    if (m_iptvChannelList.empty() || idx >= m_iptvChannelList.size()) return false;
    // Debounce: chặn double-fire trong 300ms (bấm A 2 lần)
    uint32_t nowMs = SDL_GetTicks();
    iptvDbg("SWITCH req idx=" + std::to_string(idx) + " cur=" + std::to_string(m_iptvCurrentIndex));
    if (nowMs - m_lastASwitchMs < 300) {
        iptvDbg("SWITCH debounced (<300ms), skip");
        return true;
    }
    m_lastASwitchMs = nowMs;
    IPTVChannel ch = m_iptvChannelList[idx];
    if (ch.url.empty()) return false;
    uint32_t t0 = SDL_GetTicks();
    bool cacheHit = false;
    std::string url;
    {
        std::lock_guard<std::mutex> lock(m_streamCacheMutex);
        auto it = m_streamUrlCache.find(ch.url);
        if (it != m_streamUrlCache.end()) {
            url = it->second;
            cacheHit = true;
        } else {
            url = resolveCappedHlsUrl(ch.url, 720);
            if (m_streamUrlCache.size() >= kStreamCacheMax) {
                m_streamUrlCache.erase(m_streamUrlCache.begin());
            }
            m_streamUrlCache[ch.url] = url;
        }
    }
    {
        std::lock_guard<std::mutex> pl(m_prefetchMutex);
        if (m_prefetchKey == ch.url && !m_prefetchUrl.empty()) {
            url = m_prefetchUrl;
            cacheHit = true;
            iptvDbg("SWITCH prefetch HIT");
        }
    }
    iptvDbg("SWITCH resolve hit=" + std::string(cacheHit ? "1" : "0") +
            " took=" + std::to_string(SDL_GetTicks() - t0) + "ms");
    if (url.empty()) return false;
    uint32_t tk = SDL_GetTicks();
    iptvDbg("SWITCH kill old pid=" + std::to_string(MpvPlayer::instance().pid()));
    killMpvPidBlocking(MpvPlayer::instance().pid());
    iptvDbg("SWITCH killed took=" + std::to_string(SDL_GetTicks() - tk) + "ms");
    uint32_t ts = SDL_GetTicks();
    pid_t npid = spawnMpvForUrl(url);
    if (npid <= 0) { iptvDbg("SWITCH spawn FAIL"); return false; }
    if (MpvPlayer::instance().pollExited()) {
        iptvDbg("SWITCH new died early");
        return false;
    }
    iptvDbg("SWITCH spawned pid=" + std::to_string(npid) +
            " took=" + std::to_string(SDL_GetTicks() - ts) + "ms");
    m_iptvSelectedIndex = idx;
    m_iptvCurrentIndex = idx;
    m_currentChannel = ch.name;
    {
        std::vector<std::string> keys;
        if (idx + 1 < m_iptvChannelList.size()) keys.push_back(m_iptvChannelList[idx + 1].url);
        if (idx > 0) keys.push_back(m_iptvChannelList[idx - 1].url);
        std::thread([this, keys]() {
            for (auto& k : keys) {
                if (k.empty()) continue;
                { std::lock_guard<std::mutex> l(m_streamCacheMutex);
                  if (m_streamUrlCache.find(k) != m_streamUrlCache.end()) continue; }
                std::string u = resolveCappedHlsUrl(k, 720);
                if (u.empty()) continue;
                { std::lock_guard<std::mutex> l(m_streamCacheMutex);
                  if (m_streamUrlCache.size() >= kStreamCacheMax) m_streamUrlCache.erase(m_streamUrlCache.begin());
                  m_streamUrlCache[k] = u; }
                { std::lock_guard<std::mutex> pl(m_prefetchMutex);
                  m_prefetchKey = k; m_prefetchUrl = u; }
                iptvDbg("PREFETCH done");
            }
        }).detach();
    }
    Logger::info("IPTV: switched restart to: " + ch.name);
    return true;
}

// showIPTVChannelOSD: ve list 3 dong (prev/highlight/next) vao dai den duoi.
// Video 16:9 full-width dinh mep tren (y=0..576) -> panel 1024x192 tai (0,576).
void IPTVManager::showIPTVChannelOSD(
    const std::vector<IPTVChannel>& channels,
    int selectedIndex,
    const std::string& groupName,
    int durationMs)
{
    (void)durationMs; // list hien lien tuc, khong tu an
    if (!MpvPlayer::instance().isPlaying()) return;
    if (channels.empty()) return;
    if (selectedIndex < 0) selectedIndex = 0;
    if (selectedIndex >= (int)channels.size()) selectedIndex = (int)channels.size() - 1;

    // Get font path (same as UIManager uses)
    std::string appRoot = AppConfig::instance().getAppRoot();
    if (appRoot.empty()) appRoot = "/mnt/SDCARD/Apps/RomCloud";
    std::string fontPath = resolveOsdFont(appRoot);

    // Open fonts if not cached (ten kenh to + header/footer nho)
    static TTF_Font* s_fontBig = nullptr;
    static TTF_Font* s_fontSmall = nullptr;
    if (!s_fontBig) {
        if (TTF_Init() == -1) return;
        s_fontBig = TTF_OpenFont(fontPath.c_str(), 24);
        s_fontSmall = TTF_OpenFont(fontPath.c_str(), 18);
        if (!s_fontBig || !s_fontSmall) return;
    }

    // Panel 1024x192 nam gon trong dai den duoi video (576..768)
    const int CW = 1024;
    const int CH = 192;
    const int OY = 576;
    const int STRIDE = CW * 4;
    std::vector<uint8_t> canvas(STRIDE * CH, 0);

    auto fillRect = [&](int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > CW) w = CW - x;
        if (y + h > CH) h = CH - y;
        if (w <= 0 || h <= 0) return;
        for (int yy = y; yy < y + h; yy++)
            for (int xx = x; xx < x + w; xx++) {
                size_t i = size_t(yy) * STRIDE + xx * 4;
                canvas[i+0] = b; canvas[i+1] = g; canvas[i+2] = r; canvas[i+3] = a;
            }
    };

    auto blitText = [&](TTF_Font* font, const std::string& text, SDL_Color color,
                        int x, int y, uint8_t alpha) {
        if (text.empty() || !font) return;
        SDL_Surface* s = TTF_RenderUTF8_Blended(font, text.c_str(), color);
        if (!s) return;
        SDL_LockSurface(s);
        for (int yy = 0; yy < s->h; yy++) {
            if (y + yy < 0 || y + yy >= CH) continue;
            for (int xx = 0; xx < s->w; xx++) {
                if (x + xx < 0 || x + xx >= CW) continue;
                uint32_t px = ((uint32_t*)s->pixels)[yy * (s->pitch / 4) + xx];
                uint8_t a = (px >> 24) & 0xFF;
                if (a < 16) continue;
                size_t i = size_t(y + yy) * STRIDE + (x + xx) * 4;
                uint8_t sr = (px >> 16) & 0xFF, sg = (px >> 8) & 0xFF, sb = px & 0xFF;
                uint16_t aa = (uint16_t)a * alpha / 255, inv = 255 - aa;
                canvas[i+0] = (uint8_t)((sb * aa + canvas[i+0] * inv) / 255);
                canvas[i+1] = (uint8_t)((sg * aa + canvas[i+1] * inv) / 255);
                canvas[i+2] = (uint8_t)((sr * aa + canvas[i+2] * inv) / 255);
                if (canvas[i+3] < alpha) canvas[i+3] = alpha;
            }
        }
        SDL_UnlockSurface(s);
        SDL_FreeSurface(s);
    };

    SDL_Color white = {255, 255, 255, 255};
    SDL_Color gold  = {0, 180, 216, 255};
    SDL_Color gray  = {170, 180, 195, 255};
    SDL_Color dim   = {130, 140, 155, 255};

    auto fillCircle = [&](int cx, int cy, int rad, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        for (int yy = cy - rad; yy <= cy + rad; yy++) {
            if (yy < 0 || yy >= CH) continue;
            for (int xx = cx - rad; xx <= cx + rad; xx++) {
                if (xx < 0 || xx >= CW) continue;
                int dx = xx - cx, dy = yy - cy;
                if (dx * dx + dy * dy > rad * rad) continue;
                size_t i = size_t(yy) * STRIDE + xx * 4;
                canvas[i+0] = b; canvas[i+1] = g; canvas[i+2] = r; canvas[i+3] = a;
            }
        }
    };

    // Nen den mo + vien xanh tren (tone app)
    fillRect(0, 0, CW, CH, 14, 18, 26, 235);
    fillRect(0, 0, CW, 3, 0, 180, 216, 255);

    // Header: tên group + số kênh (giữ nguyên dấu tiếng Việt, font hệ thống đủ glyph)
    std::string grp = groupName;
    if (grp.empty() && selectedIndex < (int)channels.size()) grp = channels[selectedIndex].group;

    if (grp.empty()) grp = "Truyền hình";
    grp = truncateUtf8Chars(grp, 40);
    char cntBuf[64];
    snprintf(cntBuf, sizeof(cntBuf), "%d kênh", (int)channels.size());
    blitText(s_fontSmall, grp, gold, 20, 8, 255);
    blitText(s_fontSmall, cntBuf, gray, CW - 120, 8, 255);

    // Cua so 3 dong neo theo highlight: [prev][highlight][next]
    int n = (int)channels.size();
    int start = selectedIndex - 1;
    if (selectedIndex <= 0) start = 0;
    if (selectedIndex >= n - 1) start = n - 3;
    if (start < 0) start = 0;
    int end = std::min(n, start + 3);

    const int rowY0 = 38;
    const int rowH = 40;
    for (int r = 0; r < (end - start); r++) {
        int i = start + r;
        int ry = rowY0 + r * rowH;
        bool isSel = (i == selectedIndex);
        bool isPlay = (i == (int)m_iptvCurrentIndex);
        if (isSel) {
            fillRect(12, ry, CW - 24, rowH - 4, 23, 55, 110, 235);
            fillRect(12, ry, CW - 24, 2, 0, 180, 216, 255);
            fillRect(12, ry + rowH - 6, CW - 24, 2, 0, 180, 216, 255);
        }
        std::string nm = truncateUtf8Chars(channels[i].name, 30);
        char idxBuf[16];
        snprintf(idxBuf, sizeof(idxBuf), "%02d", i + 1);
        blitText(s_fontBig, idxBuf, isSel ? gold : dim, 28, ry + 6, 255);
        blitText(s_fontBig, nm, isSel ? white : gray, 90, ry + 6, 255);
        if (isPlay) {
            int dotCX = CW - 165, dotCY = ry + 20;
            fillCircle(dotCX, dotCY, 7, 34, 197, 94, 255);
            blitText(s_fontSmall, isSel ? "ĐANG XEM" : "đang xem", gold, CW - 150, ry + 10, 255);
        }
    }

    blitText(s_fontSmall, "Lên/Xuống: Chọn  |  A: Xem  |  SELECT: Ẩn/Hiện OSD  |  B: Thoát",
             dim, 20, CH - 24, 255);

    // Write to raw file
    FILE* fp = fopen("/tmp/osd_list.raw", "wb");
    if (!fp) return;
    fwrite(canvas.data(), 1, canvas.size(), fp);
    fclose(fp);

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "{\"command\":[\"overlay-add\",1,0,%d,\"/tmp/osd_list.raw\",0,\"bgra\",%d,%d,%d]}\n",
        OY, CW, CH, STRIDE);

    std::string reply;
    bool ok = sendMpvIpcCommand(cmd, &reply, "/tmp/mpv_iptv.sock");
    RC_LOG_INFO(OSD, "overlay-add list3: ok=" + std::to_string(ok) +
                       " surf=" + std::to_string(CW) + "x" + std::to_string(CH) +
                       " @ (0," + std::to_string(OY) + ")");
}

void IPTVManager::hideIPTVChannelOSD() {
    std::string reply;
    // Remove all overlays (0 and 1)
    sendMpvIpcCommand("{\"command\":[\"overlay-remove\",0]}\n", &reply, "/tmp/mpv_iptv.sock");
    sendMpvIpcCommand("{\"command\":[\"overlay-remove\",1]}\n", &reply, "/tmp/mpv_iptv.sock");
    hideIPTVPlaybackFooter();
}

void IPTVManager::showIPTVPlaybackFooter(bool isPaused) {
    // Footer chuẩn app (giống drawAppFooter/drawFooterHintsCentered):
    // nền FOOTER_BG + kẻ FOOTER_LINE, icon nút thật, hints canh giữa.
    // Hints khớp control loop playChannel: A pause/resume, B/MENU thoát,
    // SELECT ẩn/hiện list kênh, UP/DOWN mở list + đổi highlight.
    const int CW = 1024;
    const int CH = 53;
    const int OY = 715;  // UiTheme::FOOTER_Y
    const int STRIDE = CW * 4;
    const int ICON_SIZE = 26;  // UiTheme::FOOTER_ICON
    const int GAP = 8;         // UiTheme::FOOTER_GAP
    const int HINT_GAP = 28;   // UiTheme::FOOTER_HINT_GAP
    std::vector<uint8_t> canvas(STRIDE * CH, 0);

    auto fillRect = [&](int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > CW) w = CW - x;
        if (y + h > CH) h = CH - y;
        if (w <= 0 || h <= 0) return;
        for (int yy = y; yy < y + h; yy++)
            for (int xx = x; xx < x + w; xx++) {
                size_t i = size_t(yy) * STRIDE + xx * 4;
                canvas[i+0] = b; canvas[i+1] = g; canvas[i+2] = r; canvas[i+3] = a;
            }
    };

    // Open font
    std::string appRoot = AppConfig::instance().getAppRoot();
    if (appRoot.empty()) appRoot = "/mnt/SDCARD/Apps/RomCloud";
    std::string fontPath = resolveOsdFont(appRoot);
    static TTF_Font* s_fontFooter = nullptr;
    if (!s_fontFooter) {
        if (TTF_Init() == -1) return;
        s_fontFooter = TTF_OpenFont(fontPath.c_str(), 22);
        if (!s_fontFooter) return;
    }

    auto textWidthPx = [&](const std::string& text) -> int {
        if (text.empty() || !s_fontFooter) return 0;
        int w = 0, h = 0;
        if (TTF_SizeUTF8(s_fontFooter, text.c_str(), &w, &h) != 0) return 0;
        return w;
    };

    auto blitText = [&](TTF_Font* font, const std::string& text, SDL_Color color,
                        int x, int y) {
        if (text.empty() || !font) return;
        SDL_Surface* s = TTF_RenderUTF8_Blended(font, text.c_str(), color);
        if (!s) return;
        SDL_LockSurface(s);
        for (int yy = 0; yy < s->h; yy++) {
            if (y + yy < 0 || y + yy >= CH) continue;
            for (int xx = 0; xx < s->w; xx++) {
                if (x + xx < 0 || x + xx >= CW) continue;
                uint32_t px = ((uint32_t*)s->pixels)[yy * (s->pitch / 4) + xx];
                uint8_t a = (px >> 24) & 0xFF;
                if (a < 16) continue;
                size_t i = size_t(y + yy) * STRIDE + (x + xx) * 4;
                uint8_t sr = (px >> 16) & 0xFF, sg = (px >> 8) & 0xFF, sb = px & 0xFF;
                uint16_t aa = (uint16_t)a;
                uint16_t inv = 255 - aa;
                canvas[i+0] = (uint8_t)((sb * aa + canvas[i+0] * inv) / 255);
                canvas[i+1] = (uint8_t)((sg * aa + canvas[i+1] * inv) / 255);
                canvas[i+2] = (uint8_t)((sr * aa + canvas[i+2] * inv) / 255);
                if (canvas[i+3] < a) canvas[i+3] = a;
            }
        }
        SDL_UnlockSurface(s);
        SDL_FreeSurface(s);
    };

    // Blit icon nút chuẩn (assets/button_icons/*, icon chuyên biệt
    // SELECT.png / vertical.png — cấm icon generic legacy).
    auto blitIcon = [&](const std::string& iconFile, int x, int y, int size) -> bool {
        std::string p = appRoot + "/assets/button_icons/" + iconFile;
        SDL_Surface* surf = IMG_Load(p.c_str());
        if (!surf) return false;
        bool ok = false;
        if (surf->w > 0 && surf->h > 0) {
            float scale = (float)size / (float)std::max(surf->w, surf->h);
            int dw = (int)(surf->w * scale);
            int dh = (int)(surf->h * scale);
            if (dw <= 0) dw = 1;
            if (dh <= 0) dh = 1;
            int dx0 = x + (size - dw) / 2;
            int dy0 = y + (size - dh) / 2;
            SDL_LockSurface(surf);
            for (int dy = 0; dy < dh; dy++) {
                int sy = dy * surf->h / dh;
                int cy = dy0 + dy;
                if (cy < 0 || cy >= CH) continue;
                for (int dx = 0; dx < dw; dx++) {
                    int sx = dx * surf->w / dw;
                    int cx = dx0 + dx;
                    if (cx < 0 || cx >= CW) continue;
                    Uint8 sr = 255, sg = 255, sb = 255, sa = 255;
                    Uint32 px = 0;
                    if (surf->format->BytesPerPixel == 4) {
                        px = ((Uint32*)surf->pixels)[sy * (surf->pitch / 4) + sx];
                        SDL_GetRGBA(px, surf->format, &sr, &sg, &sb, &sa);
                    } else if (surf->format->BytesPerPixel == 3) {
                        Uint8* pp = (Uint8*)surf->pixels + sy * surf->pitch + sx * 3;
                        Uint32 tmp = (Uint32)pp[0] | ((Uint32)pp[1] << 8) | ((Uint32)pp[2] << 16);
                        SDL_GetRGB(tmp, surf->format, &sr, &sg, &sb);
                        sa = 255;
                    } else {
                        continue;
                    }
                    if (sa < 16) continue;
                    size_t i = size_t(cy) * STRIDE + cx * 4;
                    uint16_t aa = (uint16_t)sa;
                    uint16_t inv = 255 - aa;
                    canvas[i+0] = (uint8_t)((sb * aa + canvas[i+0] * inv) / 255);
                    canvas[i+1] = (uint8_t)((sg * aa + canvas[i+1] * inv) / 255);
                    canvas[i+2] = (uint8_t)((sr * aa + canvas[i+2] * inv) / 255);
                    if (canvas[i+3] < sa) canvas[i+3] = sa;
                }
            }
            SDL_UnlockSurface(surf);
            ok = true;
        }
        SDL_FreeSurface(surf);
        return ok;
    };

    // Nền + kẻ ngăn cách chuẩn (UiTheme::FOOTER_BG / FOOTER_LINE).
    // drawAppFooter tự vẽ 2 lớp này — footer mpv overlay phải vẽ tay y hệt.
    fillRect(0, 0, CW, CH, 18, 22, 30, 255);
    fillRect(0, 0, CW, 1, 40, 48, 62, 255);

    // Thứ tự SELECT trước START theo Chin Buttons Standard (ở đây không có START).
    struct Hint { const char* btn; const char* icon; std::string label; };
    std::vector<Hint> hints = {
        {"A",      "a.png",        isPaused ? "Phát" : "Tạm dừng"},
        {"B",      "b.png",        "Thoát"},
        {"SELECT", "SELECT.png",   "Kênh"},
        {"UPDOWN", "vertical.png", "Chọn"},
    };
    SDL_Color labelColor = {200, 210, 225, 255};  // UiTheme::TEXT_DIM

    // Đo tổng rộng rồi canh giữa — y hệt drawFooterHintsCentered.
    int totalW = 0;
    for (size_t k = 0; k < hints.size(); ++k) {
        totalW += ICON_SIZE + GAP + textWidthPx(hints[k].label);
        if (k + 1 < hints.size()) totalW += HINT_GAP;
    }
    int xPos = (CW - totalW) / 2;
    if (xPos < 8) xPos = 8;
    int centerY = CH / 2;
    int th = TTF_FontHeight(s_fontFooter);
    for (size_t k = 0; k < hints.size(); ++k) {
        int iconY = centerY - ICON_SIZE / 2;
        if (!blitIcon(hints[k].icon, xPos, iconY, ICON_SIZE)) {
            // Fallback khi thiếu PNG: vẽ tên nút dạng text để không mất hint.
            std::string fb = std::string("[") + hints[k].btn + "]";
            blitText(s_fontFooter, fb, labelColor, xPos, centerY - th / 2);
        }
        xPos += ICON_SIZE + GAP;
        blitText(s_fontFooter, hints[k].label, labelColor, xPos, centerY - th / 2);
        xPos += textWidthPx(hints[k].label);
        if (k + 1 < hints.size()) xPos += HINT_GAP;
    }

    // Write to raw file
    FILE* fp = fopen("/tmp/osd_footer.raw", "wb");
    if (!fp) return;
    fwrite(canvas.data(), 1, canvas.size(), fp);
    fclose(fp);

    // Send to mpv as overlay ID 2 (persistent footer)
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "{\"command\":[\"overlay-add\",2,0,%d,\"/tmp/osd_footer.raw\",0,\"bgra\",%d,%d,%d]}\n",
        OY, CW, CH, STRIDE);
    std::string reply;
    sendMpvIpcCommand(cmd, &reply, "/tmp/mpv_iptv.sock");
}

void IPTVManager::hideIPTVPlaybackFooter() {
    std::string reply;
    sendMpvIpcCommand("{\"command\":[\"overlay-remove\",2]}\n", &reply, "/tmp/mpv_iptv.sock");
}

// playChannel: blocking. Forks mpv and handles all playback controls + channel list OSD
// in the same process. This is the same pattern as playYouTubeVideo.
bool IPTVManager::playChannel(const IPTVChannel& channel, size_t initialIndex, const std::vector<IPTVChannel>& customList) {
    stop();

    if (channel.url.empty()) {
        Logger::error("IPTV: channel URL is empty");
        return false;
    }

    m_iptvChannelList = customList.empty() ? m_channels : customList;
    m_iptvSelectedIndex = initialIndex;
    m_iptvCurrentIndex = initialIndex;

    // Resolve URL với LRU cache (bản cũ chạy ổn: nhiều nguồn cần User-Agent
    // khi lấy master playlist, mpv 0.32 gửi thẳng hay rớt).
    std::string url;
    {
        std::lock_guard<std::mutex> lock(m_streamCacheMutex);
        auto it = m_streamUrlCache.find(channel.url);
        if (it != m_streamUrlCache.end()) {
            url = it->second;
        } else {
            url = resolveCappedHlsUrl(channel.url, 720);
            if (m_streamUrlCache.size() >= kStreamCacheMax) {
                m_streamUrlCache.erase(m_streamUrlCache.begin());
            }
            m_streamUrlCache[channel.url] = url;
        }
    }
    if (url.empty()) {
        Logger::error("IPTV: URL resolution failed for: " + channel.url);
        return false;
    }
    Logger::info("IPTV: playing channel: " + channel.name);

    // P1-3: fork qua MpvPlayer (giu flag OSD/list kenh cu).
    if (!MpvPlayer::instance().play(url, {
            "--video-align-y=0", "--video-align-x=0",
            "--vd-lavc-fast", "--vd-lavc-skiploopfilter=nonref",
            "--vd-lavc-framedrop=nonref", "--sws-scaler=fast-bilinear",
            "--dscale=bilinear", "--scale=bilinear",
            "--demuxer-max-bytes=32M", "--demuxer-readahead-secs=8",
            "--audio-buffer=1.0", "--osd-level=2", "--osd-bar=no",
            "--osd-font-size=24", "--osd-margin-x=16", "--osd-margin-y=8",
            "--osd-align-x=left", "--osd-align-y=bottom",
            "--osd-border-size=1", "--osd-duration=3000" },
            "/tmp/mpv_iptv.sock")) {
        Logger::error("IPTV: No mpv binary found");
        return false;
    }
    pid_t pid = MpvPlayer::instance().pid();
    // P1-3: MpvPlayer da fork + doi sock; control loop ben duoi giu nguyen.
        m_isPlaying = true;
        m_currentChannel = channel.name;
        uint32_t playStartTime = SDL_GetTicks();
        m_lastASwitchMs = 0;
        bool isPaused = false;
        bool channelListVisible = false;
        m_overlayExpireTime = 0;

        // Hiện footer bar khi bắt đầu phát
        showIPTVPlaybackFooter(isPaused);
        Logger::info("IPTV: mpv started with PID: " + std::to_string(pid));

        SDL_PumpEvents();
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);

        // Wait for IPC socket (MpvPlayer::play da doi sock; chi check chet som).
        MpvPlayer::instance().waitForSocket(2000);

        // Blocking control loop
        // Video phat o center; hien ten kenh 2.5s dau
        channelListVisible = false;
        sendMpvIpcCommand("{\"command\":[\"show-text\",\"" + escapeJsonString(channel.name) + "\",2500]}", nullptr, "/tmp/mpv_iptv.sock");
        while (m_isPlaying && MpvPlayer::instance().isPlaying()) {
            if (MpvPlayer::instance().pollExited()) break;

            if (m_overlayExpireTime > 0 && SDL_GetTicks() >= m_overlayExpireTime) {
                sendMpvIpcCommand("{\"command\":[\"overlay-remove\",0]}", nullptr, "/tmp/mpv_iptv.sock");
                m_overlayExpireTime = 0;
            }

            InputManager::instance().update();
            auto& input = InputManager::instance();

            if (SDL_GetTicks() - playStartTime < 600) {
                SDL_Delay(35);
                continue;
            }

            // B: Quit
            if (input.isButtonJustPressed(Button::B) || input.isButtonJustPressed(Button::MENU)) {
                stop();
                break;

            // A: Chuyển kênh khi list hien, Pause/Resume khi list an
            } else if (input.isButtonJustPressed(Button::A)) {
                iptvDbg("BTN A pressed, listVisible=" + std::string(channelListVisible ? "1" : "0") +
                        " sel=" + std::to_string(m_iptvSelectedIndex) + " cur=" + std::to_string(m_iptvCurrentIndex));
                if (channelListVisible && !m_iptvChannelList.empty()) {
                    size_t idx = m_iptvSelectedIndex;
                    if (idx < m_iptvChannelList.size()) {
                        if (idx != m_iptvCurrentIndex) {
                            uint32_t ta = SDL_GetTicks();
                            bool okSw = switchIPTVChannelByIndex(idx);
                            iptvDbg("BTN A switch done ok=" + std::string(okSw ? "1" : "0") +
                                    " total=" + std::to_string(SDL_GetTicks() - ta) + "ms");
                        }
                    }
                    // Chon kenh xong: an OSD va tra video ve lai giua man hinh (auto center)
                    channelListVisible = false;
                    sendMpvIpcCommand("{\"command\":[\"overlay-remove\",1]}", nullptr, "/tmp/mpv_iptv.sock");
                    sendMpvIpcCommand("{\"command\":[\"set_property\",\"video-align-y\",0]}", nullptr, "/tmp/mpv_iptv.sock");
                } else {
                    isPaused = !isPaused;
                    sendMpvIpcCommand("{\"command\":[\"cycle\",\"pause\"]}", nullptr, "/tmp/mpv_iptv.sock");
                    showOverlayIcon(isPaused ? "pause" : "play", 1500);
                    sendMpvIpcCommand(
                        isPaused ? "{\"command\":[\"show-text\",\"{\\\\an2\\\\fs44\\\\bord2\\\\b1}❚❚  TẠM DỪNG\", 1500]}"
                                 : "{\"command\":[\"show-text\",\"{\\\\an2\\\\fs44\\\\bord2\\\\b1}▶  ĐANG PHÁT\", 1500]}",
                        nullptr, "/tmp/mpv_iptv.sock");
                }

            // LEFT: Seek -10s
            } else if (input.isButtonJustPressed(Button::LEFT)) {
                sendMpvIpcCommand("{\"command\":[\"seek\",-10,\"relative\"]}", nullptr, "/tmp/mpv_iptv.sock");
                showOverlayIcon("rewind", 1200);
                sendMpvIpcCommand("{\"command\":[\"show-text\",\"{\\\\an2\\\\fs44\\\\bord2\\\\b1}◀◀  -10s\", 1200]}", nullptr, "/tmp/mpv_iptv.sock");

            // RIGHT: Seek +10s
            } else if (input.isButtonJustPressed(Button::RIGHT)) {
                sendMpvIpcCommand("{\"command\":[\"seek\",10,\"relative\"]}", nullptr, "/tmp/mpv_iptv.sock");
                showOverlayIcon("forward", 1200);
                sendMpvIpcCommand("{\"command\":[\"show-text\",\"{\\\\an2\\\\fs44\\\\bord2\\\\b1}▶▶  +10s\", 1200]}", nullptr, "/tmp/mpv_iptv.sock");

            // L1: Nhay 8 kenh khi list hien, Seek -60s khi list an
            } else if (input.isButtonJustPressed(Button::L1)) {
                if (channelListVisible && !m_iptvChannelList.empty()) {
                    m_iptvSelectedIndex = (m_iptvSelectedIndex >= 8) ? m_iptvSelectedIndex - 8 : 0;
                    showIPTVChannelOSD(m_iptvChannelList, (int)m_iptvSelectedIndex, "", 0);
                } else {
                    sendMpvIpcCommand("{\"command\":[\"seek\",-60,\"relative\"]}", nullptr, "/tmp/mpv_iptv.sock");
                    showOverlayIcon("rewind", 1400);
                    sendMpvIpcCommand("{\"command\":[\"show-text\",\"{\\\\an2\\\\fs44\\\\bord2\\\\b1}◀◀  -60s\", 1400]}", nullptr, "/tmp/mpv_iptv.sock");
                }

            // R1: Nhay 8 kenh khi list hien, Seek +60s khi list an
            } else if (input.isButtonJustPressed(Button::R1)) {
                if (channelListVisible && !m_iptvChannelList.empty()) {
                    m_iptvSelectedIndex = std::min(m_iptvChannelList.size() - 1, m_iptvSelectedIndex + 8);
                    showIPTVChannelOSD(m_iptvChannelList, (int)m_iptvSelectedIndex, "", 0);
                } else {
                    sendMpvIpcCommand("{\"command\":[\"seek\",60,\"relative\"]}", nullptr, "/tmp/mpv_iptv.sock");
                    showOverlayIcon("forward", 1400);
                    sendMpvIpcCommand("{\"command\":[\"show-text\",\"{\\\\an2\\\\fs44\\\\bord2\\\\b1}▶▶  +60s\", 1400]}", nullptr, "/tmp/mpv_iptv.sock");
                }

            // UP: Chuyen highlight khi list hien (hoac tu mo OSD day video len tren)
            } else if (input.isButtonJustPressed(Button::UP)) {
                if (!channelListVisible) {
                    channelListVisible = true;
                    hideIPTVPlaybackFooter();  // Ẩn footer khi hiện OSD
                    sendMpvIpcCommand("{\"command\":[\"set_property\",\"video-align-y\",-1]}", nullptr, "/tmp/mpv_iptv.sock");
                }
                if (!m_iptvChannelList.empty()) {
                    m_iptvSelectedIndex = (m_iptvSelectedIndex > 0) ? m_iptvSelectedIndex - 1 : m_iptvChannelList.size() - 1;
                    showIPTVChannelOSD(m_iptvChannelList, (int)m_iptvSelectedIndex, "", 0);
                }

            // DOWN: Chuyen highlight khi list hien (hoac tu mo OSD day video len tren)
            } else if (input.isButtonJustPressed(Button::DOWN)) {
                if (!channelListVisible) {
                    channelListVisible = true;
                    hideIPTVPlaybackFooter();  // Ẩn footer khi hiện OSD
                    sendMpvIpcCommand("{\"command\":[\"set_property\",\"video-align-y\",-1]}", nullptr, "/tmp/mpv_iptv.sock");
                }
                if (!m_iptvChannelList.empty()) {
                    m_iptvSelectedIndex = (m_iptvSelectedIndex + 1 < m_iptvChannelList.size()) ? m_iptvSelectedIndex + 1 : 0;
                    showIPTVChannelOSD(m_iptvChannelList, (int)m_iptvSelectedIndex, "", 0);
                }

            // X: Aspect ratio
            } else if (input.isButtonJustPressed(Button::X)) {
                sendMpvIpcCommand("{\"command\":[\"cycle-values\",\"video-aspect-override\",\"16:9\",\"4:3\",\"-1\"]}", nullptr, "/tmp/mpv_iptv.sock");
                sendMpvIpcCommand("{\"command\":[\"show-text\",\"{\\\\an5\\\\fs75\\\\bord3\\\\b1}Tỉ lệ màn hình\", 1400]}", nullptr, "/tmp/mpv_iptv.sock");

            // Y: Subtitles
            } else if (input.isButtonJustPressed(Button::Y)) {
                sendMpvIpcCommand("{\"command\":[\"cycle\",\"sub\"]}", nullptr, "/tmp/mpv_iptv.sock");
                sendMpvIpcCommand("{\"command\":[\"show-text\",\"{\\\\an5\\\\fs75\\\\bord3\\\\b1}Phụ đề (CC)\", 1400]}", nullptr, "/tmp/mpv_iptv.sock");

            // START: Playback speed
            } else if (input.isButtonJustPressed(Button::START)) {
                sendMpvIpcCommand("{\"command\":[\"cycle-values\",\"speed\",\"1.0\",\"1.25\",\"1.5\",\"0.75\"]}", nullptr, "/tmp/mpv_iptv.sock");
                sendMpvIpcCommand("{\"command\":[\"show-text\",\"{\\\\an5\\\\fs80\\\\bord3\\\\b1}Tốc độ phát\", 1400]}", nullptr, "/tmp/mpv_iptv.sock");

            // SELECT: Toggle channel list OSD (day video len tren / dua ve center)
            } else if (input.isButtonJustPressed(Button::SELECT)) {
                channelListVisible = !channelListVisible;
                if (channelListVisible) {
                    hideIPTVPlaybackFooter();  // Ẩn footer khi hiện OSD
                    sendMpvIpcCommand("{\"command\":[\"set_property\",\"video-align-y\",-1]}", nullptr, "/tmp/mpv_iptv.sock");
                    showIPTVChannelOSD(m_iptvChannelList, (int)m_iptvSelectedIndex, "", 0);
                } else {
                    sendMpvIpcCommand("{\"command\":[\"overlay-remove\",1]}", nullptr, "/tmp/mpv_iptv.sock");
                    sendMpvIpcCommand("{\"command\":[\"set_property\",\"video-align-y\",0]}", nullptr, "/tmp/mpv_iptv.sock");
                    showIPTVPlaybackFooter(isPaused);  // Hiện lại footer
                }
            }

            SDL_Delay(35);
        }

        if (m_overlayExpireTime > 0) {
            sendMpvIpcCommand("{\"command\":[\"overlay-remove\",0]}", nullptr, "/tmp/mpv_iptv.sock");
            m_overlayExpireTime = 0;
        }

        unlink("/tmp/stay_awake");
        unlink("/tmp/mpv_iptv.sock");
        m_isPlaying = false;
        m_currentChannel = "";
        m_iptvChannelList.clear();

        SDL_PumpEvents();
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
        InputManager::instance().reset();
        Logger::info("IPTV: player finished");
        return true;
}

bool IPTVManager::switchYouTubeQuality(const std::string& videoId, const std::string& targetQuality) {
    if (!m_isPlaying || !MpvPlayer::instance().isPlaying()) return false;

    // Resolve new stream URL (ưu tiên cache từ auto-upgrade ngầm)
    std::string newUrl = fetchYouTubeStreamUrl(videoId, targetQuality);
    if (newUrl.empty()) {
        sendMpvIpcCommand("{\"command\":[\"show-text\",\"Không thể lấy luồng " + targetQuality + "p\",3000]}");
        return false;
    }
    return switchYouTubeStream(videoId, newUrl, targetQuality);
}

bool IPTVManager::switchYouTubeStream(const std::string& videoId, const std::string& newUrl, const std::string& targetQuality) {
    if (!m_isPlaying || !MpvPlayer::instance().isPlaying()) return false;
    (void)videoId;
    sendMpvIpcCommand("{\"command\":[\"show-text\",\"Đang đổi sang " + targetQuality + "p...\",5000]}");

    // Query current time position from MPV
    std::string resp;
    double timePos = 0.0;
    if (sendMpvIpcCommand("{\"command\":[\"get_property\",\"time-pos\"]}", &resp)) {
        size_t p = resp.find("\"data\":");
        if (p != std::string::npos) {
            timePos = std::atof(resp.c_str() + p + 7);
        }
    }

    std::string videoUrl = newUrl;
    std::string audioUrl;
    size_t pipePos = newUrl.find('|');
    if (pipePos != std::string::npos) {
        videoUrl = newUrl.substr(0, pipePos);
        audioUrl = newUrl.substr(pipePos + 1);
    }

    char reloadCmd[2048];
    snprintf(reloadCmd, sizeof(reloadCmd),
        "{\"command\":[\"loadfile\",\"%s\",\"replace\",\"start=%.2f\"]}",
        videoUrl.c_str(), timePos);
    sendMpvIpcCommand(reloadCmd);

    if (!audioUrl.empty()) {
        // Đợi file mới active rồi mới audio-add: gửi liền là rớt vào file
        // cũ/file chưa mở xong -> mất tiếng (bug đã tái hiện).
        std::string probe = videoUrl.substr(0, 80);
        bool active = false;
        for (int i = 0; i < 50; ++i) {
            SDL_Delay(200);
            if (!m_isPlaying || !MpvPlayer::instance().isPlaying()) break;
            std::string pr;
            if (sendMpvIpcCommand("{\"command\":[\"get_property\",\"path\"]}", &pr) &&
                pr.find(probe) != std::string::npos) {
                active = true;
                break;
            }
        }
        if (active && m_isPlaying && MpvPlayer::instance().isPlaying()) {
            char audioCmd[2048];
            snprintf(audioCmd, sizeof(audioCmd),
                "{\"command\":[\"audio-add\",\"%s\",\"select\"]}", audioUrl.c_str());
            sendMpvIpcCommand(audioCmd);
        }
    }

    sendMpvIpcCommand("{\"command\":[\"show-text\",\"Độ phân giải: " + targetQuality + "p\",3000]}");
    return true;
}

bool IPTVManager::playYouTubeUrl(const std::string& url) {
    return playYouTubeVideo("", url, "720");
}

double IPTVManager::ytTimePos() {
    return ytMpvNumber("time-pos");
}

int IPTVManager::ytMeasuredW() {
    double v = ytMpvNumber("video-params/w");
    return v > 0 ? (int)v : 0;
}

int IPTVManager::ytMeasuredH() {
    double v = ytMpvNumber("video-params/h");
    return v > 0 ? (int)v : 0;
}

double IPTVManager::ytMpvNumber(const std::string& prop) {
    std::string resp;
    std::string cmd = "{\"command\":[\"get_property\",\"" + prop + "\"]}";
    if (!sendMpvIpcCommand(cmd, &resp))
        return -1;
    size_t p = resp.find("\"data\":");
    if (p == std::string::npos || p + 7 >= resp.size()) return -1;
    if (resp[p + 7] == 'n') return -1; // null
    return atof(resp.c_str() + p + 7);
}

double IPTVManager::ytDuration() {    if (m_ytDuration >= 0) return m_ytDuration;
    std::string resp;
    if (sendMpvIpcCommand("{\"command\":[\"get_property\",\"duration\"]}", &resp)) {
        size_t p = resp.find("\"data\":");
        if (p != std::string::npos && p + 7 < resp.size() && resp[p + 7] != 'n')
            m_ytDuration = atof(resp.c_str() + p + 7);
    }
    return m_ytDuration;
}

double IPTVManager::ytCacheAhead() {
    // Số giây video đã load sẵn phía trước (xám buffer sau thanh đỏ)
    std::string resp;
    if (!sendMpvIpcCommand("{\"command\":[\"get_property\",\"demuxer-cache-time\"]}", &resp))
        return -1;
    size_t p = resp.find("\"data\":");
    if (p == std::string::npos || p + 7 >= resp.size()) return -1;
    if (resp[p + 7] == 'n') return -1;
    return atof(resp.c_str() + p + 7);
}

void IPTVManager::showYouTubeOSD(bool extendExpire) {
    std::string appRoot = AppConfig::instance().getAppRoot();
    if (appRoot.empty()) appRoot = "/mnt/SDCARD/Apps/RomCloud";
    static TTF_Font* s_fTitle = nullptr;
    static TTF_Font* s_fBar = nullptr;
    TTF_Font* fTitle = ytOsdFont(appRoot, 24, s_fTitle);
    TTF_Font* fBar = ytOsdFont(appRoot, 22, s_fBar);

    double cur = ytTimePos();
    double dur = ytDuration();
    double cached = ytCacheAhead();

    // --- Top bar 1024x64: tên video + chất lượng ---
    {
        const int CW = 1024, CH = 64, STRIDE = CW * 4;
        std::vector<uint8_t> cv(STRIDE * CH, 0);
        ytFillRect(cv, CW, CH, 0, 0, CW, CH, 0, 0, 0, 140);
        std::string title = m_ytTitle.empty() ? "YouTube" : m_ytTitle;
        ytBlitText(cv, CW, CH, fTitle, ytTruncUtf8(fTitle, title, 780),
                   {255, 255, 255, 255}, 24, 18);
        // Số đo thật từ mpv (video-params); chưa có thì rớt về nhãn yêu cầu.
        std::string q;
        {
            int mw = ytMeasuredW(), mh = ytMeasuredH();
            if (mw > 0 && mh > 0)
                q = std::to_string(mw) + "x" + std::to_string(mh);
            else
                q = m_ytQuality + "p";
        }
        int qw = 0, qh = 0;
        if (fBar && TTF_SizeUTF8(fBar, q.c_str(), &qw, &qh) == 0)
            ytBlitText(cv, CW, CH, fBar, q, {0, 180, 216, 255}, 1000 - qw, 20);
        FILE* fp = fopen("/tmp/yt_osd_top.raw", "wb");
        if (fp) { fwrite(cv.data(), 1, cv.size(), fp); fclose(fp); }
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
            "{\"command\":[\"overlay-add\",1,0,0,\"/tmp/yt_osd_top.raw\",0,\"bgra\",%d,%d,%d]}\n",
            CW, CH, STRIDE);
        sendMpvIpcCommand(cmd);
    }

    // --- Bottom bar 1024x120 @y=648: tiến trình + giờ + gợi ý nút ---
    {
        const int CW = 1024, CH = 120, OY = 648, STRIDE = CW * 4;
        std::vector<uint8_t> cv(STRIDE * CH, 0);
        ytFillRect(cv, CW, CH, 0, 0, CW, CH, 0, 0, 0, 140);
        double frac = (dur > 0 && cur >= 0) ? cur / dur : 0;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        const int barX = 24, barW = 976, barY = 18, barH = 8;
        ytFillRect(cv, CW, CH, barX, barY, barW, barH, 90, 90, 90, 255);
        // Lớp xám: mức video đã load sẵn (giống YouTube)
        double bufFrac = (dur > 0 && cur >= 0 && cached > 0)
            ? (cur + cached) / dur : frac;
        if (bufFrac < 0) bufFrac = 0;
        if (bufFrac > 1) bufFrac = 1;
        int bufW = (int)(barW * bufFrac);
        if (bufW > 0)
            ytFillRect(cv, CW, CH, barX, barY, bufW, barH, 170, 170, 170, 255);
        int fillW = (int)(barW * frac);
        if (fillW > 0)
            ytFillRect(cv, CW, CH, barX, barY, fillW, barH, 255, 0, 0, 255);
        ytFillCircle(cv, CW, CH, barX + fillW, barY + barH / 2, 7,
                     255, 255, 255, 255);
        std::string t = ytFmtTime(cur) + " / " + (dur >= 0 ? ytFmtTime(dur) : "--:--");
        ytBlitText(cv, CW, CH, fBar, t, {255, 255, 255, 255}, 24, 42);
        std::string hints = "A: Tạm dừng   \u2190 \u2192 +-10s   SELECT: 720/360   B: Thoát";
        int hintW = 0, hintH = 0;
        if (fBar && TTF_SizeUTF8(fBar, hints.c_str(), &hintW, &hintH) == 0)
            ytBlitText(cv, CW, CH, fBar, hints,
                       {170, 180, 195, 255}, (CW - hintW) / 2, 76);
        FILE* fp = fopen("/tmp/yt_osd_bot.raw", "wb");
        if (fp) { fwrite(cv.data(), 1, cv.size(), fp); fclose(fp); }
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
            "{\"command\":[\"overlay-add\",2,0,%d,\"/tmp/yt_osd_bot.raw\",0,\"bgra\",%d,%d,%d]}\n",
            OY, CW, CH, STRIDE);
        sendMpvIpcCommand(cmd);
    }

    m_ytOsdOn = true;
    // Refresh tiến trình mỗi giây KHÔNG gia hạn (không thì OSD dính luôn).
    // Chỉ thao tác của user (gọi mặc định) mới gia hạn thêm 3s.
    if (extendExpire) m_ytOsdExpire = SDL_GetTicks() + 3000;
    m_ytOsdLastRefresh = SDL_GetTicks();
}

void IPTVManager::hideYouTubeOSD() {
    if (!m_ytOsdOn) return;
    m_ytOsdOn = false;
    sendMpvIpcCommand("{\"command\":[\"overlay-remove\",1]}");
    sendMpvIpcCommand("{\"command\":[\"overlay-remove\",2]}");
}

void IPTVManager::flashCenter(const std::string& icon, const std::string& textCmd) {
    // Icon PNG nếu user đã thêm file (decode lúc flash); chưa có thì chữ.
    std::string appRoot = AppConfig::instance().getAppRoot();
    if (appRoot.empty()) appRoot = "/mnt/SDCARD/Apps/RomCloud";
    if (!icon.empty() &&
        access((appRoot + "/assets/player_icons/" + icon + ".png").c_str(), R_OK) == 0) {
        showOverlayIcon(icon, 1400);
        return;
    }
    sendMpvIpcCommand(textCmd);
}

bool IPTVManager::playYouTubeVideo(const std::string& videoId, const std::string& initialUrl, const std::string& quality, const std::string& title) {
    stop();

    if (initialUrl.empty()) {
        Logger::error("YouTube URL is empty");
        return false;
    }

    ensureMediaPlayerAvailable();

    Logger::info("Playing YouTube Video: " + videoId + " (quality=" + quality + ")");

    // P1-3: tach video/audio (pipe), dat YTDL_EXE roi fork qua MpvPlayer.
    std::string appRoot = AppConfig::instance().getAppRoot();
    if (appRoot.empty()) appRoot = "/mnt/SDCARD/Apps/RomCloud";
    setenv("YTDL_EXE", (appRoot + "/bin/yt-dlp").c_str(), 1);
    std::string videoUrl = initialUrl, audioUrl;
    { size_t pp = initialUrl.find('|');
      if (pp != std::string::npos) { videoUrl = initialUrl.substr(0, pp); audioUrl = initialUrl.substr(pp + 1); } }
    std::vector<std::string> ytExtra = {
        "--vd-lavc-fast",
        "--vd-lavc-skiploopfilter=nonref", "--vd-lavc-framedrop=nonref",
        "--sws-scaler=fast-bilinear", "--dscale=bilinear", "--scale=bilinear",
        "--framedrop=vo", "--demuxer-max-bytes=24M", "--demuxer-readahead-secs=8",
        "--audio-buffer=0.5", "--osd-level=1", "--osd-font-size=48",
        "--osd-align-x=center", "--osd-align-y=center", "--osd-color=#FFFFFF",
        "--osd-border-color=#10141E", "--osd-border-size=3", "--osd-duration=1400" };
    if (!audioUrl.empty()) ytExtra.push_back("--audio-file=" + audioUrl);
    if (!MpvPlayer::instance().play(videoUrl, ytExtra, "/tmp/mpv_youtube.sock",
                                    appRoot + "/youtube_mpv.log")) {
        Logger::error("No media player found for YouTube playback");
        return false;
    }
    Logger::info("YouTube player started");
    // P1-3: khoi fork truc tiep; control loop ben duoi giu nguyen.
    pid_t pid = MpvPlayer::instance().pid();
        m_isPlaying = true;
        m_currentChannel = "YouTube";
        // YouTube OSD (UI-only): giữ tiêu đề + reset trạng thái thanh điều khiển
        m_ytTitle = title;
        m_ytQuality = quality.empty() ? "720" : quality;
        m_ytDuration = -1.0;
        m_ytOsdOn = false;
        m_ytOsdExpire = 0;
        uint32_t playStartTime = SDL_GetTicks();
        std::string currentQuality = quality.empty() ? "720" : quality;
        bool isPaused = false;
        m_overlayExpireTime = 0;
        Logger::info("YouTube player started with PID: " + std::to_string(pid));

        SDL_PumpEvents();
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);

        while (m_isPlaying && MpvPlayer::instance().isPlaying()) {
            if (MpvPlayer::instance().pollExited()) break;

            if (m_overlayExpireTime > 0 && SDL_GetTicks() >= m_overlayExpireTime) {
                sendMpvIpcCommand("{\"command\":[\"overlay-remove\",0]}");
                m_overlayExpireTime = 0;
            }

            InputManager::instance().update();
            auto& input = InputManager::instance();

            if (SDL_GetTicks() - playStartTime >= 600) {
                if (input.isButtonJustPressed(Button::B) || input.isButtonJustPressed(Button::MENU)) {
                    stop();
                    break;
                } else if (input.isButtonJustPressed(Button::A)) {
                    isPaused = !isPaused;
                    sendMpvIpcCommand("{\"command\":[\"cycle\",\"pause\"]}");
                    showOverlayIcon(isPaused ? "pause" : "play", 1400);
                    sendMpvIpcCommand(isPaused
                        ? "{\"command\":[\"show-text\",\"{\\\\an2\\\\fs44\\\\bord2\\\\b1}❚❚  TẠM DỪNG\", 1400]}"
                        : "{\"command\":[\"show-text\",\"{\\\\an2\\\\fs44\\\\bord2\\\\b1}▶  ĐANG PHÁT\", 1400]}");
                    showYouTubeOSD();
                } else if (input.isButtonJustPressed(Button::RIGHT)) {
                    sendMpvIpcCommand("{\"command\":[\"seek\",10,\"relative\"]}");
                    showOverlayIcon("forward", 1200);
                    sendMpvIpcCommand("{\"command\":[\"show-text\",\"{\\\\an2\\\\fs44\\\\bord2\\\\b1}▶▶  +10s\", 1200]}");
                    showYouTubeOSD();
                } else if (input.isButtonJustPressed(Button::LEFT)) {
                    sendMpvIpcCommand("{\"command\":[\"seek\",-10,\"relative\"]}");
                    showOverlayIcon("rewind", 1200);
                    sendMpvIpcCommand("{\"command\":[\"show-text\",\"{\\\\an2\\\\fs44\\\\bord2\\\\b1}◀◀  -10s\", 1200]}");
                    showYouTubeOSD();
                } else if (input.isButtonJustPressed(Button::UP)) {
                    sendMpvIpcCommand("{\"command\":[\"add\",\"volume\",5]}");
                    showOverlayIcon("vol_up", 1200);
                    sendMpvIpcCommand("{\"command\":[\"show-text\",\"{\\\\an5\\\\fs70\\\\bord3\\\\b1}▲  Âm lượng +5%\", 1200]}");
                    showYouTubeOSD();
                } else if (input.isButtonJustPressed(Button::DOWN)) {
                    sendMpvIpcCommand("{\"command\":[\"add\",\"volume\",-5]}");
                    showOverlayIcon("vol_down", 1200);
                    sendMpvIpcCommand("{\"command\":[\"show-text\",\"{\\\\an5\\\\fs70\\\\bord3\\\\b1}▼  Âm lượng -5%\", 1200]}");
                    showYouTubeOSD();
                } else if (input.isButtonJustPressed(Button::R1)) {
                    sendMpvIpcCommand("{\"command\":[\"seek\",60,\"relative\"]}");
                    showOverlayIcon("forward", 1400);
                    sendMpvIpcCommand("{\"command\":[\"show-text\",\"{\\\\an2\\\\fs44\\\\bord2\\\\b1}▶▶  +60s\", 1400]}");
                    showYouTubeOSD();
                } else if (input.isButtonJustPressed(Button::L1)) {
                    sendMpvIpcCommand("{\"command\":[\"seek\",-60,\"relative\"]}");
                    showOverlayIcon("rewind", 1400);
                    sendMpvIpcCommand("{\"command\":[\"show-text\",\"{\\\\an2\\\\fs44\\\\bord2\\\\b1}◀◀  -60s\", 1400]}");
                    showYouTubeOSD();
                } else if (input.isButtonJustPressed(Button::X)) {
                    sendMpvIpcCommand("{\"command\":[\"cycle-values\",\"video-aspect-override\",\"16:9\",\"4:3\",\"-1\"]}");
                    flashCenter("aspect", "{\"command\":[\"show-text\",\"{\\\\an5\\\\fs75\\\\bord3\\\\b1}Tỉ lệ màn hình\", 1400]}");
                    showYouTubeOSD();
                } else if (input.isButtonJustPressed(Button::Y)) {
                    sendMpvIpcCommand("{\"command\":[\"cycle\",\"sub\"]}");
                    flashCenter("cc", "{\"command\":[\"show-text\",\"{\\\\an5\\\\fs75\\\\bord3\\\\b1}Phụ đề (CC)\", 1400]}");
                    showYouTubeOSD();
                } else if (input.isButtonJustPressed(Button::START)) {
                    sendMpvIpcCommand("{\"command\":[\"cycle-values\",\"speed\",\"1.0\",\"1.25\",\"1.5\",\"0.75\"]}");
                    flashCenter("speed", "{\"command\":[\"show-text\",\"{\\\\an5\\\\fs80\\\\bord3\\\\b1}Tốc độ phát\", 1400]}");
                    showYouTubeOSD();
                } else if (input.isButtonJustPressed(Button::SELECT)) {
                    if (!videoId.empty()) {
                        std::string nextQ = (currentQuality == "720") ? "360" : "720";
                        showOverlayIcon("quality", 1400);
                        sendMpvIpcCommand("{\"command\":[\"show-text\",\"{\\\\an5\\\\fs75\\\\bord3\\\\b1}Đổi chất lượng: " + nextQ + "p...\", 1400]}");
                        if (switchYouTubeQuality(videoId, nextQ)) {
                            currentQuality = nextQ;
                            m_ytQuality = currentQuality;
                        }
                    }
                }
            }
            // YouTube OSD tick (UI-only): tu an sau 3s, ve lai tien trinh 1s/lan
            if (m_ytOsdOn) {
                uint32_t nowOsd = SDL_GetTicks();
                if (nowOsd >= m_ytOsdExpire) {
                    hideYouTubeOSD();
                } else if (nowOsd - m_ytOsdLastRefresh >= 1000) {
                    showYouTubeOSD(false); // refresh, không gia hạn
                }
            }
            SDL_Delay(35);
        }

        if (m_overlayExpireTime > 0) {
            sendMpvIpcCommand("{\"command\":[\"overlay-remove\",0]}");
            m_overlayExpireTime = 0;
        }
        hideYouTubeOSD();

        unlink("/tmp/stay_awake");
        unlink("/tmp/mpv_youtube.sock");
        m_isPlaying = false;
        m_currentChannel = "";

        SDL_PumpEvents();
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
        InputManager::instance().reset();
        Logger::info("YouTube player finished");
        return true;
}

bool IPTVManager::stop() {
    // P1-3: quit ca sock cu (tuong thich), MpvPlayer reap PID hien tai.
    sendMpvIpcCommand("{\"command\":[\"quit\"]}", nullptr, "/tmp/mpv_iptv.sock");
    sendMpvIpcCommand("{\"command\":[\"quit\"]}", nullptr, "/tmp/mpv_youtube.sock");
    MpvPlayer::instance().stop();

    unlink("/tmp/mpv_iptv.sock");
    unlink("/tmp/mpv_youtube.sock");
    unlink("/tmp/stay_awake");
    m_isPlaying = false;
    m_currentChannel = "";
    m_ytPreActive = false; // hủy prefetch 720p dở dang (nếu có)
    m_ytPreVid.clear();
    m_ytPreVideo.clear();
    m_ytPreAudio.clear();

#ifdef __GLIBC__
    malloc_trim(0);
#endif
#ifndef PC_SIMULATOR_MODE
    // Dừng phát mà ở lại list: vẫn thả pagecache video cho nhẹ RAM.
    FILE* f = fopen("/proc/sys/vm/drop_caches", "w");
    if (f) {
        fputs("1\n", f);
        fclose(f);
    }
#endif

    return true;
}

void IPTVManager::createDefaultPlaylist(const std::string& filepath) {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        Logger::error("Cannot create default playlist: " + filepath);
        return;
    }

    file << "#EXTM3U\n"
         << "#EXTINF:-1 tvg-id=\"CanThoTV.vn@SD\" tvg-name=\"Cần Thơ TV\" group-title=\"Miền Tây\",Cần Thơ TV (HD)\n"
         << "https://live.canthotv.vn/live/tv/chunklist.m3u8\n"
         << "#EXTINF:-1 tvg-id=\"CanThoTV2.vn@SD\" tvg-name=\"Cần Thơ TV 2\" group-title=\"Miền Tây\",Cần Thơ TV 2 (HD)\n"
         << "https://live.canthotv.vn/cs2/live.stream/playlist.m3u8\n"
         << "#EXTINF:-1 tvg-id=\"DongNaiTV1.vn@SD\" tvg-name=\"Đồng Nai 1\" group-title=\"Đông Nam Bộ\",Đồng Nai 1 (HD)\n"
         << "https://vtvgolive-ott3.vtvdigital.vn/live/dongnai1tv/chunklist_2.m3u8\n"
         << "#EXTINF:-1 tvg-id=\"DongNaiTV2.vn@SD\" tvg-name=\"Đồng Nai 2\" group-title=\"Đông Nam Bộ\",Đồng Nai 2 (HD)\n"
         << "https://vtvgolive-ott3.vtvdigital.vn/live/dongnai2tv/chunklist_2.m3u8\n"
         << "#EXTINF:-1 tvg-id=\"DongNaiTV3.vn@SD\" tvg-name=\"Đồng Nai 3\" group-title=\"Đông Nam Bộ\",Đồng Nai 3 (720p)\n"
         << "https://dethich.pw/dongnai3/index.m3u8\n"
         << "#EXTINF:-1 tvg-id=\"AnNinhTV.vn@HD\" tvg-name=\"An Ninh TV\" group-title=\"Thời Sự\",An Ninh TV HD (1080p)\n"
         << "https://liveh12.vtvprime.vn/hls/ANNINHTV/index.m3u8\n"
         << "#EXTINF:-1 tvg-id=\"CaoBangTV.vn@SD\" tvg-name=\"Cao Bằng TV\" group-title=\"Miền Bắc\",Cao Bằng TV (HD)\n"
         << "https://stream.thingnet.vn/live/smil:CRTV.smil/chunklist.m3u8\n"
         << "#EXTINF:-1 tvg-id=\"DienBienTV.vn@SD\" tvg-name=\"Điện Biên TV\" group-title=\"Miền Bắc\",Điện Biên TV (1080p)\n"
         << "https://stream.langsontv.vn/live/2855dfeccb7f49a41a2b0441b3bfeda413c/playlist.m3u8\n"
         << "#EXTINF:-1 tvg-id=\"HaTinhTV.vn@SD\" tvg-name=\"Hà Tĩnh TV\" group-title=\"Miền Trung\",Hà Tĩnh TV (720p)\n"
         << "https://cohauw9bgpvod.vcdn.cloud/httv1/index.m3u8\n"
         << "#EXTINF:-1 tvg-id=\"HTV1.vn@SD\" tvg-name=\"HTV1\" group-title=\"HTV TP.HCM\",HTV1 (720p)\n"
         << "https://dethich.pw/htv1/index.m3u8\n"
         << "#EXTINF:-1 tvg-id=\"HTV2.vn@SD\" tvg-name=\"HTV2\" group-title=\"HTV TP.HCM\",HTV2 Vie Channel (720p)\n"
         << "https://dethich.pw/htv2/index.m3u8\n"
         << "#EXTINF:-1 tvg-id=\"HTV3.vn@SD\" tvg-name=\"HTV3\" group-title=\"HTV TP.HCM\",HTV3 DreamsTV (720p)\n"
         << "https://dethich.pw/htv3/index.m3u8\n"
         << "#EXTINF:-1 tvg-id=\"HTV7.vn@SD\" tvg-name=\"HTV7\" group-title=\"HTV TP.HCM\",HTV7 (720p)\n"
         << "https://dethich.pw/htv7/index.m3u8\n"
         << "#EXTINF:-1 tvg-id=\"HTV9.vn@SD\" tvg-name=\"HTV9\" group-title=\"HTV TP.HCM\",HTV9 (720p)\n"
         << "https://dethich.pw/htv9/index.m3u8\n"
         << "#EXTINF:-1 tvg-id=\"HTVSports.vn@SD\" tvg-name=\"HTV Thể Thao\" group-title=\"Thể Thao\",HTV Thể Thao (HD)\n"
         << "https://dethich.pw/htvthethao/index.m3u8\n"
         << "#EXTINF:-1 tvg-id=\"DongThapTV.vn@SD\" tvg-name=\"Đồng Tháp TV\" group-title=\"Miền Tây\",Đồng Tháp TV (720p)\n"
         << "https://liveh34.vtvprime.vn/hls/DONGTHAPTV/index.m3u8\n"
         << "#EXTINF:-1 tvg-id=\"BBB\" tvg-name=\"Big Buck Bunny\" group-title=\"Phim & Test\",Big Buck Bunny (1080p 60fps)\n"
         << "https://test-streams.mux.dev/outcasts/index.m3u8\n"
         << "#EXTINF:-1 tvg-id=\"TOS\" tvg-name=\"Tears of Steel\" group-title=\"Phim & Test\",Tears of Steel (1080p FHD)\n"
         << "https://bitdash-a.akamaihd.net/content/sintel/hls/playlist.m3u8\n";

    file.close();
    Logger::info("Created default playlist: " + filepath);
}

static std::mutex s_pingMutex;

int IPTVManager::getCachedPing(const std::string &url) {
    std::lock_guard<std::mutex> lk(s_pingMutex);
    auto it = m_pingCache.find(url);
    if (it != m_pingCache.end()) return it->second;
    if (m_pingInFlight.count(url)) return -2;
    return -2;
}

void IPTVManager::clearPingCache() {
    std::lock_guard<std::mutex> lk(s_pingMutex);
    m_pingCache.clear();
    m_pingInFlight.clear();
}

void IPTVManager::prefetchPings(const std::vector<std::string> &urls) {
    {
        std::lock_guard<std::mutex> lk(s_pingMutex);
        // Lọc URL chưa đo; hàng đợi chỉ giữ cửa sổ mới nhất (ghi đè).
        m_pingPending.clear();
        for (const auto &url : urls) {
            if (url.empty() || m_pingCache.count(url) || m_pingInFlight.count(url))
                continue;
            m_pingPending.push_back(url);
        }
        if (m_pingPending.empty())
            return;
        if (m_pingWorkerActive.load())
            return; // worker đang chạy sẽ lấy hàng đợi mới khi xong batch
        m_pingWorkerActive.store(true);
    }

    try {
        std::thread([this]() {
            for (;;) {
                std::vector<std::string> batch;
                {
                    std::lock_guard<std::mutex> lk(s_pingMutex);
                    if (m_pingPending.empty()) {
                        m_pingWorkerActive.store(false);
                        return;
                    }
                    batch = std::move(m_pingPending);
                    m_pingPending.clear();
                    for (const auto &u : batch)
                        m_pingInFlight.insert(u);
                }
                for (const auto &url : batch) {
                    uint32_t t0 = SDL_GetTicks();
                    std::string cmd = "curl -k -s -L -I -m 2 -o /dev/null -w \"%{http_code}\" \"" + url + "\" 2>/dev/null";
                    FILE *fp = popen(cmd.c_str(), "r");
                    int code = 0;
                    if (fp) {
                        char buf[32];
                        if (fgets(buf, sizeof(buf), fp)) code = atoi(buf);
                        pclose(fp);
                    }
                    uint32_t t1 = SDL_GetTicks();
                    int ms = (code >= 200 && code < 400) ? static_cast<int>(t1 - t0) : -1;
                    {
                        std::lock_guard<std::mutex> lk(s_pingMutex);
                        m_pingCache[url] = ms;
                        m_pingInFlight.erase(url);
                    }
                }
            }
        }).detach();
    } catch (const std::exception &e) {
        // Hết tài nguyên tạo thread: trả lại hàng đợi, log, không crash.
        std::lock_guard<std::mutex> lk(s_pingMutex);
        m_pingPending.clear();
        m_pingInFlight.clear();
        m_pingWorkerActive.store(false);
        Logger::warn(std::string("prefetchPings: cannot spawn worker: ") + e.what());
    }
}

void IPTVManager::showIPTVVideoFooter(bool isPaused) {
    std::string text = isPaused ? "❚❚  TẠM DỪNG" : "▶  ĐANG PHÁT";
    if (!m_currentChannel.empty()) text = m_currentChannel + "  •  " + text;
    sendMpvIpcCommand("{\"command\":[\"show-text\",\"{\\\\an2\\\\fs40\\\\bord2\\\\b1}" + text + "\", 2000]}", nullptr, "/tmp/mpv_iptv.sock");
}

void IPTVManager::hideIPTVVideoFooter() {
    sendMpvIpcCommand("{\"command\":[\"show-text\",\"\", 0]}", nullptr, "/tmp/mpv_iptv.sock");
}

} // namespace RomCloud
