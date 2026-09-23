#include "IssueLogger.h"
#include "../config/AppConfig.h"
#include "../platform/PlatformInfo.h"
#include "../logging/Logger.h"
#include "../database/DatabaseManager.h"
#include "../ota/UpdateManager.h"

#include <curl/curl.h>
#include <sys/utsname.h>
#include <time.h>
#include <sstream>
#include <iomanip>

namespace RomCloud {

IssueLogger& IssueLogger::instance() {
    static IssueLogger instance;
    return instance;
}

IssueLogger::IssueLogger()
    : m_enabled(false), m_issueCount(0) {

    // Check if GitHub integration is enabled
    m_githubToken = DatabaseManager::instance().getSetting("github_token", "");
    m_repoOwner = "bun2it";
    m_repoName = "RomCloud";

    if (!m_githubToken.empty()) {
        m_enabled = true;
        Logger::info("IssueLogger: GitHub integration enabled");
    } else {
        // Try to get token from config file
        std::string configPath = AppConfig::instance().getConfigDir() + "/github_token";
        FILE* f = fopen(configPath.c_str(), "r");
        if (f) {
            char token[256] = {0};
            if (fgets(token, sizeof(token), f)) {
                m_githubToken = token;
                // Remove trailing newline
                if (!m_githubToken.empty() && m_githubToken.back() == '\n') {
                    m_githubToken.pop_back();
                }
                if (!m_githubToken.empty()) {
                    m_enabled = true;
                    Logger::info("IssueLogger: GitHub token loaded from config");
                }
            }
            fclose(f);
        }
    }

    if (!m_enabled) {
        Logger::warn("IssueLogger: GitHub integration disabled (no token found)");
    }
}

IssueLogger::~IssueLogger() {
}

bool IssueLogger::isEnabled() const {
    return m_enabled;
}

int IssueLogger::getRecentIssuesCount() const {
    return m_issueCount;
}

std::string IssueLogger::escapeMarkdown(const std::string& text) {
    std::string result;
    for (char c : text) {
        switch (c) {
            case '\\': result += "\\\\"; break;
            case '`': result += "\\`"; break;
            case '*': result += "\\*"; break;
            case '_': result += "\\_"; break;
            case '#': result += "\\#"; break;
            case '+': result += "\\+"; break;
            case '-': result += "\\-"; break;
            case '.': result += "\\."; break;
            case '!': result += "\\!"; break;
            case '[': result += "\\["; break;
            case ']': result += "\\]"; break;
            case '(': result += "\\("; break;
            case ')': result += "\\)"; break;
            default: result += c; break;
        }
    }
    return result;
}

std::string IssueLogger::getDeviceInfo() {
    std::ostringstream oss;
    auto diag = PlatformInfo::instance().getDiagnostics();

    oss << "- **App Version:** v" << APP_VERSION << "\n";
    oss << "- **Device:** " << diag.socName << "\n";
    oss << "- **OS:** " << diag.osName << " " << diag.kernelRelease << "\n";
    oss << "- **Display:** " << diag.displayResolution << "\n";
    oss << "- **RAM:** " << diag.freeRam << " / " << diag.totalRam << "\n";
    oss << "- **Storage:** " << diag.sdFreeSpace << " / " << diag.sdTotalSpace << "\n";
    oss << "- **Network:** " << diag.networkStatus;
    if (diag.ipAddress != "N/A") {
        oss << " (IP: " << diag.ipAddress << ")";
    }
    oss << "\n";

    return oss.str();
}

bool IssueLogger::createGitHubIssue(const IssueInfo& issue) {
    if (!m_enabled || m_githubToken.empty()) {
        Logger::warn("IssueLogger: Cannot create issue - GitHub integration disabled");
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = "https://api.github.com/repos/" + m_repoOwner + "/" + m_repoName + "/issues";

    // Build JSON payload
    std::string json = "{";
    json += "\"title\":\"" + escapeMarkdown(issue.title) + "\",";
    json += "\"body\":\"" + escapeMarkdown(issue.body) + "\",";
    json += "\"labels\":[";
    std::string labelList = issue.labels;
    size_t pos = 0;
    bool first = true;
    while ((pos = labelList.find(',')) != std::string::npos) {
        std::string label = labelList.substr(0, pos);
        if (!first) json += ",";
        json += "\"" + escapeMarkdown(label) + "\"";
        first = false;
        labelList = labelList.substr(pos + 1);
    }
    if (!labelList.empty()) {
        if (!first) json += ",";
        json += "\"" + escapeMarkdown(labelList) + "\"";
    }
    json += "]}";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: token " + m_githubToken).c_str());
    headers = curl_slist_append(headers, "Accept: application/vnd.github.v3+json");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "User-Agent: RomCloud-IssueLogger/1.0");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t size, size_t nmemb, void* stream) -> size_t {
        return size * nmemb;
    });
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (res == CURLE_OK && (httpCode == 201 || httpCode == 200)) {
        m_issueCount++;
        Logger::info("IssueLogger: Created GitHub issue: " + issue.title);
        return true;
    }

    Logger::error("IssueLogger: Failed to create issue (HTTP " + std::to_string(httpCode) + ")");
    return false;
}

bool IssueLogger::logError(const std::string& errorType,
                           const std::string& errorMessage,
                           const std::string& stackTrace,
                           const std::string& context) {
    IssueInfo issue;
    issue.title = "[Auto-Report] " + errorType + ": " +
                  (errorMessage.length() > 50 ? errorMessage.substr(0, 47) + "..." : errorMessage);
    issue.labels = "auto-reported,bug";

    std::ostringstream body;
    body << "## Lỗi được tự động báo cáo từ thiết bị người dùng\n\n";
    body << "### Mô tả lỗi\n";
    body << "```\n" << errorMessage << "\n```\n\n";

    if (!stackTrace.empty()) {
        body << "### Stack Trace\n";
        body << "```\n" << stackTrace << "\n```\n\n";
    }

    if (!context.empty()) {
        body << "### Ngữ cảnh\n";
        body << context << "\n\n";
    }

    body << "### Thông tin thiết bị\n";
    body << getDeviceInfo();

    body << "---\n";
    body << "*⚠️ Đây là báo cáo tự động từ RomCloud v" << APP_VERSION << "*\n";

    issue.body = body.str();
    issue.version = APP_VERSION;

    return createGitHubIssue(issue);
}

bool IssueLogger::logCrash(const std::string& crashInfo,
                           const std::string& stackTrace,
                           const std::string& deviceInfo) {
    IssueInfo issue;
    issue.title = "[CRASH] " +
                  (crashInfo.length() > 60 ? crashInfo.substr(0, 57) + "..." : crashInfo);
    issue.labels = "auto-reported,crash,critical";

    std::ostringstream body;
    body << "## 🔴 RomCloud Crash Report\n\n";
    body << "### Crash Info\n";
    body << "```\n" << crashInfo << "\n```\n\n";

    if (!stackTrace.empty()) {
        body << "### Stack Trace\n";
        body << "```\n" << stackTrace << "\n```\n\n";
    }

    body << "### Thông tin thiết bị\n";
    body << getDeviceInfo();

    body << "---\n";
    body << "*⚠️ Đây là báo cáo crash tự động từ RomCloud*\n";

    issue.body = body.str();
    issue.version = APP_VERSION;

    return createGitHubIssue(issue);
}

} // namespace RomCloud
