#include "UIManager.h"
#include "../common/BackgroundTask.h"
#include "../flood/FloodManager.h"
#include "../input/InputManager.h"
#include "../logging/Logger.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace RomCloud {

void UIManager::pollFloodLive() {
    if (FloodManager::instance().liveFresh()) return;
    if (m_floodTask.isRunning() || m_floodFetching) return;
    m_floodFetching = true;
    m_floodTask.run([](TaskProgress &) { FloodManager::instance().refreshLive(); });
}

static SDL_Color floodLvlColor(int lvl) {
    if (lvl >= 2) return {239, 68, 68, 255};    // nặng: đỏ
    if (lvl == 1) return {250, 204, 21, 255};   // vừa: vàng
    return {148, 163, 184, 255};                // nhẹ: xám
}

static const char *floodLvlLabel(int lvl) {
    if (lvl >= 2) return "Nặng";
    if (lvl == 1) return "Vừa";
    return "Nhẹ";
}

void UIManager::drawFloodHeader(size_t vtotal) {
    // Hàng pill tab chung (đồng nhất với renderTraffic).
    int pillW0 = pillWidth("THỜI TIẾT", m_fontSmall) + 32;
    int pillW1 = pillWidth("ĐỒNG HỒ", m_fontSmall) + 32;
    int pillW2 = pillWidth("GIAO THÔNG", m_fontSmall) + 32;
    int pillW3 = pillWidth("GIÁ CẢ", m_fontSmall) + 32;
    int pillW4 = pillWidth("THỊ TRƯỜNG", m_fontSmall) + 32;
    int pillX4 = 1024 - 24 - pillW4;
    int pillX3 = pillX4 - 8 - pillW3;
    int pillX2 = pillX3 - 8 - pillW2;
    int pillX1 = pillX2 - 8 - pillW1;
    int pillX0 = pillX1 - 8 - pillW0;

    // Header: title + live summary NGANG hàng (pattern tab Thời tiết).
    drawRect(0, 0, UiTheme::APP_W, UiTheme::HEADER_H, UiTheme::FOOTER_BG, true);
    drawRect(0, UiTheme::HEADER_H - 1, UiTheme::APP_W, 1, UiTheme::FOOTER_LINE,
             true);
    {
        const std::string title = "BÁO NGẬP";
        drawText(title, 24, textYCentered(0, UiTheme::HEADER_H, m_fontLarge),
                 UiTheme::ACCENT_CYAN, m_fontLarge);
        const FloodLive& lv = FloodManager::instance().live();
        std::string sub;
        if (lv.ok) {
            char sb[128];
            std::snprintf(sb, sizeof(sb), "%zu điểm • %s", vtotal, lv.summary.c_str());
            sub = sb;
        } else if (m_floodFetching) {
            sub = "Đang tải dữ liệu live...";
        } else {
            char sb[64];
            std::snprintf(sb, sizeof(sb), "%zu điểm • chưa có live (cần mạng)", vtotal);
            sub = sb;
        }
        int tx = 24 + textWidth(title, m_fontLarge) + 16;
        drawText(truncateToWidth(sub, m_fontSmall, UiTheme::APP_W - tx - 300),
                 tx, textYCentered(0, UiTheme::HEADER_H, m_fontSmall),
                 UiTheme::TEXT_DIM, m_fontSmall, false);
    }
    drawHeaderStatus();
    drawPill(pillX0, 72, pillW0, 32, "THỜI TIẾT", false, m_fontSmall);
    drawPill(pillX1, 72, pillW1, 32, "ĐỒNG HỒ", false, m_fontSmall);
    drawPill(pillX2, 72, pillW2, 32, "GIAO THÔNG", true, m_fontSmall);
    drawPill(pillX3, 72, pillW3, 32, "GIÁ CẢ", false, m_fontSmall);
    drawPill(pillX4, 72, pillW4, 32, "THỊ TRƯỜNG", false, m_fontSmall);
}

void UIManager::renderFlood() {
    FloodManager::instance().load();
    if (m_floodVisDirty) rebuildFloodVis();
    // Live nền: vào màn là fetch nếu cache quá 2h (delay chấp nhận được).
    pollFloodLive();
    if (!m_floodTask.isRunning() && m_floodFetching) {
        m_floodFetching = false;
        m_floodVisDirty = true; // có risk mới -> sort lại
    }
    size_t vtotal = floodVisCount();
    drawFloodHeader(vtotal);

    // Không pill phụ: Báo ngập là 1 trang trong tab Giao thông,
    // D-pad Trái/Phải chuyển qua lại với Camera.
    const int listTop = 124;
    const int listH = 715 - listTop - 16;
    if (vtotal == 0) {
        if (!m_floodFilter.empty()) {
            drawText("Không tìm thấy. Bấm START xóa lọc.", 512, 400,
                     {170, 180, 195, 255}, m_fontSmall, true);
            drawAppFooter({{UiTheme::PadBtn::START, "Xóa lọc"},
                           {UiTheme::PadBtn::B, "Lùi"},
                           {UiTheme::PadBtn::L1R1, "Tab"}});
        } else {
            drawText("Chưa có dữ liệu điểm ngập.", 512, 400,
                     {170, 180, 195, 255}, m_fontSmall, true);
            drawAppFooter({{UiTheme::PadBtn::B, "Lùi"},
                           {UiTheme::PadBtn::L1R1, "Tab"}});
        }
        if (m_floodModalOpen) SearchInputModal::render(m_floodModalCfg);
        return;
    }
    if (m_floodSel >= vtotal) m_floodSel = vtotal - 1;

    // Card list full-width, hàng 48px.
    const int llx = 24, llw = 1024 - 48;
    drawRoundedRect(llx, listTop, llw, listH, UiTheme::RADIUS_CARD,
                    {22, 28, 38, 255}, true);
    const int rowH = 48;
    int visible = (listH - 8) / rowH;
    if (visible < 1) visible = 1;
    if ((int)m_floodSel < m_floodScroll) m_floodScroll = (int)m_floodSel;
    if ((int)m_floodSel >= m_floodScroll + visible)
        m_floodScroll = (int)m_floodSel - visible + 1;
    for (int i = 0; i < visible && m_floodScroll + i < (int)vtotal; ++i) {
        int pos = m_floodScroll + i;
        int idx = (int)floodVisToReal(pos);
        int y = listTop + 4 + i * rowH;
        bool sel = (pos == (int)m_floodSel);
        if (sel)
            drawHighlight(llx + 8, y + 3, llw - 16, rowH - 6);
        const auto &pt = FloodManager::instance().at(idx);
        // Badge phải: live ok -> tier nguy cơ, chưa có live -> mức lịch sử.
        int rk = FloodManager::instance().riskOf(idx);
        SDL_Color lc;
        const char *lbl;
        if (rk >= 0) {
            if (rk >= 2) { lc = {239, 68, 68, 255}; lbl = "Cao"; }
            else if (rk == 1) { lc = {250, 204, 21, 255}; lbl = "Chú ý"; }
            else { lc = {148, 163, 184, 255}; lbl = "Thấp"; }
        } else {
            lc = floodLvlColor(pt.lvl);
            lbl = floodLvlLabel(pt.lvl);
        }
        drawDot(llx + 28, y + rowH / 2, 5, lc);
        int nameX = llx + 48;
        int rightW = 0;
        std::string right;
        if (!pt.ward.empty()) {
            right = pt.ward;
            if (!pt.cause.empty()) right += " • " + pt.cause;
            rightW = textWidth(right, m_fontSmall) + 32;
        }
        // Rộng badge theo đúng nhãn đang vẽ (live "Chú ý" rộng hơn "Nhẹ"),
        // + gap để cột ward không giáp badge.
        int lvlW = textWidth(lbl, m_fontSmall) + 36;
        int nameW = llx + llw - 20 - nameX - rightW - lvlW;
        if (nameW < 60) nameW = 60;
        drawText(truncateToWidth(pt.name, m_fontMedium, nameW),
                 nameX, y + (rowH - 6 - textHeight(m_fontMedium)) / 2,
                 sel ? SDL_Color{255, 255, 255, 255} : UiTheme::TEXT_MAIN,
                 m_fontMedium, false);
        if (!right.empty())
            drawTextRight(right, llx + llw - 20 - lvlW,
                          y + (rowH - 6 - textHeight(m_fontSmall)) / 2,
                          UiTheme::TEXT_SUB, m_fontSmall);
        drawTextRight(lbl, llx + llw - 20,
                      y + (rowH - 6 - textHeight(m_fontSmall)) / 2, lc,
                      m_fontSmall);
    }

    drawAppFooter({{UiTheme::PadBtn::UPDOWN, "Chọn"},
                   {UiTheme::PadBtn::SELECT, "Tìm"},
                   {UiTheme::PadBtn::DPAD, "Camera"},
                   {UiTheme::PadBtn::L1R1, "Tab"},
                   {UiTheme::PadBtn::B, "Lùi"}});
    if (m_floodModalOpen) SearchInputModal::render(m_floodModalCfg);
}

bool UIManager::handleFloodInput() {
    InputManager& input = InputManager::instance();
    bool l1Tab = input.isButtonJustPressed(Button::L1);
    bool r1Tab = input.isButtonJustPressed(Button::R1);
    if (l1Tab || r1Tab) {
        m_wxTab = (m_wxTab + (l1Tab ? 4 : 1)) % 5;
        return true;
    }
    // Modal tìm kiếm mở thì route vào modal (cùng pattern camera).
    if (m_floodModalOpen) {
        bool a = input.isButtonJustPressed(Button::A);
        bool b = input.isButtonJustPressed(Button::B);
        bool s = input.isButtonJustPressed(Button::START);
        bool u = input.isButtonJustPressed(Button::UP);
        bool d = input.isButtonJustPressed(Button::DOWN);
        bool l = input.isButtonJustPressed(Button::LEFT);
        bool r = input.isButtonJustPressed(Button::RIGHT);
        bool x = input.isButtonJustPressed(Button::X);
        bool y = input.isButtonJustPressed(Button::Y);
        bool l1 = input.isButtonJustPressed(Button::L1);
        bool r1 = input.isButtonJustPressed(Button::R1);
        auto res = SearchInputModal::handleInput(m_floodModalCfg, a, b, s, u, d,
                                                 l, r, x, y, l1, r1);
        if (res == SearchInputModal::Result::Commit) {
            m_floodModalOpen = false;
            std::string q = m_floodVk.query;
            if (!q.empty()) {
                m_floodHist.push_back(q);
                applyFloodFilter(q);
                showToast("Tìm thấy " + std::to_string(floodVisCount()) + " điểm ngập",
                          {0, 180, 216, 255}, 2000);
            } else {
                applyFloodFilter("");
            }
        } else if (res == SearchInputModal::Result::Cancel) {
            m_floodModalOpen = false;
        }
        return true;
    }
    size_t vtotal = floodVisCount();
    if (vtotal == 0) {
        if (input.isButtonJustPressed(Button::B)) {
            if (!m_floodFilter.empty()) applyFloodFilter("");
            else m_camSub = 0;
        } else if (input.isButtonJustPressed(Button::LEFT) ||
                   input.isButtonJustPressed(Button::RIGHT)) {
            m_camSub = 0;
        } else if (input.isButtonJustPressed(Button::START)) {
            applyFloodFilter("");
        }
        return true;
    }
    // B về Camera (sub-view trước), Trái/Phải cũng về Camera.
    // Chống dội như chiều đi: nhả phím mới cho chuyển tiếp.
    if (input.isButtonJustPressed(Button::B)) {
        m_camSub = 0;
        m_camSubArmed = false;
        return true;
    }
    bool fLeft = input.isButtonJustPressed(Button::LEFT);
    bool fRight = input.isButtonJustPressed(Button::RIGHT);
    if (!input.isButtonPressed(Button::LEFT) &&
        !input.isButtonPressed(Button::RIGHT))
        m_camSubArmed = true;
    if (!m_floodModalOpen && m_camSubArmed && (fLeft || fRight)) {
        m_camSubArmed = false;
        m_camSub = 0;
        restartCamView();
        showToast("Camera giao thông", {0, 180, 216, 255}, 1200);
        return true;
    }
    if (input.isButtonJustPressed(Button::UP)) {
        m_floodSel = (m_floodSel + vtotal - 1) % vtotal;
    } else if (input.isButtonJustPressed(Button::DOWN)) {
        m_floodSel = (m_floodSel + 1) % vtotal;
    } else if (input.isButtonJustPressed(Button::SELECT)) {
        if (!m_floodModalInit) {
            m_floodModalCfg.ui = &m_ui;
            m_floodModalCfg.vk = &m_floodVk;
            m_floodModalCfg.history = &m_floodHist;
            m_floodModalCfg.fSmall = m_fontSmall;
            m_floodModalCfg.fMedium = m_fontMedium;
            m_floodModalCfg.fLarge = m_fontLarge;
            m_floodModalCfg.title = "TÌM ĐIỂM NGẬP THEO TÊN ĐƯỜNG";
            m_floodModalCfg.placeholder = "VD: Nguyen Van Qua...";
            m_floodModalInit = true;
        }
        VirtualKeyboard::reset(m_floodVk, true);
        m_floodModalOpen = true;
    } else if (input.isButtonJustPressed(Button::START)) {
        if (!m_floodFilter.empty()) {
            applyFloodFilter("");
            showToast("Đã xóa lọc", {148, 163, 184, 255}, 1500);
        }
    }
    return true;
}

size_t UIManager::floodVisCount() const {
    return m_floodVisIdx.size();
}

size_t UIManager::floodVisToReal(size_t pos) const {
    return pos < m_floodVisIdx.size() ? m_floodVisIdx[pos] : 0;
}

void UIManager::rebuildFloodVis() {
    m_floodVisDirty = false;
    m_floodVisIdx.clear();
    if (m_floodFilter.empty()) {
        size_t total = FloodManager::instance().count();
        for (size_t i = 0; i < total; ++i) m_floodVisIdx.push_back(i);
    } else {
        for (size_t i : m_floodFilter) m_floodVisIdx.push_back(i);
    }
    // Live ok: nguy cơ cao lên đầu (risk desc, rồi mức sử desc).
    if (FloodManager::instance().live().ok) {
        std::stable_sort(m_floodVisIdx.begin(), m_floodVisIdx.end(),
            [](size_t a, size_t b) {
                int ra = FloodManager::instance().riskOf(a);
                int rb = FloodManager::instance().riskOf(b);
                if (ra != rb) return ra > rb;
                return FloodManager::instance().at(a).lvl >
                       FloodManager::instance().at(b).lvl;
            });
    }
    if (!m_floodVisIdx.empty() && m_floodSel >= m_floodVisIdx.size()) {
        m_floodSel = 0;
        m_floodScroll = 0;
    }
}

static std::string floodLower(const std::string &s) {
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s) o += (char)tolower(c);
    return o;
}

void UIManager::applyFloodFilter(const std::string &query) {
    m_floodFilter.clear();
    m_floodSel = 0;
    m_floodScroll = 0;
    std::string q = floodLower(query);
    if (!q.empty()) {
        size_t total = FloodManager::instance().count();
        for (size_t i = 0; i < total; ++i) {
            const auto &pt = FloodManager::instance().at(i);
            std::string hay = floodLower(pt.name + " " + pt.ward);
            if (hay.find(q) != std::string::npos)
                m_floodFilter.push_back(i);
        }
    }
    rebuildFloodVis();
}

} // namespace RomCloud
