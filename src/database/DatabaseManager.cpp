#include "DatabaseManager.h"
#include "Schema.h"
#include "../logging/Logger.h"
#include "../config/AppConfig.h"
#include "../filesystem/FileSystemManager.h"
#include "../network/JsonHelper.h"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <iostream>
#include <cstring>

namespace RomCloud {

DatabaseManager& DatabaseManager::instance() {
    static DatabaseManager instance;
    return instance;
}

DatabaseManager::~DatabaseManager() {
    close();
}

std::string DatabaseManager::getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

bool DatabaseManager::executeSimpleQuery(const char* sql) {
    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        Logger::error(std::string("SQLite exec error: ") + (errMsg ? errMsg : "Unknown"));
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool DatabaseManager::init(const std::string& dbPath) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_db) close();

    m_dbPath = dbPath;
    int rc = sqlite3_open(dbPath.c_str(), &m_db);
    if (rc != SQLITE_OK) {
        Logger::error(std::string("Cannot open SQLite database: ") + (m_db ? sqlite3_errmsg(m_db) : "Unknown"));
        m_db = nullptr;
        return false;
    }

    executeSimpleQuery("PRAGMA journal_mode = WAL;");
    executeSimpleQuery("PRAGMA synchronous = NORMAL;");
    executeSimpleQuery("PRAGMA foreign_keys = ON;");

    Logger::info("Opened SQLite library database at: " + dbPath);

    // Check schema version BEFORE deciding to wipe DB.
    // ONLY wipe if schema is completely missing (version 0) and migration fails.
    // NEVER wipe an existing schema v1+ database - that would destroy cloud game index!
    int existingVersion = getSchemaVersion();
    if (existingVersion >= CURRENT_SCHEMA_VERSION) {
        Logger::info("Database schema is current (v" + std::to_string(existingVersion) + "). Skipping migration.");
        // Ensure all tables exist (safe idempotent create-if-not-exists)
        executeSimpleQuery(SCHEMA_V1_SQL);
    } else {
        if (!migrateSchema()) {
            if (existingVersion == 0) {
                // Fresh empty DB - safe to wipe and recreate
                Logger::warn("Schema migration failed on fresh DB. Performing clean recovery...");
                close();
                remove(dbPath.c_str());
                remove((dbPath + "-wal").c_str());
                remove((dbPath + "-shm").c_str());

                rc = sqlite3_open(dbPath.c_str(), &m_db);
                if (rc != SQLITE_OK) {
                    Logger::error("Failed to recreate SQLite database after reset.");
                    return false;
                }

                executeSimpleQuery("PRAGMA journal_mode = WAL;");
                executeSimpleQuery("PRAGMA synchronous = NORMAL;");
                executeSimpleQuery("PRAGMA foreign_keys = ON;");

                if (!migrateSchema()) {
                    Logger::error("Schema migration failed on fresh database.");
                    return false;
                }
            } else {
                // Existing DB with unexpected schema - do NOT wipe, just warn
                Logger::warn("Schema migration issue on existing DB (v" + std::to_string(existingVersion) + "). Continuing with existing data.");
            }
        }
    }

    seedDefaultSystems();

    std::string settingsPath = AppConfig::instance().getSettingsPath();
    if (FileSystemManager::instance().fileExists(settingsPath)) {
        std::ifstream f(settingsPath);
        if (f.is_open()) {
            std::stringstream buffer;
            buffer << f.rdbuf();
            std::string content = buffer.str();
            std::string apiKey = JsonHelper::extractString(content, "google_api_key");
            if (!apiKey.empty() && getSetting("google_api_key", "").empty()) {
                setSetting("google_api_key", apiKey);
                Logger::info("Imported google_api_key from settings.json into database.");
            }
        }
    }

    return true;
}

void DatabaseManager::close() {
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
        Logger::info("Closed SQLite database.");
    }
}

int DatabaseManager::getSchemaVersion() {
    if (!m_db) return -1;
    sqlite3_stmt* stmt = nullptr;
    int version = 0;
    if (sqlite3_prepare_v2(m_db, "PRAGMA user_version;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            version = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return version;
}

bool DatabaseManager::setSchemaVersion(int version) {
    if (!m_db) return false;
    std::string sql = "PRAGMA user_version = " + std::to_string(version) + ";";
    return executeSimpleQuery(sql.c_str());
}

bool DatabaseManager::migrateSchema() {
    int currentVer = getSchemaVersion();
    Logger::info("Current database schema version: " + std::to_string(currentVer));

    if (currentVer < 1) {
        Logger::info("Applying Schema v1 migration...");
        if (!executeSimpleQuery(SCHEMA_V1_SQL)) return false;
        if (!setSchemaVersion(1)) return false;
        Logger::info("Schema v1 migration applied successfully.");
    }
    return true;
}

bool DatabaseManager::seedDefaultSystems() {
    if (!m_db) return false;

    const char* checkSql = "SELECT COUNT(*) FROM systems;";
    sqlite3_stmt* stmt = nullptr;
    int count = 0;
    if (sqlite3_prepare_v2(m_db, checkSql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    if (count > 0) return true;

    Logger::info("Seeding default TrimUI gaming systems into database...");
    beginTransaction();

    const char* insertSql = R"(
        INSERT OR IGNORE INTO systems (code, name, rom_dir, img_dir, ext_list, sort_order)
        VALUES (?, ?, ?, ?, ?, ?);
    )";

    if (sqlite3_prepare_v2(m_db, insertSql, -1, &stmt, nullptr) != SQLITE_OK) {
        rollbackTransaction();
        return false;
    }

    for (size_t i = 0; i < DEFAULT_SYSTEMS_COUNT; ++i) {
        const auto& sys = DEFAULT_SYSTEMS[i];
        sqlite3_bind_text(stmt, 1, sys.code, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, sys.name, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, sys.rom_dir, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, sys.img_dir, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, sys.ext_list, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 6, sys.sort_order);

        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }

    sqlite3_finalize(stmt);
    commitTransaction();
    Logger::info("Seeded " + std::to_string(DEFAULT_SYSTEMS_COUNT) + " default systems.");
    return true;
}

std::vector<SystemRecord> DatabaseManager::getSystems(bool includeCounts) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<SystemRecord> list;
    if (!m_db) return list;

    const char* sql = includeCounts ? R"(
        SELECT s.id, s.code, s.name, s.rom_dir, s.img_dir, s.ext_list, s.icon_path, s.sort_order,
               COALESCE(SUM(CASE WHEN g.local_state = 1 THEN 1 ELSE 0 END), 0) AS local_count,
               COALESCE(SUM(CASE WHEN g.local_state = 0 THEN 1 ELSE 0 END), 0) AS cloud_count
        FROM systems s
        LEFT JOIN games g ON s.id = g.system_id
        GROUP BY s.id
        ORDER BY s.sort_order ASC;
    )" : R"(
        SELECT id, code, name, rom_dir, img_dir, ext_list, icon_path, sort_order, 0, 0
        FROM systems
        ORDER BY sort_order ASC;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            SystemRecord sys;
            sys.id = sqlite3_column_int(stmt, 0);
            sys.code = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            sys.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            sys.romDir = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            sys.imgDir = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            sys.extList = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            const char* icon = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            if (icon) sys.iconPath = icon;
            sys.sortOrder = sqlite3_column_int(stmt, 7);
            sys.localCount = sqlite3_column_int(stmt, 8);
            sys.cloudCount = sqlite3_column_int(stmt, 9);
            list.push_back(sys);
        }
        sqlite3_finalize(stmt);
    }
    return list;
}

bool DatabaseManager::getSystemByCode(const std::string& code, SystemRecord& outSystem) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;

    const char* sql = "SELECT id, code, name, rom_dir, img_dir, ext_list, icon_path, sort_order FROM systems WHERE code = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    bool found = false;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, code.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            outSystem.id = sqlite3_column_int(stmt, 0);
            outSystem.code = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            outSystem.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            outSystem.romDir = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            outSystem.imgDir = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            outSystem.extList = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            const char* icon = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            if (icon) outSystem.iconPath = icon;
            outSystem.sortOrder = sqlite3_column_int(stmt, 7);
            found = true;
        }
        sqlite3_finalize(stmt);
    }
    return found;
}

bool DatabaseManager::getSystemById(int systemId, SystemRecord& outSystem) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;

    const char* sql = "SELECT id, code, name, rom_dir, img_dir, ext_list, icon_path, sort_order FROM systems WHERE id = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    bool found = false;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, systemId);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            outSystem.id = sqlite3_column_int(stmt, 0);
            outSystem.code = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            outSystem.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            outSystem.romDir = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            outSystem.imgDir = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            outSystem.extList = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            const char* icon = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            if (icon) outSystem.iconPath = icon;
            outSystem.sortOrder = sqlite3_column_int(stmt, 7);
            found = true;
        }
        sqlite3_finalize(stmt);
    }
    return found;
}

std::vector<GameRecord> DatabaseManager::getGamesBySystem(int systemId, int stateFilter, const std::string& searchQuery) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<GameRecord> list;
    if (!m_db) return list;

    std::string sql = "SELECT id, cloud_file_id, system_id, filename, title, size_bytes, mime_type, drive_modified_time, checksum_sha256, local_path, local_state, cover_path, created_at, updated_at FROM games WHERE system_id = ?";

    if (stateFilter >= 0) {
        sql += " AND local_state = " + std::to_string(stateFilter);
    }
    if (!searchQuery.empty()) {
        sql += " AND (title LIKE ? OR filename LIKE ?)";
    }
    sql += " ORDER BY title ASC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        int paramIdx = 1;
        sqlite3_bind_int(stmt, paramIdx++, systemId);
        if (!searchQuery.empty()) {
            std::string pattern = "%" + searchQuery + "%";
            sqlite3_bind_text(stmt, paramIdx++, pattern.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, paramIdx++, pattern.c_str(), -1, SQLITE_TRANSIENT);
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            GameRecord g;
            g.id = sqlite3_column_int64(stmt, 0);
            const char* cid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (cid) g.cloudFileId = cid;
            g.systemId = sqlite3_column_int(stmt, 2);
            g.filename = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            g.title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            g.sizeBytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 5));
            const char* mime = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            if (mime) g.mimeType = mime;
            const char* mod = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
            if (mod) g.driveModifiedTime = mod;
            const char* sha = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
            if (sha) g.checksumSha256 = sha;
            const char* lpath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
            if (lpath) g.localPath = lpath;
            g.localState = static_cast<GameState>(sqlite3_column_int(stmt, 10));
            const char* cov = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
            if (cov) g.coverPath = cov;
            g.createdAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
            g.updatedAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
            list.push_back(g);
        }
        sqlite3_finalize(stmt);
    }
    return list;
}

std::vector<GameRecord> DatabaseManager::searchAllGames(const std::string& query, int limit) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<GameRecord> list;
    if (!m_db || query.empty()) return list;

    std::string sql = "SELECT g.id, g.cloud_file_id, g.system_id, g.filename, g.title, g.size_bytes, g.mime_type, g.drive_modified_time, g.checksum_sha256, g.local_path, g.local_state, g.cover_path, g.created_at, g.updated_at, COALESCE(s.code,'') FROM games g LEFT JOIN systems s ON g.system_id = s.id WHERE g.title LIKE ? OR g.filename LIKE ? ORDER BY g.title ASC LIMIT ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        std::string pattern = "%" + query + "%";
        sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, limit);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            GameRecord g;
            g.id = sqlite3_column_int64(stmt, 0);
            const char* cid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (cid) g.cloudFileId = cid;
            g.systemId = sqlite3_column_int(stmt, 2);
            const char* fn = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            if (fn) g.filename = fn;
            const char* tt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            if (tt) g.title = tt;
            g.sizeBytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 5));
            const char* mime = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            if (mime) g.mimeType = mime;
            const char* mod = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
            if (mod) g.driveModifiedTime = mod;
            const char* sha = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
            if (sha) g.checksumSha256 = sha;
            const char* lpath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
            if (lpath) g.localPath = lpath;
            g.localState = static_cast<GameState>(sqlite3_column_int(stmt, 10));
            const char* cov = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
            if (cov) g.coverPath = cov;
            const char* cat = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
            if (cat) g.createdAt = cat;
            const char* uat = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
            if (uat) g.updatedAt = uat;
            const char* scode = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 14));
            if (scode) g.systemCode = scode;
            list.push_back(g);
        }
        sqlite3_finalize(stmt);
    }
    return list;
}

std::vector<GameRecord> DatabaseManager::getGamesFiltered(int systemId, int stateFilter, const std::string& searchQuery, int limit, int offset, int& outTotalCount) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::vector<GameRecord> list;
    outTotalCount = 0;
    if (!m_db) return list;

    std::string whereClause = " WHERE 1=1";
    if (systemId > 0) {
        whereClause += " AND g.system_id = " + std::to_string(systemId);
    }
    if (stateFilter >= 0) {
        whereClause += " AND g.local_state = " + std::to_string(stateFilter);
    }
    if (!searchQuery.empty()) {
        whereClause += " AND (g.title LIKE ? OR g.filename LIKE ?)";
    }

    // 1. Get total count
    std::string countSql = "SELECT COUNT(*) FROM games g" + whereClause + ";";
    sqlite3_stmt* cStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, countSql.c_str(), -1, &cStmt, nullptr) == SQLITE_OK) {
        if (!searchQuery.empty()) {
            std::string pattern = "%" + searchQuery + "%";
            sqlite3_bind_text(cStmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(cStmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
        }
        if (sqlite3_step(cStmt) == SQLITE_ROW) {
            outTotalCount = sqlite3_column_int(cStmt, 0);
        }
        sqlite3_finalize(cStmt);
    }

    // 2. Fetch page items
    std::string sql = "SELECT g.id, g.cloud_file_id, g.system_id, g.filename, g.title, g.size_bytes, g.mime_type, g.drive_modified_time, g.checksum_sha256, g.local_path, g.local_state, g.cover_path, g.created_at, g.updated_at, COALESCE(s.code,'') FROM games g LEFT JOIN systems s ON g.system_id = s.id" + whereClause + " ORDER BY g.title ASC LIMIT ? OFFSET ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        int pIdx = 1;
        if (!searchQuery.empty()) {
            std::string pattern = "%" + searchQuery + "%";
            sqlite3_bind_text(stmt, pIdx++, pattern.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, pIdx++, pattern.c_str(), -1, SQLITE_TRANSIENT);
        }
        sqlite3_bind_int(stmt, pIdx++, limit > 0 ? limit : 50);
        sqlite3_bind_int(stmt, pIdx++, offset >= 0 ? offset : 0);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            GameRecord g;
            g.id = sqlite3_column_int64(stmt, 0);
            const char* cid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (cid) g.cloudFileId = cid;
            g.systemId = sqlite3_column_int(stmt, 2);
            const char* fn = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            if (fn) g.filename = fn;
            const char* tt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            if (tt) g.title = tt;
            g.sizeBytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 5));
            const char* mime = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            if (mime) g.mimeType = mime;
            const char* mod = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
            if (mod) g.driveModifiedTime = mod;
            const char* sha = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
            if (sha) g.checksumSha256 = sha;
            const char* lpath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
            if (lpath) g.localPath = lpath;
            g.localState = static_cast<GameState>(sqlite3_column_int(stmt, 10));
            const char* cov = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
            if (cov) g.coverPath = cov;
            const char* cat = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
            if (cat) g.createdAt = cat;
            const char* uat = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
            if (uat) g.updatedAt = uat;
            const char* scode = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 14));
            if (scode) g.systemCode = scode;
            list.push_back(g);
        }
        sqlite3_finalize(stmt);
    }
    return list;
}

bool DatabaseManager::getGameById(int64_t gameId, GameRecord& outGame) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;

    const char* sql = "SELECT id, cloud_file_id, system_id, filename, title, size_bytes, mime_type, drive_modified_time, checksum_sha256, local_path, local_state, cover_path, created_at, updated_at FROM games WHERE id = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    bool found = false;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, gameId);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            outGame.id = sqlite3_column_int64(stmt, 0);
            const char* cid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (cid) outGame.cloudFileId = cid;
            outGame.systemId = sqlite3_column_int(stmt, 2);
            outGame.filename = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            outGame.title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            outGame.sizeBytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 5));
            const char* mime = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            if (mime) outGame.mimeType = mime;
            const char* mod = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
            if (mod) outGame.driveModifiedTime = mod;
            const char* sha = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
            if (sha) outGame.checksumSha256 = sha;
            const char* lpath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
            if (lpath) outGame.localPath = lpath;
            outGame.localState = static_cast<GameState>(sqlite3_column_int(stmt, 10));
            const char* cov = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
            if (cov) outGame.coverPath = cov;
            outGame.createdAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
            outGame.updatedAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
            found = true;
        }
        sqlite3_finalize(stmt);
    }
    return found;
}

bool DatabaseManager::getGameByFilename(int systemId, const std::string& filename, GameRecord& outGame) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;

    const char* sql = "SELECT id, cloud_file_id, system_id, filename, title, size_bytes, mime_type, drive_modified_time, checksum_sha256, local_path, local_state, cover_path, created_at, updated_at FROM games WHERE system_id = ? AND filename = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    bool found = false;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, systemId);
        sqlite3_bind_text(stmt, 2, filename.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            outGame.id = sqlite3_column_int64(stmt, 0);
            const char* cid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (cid) outGame.cloudFileId = cid;
            outGame.systemId = sqlite3_column_int(stmt, 2);
            outGame.filename = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            outGame.title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            outGame.sizeBytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 5));
            const char* mime = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            if (mime) outGame.mimeType = mime;
            const char* mod = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
            if (mod) outGame.driveModifiedTime = mod;
            const char* sha = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
            if (sha) outGame.checksumSha256 = sha;
            const char* lpath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
            if (lpath) outGame.localPath = lpath;
            outGame.localState = static_cast<GameState>(sqlite3_column_int(stmt, 10));
            const char* cov = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
            if (cov) outGame.coverPath = cov;
            outGame.createdAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
            outGame.updatedAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
            found = true;
        }
        sqlite3_finalize(stmt);
    }
    return found;
}

bool DatabaseManager::getGameByCloudId(const std::string& cloudFileId, GameRecord& outGame) {
    if (cloudFileId.empty()) return false;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;

    const char* sql = "SELECT id, cloud_file_id, system_id, filename, title, size_bytes, mime_type, drive_modified_time, checksum_sha256, local_path, local_state, cover_path, created_at, updated_at FROM games WHERE cloud_file_id = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    bool found = false;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, cloudFileId.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            outGame.id = sqlite3_column_int64(stmt, 0);
            outGame.cloudFileId = cloudFileId;
            outGame.systemId = sqlite3_column_int(stmt, 2);
            outGame.filename = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            outGame.title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            outGame.sizeBytes = static_cast<uint64_t>(sqlite3_column_int64(stmt, 5));
            const char* mime = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            if (mime) outGame.mimeType = mime;
            const char* mod = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
            if (mod) outGame.driveModifiedTime = mod;
            const char* sha = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
            if (sha) outGame.checksumSha256 = sha;
            const char* lpath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
            if (lpath) outGame.localPath = lpath;
            outGame.localState = static_cast<GameState>(sqlite3_column_int(stmt, 10));
            const char* cov = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
            if (cov) outGame.coverPath = cov;
            outGame.createdAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
            outGame.updatedAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
            found = true;
        }
        sqlite3_finalize(stmt);
    }
    return found;
}

bool DatabaseManager::upsertGame(const GameRecord& game, int64_t* outInsertedId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;

    std::string now = getCurrentTimestamp();

    if (game.cloudFileId.empty()) {
        const char* checkLocalSql = "SELECT id FROM games WHERE system_id = ? AND filename = ? LIMIT 1;";
        sqlite3_stmt* checkStmt = nullptr;
        int64_t existingId = 0;
        if (sqlite3_prepare_v2(m_db, checkLocalSql, -1, &checkStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(checkStmt, 1, game.systemId);
            sqlite3_bind_text(checkStmt, 2, game.filename.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(checkStmt) == SQLITE_ROW) {
                existingId = sqlite3_column_int64(checkStmt, 0);
            }
            sqlite3_finalize(checkStmt);
        }

        if (existingId > 0) {
            const char* updateSql = "UPDATE games SET title = ?, size_bytes = ?, local_path = ?, local_state = ?, updated_at = ? WHERE id = ?;";
            sqlite3_stmt* ustmt = nullptr;
            if (sqlite3_prepare_v2(m_db, updateSql, -1, &ustmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(ustmt, 1, game.title.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_int64(ustmt, 2, game.sizeBytes);
                sqlite3_bind_text(ustmt, 3, game.localPath.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_int(ustmt, 4, static_cast<int>(game.localState));
                sqlite3_bind_text(ustmt, 5, now.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_int64(ustmt, 6, existingId);
                sqlite3_step(ustmt);
                sqlite3_finalize(ustmt);
                if (outInsertedId) *outInsertedId = existingId;
                return true;
            }
        } else {
            const char* insertLocalSql = R"(
                INSERT INTO games (cloud_file_id, system_id, filename, title, size_bytes, mime_type, drive_modified_time, checksum_sha256, local_path, local_state, cover_path, created_at, updated_at)
                VALUES (NULL, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
            )";
            sqlite3_stmt* instmt = nullptr;
            if (sqlite3_prepare_v2(m_db, insertLocalSql, -1, &instmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(instmt, 1, game.systemId);
                sqlite3_bind_text(instmt, 2, game.filename.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(instmt, 3, game.title.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_int64(instmt, 4, game.sizeBytes);
                sqlite3_bind_text(instmt, 5, game.mimeType.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(instmt, 6, game.driveModifiedTime.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(instmt, 7, game.checksumSha256.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(instmt, 8, game.localPath.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_int(instmt, 9, static_cast<int>(game.localState));
                sqlite3_bind_text(instmt, 10, game.coverPath.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(instmt, 11, now.c_str(), -1, SQLITE_STATIC);
                sqlite3_bind_text(instmt, 12, now.c_str(), -1, SQLITE_STATIC);
                sqlite3_step(instmt);
                int64_t lastId = sqlite3_last_insert_rowid(m_db);
                sqlite3_finalize(instmt);
                if (outInsertedId) *outInsertedId = lastId;
                return true;
            }
        }
    }

    const char* checkCloudSql = "SELECT id FROM games WHERE cloud_file_id = ? LIMIT 1;";
    sqlite3_stmt* checkCloudStmt = nullptr;
    int64_t existingCloudId = 0;
    if (sqlite3_prepare_v2(m_db, checkCloudSql, -1, &checkCloudStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(checkCloudStmt, 1, game.cloudFileId.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(checkCloudStmt) == SQLITE_ROW) {
            existingCloudId = sqlite3_column_int64(checkCloudStmt, 0);
        }
        sqlite3_finalize(checkCloudStmt);
    }

    if (existingCloudId > 0) {
        const char* uCloudSql = R"(
            UPDATE games SET
                filename = ?,
                title = ?,
                size_bytes = ?,
                mime_type = ?,
                drive_modified_time = ?,
                checksum_sha256 = ?,
                local_state = CASE WHEN ? != 0 THEN ? ELSE games.local_state END,
                updated_at = ?
            WHERE id = ?;
        )";
        sqlite3_stmt* ustmt = nullptr;
        if (sqlite3_prepare_v2(m_db, uCloudSql, -1, &ustmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(ustmt, 1, game.filename.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(ustmt, 2, game.title.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int64(ustmt, 3, game.sizeBytes);
            sqlite3_bind_text(ustmt, 4, game.mimeType.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(ustmt, 5, game.driveModifiedTime.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(ustmt, 6, game.checksumSha256.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(ustmt, 7, static_cast<int>(game.localState));
            sqlite3_bind_int(ustmt, 8, static_cast<int>(game.localState));
            sqlite3_bind_text(ustmt, 9, now.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int64(ustmt, 10, existingCloudId);
            sqlite3_step(ustmt);
            sqlite3_finalize(ustmt);
            if (outInsertedId) *outInsertedId = existingCloudId;
            return true;
        }
        return false;
    } else {
        const char* inSql = R"(
            INSERT INTO games (cloud_file_id, system_id, filename, title, size_bytes, mime_type, drive_modified_time, checksum_sha256, local_path, local_state, cover_path, created_at, updated_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
        )";
        sqlite3_stmt* instmt = nullptr;
        if (sqlite3_prepare_v2(m_db, inSql, -1, &instmt, nullptr) != SQLITE_OK) {
            Logger::error("upsertGame insert prepare error: " + std::string(m_db ? sqlite3_errmsg(m_db) : "NULL db"));
            return false;
        }

        sqlite3_bind_text(instmt, 1, game.cloudFileId.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(instmt, 2, game.systemId);
        sqlite3_bind_text(instmt, 3, game.filename.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(instmt, 4, game.title.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(instmt, 5, game.sizeBytes);
        sqlite3_bind_text(instmt, 6, game.mimeType.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(instmt, 7, game.driveModifiedTime.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(instmt, 8, game.checksumSha256.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(instmt, 9, game.localPath.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(instmt, 10, static_cast<int>(game.localState));
        sqlite3_bind_text(instmt, 11, game.coverPath.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(instmt, 12, now.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(instmt, 13, now.c_str(), -1, SQLITE_STATIC);

        int rc = sqlite3_step(instmt);
        int64_t lastId = sqlite3_last_insert_rowid(m_db);
        sqlite3_finalize(instmt);

        if (rc == SQLITE_DONE) {
            if (outInsertedId) *outInsertedId = lastId;
            return true;
        }
        Logger::error("upsertGame insert step failed: " + std::string(m_db ? sqlite3_errmsg(m_db) : "NULL db") + " code=" + std::to_string(rc));
        return false;
    }
}

bool DatabaseManager::updateGameLocalState(int64_t gameId, GameState state, const std::string& localPath) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;

    std::string now = getCurrentTimestamp();
    const char* sql = "UPDATE games SET local_state = ?, local_path = ?, updated_at = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, static_cast<int>(state));
        sqlite3_bind_text(stmt, 2, localPath.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, now.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 4, gameId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return true;
    }
    return false;
}

bool DatabaseManager::updateGameCover(int64_t gameId, const std::string& coverPath) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;

    std::string now = getCurrentTimestamp();
    const char* sql = "UPDATE games SET cover_path = ?, updated_at = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, coverPath.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, now.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 3, gameId);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return true;
    }
    return false;
}

bool DatabaseManager::markGameDeletedLocally(int64_t gameId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;

    const char* checkSql = "SELECT cloud_file_id, system_id, filename, local_path FROM games WHERE id = ?;";
    sqlite3_stmt* cstmt = nullptr;
    bool hasCloudId = false;
    std::string lPath;
    std::string fname;
    int sysId = 0;

    if (sqlite3_prepare_v2(m_db, checkSql, -1, &cstmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(cstmt, 1, gameId);
        if (sqlite3_step(cstmt) == SQLITE_ROW) {
            const char* cid = reinterpret_cast<const char*>(sqlite3_column_text(cstmt, 0));
            if (cid && std::strlen(cid) > 0) {
                hasCloudId = true;
            }
            sysId = sqlite3_column_int(cstmt, 1);
            const char* fn = reinterpret_cast<const char*>(sqlite3_column_text(cstmt, 2));
            if (fn) fname = fn;
            const char* lp = reinterpret_cast<const char*>(sqlite3_column_text(cstmt, 3));
            if (lp) lPath = lp;
        }
        sqlite3_finalize(cstmt);
    }

    // Ensure physical ROM file is removed from SD Card
    if (!lPath.empty() && FileSystemManager::instance().fileExists(lPath)) {
        FileSystemManager::instance().removeFile(lPath);
    } else if (!fname.empty() && sysId > 0) {
        SystemRecord sys;
        if (getSystemById(sysId, sys)) {
            std::string fallbackPath = AppConfig::instance().getSystemRomsDir(sys.romDir) + "/" + fname;
            if (FileSystemManager::instance().fileExists(fallbackPath)) {
                FileSystemManager::instance().removeFile(fallbackPath);
            }
        }
    }

    if (hasCloudId) {
        std::string now = getCurrentTimestamp();
        const char* sql = "UPDATE games SET local_state = 0, local_path = '', updated_at = ? WHERE id = ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 2, gameId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            return true;
        }
    } else {
        const char* sql = "DELETE FROM games WHERE id = ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, gameId);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            return true;
        }
    }
    return false;
}

bool DatabaseManager::clearCloudGames() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;
    executeSimpleQuery("DELETE FROM games WHERE local_state = 0;");
    executeSimpleQuery("UPDATE sync_state SET total_cloud_games = 0;");
    executeSimpleQuery("UPDATE settings SET value = 'Never' WHERE key = 'last_cloud_sync_time';");
    Logger::info("DatabaseManager: Cleared all un-downloaded cloud game records.");
    return true;
}

bool DatabaseManager::getGameCountsBySystem(int systemId, int& outLocal, int& outCloud) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    outLocal = 0;
    outCloud = 0;
    if (!m_db) return false;

    const char* sql = R"(
        SELECT 
            COALESCE(SUM(CASE WHEN local_state = 1 THEN 1 ELSE 0 END), 0) AS local_count,
            COALESCE(SUM(CASE WHEN local_state = 0 THEN 1 ELSE 0 END), 0) AS cloud_count
        FROM games WHERE system_id = ?;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, systemId);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            outLocal = sqlite3_column_int(stmt, 0);
            outCloud = sqlite3_column_int(stmt, 1);
        }
        sqlite3_finalize(stmt);
        return true;
    }
    return false;
}

bool DatabaseManager::getTotalGameCounts(int& outLocal, int& outCloud) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    outLocal = 0;
    outCloud = 0;
    if (!m_db) return false;

    const char* sql = R"(
        SELECT 
            COALESCE(SUM(CASE WHEN local_state = 1 THEN 1 ELSE 0 END), 0) AS local_count,
            COALESCE(SUM(CASE WHEN local_state = 0 THEN 1 ELSE 0 END), 0) AS cloud_count
        FROM games;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            outLocal = sqlite3_column_int(stmt, 0);
            outCloud = sqlite3_column_int(stmt, 1);
        }
        sqlite3_finalize(stmt);
        return true;
    }
    return false;
}

std::string DatabaseManager::getSetting(const std::string& key, const std::string& defaultValue) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return defaultValue;

    const char* sql = "SELECT value FROM settings WHERE key = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    std::string val = defaultValue;

    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            val = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    return val;
}

bool DatabaseManager::setSetting(const std::string& key, const std::string& value) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_db) return false;

    const char* sql = "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return true;
    }
    return false;
}

bool DatabaseManager::beginTransaction() {
    return executeSimpleQuery("BEGIN TRANSACTION;");
}

bool DatabaseManager::commitTransaction() {
    return executeSimpleQuery("COMMIT;");
}

bool DatabaseManager::rollbackTransaction() {
    return executeSimpleQuery("ROLLBACK;");
}

} // namespace RomCloud
