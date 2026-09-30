#include "TikTokManager.h"
#include "../config/AppConfig.h"
#include "../logging/Logger.h"
#include "../network/HttpClient.h"
#include "../network/JsonHelper.h"
#include <cstdio>
#include <sstream>
#include <sys/stat.h>

namespace RomCloud {

std::vector<TikTokVideo> TikTokManager::getFeedForTag(const std::string& tagOrQuery) {
    std::vector<TikTokVideo> feed;
    std::string clean = tagOrQuery;
    while (!clean.empty() && (clean.front() == '#' || clean.front() == ' ')) clean.erase(0, 1);
    while (!clean.empty() && clean.back() == ' ') clean.pop_back();
    if (clean.empty()) clean = "haihuoc";

    Logger::info("[TikTok] Fetching feed for tag: " + clean);

    // 1. Search challenge ID
    std::string cid = "57577487"; // Default challenge ID for haihuoc
    std::string searchUrl = "https://www.tikwm.com/api/challenge/search?keywords=" + HttpClient::instance().urlEncode(clean);
    HttpResponse sResp = HttpClient::instance().get(searchUrl, {}, 8);
    if (sResp.success && !sResp.body.empty()) {
        auto chalList = JsonHelper::extractArrayObjects(sResp.body, "challenge_list");
        if (!chalList.empty()) {
            std::string foundId = JsonHelper::extractString(chalList[0], "id");
            if (!foundId.empty()) {
                cid = foundId;
            }
        }
    }

    Logger::info("[TikTok] Using challenge CID: " + cid);

    // 2. Fetch posts for this challenge ID
    std::string postsUrl = "https://www.tikwm.com/api/challenge/posts?challenge_id=" + cid + "&count=30&cursor=0";
    HttpResponse pResp = HttpClient::instance().get(postsUrl, {}, 10);
    if (!pResp.success || pResp.body.empty()) {
        Logger::error("[TikTok] Failed to fetch posts for challenge " + cid);
        return feed;
    }

    auto vObjs = JsonHelper::extractArrayObjects(pResp.body, "videos");
    for (const auto& vObj : vObjs) {
        TikTokVideo item;
        item.id = JsonHelper::extractString(vObj, "video_id");
        item.title = JsonHelper::extractString(vObj, "title");
        item.playUrl = JsonHelper::extractString(vObj, "play");
        if (item.playUrl.empty()) {
            item.playUrl = JsonHelper::extractString(vObj, "wmplay");
        }
        item.author = JsonHelper::extractString(vObj, "nickname");
        if (item.author.empty()) {
            item.author = JsonHelper::extractString(vObj, "unique_id");
        }
        item.duration = JsonHelper::extractInt(vObj, "duration", 0);

        if (!item.playUrl.empty()) {
            feed.push_back(item);
        }
    }

    Logger::info("[TikTok] Successfully fetched " + std::to_string(feed.size()) + " videos for tag " + clean);
    return feed;
}

static std::string execScript(const std::string& cmd) {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result;

    char buffer[2048];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    return result;
}

static std::string getAppRoot() {
    std::string root = AppConfig::instance().getAppRoot();
    if (root.empty()) root = "/mnt/SDCARD/Apps/RomCloud";
    return root;
}

std::string TikTokManager::runScript(const std::string& subcommand, const std::string& arg,
                                    int page, int perPage) {
    std::string appRoot = getAppRoot();
    std::string scriptPath = appRoot + "/scripts/tiktok_search.sh";

    struct stat st;
    if (stat(scriptPath.c_str(), &st) != 0) {
        Logger::error("[TikTok] Script not found: " + scriptPath);
        return "";
    }

    std::string cmd;
    if (subcommand == "url") {
        cmd = "\"" + scriptPath + "\" " + subcommand + " \"" + arg + "\" 2>/dev/null";
    } else if (subcommand == "search" || subcommand == "trending") {
        cmd = "\"" + scriptPath + "\" " + subcommand + " \"" + arg + "\" " +
              std::to_string(page) + " " + std::to_string(perPage) + " 2>/dev/null";
    } else {
        return "";
    }

    return execScript(cmd);
}

std::vector<std::string> TikTokManager::search(const std::string& query, int page) {
    std::vector<std::string> results;
    if (query.empty()) return results;

    Logger::info("[TikTok] Searching: " + query);
    std::string output = runScript("search", query, page, 6);
    if (output.empty()) return results;

    std::stringstream ss(output);
    std::string line;
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.find("ERROR:") == 0 || line.find("WARNING:") == 0) continue;
        if (std::count(line.begin(), line.end(), '|') >= 2) {
            results.push_back(line);
        }
    }
    Logger::info("[TikTok] search returned " + std::to_string(results.size()) + " results");
    return results;
}

std::vector<std::string> TikTokManager::trending(int page) {
    std::vector<std::string> results;

    Logger::info("[TikTok] Fetching trending feed");
    std::string output = runScript("trending", "", page, 6);
    if (output.empty()) return results;

    std::stringstream ss(output);
    std::string line;
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.find("ERROR:") == 0 || line.find("WARNING:") == 0) continue;
        if (std::count(line.begin(), line.end(), '|') >= 2) {
            results.push_back(line);
        }
    }
    Logger::info("[TikTok] trending returned " + std::to_string(results.size()) + " results");
    return results;
}

std::string TikTokManager::resolveStreamUrl(const std::string& tiktokUrl) {
    if (tiktokUrl.empty()) return "";

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto it = m_streamCache.find(tiktokUrl);
        if (it != m_streamCache.end() && !it->second.empty()) {
            Logger::info("[TikTok] Using cached stream URL");
            return it->second;
        }
    }

    Logger::info("[TikTok] Resolving stream: " + tiktokUrl);
    std::string output = runScript("url", tiktokUrl, 1, 1);
    if (output.empty()) return "";

    std::stringstream ss(output);
    std::string line;
    std::string streamUrl;
    while (std::getline(ss, line)) {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.find("http://") == 0 || line.find("https://") == 0) {
            streamUrl = line;
            break;
        }
    }

    if (!streamUrl.empty()) {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_streamCache[tiktokUrl] = streamUrl;
    }
    return streamUrl;
}

TikTokManager& TikTokManager::instance() {
    static TikTokManager inst;
    return inst;
}

} // namespace RomCloud
