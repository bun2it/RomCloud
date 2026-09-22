#pragma once
#include <string>

namespace RomCloud {

class AppConfig {
public:
    static AppConfig& instance();
    void setAppRoot(const std::string& root);
    const std::string& getAppRoot() const { return m_appRoot; }

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

    std::string getLogFilePath() const;
    std::string getDatabasePath() const;
    std::string getSchemaVersionPath() const;
    std::string getSettingsPath() const;
    std::string getFontPath() const;

    std::string getRomsDir() const;
    std::string getSystemRomsDir(const std::string& systemName) const;
    std::string getImgsDir() const;
    std::string getSystemImgsDir(const std::string& systemName) const;

private:
    AppConfig();
    std::string m_appRoot;
    std::string m_sdRoot;
};

} // namespace RomCloud
