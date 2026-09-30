#include "UIManager.h"
#include "../filesystem/FileSystemManager.h"

namespace RomCloud {

void UIManager::renderFileExplorer() {
    if (m_explorer.creatingFolder() || m_explorer.renaming()) { renderExplorerKeyboard(); return; }
    drawAppBackground();
    FileListView::Layout lo = FileListView::defaultLayout();
    drawRect(0, 0, 1024, 112, {15, 23, 42, 255}, true);
    drawRect(0, 112, 1024, 1, {30, 41, 59, 255}, true);
    drawGridIcon("FOLDER.png", 28, 24, 56, 56);
    drawText("FILE EXPLORER", 96, 24, {255, 255, 255, 255}, m_fontLarge, false);
    std::string p = m_explorer.currentPath();
    const std::string root = "/mnt/SDCARD";
    if (p.compare(0, root.size(), root) == 0) p = p.substr(root.size());
    if (p.empty()) p = "/";
    if ((int)p.size() > 48) p = ".." + p.substr(p.size() - 46);
    drawText(p, 96, 68, {148, 163, 184, 255}, m_fontSmall, false);
    if (m_explorer.clipboard().active()) {
        std::string b = m_explorer.clipboard().op == ExplorerClipboard::Op::CUT ? "CUT" : "COPY";
        drawText(b, 996, 24, {245, 158, 11, 255}, m_fontSmall, false);
    }
    const auto& entries = m_explorer.entries();
    int total = (int)entries.size();
    int visibleRows = FileListView::calcVisibleRows(lo.leftH - 24, lo.rowH);
    m_explorerScroll = FileListView::calcScroll(m_explorer.selected(), visibleRows, m_explorerScroll);
    drawRect(lo.leftX, lo.leftY, lo.leftW, lo.leftH, {15, 23, 42, 255}, true);
    int listX = lo.leftX + 12, listY = lo.leftY + 12;
    if (total == 0) {
        drawText("(Thu muc trong)", lo.leftX + lo.leftW / 2, listY + lo.leftH / 2 - 10,
                 {100, 116, 139, 255}, m_fontMedium, true);
    } else {
        for (int i = 0; i < visibleRows && m_explorerScroll + i < total; ++i) {
            int idx = m_explorerScroll + i;
            int y = listY + i * lo.rowH;
            bool sel = (idx == m_explorer.selected());
            if (sel) {
                drawRect(listX, y, lo.leftW - 24, lo.rowH - 6, {30, 58, 138, 255}, true);
                drawRect(listX, y, 4, lo.rowH - 6, {59, 130, 246, 255}, true);
            }
            const auto& e = entries[(size_t)idx];
            drawGridIcon(e.isDir ? "FOLDER.png" : "GAMES.png", listX + 10, y + 8, 36, 36);
            drawText(FileListView::shorten(e.name, 26), listX + 56, y + 12,
                     sel ? SDL_Color{255, 255, 255, 255} : SDL_Color{203, 213, 225, 255},
                     m_fontMedium, false);
            std::string badge;
            if (m_explorer.clipboard().active() && e.path == m_explorer.clipboard().srcPath)
                badge = (m_explorer.clipboard().op == ExplorerClipboard::Op::CUT ? "CUT" : "COPY");
            std::string sub = badge.empty()
                ? (e.isDir ? "<DIR>" : FileSystemManager::instance().formatBytes(e.sizeBytes))
                : badge;
            drawText(sub, listX + 56, y + 34, {148, 163, 184, 255}, m_fontSmall, false);
        }
    }
    drawRect(lo.rightX, lo.rightY, lo.rightW, lo.rightH, {15, 23, 42, 255}, true);
    const ExplorerEntry* cur = m_explorer.current();
    std::string nm = cur ? cur->name : "-";
    if ((int)nm.size() > 18) nm = nm.substr(0, 17) + "..";
    drawText(nm, lo.rightX + 16, lo.rightY + 16, {255, 255, 255, 255}, m_fontLarge, false);
    drawText(cur ? (cur->isDir ? "Thu muc" : "File") : "", lo.rightX + 16, lo.rightY + 56,
             {148, 163, 184, 255}, m_fontSmall, false);
    if (cur && !cur->isDir)
        drawText(FileSystemManager::instance().formatBytes(cur->sizeBytes),
                 lo.rightX + 16, lo.rightY + 84, {148, 163, 184, 255}, m_fontSmall, false);
    else
        drawText(std::to_string(total) + " muc", lo.rightX + 16, lo.rightY + 84,
                 {148, 163, 184, 255}, m_fontSmall, false);
    drawRect(lo.rightX + 16, lo.rightY + 124, lo.rightW - 32, 170, {29, 78, 216, 255}, true);
    drawText("L1: thu muc moi", lo.rightX + lo.rightW / 2, lo.rightY + 136,
             {255, 255, 255, 255}, m_fontSmall, true);
    drawText("START: doi ten", lo.rightX + lo.rightW / 2, lo.rightY + 158,
             {255, 255, 255, 255}, m_fontSmall, true);
    drawText("LEFT: xoa", lo.rightX + lo.rightW / 2, lo.rightY + 180,
             {255, 255, 255, 255}, m_fontSmall, true);
    drawText("R1: paste", lo.rightX + lo.rightW / 2, lo.rightY + 202,
             {219, 234, 254, 255}, m_fontSmall, true);
    drawText("SELECT: huy clip", lo.rightX + lo.rightW / 2, lo.rightY + 224,
             {219, 234, 254, 255}, m_fontSmall, true);
}

} // namespace RomCloud
