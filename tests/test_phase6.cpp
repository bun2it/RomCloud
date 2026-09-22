#include <iostream>
#include <cassert>
#include <fstream>
#include "../src/utils/HashHelper.h"
#include "../src/database/DatabaseManager.h"
#include "../src/filesystem/FileSystemManager.h"

using namespace RomCloud;

int main() {
    std::cout << "[TEST] Starting RomCloud Phase 6 Validation Suite..." << std::endl;

    // 1. Test HashHelper MD5 accuracy
    std::string testFilePath = "/tmp/test_md5_sample.txt";
    {
        std::ofstream out(testFilePath, std::ios::binary);
        out << "The quick brown fox jumps over the lazy dog";
    }

    std::string expectedMd5 = "9e107d9d372bb6826bd81d3542a419d6";
    std::string actualMd5 = HashHelper::computeFileMd5(testFilePath);
    assert(actualMd5 == expectedMd5 && "MD5 checksum did not match standard test vector");
    std::cout << "[PASS] MD5 standard vector verified: " << actualMd5 << std::endl;

    FileSystemManager::instance().removeFile(testFilePath);

    // 2. Test SQLite State Transition: CLOUD (0) -> DOWNLOADING (2) -> LOCAL (1)
    std::string testDbPath = "/tmp/test_phase6_library.db";
    FileSystemManager::instance().removeFile(testDbPath);

    DatabaseManager& db = DatabaseManager::instance();
    db.init(testDbPath);

    SystemRecord gbaSys;
    db.getSystemByCode("GBA", gbaSys);

    GameRecord cloudGame;
    cloudGame.systemId = gbaSys.id;
    cloudGame.cloudFileId = "drive_file_id_metroid_fusion";
    cloudGame.filename = "Metroid Fusion (USA).gba";
    cloudGame.title = "Metroid Fusion";
    cloudGame.sizeBytes = 8388608;
    cloudGame.checksumSha256 = "c6569ec987910931eb6f65fe36294d13";
    cloudGame.localState = GameState::CLOUD;

    int64_t insertedId = 0;
    bool up = db.upsertGame(cloudGame, &insertedId);
    assert(up && insertedId > 0);
    std::cout << "[PASS] Seeded Cloud game (ID: " << insertedId << ")." << std::endl;

    // Simulate transition to DOWNLOADING
    bool stDown = db.updateGameLocalState(insertedId, GameState::DOWNLOADING);
    assert(stDown);
    GameRecord gCheck;
    db.getGameById(insertedId, gCheck);
    assert(gCheck.localState == GameState::DOWNLOADING);
    std::cout << "[PASS] Transition to DOWNLOADING verified." << std::endl;

    // Simulate download complete and transition to LOCAL
    std::string finalPath = "/mnt/SDCARD/Roms/GBA/Metroid Fusion (USA).gba";
    bool stLocal = db.updateGameLocalState(insertedId, GameState::LOCAL, finalPath);
    assert(stLocal);
    db.getGameById(insertedId, gCheck);
    assert(gCheck.localState == GameState::LOCAL);
    assert(gCheck.localPath == finalPath);
    std::cout << "[PASS] Transition to LOCAL verified with local_path saved." << std::endl;

    // System counts check
    int lCount = 0, cCount = 0;
    db.getGameCountsBySystem(gbaSys.id, lCount, cCount);
    assert(lCount == 1 && cCount == 0);
    std::cout << "[PASS] System counts: 1 Local, 0 Cloud." << std::endl;

    db.close();
    FileSystemManager::instance().removeFile(testDbPath);

    std::cout << "[SUCCESS] RomCloud Phase 6 Validation Suite PASSED!" << std::endl;
    return 0;
}
