#include "CastManager.h"
#include "../platform/PlatformInfo.h"
#include "../logging/Logger.h"
#include "../network/HttpClient.h"
#include "../config/AppConfig.h"
#include "../database/DatabaseManager.h"
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <thread>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <cstring>
#include <vector>

namespace RomCloud {

static const char* PID_FILE = "/tmp/gamecast.pid";
static const char* FN_GPIO_PATH = "/sys/class/gpio/gpio243/value";

CastManager& CastManager::instance() {
    static CastManager s_instance;
    return s_instance;
}

bool CastManager::init() {
    // Ensure GPIO 243 is exported for FN switch
    if (access(FN_GPIO_PATH, R_OK) != 0) {
        std::system("echo 243 > /sys/class/gpio/export 2>/dev/null; echo in > /sys/class/gpio/gpio243/direction 2>/dev/null");
    }
    m_lastFnState = isFnSwitchOn() ? 1 : 0;

    // P3-A: restore HD mode preference tu DB (persistent across reboots).
    // Mac dinh la false (downscale 512x384) neu user chua tung set.
    std::string hdStr = DatabaseManager::instance().getSetting(
        "cast_hd_mode", "false");
    m_nativeRes = (hdStr == "true" || hdStr == "1");
    Logger::info("CastManager: init HD mode = " +
                 std::string(m_nativeRes ? "Native HD" : "Smooth (downscale)"));
    return true;
}

int CastManager::getRunningPid() {
    // 1. Try reading from PID file first
    FILE* fp = std::fopen(PID_FILE, "r");
    if (fp) {
        int pid = -1;
        if (std::fscanf(fp, "%d", &pid) == 1 && pid > 0) {
            // Verify if process really exists and is named gamecast_d
            char commPath[64];
            std::snprintf(commPath, sizeof(commPath), "/proc/%d/comm", pid);
            FILE* commFp = std::fopen(commPath, "r");
            if (commFp) {
                char comm[64] = {0};
                if (std::fgets(comm, sizeof(comm), commFp)) {
                    comm[strcspn(comm, "\r\n")] = 0;
                    if (std::strcmp(comm, "gamecast_d") == 0) {
                        std::fclose(commFp);
                        std::fclose(fp);
                        return pid;
                    }
                }
                std::fclose(commFp);
            }
        }
        std::fclose(fp);
    }

    // 2. Fallback: scan /proc for gamecast_d (cannot be fooled by deleted/corrupted PID file)
    DIR* dir = opendir("/proc");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type == DT_DIR || entry->d_type == DT_UNKNOWN) {
                int pid = std::atoi(entry->d_name);
                if (pid > 0 && pid != getpid()) {
                    char commPath[64];
                    std::snprintf(commPath, sizeof(commPath), "/proc/%d/comm", pid);
                    FILE* commFp = std::fopen(commPath, "r");
                    if (commFp) {
                        char comm[64] = {0};
                        if (std::fgets(comm, sizeof(comm), commFp)) {
                            comm[strcspn(comm, "\r\n")] = 0;
                            if (std::strcmp(comm, "gamecast_d") == 0) {
                                std::fclose(commFp);
                                closedir(dir);
                                // Repair PID file so other tools know the correct PID
                                FILE* nfp = std::fopen(PID_FILE, "w");
                                if (nfp) {
                                    std::fprintf(nfp, "%d\n", pid);
                                    std::fclose(nfp);
                                }
                                return pid;
                            }
                        }
                        std::fclose(commFp);
                    }
                }
            }
        }
        closedir(dir);
    }

    // No gamecast_d found running
    unlink(PID_FILE);
    return -1;
}

bool CastManager::isRunning() {
    return getRunningPid() > 0;
}

void CastManager::setHdMode(bool hd) {
    // P3-A: persist preference truoc khi goi POST (khoi reboot/reseed van giu).
    if (m_nativeRes != hd) {
        m_nativeRes = hd;
        DatabaseManager::instance().setSetting("cast_hd_mode",
                                               hd ? "true" : "false");
    }
    if (isRunning()) {
        std::string mode = hd ? "native" : "downscale";
        std::thread([mode]() {
            HttpClient::instance().post("http://127.0.0.1:8090/api/resolution?mode=" + mode, "", {}, 1);
        }).detach();
    }
}

bool CastManager::start(bool nativeRes) {
    m_nativeRes = nativeRes;
    if (isRunning()) return true;

    Logger::info("Starting GameCast daemon...");

    // Make sure any stale zombie instance is completely killed first
    stop();

    std::string resFlag = nativeRes ? "--native" : "--downscale";

    // Locate binary
    std::string binPath;
    const std::vector<std::string> searchPaths = {
        "/mnt/SDCARD/Apps/RomCloud/bin/gamecast_d",
        AppConfig::instance().getAppRoot() + "/bin/gamecast_d",
        "./bin/gamecast_d",
        "bin/gamecast_d"
    };

    for (const auto& p : searchPaths) {
        if (access(p.c_str(), X_OK) == 0) {
            binPath = p;
            break;
        } else if (access(p.c_str(), F_OK) == 0) {
            std::string chmodCmd = "chmod +x \"" + p + "\" 2>/dev/null";
            std::system(chmodCmd.c_str());
            binPath = p;
            break;
        }
    }

    if (binPath.empty()) {
        binPath = "/mnt/SDCARD/Apps/RomCloud/bin/gamecast_d";
    }

    // Launch daemon in background with nice -n 10 so emulator always has CPU priority
    std::string cmd = "(nice -n 10 " + binPath + " " + resFlag + " >/tmp/gamecast.log 2>&1 &)";
    std::system(cmd.c_str());

    // Wait and verify in /proc (up to 12 iterations * 100ms = 1.2s)
    for (int i = 0; i < 12; ++i) {
        usleep(100000);
        if (isRunning()) {
            // P3-C: danh dau thoi diem bat dau stream de tinh elapsed.
            m_streamStartedAt = SDL_GetTicks();
            // P3-B: user mong muốn stream chạy → checkAutoReconnect sẽ theo dõi.
            m_wantRunning = true;
            m_consecutiveRestartFailures = 0; // reset counter khi bat dau thanh cong.
            Logger::info("GameCast daemon started successfully (PID " + std::to_string(getRunningPid()) + ")");
            return true;
        }
    }

    Logger::warn("GameCast daemon failed to start. Check /tmp/gamecast.log");
    return false;
}

bool CastManager::stop() {
    Logger::info("Stopping GameCast daemon (absolute background kill)...");

    // 1. Gather all gamecast_d PIDs from /proc
    std::vector<int> pids;
    DIR* dir = opendir("/proc");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type == DT_DIR || entry->d_type == DT_UNKNOWN) {
                int pid = std::atoi(entry->d_name);
                if (pid > 0 && pid != getpid()) {
                    char commPath[64];
                    std::snprintf(commPath, sizeof(commPath), "/proc/%d/comm", pid);
                    FILE* commFp = std::fopen(commPath, "r");
                    if (commFp) {
                        char comm[64] = {0};
                        if (std::fgets(comm, sizeof(comm), commFp)) {
                            comm[strcspn(comm, "\r\n")] = 0;
                            if (std::strcmp(comm, "gamecast_d") == 0) {
                                pids.push_back(pid);
                            }
                        }
                        std::fclose(commFp);
                    }
                }
            }
        }
        closedir(dir);
    }

    // 2. Send SIGTERM first for graceful socket disconnect
    for (int p : pids) {
        kill(p, SIGTERM);
    }
    if (!pids.empty()) {
        usleep(50000);
    }

    // 3. Send SIGKILL (kill -9) to guarantee absolute termination
    for (int p : pids) {
        kill(p, SIGKILL);
    }

    // 4. Force killall -9
    std::system("killall -9 gamecast_d 2>/dev/null");

    // 5. Cleanup files
    unlink(PID_FILE);
    usleep(50000);

    // P3-C: reset elapsed timer.
    m_streamStartedAt = 0;
    // P3-B: user chủ động stop → không auto-reconnect nữa.
    m_wantRunning = false;
    m_consecutiveRestartFailures = 0;

    Logger::info("GameCast daemon stopped cleanly");
    return true;
}

bool CastManager::toggle() {
    if (isRunning()) {
        stop();
        return false;
    } else {
        return start(m_nativeRes);
    }
}

bool CastManager::isFnSwitchOn() {
    int fd = open(FN_GPIO_PATH, O_RDONLY);
    if (fd < 0) {
        // Try export once if not accessible
        std::system("echo 243 > /sys/class/gpio/export 2>/dev/null; echo in > /sys/class/gpio/gpio243/direction 2>/dev/null");
        fd = open(FN_GPIO_PATH, O_RDONLY);
        if (fd < 0) return false;
    }
    char c = '0';
    ssize_t n = read(fd, &c, 1);
    close(fd);
    return (n > 0 && c == '1');
}

void CastManager::update() {
    uint32_t now = SDL_GetTicks();

    // P3-B: health check mỗi 3s — nếu user đang mong muốn stream chạy mà
    // daemon không tồn tại (crash/kill ngoài) thì tự khởi động lại.
    if (m_wantRunning && now - m_lastReconnectCheckTicks >= 3000) {
        m_lastReconnectCheckTicks = now;
        checkAutoReconnect();
    }

    // Poll FN switch every 200ms
    if (now - m_lastFnPollTicks < 200) return;
    m_lastFnPollTicks = now;

    bool fnOn = isFnSwitchOn();
    int current = fnOn ? 1 : 0;

    if (m_lastFnState == -1) {
        m_lastFnState = current;
        return;
    }

    if (current != m_lastFnState) {
        m_lastFnState = current;
        bool fnActionSucceeded = true; // P3-D: neu skip do noWifi -> khong callback
        if (current == 1) {
            // P3-D: kiem tra Wi-Fi truoc khi start. Neu IP placeholder (Wi-Fi
            // mat), skip start + invoke canhister callback de UI canh.
            if (getIpAddress() == "192.168.x.x") {
                Logger::warn("FN ON but no Wi-Fi — skip start() to avoid "
                             "spawning useless gamecast_d");
                fnActionSucceeded = false;
                if (m_noWifiFnCb) m_noWifiFnCb();
            } else {
                // User switched FN to ON -> start GameCast
                Logger::info("FN switch turned ON -> Starting GameCast");
                start(m_nativeRes);
            }
        } else {
            // User switched FN to OFF -> ABSOLUTELY STOP GameCast
            Logger::info("FN switch turned OFF -> Absolute STOP GameCast");
            stop();
        }
        // P1-2: thong bao UI qua callback (running state + flag FN-driven).
        // P3-D: chi callback thong thuong neu FN action that su start/stop
        // (khong bao gom canhister "noWifi").
        if (m_fnToggleCb && fnActionSucceeded) {
            m_fnToggleCb(isRunning(), true);
        }
    }
}

std::string CastManager::getIpAddress() {
    std::string ip = PlatformInfo::instance().getIpAddress();
    if (ip.empty() || ip == "127.0.0.1") {
        ip = "192.168.x.x";
    }
    return ip;
}

std::string CastManager::getCastUrl() {
    return "http://" + getIpAddress() + ":8090/cast";
}

uint32_t CastManager::getStreamElapsedMs() const {
    if (m_streamStartedAt == 0) return 0;
    uint32_t now = SDL_GetTicks();
    // Xu ly SDL tick 32-bit wrap-around (~49 ngay tu SDL_Init()).
    if (now >= m_streamStartedAt) {
        return now - m_streamStartedAt;
    }
    return (UINT32_MAX - m_streamStartedAt) + now + 1;
}

void CastManager::checkAutoReconnect() {
    if (!m_wantRunning) return;
    if (isRunning()) {
        // Daemon OK — reset failure counter nếu có.
        m_consecutiveRestartFailures = 0;
        return;
    }

    // Daemon chết dù user vẫn muốn stream chạy → thử restart.
    // Backoff: failure đầu tiên restart ngay, các lần sau chờ 5s/lần.
    if (m_consecutiveRestartFailures >= 5) {
        // Quá nhiều lần fail liên tiếp — dừng retry, báo UI.
        Logger::error("Auto-reconnect: gave up after " +
                      std::to_string(m_consecutiveRestartFailures) +
                      " consecutive failures");
        m_wantRunning = false; // dừng retry, user phải bấm A lại
        if (m_autoReconnectCb) m_autoReconnectCb(false);
        return;
    }

    Logger::warn("Auto-reconnect: gamecast_d died, attempting restart (attempt " +
                 std::to_string(m_consecutiveRestartFailures + 1) + "/5)");
    bool ok = start(m_nativeRes);
    if (ok) {
        Logger::info("Auto-reconnect: restart successful");
        m_consecutiveRestartFailures = 0;
        if (m_autoReconnectCb) m_autoReconnectCb(true);
    } else {
        m_consecutiveRestartFailures++;
        Logger::warn("Auto-reconnect: restart failed (consecutive " +
                     std::to_string(m_consecutiveRestartFailures) + "/5)");
    }
}

} // namespace RomCloud
