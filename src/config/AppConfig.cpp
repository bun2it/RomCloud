#include "AppConfig.h"
#include "../logging/Logger.h"
#include <unistd.h>
#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include <cstring>

namespace RomCloud {

// ============================================================================
// OS-SPECIFIC PATH CONFIGURATIONS
// ============================================================================
// Based on official documentation from:
// - Stock PS: /mnt/SDCARD/Roms, /mnt/SDCARD/Imgs
// - NextUI: https://nextui.loveretro.games/docs/
// - SpruceOS: https://github.com/spruceUI/spruceOS/

// ============================================================================
// APP CONFIG
// ============================================================================

AppConfig& AppConfig::instance() {
    static AppConfig instance;
    return instance;
}

AppConfig::AppConfig() {
    struct stat st;

    // Default paths (Stock PS)
    m_sdRoot = "/mnt/SDCARD";
    m_appRoot = "/mnt/SDCARD/Apps/RomCloud";
    m_osType = OSType::AUTO;

    // Check if SD card exists, fallback to dev environment if not
    if (stat("/mnt/SDCARD", &st) != 0) {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) != nullptr) {
            m_sdRoot = std::string(cwd);
            m_appRoot = m_sdRoot;
        }
    }

    // Load saved OS type from settings
    loadSettings();
    if (m_osType == OSType::AUTO) {
        m_osType = detectOSType();
    }
}

void AppConfig::setAppRoot(const std::string& root) {
    m_appRoot = root;
}

void AppConfig::setOSType(OSType type) {
    m_osType = type;
    saveSettings();
    Logger::info("OS Type set to: " + std::string(OSTypeToString(type)));
}

OSType AppConfig::detectOSType() const {
    struct stat st;

    // Check for SpruceOS markers (spruce, SPRUCE, .spruceos, or Emu singular without Emus)
    if (stat((m_sdRoot + "/spruce").c_str(), &st) == 0 ||
        stat((m_sdRoot + "/SPRUCE").c_str(), &st) == 0 ||
        stat((m_sdRoot + "/.spruceos").c_str(), &st) == 0 ||
        stat((m_sdRoot + "/Themes/SPRUCE").c_str(), &st) == 0 ||
        (stat((m_sdRoot + "/Emu").c_str(), &st) == 0 && stat((m_sdRoot + "/Emus").c_str(), &st) != 0)) {
        return OSType::SPRUCE_OS;
    }

    // Check for NextUI markers
    if (stat((m_sdRoot + "/.nextui").c_str(), &st) == 0 ||
        stat((m_sdRoot + "/NextUI").c_str(), &st) == 0 ||
        stat((m_sdRoot + "/nextui").c_str(), &st) == 0 ||
        stat((m_sdRoot + "/.nextui_update").c_str(), &st) == 0) {
        return OSType::NEXTUI;
    }

    // Check for Stock PS / standard TrimUI
    return OSType::STOCK_PS;
}

std::string AppConfig::getOSName() const {
    std::string name = OSTypeToString(m_osType);
    return name;
}

std::string AppConfig::getBinDir() const {
    return getAppRoot() + "/bin";
}

std::string AppConfig::getConfigDir() const {
    return getAppRoot() + "/config";
}

std::string AppConfig::getDataDir() const {
    return getAppRoot() + "/data";
}

std::string AppConfig::getCacheDir() const {
    return getAppRoot() + "/cache";
}

std::string AppConfig::getCoversDir() const {
    return getCacheDir() + "/covers";
}

std::string AppConfig::getMetadataDir() const {
    return getCacheDir() + "/metadata";
}

std::string AppConfig::getLogsDir() const {
    return getDataDir() + "/logs";
}

std::string AppConfig::getTempDir() const {
    return getDataDir() + "/temp";
}

std::string AppConfig::getAssetsDir() const {
    return getAppRoot() + "/assets";
}

std::string AppConfig::getFontsDir() const {
    return getAssetsDir() + "/fonts";
}

std::string AppConfig::getIptvDir() const {
    return getAppRoot() + "/iptv";
}

// Paths
std::string AppConfig::getLogFilePath() const {
    return getLogsDir() + "/romcloud.log";
}

std::string AppConfig::getDatabasePath() const {
    return getDataDir() + "/library.db";
}

std::string AppConfig::getSchemaVersionPath() const {
    return getDataDir() + "/schema_version";
}

std::string AppConfig::getSettingsPath() const {
    return getConfigDir() + "/settings.json";
}

std::string AppConfig::getFontPath() const {
    // Try multiple font locations
    struct stat st;
    static std::string fontPath;

    if (!fontPath.empty()) {
        return fontPath;
    }

    const std::string fonts[] = {
        getFontsDir() + "/font.ttf",
        "/mnt/SDCARD/Apps/RomCloud/assets/fonts/font.ttf",
        "/usr/trimui/res/full.ttf",
        "/usr/trimui/res/regular.ttf",
        "/mnt/SDCARD/Themes/TRIMUI YaHei/msyh.ttf",
        "/system/media/fonts/TrimUI.ttf"
    };

    for (const auto& f : fonts) {
        if (stat(f.c_str(), &st) == 0) {
            fontPath = f;
            return fontPath;
        }
    }

    return ""; // No font found
}

// ROM directories
std::string AppConfig::getRomsDir() const {
    return m_sdRoot + "/Roms";
}

std::string AppConfig::getSystemRomsDir(const std::string& systemName) const {
    return getRomsDir() + "/" + systemName;
}

std::string AppConfig::getImgsDir() const {
    OSType os = (m_osType == OSType::AUTO) ? detectOSType() : m_osType;
    if (os == OSType::SPRUCE_OS) {
        return m_sdRoot + "/Roms";
    }
    return m_sdRoot + "/Imgs";
}

std::string AppConfig::getSystemImgsDir(const std::string& systemName) const {
    return getImgsDir() + "/" + systemName;
}

// Media player path for IPTV (Hardware-accelerated mpv CedarX)
std::string AppConfig::getMediaPlayerPath() const {
    struct stat st;
    std::string appMpv = getBinDir() + "/mpv";
    const std::string players[] = {
        appMpv,
        m_sdRoot + "/System/bin/mpv",
        m_sdRoot + "/Emus/VIDEOS/mpv.sh",
        m_sdRoot + "/Emu/VIDEOS/mpv.sh",
        "/usr/trimui/bin/mpv",
        "/usr/bin/mpv",
        getBinDir() + "/ffplay",
        "/usr/bin/ffplay"
    };

    for (const auto& p : players) {
        if (stat(p.c_str(), &st) == 0 && access(p.c_str(), X_OK) == 0) {
            return p;
        }
    }
    return appMpv;
}

// ============================================================================
// SETTINGS PERSISTENCE
// ============================================================================

void AppConfig::loadSettings() {
    std::string path = getConfigDir() + "/os_config.json";
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    // Parse simple JSON
    size_t osTypePos = content.find("\"os_type\"");
    if (osTypePos != std::string::npos) {
        size_t colonPos = content.find(":", osTypePos);
        if (colonPos != std::string::npos) {
            size_t valueStart = content.find("\"", colonPos);
            size_t valueEnd = content.find("\"", valueStart + 1);
            if (valueStart != std::string::npos && valueEnd != std::string::npos) {
                std::string value = content.substr(valueStart + 1, valueEnd - valueStart - 1);

                if (value == "STOCK_PS") m_osType = OSType::STOCK_PS;
                else if (value == "NEXTUI") m_osType = OSType::NEXTUI;
                else if (value == "SPRUCE_OS") m_osType = OSType::SPRUCE_OS;
                else m_osType = OSType::AUTO;
            }
        }
    }
}

void AppConfig::saveSettings() const {
    std::string path = getConfigDir() + "/os_config.json";
    std::ofstream file(path);
    if (!file.is_open()) {
        Logger::error("Cannot save OS config: " + path);
        return;
    }

    std::string osTypeStr;
    switch (m_osType) {
        case OSType::STOCK_PS:   osTypeStr = "STOCK_PS"; break;
        case OSType::NEXTUI:    osTypeStr = "NEXTUI"; break;
        case OSType::SPRUCE_OS: osTypeStr = "SPRUCE_OS"; break;
        default:                osTypeStr = "AUTO"; break;
    }

    file << "{\n";
    file << "  \"os_type\": \"" << osTypeStr << "\"\n";
    file << "}\n";
    file.close();
}

} // namespace RomCloud
