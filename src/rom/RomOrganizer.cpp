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
    return applyFixes(std::move(items), baseDir);
}

std::vector<MisplacedRomRecord> RomOrganizer::fixSystemFolder(const std::string& systemCode,
                                                             const std::string& romsBaseDir) {
    auto items = auditMisplacedRoms(romsBaseDir);
    std::vector<MisplacedRomRecord> mine;
    for (auto& it : items) {
        if (it.currentSystemCode == systemCode) mine.push_back(it);
    }
    if (mine.empty()) return mine;

    std::string baseDir = romsBaseDir.empty() ? AppConfig::instance().getRomsDir() : romsBaseDir;
    return applyFixes(std::move(mine), baseDir);
}

std::vector<MisplacedRomRecord> RomOrganizer::applyFixes(std::vector<MisplacedRomRecord> items,
                                                         const std::string& baseDir) {
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

// ---------------------------------------------------------------------------
// Chuan hoa ten folder he may
// ---------------------------------------------------------------------------

static std::string normalizeFolderKey(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (c == ' ' || c == '-' || c == '_' || c == '.') continue;
        out.push_back(static_cast<char>(::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

// key (da chuan hoa) -> ma he may chuan. CHI gom ma he / romDir trong DB
// (so hoa/thuong/khoang trang). KHONG doan alias (snes, mame, genesis...)
// vi ten folder phai trung khop cach dat cua StockOS — doan sai se lam
// game khong chay duoc. Folder ten la duoc xu ly bang cach chuyen FILE
// ROM ben trong vao dung folder he may (khong doi ten folder).
static std::unordered_map<std::string, std::string> buildFolderAliasMap(
    const std::vector<SystemRecord>& systems) {
    std::unordered_map<std::string, std::string> m;
    for (const auto& s : systems) {
        m[normalizeFolderKey(s.code)] = s.code;
        m[normalizeFolderKey(s.romDir)] = s.code;
    }
    return m;
}

int RomOrganizer::normalizeSystemFolders(const std::string& romsBaseDir) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_busy = true;
    int changed = 0;

    std::string baseDir = romsBaseDir.empty() ? AppConfig::instance().getRomsDir() : romsBaseDir;
    auto systems = DatabaseManager::instance().getSystems(false);
    std::unordered_map<std::string, SystemRecord> codeToSys;
    std::unordered_map<std::string, std::string> romDirToCode;
    for (const auto& s : systems) {
        codeToSys[s.code] = s;
        romDirToCode[s.romDir] = s.code;
    }
    auto aliasMap = buildFolderAliasMap(systems);

    DIR* dir = opendir(baseDir.c_str());
    if (!dir) {
        m_busy = false;
        return 0;
    }
    // Thu thap truoc (doi ten folder trong luc duyet readdir khong an toan)
    std::vector<std::string> subdirs;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string name = entry->d_name;
        if (name == "_INBOX") continue;
        std::string full = baseDir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            subdirs.push_back(name);
        }
    }
    closedir(dir);

    for (const auto& name : subdirs) {
        if (romDirToCode.find(name) != romDirToCode.end()) continue; // da chuan
        auto it = aliasMap.find(normalizeFolderKey(name));
        if (it == aliasMap.end()) continue; // folder la: xu ly o buoc sau
        const std::string& code = it->second;
        auto sysIt = codeToSys.find(code);
        if (sysIt == codeToSys.end()) continue;
        std::string targetDir = baseDir + "/" + sysIt->second.romDir;
        std::string srcDir = baseDir + "/" + name;
        if (srcDir == targetDir) continue;

        struct stat stTarget;
        bool targetExists = (stat(targetDir.c_str(), &stTarget) == 0);
        if (!targetExists) {
            // Doi ten truc tiep: "gba" -> "GBA"
            if (rename(srcDir.c_str(), targetDir.c_str()) == 0) {
                sync();
                ++changed;
                Logger::info("RomOrganizer: Doi ten folder " + name + " -> " +
                             sysIt->second.romDir);
            } else {
                Logger::warn("RomOrganizer: Khong doi ten duoc folder " + name);
            }
        } else {
            // Gop: ca "GBA" va "gba" ton tai -> don file vao folder chuan
            DIR* sd = opendir(srcDir.c_str());
            if (!sd) continue;
            std::vector<std::string> files;
            struct dirent* e2 = nullptr;
            while ((e2 = readdir(sd)) != nullptr) {
                if (e2->d_name[0] == '.') continue;
                files.push_back(e2->d_name);
            }
            closedir(sd);
            for (const auto& f : files) {
                std::string finalDst;
                safeMoveFile(srcDir + "/" + f, targetDir + "/" + f, finalDst);
            }
            rmdir(srcDir.c_str());
            sync();
            ++changed;
            Logger::info("RomOrganizer: Gop folder " + name + " vao " +
                         sysIt->second.romDir);
        }
    }

    m_busy = false;
    return changed;
}

// ---------------------------------------------------------------------------
// Sap xep de quy folder la (file ROM nam rac trong folder khong phai he may)
// ---------------------------------------------------------------------------

int RomOrganizer::organizeUnknownDirRecursive(
    const std::string& sourceDir, const std::string& baseDir,
    const std::unordered_map<std::string, SystemRecord>& codeToSys,
    OrganizeSummary& summary) {
    int moved = 0;
    DIR* dir = opendir(sourceDir.c_str());
    if (!dir) return 0;

    // Thu thap truoc de tranh sua folder trong luc duyet
    std::vector<std::pair<std::string, bool>> entries; // (ten, laFolder)
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string name = entry->d_name;
        if (name == "Imgs" || name == "imgs" || name == "media") continue;
        std::string full = sourceDir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        entries.emplace_back(name, S_ISDIR(st.st_mode));
    }
    closedir(dir);

    for (const auto& [name, isDir] : entries) {
        std::string full = sourceDir + "/" + name;
        if (isDir) {
            moved += organizeUnknownDirRecursive(full, baseDir, codeToSys, summary);
            // Xoa folder rong sau khi don sach
            DIR* chk = opendir(full.c_str());
            bool empty = true;
            if (chk) {
                struct dirent* e2 = nullptr;
                while ((e2 = readdir(chk)) != nullptr) {
                    std::string n2 = e2->d_name;
                    if (n2 != "." && n2 != "..") { empty = false; break; }
                }
                closedir(chk);
            }
            if (empty) rmdir(full.c_str());
            continue;
        }
        RomDetectionResult det = RomDetector::instance().detectSystem(full);
        if (!det.detected) {
            ++summary.filesSkipped;
            continue;
        }
        auto targetIt = codeToSys.find(det.systemCode);
        if (targetIt == codeToSys.end()) {
            ++summary.filesSkipped;
            continue;
        }
        std::string targetDir = baseDir + "/" + targetIt->second.romDir;
        FileSystemManager::instance().createDirectoryRecursive(targetDir);
        std::string finalDst;
        if (safeMoveFile(full, targetDir + "/" + name, finalDst)) {
            ++moved;
            ++summary.filesMoved;
            // Ghi DB
            GameRecord g;
            g.systemId = targetIt->second.id;
            g.filename = name;
            g.title = getStemFromPath(finalDst);
            g.localPath = finalDst;
            g.localState = GameState::LOCAL;
            struct stat st2;
            if (stat(finalDst.c_str(), &st2) == 0) g.sizeBytes = st2.st_size;
            DatabaseManager::instance().upsertGame(g);
        } else {
            summary.errors.push_back("Khong chuyen duoc: " + name);
        }
    }
    return moved;
}

// ---------------------------------------------------------------------------
// Lam tron 1 lan: chuan hoa folder -> sap xep file la -> dua file lac cho ve
// ---------------------------------------------------------------------------

OrganizeSummary RomOrganizer::organizeAll(const std::string& romsBaseDir) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_busy) return OrganizeSummary{};
        m_busy = true;
    }
    OrganizeSummary summary;
    std::string baseDir = romsBaseDir.empty() ? AppConfig::instance().getRomsDir() : romsBaseDir;

    if (!FileSystemManager::instance().directoryExists(baseDir)) {
        summary.errors.push_back("Khong tim thay thu muc Roms: " + baseDir);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_busy = false;
        return summary;
    }

    auto systems = DatabaseManager::instance().getSystems(false);
    std::unordered_map<std::string, SystemRecord> codeToSys;
    std::unordered_map<std::string, bool> knownDirs;
    for (const auto& s : systems) {
        codeToSys[s.code] = s;
        knownDirs[s.romDir] = true;
    }

    // Buoc 1: sua ten folder he may ve chuan ("gba" -> "GBA", gop trung)
    int renamed = normalizeSystemFolders(baseDir);
    summary.foldersRenamed = renamed;

    // Buoc 2: sap xep file trong cac folder la (khong phai he may, khac _INBOX)
    DIR* dir = opendir(baseDir.c_str());
    if (dir) {
        std::vector<std::string> unknownDirs;
        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') continue;
            std::string name = entry->d_name;
            if (name == "_INBOX") continue;
            if (knownDirs.find(name) != knownDirs.end()) continue;
            std::string full = baseDir + "/" + name;
            struct stat st;
            if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                unknownDirs.push_back(full);
            }
        }
        closedir(dir);
        for (const auto& ud : unknownDirs) {
            organizeUnknownDirRecursive(ud, baseDir, codeToSys, summary);
            // Xoa folder la neu da rong
            DIR* chk = opendir(ud.c_str());
            bool empty = true;
            if (chk) {
                struct dirent* e2 = nullptr;
                while ((e2 = readdir(chk)) != nullptr) {
                    std::string n2 = e2->d_name;
                    if (n2 != "." && n2 != "..") { empty = false; break; }
                }
                closedir(chk);
            }
            if (empty) rmdir(ud.c_str());
        }
    }

    // Buoc 3: dua file lac cho trong cac folder he may + root + _INBOX ve dung cho
    auto fixed = fixMisplacedRoms(baseDir);
    for (const auto& item : fixed) {
        if (item.fixed) ++summary.filesMoved;
    }
    sync();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_busy = false;
    }
    Logger::info("RomOrganizer::organizeAll xong: " +
                 std::to_string(summary.foldersRenamed) + " folder, " +
                 std::to_string(summary.filesMoved) + " file da chuyen");
    return summary;
}

} // namespace RomCloud
