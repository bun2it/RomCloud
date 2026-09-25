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

class Logger {
public:
    static Logger& instance();
    void init(const std::string& logFilePath);
    void log(LogLevel level, const std::string& message);
    void header(const std::string& message);
    void flush();

    static void debug(const std::string& msg);
    static void info(const std::string& msg);
    static void warn(const std::string& msg);
    static void error(const std::string& msg);

    const std::string& getLogFilePath() const { return m_filePath; }

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::ofstream m_logFile;
    std::mutex m_mutex;
    std::string m_filePath;
    bool m_initialized = false;
};

} // namespace RomCloud
