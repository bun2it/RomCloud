#pragma once
#include <string>
#include <fstream>
#include <mutex>

namespace RomCloud {

enum class LogLevel {
    DEBUG,
    INFO,
    WARNING,
    LOG_ERROR
};

enum class LogCategory {
    CORE,
    UI,
    STATE,
    NETWORK,
    IPTV,
    MPV,
    OSD,
    YT,
    LOCALSEND,
    SYNC,
    OTA,
    SCRAPER,
    FS,
    AUTH,
    BACKUP,
    DIAG,
    UNKNOWN
};

class Logger {
public:
    static Logger& instance();

    // init: ghi ra 1 file duy nhat (debug.log). Version + commit inject vao session header.
    void init(const std::string& logFilePath,
              const std::string& appVersion = "",
              const std::string& commitHash = "");

    // Log core: format [ISO8601 UTC ms] [LEVEL] [CAT] msg \n -> file:line func()
    //           + state snapshot neu WARN/ERROR
    void log(LogLevel level, LogCategory cat, const std::string& message,
             const char* file = nullptr, int line = 0, const char* func = nullptr);

    // Banner co dinh (khong qua filter level).
    void header(const std::string& message);

    // Dump device/RAM/app version/commit vao 1 banner o dau session.
    void logSessionHeader();

    void flush();

    // API moi co category + auto file:line:func qua macro RC_LOG_*
    static void debug(LogCategory cat, const std::string& msg,
                      const char* file = nullptr, int line = 0, const char* func = nullptr);
    static void info (LogCategory cat, const std::string& msg,
                      const char* file = nullptr, int line = 0, const char* func = nullptr);
    static void warn (LogCategory cat, const std::string& msg,
                      const char* file = nullptr, int line = 0, const char* func = nullptr);
    static void error(LogCategory cat, const std::string& msg,
                      const char* file = nullptr, int line = 0, const char* func = nullptr);

    // Back-compat: cu, default CORE, khong co file:line
    static void debug(const std::string& msg);
    static void info (const std::string& msg);
    static void warn (const std::string& msg);
    static void error(const std::string& msg);

    const std::string& getLogFilePath() const { return m_filePath; }

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // helpers
    std::string formatTimestamp() const;          // ISO 8601 UTC + ms
    std::string levelName(LogLevel l) const;      // DEBUG/INFO/WARN/ERROR
    std::string categoryName(LogCategory c) const;
    std::string sanitize(const std::string& msg) const;  // mask api_key/passw/token
    std::string getStateSnapshot() const;         // ram/ip/wifi cho ERROR/WARN
    static std::string shortFile(const char* path);  // chi lay basename

    std::ofstream m_logFile;
    std::mutex m_mutex;
    std::string m_filePath;
    std::string m_appVersion;
    std::string m_commitHash;
    bool m_initialized = false;
};

// =============================================================================
// Macros tien dung: tu dong capture __FILE__, __LINE__, __func__.
// Dung:  RC_LOG_INFO(IPTV, "Channel switch: " + std::to_string(idx));
// =============================================================================
#define RC_LOG_DBG(cat, msg) \
    ::RomCloud::Logger::debug(::RomCloud::LogCategory::cat, msg, \
                             __FILE__, __LINE__, __func__)
#define RC_LOG_INFO(cat, msg) \
    ::RomCloud::Logger::info(::RomCloud::LogCategory::cat, msg, \
                            __FILE__, __LINE__, __func__)
#define RC_LOG_WARN(cat, msg) \
    ::RomCloud::Logger::warn(::RomCloud::LogCategory::cat, msg, \
                            __FILE__, __LINE__, __func__)
#define RC_LOG_ERR(cat, msg) \
    ::RomCloud::Logger::error(::RomCloud::LogCategory::cat, msg, \
                             __FILE__, __LINE__, __func__)

} // namespace RomCloud
