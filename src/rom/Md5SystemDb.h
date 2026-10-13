#pragma once

#include <string>
#include <unordered_map>
#include <mutex>

namespace RomCloud {

// Md5SystemDb: tra cứu MD5 / CRC32 -> mã hệ máy ("GBA", "FC", ...).
// Dữ liệu đọc từ file assets/md5systems.dat
// (mỗi dòng: "<hash> <SYSTEMCODE>|<TÊN GAME>").
//   - hash 32 ký tự hex = MD5 của file ROM (khớp No-Intro DAT)
//   - hash 8 ký tự hex  = CRC32 của file bên trong .zip (đọc từ central
//     directory, khong can giai nen — khop truong crc trong No-Intro DAT)
// File dữ liệu được sinh bởi tools/gen_md5db.py từ No-Intro DATs.
// Load lazy 1 lần khi tra cứu đầu tiên; thiếu file thì tra cứu trả về false
// (khong crash, vong 1 van hoat dong binh thuong).
class Md5SystemDb {
public:
    static Md5SystemDb& instance();

    // Tra cứu hash (md5 32 ký tự hoặc crc32 8 ký tự, hex thường).
    // Trả về true + outSystemCode nếu tìm thấy.
    bool lookup(const std::string& hashHex, std::string& outSystemCode);
    // Như trên + tên game gốc trong DAT ("" nếu dòng cũ không có tên).
    bool lookup(const std::string& hashHex, std::string& outSystemCode,
                std::string& outTitle);

    // Số mục đã nạp (0 = chưa nạp hoặc file không tồn tại).
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
