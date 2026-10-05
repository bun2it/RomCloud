#pragma once
// ============================================================================
// RomCloud ArchiveEngine — bung .zip/.rar/.7z trong File Explorer.
// Backend: bin/7zzs kèm theo app (static aarch64, không cần libc hệ thống),
// fallback `unzip` của máy cho .zip. Header-only (kiểu BackgroundTask).
// Chạy trong FileExplorer::m_task → progress dialog + hủy có sẵn.
// ============================================================================
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include "../common/BackgroundTask.h"

namespace RomCloud {

class ArchiveEngine {
public:
    // Chỉ 3 định dạng user duyệt (zip/rar/7z).
    static bool isArchive(const std::string& path) {
        std::string ext = lowerExt(path);
        return ext == ".zip" || ext == ".rar" || ext == ".7z";
    }

    struct Tool {
        std::string bin;   // binary cần chạy
        bool use7z = true; // false = unzip hệ thống (chỉ .zip)
        bool ok = false;
    };

    // Tìm backend bung: ưu tiên 7zzs kèm app, rớt xuống unzip cho .zip.
    static Tool findTool(const std::string& appRoot, const std::string& archive) {
        Tool t;
        std::string seven = appRoot + "/bin/7zzs";
        bool extIsZip = (lowerExt(archive) == ".zip");
        if (access(seven.c_str(), X_OK) == 0) {
            t.bin = seven;
            t.use7z = true;
            t.ok = true;
            return t;
        }
        if (extIsZip) {
            t.bin = "unzip";
            t.use7z = false;
            t.ok = true;
            return t;
        }
        return t; // ok=false: rar/7z mà thiếu 7zzs
    }

    static const char* missingToolError() {
        return "Thiếu 7zzs, cập nhật app bản mới!";
    }

    // Bung archive vào dstDir (tự tạo). Báo % qua prog (total=100).
    // prog.cancel=true → kill tiến trình con, prog.error="Cancelled".
    static bool extract(const std::string& toolBin, bool use7z,
                        const std::string& archive, const std::string& dstDir,
                        TaskProgress& prog) {
        prog.total = 100;
        prog.done = 0;

        if (!use7z) {
            // Fallback unzip hệ thống (không có % tiến trình).
            std::string cmd = "mkdir -p '" + dstDir + "' 2>/dev/null";
            if (system(cmd.c_str()) != 0) {}
            cmd = "unzip -o -qq '" + archive + "' -d '" + dstDir + "' 2>/dev/null";
            int rc = system(cmd.c_str());
            if (prog.cancel) { prog.error = "Cancelled"; return false; }
            if (rc != 0) { prog.error = "Bung .zip thất bại"; return false; }
            prog.done = 100;
            return true;
        }

        // 1. Liệt kê kiểm tra: chặn path traversal + ước lượng chỗ trống.
        uint64_t needBytes = 0;
        std::string listErr;
        if (!safeList(toolBin, archive, needBytes, listErr)) {
            prog.error = listErr;
            return false;
        }
        struct statvfs vfs;
        if (needBytes > 0 && statvfs(dstDir.c_str(), &vfs) == 0) {
            uint64_t avail = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
            if (needBytes > avail) {
                prog.error = "Thẻ nhớ không đủ chỗ trống!";
                return false;
            }
        }
        if (mkdir(dstDir.c_str(), 0755) != 0) {
            // Đã tồn tại thì thôi, lỗi khác cũng kệ (7z sẽ báo sau).
        }

        // 2. Bung bằng fork/exec để hủy được giữa chừng.
        int fds[2];
        if (pipe(fds) != 0) {
            prog.error = "Bung thất bại (pipe)";
            return false;
        }
        pid_t pid = fork();
        if (pid < 0) {
            close(fds[0]); close(fds[1]);
            prog.error = "Bung thất bại (fork)";
            return false;
        }
        if (pid == 0) {
            // Tiến trình con: chỉ exec (an toàn sau fork).
            dup2(fds[1], STDOUT_FILENO);
            dup2(fds[1], STDERR_FILENO);
            close(fds[0]); close(fds[1]);
            std::string outArg = "-o" + dstDir;
            execl(toolBin.c_str(), toolBin.c_str(), "x", "-y", "-bsp1",
                  outArg.c_str(), "--", archive.c_str(), (char*)nullptr);
            _exit(127);
        }
        close(fds[1]);
        FILE* fp = fdopen(fds[0], "r");
        char buf[512];
        int lastPct = 0;
        if (fp) {
            while (fgets(buf, sizeof(buf), fp)) {
                if (prog.cancel) break;
                // Dòng tiến trình 7z chứa " 12%" — lấy số % cuối dòng.
                int pct = -1;
                for (char* p = buf; *p; ++p) {
                    if (*p == '%' && p > buf) {
                        char* e = p - 1;
                        while (e > buf && *(e - 1) >= '0' && *(e - 1) <= '9') --e;
                        if (*e >= '0' && *e <= '9') pct = atoi(e);
                    }
                }
                if (pct >= 0 && pct <= 100 && pct >= lastPct) {
                    lastPct = pct;
                    prog.done = (uint64_t)pct;
                }
            }
            fclose(fp);
        }
        int status = 0;
        if (prog.cancel) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            prog.error = "Cancelled";
            return false;
        }
        waitpid(pid, &status, 0);
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        // 7z: 0 = ok, 1 = warning (vài file lỗi, còn lại xong).
        if (code != 0 && code != 1) {
            prog.error = "Bung thất bại";
            return false;
        }
        prog.done = 100;
        return true;
    }

private:
    static std::string lowerExt(const std::string& path) {
        size_t dot = path.find_last_of('.');
        size_t slash = path.find_last_of('/');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
            return "";
        std::string ext = path.substr(dot);
        for (char& c : ext) c = (char)tolower((unsigned char)c);
        return ext;
    }

    // Chạy `7zzs l -slt`: từ chối entry có "..", cộng tổng Size.
    static bool safeList(const std::string& toolBin, const std::string& archive,
                         uint64_t& outBytes, std::string& outErr) {
        outBytes = 0;
        std::string cmd = "'" + toolBin + "' l -slt -- '" + archive +
                          "' 2>/dev/null";
        FILE* fp = popen(cmd.c_str(), "r");
        if (!fp) { outErr = "Không đọc được file nén"; return false; }
        char line[1024];
        bool anyPath = false;
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "Path = ", 7) == 0) {
                anyPath = true;
                std::string p = line + 7;
                while (!p.empty() && (p.back() == '\n' || p.back() == '\r'))
                    p.pop_back();
                if (p.find("..") != std::string::npos) {
                    pclose(fp);
                    outErr = "File nén không an toàn!";
                    return false;
                }
            } else if (strncmp(line, "Size = ", 7) == 0) {
                outBytes += (uint64_t)atoll(line + 7);
            }
        }
        int rc = pclose(fp);
        if (!anyPath || rc != 0) {
            outErr = "File nén hỏng hoặc không đọc được";
            return false;
        }
        return true;
    }
};

} // namespace RomCloud
