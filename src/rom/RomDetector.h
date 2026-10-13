#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "../database/DatabaseManager.h"

namespace RomCloud {

struct RomDetectionResult {
    bool detected = false;
    std::string systemCode;      // e.g. "GBA", "FC", "SFC", "MD", "PS", "ARCADE"
    std::string systemName;      // e.g. "Game Boy Advance"
    std::string confidence;      // "EXTENSION", "HEADER_MAGIC", "ZIP_INNER_FILE", "ARCADE_DB", "CHECKSUM"
    std::string details;         // Detailed explanation for user
    std::string suggestedDir;    // e.g. "GBA"
};

class RomDetector {
public:
    static RomDetector& instance();

    // Primary entry point: detect gaming system from file path
    RomDetectionResult detectSystem(const std::string& filePath);

    // Tier 1: Identify from file extension
    RomDetectionResult detectByExtension(const std::string& filename);

    // Tier 2: Identify from binary header / magic bytes
    RomDetectionResult detectByBinaryHeader(const std::string& filePath);

    // Tier 3: Identify from zip archive contents
    RomDetectionResult detectFromZip(const std::string& zipPath);

    // Tier 4: Identify via CRC32 / MD5 checksum (local or ScreenScraper)
    RomDetectionResult detectByChecksum(const std::string& filePath);

    // Helper: list filenames contained in a .zip file (without decompressing)
    std::vector<std::string> getZipEntries(const std::string& zipPath, size_t maxEntries = 50);

    // Helper: CRC32 (hex 8 ký tự) của các file bên trong .zip, đọc từ
    // central directory (không giải nén). Dùng cho vòng MD5/CRC32.
    std::vector<std::string> getZipEntryCrc32s(const std::string& zipPath, size_t maxEntries = 50);

    // Helper: cặp (tên file, CRC32 hex) bên trong .zip. Dùng cho verdict
    // tuong thich arcade (FBNEO_OK / MAME_ONLY).
    std::vector<std::pair<std::string, std::string>> getZipEntryNameCrcs(
        const std::string& zipPath, size_t maxEntries = 200);

    // Helper: như trên nhưng cho cả .zip lẫn .7z (.7z đọc qua 7zzs của
    // máy, không giải nén). Dùng để zip/7z đổi tên vẫn bắt được bằng ruột.
    std::vector<std::pair<std::string, std::string>> getArchiveEntryNameCrcs(
        const std::string& archivePath, size_t maxEntries = 200);

    // Parse output `7z l -slt` thành cặp (tên, CRC). Hàm tĩnh để test độc
    // lập không cần binary/device.
    static std::vector<std::pair<std::string, std::string>> parseSevenZipListing(
        const std::string& text);

    // Verdict tương thích arcade cho 1 file .zip (chỉ có nghĩa khi file là
    // ROM arcade): FBNEO_OK = core mới ăn được; MAME_ONLY = dump kiểu MAME
    // (core mới không ăn, để ở MAME); UNKNOWN = không đủ chứng cứ (để yên).
    enum class ArcadeCompat { UNKNOWN, FBNEO_OK, MAME_ONLY };

    // Chi tiết từng file trong set arcade (đọc central directory, không
    // giải nén): OK / THIẾU / SAI CRC / THỪA. Dùng cho màn "Chi tiết file".
    struct RomAuditEntry {
        std::string name;
        std::string expectedCrc; // "" nếu THỪA
        std::string actualCrc;   // "" nếu THIẾU
        int status = 0;          // 0=OK, 1=THIẾU, 2=SAI CRC, 3=THỪA
    };
    struct RomAuditResult {
        bool knownSet = false;
        std::string setName;
        std::vector<RomAuditEntry> entries;
        ArcadeCompat verdict = ArcadeCompat::UNKNOWN;
    };
    RomAuditResult auditArchive(const std::string& archivePath);
    ArcadeCompat detectArcadeCompat(const std::string& zipPath);

    // Helper: check if a system code is known/supported
    bool isKnownSystemCode(const std::string& code) const;

    // Helper: check if file is in the correct system directory
    bool isFileInCorrectSystem(const std::string& filePath, const std::string& currentSystemCode, RomDetectionResult& outResult);

private:
    RomDetector();
    ~RomDetector() = default;

    std::unordered_map<std::string, std::string> m_extToSystem;
    std::unordered_map<std::string, std::string> m_systemNames;
    void initSystemMaps();
};

} // namespace RomCloud
