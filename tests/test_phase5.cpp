#include <iostream>
#include <cassert>
#include <vector>
#include "../src/sync/DriveSyncEngine.h"
#include "../src/network/JsonHelper.h"
#include "../src/database/DatabaseManager.h"
#include "../src/filesystem/FileSystemManager.h"

using namespace RomCloud;

int main() {
    std::cout << "[TEST] Starting RomCloud Phase 5 Validation Suite..." << std::endl;

    // 1. Test Drive API JSON Array parsing
    std::string driveResponse = R"({
        "nextPageToken": "token_page_2",
        "files": [
            {
                "id": "drive_id_super_mario",
                "name": "Super Mario 64 (USA).z64",
                "size": "8388608",
                "md5Checksum": "20b854b23b020d86da1a5d00b41fe2ac",
                "modifiedTime": "2026-09-01T12:00:00Z",
                "mimeType": "application/octet-stream"
            },
            {
                "id": "drive_id_zelda_oot",
                "name": "Legend of Zelda, The - Ocarina of Time (USA).z64",
                "size": "33554432",
                "md5Checksum": "cd6959b867eb41940984852e69fa0675",
                "modifiedTime": "2026-09-02T15:30:00Z",
                "mimeType": "application/octet-stream"
            }
        ]
    })";

    auto files = JsonHelper::extractArrayObjects(driveResponse, "files");
    assert(files.size() == 2 && "Expected 2 files extracted from JSON array");
    std::cout << "[PASS] Extracted 2 files from Google Drive JSON response." << std::endl;

    std::string id0 = JsonHelper::extractString(files[0], "id");
    std::string name0 = JsonHelper::extractString(files[0], "name");
    uint64_t size0 = JsonHelper::extractUInt64(files[0], "size");
    std::string md5_0 = JsonHelper::extractString(files[0], "md5Checksum");

    assert(id0 == "drive_id_super_mario");
    assert(name0 == "Super Mario 64 (USA).z64");
    assert(size0 == 8388608);
    assert(md5_0 == "20b854b23b020d86da1a5d00b41fe2ac");
    std::cout << "[PASS] File attributes correctly parsed." << std::endl;

    // 2. Test Database Reconciliation (Local vs Cloud Preservation)
    std::string testDbPath = "/tmp/test_phase5_library.db";
    FileSystemManager::instance().removeFile(testDbPath);

    DatabaseManager& db = DatabaseManager::instance();
    db.init(testDbPath);

    SystemRecord n64Sys;
    db.getSystemByCode("N64", n64Sys);

    // Seed an existing LOCAL game on SD card
    GameRecord existingLocal;
    existingLocal.systemId = n64Sys.id;
    existingLocal.filename = "Super Mario 64 (USA).z64";
    existingLocal.title = "Super Mario 64";
    existingLocal.sizeBytes = 8388608;
    existingLocal.localState = GameState::LOCAL;
    existingLocal.localPath = "/mnt/SDCARD/Roms/N64/Super Mario 64 (USA).z64";
    db.upsertGame(existingLocal);

    // Simulate Sync discovering both files on Drive
    for (const auto& fileJson : files) {
        std::string fId = JsonHelper::extractString(fileJson, "id");
        std::string fName = JsonHelper::extractString(fileJson, "name");
        uint64_t fSize = JsonHelper::extractUInt64(fileJson, "size");
        std::string fMd5 = JsonHelper::extractString(fileJson, "md5Checksum");
        std::string fMod = JsonHelper::extractString(fileJson, "modifiedTime");

        GameRecord match;
        if (db.getGameByFilename(n64Sys.id, fName, match)) {
            // Must preserve LOCAL state!
            match.cloudFileId = fId;
            match.driveModifiedTime = fMod;
            match.checksumSha256 = fMd5;
            db.upsertGame(match);
        } else {
            // New CLOUD-only game
            GameRecord newGame;
            newGame.systemId = n64Sys.id;
            newGame.cloudFileId = fId;
            newGame.filename = fName;
            newGame.title = "Legend of Zelda, The - Ocarina of Time";
            newGame.sizeBytes = fSize;
            newGame.checksumSha256 = fMd5;
            newGame.driveModifiedTime = fMod;
            newGame.localState = GameState::CLOUD;
            db.upsertGame(newGame);
        }
    }

    // Verify results
    GameRecord marioCheck;
    db.getGameByFilename(n64Sys.id, "Super Mario 64 (USA).z64", marioCheck);
    assert(marioCheck.localState == GameState::LOCAL && "Local state must be preserved!");
    assert(marioCheck.cloudFileId == "drive_id_super_mario" && "Cloud file ID must be linked!");
    std::cout << "[PASS] Existing local game preserved LOCAL state and gained Cloud ID." << std::endl;

    GameRecord zeldaCheck;
    db.getGameByFilename(n64Sys.id, "Legend of Zelda, The - Ocarina of Time (USA).z64", zeldaCheck);
    assert(zeldaCheck.localState == GameState::CLOUD && "New game must be CLOUD state!");
    assert(zeldaCheck.cloudFileId == "drive_id_zelda_oot");
    std::cout << "[PASS] New cloud game indexed with CLOUD state." << std::endl;

    int lCount = 0, cCount = 0;
    db.getGameCountsBySystem(n64Sys.id, lCount, cCount);
    assert(lCount == 1 && cCount == 1);
    std::cout << "[PASS] System counts: 1 Local, 1 Cloud." << std::endl;

    db.close();
    FileSystemManager::instance().removeFile(testDbPath);

    std::cout << "[SUCCESS] RomCloud Phase 5 Validation Suite PASSED!" << std::endl;
    return 0;
}
