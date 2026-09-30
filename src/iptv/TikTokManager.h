#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace RomCloud {

struct TikTokVideo {
    std::string id;
    std::string title;
    std::string author;
    std::string playUrl;
    int duration = 0;
};

class TikTokManager {
public:
    static TikTokManager& instance();

    // Pure TikTok API: search challenge & get vertical video feed
    std::vector<TikTokVideo> getFeedForTag(const std::string& tagOrQuery);

    std::vector<std::string> search(const std::string& query, int page = 1);
    std::vector<std::string> trending(int page = 1);
    std::string resolveStreamUrl(const std::string& tiktokUrl);

private:
    TikTokManager() = default;
    ~TikTokManager() = default;
    TikTokManager(const TikTokManager&) = delete;
    TikTokManager& operator=(const TikTokManager&) = delete;

    std::string runScript(const std::string& subcommand, const std::string& arg,
                         int page = 1, int perPage = 6);

    std::unordered_map<std::string, std::string> m_streamCache;
    std::mutex m_cacheMutex;
};

} // namespace RomCloud
