#include "Md5SystemDb.h"
#include "../config/AppConfig.h"
#include "../logging/Logger.h"
#include <fstream>
#include <algorithm>
#include <cctype>

namespace RomCloud {

Md5SystemDb& Md5SystemDb::instance() {
    static Md5SystemDb s_instance;
    return s_instance;
}

static bool isHexHash(const std::string& s) {
    if (s.size() != 8 && s.size() != 32) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool Md5SystemDb::ensureLoaded() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_loadAttempted) return m_loaded;
    m_loadAttempted = true;

    std::string path = AppConfig::instance().getAssetsDir() + "/md5systems.dat";
    std::ifstream file(path);
    if (!file.is_open()) {
        Logger::warn("Md5SystemDb: khong tim thay " + path +
                     " (vong MD5 se bo qua)");
        return false;
    }

    std::string line;
    size_t count = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string hash = line.substr(0, sp);
        std::string code = line.substr(sp + 1);
        // cat khoang trang thua o cuoi ma he
        while (!code.empty() && (code.back() == ' ' || code.back() == '\r' ||
                                 code.back() == '\t')) {
            code.pop_back();
        }
        std::transform(hash.begin(), hash.end(), hash.begin(), ::tolower);
        if (!isHexHash(hash) || code.empty()) continue;
        // giu muc dau tien gap (uu tien DAT nap truoc)
        if (m_table.find(hash) == m_table.end()) {
            m_table.emplace(hash, code);
            ++count;
        }
    }

    m_loaded = true;
    Logger::info("Md5SystemDb: da nap " + std::to_string(count) +
                 " muc tu " + path);
    return true;
}

bool Md5SystemDb::lookup(const std::string& hashHex, std::string& outSystemCode) {
    if (!ensureLoaded()) return false;
    std::string key = hashHex;
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    auto it = m_table.find(key);
    if (it == m_table.end()) return false;
    outSystemCode = it->second;
    return true;
}

} // namespace RomCloud
