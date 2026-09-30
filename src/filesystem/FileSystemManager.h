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

struct DirEntryInfo {
    std::string name;        // file/folder name (basename)
    std::string path;        // full path
    bool isDirectory = false;
    uint64_t sizeBytes = 0;  // 0 for directories
};

// Returns sorted listing: directories first, then files (alpha).
// If `dirsOnly` true → return only directories.
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

    // List thư mục. Trả về vector rỗng nếu dir không tồn tại hoặc lỗi.
    // `dirsOnly` = true nếu chỉ cần sub-folders (vd cho LocalSend Apps tab).
    std::vector<DirEntryInfo> listDirectory(const std::string& path, bool dirsOnly = false);

private:
    FileSystemManager() = default;
};

} // namespace RomCloud
