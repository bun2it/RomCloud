#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include <unordered_map>
#include "RomDetector.h"

namespace RomCloud {

struct MisplacedRomRecord {
    std::string originalPath;
    std::string filename;
    std::string currentSystemCode;   // e.g. "FC", or "_INBOX", or "ROOT"
    std::string detectedSystemCode;  // e.g. "GBA"
    std::string detectedSystemName;  // e.g. "Game Boy Advance"
    std::string targetPath;
    std::string confidence;          // "EXTENSION", "HEADER_MAGIC", etc.
    std::string reason;
    bool fixed = false;
    std::string statusMessage;
};

// Ket qua tong hop 1 lan sap xep toan bo thu muc Roms
struct OrganizeSummary {
    int foldersRenamed = 0;  // folder he may duoc sua ten ve chuan
    int foldersMerged = 0;   // folder trung ten duoc gop lai
    int filesMoved = 0;      // file ROM duoc chuyen dung cho
    int filesSkipped = 0;    // file khong nhan dien duoc he may
    std::vector<std::string> errors;
};

class RomOrganizer {
public:
    static RomOrganizer& instance();

    // Audit all ROMs on SD card to find files in wrong folders
    std::vector<MisplacedRomRecord> auditMisplacedRoms(const std::string& romsBaseDir = "");

    // Automatically fix / move all misplaced ROMs to their correct folders
    std::vector<MisplacedRomRecord> fixMisplacedRoms(const std::string& romsBaseDir = "");

    // Chi sua file lac trong 1 folder he may (dung cho nut START o tab The nho).
    // Loc ket qua audit theo currentSystemCode roi ap dung nhu fixMisplacedRoms.
    std::vector<MisplacedRomRecord> fixSystemFolder(const std::string& systemCode,
                                                    const std::string& romsBaseDir = "");

    // Organize a single specific directory (e.g. /mnt/SDCARD/Roms/_INBOX)
    std::vector<MisplacedRomRecord> organizeDirectory(const std::string& sourceDir, const std::string& romsBaseDir = "");

    // Chuan hoa ten folder he may: "gba" -> "GBA", "snes" -> "SFC",
    // "Mame" -> "ARCADE"... Folder trung (ca "GBA" va "gba") duoc gop.
    // Tra ve so folder da sua/gop.
    int normalizeSystemFolders(const std::string& romsBaseDir = "");

    // Lam tron 1 lan: chuan hoa ten folder -> sap xep file trong folder la/
    // root/_INBOX -> dua file lac cho ve dung he. Chay lau: goi tu thread nen.
    OrganizeSummary organizeAll(const std::string& romsBaseDir = "");

    // Safely move a file across FAT32/exFAT with rename/copy+delete and sync
    bool safeMoveFile(const std::string& src, const std::string& dst, std::string& outFinalDst);

    // Ap dung di chuyen + cap nhat DB cho danh sach audit (dung chung cho
    // fixMisplacedRoms va fixSystemFolder).
    std::vector<MisplacedRomRecord> applyFixes(std::vector<MisplacedRomRecord> items,
                                               const std::string& baseDir);

    // Get the standard _INBOX path
    std::string getInboxDir() const;

    // Check if organizer is currently running
    bool isBusy() const { return m_busy; }

private:
    RomOrganizer() = default;
    ~RomOrganizer() = default;

    // Sap xep de quy 1 folder la (gom ca folder con ben trong)
    int organizeUnknownDirRecursive(const std::string& sourceDir,
                                    const std::string& baseDir,
                                    const std::unordered_map<std::string, SystemRecord>& codeToSys,
                                    OrganizeSummary& summary);

    std::mutex m_mutex;
    bool m_busy = false;
};

} // namespace RomCloud
