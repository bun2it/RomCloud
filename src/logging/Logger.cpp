#include "Logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

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
    if (m_logFile.is_open()) m_logFile.close();
    m_logFile.open(logFilePath, std::ios::out | std::ios::app);
    m_initialized = m_logFile.is_open();
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
    if (m_initialized && m_logFile.is_open()) {
        m_logFile << line << std::endl;
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
