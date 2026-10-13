#include "FbneoCompatDb.h"
#include "../config/AppConfig.h"
#include "../logging/Logger.h"
#include <fstream>
#include <algorithm>
#include <cctype>
#include <vector>
#include <utility>

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
        // Index ngược theo CRC để guessSetByContent tra nhanh.
        size_t pp = pair.rfind('|');
        if (pp != std::string::npos && pp + 1 < pair.size()) {
            std::string crc = pair.substr(pp + 1);
            std::transform(crc.begin(), crc.end(), crc.begin(), ::tolower);
            bool isHex = !crc.empty();
            for (char c : crc) {
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                    isHex = false;
                    break;
                }
            }
            if (isHex)
                m_crcToStems[crc].push_back(stem);
        }
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

bool FbneoCompatDb::getSetPairs(const std::string& stemLower,
                                std::vector<std::string>& out) {
    out.clear();
    if (!ensureLoaded()) return false;
    auto it = m_table.find(stemLower);
    if (it == m_table.end()) return false;
    for (const auto& e : it->second) out.push_back(e);
    return true;
}

bool FbneoCompatDb::guessSetByContent(
    const std::vector<std::pair<std::string, std::string>>& romCrcs,
    std::string& outStem, size_t& outMatched, size_t& outExpected) {
    outStem.clear();
    outMatched = 0;
    outExpected = 0;
    if (!ensureLoaded() || romCrcs.empty()) return false;

    // Ứng viên = mọi set chứa ít nhất 1 CRC trong file (so tên file +
    // CRC chính xác, lowercase cả 2 đầu).
    std::unordered_set<std::string> candidates;
    std::vector<std::pair<std::string, std::string>> norm;
    norm.reserve(romCrcs.size());
    for (const auto& p : romCrcs) {
        std::string nm = p.first;
        std::string crc = p.second;
        std::transform(nm.begin(), nm.end(), nm.begin(), ::tolower);
        std::transform(crc.begin(), crc.end(), crc.begin(), ::tolower);
        norm.push_back({nm, crc});
        auto it = m_crcToStems.find(crc);
        if (it != m_crcToStems.end()) {
            for (const auto& s : it->second) candidates.insert(s);
        }
    }
    if (candidates.empty()) return false;

    // Bỏ phiếu: set nào khớp nhiều cặp (tên+crc) nhất thì thắng.
    std::string best;
    size_t bestMatched = 0, bestExpected = 0;
    for (const auto& stem : candidates) {
        auto it = m_table.find(stem);
        if (it == m_table.end()) continue;
        size_t matched = 0;
        for (const auto& p : norm) {
            std::string key = p.first + "|" + p.second;
            if (it->second.find(key) != it->second.end()) ++matched;
        }
        size_t expected = it->second.size();
        bool accepted = (matched >= 3) || (expected > 0 && matched == expected);
        if (!accepted) continue;
        if (matched > bestMatched ||
            (matched == bestMatched && expected < bestExpected) ||
            (matched == bestMatched && expected == bestExpected &&
             (best.empty() || stem < best))) {
            best = stem;
            bestMatched = matched;
            bestExpected = expected;
        }
    }
    if (best.empty()) return false;
    outStem = best;
    outMatched = bestMatched;
    outExpected = bestExpected;
    return true;
}

} // namespace RomCloud
