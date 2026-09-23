#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <sqlite3.h>

namespace RomCloud {

enum class GameState {
    CLOUD = 0,
    LOCAL = 1,
    DOWNLOADING = 2,
    STATE_ERROR = 3
};

struct SystemRecord {
    int id = 0;
    std::string code;
    std::string name;
    std::string romDir;
    std::string imgDir;
    std::string extList;
    std::string iconPath;
    int sortOrder = 0;
    int localCount = 0;
    int cloudCount = 0;
};

struct GameRecord {
    int64_t id = 0;
    std::string cloudFileId;
    int systemId = 0;
    std::string systemCode; // joined from systems table (may be empty)
    std::string filename;
    std::string title;
    uint64_t sizeBytes = 0;
    std::string mimeType;
    std::string driveModifiedTime;
    std::string checksumSha256;
    std::string localPath;
    GameState localState = GameState::CLOUD;
    std::string coverPath;
    std::string description;
    std::string developer;
    std::string publisher;
    std::string genre;
    std::string releaseYear;
    std::string createdAt;
    std::string updatedAt;
};

class DatabaseManager {
public:
    static DatabaseManager& instance();
    bool init(const std::string& dbPath);
    void close();
    bool isOpen() const { return m_db != nullptr; }

    int getSchemaVersion();
    bool setSchemaVersion(int version);
    bool migrateSchema();

    bool seedDefaultSystems();
    std::vector<SystemRecord> getSystems(bool includeCounts = true);
    bool getSystemByCode(const std::string& code, SystemRecord& outSystem);
    bool getSystemById(int systemId, SystemRecord& outSystem);

    std::vector<GameRecord> getGamesBySystem(int systemId, int stateFilter = -1, const std::string& searchQuery = "");
    std::vector<GameRecord> searchAllGames(const std::string& query, int limit = 60);
    std::vector<GameRecord> getGamesFiltered(int systemId, int stateFilter, const std::string& searchQuery, int limit, int offset, int& outTotalCount);
    std::vector<GameRecord> getUnscrapedLocalGames(int limit = 0);
    bool getGameById(int64_t gameId, GameRecord& outGame);
    bool getGameByFilename(int systemId, const std::string& filename, GameRecord& outGame);
    bool getGameByCloudId(const std::string& cloudFileId, GameRecord& outGame);

    bool upsertGame(const GameRecord& game, int64_t* outInsertedId = nullptr);
    bool updateGameLocalState(int64_t gameId, GameState state, const std::string& localPath = "");
    bool updateGameCover(int64_t gameId, const std::string& coverPath);
    bool updateGameMetadata(int64_t gameId, const std::string& description, const std::string& releaseYear, const std::string& developer, const std::string& genre, const std::string& coverPath = "");
    bool moveGameToSystem(int64_t gameId, int newSystemId, const std::string& newLocalPath, const std::string& newCoverPath = "");
    bool markGameDeletedLocally(int64_t gameId);
    bool clearCloudGames();

    bool getGameCountsBySystem(int systemId, int& outLocal, int& outCloud);
    bool getTotalGameCounts(int& outLocal, int& outCloud);

    std::string getSetting(const std::string& key, const std::string& defaultValue = "");
    bool setSetting(const std::string& key, const std::string& value);

    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();

private:
    DatabaseManager() = default;
    ~DatabaseManager();
    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    sqlite3* m_db = nullptr;
    std::recursive_mutex m_mutex;
    std::string m_dbPath;

    bool executeSimpleQuery(const char* sql);
    std::string getCurrentTimestamp();
};

} // namespace RomCloud
