#pragma once

#include <string>
#include <unordered_map>
#include <mutex>

namespace RomCloud {

// Md5SystemDb: tra cuu MD5 / CRC32 -> ma he may ("GBA", "FC", ...).
// Du lieu doc tu file assets/md5systems.dat (moi dong: "<hash> <SYSTEMCODE>").
//   - hash 32 ky tu hex = MD5 cua file ROM (khop No-Intro DAT)
//   - hash 8 ky tu hex  = CRC32 cua file ben trong .zip (doc tu central
//     directory, khong can giai nen — khop truong crc trong No-Intro DAT)
// File du lieu duoc sinh boi tools/gen_md5db.py tu No-Intro DATs.
// Load lazy 1 lan khi tra cuu dau tien; thieu file thi tra cuu tra ve false
// (khong crash, vong 1 van hoat dong binh thuong).
class Md5SystemDb {
public:
    static Md5SystemDb& instance();

    // Tra cuu hash (md5 32 ky tu hoac crc32 8 ky tu, hex thuong).
    // Tra ve true + outSystemCode neu tim thay.
    bool lookup(const std::string& hashHex, std::string& outSystemCode);

    // So muc da nap (0 = chua nap hoac file khong ton tai).
    size_t entryCount() const { return m_table.size(); }
    bool isLoaded() const { return m_loaded; }

private:
    Md5SystemDb() = default;
    ~Md5SystemDb() = default;
    bool ensureLoaded();

    std::unordered_map<std::string, std::string> m_table;
    bool m_loaded = false;
    bool m_loadAttempted = false;
    std::mutex m_mutex;
};

} // namespace RomCloud
