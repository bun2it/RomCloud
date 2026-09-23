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
