#include "AppConfig.h"
#include <unistd.h>
#include <sys/stat.h>

namespace RomCloud {

AppConfig& AppConfig::instance() {
    static AppConfig instance;
    return instance;
}

AppConfig::AppConfig() {
    m_sdRoot = "/mnt/SDCARD";
    m_appRoot = "/mnt/SDCARD/Apps/RomCloud";

    struct stat st;
    if (stat("/mnt/SDCARD", &st) != 0) {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) != nullptr) {
            m_sdRoot = std::string(cwd);
            m_appRoot = m_sdRoot + "/Apps/RomCloud";
        }
    }
}

void AppConfig::setAppRoot(const std::string& root) {
    m_appRoot = root;
}

std::string AppConfig::getBinDir() const       { return m_appRoot + "/bin"; }
std::string AppConfig::getConfigDir() const     { return m_appRoot + "/config"; }
std::string AppConfig::getDataDir() const       { return m_appRoot + "/data"; }
std::string AppConfig::getCacheDir() const      { return m_appRoot + "/cache"; }
std::string AppConfig::getCoversDir() const     { return m_appRoot + "/cache/covers"; }
std::string AppConfig::getMetadataDir() const   { return m_appRoot + "/cache/metadata"; }
std::string AppConfig::getLogsDir() const       { return m_appRoot + "/logs"; }
std::string AppConfig::getTempDir() const       { return m_appRoot + "/temp"; }
std::string AppConfig::getAssetsDir() const     { return m_appRoot + "/assets"; }
std::string AppConfig::getFontsDir() const      { return m_appRoot + "/assets/fonts"; }

std::string AppConfig::getLogFilePath() const       { return getLogsDir() + "/romcloud.log"; }
std::string AppConfig::getDatabasePath() const      { return getDataDir() + "/library.db"; }
std::string AppConfig::getSchemaVersionPath() const { return getDataDir() + "/schema_version"; }
std::string AppConfig::getSettingsPath() const      { return getConfigDir() + "/settings.json"; }
std::string AppConfig::getFontPath() const          { return getFontsDir() + "/font.ttf"; }

std::string AppConfig::getRomsDir() const           { return m_sdRoot + "/Roms"; }
std::string AppConfig::getSystemRomsDir(const std::string& systemName) const {
    return getRomsDir() + "/" + systemName;
}
std::string AppConfig::getImgsDir() const           { return m_sdRoot + "/Imgs"; }
std::string AppConfig::getSystemImgsDir(const std::string& systemName) const {
    return getImgsDir() + "/" + systemName;
}

} // namespace RomCloud
