#include "PlatformInfo.h"
#include "../filesystem/FileSystemManager.h"
#include "../config/AppConfig.h"
#include "../logging/Logger.h"
#include "../ota/UpdateManager.h"

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

PlatformInfo::PlatformInfo() {
    // Detect display resolution from SDL
    if (SDL_Init(SDL_INIT_VIDEO) == 0) {
        SDL_DisplayMode mode;
        if (SDL_GetDesktopDisplayMode(0, &mode) == 0) {
            m_displayWidth = mode.w;
            m_displayHeight = mode.h;
            Logger::info("Detected display: " + std::to_string(m_displayWidth) + "x" + std::to_string(m_displayHeight));
        }
    }

    // Calculate aspect ratio as float
    AspectRatio ratio = calculateAspectRatio(m_displayWidth, m_displayHeight);
    switch (ratio) {
        case AspectRatio::RATIO_4_3: m_aspectRatio = 4.0f / 3.0f; break;
        case AspectRatio::RATIO_3_2: m_aspectRatio = 3.0f / 2.0f; break;
        case AspectRatio::RATIO_16_9: m_aspectRatio = 16.0f / 9.0f; break;
        default: m_aspectRatio = 4.0f / 3.0f; break;
    }

    // Detect device type based on resolution
    m_deviceType = detectDeviceType();

    // Get device name
    std::string deviceName = getDeviceName();
    Logger::info("Detected device: " + deviceName);

    m_initialized = true;
}

PlatformInfo& PlatformInfo::instance() {
    static PlatformInfo instance;
    return instance;
}

DeviceType PlatformInfo::detectDeviceType() {
    // TrimUI Brick Pro: 1024x768
    if (m_displayWidth == 1024 && m_displayHeight == 768) {
        return DeviceType::TRIMUI_BRICK_PRO;
    }
    // TrimUI Smart Pro: 640x480
    if (m_displayWidth == 640 && m_displayHeight == 480) {
        return DeviceType::TRIMUI_SMART_PRO;
    }
    // TrimUI Beta: 480x320
    if (m_displayWidth == 480 && m_displayHeight == 320) {
        return DeviceType::TRIMUI_BETA;
    }
    // PocketGo: 320x240
    if (m_displayWidth == 320 && m_displayHeight == 240) {
        return DeviceType::POCKETGO;
    }

    // Try to detect from /proc/device-tree/model
    std::ifstream modelFile("/proc/device-tree/model");
    if (modelFile.is_open()) {
        std::string model;
        std::getline(modelFile, model);
        modelFile.close();

        if (model.find("Brick") != std::string::npos || model.find("brick") != std::string::npos) {
            return DeviceType::TRIMUI_BRICK_PRO;
        }
        if (model.find("Smart") != std::string::npos || model.find("smart") != std::string::npos) {
            return DeviceType::TRIMUI_SMART_PRO;
        }
    }

    return DeviceType::UNKNOWN;
}

AspectRatio PlatformInfo::calculateAspectRatio(int width, int height) {
    if (height == 0) return AspectRatio::RATIO_OTHER;

    float ratio = (float)width / (float)height;

    // Allow small tolerance for floating point
    if (ratio >= 1.3f && ratio <= 1.4f) return AspectRatio::RATIO_3_2;
    if (ratio >= 1.7f && ratio <= 1.8f) return AspectRatio::RATIO_16_9;
    if (ratio >= 1.25f && ratio <= 1.4f) return AspectRatio::RATIO_4_3;

    return AspectRatio::RATIO_OTHER;
}

DeviceType PlatformInfo::getDeviceType() {
    return m_deviceType;
}

AspectRatio PlatformInfo::getAspectRatio() {
    return calculateAspectRatio(m_displayWidth, m_displayHeight);
}

std::string PlatformInfo::getDeviceName() {
    switch (m_deviceType) {
        case DeviceType::TRIMUI_BRICK_PRO:  return "TrimUI Brick Pro";
        case DeviceType::TRIMUI_SMART_PRO:  return "TrimUI Smart Pro";
        case DeviceType::TRIMUI_BETA:       return "TrimUI Beta";
        case DeviceType::POCKETGO:          return "PocketGo";
        default:                            return "Unknown Device";
    }
}

void PlatformInfo::getDisplayMetrics(int& width, int& height, float& aspectRatio) {
    width = m_displayWidth;
    height = m_displayHeight;
    aspectRatio = m_aspectRatio;
}

int PlatformInfo::scaleX(int x) {
    // Scale X based on display width vs base width (1024)
    if (m_displayWidth == 0 || m_displayWidth == 1024) return x;
    return static_cast<int>(x * m_displayWidth / 1024.0f);
}

int PlatformInfo::scaleY(int y) {
    // Scale Y based on display height vs base height (768)
    if (m_displayHeight == 0 || m_displayHeight == 768) return y;
    return static_cast<int>(y * m_displayHeight / 768.0f);
}

int PlatformInfo::scaleW(int w) {
    // Scale width based on display width vs base width (1024)
    if (m_displayWidth == 0 || m_displayWidth == 1024) return w;
    return static_cast<int>(w * m_displayWidth / 1024.0f);
}

int PlatformInfo::scaleH(int h) {
    // Scale height based on display height vs base height (768)
    if (m_displayHeight == 0 || m_displayHeight == 768) return h;
    return static_cast<int>(h * m_displayHeight / 768.0f);
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
    diag.appVersion = std::string(APP_VERSION);
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

    // SOC name based on device type
    switch (m_deviceType) {
        case DeviceType::TRIMUI_BRICK_PRO:
            diag.socName = "Allwinner A133P (4x Cortex-A53)"; break;
        case DeviceType::TRIMUI_SMART_PRO:
            diag.socName = "Allwinner R528 (ARM9)"; break;
        case DeviceType::TRIMUI_BETA:
            diag.socName = "Allwinner F1C100s"; break;
        default:
            diag.socName = "Unknown SoC"; break;
    }

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

    // Dynamic display resolution
    std::string ratioStr;
    switch (getAspectRatio()) {
        case AspectRatio::RATIO_4_3: ratioStr = "4:3"; break;
        case AspectRatio::RATIO_3_2: ratioStr = "3:2"; break;
        case AspectRatio::RATIO_16_9: ratioStr = "16:9"; break;
        default: ratioStr = "Custom"; break;
    }
    diag.displayResolution = std::to_string(m_displayWidth) + " x " + std::to_string(m_displayHeight) + " (" + ratioStr + ")";

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
