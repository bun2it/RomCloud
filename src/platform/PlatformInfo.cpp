#include "PlatformInfo.h"
#include "../filesystem/FileSystemManager.h"
#include "../config/AppConfig.h"

#include <sys/utsname.h>
#include <fstream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <sqlite3.h>

namespace RomCloud {

PlatformInfo& PlatformInfo::instance() {
    static PlatformInfo instance;
    return instance;
}

std::string PlatformInfo::getIpAddress(const std::string& interfaceName) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return "Disconnected";

    struct ifreq ifr;
    ifr.ifr_addr.sa_family = AF_INET;
    std::strncpy(ifr.ifr_name, interfaceName.c_str(), IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    if (ioctl(fd, SIOCGIFADDR, &ifr) < 0) {
        close(fd);
        return "Disconnected";
    }

    close(fd);
    struct sockaddr_in* ipaddr = (struct sockaddr_in*)&ifr.ifr_addr;
    return std::string(inet_ntoa(ipaddr->sin_addr));
}

bool PlatformInfo::isNetworkConnected() {
    std::string ip = getIpAddress("wlan0");
    return (ip != "Disconnected" && ip != "127.0.0.1" && !ip.empty());
}

SystemDiagnostics PlatformInfo::getDiagnostics() {
    SystemDiagnostics diag;
    diag.appVersion = "1.0.0 (Phase 2 SQLite)";
    diag.buildDate = __DATE__ " " __TIME__;

    struct utsname uts;
    if (uname(&uts) == 0) {
        diag.osName = uts.sysname;
        diag.kernelRelease = uts.release;
        diag.cpuArch = uts.machine;
    } else {
        diag.osName = "Linux";
        diag.kernelRelease = "Unknown";
        diag.cpuArch = "aarch64";
    }
    diag.socName = "Allwinner A133P (4x Cortex-A53)";

    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        unsigned long totalKb = 0, availKb = 0;
        while (std::getline(meminfo, line)) {
            if (line.find("MemTotal:") == 0) {
                std::sscanf(line.c_str(), "MemTotal: %lu kB", &totalKb);
            } else if (line.find("MemAvailable:") == 0) {
                std::sscanf(line.c_str(), "MemAvailable: %lu kB", &availKb);
            }
        }
        meminfo.close();
        if (totalKb > 0) {
            diag.totalRam = FileSystemManager::instance().formatBytes(static_cast<uint64_t>(totalKb) * 1024);
            diag.freeRam = FileSystemManager::instance().formatBytes(static_cast<uint64_t>(availKb) * 1024);
        } else {
            diag.totalRam = "1.0 GB";
            diag.freeRam = "Available";
        }
    } else {
        diag.totalRam = "1.0 GB";
        diag.freeRam = "Available";
    }

    diag.displayResolution = "1024 x 768 (4:3 @ 60Hz)";

    SDL_version sdlVer;
    SDL_GetVersion(&sdlVer);
    diag.sdlVersion = std::to_string(sdlVer.major) + "." +
                      std::to_string(sdlVer.minor) + "." +
                      std::to_string(sdlVer.patch);

    diag.sqliteVersion = sqlite3_libversion();
    diag.curlVersion = "7.x (glibc)";

    diag.sdMountPoint = "/mnt/SDCARD";
    auto space = FileSystemManager::instance().getDiskSpace(AppConfig::instance().getAppRoot());
    diag.sdTotalSpace = FileSystemManager::instance().formatBytes(space.totalBytes);
    diag.sdFreeSpace = FileSystemManager::instance().formatBytes(space.availableBytes);

    int numJoysticks = SDL_NumJoysticks();
    if (numJoysticks > 0) {
        const char* name = SDL_JoystickNameForIndex(0);
        diag.controllerName = name ? name : "TRIMUI Player1";
    } else {
        diag.controllerName = "TRIMUI Player1 (evdev)";
    }

    std::string ip = getIpAddress("wlan0");
    if (ip != "Disconnected") {
        diag.networkStatus = "Connected (Wi-Fi)";
        diag.ipAddress = ip;
    } else {
        diag.networkStatus = "Disconnected (Offline)";
        diag.ipAddress = "N/A";
    }

    return diag;
}

} // namespace RomCloud
