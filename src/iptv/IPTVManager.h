#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace RomCloud {

struct IPTVChannel {
    std::string name;
    std::string url;
    std::string group;
    std::string logo;
    std::string source;      // Tên nguồn hiển thị (vd: "Việt Nam", "Mặc định", "Thể Thao")
    std::string sourceFile;  // Tên file (vd: "vietnam.m3u")
    bool isFavorite = false;
};

struct IPTVSource {
    std::string name;
    std::string filename;
    std::string type; // "file" or "url"
    std::string url;
    size_t channelCount = 0;
    size_t fileSize = 0;
};

class IPTVManager {
public:
    static IPTVManager& instance();

    // Load playlists from directory
    bool loadPlaylists(const std::string& directory = "");

    // Get IPTV directory
    const std::string& getIptvDir() const { return m_iptvDir; }

    // Get all channels
    const std::vector<IPTVChannel>& getChannels() const { return m_channels; }

    // Sources management
    std::vector<IPTVSource> getSources() const { return m_sources; }
    bool addSourceFromUrl(const std::string& url, const std::string& customName, std::string& outError, std::string& outFilename, size_t& outChannelCount);
    bool addSourceFromFile(const std::string& filename, const std::string& customName);
    bool deleteSource(const std::string& filename, std::string& outError);

    // Favorites support
    bool isFavorite(const std::string& channelName) const;
    void toggleFavorite(const std::string& channelName);
    void loadFavorites();
    void saveFavorites();
    std::vector<IPTVChannel> getFavoriteChannels() const;

    // Get channels by group
    std::vector<IPTVChannel> getChannelsByGroup(const std::string& group) const;

    // Get all groups
    std::vector<std::string> getGroups() const;

    // Search channels
    std::vector<IPTVChannel> search(const std::string& query) const;

    // Get channel by index
    IPTVChannel* getChannel(size_t index);

    // Create default playlist
    void createDefaultPlaylist(const std::string& filepath);

    // Media player check & auto installation
    bool isMediaPlayerInstalled() const;
    bool ensureMediaPlayerAvailable();

    // Play channel with mpv/ffplay
    bool playChannel(const IPTVChannel& channel);
    bool playYouTubeVideo(const std::string& videoId, const std::string& initialUrl, const std::string& quality = "360");
    bool playYouTubeUrl(const std::string& url);
    bool switchYouTubeQuality(const std::string& videoId, const std::string& targetQuality);
    void showOverlayIcon(const std::string& iconName, uint32_t durationMs = 1400);
    bool sendMpvIpcCommand(const std::string& cmd, std::string* response = nullptr, const std::string& sockPath = "");
    bool isYouTubePlaying() const { return m_isPlaying && m_currentChannel == "YouTube"; }
    bool stop();

    // Status
    bool isPlaying() const { return m_isPlaying; }
    const std::string& getCurrentChannelName() const { return m_currentChannel; }

private:
    IPTVManager();
    ~IPTVManager();
    IPTVManager(const IPTVManager&) = delete;
    IPTVManager& operator=(const IPTVManager&) = delete;

    bool parseM3UFile(const std::string& filepath, const std::string& sourceName, const std::string& filename, size_t* outChannelCount = nullptr);
    void loadSourcesMeta();
    void saveSourcesMeta();
    std::string extractGroup(const std::string& line);
    std::string extractName(const std::string& line);
    std::string extractLogo(const std::string& line);

    std::string m_iptvDir;
    std::vector<IPTVChannel> m_channels;
    std::vector<IPTVSource> m_sources;
    std::unordered_map<std::string, IPTVSource> m_sourcesMeta;
    std::unordered_set<std::string> m_favorites;
    std::unordered_map<std::string, std::vector<size_t>> m_groups; // group -> channel indices
    bool m_isPlaying = false;
    std::string m_currentChannel;
    int m_mpvPid = -1;
    uint32_t m_overlayExpireTime = 0;
};

} // namespace RomCloud
