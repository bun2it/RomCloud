#define _GNU_SOURCE
#include "EmuLauncher.h"
#include "../config/AppConfig.h"
#include "../logging/Logger.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <unistd.h>
#include <vector>
#include <sys/wait.h>

extern char** environ;

namespace RomCloud {

EmuLauncher& EmuLauncher::instance() {
    static EmuLauncher inst;
    return inst;
}

EmuLauncher::~EmuLauncher() {
    stop();
}

bool EmuLauncher::findLauncher(const std::string& systemCode,
                               std::string& outScript) {
    std::string sdRoot = AppConfig::instance().getSdRoot();
    if (sdRoot.empty()) sdRoot = "/mnt/SDCARD";
    // Mã hệ trong DB trùng tên thư mục Emus (GBA->Emus/GBA, FC->Emus/FC).
    const std::string cands[] = {
        sdRoot + "/Emus/" + systemCode + "/launch.sh",
    };
    for (const auto& p : cands) {
        if (access(p.c_str(), X_OK) == 0) {
            outScript = p;
            return true;
        }
    }
    return false;
}

static std::vector<std::string> sanitizedEnv() {
    // Bỏ biến nạp thư viện của RomCloud để RetroArch dùng lib hệ thống.
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
        std::string s = *e;
        if (s.rfind("LD_LIBRARY_PATH=", 0) == 0) continue;
        if (s.rfind("LD_PRELOAD=", 0) == 0) continue;
        env.push_back(s);
    }
    return env;
}

bool EmuLauncher::play(const std::string& systemCode,
                       const std::string& romPath,
                       const std::string& title) {
    if (isRunning()) stop();
    pollExited();
    if (romPath.empty() || access(romPath.c_str(), R_OK) != 0) {
        Logger::warn("EmuLauncher: không đọc được rom: " + romPath);
        return false;
    }
    std::string script;
    if (!findLauncher(systemCode, script)) {
        Logger::warn("EmuLauncher: không có launch.sh cho hệ " + systemCode);
        return false;
    }
    m_title = title;
    Logger::info("EmuLauncher: chạy [" + systemCode + "] " + romPath);

    std::vector<std::string> envStore = sanitizedEnv();
    std::vector<char*> envp;
    envp.reserve(envStore.size() + 1);
    for (auto& s : envStore) envp.push_back(s.data());
    envp.push_back(nullptr);

    // Session riêng: tín hiệu gửi tới RomCloud không lan sang emulator.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
#endif
    char* argv[] = {(char*)"/bin/sh", (char*)script.c_str(),
                     (char*)romPath.c_str(), nullptr};
    pid_t pid = -1;
    int rc = posix_spawn(&pid, "/bin/sh", nullptr, &attr, argv, envp.data());
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        Logger::warn("EmuLauncher: fork thất bại: " + std::string(strerror(rc)));
        return false;
    }
    m_pid = pid;
    return true;
}

bool EmuLauncher::isRunning() {
    if (m_pid <= 0) return false;
    return kill(m_pid, 0) == 0 || errno == EPERM;
}

bool EmuLauncher::pollExited() {
    if (m_pid <= 0) return true;
    int status = 0;
    pid_t r = waitpid(m_pid, &status, WNOHANG);
    if (r == m_pid) {
        if (WIFEXITED(status)) {
            Logger::info("EmuLauncher: emulator thoát, exit code " +
                         std::to_string(WEXITSTATUS(status)));
        } else if (WIFSIGNALED(status)) {
            Logger::warn("EmuLauncher: emulator bị tín hiệu " +
                         std::to_string(WTERMSIG(status)));
        }
        m_pid = -1;
        return true;
    }
    if (r < 0 && errno == ECHILD) {
        m_pid = -1;
        return true;
    }
    return false;
}

bool EmuLauncher::stop() {
    if (m_pid <= 0) return true;
    Logger::info("EmuLauncher: dừng emulator pid " + std::to_string(m_pid));
    kill(m_pid, SIGTERM);
    for (int i = 0; i < 100; ++i) {
        if (pollExited()) return true;
        usleep(20 * 1000);
    }
    kill(-m_pid, SIGKILL);
    kill(m_pid, SIGKILL);
    int status = 0;
    waitpid(m_pid, &status, 0);
    m_pid = -1;
    return true;
}

} // namespace RomCloud
