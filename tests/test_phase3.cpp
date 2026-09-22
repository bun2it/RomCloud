#include <iostream>
#include <cassert>
#include <vector>
#include "../src/database/DatabaseManager.h"
#include "../src/database/RomIndexer.h"
#include "../src/database/Schema.h"
#include "../src/filesystem/FileSystemManager.h"

using namespace RomCloud;

int main() {
    std::cout << "[TEST] Starting RomCloud Phase 3 Validation Suite..." << std::endl;

    std::string testDbPath = "/tmp/test_phase3_library.db";
    FileSystemManager::instance().removeFile(testDbPath);

    DatabaseManager& db = DatabaseManager::instance();
    bool dbOk = db.init(testDbPath);
    assert(dbOk && "DatabaseManager init failed");
    std::cout << "[PASS] Database initialized." << std::endl;

    // Verify 24 systems
    auto systems = db.getSystems(true);
    assert(systems.size() == 24 && "Expected 24 default systems");
    std::cout << "[PASS] 24 systems loaded." << std::endl;

    // Add mock games (both local and cloud)
    SystemRecord gbaSys;
    bool foundGba = db.getSystemByCode("GBA", gbaSys);
    assert(foundGba && "GBA system not found");

    GameRecord localGame;
    localGame.systemId = gbaSys.id;
    localGame.filename = "Pokemon - Emerald Version (USA).gba";
    localGame.title = "Pokemon - Emerald Version";
    localGame.sizeBytes = 16777216;
    localGame.localState = GameState::LOCAL;
    localGame.localPath = "/mnt/SDCARD/Roms/GBA/Pokemon - Emerald Version (USA).gba";
    bool up1 = db.upsertGame(localGame);
    assert(up1 && "Failed to upsert local game");

    GameRecord cloudGame;
    cloudGame.systemId = gbaSys.id;
    cloudGame.cloudFileId = "drive_file_id_zelda_minish_cap";
    cloudGame.filename = "Legend of Zelda, The - The Minish Cap (USA).gba";
    cloudGame.title = "Legend of Zelda, The - The Minish Cap";
    cloudGame.sizeBytes = 16777216;
    cloudGame.localState = GameState::CLOUD;
    bool up2 = db.upsertGame(cloudGame);
    assert(up2 && "Failed to upsert cloud game");

    // Test Filters: All (-1), Local (1), Cloud (0)
    auto allGames = db.getGamesBySystem(gbaSys.id, -1);
    assert(allGames.size() == 2 && "Expected 2 total games for GBA");
    std::cout << "[PASS] Filter ALL: 2 games returned." << std::endl;

    auto localGames = db.getGamesBySystem(gbaSys.id, 1);
    assert(localGames.size() == 1 && "Expected 1 local game for GBA");
    assert(localGames[0].filename == localGame.filename);
    std::cout << "[PASS] Filter LOCAL: 1 local game returned." << std::endl;

    auto cloudGames = db.getGamesBySystem(gbaSys.id, 0);
    assert(cloudGames.size() == 1 && "Expected 1 cloud game for GBA");
    assert(cloudGames[0].filename == cloudGame.filename);
    std::cout << "[PASS] Filter CLOUD: 1 cloud game returned." << std::endl;

    // Test counts
    int lCount = 0, cCount = 0;
    db.getGameCountsBySystem(gbaSys.id, lCount, cCount);
    assert(lCount == 1 && cCount == 1);
    std::cout << "[PASS] System counts verified (Local: 1, Cloud: 1)." << std::endl;

    // Test delete transition
    bool delOk = db.markGameDeletedLocally(localGames[0].id);
    assert(delOk && "Failed to mark game deleted locally");

    auto localGamesAfter = db.getGamesBySystem(gbaSys.id, 1);
    assert(localGamesAfter.empty() && "Local game should be removed or transitioned to cloud");
    std::cout << "[PASS] Local game deletion reconciliation verified." << std::endl;

    db.close();
    FileSystemManager::instance().removeFile(testDbPath);

    std::cout << "[SUCCESS] RomCloud Phase 3 Validation Suite PASSED!" << std::endl;
    return 0;
}
