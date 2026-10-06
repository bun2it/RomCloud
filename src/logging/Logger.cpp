#include "Logger.h"
#include "../platform/PlatformInfo.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/sysinfo.h>
#endif
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <sys/utsname.h>

namespace RomCloud {

// =============================================================================
// Free helpers
// =============================================================================
static std::string getKernelRelease() {
    struct utsname u;
    if (uname(&u) == 0) return u.release;
    return "?";
}

static std::string getLocalIp() {
    struct ifaddrs *ifaddr = nullptr;
    if (getifaddrs(&ifaddr) < 0) return "0.0.0.0";
    std::string ip = "0.0.0.0";
    for (auto* q = ifaddr; q; q = q->ifa_next) {
        if (!q->ifa_addr) continue;
        if (q->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(q->ifa_name, "wlan0") == 0 ||
            strcmp(q->ifa_name, "eth0")   == 0 ||
            strcmp(q->ifa_name, "ap0")    == 0) {
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &((sockaddr_in*)q->ifa_addr)->sin_addr, buf, sizeof(buf));
            ip = buf;
            if (strcmp(q->ifa_name, "wlan0") == 0) break;
        }
    }
    if (ifaddr) freeifaddrs(ifaddr);
    return ip;
}

// =============================================================================
// Logger impl
// =============================================================================
Logger& Logger::instance() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    flush();
    if (m_logFile.is_open()) m_logFile.close();
}

void Logger::init(const std::string& logFilePath,
                  const std::string& appVersion,
                  const std::string& commitHash) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_filePath = logFilePath;
    m_appVersion = appVersion;
    m_commitHash = commitHash;
    if (m_logFile.is_open()) m_logFile.close();

    // Rotate if > 512KB
    struct stat st;
    if (stat(logFilePath.c_str(), &st) == 0 && st.st_size > 512 * 1024) {
        std::string oldPath = logFilePath + ".old";
        rename(logFilePath.c_str(), oldPath.c_str());
    }

    m_logFile.open(logFilePath, std::ios::out | std::ios::app);
    m_initialized = m_logFile.is_open();

    // KHONG goi logSessionHeader o day de tranh deadlock: header can PlatformInfo
    // ma PlatformInfo constructor co the goi Logger::info => re-entrant mutex.
    // Caller (Application.cpp) goi Logger::instance().logSessionHeader() sau init.
}

void Logger::logSessionHeader() {
    if (!m_initialized) return;
    // Lay diagnostics TRUOC khi giu mutex, vi PlatformInfo constructor co the goi
    // Logger::info (qua getDiskSpace, v.v.) => se gay deadlock neu minh giu mutex.
    SystemDiagnostics diag;
    std::string ipStr;
    {
        // Constructor PlatformInfo co the log qua Logger => unlock tam thoi
        // Su dung critical section rieng: goi getDiagnostics khong giu mutex Logger
        diag = PlatformInfo::instance().getDiagnostics();
        ipStr = getLocalIp();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_logFile.is_open()) return;
    m_logFile << "======================================================================\n";
    m_logFile << "[DEBUG LOG] RomCloud v" << m_appVersion << " Session Started";
    if (!m_commitHash.empty()) m_logFile << " (commit=" << m_commitHash << ")";
    m_logFile << "\n";
    m_logFile << "  device=" << diag.socName << " arch=" << diag.cpuArch
              << " os=" << diag.osName << "-" << getKernelRelease()
              << " ip=" << ipStr << "\n";
    m_logFile << "  ram=" << diag.freeRam << "/" << diag.totalRam
              << " disk=" << diag.freeSpace << "/" << diag.totalSpace << "\n";
    m_logFile << "  Log: " << m_filePath << "\n";
    m_logFile << "======================================================================\n";
    m_logFile.flush();
}

void Logger::header(const std::string& message) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::cout << message << std::endl;
    if (m_initialized && m_logFile.is_open()) {
        m_logFile << message << "\n";
        m_logFile.flush();
    }
}

void Logger::log(LogLevel level, LogCategory cat, const std::string& message,
                 const char* file, int line, const char* func) {
    // Snapshot state TRUOC khi lock mutex (getStateSnapshot co the goi PlatformInfo/Logger::info)
    std::string stateSnap;
    if (level == LogLevel::WARNING || level == LogLevel::LOG_ERROR) {
        stateSnap = getStateSnapshot();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string ts   = formatTimestamp();
    std::string lvl  = levelName(level);
    std::string c    = categoryName(cat);
    std::string safe = sanitize(message);

    std::string out = "[" + ts + "] [" + lvl + "] [" + c + "] " + safe;
    if (file) {
        out += "\n  -> at " + shortFile(file) + ":" + std::to_string(line);
        if (func) out += " " + std::string(func) + "()";
    }
    if (!stateSnap.empty()) {
        out += "\n  -> state: " + stateSnap;
    }
    out += "\n";

    std::cout << out;
    if (m_initialized && m_logFile.is_open()) {
        m_logFile << out;
        m_logFile.flush();
    }
}

void Logger::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized && m_logFile.is_open()) m_logFile.flush();
}

// Category-aware dispatch
void Logger::debug(LogCategory cat, const std::string& msg, const char* file, int line, const char* func) {
    instance().log(LogLevel::DEBUG, cat, msg, file, line, func);
}
void Logger::info (LogCategory cat, const std::string& msg, const char* file, int line, const char* func) {
    instance().log(LogLevel::INFO, cat, msg, file, line, func);
}
void Logger::warn (LogCategory cat, const std::string& msg, const char* file, int line, const char* func) {
    instance().log(LogLevel::WARNING, cat, msg, file, line, func);
}
void Logger::error(LogCategory cat, const std::string& msg, const char* file, int line, const char* func) {
    instance().log(LogLevel::LOG_ERROR, cat, msg, file, line, func);
}

// Back-compat (CORE, no file/line)
void Logger::debug(const std::string& msg) { instance().log(LogLevel::DEBUG, LogCategory::CORE, msg); }
void Logger::info (const std::string& msg) { instance().log(LogLevel::INFO,  LogCategory::CORE, msg); }
void Logger::warn (const std::string& msg) { instance().log(LogLevel::WARNING, LogCategory::CORE, msg); }
void Logger::error(const std::string& msg) { instance().log(LogLevel::LOG_ERROR, LogCategory::CORE, msg); }

// =============================================================================
// Helpers impl
// =============================================================================
std::string Logger::formatTimestamp() const {
    auto now = std::chrono::system_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()).count() % 1000;
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms << "Z";
    return ss.str();
}

std::string Logger::levelName(LogLevel l) const {
    switch (l) {
        case LogLevel::DEBUG:     return "DEBUG";
        case LogLevel::INFO:      return "INFO";
        case LogLevel::WARNING:   return "WARN";
        case LogLevel::LOG_ERROR: return "ERROR";
    }
    return "?";
}

std::string Logger::categoryName(LogCategory c) const {
    switch (c) {
        case LogCategory::CORE:      return "CORE";
        case LogCategory::UI:        return "UI";
        case LogCategory::STATE:     return "STATE";
        case LogCategory::NETWORK:   return "NETWORK";
        case LogCategory::IPTV:      return "IPTV";
        case LogCategory::MPV:       return "MPV";
        case LogCategory::OSD:       return "OSD";
        case LogCategory::YT:        return "YT";
        case LogCategory::LOCALSEND: return "LOCALSEND";
        case LogCategory::SYNC:      return "SYNC";
        case LogCategory::OTA:       return "OTA";
        case LogCategory::SCRAPER:   return "SCRAPER";
        case LogCategory::FS:        return "FS";
        case LogCategory::AUTH:      return "AUTH";
        case LogCategory::BACKUP:    return "BACKUP";
        case LogCategory::DIAG:      return "DIAG";
        case LogCategory::UNKNOWN:   return "UNKNOWN";
    }
    return "UNKNOWN";
}

std::string Logger::shortFile(const char* path) {
    if (!path) return "?";
    const char* slash = strrchr(path, '/');
    return slash ? (slash + 1) : path;
}

std::string Logger::sanitize(const std::string& msg) const {
    std::string out = msg;
    static const char* keys[] = {
        "api_key=", "apikey=", "api-key=",
        "password=", "passwd=",
        "token=", "access_token=",
        "Bearer ", "Authorization: Bearer ",
        "secret=",
    };
    for (const char* k : keys) {
        std::string kLower(k);
        for (auto& ch : kLower) if (ch >= 'A' && ch <= 'Z') ch += 32;
        std::string outLower = out;
        for (auto& ch : outLower) if (ch >= 'A' && ch <= 'Z') ch += 32;
        size_t pos = 0;
        while ((pos = outLower.find(kLower, pos)) != std::string::npos) {
            size_t end = out.find_first_of(" &,;\"\t\n\r", pos + kLower.size());
            if (end == std::string::npos) end = out.size();
            out.replace(pos + kLower.size(), end - (pos + kLower.size()), "***");
            outLower.replace(pos + kLower.size(), end - (pos + kLower.size()), "***");
            pos = end + 3;
            if (pos >= out.size()) break;
        }
    }
    return out;
}

std::string Logger::getStateSnapshot() const {
    std::stringstream ss;
#if defined(__linux__)
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        long total = (long)((si.totalram * si.mem_unit) / (1024 * 1024));
        long free  = (long)((si.freeram  * si.mem_unit) / (1024 * 1024));
        ss << "ram=" << free << "/" << total << "MB";
    }
#endif
    ss << " ip=" << getLocalIp();
    return ss.str();
}

} // namespace RomCloud
