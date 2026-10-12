#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

namespace RomCloud {

// FbneoCompatDb: bang tuong thich FBNeo (sinh boi tools/gen_fbneodb.py tu
// FBNeo Arcade DAT). Moi muc: stem set -> tap hop "romname|crc" FBNeo doi.
// Dung de ket luan 1 file zip arcade chay duoc core moi (FBNEO_OK) hay chi
// core MAME cu (MAME_ONLY). Load lazy 1 lan; thieu file -> moi tra cuu coi
// nhu khong co du lieu (UNKNOWN o tang goi).
class FbneoCompatDb {
public:
    static FbneoCompatDb& instance();

    // Set co trong DAT khong?
    bool hasSet(const std::string& stemLower);
    // Cap (romname, crc) co khop DAT cho stem khong?
    bool pairMatches(const std::string& stemLower, const std::string& romName,
                     const std::string& crcHex);
    // CRC FBNeo doi cho (stem, romname); "" neu khong co.
    std::string expectedCrc(const std::string& stemLower,
                            const std::string& romName);
    // So file FBNeo doi cho set; 0 neu set la.
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
