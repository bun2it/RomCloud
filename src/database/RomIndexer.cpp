#include "RomIndexer.h"
#include "../filesystem/FileSystemManager.h"
#include "../logging/Logger.h"
#include "../rom/RomOrganizer.h"
#include "../rom/RomTitle.h"

#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cctype>
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
    // Gom dir chính + dir trùng tên khác hoa/thường ("gba" cạnh "GBA"
    // trên FS phân biệt hoa/thường). FS không phân biệt (exFAT trên máy)
    // thì cùng 1 inode -> quét 1 lần (so dev+ino). Chỉ đọc, không đổi tên
    // (đổi tên là việc của organizeAll).
    std::vector<std::string> scanDirs;
    {
        struct stat dstRoot;
        bool haveRoot = (stat(systemDir.c_str(), &dstRoot) == 0 &&
                         S_ISDIR(dstRoot.st_mode));
        if (haveRoot) scanDirs.push_back(systemDir);
        DIR* base = opendir(romsBaseDir.c_str());
        if (base) {
            struct dirent* be = nullptr;
            while ((be = readdir(base)) != nullptr) {
                std::string n = be->d_name;
                if (n == "." || n == "..") continue;
                if (n.size() != system.romDir.size()) continue;
                std::string nl = n, rl = system.romDir;
                std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);
                std::transform(rl.begin(), rl.end(), rl.begin(), ::tolower);
                if (nl != rl) continue;
                std::string cand = romsBaseDir + "/" + n;
                struct stat cst;
                if (stat(cand.c_str(), &cst) != 0 || !S_ISDIR(cst.st_mode))
                    continue;
                bool dup = false;
                for (const auto& d : scanDirs) {
                    struct stat dst2;
                    if (stat(d.c_str(), &dst2) == 0 &&
                        dst2.st_dev == cst.st_dev && dst2.st_ino == cst.st_ino) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) scanDirs.push_back(cand);
            }
            closedir(base);
        }
    }
    if (scanDirs.empty()) {
        return progress;
    }

    auto& db = DatabaseManager::instance();
    db.beginTransaction();

    std::unordered_set<std::string> foundFiles;
    // Quét gộp nhiều folder vào cùng 1 hệ (foundFiles chung nên DB đúng).
    for (const std::string& curDir : scanDirs) {
    DIR* dir = opendir(curDir.c_str());
    if (!dir) continue;
    struct dirent* entry = nullptr;

    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;

        std::string fname = entry->d_name;
        std::string fullPath = curDir + "/" + fname;

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
            // Migrate một lần các title kiểu cũ (nguyên stem filename,
            // còn tag dump) sang tên hiển thị sạch. Chỉ đổi khi title
            // hiện tại đúng bằng giá trị auto cũ (hoặc rỗng) để không
            // chạm title đã chuẩn (VD từ Drive sync — cùng thuật toán
            // nên đã khớp, điều kiện dưới tự loại).
            std::string legacyAuto = isFolderGame ? fname : cleanTitleFromFilename(fname);
            std::string proper = cleanDisplayTitle(fname);
            if (existing.title != proper &&
                (existing.title.empty() || existing.title == legacyAuto)) {
                db.updateGameTitle(existing.id, proper);
                progress.filesUpdated++;
            }
        } else {
            GameRecord newGame;
            newGame.systemId = system.id;
            newGame.filename = fname;
            newGame.title = cleanDisplayTitle(fname);
            newGame.sizeBytes = entrySize;
            newGame.localPath = fullPath;
            newGame.localState = GameState::LOCAL;

            db.upsertGame(newGame);
            progress.filesAdded++;
        }

        if (progressCb) progressCb(progress);
    }
    closedir(dir);
    } // het 1 folder (chinh + bien the hoa/thuong)

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
