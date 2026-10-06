#include "IssueLogger.h"
#include "../config/AppConfig.h"
#include "../platform/PlatformInfo.h"
#include "../logging/Logger.h"
#include "../database/DatabaseManager.h"
#include "../ota/UpdateManager.h"
#include "../diagnostics/DeviceIdentity.h"

#include <curl/curl.h>
#include <sys/utsname.h>
#include <time.h>
#include <sstream>
#include <iomanip>
#include <regex>
#include <mutex>
#include <fstream>

// Worker relay endpoint
constexpr const char* WORKER_ENDPOINT = "https://romcloud-issue-relay.bun2it.workers.dev";

namespace RomCloud {

static std::mutex g_curlMutex;

IssueLogger& IssueLogger::instance() {
    static IssueLogger instance;
    return instance;
}

IssueLogger::IssueLogger()
    : m_enabled(false), m_issueCount(0) {

    // Check if reporting is enabled in settings
    std::string enabled = DatabaseManager::instance().getSetting("issue_reporting_enabled", "false");
    m_enabled = (enabled == "true");

    if (m_enabled) {
        Logger::info("IssueLogger: Issue reporting enabled (using Worker relay)");
    } else {
        Logger::debug("IssueLogger: Issue reporting disabled");
    }
}

IssueLogger::~IssueLogger() {
}

void IssueLogger::setEnabled(bool enabled) {
    m_enabled = enabled;
    DatabaseManager::instance().setSetting("issue_reporting_enabled", enabled ? "true" : "false");
    Logger::info(std::string("IssueLogger: Reporting ") + (enabled ? "enabled" : "disabled"));
}

bool IssueLogger::isEnabled() const {
    return m_enabled;
}

int IssueLogger::getRecentIssuesCount() const {
    return m_issueCount;
}

// Filter sensitive data from text
std::string IssueLogger::filterSensitiveData(const std::string& text) {
    if (text.empty()) return text;

    std::string filtered = text;

    // Token patterns
    filtered = std::regex_replace(filtered, std::regex("ghp_[a-zA-Z0-9]{36}"), "[GITHUB_TOKEN]");
    filtered = std::regex_replace(filtered, std::regex("github_pat_[a-zA-Z0-9_]{80,}"), "[GITHUB_TOKEN]");

    // Password/API key patterns
    filtered = std::regex_replace(filtered, std::regex(R"(password["\s:=]+[^\s,}]+)"), "password=[REDACTED]");
    filtered = std::regex_replace(filtered, std::regex(R"(api[_-]?key["\s:=]+[^\s,}]+)"), "api_key=[REDACTED]");
    filtered = std::regex_replace(filtered, std::regex(R"(secret["\s:=]+[^\s,}]+)"), "secret=[REDACTED]");

    // IP addresses (internal ranges)
    filtered = std::regex_replace(filtered, std::regex(R"(\b10\.\d{1,3}\.\d{1,3}\.\d{1,3}\b)"), "[PRIVATE_IP]");
    filtered = std::regex_replace(filtered, std::regex(R"(\b192\.168\.\d{1,3}\.\d{1,3}\b)"), "[PRIVATE_IP]");
    filtered = std::regex_replace(filtered, std::regex(R"(\b172\.(1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3}\b)"), "[PRIVATE_IP]");

    // MAC addresses
    filtered = std::regex_replace(filtered, std::regex("([0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}"), "[MAC]");

    // Email addresses
    filtered = std::regex_replace(filtered, std::regex("[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}"), "[EMAIL]");

    // File paths (user home directories)
    filtered = std::regex_replace(filtered, std::regex(R"(/home/[a-zA-Z0-9_]+/)"), "/home/[user]/");
    filtered = std::regex_replace(filtered, std::regex(R"(/Users/[a-zA-Z0-9_.]+/)"), "/Users/[user]/");
    filtered = std::regex_replace(filtered, std::regex(R"(C:\\Users\\[a-zA-Z0-9_.]+\\)"), "C:\\Users\\[user]\\");

    // Long hex strings (likely raw hardware IDs)
    filtered = std::regex_replace(filtered, std::regex(R"(\b[a-fA-F0-9]{16,}\b)"), "[HW_ID]");

    return filtered;
}

std::string IssueLogger::getDeviceInfo() {
    std::ostringstream oss;
    auto diag = PlatformInfo::instance().getDiagnostics();
    auto deviceId = DeviceIdentity::instance().getDeviceId();

    oss << "- **App Version:** v" << APP_VERSION << "\n";
    oss << "- **Device ID:** " << deviceId << "\n";
    oss << "- **Hardware ID Source:** " << DeviceIdentity::instance().getIdSource() << "\n";
    oss << "- **Device:** " << diag.socName << "\n";
    oss << "- **OS:** " << diag.osName << " " << diag.kernelRelease << "\n";
    oss << "- **Display:** " << diag.displayResolution << "\n";
    oss << "- **RAM:** " << diag.freeRam << " / " << diag.totalRam << "\n";
    oss << "- **Storage:** " << diag.freeRam << " / " << diag.totalSpace << "\n";
    oss << "- **Network:** " << diag.networkStatus;
    if (diag.ipAddress != "N/A") {
        oss << " (IP: [REDACTED_IP])";  // Always redact IP in reports
    }
    oss << "\n";

    return oss.str();
}

bool IssueLogger::sendToWorker(const std::string& payload) {
    std::lock_guard<std::mutex> lock(g_curlMutex);

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::string url = std::string(WORKER_ENDPOINT) + "/report";

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    // Cloudflare reject request không có User-Agent (error 1010). Issue v1.0 worker trên .dev.
    const std::string userAgent = std::string("User-Agent: RomCloud/") + APP_VERSION;
    headers = curl_slist_append(headers, userAgent.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);  // Disable for TrimUI (limited certs)
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);

    // Response buffer
    std::string response;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t size, size_t nmemb, std::string* stream) -> size_t {
        stream->append((char*)ptr, size * nmemb);
        return size * nmemb;
    });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (res == CURLE_OK) {
        Logger::debug("IssueLogger: Worker HTTP " + std::to_string(httpCode) + ", response: " + response);
        if (httpCode == 200 && response.find("\"success\":true") != std::string::npos) {
            Logger::debug("IssueLogger: Report sent via Worker");
            return true;
        }
        Logger::warn("IssueLogger: Worker returned HTTP " + std::to_string(httpCode));
    } else {
        Logger::error("IssueLogger: Worker request failed: " + std::string(curl_easy_strerror(res)));
    }

    return false;
}

// Escape string for JSON
std::string escapeJsonString(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    for (char c : input) {
        switch (c) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            default:
                if (c >= 0 && c < 32) {
                    result += "\\u";
                    char buf[8];
                    snprintf(buf, sizeof(buf), "%04x", (unsigned char)c);
                    result += buf;
                } else {
                    result += c;
                }
                break;
        }
    }
    return result;
}

bool IssueLogger::createGitHubIssue(const IssueInfo& issue) {
    // m_enabled đã được check tại mỗi caller (logError, logCrash);
    // sendManualReport CỐ Ý bypass để cho phép user gửi kể cả khi auto-reporting OFF.
    // Xem comment trong sendManualReport().

    // Build JSON payload for Worker - simple format
    // Worker reject errorMessage > 5000 chars (validatePayload trong worker/src/index.ts).
    // Truncate thêm 1 lần nữa để safe (recentLogs UTF-8 có thể nở 2-4× khi escape).
    std::ostringstream json;
    std::string errorMessage = filterSensitiveData(issue.body);
    if (errorMessage.size() > 4900) {
        errorMessage.resize(4900);
        errorMessage += "\n\n... [truncated to fit 5000-char Worker limit]";
    }
    json << "{";
    json << "\"deviceId\":\"" << DeviceIdentity::instance().getDeviceId() << "\",";
    json << "\"version\":\"" << APP_VERSION << "\",";
    json << "\"errorType\":\"" << escapeJsonString(filterSensitiveData(issue.title)) << "\",";
    json << "\"errorMessage\":\"" << escapeJsonString(errorMessage) << "\",";
    json << "\"timestamp\":\"" << getCurrentTimestamp() << "\"";
    json << "}";

    Logger::debug("IssueLogger: Sending payload: " + json.str());

    bool success = sendToWorker(json.str());

    if (success) {
        m_issueCount++;
    }

    return success;
}

std::string IssueLogger::getCurrentTimestamp() {
    time_t now = time(nullptr);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    return std::string(buf);
}

bool IssueLogger::logError(const std::string& errorType,
                           const std::string& errorMessage,
                           const std::string& stackTrace,
                           const std::string& context) {
    if (!m_enabled) {
        return false;
    }

    IssueInfo issue;
    issue.title = errorType;

    std::ostringstream body;
    body << "### Error Message\n";
    body << filterSensitiveData(errorMessage) << "\n\n";

    if (!stackTrace.empty()) {
        body << "### Stack Trace\n";
        body << "```\n" << filterSensitiveData(stackTrace) << "\n```\n\n";
    }

    if (!context.empty()) {
        body << "### Context\n";
        body << filterSensitiveData(context) << "\n\n";
    }

    body << "### Device Info\n";
    body << getDeviceInfo();

    issue.body = body.str();
    issue.labels = "auto-reported,bug";

    return createGitHubIssue(issue);
}

bool IssueLogger::logCrash(const std::string& crashInfo,
                           const std::string& stackTrace,
                           const std::string& /*deviceInfo*/) {
    if (!m_enabled) {
        return false;
    }

    IssueInfo issue;
    issue.title = "[CRASH] " + crashInfo;

    std::ostringstream body;
    body << "### Crash Info\n";
    body << "```\n" << filterSensitiveData(crashInfo) << "\n```\n\n";

    if (!stackTrace.empty()) {
        body << "### Stack Trace\n";
        body << "```\n" << filterSensitiveData(stackTrace) << "\n```\n\n";
    }

    body << "### Device Info\n";
    body << getDeviceInfo();

    issue.body = body.str();
    issue.labels = "auto-reported,crash,critical";

    return createGitHubIssue(issue);
}

bool IssueLogger::sendManualReport() {
    // Always allow manual reports regardless of m_enabled (caller enforces for logError/logCrash)
    // Read recent log file
    std::string logPath = AppConfig::instance().getDebugLogPath();
    std::ifstream logFile(logPath);
    std::string recentLogs;

    if (logFile.is_open()) {
        // Worker reject errorMessage > 5000 chars. Reserve budget:
        //   recentLogs (~3KB) + device info header (~600) + markdown wrapper (~200) ≈ 3.8KB
        //   → đọc 3KB logs an toàn dưới 5000-char limit (UTF-8 có thể nở 2-4×).
        logFile.seekg(0, std::ios::end);
        size_t fileSize = logFile.tellg();
        size_t readSize = std::min(fileSize, (size_t)3072); // 3KB (an toàn cho UTF-8)
        logFile.seekg(fileSize - readSize);
        recentLogs.assign((std::istreambuf_iterator<char>(logFile)),
                          std::istreambuf_iterator<char>());
        logFile.close();
    }

    IssueInfo issue;
    issue.title = "[Manual] User submitted report";
    issue.labels = "manual-report,user-feedback";

    std::ostringstream body;
    body << "### User Manual Report\n";
    body << "```\n" << filterSensitiveData(recentLogs) << "\n```\n\n";
    body << "### Device Info\n";
    body << getDeviceInfo();

    issue.body = body.str();

    return createGitHubIssue(issue);
}

} // namespace RomCloud
