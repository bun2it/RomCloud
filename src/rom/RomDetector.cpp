#include "RomDetector.h"
#include "../logging/Logger.h"
#include "../ui/BoxartScraper.h"
#include "../utils/HashHelper.h"
#include <fstream>
#include <algorithm>
#include <cstring>
#include <cctype>

namespace RomCloud {

RomDetector& RomDetector::instance() {
    static RomDetector s_instance;
    return s_instance;
}

RomDetector::RomDetector() {
    initSystemMaps();
}

void RomDetector::initSystemMaps() {
    // Unique, unambiguous extensions -> System Code
    m_extToSystem["gba"]   = "GBA";
    m_extToSystem["gbc"]   = "GBC";
    m_extToSystem["gb"]    = "GB";
    m_extToSystem["sfc"]   = "SFC";
    m_extToSystem["smc"]   = "SFC";
    m_extToSystem["fig"]   = "SFC";
    m_extToSystem["swc"]   = "SFC";
    m_extToSystem["gd3"]   = "SFC";
    m_extToSystem["dx2"]   = "SFC";
    m_extToSystem["nes"]   = "FC";
    m_extToSystem["fds"]   = "FC";
    m_extToSystem["unf"]   = "FC";
    m_extToSystem["unif"]  = "FC";
    m_extToSystem["gen"]   = "MD";
    m_extToSystem["smd"]   = "MD";
    m_extToSystem["32x"]   = "MD";
    m_extToSystem["nds"]   = "NDS";
    m_extToSystem["n64"]   = "N64";
    m_extToSystem["v64"]   = "N64";
    m_extToSystem["z64"]   = "N64";
    m_extToSystem["pce"]   = "PCE";
    m_extToSystem["p8"]    = "PICO8";
    m_extToSystem["ws"]    = "WS";
    m_extToSystem["wsc"]   = "WS";
    m_extToSystem["a26"]   = "ATARI2600";
    m_extToSystem["a78"]   = "ATARI7800";
    m_extToSystem["lnx"]   = "LYNX";
    m_extToSystem["sms"]   = "MS";
    m_extToSystem["sg"]    = "MS";
    m_extToSystem["gg"]    = "GG";
    m_extToSystem["pak"]   = "OPENBOR";
    m_extToSystem["cso"]   = "PSP";
    m_extToSystem["gdi"]   = "DC";
    m_extToSystem["cdi"]   = "DC";

    // Friendly names for notifications
    m_systemNames["GBA"]       = "Game Boy Advance";
    m_systemNames["GBC"]       = "Game Boy Color";
    m_systemNames["GB"]        = "Game Boy";
    m_systemNames["FC"]        = "NES / Famicom";
    m_systemNames["SFC"]       = "Super Nintendo (SFC)";
    m_systemNames["MD"]        = "Mega Drive / Genesis";
    m_systemNames["PS"]        = "Sony PlayStation (PS1)";
    m_systemNames["PSP"]       = "PlayStation Portable";
    m_systemNames["N64"]       = "Nintendo 64";
    m_systemNames["NDS"]       = "Nintendo DS";
    m_systemNames["ARCADE"]    = "Arcade (MAME / FBNeo)";
    m_systemNames["NEOGEO"]    = "SNK Neo Geo";
    m_systemNames["PCE"]       = "PC Engine / TurboGrafx";
    m_systemNames["DC"]        = "Sega Dreamcast";
    m_systemNames["SS"]        = "Sega Saturn";
    m_systemNames["WS"]        = "WonderSwan";
    m_systemNames["PICO8"]     = "PICO-8";
    m_systemNames["ATARI2600"] = "Atari 2600";
    m_systemNames["ATARI7800"] = "Atari 7800";
    m_systemNames["LYNX"]      = "Atari Lynx";
    m_systemNames["MS"]        = "Master System";
    m_systemNames["GG"]        = "Game Gear";
    m_systemNames["SEGACD"]    = "Sega CD";
    m_systemNames["OPENBOR"]   = "OpenBOR";
}

bool RomDetector::isKnownSystemCode(const std::string& code) const {
    return m_systemNames.find(code) != m_systemNames.end();
}

static std::string getFileExtension(const std::string& filename) {
    size_t dot = filename.rfind('.');
    if (dot == std::string::npos || dot == filename.length() - 1) return "";
    std::string ext = filename.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

static std::string getFileStem(const std::string& filename) {
    size_t slash = filename.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? filename : filename.substr(slash + 1);
    size_t dot = base.rfind('.');
    if (dot == std::string::npos) return base;
    return base.substr(0, dot);
}

RomDetectionResult RomDetector::detectByExtension(const std::string& filename) {
    RomDetectionResult res;
    std::string ext = getFileExtension(filename);
    if (ext.empty()) return res;

    auto it = m_extToSystem.find(ext);
    if (it != m_extToSystem.end()) {
        res.detected = true;
        res.systemCode = it->second;
        res.suggestedDir = it->second;
        res.systemName = m_systemNames[it->second];
        res.confidence = "EXTENSION";
        res.details = "Nhận diện qua đuôi file độc quyền ." + ext;
        return res;
    }
    return res;
}

std::vector<std::string> RomDetector::getZipEntries(const std::string& zipPath, size_t maxEntries) {
    std::vector<std::string> entries;
    std::ifstream file(zipPath, std::ios::binary);
    if (!file.is_open()) return entries;

    file.seekg(0, std::ios::end);
    std::streamoff fileSize = file.tellg();
    if (fileSize < 22) return entries;

    // Search for End of Central Directory Record (EOCD: 0x06054b50) in last 65KB
    size_t searchLen = std::min<size_t>(fileSize, 65557);
    file.seekg(fileSize - searchLen, std::ios::beg);
    std::vector<uint8_t> buffer(searchLen);
    file.read(reinterpret_cast<char*>(buffer.data()), searchLen);

    int64_t eocdOffset = -1;
    for (int64_t i = searchLen - 22; i >= 0; --i) {
        if (buffer[i] == 0x50 && buffer[i+1] == 0x4b && buffer[i+2] == 0x05 && buffer[i+3] == 0x06) {
            eocdOffset = fileSize - searchLen + i;
            break;
        }
    }

    if (eocdOffset >= 0) {
        file.seekg(eocdOffset + 12, std::ios::beg);
        uint32_t cdSize = 0;
        uint32_t cdOffset = 0;
        uint16_t numEntries = 0;
        file.read(reinterpret_cast<char*>(&cdSize), 4);
        file.seekg(eocdOffset + 16, std::ios::beg);
        file.read(reinterpret_cast<char*>(&cdOffset), 4);
        file.seekg(eocdOffset + 10, std::ios::beg);
        file.read(reinterpret_cast<char*>(&numEntries), 2);

        file.seekg(cdOffset, std::ios::beg);
        while (file.good() && entries.size() < maxEntries) {
            uint32_t sig = 0;
            file.read(reinterpret_cast<char*>(&sig), 4);
            if (sig != 0x02014b50) break; // Central Directory File Header

            file.seekg(24, std::ios::cur);
            uint16_t fnameLen = 0, extraLen = 0, commentLen = 0;
            file.read(reinterpret_cast<char*>(&fnameLen), 2);
            file.read(reinterpret_cast<char*>(&extraLen), 2);
            file.read(reinterpret_cast<char*>(&commentLen), 2);
            file.seekg(8, std::ios::cur); // skip relative offset

            if (fnameLen > 0 && fnameLen < 1024) {
                std::string entryName(fnameLen, '\0');
                file.read(&entryName[0], fnameLen);
                entries.push_back(entryName);
            }
            file.seekg(extraLen + commentLen, std::ios::cur);
        }
        return entries;
    }

    // Fallback: parse local headers from beginning
    file.seekg(0, std::ios::beg);
    while (file.good() && entries.size() < maxEntries) {
        uint32_t sig = 0;
        file.read(reinterpret_cast<char*>(&sig), 4);
        if (sig != 0x04034b50) break; // Local File Header

        file.seekg(22, std::ios::cur);
        uint16_t fnameLen = 0, extraLen = 0;
        file.read(reinterpret_cast<char*>(&fnameLen), 2);
        file.read(reinterpret_cast<char*>(&extraLen), 2);

        if (fnameLen > 0 && fnameLen < 1024) {
            std::string entryName(fnameLen, '\0');
            file.read(&entryName[0], fnameLen);
            entries.push_back(entryName);
        }
        file.seekg(extraLen, std::ios::cur);
        // Note: Without jumping compressed size, we stop or rely on EOCD.
        break;
    }

    return entries;
}

static bool isNeoGeoGame(const std::string& stemLower) {
    if (stemLower.rfind("neogeo", 0) == 0) return true;
    if (stemLower.rfind("mslug", 0) == 0) return true;
    if (stemLower.rfind("kof", 0) == 0) return true;
    if (stemLower.rfind("samsho", 0) == 0) return true;
    if (stemLower.rfind("fatfur", 0) == 0) return true;
    if (stemLower.rfind("aof", 0) == 0) return true;
    if (stemLower.rfind("rbff", 0) == 0) return true;
    if (stemLower == "garou" || stemLower == "lastblad" || stemLower == "lastbld2") return true;
    if (stemLower == "pulstar" || stemLower == "blazstar" || stemLower == "wakuwak7") return true;
    if (stemLower == "nam1975" || stemLower == "spinmast" || stemLower == "magdrop3") return true;
    if (stemLower == "shocktro" || stemLower == "shocktr2" || stemLower == "viewpoin") return true;
    return false;
}

RomDetectionResult RomDetector::detectFromZip(const std::string& zipPath) {
    RomDetectionResult res;
    std::string stem = getFileStem(zipPath);
    std::string stemLower = stem;
    std::transform(stemLower.begin(), stemLower.end(), stemLower.begin(), ::tolower);

    // 1. Check if the zip file itself is an Arcade / NeoGeo MAME ROM set
    std::string arcadeTitle = BoxartScraper::instance().resolveArcadeTitle(stemLower);
    if (!arcadeTitle.empty() || stemLower == "neogeo" || stemLower == "pgm") {
        res.detected = true;
        if (isNeoGeoGame(stemLower)) {
            res.systemCode = "NEOGEO";
            res.suggestedDir = "NEOGEO";
            res.systemName = m_systemNames["NEOGEO"];
            res.details = "Khớp ROM Neo Geo: " + (arcadeTitle.empty() ? stem : arcadeTitle);
        } else {
            res.systemCode = "ARCADE";
            res.suggestedDir = "ARCADE";
            res.systemName = m_systemNames["ARCADE"];
            res.details = "Khớp ROM Arcade/MAME: " + (arcadeTitle.empty() ? stem : arcadeTitle);
        }
        res.confidence = "ARCADE_DB";
        return res;
    }

    // 2. Inspect inner entries inside the zip file
    auto entries = getZipEntries(zipPath, 30);
    if (!entries.empty()) {
        // Count extensions inside zip
        int arcadeChipCount = 0;
        for (const auto& entry : entries) {
            std::string innerExt = getFileExtension(entry);
            auto it = m_extToSystem.find(innerExt);
            if (it != m_extToSystem.end()) {
                res.detected = true;
                res.systemCode = it->second;
                res.suggestedDir = it->second;
                res.systemName = m_systemNames[it->second];
                res.confidence = "ZIP_INNER_FILE";
                res.details = "Bên trong file zip chứa ROM: " + entry + " (" + res.systemName + ")";
                return res;
            }

            // Check if entries look like arcade ROM chips (.bin, .rom, .p1, .sp2, etc.)
            if (innerExt == "rom" || innerExt == "p1" || innerExt == "p2" || innerExt == "sp2" ||
                innerExt == "v1" || innerExt == "v2" || innerExt == "c1" || innerExt == "m1") {
                arcadeChipCount++;
            }
        }

        if (arcadeChipCount >= 2 || (entries.size() > 3 && entries[0].find(".bin") != std::string::npos)) {
            res.detected = true;
            if (isNeoGeoGame(stemLower)) {
                res.systemCode = "NEOGEO";
                res.suggestedDir = "NEOGEO";
                res.systemName = m_systemNames["NEOGEO"];
            } else {
                res.systemCode = "ARCADE";
                res.suggestedDir = "ARCADE";
                res.systemName = m_systemNames["ARCADE"];
            }
            res.confidence = "ARCADE_CHIPS";
            res.details = "File zip chứa tập lệnh chip ROM Arcade/NeoGeo (" + std::to_string(entries.size()) + " files)";
            return res;
        }
    }

    return res;
}

RomDetectionResult RomDetector::detectByBinaryHeader(const std::string& filePath) {
    RomDetectionResult res;
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return res;

    // Read up to 64KB for header analysis
    std::vector<uint8_t> buf(65536, 0);
    file.read(reinterpret_cast<char*>(buf.data()), buf.size());
    std::streamsize bytesRead = file.gcount();
    if (bytesRead < 16) return res;

    // 1. NES / Famicom (iNES: 0x4E, 0x45, 0x53, 0x1A)
    if (bytesRead >= 4 && buf[0] == 0x4E && buf[1] == 0x45 && buf[2] == 0x53 && buf[3] == 0x1A) {
        res.detected = true;
        res.systemCode = "FC";
        res.suggestedDir = "FC";
        res.systemName = m_systemNames["FC"];
        res.confidence = "HEADER_MAGIC";
        res.details = "Header chuẩn iNES (NES / Famicom)";
        return res;
    }
    // FDS header: FDS\x1a
    if (bytesRead >= 4 && buf[0] == 0x46 && buf[1] == 0x44 && buf[2] == 0x53 && buf[3] == 0x1A) {
        res.detected = true;
        res.systemCode = "FC";
        res.suggestedDir = "FC";
        res.systemName = m_systemNames["FC"];
        res.confidence = "HEADER_MAGIC";
        res.details = "Header Famicom Disk System (FDS)";
        return res;
    }

    // 2. Nintendo 64 (Magic bytes)
    if (bytesRead >= 4) {
        // Big Endian .z64
        if (buf[0] == 0x80 && buf[1] == 0x37 && buf[2] == 0x12 && buf[3] == 0x40) {
            res.detected = true;
            res.systemCode = "N64";
            res.suggestedDir = "N64";
            res.systemName = m_systemNames["N64"];
            res.confidence = "HEADER_MAGIC";
            res.details = "Header chuẩn Nintendo 64 (Big Endian)";
            return res;
        }
        // Byte-swapped .v64
        if (buf[0] == 0x37 && buf[1] == 0x80 && buf[2] == 0x40 && buf[3] == 0x12) {
            res.detected = true;
            res.systemCode = "N64";
            res.suggestedDir = "N64";
            res.systemName = m_systemNames["N64"];
            res.confidence = "HEADER_MAGIC";
            res.details = "Header chuẩn Nintendo 64 (Byte-swapped)";
            return res;
        }
        // Little Endian .n64
        if (buf[0] == 0x40 && buf[1] == 0x12 && buf[2] == 0x37 && buf[3] == 0x80) {
            res.detected = true;
            res.systemCode = "N64";
            res.suggestedDir = "N64";
            res.systemName = m_systemNames["N64"];
            res.confidence = "HEADER_MAGIC";
            res.details = "Header chuẩn Nintendo 64 (Little Endian)";
            return res;
        }
    }

    // 3. Game Boy Advance (GBA)
    // Nintendo logo at offset 0x04..0x13
    if (bytesRead >= 192) {
        const uint8_t gbaLogoPrefix[] = {0x24, 0xFF, 0xAE, 0x51, 0x69, 0x9A, 0xA2, 0x21};
        if (memcmp(&buf[4], gbaLogoPrefix, sizeof(gbaLogoPrefix)) == 0) {
            res.detected = true;
            res.systemCode = "GBA";
            res.suggestedDir = "GBA";
            res.systemName = m_systemNames["GBA"];
            res.confidence = "HEADER_MAGIC";
            res.details = "Logo phần cứng Nintendo Game Boy Advance";
            return res;
        }
    }

    // 4. Game Boy / Game Boy Color (GB / GBC)
    // Logo at offset 0x0104..0x0133
    if (bytesRead >= 384) {
        const uint8_t gbLogoPrefix[] = {0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B};
        if (memcmp(&buf[0x104], gbLogoPrefix, sizeof(gbLogoPrefix)) == 0) {
            uint8_t cgbFlag = buf[0x143];
            bool isGbc = (cgbFlag == 0x80 || cgbFlag == 0xC0);
            res.detected = true;
            res.systemCode = isGbc ? "GBC" : "GB";
            res.suggestedDir = isGbc ? "GBC" : "GB";
            res.systemName = m_systemNames[res.systemCode];
            res.confidence = "HEADER_MAGIC";
            res.details = isGbc ? "Header Game Boy Color (CGB Flag)" : "Header Game Boy kinh điển (DMG)";
            return res;
        }
    }

    // 5. Mega Drive / Genesis (Offset 0x0100)
    if (bytesRead >= 512) {
        std::string segaSig(reinterpret_cast<char*>(&buf[0x100]), std::min<size_t>(bytesRead - 0x100, 32));
        if (segaSig.find("SEGA MEGA DRIVE") != std::string::npos ||
            segaSig.find("SEGA GENESIS") != std::string::npos ||
            segaSig.find("SEGA MEGADRIVE") != std::string::npos ||
            segaSig.find("SEGA 32X") != std::string::npos) {
            res.detected = true;
            res.systemCode = "MD";
            res.suggestedDir = "MD";
            res.systemName = m_systemNames["MD"];
            res.confidence = "HEADER_MAGIC";
            res.details = "Chữ ký hệ thống Sega Mega Drive / Genesis";
            return res;
        }
    }

    // 6. Super Nintendo (SFC / SNES) Checksum complement check
    // LoROM: 0x7FC0, HiROM: 0xFFC0, with 512-byte header: 0x81C0
    const size_t snesOffsets[] = {0x7FC0, 0x81C0, 0xFFC0};
    for (size_t off : snesOffsets) {
        if (static_cast<size_t>(bytesRead) >= off + 32) {
            uint16_t comp = buf[off + 0x1C] | (buf[off + 0x1D] << 8);
            uint16_t csum = buf[off + 0x1E] | (buf[off + 0x1F] << 8);
            if ((comp ^ csum) == 0xFFFF && csum != 0 && comp != 0) {
                res.detected = true;
                res.systemCode = "SFC";
                res.suggestedDir = "SFC";
                res.systemName = m_systemNames["SFC"];
                res.confidence = "HEADER_MAGIC";
                res.details = "Internal Header & Checksum SNES / SFC hợp lệ";
                return res;
            }
        }
    }

    // 7. PlayStation 1 (ISO / BIN at Sector 16: offset 0x8000 or 0x9300)
    const size_t psOffsets[] = {0x8000, 0x9300};
    for (size_t off : psOffsets) {
        if (static_cast<size_t>(bytesRead) >= off + 32) {
            if (buf[off + 1] == 'C' && buf[off + 2] == 'D' && buf[off + 3] == '0' && buf[off + 4] == '0' && buf[off + 5] == '1') {
                std::string desc(reinterpret_cast<char*>(&buf[off + 8]), 24);
                if (desc.find("PLAYSTATION") != std::string::npos) {
                    res.detected = true;
                    res.systemCode = "PS";
                    res.suggestedDir = "PS";
                    res.systemName = m_systemNames["PS"];
                    res.confidence = "HEADER_MAGIC";
                    res.details = "Định dạng đĩa ISO9660 PlayStation 1";
                    return res;
                }
            }
        }
    }

    // 8. PSP (CSO header: 'CISO')
    if (bytesRead >= 4 && buf[0] == 'C' && buf[1] == 'I' && buf[2] == 'S' && buf[3] == 'O') {
        res.detected = true;
        res.systemCode = "PSP";
        res.suggestedDir = "PSP";
        res.systemName = m_systemNames["PSP"];
        res.confidence = "HEADER_MAGIC";
        res.details = "Định dạng nén CISO PlayStation Portable (PSP)";
        return res;
    }

    // 9. Sega Dreamcast ('SEGA SEGAKATANA')
    std::string dcSig(reinterpret_cast<char*>(buf.data()), std::min<size_t>(bytesRead, 64));
    if (dcSig.find("SEGA SEGAKATANA") != std::string::npos) {
        res.detected = true;
        res.systemCode = "DC";
        res.suggestedDir = "DC";
        res.systemName = m_systemNames["DC"];
        res.confidence = "HEADER_MAGIC";
        res.details = "Chữ ký bảo mật Sega Dreamcast (KATANA)";
        return res;
    }

    // 10. Sega Saturn ('SEGA SEGASATURN')
    if (dcSig.find("SEGA SEGASATURN") != std::string::npos) {
        res.detected = true;
        res.systemCode = "SS";
        res.suggestedDir = "SS";
        res.systemName = m_systemNames["SS"];
        res.confidence = "HEADER_MAGIC";
        res.details = "Chữ ký bảo mật Sega Saturn";
        return res;
    }

    return res;
}

RomDetectionResult RomDetector::detectByChecksum(const std::string& filePath) {
    RomDetectionResult res;
    std::string crc = HashHelper::computeFileCrc32(filePath);
    if (crc.empty()) return res;

    // Check with ScreenScraper via BoxartScraper
    // Note: If needed, ScreenScraper returns <systeme id="..."> which can resolve rare unclassified ROMs.
    return res;
}

RomDetectionResult RomDetector::detectSystem(const std::string& filePath) {
    std::string ext = getFileExtension(filePath);

    // Tier 1: Check unique extension first
    RomDetectionResult res = detectByExtension(filePath);
    if (res.detected) return res;

    // Tier 2: If archive (.zip, .7z), inspect contents
    if (ext == "zip" || ext == "7z") {
        res = detectFromZip(filePath);
        if (res.detected) return res;
    }

    // Tier 3: Binary header magic check (.bin, .iso, .rom, or unknown)
    res = detectByBinaryHeader(filePath);
    if (res.detected) return res;

    // Tier 4: Check if filename matches Arcade MAME dictionary (even if not recognized by zip)
    std::string stem = getFileStem(filePath);
    std::string stemLower = stem;
    std::transform(stemLower.begin(), stemLower.end(), stemLower.begin(), ::tolower);
    std::string arcadeTitle = BoxartScraper::instance().resolveArcadeTitle(stemLower);
    if (!arcadeTitle.empty() || stemLower == "neogeo") {
        res.detected = true;
        if (isNeoGeoGame(stemLower)) {
            res.systemCode = "NEOGEO";
            res.suggestedDir = "NEOGEO";
            res.systemName = m_systemNames["NEOGEO"];
        } else {
            res.systemCode = "ARCADE";
            res.suggestedDir = "ARCADE";
            res.systemName = m_systemNames["ARCADE"];
        }
        res.confidence = "ARCADE_DB";
        res.details = "Khớp tên game Arcade trong từ điển MAME: " + arcadeTitle;
        return res;
    }

    // Tier 5: Fallback to checksum lookup
    res = detectByChecksum(filePath);
    if (res.detected) return res;

    return res;
}

bool RomDetector::isFileInCorrectSystem(const std::string& filePath, const std::string& currentSystemCode, RomDetectionResult& outResult) {
    outResult = detectSystem(filePath);
    if (!outResult.detected) {
        return true; // Cannot determine, assume ok
    }

    // Cross-system validation:
    // Does the detected system code match the current folder's system code?
    if (outResult.systemCode == currentSystemCode) {
        return true;
    }

    // Special allowance: ARCADE vs NEOGEO can coexist in some setups, but separate is cleaner
    return false;
}

} // namespace RomCloud
