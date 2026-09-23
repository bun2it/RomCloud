#include "RomOrganizer.h"
#include "../logging/Logger.h"
#include "../config/AppConfig.h"
#include "../filesystem/FileSystemManager.h"
#include "../database/DatabaseManager.h"
#include "../utils/HashHelper.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <algorithm>

namespace RomCloud {

RomOrganizer& RomOrganizer::instance() {
    static RomOrganizer s_instance;
    return s_instance;
}

std::string RomOrganizer::getInboxDir() const {
    std::string inbox = AppConfig::instance().getRomsDir() + "/_INBOX";
    FileSystemManager::instance().createDirectoryRecursive(inbox);
    return inbox;
}

static std::string getStemFromPath(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = base.rfind('.');
    if (dot == std::string::npos) return base;
    return base.substr(0, dot);
}

static std::string getFilenameFromPath(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

bool RomOrganizer::safeMoveFile(const std::string& src, const std::string& dst, std::string& outFinalDst) {
    outFinalDst = dst;
    if (src == dst) return true;

    struct stat stSrc;
    if (stat(src.c_str(), &stSrc) != 0) return false;

    // Check if destination file already exists
    struct stat stDst;
    if (stat(dst.c_str(), &stDst) == 0) {
        // If identical size and CRC32, source is a redundant duplicate
        if (stSrc.st_size == stDst.st_size) {
            std::string crcSrc = HashHelper::computeFileCrc32(src);
            std::string crcDst = HashHelper::computeFileCrc32(dst);
            if (!crcSrc.empty() && crcSrc == crcDst) {
                Logger::info("RomOrganizer: Duplicate file detected, removing redundant source: " + src);
                unlink(src.c_str());
                sync();
                return true;
            }
        }

        // Different file with same name -> append _fixed
        size_t dot = dst.rfind('.');
        if (dot != std::string::npos) {
            outFinalDst = dst.substr(0, dot) + "_fixed" + dst.substr(dot);
        } else {
            outFinalDst = dst + "_fixed";
        }
    }

    // Attempt direct rename
    if (rename(src.c_str(), outFinalDst.c_str()) == 0) {
        sync();
        return true;
    }

    // Fallback: chunked copy then delete
    std::ifstream in(src, std::ios::binary);
    if (!in.is_open()) return false;

    std::ofstream out(outFinalDst, std::ios::binary);
    if (!out.is_open()) return false;

    char buf[65536];
    while (in.read(buf, sizeof(buf))) {
        out.write(buf, in.gcount());
    }
    if (in.gcount() > 0) {
        out.write(buf, in.gcount());
    }

    in.close();
    out.close();

    // Verify size
    struct stat stNew;
    if (stat(outFinalDst.c_str(), &stNew) == 0 && stNew.st_size == stSrc.st_size) {
        unlink(src.c_str());
        sync();
        return true;
    }

    return false;
}

std::vector<MisplacedRomRecord> RomOrganizer::auditMisplacedRoms(const std::string& romsBaseDir) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_busy = true;

    std::vector<MisplacedRomRecord> results;
    std::string baseDir = romsBaseDir.empty() ? AppConfig::instance().getRomsDir() : romsBaseDir;
    if (!FileSystemManager::instance().directoryExists(baseDir)) {
        m_busy = false;
        return results;
    }

    auto systems = DatabaseManager::instance().getSystems(false);
    std::unordered_map<std::string, SystemRecord> codeToSys;
    for (const auto& s : systems) {
        codeToSys[s.code] = s;
    }

    // 1. Audit each system's directory (e.g. /mnt/SDCARD/Roms/FC/, GBA/, etc.)
    for (const auto& sys : systems) {
        std::string sysDir = baseDir + "/" + sys.romDir;
        DIR* dir = opendir(sysDir.c_str());
        if (!dir) continue;

        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') continue;
            std::string fname = entry->d_name;
            if (fname == "Imgs" || fname == "imgs" || fname == "media") continue;

            std::string fullPath = sysDir + "/" + fname;
            struct stat st;
            if (stat(fullPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;

            RomDetectionResult det;
            bool isCorrect = RomDetector::instance().isFileInCorrectSystem(fullPath, sys.code, det);
            if (!isCorrect && det.detected) {
                MisplacedRomRecord item;
                item.originalPath = fullPath;
                item.filename = fname;
                item.currentSystemCode = sys.code;
                item.detectedSystemCode = det.systemCode;
                item.detectedSystemName = det.systemName;
                item.confidence = det.confidence;
                item.reason = det.details;

                auto targetIt = codeToSys.find(det.systemCode);
                if (targetIt != codeToSys.end()) {
                    item.targetPath = baseDir + "/" + targetIt->second.romDir + "/" + fname;
                } else {
                    item.targetPath = baseDir + "/" + det.suggestedDir + "/" + fname;
                }

                results.push_back(item);
                Logger::info("RomOrganizer Audit: Found misplaced ROM " + fname + " in " + sys.code + " -> belongs to " + det.systemCode);
            }
        }
        closedir(dir);
    }

    // 2. Audit root Roms folder (files placed directly in /mnt/SDCARD/Roms/ without subfolder)
    DIR* rootDir = opendir(baseDir.c_str());
    if (rootDir) {
        struct dirent* entry = nullptr;
        while ((entry = readdir(rootDir)) != nullptr) {
            if (entry->d_name[0] == '.') continue;
            std::string fname = entry->d_name;
            std::string fullPath = baseDir + "/" + fname;

            struct stat st;
            if (stat(fullPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;

            RomDetectionResult det = RomDetector::instance().detectSystem(fullPath);
            if (det.detected) {
                MisplacedRomRecord item;
                item.originalPath = fullPath;
                item.filename = fname;
                item.currentSystemCode = "ROOT";
                item.detectedSystemCode = det.systemCode;
                item.detectedSystemName = det.systemName;
                item.confidence = det.confidence;
                item.reason = "Nằm ở thư mục gốc Roms, chưa đưa vào Emulator: " + det.details;

                auto targetIt = codeToSys.find(det.systemCode);
                if (targetIt != codeToSys.end()) {
                    item.targetPath = baseDir + "/" + targetIt->second.romDir + "/" + fname;
                } else {
                    item.targetPath = baseDir + "/" + det.suggestedDir + "/" + fname;
                }

                results.push_back(item);
            }
        }
        closedir(rootDir);
    }

    // 3. Audit _INBOX folder
    std::string inboxDir = baseDir + "/_INBOX";
    DIR* inDir = opendir(inboxDir.c_str());
    if (inDir) {
        struct dirent* entry = nullptr;
        while ((entry = readdir(inDir)) != nullptr) {
            if (entry->d_name[0] == '.') continue;
            std::string fname = entry->d_name;
            std::string fullPath = inboxDir + "/" + fname;

            struct stat st;
            if (stat(fullPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;

            RomDetectionResult det = RomDetector::instance().detectSystem(fullPath);
            if (det.detected) {
                MisplacedRomRecord item;
                item.originalPath = fullPath;
                item.filename = fname;
                item.currentSystemCode = "_INBOX";
                item.detectedSystemCode = det.systemCode;
                item.detectedSystemName = det.systemName;
                item.confidence = det.confidence;
                item.reason = "Nằm trong Hộp tiếp nhận _INBOX: " + det.details;

                auto targetIt = codeToSys.find(det.systemCode);
                if (targetIt != codeToSys.end()) {
                    item.targetPath = baseDir + "/" + targetIt->second.romDir + "/" + fname;
                } else {
                    item.targetPath = baseDir + "/" + det.suggestedDir + "/" + fname;
                }

                results.push_back(item);
            }
        }
        closedir(inDir);
    }

    m_busy = false;
    return results;
}

std::vector<MisplacedRomRecord> RomOrganizer::fixMisplacedRoms(const std::string& romsBaseDir) {
    auto items = auditMisplacedRoms(romsBaseDir);
    if (items.empty()) return items;

    std::string baseDir = romsBaseDir.empty() ? AppConfig::instance().getRomsDir() : romsBaseDir;
    auto& db = DatabaseManager::instance();

    for (auto& item : items) {
        SystemRecord targetSys;
        if (!db.getSystemByCode(item.detectedSystemCode, targetSys)) {
            item.fixed = false;
            item.statusMessage = "Lỗi: Không tìm thấy hệ máy đích " + item.detectedSystemCode + " trong database.";
            continue;
        }

        std::string targetDir = baseDir + "/" + targetSys.romDir;
        FileSystemManager::instance().createDirectoryRecursive(targetDir);

        std::string finalDst;
        if (!safeMoveFile(item.originalPath, item.targetPath, finalDst)) {
            item.fixed = false;
            item.statusMessage = "Lỗi: Không thể di chuyển file " + item.filename;
            continue;
        }

        item.targetPath = finalDst;
        item.fixed = true;

        // Also move existing boxart if present in old system's Imgs folder
        std::string stem = getStemFromPath(item.originalPath);
        std::string targetImgsDir = targetDir + "/Imgs";
        FileSystemManager::instance().createDirectoryRecursive(targetImgsDir);
        std::string newCoverPath = targetImgsDir + "/" + stem + ".png";

        if (item.currentSystemCode != "ROOT" && item.currentSystemCode != "_INBOX") {
            SystemRecord oldSys;
            if (db.getSystemByCode(item.currentSystemCode, oldSys)) {
                std::string oldCoverPath = baseDir + "/" + oldSys.romDir + "/Imgs/" + stem + ".png";
                if (FileSystemManager::instance().fileExists(oldCoverPath)) {
                    std::string finalCover;
                    safeMoveFile(oldCoverPath, newCoverPath, finalCover);
                    Logger::info("RomOrganizer: Moved boxart from " + oldCoverPath + " to " + newCoverPath);
                }
            }
        }

        // Update database record
        GameRecord existing;
        SystemRecord oldSys;
        int oldSysId = 0;
        if (db.getSystemByCode(item.currentSystemCode, oldSys)) {
            oldSysId = oldSys.id;
        }

        if (oldSysId > 0 && db.getGameByFilename(oldSysId, item.filename, existing)) {
            std::string coverToSet = FileSystemManager::instance().fileExists(newCoverPath) ? newCoverPath : existing.coverPath;
            db.moveGameToSystem(existing.id, targetSys.id, finalDst, coverToSet);
            Logger::info("RomOrganizer: Updated DB game #" + std::to_string(existing.id) + " to system " + targetSys.code);
        } else {
            // Check if game exists in target system or insert new
            if (!db.getGameByFilename(targetSys.id, getFilenameFromPath(finalDst), existing)) {
                GameRecord newGame;
                newGame.systemId = targetSys.id;
                newGame.filename = getFilenameFromPath(finalDst);
                newGame.title = stem;
                newGame.localPath = finalDst;
                newGame.localState = GameState::LOCAL;
                if (FileSystemManager::instance().fileExists(newCoverPath)) {
                    newGame.coverPath = newCoverPath;
                }
                struct stat st;
                if (stat(finalDst.c_str(), &st) == 0) {
                    newGame.sizeBytes = st.st_size;
                }
                db.upsertGame(newGame);
                Logger::info("RomOrganizer: Inserted new game record for " + newGame.filename + " into " + targetSys.code);
            }
        }

        item.statusMessage = "Đã chuyển thành công từ " + item.currentSystemCode + " sang " + item.detectedSystemName;
        Logger::info("RomOrganizer Fix: " + item.filename + " (" + item.currentSystemCode + " -> " + item.detectedSystemCode + ")");
    }

    return items;
}

std::vector<MisplacedRomRecord> RomOrganizer::organizeDirectory(const std::string& sourceDir, const std::string& romsBaseDir) {
    std::vector<MisplacedRomRecord> results;
    std::string baseDir = romsBaseDir.empty() ? AppConfig::instance().getRomsDir() : romsBaseDir;

    DIR* dir = opendir(sourceDir.c_str());
    if (!dir) return results;

    auto systems = DatabaseManager::instance().getSystems(false);
    std::unordered_map<std::string, SystemRecord> codeToSys;
    for (const auto& s : systems) {
        codeToSys[s.code] = s;
    }

    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string fname = entry->d_name;
        std::string fullPath = sourceDir + "/" + fname;

        struct stat st;
        if (stat(fullPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;

        RomDetectionResult det = RomDetector::instance().detectSystem(fullPath);
        if (det.detected) {
            MisplacedRomRecord item;
            item.originalPath = fullPath;
            item.filename = fname;
            item.currentSystemCode = getFilenameFromPath(sourceDir);
            item.detectedSystemCode = det.systemCode;
            item.detectedSystemName = det.systemName;
            item.confidence = det.confidence;
            item.reason = det.details;

            auto targetIt = codeToSys.find(det.systemCode);
            if (targetIt != codeToSys.end()) {
                std::string targetDir = baseDir + "/" + targetIt->second.romDir;
                FileSystemManager::instance().createDirectoryRecursive(targetDir);
                item.targetPath = targetDir + "/" + fname;

                std::string finalDst;
                if (safeMoveFile(fullPath, item.targetPath, finalDst)) {
                    item.targetPath = finalDst;
                    item.fixed = true;
                    item.statusMessage = "Đã đưa vào " + det.systemName;

                    // Insert to DB
                    GameRecord newGame;
                    newGame.systemId = targetIt->second.id;
                    newGame.filename = getFilenameFromPath(finalDst);
                    newGame.title = getStemFromPath(finalDst);
                    newGame.localPath = finalDst;
                    newGame.localState = GameState::LOCAL;
                    newGame.sizeBytes = st.st_size;
                    DatabaseManager::instance().upsertGame(newGame);
                } else {
                    item.fixed = false;
                    item.statusMessage = "Lỗi di chuyển file";
                }
            }
            results.push_back(item);
        }
    }
    closedir(dir);
    return results;
}

} // namespace RomCloud
