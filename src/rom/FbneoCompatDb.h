#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

namespace RomCloud {

// FbneoCompatDb: bang tuong thich FBNeo (sinh boi tools/gen_fbneodb.py tu
// FBNeo Arcade DAT). Moi muc: stem set -> tap hop "romname|crc" FBNeo doi.
// Dùng để kết luận 1 file zip arcade chạy được core mới (FBNEO_OK) hay chỉ
// core MAME cũ (MAME_ONLY). Load lazy 1 lần; thiếu file -> mọi tra cứu coi
// như không có dữ liệu (UNKNOWN ở tầng gọi).
class FbneoCompatDb {
public:
    static FbneoCompatDb& instance();

    // Set có trong DAT không?
    bool hasSet(const std::string& stemLower);
    // Cặp (romname, crc) có khớp DAT cho stem không?
    bool pairMatches(const std::string& stemLower, const std::string& romName,
                     const std::string& crcHex);
    // CRC FBNeo đòi cho (stem, romname); "" nếu không có.
    std::string expectedCrc(const std::string& stemLower,
                            const std::string& romName);
    // Số file FBNeo đòi cho set; 0 nếu set lạ.
    size_t expectedCount(const std::string& stemLower);

    size_t setCount() const { return m_table.size(); }
    bool isLoaded() const { return m_loaded; }

private:
    FbneoCompatDb() = default;
    ~FbneoCompatDb() = default;
    bool ensureLoaded();

    std::unordered_map<std::string, std::unordered_set<std::string>> m_table;
    bool m_loaded = false;
    bool m_loadAttempted = false;
    std::mutex m_mutex;
};

} // namespace RomCloud
