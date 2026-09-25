#include "Logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>

namespace RomCloud {

Logger& Logger::instance() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    flush();
    if (m_logFile.is_open()) m_logFile.close();
}

void Logger::init(const std::string& logFilePath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_filePath = logFilePath;
    if (m_logFile.is_open()) m_logFile.close();

    // Check file size, if > 512KB rotate to keep debug log compact
    struct stat st;
    if (stat(logFilePath.c_str(), &st) == 0 && st.st_size > 512 * 1024) {
        std::string oldPath = logFilePath + ".old";
        rename(logFilePath.c_str(), oldPath.c_str());
    }

    m_logFile.open(logFilePath, std::ios::out | std::ios::app);
    m_initialized = m_logFile.is_open();
}

void Logger::header(const std::string& message) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::cout << message << std::endl;
    if (m_initialized && m_logFile.is_open()) {
        m_logFile << message << std::endl;
        m_logFile.flush();
    }
}

void Logger::log(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S");

    const char* levelStr = "INFO";
    switch (level) {
        case LogLevel::DEBUG:     levelStr = "DEBUG"; break;
        case LogLevel::INFO:      levelStr = "INFO";  break;
        case LogLevel::WARNING:   levelStr = "WARN";  break;
        case LogLevel::LOG_ERROR: levelStr = "ERROR"; break;
    }

    std::string line = "[" + ss.str() + "] [" + levelStr + "] " + message;
    std::cout << line << std::endl;

    // Filter out INFO and DEBUG from debug log file on disk!
    // ONLY save WARNING and ERROR to disk so the log is clean and focused for developer debugging!
    if (level == LogLevel::WARNING || level == LogLevel::LOG_ERROR) {
        if (m_initialized && m_logFile.is_open()) {
            m_logFile << line << std::endl;
            m_logFile.flush();
        }
    }
}

void Logger::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized && m_logFile.is_open()) m_logFile.flush();
}

void Logger::debug(const std::string& msg) { instance().log(LogLevel::DEBUG, msg); }
void Logger::info(const std::string& msg)  { instance().log(LogLevel::INFO, msg); }
void Logger::warn(const std::string& msg)  { instance().log(LogLevel::WARNING, msg); }
void Logger::error(const std::string& msg) { instance().log(LogLevel::LOG_ERROR, msg); }

} // namespace RomCloud
