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

    // Enable/disable issue reporting
    void setEnabled(bool enabled);

    // Log an error as a GitHub issue
    bool logError(const std::string& errorType,
                  const std::string& errorMessage,
                  const std::string& stackTrace = "",
                  const std::string& context = "");

    // Log a crash report
    bool logCrash(const std::string& crashInfo,
                  const std::string& stackTrace = "",
                  const std::string& deviceInfo = "");

    // Check if issue reporting is enabled
    bool isEnabled() const;

    // Get recent issues count
    int getRecentIssuesCount() const;

    // Send a manual bug report
    bool sendManualReport();

    // Filter sensitive data before sending
    static std::string filterSensitiveData(const std::string& text);

private:
    IssueLogger();
    ~IssueLogger();
    IssueLogger(const IssueLogger&) = delete;
    IssueLogger& operator=(const IssueLogger&) = delete;

    bool createGitHubIssue(const IssueInfo& issue);
    std::string getDeviceInfo();
    std::string getCurrentTimestamp();
    bool sendToWorker(const std::string& payload);

    bool m_enabled;
    int m_issueCount;
};

} // namespace RomCloud
