#pragma once
#include <string>

namespace RomCloud {

// OS Types for TrimUI variants
enum class OSType {
    AUTO,           // Auto detect
    STOCK_PS,       // Stock TrimUI OS / PS Classic Stock
    NEXTUI,         // NextUI custom firmware
    SPRUCE_OS       // SpruceOS / SpruceUI
};

class AppConfig {
public:
    static AppConfig& instance();

    // App root
    void setAppRoot(const std::string& root);
    const std::string& getAppRoot() const { return m_appRoot; }
    const std::string& getSdRoot() const { return m_sdRoot; }

    // OS Type configuration
    void setOSType(OSType type);
    OSType getOSType() const { return m_osType; }
    std::string getOSName() const;
    OSType detectOSType() const;
    void loadSettings();
    void saveSettings() const;

    // Directories based on OS type
    std::string getBinDir() const;
    std::string getConfigDir() const;
    std::string getDataDir() const;
    std::string getCacheDir() const;
    std::string getCoversDir() const;
    std::string getMetadataDir() const;
    std::string getLogsDir() const;
    std::string getTempDir() const;
    std::string getAssetsDir() const;
    std::string getFontsDir() const;
    std::string getIptvDir() const;

    // Paths
    std::string getLogFilePath() const;
    std::string getDatabasePath() const;
    std::string getSchemaVersionPath() const;
    std::string getSettingsPath() const;
    std::string getFontPath() const;

    // ROM directories
    std::string getRomsDir() const;
    std::string getSystemRomsDir(const std::string& systemName) const;
    std::string getImgsDir() const;
    std::string getSystemImgsDir(const std::string& systemName) const;

    // mpv/ffmpeg path for IPTV
    std::string getMediaPlayerPath() const;

private:
    AppConfig();
    std::string getBaseDir() const; // Different base per OS

    OSType m_osType = OSType::AUTO;
    std::string m_appRoot;
    std::string m_sdRoot;
};

// Helper to convert OSType to string
inline const char* OSTypeToString(OSType t) {
    switch (t) {
        case OSType::STOCK_PS:   return "Stock (PS)";
        case OSType::NEXTUI:     return "NextUI";
        case OSType::SPRUCE_OS:  return "SpruceOS";
        default:                 return "Auto";
    }
}

} // namespace RomCloud
