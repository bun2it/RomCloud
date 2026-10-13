#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>
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
    // Liệt kê các mục "romname|crc" của set; false nếu set lạ.
    bool getSetPairs(const std::string& stemLower,
                     std::vector<std::string>& out);
    // Đoán set từ ruột file (cho zip ĐỔI TÊN: stem lạ nhưng CRC khớp).
    // pairs = (romname, crcHex) đọc từ central directory (không giải nén).
    // Trả true + set khớp nhất khi: matched>=3, hoặc matched==expected>0
    // (set tí hon). Bỏ phiếu đa số nên file thừa (hack) không đánh lừa được;
    // BIOS chung (khớp 1-2 ROM của hàng chục set) bị loại.
    bool guessSetByContent(
        const std::vector<std::pair<std::string, std::string>>& romCrcs,
        std::string& outStem, size_t& outMatched, size_t& outExpected);

    size_t setCount() const { return m_table.size(); }
    bool isLoaded() const { return m_loaded; }

private:
    FbneoCompatDb() = default;
    ~FbneoCompatDb() = default;
    bool ensureLoaded();

    std::unordered_map<std::string, std::unordered_set<std::string>> m_table;
    // Index ngược CRC(lower) -> các stem chứa ROM có CRC đó. Dùng đoán set
    // cho zip đổi tên (không tốn quét toàn bảng mỗi lần).
    std::unordered_map<std::string, std::vector<std::string>> m_crcToStems;
    bool m_loaded = false;
    bool m_loadAttempted = false;
    std::mutex m_mutex;
};

} // namespace RomCloud
