#include <iostream>
#include <cassert>
#include "../src/config/AppConfig.h"
#include "../src/filesystem/FileSystemManager.h"
#include "../src/database/DatabaseManager.h"
#include "../src/database/RomIndexer.h"
#include "../src/logging/Logger.h"

void cleanupTestDb(const std::string& path) {
    auto& fs = RomCloud::FileSystemManager::instance();
    fs.removeFile(path);
    fs.removeFile(path + "-wal");
    fs.removeFile(path + "-shm");
}

void testDatabaseSchemaAndSystems() {
    std::string testDb = "/tmp/test_library_p2.db";
    cleanupTestDb(testDb);

    auto& db = RomCloud::DatabaseManager::instance();
    assert(db.init(testDb) == true);
    assert(db.getSchemaVersion() == 1);

    auto systems = db.getSystems(true);
    assert(systems.size() == 24);

    RomCloud::SystemRecord gba;
    assert(db.getSystemByCode("GBA", gba) == true);
    assert(gba.code == "GBA");
    assert(gba.name == "Game Boy Advance");
    assert(gba.romDir == "GBA");

    std::cout << "[PASS] testDatabaseSchemaAndSystems (Seeded " << systems.size() << " systems)" << std::endl;
}

void testGameUpsertAndStateTransitions() {
    auto& db = RomCloud::DatabaseManager::instance();

    RomCloud::SystemRecord gba;
    assert(db.getSystemByCode("GBA", gba) == true);

    // 1. Insert cloud-only game
    RomCloud::GameRecord cloudGame;
    cloudGame.cloudFileId = "drive_file_12345";
    cloudGame.systemId = gba.id;
    cloudGame.filename = "Pokemon - Emerald Version (USA).gba";
    cloudGame.title = "Pokemon - Emerald Version (USA)";
    cloudGame.sizeBytes = 16777216;
    cloudGame.localState = RomCloud::GameState::CLOUD;
    cloudGame.localPath = "";

    int64_t gameId = 0;
    assert(db.upsertGame(cloudGame, &gameId) == true);
    assert(gameId > 0);

    RomCloud::GameRecord fetched;
    assert(db.getGameById(gameId, fetched) == true);
    assert(fetched.localState == RomCloud::GameState::CLOUD);
    assert(fetched.cloudFileId == "drive_file_12345");

    // 2. Transition to LOCAL
    assert(db.updateGameLocalState(gameId, RomCloud::GameState::LOCAL, "/mnt/SDCARD/Roms/GBA/Pokemon - Emerald Version (USA).gba") == true);
    assert(db.getGameById(gameId, fetched) == true);
    assert(fetched.localState == RomCloud::GameState::LOCAL);

    // 3. Delete Local (reverts to CLOUD)
    assert(db.markGameDeletedLocally(gameId) == true);
    assert(db.getGameById(gameId, fetched) == true);
    assert(fetched.localState == RomCloud::GameState::CLOUD);

    // 4. Local-only game
    RomCloud::GameRecord localOnlyGame;
    localOnlyGame.systemId = gba.id;
    localOnlyGame.filename = "Custom_Homebrew.gba";
    localOnlyGame.title = "Custom Homebrew";
    localOnlyGame.sizeBytes = 4096;
    localOnlyGame.localState = RomCloud::GameState::LOCAL;
    localOnlyGame.localPath = "/mnt/SDCARD/Roms/GBA/Custom_Homebrew.gba";

    int64_t localId = 0;
    assert(db.upsertGame(localOnlyGame, &localId) == true);
    assert(localId > 0);

    assert(db.markGameDeletedLocally(localId) == true);
    assert(db.getGameById(localId, fetched) == false);

    std::cout << "[PASS] testGameUpsertAndStateTransitions" << std::endl;
}

void testRomIndexer() {
    auto& fs = RomCloud::FileSystemManager::instance();
    std::string fakeRomsDir = "/tmp/test_roms_p2";
    fs.createDirectoryRecursive(fakeRomsDir + "/GBA");
    fs.createDirectoryRecursive(fakeRomsDir + "/FC");

    std::ofstream(fakeRomsDir + "/GBA/Metroid_Fusion.gba") << "fake rom data";
    std::ofstream(fakeRomsDir + "/GBA/Zelda_Minish_Cap.zip") << "fake zip data";
    std::ofstream(fakeRomsDir + "/GBA/readme.txt") << "ignore me";
    std::ofstream(fakeRomsDir + "/FC/Super_Mario_Bros.nes") << "fake nes data";

    auto& indexer = RomCloud::RomIndexer::instance();
    auto result = indexer.scanAllSystems(fakeRomsDir);

    assert(result.filesFound == 3); // 2 GBA + 1 FC
    assert(result.filesAdded == 3);

    auto& db = RomCloud::DatabaseManager::instance();
    int totalLocal = 0, totalCloud = 0;
    assert(db.getTotalGameCounts(totalLocal, totalCloud) == true);
    assert(totalLocal == 3);

    std::cout << "[PASS] testRomIndexer (Indexed " << result.filesFound << " valid ROMs successfully)" << std::endl;
}

int main() {
    std::cout << "=== Running RomCloud Phase 2 Unit Tests ===" << std::endl;
    testDatabaseSchemaAndSystems();
    testGameUpsertAndStateTransitions();
    testRomIndexer();
    std::cout << "=== All Phase 2 Database & Indexer Tests Passed! ===" << std::endl;
    return 0;
}
