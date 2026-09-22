#include "FileSystemManager.h"
#include "../config/AppConfig.h"
#include "../logging/Logger.h"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <sstream>
#include <iomanip>

namespace RomCloud {

FileSystemManager& FileSystemManager::instance() {
    static FileSystemManager instance;
    return instance;
}

bool FileSystemManager::directoryExists(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) return false;
    return (info.st_mode & S_IFDIR) != 0;
}

bool FileSystemManager::createDirectoryRecursive(const std::string& path) {
    if (path.empty()) return false;
    if (directoryExists(path)) return true;

    std::string currentPath = "";
    size_t pos = 0;
    if (path[0] == '/') {
        currentPath = "/";
        pos = 1;
    }

    while (pos < path.length()) {
        size_t nextPos = path.find('/', pos);
        std::string component;
        if (nextPos == std::string::npos) {
            component = path.substr(pos);
            pos = path.length();
        } else {
            component = path.substr(pos, nextPos - pos);
            pos = nextPos + 1;
        }

        if (component.empty()) continue;

        if (currentPath.empty() || currentPath == "/") {
            currentPath += component;
        } else {
            currentPath += "/" + component;
        }

        if (!directoryExists(currentPath)) {
            if (mkdir(currentPath.c_str(), 0755) != 0) {
                if (!directoryExists(currentPath)) {
                    Logger::error("Failed to create directory: " + currentPath);
                    return false;
                }
            }
        }
    }
    return true;
}

bool FileSystemManager::fileExists(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) return false;
    return (info.st_mode & S_IFREG) != 0;
}

uint64_t FileSystemManager::getFileSize(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) return 0;
    return static_cast<uint64_t>(info.st_size);
}

DiskSpaceInfo FileSystemManager::getDiskSpace(const std::string& path) {
    DiskSpaceInfo info;
    struct statvfs stat;
    if (statvfs(path.c_str(), &stat) == 0) {
        info.totalBytes = static_cast<uint64_t>(stat.f_blocks) * stat.f_frsize;
        info.freeBytes = static_cast<uint64_t>(stat.f_bfree) * stat.f_frsize;
        info.availableBytes = static_cast<uint64_t>(stat.f_bavail) * stat.f_frsize;
    }
    return info;
}

bool FileSystemManager::removeFile(const std::string& path) {
    if (!fileExists(path)) return true;
    return unlink(path.c_str()) == 0;
}

std::string FileSystemManager::formatBytes(uint64_t bytes) {
    if (bytes == 0) return "-";
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unitIndex = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unitIndex < 4) {
        size /= 1024.0;
        unitIndex++;
    }

    std::stringstream ss;
    ss << std::fixed << std::setprecision(1) << size << " " << units[unitIndex];
    return ss.str();
}

bool FileSystemManager::initializeAppDirectories() {
    const auto& config = AppConfig::instance();
    bool success = true;

    success &= createDirectoryRecursive(config.getBinDir());
    success &= createDirectoryRecursive(config.getConfigDir());
    success &= createDirectoryRecursive(config.getDataDir());
    success &= createDirectoryRecursive(config.getCacheDir());
    success &= createDirectoryRecursive(config.getCoversDir());
    success &= createDirectoryRecursive(config.getMetadataDir());
    success &= createDirectoryRecursive(config.getLogsDir());
    success &= createDirectoryRecursive(config.getTempDir());
    success &= createDirectoryRecursive(config.getAssetsDir());
    success &= createDirectoryRecursive(config.getFontsDir());

    if (success) {
        Logger::info("Application directory structure verified under: " + config.getAppRoot());
    } else {
        Logger::error("Failed to create complete application directory structure");
    }

    return success;
}

} // namespace RomCloud
