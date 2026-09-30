#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

namespace RomCloud {

// ---------------------------------------------------------------------------
// PlaylistItem — mot kenh trong mot playlist doc lap
// ---------------------------------------------------------------------------
struct PlaylistItem {
    std::string name;
    std::string url;
    std::string group;       // tu group-title trong #EXTINF
    std::string logo;        // tu tvg-logo
    bool isFavorite = false;
};

// ---------------------------------------------------------------------------
// Playlist — dai dien cho mot file .m3u doc lap
// ---------------------------------------------------------------------------
struct Playlist {
    std::string id;           // slug (e.g. "vietnam")
    std::string name;         // Ten hien thi (e.g. "Viet Nam")
    std::string sourceFile;   // Ten file (e.g. "vietnam.m3u")
    std::string sourcePath;   // Duong dan day du
    std::string sourceUrl;    // URL goc neu tai tu mang
    size_t fileSize = 0;

    std::vector<PlaylistItem> channels;

    // groups: ten group -> danh sach index trong `channels`
    std::unordered_map<std::string, std::vector<size_t>> groups;

    size_t channelCount() const { return channels.size(); }

    std::vector<std::string> getGroups() const {
        std::vector<std::string> result;
        result.reserve(groups.size());
        for (const auto& kv : groups) result.push_back(kv.first);
        std::sort(result.begin(), result.end());
        return result;
    }

    std::vector<PlaylistItem> getChannelsByGroup(const std::string& group) const {
        std::vector<PlaylistItem> result;
        auto it = groups.find(group);
        if (it != groups.end()) {
            for (size_t idx : it->second)
                if (idx < channels.size()) result.push_back(channels[idx]);
        }
        return result;
    }

    std::vector<PlaylistItem> search(const std::string& query) const {
        std::string lq = query;
        std::transform(lq.begin(), lq.end(), lq.begin(), ::tolower);
        std::vector<PlaylistItem> result;
        for (const auto& ch : channels) {
            std::string ln = ch.name, lg = ch.group;
            std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
            std::transform(lg.begin(), lg.end(), lg.begin(), ::tolower);
            if (ln.find(lq) != std::string::npos || lg.find(lq) != std::string::npos)
                result.push_back(ch);
        }
        return result;
    }
};

// ---------------------------------------------------------------------------
// IPTVChannel — backward-compat alias, gop thong tin nguon vao channel
// ---------------------------------------------------------------------------
struct IPTVChannel {
    std::string name;
    std::string url;
    std::string group;
    std::string logo;
    std::string source;      // Ten playlist (vd: "Viet Nam")
    std::string sourceFile;  // Tên file (vd: "vietnam.m3u")
    bool isFavorite = false;

    static IPTVChannel fromItem(const PlaylistItem& item,
                                const std::string& srcName,
                                const std::string& srcFile) {
        IPTVChannel ch;
        ch.name = item.name; ch.url = item.url;
        ch.group = item.group; ch.logo = item.logo;
        ch.isFavorite = item.isFavorite;
        ch.source = srcName; ch.sourceFile = srcFile;
        return ch;
    }
};

// ---------------------------------------------------------------------------
// IPTVSource — thong tin meta cua nguon (dung trong Settings UI)
// ---------------------------------------------------------------------------
struct IPTVSource {
    std::string name;
    std::string filename;
    std::string type; // "file" or "url"
    std::string url;
    size_t channelCount = 0;
    size_t fileSize = 0;
    // Auto-refresh (chi ap dung khi type == "url")
    std::time_t lastRefreshed     = 0;  // Unix timestamp lan refresh cuoi
    int  refreshIntervalHours     = 24; // 0 = tat tu dong refresh

    bool isStale() const {
        if (type != "url" || url.empty() || refreshIntervalHours <= 0) return false;
        if (lastRefreshed == 0) return true; // chua tung refresh
        std::time_t now = std::time(nullptr);
        return (now - lastRefreshed) >= static_cast<std::time_t>(refreshIntervalHours * 3600);
    }
    std::string lastRefreshedStr() const {
        if (lastRefreshed == 0) return "Chưa refresh";
        char buf[64];
        struct tm* tm_info = localtime(&lastRefreshed);
        strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M", tm_info);
        return buf;
    }
};

class IPTVManager {
public:
    static IPTVManager& instance();

    // -----------------------------------------------------------------------
    // Loading
    // -----------------------------------------------------------------------
    bool loadPlaylists(const std::string& directory = "");
    const std::string& getIptvDir() const { return m_iptvDir; }

    // -----------------------------------------------------------------------
    // Multi-Playlist API (MOI)
    // -----------------------------------------------------------------------
    const std::vector<Playlist>& getPlaylists() const { return m_playlists; }
    const Playlist* getPlaylist(size_t index) const {
        return index < m_playlists.size() ? &m_playlists[index] : nullptr;
    }
    const Playlist* getPlaylistById(const std::string& id) const {
        for (const auto& pl : m_playlists)
            if (pl.id == id) return &pl;
        return nullptr;
    }
    size_t playlistCount() const { return m_playlists.size(); }

    // -----------------------------------------------------------------------
    // Backward-compat: flat channel list gop tu tat ca playlists
    // -----------------------------------------------------------------------
    const std::vector<IPTVChannel>& getChannels() const { return m_channels; }

    // -----------------------------------------------------------------------
    // Sources management (Settings UI)
    // -----------------------------------------------------------------------
    std::vector<IPTVSource> getSources() const { return m_sources; }
    bool addSourceFromUrl(const std::string& url, const std::string& customName,
                          std::string& outError, std::string& outFilename,
                          size_t& outChannelCount);
    bool addSourceFromFile(const std::string& filename, const std::string& customName);
    bool deleteSource(const std::string& filename, std::string& outError);

    // -----------------------------------------------------------------------
    // Favorites
    // -----------------------------------------------------------------------
    bool isFavorite(const std::string& channelName) const;
    void toggleFavorite(const std::string& channelName);
    void loadFavorites();
    void saveFavorites();
    std::vector<IPTVChannel> getFavoriteChannels() const;

    // -----------------------------------------------------------------------
    // Global search (cross-playlist)
    // -----------------------------------------------------------------------
    std::vector<IPTVChannel> search(const std::string& query) const;

    // Deprecated helpers (giu de UIManager hien tai khong loi)
    std::vector<IPTVChannel> getChannelsByGroup(const std::string& group) const;
    std::vector<std::string> getGroups() const;

    // -----------------------------------------------------------------------
    // Auto-refresh (cho playlist duoc add qua URL)
    // -----------------------------------------------------------------------
    // Refresh mot playlist cu the bang cach tai lai tu URL nguon
    bool refreshPlaylistFromUrl(const std::string& filename, std::string& outError);
    // Check va tu dong refresh tat ca URL-playlist bi stale (goi luc startup)
    int  checkAndAutoRefresh(); // tra ve so playlist da duoc refresh
    // Dat refresh interval (so gio, 0 = tat tu dong refresh)
    void setRefreshInterval(const std::string& filename, int hours);
    // Lay chuoi thoi gian refresh cuoi
    std::string getLastRefreshedStr(const std::string& filename) const;
    // Lay danh sach filename cua cac URL playlist can refresh
    std::vector<std::string> getStalePlaylistFiles() const;


    // Get channel by index
    IPTVChannel* getChannel(size_t index);

    // Create default playlist
    void createDefaultPlaylist(const std::string& filepath);

    // Media player check & auto installation
    bool isMediaPlayerInstalled() const;
    bool ensureMediaPlayerAvailable();

    int getLastPlayingIndex() const { return m_lastPlayingIndex; }
    bool playYouTubeVideo(const std::string& videoId, const std::string& initialUrl, const std::string& quality = "360");
    bool playYouTubeUrl(const std::string& url);
    bool switchYouTubeQuality(const std::string& videoId, const std::string& targetQuality);
    void showOverlayIcon(const std::string& iconName, uint32_t durationMs = 1400);
    bool sendMpvIpcCommand(const std::string& cmd, std::string* response = nullptr, const std::string& sockPath = "");
    bool isYouTubePlaying() const { return m_isPlaying && m_currentChannel == "YouTube"; }
    bool stop();

    // IPTV playback — blocking loop (mpv controls via IPC, SDL idle)
    bool playChannel(const IPTVChannel& channel, size_t initialIndex = 0, const std::vector<IPTVChannel>& customList = {});
    // Chuyen kenh trong luc dang phat (loadfile replace, khong restart mpv)
    bool switchIPTVChannelByIndex(size_t idx);
    void showIPTVChannelOSD(const std::vector<IPTVChannel>& channels,
                            int selectedIndex,
                            const std::string& groupName = "",
                            int durationMs = 4000);
    bool isIPTVPlaying() const;

    // Status
    bool isPlaying() const { return m_isPlaying; }
    const std::string& getCurrentChannelName() const { return m_currentChannel; }

private:
    IPTVManager();
    ~IPTVManager();
    IPTVManager(const IPTVManager&) = delete;
    IPTVManager& operator=(const IPTVManager&) = delete;

    // -----------------------------------------------------------------------
    // Parsing internals
    // -----------------------------------------------------------------------
    // Phan tich mot file .m3u thanh mot Playlist object doc lap
    bool parseM3UIntoPlaylist(const std::string& filepath, Playlist& out);

    // Giu lai de khong vo cac call cu o phan playback
    bool parseM3UFile(const std::string& filepath, const std::string& sourceName,
                      const std::string& filename, size_t* outChannelCount = nullptr);

    void loadSourcesMeta();
    void saveSourcesMeta();
    std::string extractGroup(const std::string& line);
    std::string extractName(const std::string& line);
    std::string extractLogo(const std::string& line);

    // Rebuild flat m_channels & m_groups tu m_playlists (sau moi lan load)
    void rebuildFlatChannelList();

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------
    std::string m_iptvDir;

    // === MULTI-PLAYLIST (MOI): moi file .m3u = mot Playlist doc lap ===
    std::vector<Playlist> m_playlists;

    // === BACKWARD-COMPAT: flat list gop (rebuild tu m_playlists) ===
    std::vector<IPTVChannel> m_channels;
    std::unordered_map<std::string, std::vector<size_t>> m_groups;

    std::vector<IPTVSource> m_sources;
    std::unordered_map<std::string, IPTVSource> m_sourcesMeta;
    std::unordered_set<std::string> m_favorites;

    bool m_isPlaying = false;
    std::string m_currentChannel;
    // P1-3: PID mpv do MpvPlayer giu duy nhat (isPlaying()/pid()/pollExited()).
    // Dùng MpvPlayer::instance() thay cho mirror m_mpvPid.
    bool isMpvPlaying() const;
    uint32_t m_overlayExpireTime = 0;
    int m_lastPlayingIndex = 0;

    // Stream URL LRU cache (key = original URL, value = resolved capped URL)
    std::unordered_map<std::string, std::string> m_streamUrlCache;
    static constexpr size_t kStreamCacheMax = 50;
    std::mutex m_streamCacheMutex;

    std::vector<IPTVChannel> m_iptvChannelList;
    size_t m_iptvSelectedIndex = 0;
    size_t m_iptvCurrentIndex  = 0;

    // Debounce nút A + chống pause kẹt sau loadfile
    uint32_t m_lastASwitchMs = 0;
    // Prefetch resolve kênh kế tiếp (giảm đơ khi bấm A sang kênh chưa xem)
    std::string m_prefetchKey;
    std::string m_prefetchUrl;
    std::mutex m_prefetchMutex;
    // Khởi động tiến trình mpv mới cho URL đã resolve (dùng khi restart đổi kênh)
    pid_t spawnMpvForUrl(const std::string& url);
};

} // namespace RomCloud
