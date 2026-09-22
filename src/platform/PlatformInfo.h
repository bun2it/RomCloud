#pragma once
#include <string>
#include <SDL2/SDL.h>

namespace RomCloud {

enum class DeviceType {
    TRIMUI_BRICK_PRO,     // 1024x768 (4:3)
    TRIMUI_SMART_PRO,      // 640x480 (4:3)
    TRIMUI_BETA,           // 480x320 (3:2)
    POCKETGO,              // 320x240 (4:3)
    UNKNOWN
};

enum class AspectRatio {
    RATIO_4_3,    // 1024x768, 640x480, 320x240
    RATIO_3_2,    // 480x320
    RATIO_16_9,   // Widescreen
    RATIO_OTHER
};

struct SystemDiagnostics {
    std::string appVersion;
    std::string buildDate;
    std::string osName;
    std::string kernelRelease;
    std::string cpuArch;
    std::string socName;
    std::string totalRam;
    std::string freeRam;
    std::string displayResolution;
    std::string sdlVersion;
    std::string sqliteVersion;
    std::string curlVersion;
    std::string sdMountPoint;
    std::string sdTotalSpace;
    std::string sdFreeSpace;
    std::string controllerName;
    std::string networkStatus;
    std::string ipAddress;
};

class PlatformInfo {
public:
    static PlatformInfo& instance();
    SystemDiagnostics getDiagnostics();
    bool isNetworkConnected();
    std::string getIpAddress(const std::string& interfaceName = "wlan0");

    // Device and aspect ratio detection
    DeviceType getDeviceType();
    AspectRatio getAspectRatio();
    std::string getDeviceName();
    void getDisplayMetrics(int& width, int& height, float& aspectRatio);

    // Scaling helpers for different screen sizes
    int scaleX(int x);     // Scale X coordinate based on device
    int scaleY(int y);      // Scale Y coordinate based on device
    int scaleW(int w);     // Scale width based on device
    int scaleH(int h);     // Scale height based on device

private:
    PlatformInfo();
    DeviceType detectDeviceType();
    AspectRatio calculateAspectRatio(int width, int height);

    DeviceType m_deviceType = DeviceType::UNKNOWN;
    int m_displayWidth = 1024;
    int m_displayHeight = 768;
    float m_aspectRatio = 4.0f / 3.0f;
    bool m_initialized = false;
};

} // namespace RomCloud
