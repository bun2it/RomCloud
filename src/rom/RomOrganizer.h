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

    // Chỉ sửa file lạc trong 1 folder hệ máy (dùng cho nút START ở tab Thẻ nhớ).
    // Lọc kết quả audit theo currentSystemCode rồi áp dụng như fixMisplacedRoms.
    std::vector<MisplacedRomRecord> fixSystemFolder(const std::string& systemCode,
                                                    const std::string& romsBaseDir = "");

    // Organize a single specific directory (e.g. /mnt/SDCARD/Roms/_INBOX)
    std::vector<MisplacedRomRecord> organizeDirectory(const std::string& sourceDir, const std::string& romsBaseDir = "");

    // Chuẩn hóa tên folder hệ máy: "gba" -> "GBA", "snes" -> "SFC",
    // "Mame" -> "ARCADE"... Folder trùng (cả "GBA" và "gba") được gộp.
    // Trả về số folder đã sửa/gộp.
    int normalizeSystemFolders(const std::string& romsBaseDir = "");

    // Làm tròn 1 lần: chuẩn hóa tên folder -> sắp xếp file trong folder lạ/
    // root/_INBOX -> đưa file lạc chỗ về đúng hệ. Chạy lâu: gọi từ thread nền.
    OrganizeSummary organizeAll(const std::string& romsBaseDir = "");

    // Safely move a file across FAT32/exFAT with rename/copy+delete and sync
    bool safeMoveFile(const std::string& src, const std::string& dst, std::string& outFinalDst);

    // Áp dụng di chuyển + cập nhật DB cho danh sách audit (dùng chung cho
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

    // Sắp xếp đệ quy 1 folder lạ (gồm cả folder con bên trong)
    int organizeUnknownDirRecursive(const std::string& sourceDir,
                                    const std::string& baseDir,
                                    const std::unordered_map<std::string, SystemRecord>& codeToSys,
                                    OrganizeSummary& summary);

    std::mutex m_mutex;
    bool m_busy = false;
};

} // namespace RomCloud
