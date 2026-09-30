#include "FileSystemManager.h"
#include "../common/BackgroundTask.h"
#include "../config/AppConfig.h"
#include "../logging/Logger.h"

#include <sys/stat.h>
#ifndef _WIN32
#include <sys/statvfs.h>
#include <unistd.h>
#else
#include <direct.h>
#include <io.h>
#ifndef lstat
#define lstat stat
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) 0
#endif
#endif
#include <dirent.h>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
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
#ifndef _WIN32
    struct statvfs stat;
    if (statvfs(path.c_str(), &stat) == 0) {
        info.totalBytes = static_cast<uint64_t>(stat.f_blocks) * stat.f_frsize;
        info.freeBytes = static_cast<uint64_t>(stat.f_bfree) * stat.f_frsize;
        info.availableBytes = static_cast<uint64_t>(stat.f_bavail) * stat.f_frsize;
    }
#else
    (void)path;
    info.totalBytes = 64ULL * 1024 * 1024 * 1024;
    info.freeBytes = 32ULL * 1024 * 1024 * 1024;
    info.availableBytes = 32ULL * 1024 * 1024 * 1024;
#endif
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

std::vector<DirEntryInfo> FileSystemManager::listDirectory(const std::string& path, bool dirsOnly) {
    std::vector<DirEntryInfo> out;
    if (path.empty()) return out;

    DIR* d = opendir(path.c_str());
    if (!d) {
        Logger::warn("listDirectory: cannot open " + path);
        return out;
    }

    struct dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
        const char* name = ent->d_name;
        // Skip . / ..
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;

        std::string fullPath = path;
        if (!fullPath.empty() && fullPath.back() != '/') fullPath += '/';
        fullPath += name;

        struct stat st;
        bool isDir = false;
        uint64_t sz = 0;
        if (stat(fullPath.c_str(), &st) == 0) {
            if ((st.st_mode & S_IFDIR) != 0) {
                isDir = true;
            } else if ((st.st_mode & S_IFREG) != 0) {
                sz = static_cast<uint64_t>(st.st_size);
            } else {
                continue; // skip symlinks, devices, etc.
            }
        } else {
            continue;
        }

        if (dirsOnly && !isDir) continue;

        DirEntryInfo e;
        e.name = name;
        e.path = fullPath;
        e.isDirectory = isDir;
        e.sizeBytes = sz;
        out.push_back(e);
    }
    closedir(d);

    // Sort: directories first, then files, alphabetically within each group
    std::sort(out.begin(), out.end(), [](const DirEntryInfo& a, const DirEntryInfo& b) {
        if (a.isDirectory != b.isDirectory) return a.isDirectory;  // dirs first
        return a.name < b.name;
    });

    return out;
}

bool FileSystemManager::renamePath(const std::string& from, const std::string& to) {
    if (from.empty() || to.empty() || from == to) return false;
    struct stat st;
    if (stat(from.c_str(), &st) != 0) return false;
    if (stat(to.c_str(), &st) == 0) return false; // khong ghi de dich da ton tai
    if (rename(from.c_str(), to.c_str()) != 0) {
        Logger::error("renamePath failed: " + from + " -> " + to);
        return false;
    }
    return true;
}

bool FileSystemManager::isSubPath(const std::string& parent, const std::string& child) {
    if (parent.empty() || child.empty()) return false;
    std::string p = parent;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    if (child == p) return true;
    if (child.size() <= p.size()) return false;
    if (child.compare(0, p.size(), p) != 0) return false;
    return child[p.size()] == '/';
}

namespace {
bool removeRecursiveImpl(const std::string& path, RomCloud::TaskProgress* prog, bool& cancelled) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return true;
    if (prog && prog->cancel.load()) { cancelled = true; return false; }
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR* d = opendir(path.c_str());
        if (!d) return false;
        struct dirent* ent = nullptr;
        bool ok = true;
        while ((ent = readdir(d)) != nullptr) {
            const char* name = ent->d_name;
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
            if (!removeRecursiveImpl(path + "/" + name, prog, cancelled)) { ok = false; break; }
            if (cancelled) { ok = false; break; }
        }
        closedir(d);
        if (!ok) return false;
        if (rmdir(path.c_str()) != 0) return false;
        if (prog) prog->done++;
        return true;
    }
    if (unlink(path.c_str()) != 0) return false;
    if (prog) prog->done++;
    return true;
}

bool copyFileImpl(const std::string& src, const std::string& dst, RomCloud::TaskProgress* prog, bool& cancelled) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    char buf[128 * 1024];
    while (in) {
        if (prog && prog->cancel.load()) { cancelled = true; return false; }
        in.read(buf, sizeof(buf));
        std::streamsize n = in.gcount();
        if (n <= 0) break;
        out.write(buf, n);
        if (!out) return false;
        if (prog) prog->done += static_cast<uint64_t>(n);
    }
    return true;
}

bool copyRecursiveImpl(const std::string& src, const std::string& dst, RomCloud::TaskProgress* prog, bool& cancelled) {
    struct stat st;
    if (stat(src.c_str(), &st) != 0) return false;
    if (prog && prog->cancel.load()) { cancelled = true; return false; }
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst.c_str(), 0755) != 0) {
            struct stat dstSt;
            if (stat(dst.c_str(), &dstSt) != 0 || !S_ISDIR(dstSt.st_mode)) return false;
        }
        DIR* d = opendir(src.c_str());
        if (!d) return false;
        struct dirent* ent = nullptr;
        bool ok = true;
        while ((ent = readdir(d)) != nullptr) {
            const char* name = ent->d_name;
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
            std::string s = src + "/" + name;
            std::string t = dst + "/" + name;
            if (!copyRecursiveImpl(s, t, prog, cancelled)) { ok = false; break; }
            if (cancelled) { ok = false; break; }
        }
        closedir(d);
        return ok;
    }
    if (S_ISREG(st.st_mode)) return copyFileImpl(src, dst, prog, cancelled);
    return false;
}
} // anonymous namespace

bool FileSystemManager::removeRecursive(const std::string& path, TaskProgress* prog) {
    if (path.empty() || path == "/" || path == "/mnt" || path == "/mnt/SDCARD") return false;
    bool cancelled = false;
    if (prog) {
        DirStats s = getDirStats(path);
        prog->total = static_cast<uint64_t>(s.files + s.dirs + 1);
        prog->done = 0;
    }
    bool ok = removeRecursiveImpl(path, prog, cancelled);
    if (cancelled && prog) prog->error = "Cancelled";
    return ok && !cancelled;
}

bool FileSystemManager::copyRecursive(const std::string& src, const std::string& dst, TaskProgress* prog) {
    if (src.empty() || dst.empty() || src == dst) return false;
    if (isSubPath(src, dst)) return false;
    struct stat st;
    if (stat(src.c_str(), &st) != 0) return false;
    if (stat(dst.c_str(), &st) == 0) return false; // dich da ton tai
    if (prog) {
        DirStats s = getDirStats(src);
        prog->total = s.bytes > 0 ? s.bytes : 1;
        prog->done = 0;
    }
    bool cancelled = false;
    bool ok = copyRecursiveImpl(src, dst, prog, cancelled);
    if (!ok) {
        bool c2 = false;
        removeRecursiveImpl(dst, nullptr, c2); // don dich do dang
    }
    if (cancelled && prog) prog->error = "Cancelled";
    return ok && !cancelled;
}

DirStats FileSystemManager::getDirStats(const std::string& path) {
    DirStats out;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return out;
    if (S_ISREG(st.st_mode)) { out.files = 1; out.bytes = static_cast<uint64_t>(st.st_size); return out; }
    if (!S_ISDIR(st.st_mode)) return out;
    std::vector<std::string> stack{path};
    while (!stack.empty()) {
        std::string cur = stack.back();
        stack.pop_back();
        DIR* d = opendir(cur.c_str());
        if (!d) continue;
        struct dirent* ent = nullptr;
        while ((ent = readdir(d)) != nullptr) {
            const char* name = ent->d_name;
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
            std::string full = cur + "/" + name;
            struct stat e;
            if (stat(full.c_str(), &e) != 0) continue;
            if (S_ISDIR(e.st_mode)) { out.dirs++; stack.push_back(full); }
            else if (S_ISREG(e.st_mode)) { out.files++; out.bytes += static_cast<uint64_t>(e.st_size); }
        }
        closedir(d);
    }
    return out;
}

int64_t FileSystemManager::getModTime(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<int64_t>(st.st_mtimespec.tv_sec);
#elif defined(_WIN32)
    return static_cast<int64_t>(st.st_mtime);
#else
    return static_cast<int64_t>(st.st_mtim.tv_sec);
#endif
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
