#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace RomCloud {

// Forward declaration — tránh include <atomic> nặng ở header filesystem.
struct TaskProgress;

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

struct DirStats {
    uint64_t bytes = 0;
    size_t files = 0;
    size_t dirs = 0;
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

    // ---- FileOps (MODULE plan P1-2): thuần filesystem, không UI/thread ----
    // Đổi tên file/folder. Trả false nếu đích đã tồn tại hoặc lỗi.
    bool renamePath(const std::string& from, const std::string& to);
    // Xóa đệ quy file hoặc folder. prog có thể nullptr (chạy đồng bộ, không báo tiến trình).
    bool removeRecursive(const std::string& path, TaskProgress* prog = nullptr);
    // Copy đệ quy file hoặc folder src -> dst. dst là path đích đầy đủ.
    // Trả false nếu dst nằm trong src (tránh vòng lặp vô hạn) hoặc lỗi I/O.
    bool copyRecursive(const std::string& src, const std::string& dst, TaskProgress* prog = nullptr);
    // Thống kê đệ quy: tổng bytes + số file/folder con.
    DirStats getDirStats(const std::string& path);
    // Thời gian sửa đổi gần nhất (mtime, seconds since epoch). 0 nếu lỗi.
    int64_t getModTime(const std::string& path);
    // true nếu `child` nằm trong (hoặc trùng) `parent`. Dùng để chặn paste vào chính nó.
    static bool isSubPath(const std::string& parent, const std::string& child);

private:
    FileSystemManager() = default;
};

} // namespace RomCloud
