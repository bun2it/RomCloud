#include "FbneoCompatDb.h"
#include "../config/AppConfig.h"
#include "../logging/Logger.h"
#include <fstream>
#include <algorithm>
#include <cctype>

namespace RomCloud {

FbneoCompatDb& FbneoCompatDb::instance() {
    static FbneoCompatDb s_instance;
    return s_instance;
}

bool FbneoCompatDb::ensureLoaded() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_loadAttempted) return m_loaded;
    m_loadAttempted = true;

    std::string path = AppConfig::instance().getAssetsDir() + "/fbneosets.dat";
    std::ifstream file(path);
    if (!file.is_open()) {
        Logger::warn("FbneoCompatDb: khong tim thay " + path);
        return false;
    }

    std::string line;
    size_t count = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t p1 = line.find('|');
        if (p1 == std::string::npos) continue;
        size_t p2 = line.find('|', p1 + 1);
        if (p2 == std::string::npos) continue;
        std::string stem = line.substr(0, p1);
        std::string pair = line.substr(p1 + 1); // "romname|crc"
        std::transform(stem.begin(), stem.end(), stem.begin(), ::tolower);
        if (stem.empty() || pair.empty()) continue;
        m_table[stem].insert(pair);
        ++count;
    }

    m_loaded = true;
    Logger::info("FbneoCompatDb: da nap " + std::to_string(count) + " muc (" +
                 std::to_string(m_table.size()) + " set) tu " + path);
    return true;
}

bool FbneoCompatDb::hasSet(const std::string& stemLower) {
    if (!ensureLoaded()) return false;
    return m_table.find(stemLower) != m_table.end();
}

bool FbneoCompatDb::pairMatches(const std::string& stemLower,
                                const std::string& romName,
                                const std::string& crcHex) {
    if (!ensureLoaded()) return false;
    auto it = m_table.find(stemLower);
    if (it == m_table.end()) return false;
    std::string key = romName + "|" + crcHex;
    std::transform(key.begin() + romName.size() + 1, key.end(), key.begin() + romName.size() + 1, ::tolower);
    return it->second.find(key) != it->second.end();
}

std::string FbneoCompatDb::expectedCrc(const std::string& stemLower,
                                       const std::string& romName) {
    if (!ensureLoaded()) return std::string();
    auto it = m_table.find(stemLower);
    if (it == m_table.end()) return std::string();
    std::string prefix = romName + "|";
    for (const auto& e : it->second) {
        if (e.size() > prefix.size() && e.compare(0, prefix.size(), prefix) == 0) {
            return e.substr(prefix.size());
        }
    }
    return std::string();
}

size_t FbneoCompatDb::expectedCount(const std::string& stemLower) {
    if (!ensureLoaded()) return 0;
    auto it = m_table.find(stemLower);
    return (it == m_table.end()) ? 0 : it->second.size();
}

} // namespace RomCloud
