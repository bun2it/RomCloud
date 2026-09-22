#include "BackupManager.h"
#include "../database/DatabaseManager.h"
#include "../config/AppConfig.h"
#include "../logging/Logger.h"
#include "../ota/UpdateManager.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <sys/stat.h>

namespace RomCloud {

static std::string escapeJsonString(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:   result += c; break;
        }
    }
    return result;
}

BackupManager& BackupManager::instance() {
    static BackupManager instance;
    return instance;
}

std::string BackupManager::generateBackupFilename() const {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&time);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return "romcloud_backup_" + oss.str() + ".json";
}

std::string BackupManager::getBackupDir() const {
    return AppConfig::instance().getAppRoot() + "/backups";
}

std::string BackupManager::collectSettingsAsJson() const {
    std::ostringstream oss;
    oss << "{\n";

    // Query all settings from database
    // Keys we want to backup
    std::vector<std::string> keys = {
        "google_api_key",
        "auth_client_id",
        "auth_client_secret",
        "auth_is_linked",
        "auth_access_token",
        "auth_refresh_token",
        "auth_expires_at",
        "auth_user_email",
        "drive_folder_id",
        "drive_folder_url",
        "last_cloud_sync_time"
    };

    bool first = true;
    for (const auto& key : keys) {
        std::string value = DatabaseManager::instance().getSetting(key, "__NULL__");
        if (value != "__NULL__") {
            if (!first) oss << ",\n";
            oss << "    \"" << escapeJsonString(key) << "\": \"" << escapeJsonString(value) << "\"";
            first = false;
        }
    }

    oss << "\n  }";
    return oss.str();
}

std::string BackupManager::collectSystemsAsJson() const {
    std::ostringstream oss;
    oss << "[\n";

    auto systems = DatabaseManager::instance().getSystems(false); // Don't include counts

    bool first = true;
    for (const auto& sys : systems) {
        if (!first) oss << ",\n";
        oss << "    {\n";
        oss << "      \"id\": " << sys.id << ",\n";
        oss << "      \"code\": \"" << escapeJsonString(sys.code) << "\",\n";
        oss << "      \"name\": \"" << escapeJsonString(sys.name) << "\",\n";
        oss << "      \"rom_dir\": \"" << escapeJsonString(sys.romDir) << "\",\n";
        oss << "      \"img_dir\": \"" << escapeJsonString(sys.imgDir) << "\",\n";
        oss << "      \"ext_list\": \"" << escapeJsonString(sys.extList) << "\",\n";
        oss << "      \"icon_path\": \"" << escapeJsonString(sys.iconPath) << "\",\n";
        oss << "      \"sort_order\": " << sys.sortOrder << "\n";
        oss << "    }";
        first = false;
    }

    oss << "\n  ]";
    return oss.str();
}

std::string BackupManager::collectSyncStateAsJson() const {
    std::ostringstream oss;
    oss << "[\n";

    // Query sync_state table directly
    // This is a simplified version - just return empty for now
    // Full implementation would query sync_state table

    oss << "  ]";
    return oss.str();
}

std::string BackupManager::buildFullBackupJson() const {
    std::ostringstream oss;
    oss << "{\n";

    // Metadata
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&time);
    oss << "  \"version\": \"" << APP_VERSION << "\",\n";
    oss << "  \"app_version\": \"" << APP_VERSION << "\",\n";
    oss << "  \"exported_at\": \"" << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ") << "\",\n";

    // Settings
    oss << "  \"settings\": " << collectSettingsAsJson() << ",\n";

    // Systems
    oss << "  \"systems\": " << collectSystemsAsJson() << ",\n";

    // Sync state
    oss << "  \"sync_state\": " << collectSyncStateAsJson() << "\n";

    oss << "}";
    return oss.str();
}

bool BackupManager::writeJsonToFile(const std::string& path, const std::string& json) {
    std::ofstream file(path);
    if (!file.is_open()) {
        Logger::error("BackupManager: Cannot open file for writing: " + path);
        return false;
    }
    file << json;
    file.close();
    return true;
}

std::string BackupManager::readJsonFromFile(const std::string& path) const {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool BackupManager::applySettingsFromJson(const std::string& json) {
    if (json.empty()) return false;

    // Parse settings object
    size_t settingsStart = json.find("\"settings\"");
    if (settingsStart == std::string::npos) return false;

    size_t braceStart = json.find("{", settingsStart);
    size_t braceEnd = json.find("}", settingsStart);
    if (braceStart == std::string::npos || braceEnd == std::string::npos) return false;

    std::string settingsJson = json.substr(braceStart + 1, braceEnd - braceStart - 1);

    // Parse key-value pairs
    size_t pos = 0;
    while (pos < settingsJson.length()) {
        // Find key
        size_t keyStart = settingsJson.find("\"", pos);
        if (keyStart == std::string::npos) break;
        size_t keyEnd = settingsJson.find("\"", keyStart + 1);
        if (keyEnd == std::string::npos) break;

        std::string key = settingsJson.substr(keyStart + 1, keyEnd - keyStart - 1);

        // Find value
        size_t colonPos = settingsJson.find(":", keyEnd);
        if (colonPos == std::string::npos) break;

        size_t valueStart = settingsJson.find("\"", colonPos);
        if (valueStart == std::string::npos) break;
        size_t valueEnd = settingsJson.find("\"", valueStart + 1);
        if (valueEnd == std::string::npos) break;

        std::string value = settingsJson.substr(valueStart + 1, valueEnd - valueStart - 1);

        // Unescape value
        std::string unescaped;
        for (size_t i = 0; i < value.length(); ++i) {
            if (value[i] == '\\' && i + 1 < value.length()) {
                switch (value[i + 1]) {
                    case 'n': unescaped += '\n'; i++; break;
                    case 'r': unescaped += '\r'; i++; break;
                    case 't': unescaped += '\t'; i++; break;
                    case '"': unescaped += '"'; i++; break;
                    case '\\': unescaped += '\\'; i++; break;
                    default: unescaped += value[i]; break;
                }
            } else {
                unescaped += value[i];
            }
        }

        DatabaseManager::instance().setSetting(key, unescaped);
        Logger::info("BackupManager: Restored setting: " + key);

        pos = valueEnd + 1;
    }

    return true;
}

bool BackupManager::applySystemsFromJson(const std::string& json) {
    // Simplified - systems are seeded from code, skip restore
    Logger::info("BackupManager: Systems restoration skipped (seeded from code)");
    return true;
}

BackupResult BackupManager::exportToSdCard() {
    BackupResult result;
    result.success = false;

    // Create backup directory if needed
    std::string backupDir = getBackupDir();
    struct stat st;
    if (stat(backupDir.c_str(), &st) != 0) {
        // Create directory
        std::string cmd = "mkdir -p \"" + backupDir + "\"";
        system(cmd.c_str());
    }

    // Generate filename
    std::string filename = generateBackupFilename();
    std::string fullPath = backupDir + "/" + filename;

    // Build JSON
    std::string json = buildFullBackupJson();

    // Write file
    if (!writeJsonToFile(fullPath, json)) {
        result.errorMessage = "Cannot write backup file";
        Logger::error("BackupManager: " + result.errorMessage);
        return result;
    }

    // Count settings
    std::vector<std::string> keys = {
        "google_api_key", "auth_client_id", "auth_is_linked",
        "auth_access_token", "auth_refresh_token", "auth_user_email",
        "drive_folder_id", "last_cloud_sync_time"
    };
    for (const auto& key : keys) {
        if (DatabaseManager::instance().getSetting(key, "").empty() == false) {
            result.settingsCount++;
        }
    }

    auto systems = DatabaseManager::instance().getSystems(false);
    result.systemsCount = systems.size();

    result.success = true;
    result.filePath = fullPath;

    Logger::info("BackupManager: Backup created successfully: " + fullPath);
    return result;
}

RestoreResult BackupManager::importFromFile(const std::string& filePath) {
    RestoreResult result;
    result.success = false;

    // Read file
    std::string json = readJsonFromFile(filePath);
    if (json.empty()) {
        result.errorMessage = "Cannot read backup file: " + filePath;
        Logger::error("BackupManager: " + result.errorMessage);
        return result;
    }

    // Validate it's a backup file
    if (json.find("\"version\"") == std::string::npos ||
        json.find("\"settings\"") == std::string::npos) {
        result.errorMessage = "Invalid backup file format";
        Logger::error("BackupManager: " + result.errorMessage);
        return result;
    }

    // Apply settings
    if (!applySettingsFromJson(json)) {
        result.errorMessage = "Failed to restore settings";
        Logger::error("BackupManager: " + result.errorMessage);
        return result;
    }

    // Apply systems (if needed)
    applySystemsFromJson(json);

    result.success = true;
    result.settingsRestored = result.settingsRestored; // Count in applySettingsFromJson
    result.systemsRestored = result.systemsRestored;

    Logger::info("BackupManager: Backup restored successfully from: " + filePath);
    return result;
}

std::string BackupManager::getMostRecentBackup() const {
    auto backups = listAvailableBackups();
    if (backups.empty()) return "";
    return backups.back(); // Most recent is last (sorted)
}

std::vector<std::string> BackupManager::listAvailableBackups() const {
    std::vector<std::string> backups;
    std::string backupDir = getBackupDir();

    // Use popen to list files
    std::string cmd = "ls -1 \"" + backupDir + "\"/*.json 2>/dev/null | sort";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return backups;

    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string path(buffer);
        // Remove trailing newline
        path.erase(path.find_last_not_of("\n\r") + 1);
        backups.push_back(path);
    }
    pclose(pipe);

    return backups;
}

bool BackupManager::validateBackupFile(const std::string& filePath) const {
    std::string json = readJsonFromFile(filePath);
    if (json.empty()) return false;

    // Check required fields
    bool hasVersion = json.find("\"version\"") != std::string::npos;
    bool hasSettings = json.find("\"settings\"") != std::string::npos;
    bool hasExportedAt = json.find("\"exported_at\"") != std::string::npos;

    return hasVersion && hasSettings && hasExportedAt;
}

} // namespace RomCloud
