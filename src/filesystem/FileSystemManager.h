#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace RomCloud {

struct DiskSpaceInfo {
    uint64_t totalBytes = 0;
    uint64_t freeBytes = 0;
    uint64_t availableBytes = 0;
};

class FileSystemManager {
public:
    static FileSystemManager& instance();
    bool initializeAppDirectories();
    bool directoryExists(const std::string& path);
    bool createDirectoryRecursive(const std::string& path);
    bool fileExists(const std::string& path);
    uint64_t getFileSize(const std::string& path);
    DiskSpaceInfo getDiskSpace(const std::string& path);
    bool removeFile(const std::string& path);
    std::string formatBytes(uint64_t bytes);

private:
    FileSystemManager() = default;
};

} // namespace RomCloud
