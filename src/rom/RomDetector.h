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

    // Helper: CRC32 (hex 8 ky tu) cua cac file ben trong .zip, doc tu
    // central directory (khong giai nen). Dung cho vong MD5/CRC32.
    std::vector<std::string> getZipEntryCrc32s(const std::string& zipPath, size_t maxEntries = 50);

    // Helper: cap (ten file, CRC32 hex) ben trong .zip. Dung cho verdict
    // tuong thich arcade (FBNEO_OK / MAME_ONLY).
    std::vector<std::pair<std::string, std::string>> getZipEntryNameCrcs(
        const std::string& zipPath, size_t maxEntries = 200);

    // Verdict tuong thich arcade cho 1 file .zip (chi co nghia khi file la
    // ROM arcade): FBNEO_OK = core moi an duoc; MAME_ONLY = dump kieu MAME
    // (core moi khong an, de o MAME); UNKNOWN = khong du chung cu (de yen).
    enum class ArcadeCompat { UNKNOWN, FBNEO_OK, MAME_ONLY };
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
