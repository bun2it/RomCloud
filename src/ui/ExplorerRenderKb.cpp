#include "UIManager.h"
#include "../filesystem/FileSystemManager.h"
#include "../config/AppConfig.h"
#include "../platform/PlatformInfo.h"

namespace RomCloud {

void UIManager::renderExplorerKeyboardFor(FileExplorer& ex) {
    drawAppBackground();
    VkState& vk = ex.keyboard();
    bool isCreate = ex.creatingFolder();

    // ─── Row 1: Header (Standard 64px) ───
    drawRect(0, 0, UiTheme::APP_W, UiTheme::HEADER_H, UiTheme::FOOTER_BG, true);
    drawRect(0, UiTheme::HEADER_H - 1, UiTheme::APP_W, 1, UiTheme::FOOTER_LINE, true);
    drawGridIcon("FOLDER.png", 24, 12, 40, 40);
    drawText(isCreate ? "TẠO THƯ MỤC MỚI" : "ĐỔI TÊN", 76,
             textYCentered(0, UiTheme::HEADER_H, m_fontLarge),
             UiTheme::ACCENT_CYAN, m_fontLarge, false);
    drawHeaderStatus();

    // ─── Row 2: Input Field Box (Y = 74, H = 58) ───
    int inX = 46;
    int inY = 74;
    int inW = 932;
    int inH = 58;

    std::string bgInputPath =
        AppConfig::instance().getAssetsDir() + "/stock_keyboard/bg-search-input.png";
    SDL_Texture* bgInputTex =
        m_ui.getOrLoadImage("stock_keyboard/bg-search-input.png", bgInputPath);
    if (bgInputTex) {
        int slx = PlatformInfo::instance().scaleX(inX);
        int sly = PlatformInfo::instance().scaleY(inY);
        int slw = PlatformInfo::instance().scaleW(inW);
        int slh = PlatformInfo::instance().scaleH(inH);
        SDL_Rect dst = {slx, sly, slw, slh};
        SDL_RenderCopy(m_renderer, bgInputTex, nullptr, &dst);
    } else {
        drawRoundedRect(inX, inY, inW, inH, UiTheme::RADIUS_CARD, {12, 38, 50, 230}, true);
        drawRoundedBorder(inX, inY, inW, inH, UiTheme::RADIUS_CARD, {26, 72, 92, 255}, 1);
    }

    std::string dispQ = vk.query.empty()
        ? (isCreate ? "Nhập tên thư mục mới..." : "Nhập tên mới...")
        : (vk.query + " _");
    dispQ = truncateToWidth(dispQ, m_fontLarge, inW - 48);
    SDL_Color qCol = vk.query.empty() ? SDL_Color{75, 115, 135, 255}
                                      : SDL_Color{255, 255, 255, 255};
    drawText(dispQ, inX + 24, inY + (inH - textHeight(m_fontLarge)) / 2, qCol,
             m_fontLarge, false);

    // ─── Row 3: Current Directory Location (Y = 150) ───
    drawText("VỊ TRÍ THƯ MỤC", inX, 150, UiTheme::TEXT_SUB, m_fontSmall, false);
    std::string pathShown = truncateToWidth(ex.currentPath(), m_fontMedium, inW);
    drawText(pathShown, inX, 186, UiTheme::TEXT_MAIN, m_fontMedium, false);

    // Divider line above keyboard
    drawRect(46, 444, 932, 1, {20, 48, 64, 180}, true);

    // ─── Bottom: Compact Virtual Keyboard (Y = 452..706) ───
    static std::string s0, s1, s2, s3, s4;
    s0 = vk.shift ? "ABC" : "abc";
    s1 = vk.telexMode ? "TELEX" : "US";
    s2 = "Cách";
    s3 = "Xóa";
    s4 = "Xong";
    const char* exActs[5] = {s0.c_str(), s1.c_str(), s2.c_str(), s3.c_str(), s4.c_str()};
    m_ui.drawVirtualKeyboard(vk, 46, 452, 86, 46, 8, 6,
                             SDL_Color{0, 140, 230, 255},
                             SDL_Color{0, 180, 255, 255},
                             exActs, true, true, 1);

    // ─── Bottom Bar: drawAppFooter tự vẽ nền (B = Hủy) ───
    drawAppFooter({{UiTheme::PadBtn::A, "Nhập"},
                   {UiTheme::PadBtn::X, "Cách"},
                   {UiTheme::PadBtn::Y, "Xóa"},
                   {UiTheme::PadBtn::L1, "Hoa"},
                   {UiTheme::PadBtn::R1, "Telex"},
                   {UiTheme::PadBtn::START, "Xong"},
                   {UiTheme::PadBtn::B, "Hủy"}});
}

void UIManager::renderExplorerKeyboard() {
    renderExplorerKeyboardFor(expA());
}

} // namespace RomCloud
