#pragma once
// ============================================================================
// RomCloud VirtualKeyboard — logic ban phim ao QWERTY dung chung, khong SDL.
// UIManager / FileExplorer chi giu 1 VkState moi o nhap lieu thay vi 5 bo
// m_*KbRow/Col/Shift roi rac. Render van do UIManager dam nhiem (doi UiRenderer
// tach xong se chuyen render ve day). Tich hop san TelexHelper::processTelex.
// Mapping tay cam chuan:
//   UP/DOWN/LEFT/RIGHT: di chuyen | A: chon | B: huy/thoat | X: space
//   Y: xoa (popUtf8) | L1/Y-shift: toggle Shift | R1: toggle Telex | START: OK
// Layout: 4 hang phim (10 cot) + 1 hang action (5 o: Shift/Space/Xoa/Xong/Huy)
// ============================================================================
#include <string>

#include "TelexHelper.h"

namespace RomCloud {

struct VkState {
    std::string query;
    int row = 0;          // 0..4 (4 = hang action)
    int col = 0;          // 0..9 (hang 0..3) hoac 0..4 (hang action)
    bool shift = false;
    bool telexMode = true;
    bool inResults = false;
    size_t maxLen = 60;
};

enum class VkAction {
    None,       // vua nhap ky tu / doi trang thai, o lai ban phim
    Commit,     // START / o "Xong" — query san sang commit
    Cancel,     // B khi query rong / o "Huy" — thoat khong commit
    Backspace,  // B khi query con chu — giong hanh vi YouTube hien tai
};

class VirtualKeyboard {
public:
    static const char* lowerRows[4];
    static const char* upperRows[4];

    static void reset(VkState& s, bool telexDefault = true) {
        s.query.clear();
        s.row = 0;
        s.col = 0;
        s.shift = false;
        s.telexMode = telexDefault;
        s.inResults = false;
    }

    // Lay ky tu tai o hien tai (hang 0..3). Tra '\0' neu dang o hang action.
    static char charAt(const VkState& s) {
        if (s.row < 0 || s.row > 3 || s.col < 0 || s.col > 9) return '\0';
        return s.shift ? upperRows[s.row][s.col] : lowerRows[s.row][s.col];
    }

    // Nhan 1 ky tu vao query (co Telex neu bat). Tra false neu qua maxLen.
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

    static void backspace(VkState& s) { TelexHelper::popUtf8(s.query); }

    // Dieu huong D-pad. Tra true neu da xu ly (de caller return early).
    // upFromResults: khi focus dang o list ket qua, UP quay ve ban phim.
    static bool move(VkState& s, int dRow, int dCol) {
        if (s.inResults) {
            if (dRow < 0) { s.inResults = false; s.row = 4; s.col = 0; return true; }
            return false; // dieu huong trong results do caller xu ly
        }
        if (dRow != 0) {
            s.row += dRow;
            if (s.row < 0) s.row = 4;
            if (s.row > 4) s.row = 0;
            clampCol(s);
            return true;
        }
        if (dCol != 0) {
            if (s.row < 4) {
                s.col = (s.col + dCol + 10) % 10;
            } else {
                s.col = (s.col + dCol + 5) % 5;
            }
            return true;
        }
        return false;
    }

    // Nhan nut A (chon o hien tai). onToggleTelexNotice: callback hien toast
    // khi doi che do TELEX/US (truyen nullptr neu khong can).
    template <typename ToastFn>
    static VkAction pressA(VkState& s, ToastFn&& toast) {
        if (s.row < 4) {
            char ch = charAt(s);
            if (ch) typeChar(s, ch);
            return VkAction::None;
        }
        return pressAction(s.col, s, toast);
    }

    static VkAction pressA(VkState& s) {
        return pressA(s, [](const char*) {});
    }

    template <typename ToastFn>
    static VkAction pressAction(int idx, VkState& s, ToastFn&& toast) {
        switch (idx) {
            case 0: s.shift = !s.shift; return VkAction::None;
            case 1:
                s.telexMode = !s.telexMode;
                toast(s.telexMode ? "Che do: TELEX" : "Che do: TIENG ANH (US)");
                return VkAction::None;
            case 2: typeSpace(s); return VkAction::None;
            case 3: TelexHelper::popUtf8(s.query); return VkAction::None;
            case 4: return s.query.empty() ? VkAction::Cancel : VkAction::Commit;
            default: return VkAction::None;
        }
    }

    // Nut B: xoa dan neu con chu, het chu thi thoat (khop hanh vi YouTube cu).
    static VkAction pressB(VkState& s) {
        if (!s.query.empty()) { TelexHelper::popUtf8(s.query); return VkAction::Backspace; }
        return VkAction::Cancel;
    }

private:
    static void clampCol(VkState& s) {
        if (s.row < 4) { if (s.col > 9) s.col = 9; }
        else { if (s.col > 4) s.col = 4; }
        if (s.col < 0) s.col = 0;
    }
};

inline const char* VirtualKeyboard::lowerRows[4] = {
    "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?", "1234567890"
};
inline const char* VirtualKeyboard::upperRows[4] = {
    "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM;:/", "!@#$%^&*()"
};

} // namespace RomCloud
