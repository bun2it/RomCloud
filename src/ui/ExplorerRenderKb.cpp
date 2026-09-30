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
    const char* exActs[5] = {"Shift", "Telex", "Space", "Xoa", "Xong"};
    m_ui.drawVirtualKeyboard(vk, kbX, kbY, cellW, cellH, gx, gy,
        SDL_Color{37, 99, 235, 255}, SDL_Color{147, 197, 253, 255}, exActs, false, false, 1);
}

} // namespace RomCloud
