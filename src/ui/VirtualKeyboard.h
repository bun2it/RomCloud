#pragma once
// RomCloud VirtualKeyboard — logic ban phim ao QWERTY dung chung, khong SDL.
// UIManager / FileExplorer chỉ giữ 1 VkState mỗi ô nhập liệu thay vì 5 bộ
// m_*KbRow/Col/Shift roi rac. Render van do UIManager dam nhiem (doi UiRenderer
// tách xong sẽ chuyển render về đây). Tích hợp sẵn TelexHelper::processTelex.
// Mapping tay cam chuan (giong stock keyboard):
//   UP/DOWN/LEFT/RIGHT: di chuyển | A: chọn | B: hủy/thoát | X: space
//   Y: xoa (popUtf8) | L1: toggle Shift (abc/ABC)
//   R1: toggle Bo chu (ABC/123: chu <-> so & ky tu dac biet)
//   SELECT: toggle Telex (TELEX/US) | START: OK
// Layout: 4 hang phim (10 cot) + 1 hang action (5 o: Shift/ABC-Cach/Xoa/Xong/Huy)
#include <string>

#include "TelexHelper.h"

namespace RomCloud {

struct VkState {
    std::string query;
    int row = 0;          // 0..4 (4 = hang action)
    int col = 0;          // 0..9 (hang 0..3) hoac 0..4 (hang action)
    bool shift = false;
    bool telexMode = true;
    bool symbolMode = false;  // false = chu (ABC), true = so & ky tu (123) — giong stock R1
    bool inResults = false;
    size_t maxLen = 60;
    int charset = 0;      // 0 = Explorer (qwerty/asdf...), 1 = Media YT/TT (123/qwerty/asdf/zxcv)
};

enum class VkAction {
    None,       // vua nhap ky tu / doi trang thai, o lai ban phim
    Commit,     // START / o "Xong" — query san sang commit
    Cancel,     // B khi query rong / o "Huy" — thoat khong commit
};

class VirtualKeyboard {
public:
    static const char* lowerRows[4];
    static const char* upperRows[4];
    // Charset media YT/TT cũ: hằng số + qwerty thiếu o (pixel-identical legacy).
    static const char* mediaLowerRows[4];
    static const char* mediaUpperRows[4];
    // Bo chu so & ky tu dac biet (R1 toggle, giong stock keyboard).
    static const char* symbolLowerRows[4];
    static const char* symbolUpperRows[4];

    static void reset(VkState& s, bool telexDefault = true) {
        s.query.clear();
        s.row = 0;
        s.col = 0;
        s.shift = false;
        s.telexMode = telexDefault;
        s.symbolMode = false;
        s.inResults = false;
    }

    // Lấy ký tự tại ô hiện tại (hàng 0..3). Trả '\0' nếu đang ở hàng action.
    static char charAt(const VkState& s) {
        if (s.row < 0 || s.row > 3 || s.col < 0 || s.col > 9) return '\0';
        if (s.symbolMode)
            return s.shift ? symbolUpperRows[s.row][s.col] : symbolLowerRows[s.row][s.col];
        if (s.charset == 1)
            return s.shift ? mediaUpperRows[s.row][s.col] : mediaLowerRows[s.row][s.col];
        return s.shift ? upperRows[s.row][s.col] : lowerRows[s.row][s.col];
    }

    // Nhận 1 ký tự vào query (có Telex nếu bật). Trả false nếu quá maxLen.
    static bool typeChar(VkState& s, char ch) {
        if (s.query.size() >= s.maxLen) return false;
        if (s.telexMode) s.query = TelexHelper::processTelex(s.query, ch);
        else s.query += ch;
        return true;
    }

    static void typeSpace(VkState& s) {
        if (s.query.size() >= s.maxLen) return;
        if (s.telexMode) s.query = TelexHelper::processTelex(s.query, ' ');
        else s.query += ' ';
    }

    // Append a literal string (TLD suffixes like ".com"). No Telex transform
    // (URLs are ASCII) — respects maxLen, stops at the limit.
    static bool typeText(VkState& s, const char* t) {
        if (!t) return false;
        for (const char* p = t; *p; ++p) {
            if (s.query.size() >= s.maxLen) return false;
            s.query += *p;
        }
        return true;
    }

    static void backspace(VkState& s) { TelexHelper::popUtf8(s.query); }

    // Điều hướng D-pad. Trả true nếu đã xử lý (để caller return early).
    // upFromResults: khi focus đang ở list kết quả, UP quay về bàn phím.
    // stride2: kieu media YT/TT cu — hang action col 0..9 buoc chan (col/2).
    static bool move(VkState& s, int dRow, int dCol, bool stride2 = false) {
        if (s.inResults) {
            if (dRow < 0) { s.inResults = false; s.row = 4; s.col = 0; return true; }
            return false; // dieu huong trong results do caller xu ly
        }
        if (dRow != 0) {
            s.row += dRow;
            if (s.row < 0) s.row = 4;
            if (s.row > 4) s.row = 0;
            clampCol(s, stride2);
            // Vào hàng action kiểu stride2: snap về ô chẵn gần nhất.
            if (stride2 && s.row == 4) s.col = (s.col / 2) * 2;
            return true;
        }
        if (dCol != 0) {
            if (s.row < 4) {
                s.col = (s.col + dCol + 10) % 10;
            } else if (stride2) {
                int a = (s.col / 2 + dCol + 5) % 5;
                s.col = a * 2;
            } else {
                s.col = (s.col + dCol + 5) % 5;
            }
            return true;
        }
        return false;
    }

    // Nhấn nút A (chọn ô hiện tại). onToggleNotice: callback hiện toast
    // khi đổi bộ chữ ABC/123 (truyền nullptr nếu không cần).
    template <typename ToastFn>
    static VkAction pressA(VkState& s, ToastFn&& toast, bool stride2 = false) {
        if (s.row < 4) {
            char ch = charAt(s);
            if (ch) typeChar(s, ch);
            return VkAction::None;
        }
        int idx = stride2 ? (s.col / 2) : s.col;
        return pressAction(idx, s, toast);
    }

    static VkAction pressA(VkState& s, bool stride2 = false) {
        return pressA(s, [](const char*) {}, stride2);
    }

    template <typename ToastFn>
    static VkAction pressAction(int idx, VkState& s, ToastFn&& toast) {
        switch (idx) {
            case 0: s.shift = !s.shift; return VkAction::None;
            case 1:
                s.symbolMode = !s.symbolMode;
                toast(s.symbolMode ? "Bàn phím: Số/Ký tự (123)" : "Bàn phím: Chữ (ABC)");
                return VkAction::None;
            case 2: typeSpace(s); return VkAction::None;
            case 3: TelexHelper::popUtf8(s.query); return VkAction::None;
            case 4: return s.query.empty() ? VkAction::Cancel : VkAction::Commit;
            default: return VkAction::None;
        }
    }

    // R1 (stock): doi bo chu ABC <-> 123.
    template <typename ToastFn>
    static void toggleSymbol(VkState& s, ToastFn&& toast) {
        s.symbolMode = !s.symbolMode;
        toast(s.symbolMode ? "Bàn phím: Số/Ký tự (123)" : "Bàn phím: Chữ (ABC)");
    }

    // SELECT: doi che do go TELEX <-> US.
    template <typename ToastFn>
    static void toggleTelex(VkState& s, ToastFn&& toast) {
        s.telexMode = !s.telexMode;
        toast(s.telexMode ? "Chế độ: TELEX" : "Chế độ: TIẾNG ANH (US)");
    }

private:
    static void clampCol(VkState& s, bool stride2 = false) {
        if (s.row < 4) { if (s.col > 9) s.col = 9; }
        else { int mx = stride2 ? 9 : 4; if (s.col > mx) s.col = mx; }
        if (s.col < 0) s.col = 0;
    }
};

inline const char* VirtualKeyboard::lowerRows[4] = {
    "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?", "1234567890"
};
inline const char* VirtualKeyboard::upperRows[4] = {
    "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM;:/", "!@#$%^&*()"
};
inline const char* VirtualKeyboard::mediaLowerRows[4] = {
    "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm()/"
};
inline const char* VirtualKeyboard::mediaUpperRows[4] = {
    "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM()/"
};
inline const char* VirtualKeyboard::symbolLowerRows[4] = {
    "1234567890", "!@#$%^&*()", "-/:;()_+=*", ".,?!'\"[]{}"
};
inline const char* VirtualKeyboard::symbolUpperRows[4] = {
    "1234567890", "~`|\\<>&$%^", "[]{}<>_+-=", ".,?!'\";:/-"
};

} // namespace RomCloud
