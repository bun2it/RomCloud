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
    bool isFavorite = false;
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
    bool stop();

    // Status
    bool isPlaying() const { return m_isPlaying; }
    const std::string& getCurrentChannelName() const { return m_currentChannel; }

private:
    IPTVManager();
    ~IPTVManager();
    IPTVManager(const IPTVManager&) = delete;
    IPTVManager& operator=(const IPTVManager&) = delete;

    bool parseM3UFile(const std::string& filepath);
    std::string extractGroup(const std::string& line);
    std::string extractName(const std::string& line);
    std::string extractLogo(const std::string& line);

    std::string m_iptvDir;
    std::vector<IPTVChannel> m_channels;
    std::unordered_set<std::string> m_favorites;
    std::unordered_map<std::string, std::vector<size_t>> m_groups; // group -> channel indices
    bool m_isPlaying = false;
    std::string m_currentChannel;
    int m_mpvPid = -1;
};

} // namespace RomCloud
