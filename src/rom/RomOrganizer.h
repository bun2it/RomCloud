#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <functional>
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

class RomOrganizer {
public:
    static RomOrganizer& instance();

    // Audit all ROMs on SD card to find files in wrong folders
    std::vector<MisplacedRomRecord> auditMisplacedRoms(const std::string& romsBaseDir = "");

    // Automatically fix / move all misplaced ROMs to their correct folders
    std::vector<MisplacedRomRecord> fixMisplacedRoms(const std::string& romsBaseDir = "");

    // Organize a single specific directory (e.g. /mnt/SDCARD/Roms/_INBOX)
    std::vector<MisplacedRomRecord> organizeDirectory(const std::string& sourceDir, const std::string& romsBaseDir = "");

    // Safely move a file across FAT32/exFAT with rename/copy+delete and sync
    bool safeMoveFile(const std::string& src, const std::string& dst, std::string& outFinalDst);

    // Get the standard _INBOX path
    std::string getInboxDir() const;

    // Check if organizer is currently running
    bool isBusy() const { return m_busy; }

private:
    RomOrganizer() = default;
    ~RomOrganizer() = default;

    std::mutex m_mutex;
    bool m_busy = false;
};

} // namespace RomCloud
