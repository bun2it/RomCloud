#pragma once
#include <atomic>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

namespace RomCloud {

// PlaylistItem — mot kenh trong mot playlist doc lap
struct PlaylistItem {
    std::string name;
    std::string url;
    std::string group;       // tu group-title trong #EXTINF
    std::string logo;        // tu tvg-logo
    bool isFavorite = false;
};

// Playlist — dai dien cho mot file .m3u doc lap
struct Playlist {
    std::string id;           // slug (e.g. "vietnam")
    std::string name;         // Ten hien thi (e.g. "Viet Nam")
    std::string sourceFile;   // Ten file (e.g. "vietnam.m3u")
    std::string sourcePath;   // Duong dan day du
    std::string sourceUrl;    // URL goc neu tai tu mang
    size_t fileSize = 0;

    std::vector<PlaylistItem> channels;

    // groups: tên group -> danh sách index trong `channels`
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

// IPTVChannel — backward-compat alias, gop thong tin nguon vao channel
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

// IPTVSource — thong tin meta cua nguon (dung trong Settings UI)
struct IPTVSource {
    std::string name;
    std::string filename;
    std::string type; // "file" or "url"
    std::string url;
    size_t channelCount = 0;
    size_t fileSize = 0;
    // Auto-refresh (chỉ áp dụng khi type == "url")
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

    // Loading
    bool loadPlaylists(const std::string& directory = "");
    const std::string& getIptvDir() const { return m_iptvDir; }

    // Multi-Playlist API (MOI)
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

    // Backward-compat: flat channel list gộp từ tất cả playlists
    const std::vector<IPTVChannel>& getChannels() const { return m_channels; }

    // Sources management (Settings UI)
    std::vector<IPTVSource> getSources() const { return m_sources; }
    bool addSourceFromUrl(const std::string& url, const std::string& customName,
                          std::string& outError, std::string& outFilename,
                          size_t& outChannelCount);
    bool addSourceFromFile(const std::string& filename, const std::string& customName);
    bool deleteSource(const std::string& filename, std::string& outError);

    // Favorites
    bool isFavorite(const std::string& channelName) const;
    void toggleFavorite(const std::string& channelName);
    void loadFavorites();
    void saveFavorites();
    std::vector<IPTVChannel> getFavoriteChannels() const;

    // Global search (cross-playlist)
    std::vector<IPTVChannel> search(const std::string& query) const;

    // Deprecated helpers (giữ để UIManager hiện tại không lỗi)
    std::vector<IPTVChannel> getChannelsByGroup(const std::string& group) const;
    std::vector<std::string> getGroups() const;

    // Auto-refresh (cho playlist được add qua URL)
    // Refresh một playlist cụ thể bằng cách tải lại từ URL nguồn
    bool refreshPlaylistFromUrl(const std::string& filename, std::string& outError);
    // Check và tự động refresh tất cả URL-playlist bị stale (gọi lúc startup)
    int  checkAndAutoRefresh(); // tra ve so playlist da duoc refresh
    // Đặt refresh interval (số giờ, 0 = tắt tự động refresh)
    void setRefreshInterval(const std::string& filename, int hours);
    // Lấy chuỗi thời gian refresh cuối
    std::string getLastRefreshedStr(const std::string& filename) const;
    // Lấy danh sách filename của các URL playlist cần refresh
    std::vector<std::string> getStalePlaylistFiles() const;

    // OTA Update Playlist (tải playlist M3U mới nhất từ xa)
    bool updateOtaPlaylist(std::string& outMessage);

    IPTVChannel* getChannel(size_t index);

    // Ping cache lookup (URL -> ms; -2=chua do, -1=mat ket noi, >=0=ms)
    int getCachedPing(const std::string &url);
    void clearPingCache();
    void prefetchPings(const std::vector<std::string> &urls);

    // Media player check & auto installation
    bool isMediaPlayerInstalled() const;
    bool ensureMediaPlayerAvailable();

    int getLastPlayingIndex() const { return m_lastPlayingIndex; }
    bool playYouTubeVideo(const std::string& videoId, const std::string& initialUrl, const std::string& quality = "720", const std::string& title = "", const std::string& desc = "");
    // true nếu lần phát YouTube vừa rồi kết thúc tự nhiên (hết video),
    // false nếu user thoát (B) hoặc lỗi. Dùng cho tự phát tiếp.
    bool ytNaturalEnd() const { return m_ytNaturalEnd; }
    void setYtDesc(const std::string& d) { m_ytDesc = d; }
    bool playYouTubeUrl(const std::string& url);
    bool switchYouTubeQuality(const std::string& videoId, const std::string& targetQuality);
    // Chuyển luồng đã có URL (không resolve): dùng cho auto-upgrade.
    bool switchYouTubeStream(const std::string& videoId, const std::string& newUrl, const std::string& targetQuality);
    // URL YouTube resolve ngầm để tự lên nét (vid + quality + url, có
    // mutex). Kèm key "vid|quality" đang resolve dở để SELECT spam không
    // đẻ thread trùng.
    void setUpgradeUrl(const std::string& videoId, const std::string& quality,
                       const std::string& url);
    bool takeUpgradeUrl(const std::string& videoId, std::string& outUrl,
                        std::string& outQuality);
    void clearUpgrade(const std::string& videoId);
    void showOverlayIcon(const std::string& iconName, uint32_t durationMs = 1400);
    bool sendMpvIpcCommand(const std::string& cmd, std::string* response = nullptr, const std::string& sockPath = "");
    bool isYouTubePlaying() const { return m_isPlaying && m_currentChannel == "YouTube"; }
    bool stop();

    // IPTV video overlay (channel info + duration bar) — show/hide API
    void showIPTVVideoFooter(bool isPaused);
    void hideIPTVVideoFooter();

    // IPTV playback — blocking loop (mpv controls via IPC, SDL idle)
    bool playChannel(const IPTVChannel& channel, size_t initialIndex = 0, const std::vector<IPTVChannel>& customList = {});
    // Chuyển kênh trong lúc đang phát (loadfile replace, không restart mpv)
    bool switchIPTVChannelByIndex(size_t idx);
    void showIPTVChannelOSD(const std::vector<IPTVChannel>& channels,
                            int selectedIndex,
                            const std::string& groupName = "",
                            int durationMs = 4000);
    void hideIPTVChannelOSD();
    // Footer bar cho playback (persistent button hints)
    void showIPTVPlaybackFooter(bool isPaused);
    void hideIPTVPlaybackFooter();
    bool isIPTVPlaying() const;

    // Status
    bool isPlaying() const { return m_isPlaying; }
    const std::string& getCurrentChannelName() const { return m_currentChannel; }

private:
    IPTVManager();
    ~IPTVManager();
    IPTVManager(const IPTVManager&) = delete;
    IPTVManager& operator=(const IPTVManager&) = delete;

    // Parsing internals
    // Phân tích một file .m3u thành một Playlist object độc lập
    bool parseM3UIntoPlaylist(const std::string& filepath, Playlist& out);

    // Giữ lại để không vỡ các call cũ ở phần playback
    bool parseM3UFile(const std::string& filepath, const std::string& sourceName,
                      const std::string& filename, size_t* outChannelCount = nullptr);

    void loadSourcesMeta();
    void saveSourcesMeta();
    std::string extractGroup(const std::string& line);
    std::string extractName(const std::string& line);
    std::string extractLogo(const std::string& line);

    // Rebuild flat m_channels & m_groups tu m_playlists (sau moi lan load)
    void rebuildFlatChannelList();

    // Data
    std::string m_iptvDir;

    // MULTI-PLAYLIST (MỚI): mỗi file .m3u = một Playlist độc lập
    std::vector<Playlist> m_playlists;

    // BACKWARD-COMPAT: flat list gop (rebuild tu m_playlists)
    std::vector<IPTVChannel> m_channels;
    std::unordered_map<std::string, std::vector<size_t>> m_groups;

    std::vector<IPTVSource> m_sources;
    std::unordered_map<std::string, IPTVSource> m_sourcesMeta;
    std::unordered_set<std::string> m_favorites;

    bool m_isPlaying = false;
    std::string m_currentChannel;
    // YouTube OSD (UI-only): tiêu đề video + trạng thái thanh điều khiển
    std::string m_ytTitle;
    std::string m_ytDesc; // mo ta video (OSD dong 2)
    bool m_ytNaturalEnd = false; // het video tu nhien (up next)
    bool m_ytStopRequested = false; // user/ma goi stop() giua chung
    std::string m_ytQuality = "720";
    bool m_ytOsdOn = false;
    uint32_t m_ytOsdExpire = 0;
    uint32_t m_ytOsdLastRefresh = 0;
    double m_ytDuration = -1.0;
    void showYouTubeOSD(bool extendExpire = true);
    void hideYouTubeOSD();
    // Flash giữa: icon .raw nếu user đã thêm file, ngược lại chữ (tự chuyển,
    // không sửa logic khi có icon mới: aspect/cc/speed/quality).
    void flashCenter(const std::string& icon, const std::string& textCmd);
    double ytTimePos();
    double ytDuration();
    double ytCacheAhead();
    double ytMpvNumber(const std::string& prop);
    int ytMeasuredW(); // số đo thật từ mpv (0 = chưa có)
    int ytMeasuredH();
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
    // URL YouTube resolve ngầm để tự lên nét (vid + quality + url, có
    // mutex) + key "vid|quality" đang resolve dở (chống spam thread).
    std::string m_ytUpgradeVid;
    std::string m_ytUpgradeQuality;
    std::string m_ytUpgradeUrl;
    std::string m_ytUpgradingKey;
    std::mutex m_ytUpgradeMtx;
    // Prefetch 720p: nạp ngầm trong lúc 360 vẫn phát, đủ đệm mới nhảy index.
    std::string m_ytPreVid;
    std::string m_ytPreVideo;
    std::string m_ytPreAudio;
    bool m_ytPreActive = false;
    // Khởi động tiến trình mpv mới cho URL đã resolve (dùng khi restart đổi kênh)
    pid_t spawnMpvForUrl(const std::string& url);

    // Ping cache cho channel list (key=URL, value=ms; -1=lỗi)
    std::unordered_map<std::string, int> m_pingCache;
    std::unordered_set<std::string> m_pingInFlight;
    // Chống thread-storm khi lướt list lớn (10k kênh): chỉ 1 worker ping
    // tại một thời điểm; yêu cầu mới ghi đè vào hàng đợi.
    std::atomic<bool> m_pingWorkerActive{false};
    std::vector<std::string> m_pingPending;
};

} // namespace RomCloud
