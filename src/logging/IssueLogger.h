#pragma once
#include <string>
#include <vector>

namespace RomCloud {

struct IssueInfo {
    std::string title;
    std::string body;
    std::string labels;  // comma-separated: "bug,error,v1.3.3"
    std::string version;
    std::string deviceInfo;
};

class IssueLogger {
public:
    static IssueLogger& instance();

    // Log an error as a GitHub issue
    bool logError(const std::string& errorType,
                  const std::string& errorMessage,
                  const std::string& stackTrace = "",
                  const std::string& context = "");

    // Log a crash report
    bool logCrash(const std::string& crashInfo,
                  const std::string& stackTrace = "",
                  const std::string& deviceInfo = "");

    // Check if GitHub issues are enabled
    bool isEnabled() const;

    // Get recent issues count
    int getRecentIssuesCount() const;

private:
    IssueLogger();
    ~IssueLogger();
    IssueLogger(const IssueLogger&) = delete;
    IssueLogger& operator=(const IssueLogger&) = delete;

    bool createGitHubIssue(const IssueInfo& issue);
    std::string escapeMarkdown(const std::string& text);
    std::string getDeviceInfo();

    bool m_enabled;
    std::string m_githubToken;
    std::string m_repoOwner;
    std::string m_repoName;
    int m_issueCount;
};

} // namespace RomCloud
