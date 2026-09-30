#pragma once
// RomCloud FileListView — list 2 cot Master-Detail + footer tay cam dung chung.
// Header-only, khong giu state: caller truyen entries + selected + detail lines.
// Dung cho FileExplorer man hinh moi va LocalSend picker (dan thay renderLocalSendFolderPicker).
#include <string>
#include <vector>

#include "UiTheme.h"

namespace RomCloud {

struct FileListRow {
    std::string title;
    std::string sub;
    bool isDir = false;
    std::string badge; // "CUT"/"COPY"/"" — hien tren row khi clipboard active
};

class FileListView {
public:
    // Tinh scroll top de selected luon visible.
    static int calcScroll(int selected, int visibleRows, int prevScroll) {
        if (visibleRows <= 0) return 0;
        int top = prevScroll;
        if (selected < top) top = selected;
        if (selected >= top + visibleRows) top = selected - visibleRows + 1;
        if (top < 0) top = 0;
        return top;
    }
    static int calcVisibleRows(int listH, int rowH) {
        if (rowH <= 0) return 1;
        int n = listH / rowH;
        return n > 0 ? n : 1;
    }
    // Cat ngan tieu de theo so ky tu (fallback khi khong co font do text width).
    static std::string shorten(const std::string& s, size_t maxChars = 26) {
        if (s.size() <= maxChars) return s;
        if (maxChars <= 2) return "..";
        return s.substr(0, maxChars - 2) + "..";
    }
    // Layout A Master-Detail 65/35 tren 1024 (theo UiTheme tokens).
    struct Layout {
        int leftX = 24, leftY = 128, leftW = 624, leftH = 574;
        int rightX = 664, rightW = 336, rightY = 128, rightH = 574;
        int rowH = 56;
    };
    static Layout defaultLayout() { return Layout{}; }
};

} // namespace RomCloud
