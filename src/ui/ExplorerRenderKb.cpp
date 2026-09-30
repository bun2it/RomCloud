#include "UIManager.h"
#include "../filesystem/FileSystemManager.h"

namespace RomCloud {

void UIManager::renderExplorerKeyboard() {
    drawAppBackground();
    VkState& vk = m_explorer.keyboard();
    bool isCreate = m_explorer.creatingFolder();
    drawRect(0, 0, 1024, 100, {15, 23, 42, 255}, true);
    drawGridIcon("FOLDER.png", 32, 22, 52, 52);
    drawText(isCreate ? "THU MUC MOI" : "DOI TEN", 96, 24,
             {255, 255, 255, 255}, m_fontLarge, false);
    drawText(m_explorer.currentPath(), 96, 66, {148, 163, 184, 255}, m_fontSmall, false);
    drawRect(48, 130, 928, 64, {15, 23, 42, 255}, true);
    drawBorder(48, 130, 928, 64, {59, 130, 246, 255}, 2);
    std::string shown = vk.query.empty() ? "Nhap ten..." : vk.query;
    drawText(shown, 68, 146, vk.query.empty() ? SDL_Color{100, 116, 139, 255}
                                              : SDL_Color{255, 255, 255, 255},
             m_fontLarge, false);
    int kbX = 48, kbY = 220, gx = 8, gy = 10;
    int cellW = (1024 - 96 - gx * 9) / 10, cellH = 64;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 10; c++) {
            int cx = kbX + c * (cellW + gx);
            int cy = kbY + r * (cellH + gy);
            bool sel = (vk.row == r && vk.col == c);
            VkState t = vk; t.row = r; t.col = c;
            char ch = VirtualKeyboard::charAt(t);
            char buf[2] = {ch, '\0'};
            drawRect(cx, cy, cellW, cellH, sel ? SDL_Color{37, 99, 235, 255}
                                               : SDL_Color{15, 23, 42, 255}, true);
            drawBorder(cx, cy, cellW, cellH, sel ? SDL_Color{147, 197, 253, 255}
                                                 : SDL_Color{30, 41, 59, 255}, sel ? 2 : 1);
            drawText(std::string(buf), cx + cellW / 2, cy + 14,
                     sel ? SDL_Color{255, 255, 255, 255} : SDL_Color{203, 213, 225, 255},
                     m_fontLarge, true);
        }
    }
    const char* acts[5] = {"Shift", "Telex", "Space", "Xoa", "Xong"};
    int actW = (1024 - 96 - gx * 4) / 5;
    int actY = kbY + 4 * (cellH + gy);
    for (int i = 0; i < 5; i++) {
        int cx = kbX + i * (actW + gx);
        bool sel = (vk.row == 4 && vk.col == i);
        SDL_Color bg = sel ? SDL_Color{37, 99, 235, 255} : SDL_Color{15, 23, 42, 255};
        if (i == 4) bg = sel ? SDL_Color{22, 163, 74, 255} : SDL_Color{20, 83, 45, 255};
        if (i == 0 && vk.shift) bg = SDL_Color{29, 78, 216, 255};
        drawRect(cx, actY, actW, cellH, bg, true);
        drawBorder(cx, actY, actW, cellH, sel ? SDL_Color{147, 197, 253, 255}
                                              : SDL_Color{30, 41, 59, 255}, sel ? 2 : 1);
        drawText(acts[i], cx + actW / 2, actY + 16, {255, 255, 255, 255}, m_fontMedium, true);
    }
}

} // namespace RomCloud
