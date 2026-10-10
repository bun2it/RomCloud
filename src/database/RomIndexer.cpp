#include "RomIndexer.h"
#include "../filesystem/FileSystemManager.h"
#include "../logging/Logger.h"
#include "../rom/RomOrganizer.h"

#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <unordered_set>

namespace RomCloud {

RomIndexer& RomIndexer::instance() {
    static RomIndexer instance;
    return instance;
}

std::string RomIndexer::cleanTitleFromFilename(const std::string& filename) {
    size_t lastDot = filename.find_last_of('.');
    if (lastDot != std::string::npos && lastDot > 0) {
        return filename.substr(0, lastDot);
    }
    return filename;
}

bool RomIndexer::isExtensionSupported(const std::string& filename, const std::string& extList) {
    size_t lastDot = filename.find_last_of('.');
    if (lastDot == std::string::npos || lastDot + 1 >= filename.length()) return false;

    std::string ext = filename.substr(lastDot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    size_t start = 0;
    while (start < extList.length()) {
        size_t end = extList.find('|', start);
        std::string token;
        if (end == std::string::npos) {
            token = extList.substr(start);
            start = extList.length();
        } else {
            token = extList.substr(start, end - start);
            start = end + 1;
        }

        std::transform(token.begin(), token.end(), token.begin(), ::tolower);
        if (token == ext) return true;
    }
    return false;
}

ScanProgress RomIndexer::scanSystem(const SystemRecord& system, const std::string& romsBaseDir, ScanProgressCallback progressCb) {
    ScanProgress progress;
    progress.currentSystem = system.code;

    std::string systemDir = romsBaseDir + "/" + system.romDir;
    if (!FileSystemManager::instance().directoryExists(systemDir)) {
        return progress;
    }

    DIR* dir = opendir(systemDir.c_str());
    if (!dir) return progress;

    auto& db = DatabaseManager::instance();
    db.beginTransaction();

    std::unordered_set<std::string> foundFiles;
    struct dirent* entry = nullptr;

    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;

        std::string fname = entry->d_name;
        std::string fullPath = systemDir + "/" + fname;

        struct stat st;
        if (stat(fullPath.c_str(), &st) != 0) continue;

        bool isFolderGame = false;
        uint64_t entrySize = 0;
        if (S_ISDIR(st.st_mode)) {
            // Folder game (e.g. a PS1 game kept as bin/cue in its own
            // folder): count the folder as one game when it directly
            // holds at least one ROM file.
            DIR* sub = opendir(fullPath.c_str());
            if (!sub) continue;
            struct dirent* subEntry = nullptr;
            while ((subEntry = readdir(sub)) != nullptr) {
                if (subEntry->d_name[0] == '.') continue;
                std::string subPath = fullPath + "/" + subEntry->d_name;
                struct stat subSt;
                if (stat(subPath.c_str(), &subSt) != 0 || !S_ISREG(subSt.st_mode)) continue;
                entrySize += static_cast<uint64_t>(subSt.st_size);
                if (isExtensionSupported(subEntry->d_name, system.extList)) isFolderGame = true;
            }
            closedir(sub);
            if (!isFolderGame) continue;
        } else if (S_ISREG(st.st_mode)) {
            if (!isExtensionSupported(fname, system.extList)) continue;
            entrySize = static_cast<uint64_t>(st.st_size);
        } else {
            continue;
        }

        foundFiles.insert(fname);
        progress.filesFound++;
        progress.currentFile = fname;

        GameRecord existing;
        if (db.getGameByFilename(system.id, fname, existing)) {
            if (existing.localState != GameState::LOCAL || existing.localPath != fullPath || existing.sizeBytes != entrySize) {
                db.updateGameLocalState(existing.id, GameState::LOCAL, fullPath);
                progress.filesUpdated++;
            }
        } else {
            GameRecord newGame;
            newGame.systemId = system.id;
            newGame.filename = fname;
            newGame.title = isFolderGame ? fname : cleanTitleFromFilename(fname);
            newGame.sizeBytes = entrySize;
            newGame.localPath = fullPath;
            newGame.localState = GameState::LOCAL;

            db.upsertGame(newGame);
            progress.filesAdded++;
        }

        if (progressCb) progressCb(progress);
    }

    closedir(dir);

    auto dbGames = db.getGamesBySystem(system.id, static_cast<int>(GameState::LOCAL));
    for (const auto& g : dbGames) {
        if (foundFiles.find(g.filename) == foundFiles.end()) {
            db.markGameDeletedLocally(g.id);
            progress.filesMissing++;
        }
    }

    db.commitTransaction();
    return progress;
}

ScanProgress RomIndexer::scanAllSystems(const std::string& romsBaseDir, ScanProgressCallback progressCb) {
    ScanProgress totalProgress;

    auto systems = DatabaseManager::instance().getSystems(false);

    Logger::info("Starting local ROM library indexing across " + std::to_string(systems.size()) + " systems...");

    for (const auto& sys : systems) {
        ScanProgress sp = scanSystem(sys, romsBaseDir, progressCb);
        totalProgress.filesFound += sp.filesFound;
        totalProgress.filesAdded += sp.filesAdded;
        totalProgress.filesUpdated += sp.filesUpdated;
        totalProgress.filesMissing += sp.filesMissing;
    }

    Logger::info("ROM Indexing finished: " + std::to_string(totalProgress.filesFound) + " found (" +
                 std::to_string(totalProgress.filesAdded) + " new, " +
                 std::to_string(totalProgress.filesUpdated) + " updated, " +
                 std::to_string(totalProgress.filesMissing) + " removed)");

    return totalProgress;
}

} // namespace RomCloud
