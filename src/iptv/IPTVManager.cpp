#include "IPTVManager.h"
#include "../logging/Logger.h"
#include "../filesystem/FileSystemManager.h"
#include "../config/AppConfig.h"
#include "../input/InputManager.h"
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <curl/curl.h>

#include <SDL2/SDL.h>

namespace RomCloud {

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
    if (!dir) {
        return false;
    }

    int loaded = 0;
    struct dirent* entry;
    std::vector<std::string> playlistFiles;
    while ((entry = readdir(dir)) != nullptr) {
        std::string filename = entry->d_name;
        if (filename.length() >= 4) {
            std::string ext = "";
            size_t dot = filename.rfind('.');
            if (dot != std::string::npos) {
                ext = filename.substr(dot);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            }
            if (ext == ".m3u" || ext == ".m3u8") {
                playlistFiles.push_back(filename);
            }
        }
    }
    closedir(dir);

    std::sort(playlistFiles.begin(), playlistFiles.end());

    for (const auto& filename : playlistFiles) {
        std::string path = dirPath + "/" + filename;
        std::string sourceName = "";
        auto it = m_sourcesMeta.find(filename);
        if (it != m_sourcesMeta.end() && !it->second.name.empty()) {
            sourceName = it->second.name;
        } else {
            if (filename == "default.m3u") sourceName = "Mặc định";
            else if (filename == "vietnam.m3u") sourceName = "Việt Nam";
            else {
                sourceName = filename;
                size_t dot = sourceName.rfind('.');
                if (dot != std::string::npos) sourceName = sourceName.substr(0, dot);
                std::replace(sourceName.begin(), sourceName.end(), '_', ' ');
            }
            IPTVSource newSrc;
            newSrc.name = sourceName;
            newSrc.filename = filename;
            newSrc.type = "file";
            m_sourcesMeta[filename] = newSrc;
        }

        size_t count = 0;
        if (parseM3UFile(path, sourceName, filename, &count)) {
            IPTVSource src = m_sourcesMeta[filename];
            src.name = sourceName;
            src.filename = filename;
            src.channelCount = count;
            struct stat st;
            if (stat(path.c_str(), &st) == 0) {
                src.fileSize = st.st_size;
            }
            m_sources.push_back(src);
            loaded++;
        }
    }

    saveSourcesMeta();

    Logger::info("Loaded " + std::to_string(m_channels.size()) + " IPTV channels from " +
                 std::to_string(loaded) + " playlist(s)");

    return m_channels.size() > 0;
}

bool IPTVManager::parseM3UFile(const std::string& filepath, const std::string& sourceName, const std::string& filename, size_t* outChannelCount) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        Logger::error("Cannot open M3U file: " + filepath);
        return false;
    }

    std::string line;
    std::string currentGroup = "Chung";
    std::string currentName;
    std::string currentLogo;
    size_t count = 0;

    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) continue;

        if (line.rfind("#EXTINF:", 0) == 0) {
            currentName = extractName(line);
            std::string group = extractGroup(line);
            if (!group.empty()) {
                currentGroup = group;
            }
            currentLogo = extractLogo(line);

            if (currentName.empty()) {
                currentName = "Kênh không tên";
            }
            continue;
        }

        if (line[0] == '#') {
            continue;
        }

        std::string url = line;
        while (!url.empty() && (url.front() == ' ' || url.front() == '\t')) url.erase(0, 1);
        while (!url.empty() && (url.back() == ' ' || url.back() == '\t')) url.pop_back();

        if (!url.empty() && url.find("://") != std::string::npos) {
            IPTVChannel channel;
            channel.name = currentName;
            channel.url = url;
            channel.group = currentGroup;
            channel.logo = currentLogo;
            channel.source = sourceName;
            channel.sourceFile = filename;
            channel.isFavorite = isFavorite(currentName);

            m_channels.push_back(channel);

            size_t idx = m_channels.size() - 1;
            m_groups[currentGroup].push_back(idx);
            count++;
        }
    }

    file.close();
    if (outChannelCount) *outChannelCount = count;
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

        // format: filename|name|type|url
        std::stringstream ss(line);
        std::string fn, name, type, url;
        if (std::getline(ss, fn, '|') && std::getline(ss, name, '|')) {
            std::getline(ss, type, '|');
            std::getline(ss, url, '|');
            IPTVSource src;
            src.filename = fn;
            src.name = name;
            src.type = type.empty() ? "file" : type;
            src.url = url;
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
    file << "# filename|name|type|url\n";
    for (const auto& pair : m_sourcesMeta) {
        file << pair.second.filename << "|"
             << pair.second.name << "|"
             << pair.second.type << "|"
             << pair.second.url << "\n";
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

bool IPTVManager::playChannel(const IPTVChannel& channel) {
    stop();

    if (channel.url.empty()) {
        Logger::error("IPTV channel URL is empty");
        return false;
    }

    ensureMediaPlayerAvailable();

    Logger::info("Playing IPTV channel: " + channel.name);
    Logger::info("URL: " + channel.url);

    std::string appRoot = AppConfig::instance().getAppRoot();
    if (appRoot.empty()) appRoot = "/mnt/SDCARD/Apps/RomCloud";
    std::string sdRoot = AppConfig::instance().getSdRoot();

    std::vector<std::string> playerCandidates = {
        appRoot + "/bin/mpv",
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

    std::string playerPath;
    for (const auto& candidate : playerCandidates) {
        struct stat st;
        if (stat(candidate.c_str(), &st) == 0 && st.st_size > 1000 && access(candidate.c_str(), X_OK) == 0) {
            playerPath = candidate;
            break;
        }
    }

    if (playerPath.empty()) {
        Logger::error("No valid media player (mpv/ffplay) found on system");
        return false;
    }

    Logger::info("Selected IPTV player: " + playerPath);

    // Prevent TrimUI screen standby while watching video
    FILE* fwake = fopen("/tmp/stay_awake", "w");
    if (fwake) {
        fputs("1\n", fwake);
        fclose(fwake);
    }

    pid_t pid = fork();
    if (pid == 0) {
        // Child process: create independent process group
        setpgid(0, 0);

        // Redirect stdout & stderr to iptv_mpv.log for diagnostics
        std::string logPath = appRoot + "/iptv_mpv.log";
        int logFd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logFd >= 0) {
            dup2(logFd, STDOUT_FILENO);
            dup2(logFd, STDERR_FILENO);
            close(logFd);
        }

        // Export library paths so player can find codec libraries (RomCloud lib, SpruceOS Emu/MEDIA lib, system libs)
        std::string libPath = appRoot + "/lib:" + sdRoot + "/Emu/MEDIA/lib64:" + sdRoot + "/Emu/MEDIA/lib32:" + sdRoot + "/System/lib:/usr/lib:/lib";
        setenv("LD_LIBRARY_PATH", libPath.c_str(), 1);
        setenv("HOME", appRoot.c_str(), 1);

        std::string inputConf = appRoot + "/config/input.conf";

        if (playerPath.find("mpv.sh") != std::string::npos) {
            execl("/bin/sh", "sh", playerPath.c_str(), channel.url.c_str(), nullptr);
        } else if (playerPath.find("mpv") != std::string::npos) {
            std::vector<std::string> argList = {
                playerPath,
                channel.url,
                "--fullscreen",
                "--keepaspect=yes",
                "--hwdec=auto",
                "--vd-lavc-threads=4",
                "--framedrop=vo",
                "--demuxer-max-bytes=16M",
                "--demuxer-readahead-secs=5",
                "--audio-buffer=0.5",
                "--terminal=no"
            };
            if (access(inputConf.c_str(), R_OK) == 0) {
                argList.push_back("--input-conf=" + inputConf);
            }
            std::vector<char*> cArgs;
            for (auto& s : argList) {
                cArgs.push_back(const_cast<char*>(s.c_str()));
            }
            cArgs.push_back(nullptr);

            execv(playerPath.c_str(), cArgs.data());
        } else {
            // ffplay
            const char* args[] = {
                playerPath.c_str(),
                "-fs",
                "-autoexit",
                "-loglevel", "warning",
                channel.url.c_str(),
                nullptr
            };
            execvp(playerPath.c_str(), const_cast<char* const*>(args));
        }
        _exit(1);
    } else if (pid > 0) {
        m_mpvPid = pid;
        m_isPlaying = true;
        m_currentChannel = channel.name;
        uint32_t playStartTime = SDL_GetTicks();
        Logger::info("IPTV player started with PID: " + std::to_string(pid) + " using " + playerPath);

        // Responsive loop: wait for player to exit OR user to press B/Menu/Select
        int status = 0;
        while (m_isPlaying && m_mpvPid > 0) {
            pid_t res = waitpid(m_mpvPid, &status, WNOHANG);
            if (res != 0) {
                break;
            }

            SDL_Event ev;
            bool shouldExit = false;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                    // TrimUI physical B button = SDL_CONTROLLER_BUTTON_A (or Menu / Back)
                    if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_A ||
                        ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE ||
                        ev.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
                        Logger::info("User pressed B/Menu button, stopping IPTV player");
                        shouldExit = true;
                        break;
                    }
                } else if (ev.type == SDL_KEYDOWN) {
                    if (ev.key.keysym.sym == SDLK_b || ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_q) {
                        Logger::info("User pressed key B/ESC/Q, stopping IPTV player");
                        shouldExit = true;
                        break;
                    }
                }
            }

            if (shouldExit) {
                stop();
                break;
            }

            SDL_Delay(40);
        }

        // Clean up stay_awake
        unlink("/tmp/stay_awake");

        uint32_t playDuration = SDL_GetTicks() - playStartTime;
        bool playSuccess = true;

        if (WIFEXITED(status)) {
            int exitCode = WEXITSTATUS(status);
            Logger::info("IPTV player exited with code: " + std::to_string(exitCode) + " after " + std::to_string(playDuration) + "ms");
            if (exitCode != 0) {
                if (playDuration < 3000) {
                    playSuccess = false;
                }
                // Read last lines of iptv_mpv.log for diagnostics
                std::ifstream logFile(appRoot + "/iptv_mpv.log");
                if (logFile.is_open()) {
                    std::string errLine;
                    int count = 0;
                    while (std::getline(logFile, errLine) && count++ < 10) {
                        if (!errLine.empty()) {
                            Logger::warn("mpv: " + errLine);
                        }
                    }
                }
            }
        } else if (WIFSIGNALED(status)) {
            Logger::info("IPTV player terminated by signal: " + std::to_string(WTERMSIG(status)));
        }

        m_mpvPid = -1;
        m_isPlaying = false;
        m_currentChannel = "";
        Logger::info("IPTV player finished, returning to RomCloud UI");

        // Flush any button events queued while player was active and reset controller state
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
        InputManager::instance().reset();

        return playSuccess;
    }

    unlink("/tmp/stay_awake");
    Logger::error("Failed to fork IPTV player");
    return false;
}

bool IPTVManager::stop() {
    unlink("/tmp/stay_awake");
    if (m_mpvPid > 0) {
        Logger::info("Stopping IPTV player (PID: " + std::to_string(m_mpvPid) + ")");
        // Send SIGTERM to process group and direct PID
        kill(-m_mpvPid, SIGTERM);
        kill(m_mpvPid, SIGTERM);

        int status = 0;
        for (int i = 0; i < 5; i++) {
            pid_t res = waitpid(m_mpvPid, &status, WNOHANG);
            if (res != 0) break;
            usleep(20000); // 20ms
        }

        // If process is still active, force kill
        if (kill(m_mpvPid, 0) == 0) {
            kill(-m_mpvPid, SIGKILL);
            kill(m_mpvPid, SIGKILL);
            waitpid(m_mpvPid, &status, WNOHANG);
        }
        m_mpvPid = -1;
    }

    m_isPlaying = false;
    m_currentChannel = "";
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

} // namespace RomCloud
