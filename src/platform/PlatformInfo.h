#pragma once
#include <string>
#include <SDL2/SDL.h>

namespace RomCloud {

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

private:
    PlatformInfo() = default;
};

} // namespace RomCloud
