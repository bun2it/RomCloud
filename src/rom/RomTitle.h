#pragma once
// cleanDisplayTitle — tên game HIỂN THỊ suy từ filename: cắt đuôi ext,
// bỏ tag dump "(USA)", "[!]", "(v1.0)"..., trim space/-/_ hai đầu.
// Đây là thuật toán CHUẨN DUY NHẤT: Drive sync và quét thẻ dùng chung
// để title trong DB luôn nhất quán (không flip-flop giữa 2 nguồn),
// cover/metadata lookup theo filename nên không ảnh hưởng.
// Chỉ xử lý byte ASCII (cắt ext, bỏ ngoặc, trim) nên an toàn với tên
// file UTF-8/Tiếng Việt — không cắt giữa ký tự nhiều byte.
#include <string>

namespace RomCloud {

inline std::string cleanDisplayTitle(const std::string& filename) {
    std::string base = filename;
    size_t lastDot = base.find_last_of('.');
    if (lastDot != std::string::npos) {
        base = base.substr(0, lastDot);
    }

    // Strip dump tags like (USA), [!], (v1.0), etc.
    std::string cleaned;
    bool inParen = false, inBracket = false;
    for (char c : base) {
        if (c == '(') inParen = true;
        else if (c == ')') { inParen = false; continue; }
        else if (c == '[') inBracket = true;
        else if (c == ']') { inBracket = false; continue; }
        else if (!inParen && !inBracket) {
            cleaned += c;
        }
    }

    // Trim trailing whitespace and dashes
    while (!cleaned.empty() && (cleaned.back() == ' ' || cleaned.back() == '-' || cleaned.back() == '_')) {
        cleaned.pop_back();
    }
    while (!cleaned.empty() && (cleaned.front() == ' ' || cleaned.front() == '-' || cleaned.front() == '_')) {
        cleaned.erase(0, 1);
    }

    return cleaned.empty() ? filename : cleaned;
}

} // namespace RomCloud
