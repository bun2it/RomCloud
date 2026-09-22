#pragma once
#include <string>
#include <vector>
#include <ctime>

namespace RomCloud {

struct BackupMetadata {
    std::string version;
    std::string exportedAt;
    std::string appVersion;
};

struct BackupResult {
    bool success;
    std::string filePath;
    std::string errorMessage;
    int settingsCount = 0;
    int systemsCount = 0;
};

struct RestoreResult {
    bool success;
    std::string errorMessage;
    int settingsRestored = 0;
    int systemsRestored = 0;
};

class BackupManager {
public:
    static BackupManager& instance();

    // Export settings to JSON file on SD card
    BackupResult exportToSdCard();

    // Import settings from backup file
    RestoreResult importFromFile(const std::string& filePath);

    // Get the most recent backup file
    std::string getMostRecentBackup() const;

    // List all available backups
    std::vector<std::string> listAvailableBackups() const;

    // Validate a backup file
    bool validateBackupFile(const std::string& filePath) const;

    // Get backup directory path
    std::string getBackupDir() const;

private:
    BackupManager() = default;
    ~BackupManager() = default;
    BackupManager(const BackupManager&) = delete;
    BackupManager& operator=(const BackupManager&) = delete;

    std::string generateBackupFilename() const;
    std::string collectSettingsAsJson() const;
    std::string collectSystemsAsJson() const;
    std::string collectSyncStateAsJson() const;
    std::string buildFullBackupJson() const;
    bool writeJsonToFile(const std::string& path, const std::string& json);
    std::string readJsonFromFile(const std::string& path) const;
    bool applySettingsFromJson(const std::string& json);
    bool applySystemsFromJson(const std::string& json);
};

} // namespace RomCloud
