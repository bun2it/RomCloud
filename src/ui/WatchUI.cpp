#include "UIManager.h"
#include "../market/WatchManager.h"
#include "../input/InputManager.h"
#include "../platform/PlatformInfo.h"

#include <cstdio>

namespace RomCloud {

void UIManager::startWatchFetch() {
    if (m_watchTask.isRunning()) return;
    m_watchFetching = true;
    m_watchTask.run([](TaskProgress&) { WatchManager::instance().fetchAll(); });
}

void UIManager::pollWatch() {
    if (m_currentState != UIState::WEATHER || m_wxTab != 4) return;
    if (m_watchTask.isRunning()) return;
    uint32_t now = SDL_GetTicks();
    if (m_watchLastMs == 0 || now - m_watchLastMs >= 600000) {
        m_watchLastMs = now; // vào tab tải ngay, sau đó 10 phút/lần
        startWatchFetch();
    }
}

void UIManager::drawSparkline(const std::vector<float>& pts, int x, int y,
                              int w, int h, SDL_Color color) {
    if (pts.size() < 2 || w <= 10 || h <= 10 || !m_renderer) return;
    float mn = pts[0], mx = pts[0];
    for (float v : pts) {
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    if (mx - mn < 1e-6f) {
        mn -= 1.0f;
        mx += 1.0f;
    }
    size_t n = pts.size();
    size_t stride = (n - 1) / (size_t)w + 1;
    size_t count = (n + stride - 1) / stride;
    if (count < 2) return;
    std::vector<SDL_Point> sp;
    sp.reserve(count);
    size_t idx = 0;
    for (size_t i = 0; i < n && idx < count; i += stride, idx++) {
        float t = (pts[i] - mn) / (mx - mn); // 0 đáy .. 1 đỉnh
        int px = PlatformInfo::instance().scaleX(x) +
                 (int)((w * idx) / (count - 1));
        int py = PlatformInfo::instance().scaleY(y) +
                 (int)((1.0f - t) * (h - 1));
        sp.push_back({px, py});
    }
    SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
    SDL_RenderDrawLines(m_renderer, sp.data(), (int)sp.size());
}

void UIManager::renderWatch(int contentTop, int contentH) {
    if (!m_watchTask.isRunning() && m_watchFetching) m_watchFetching = false;
    auto items = WatchManager::instance().items();
    int n = (int)items.size();
    // Slot chọn được: các card + 1 ô "thêm mới" khi chưa đủ 4.
    int slots = n < WatchManager::kMaxCards ? n + 1 : n;
    if (slots < 1) slots = 1;
    if (m_watchSel >= slots) m_watchSel = slots - 1;
    if (m_watchSel < 0) m_watchSel = 0;
    TTF_Font* fVal = m_fontClockSm ? m_fontClockSm : m_fontLarge;

    // Lưới 2x2 cố định (tối đa 4 card), ô trống gợi ý thêm mã.
    const int cx = 24, cw = 1024 - 48;
    drawRoundedRect(cx, contentTop, cw, contentH, UiTheme::RADIUS_CARD,
                    {22, 28, 38, 255}, true);
    const int cellW = cw / 2, cellH = contentH / 2;
    drawRect(cx + cellW - 1, contentTop + 16, 1, contentH - 32,
             {30, 41, 59, 255}, true);
    drawRect(cx + 16, contentTop + cellH - 1, cw - 32, 1,
             {30, 41, 59, 255}, true);

    for (int i = 0; i < 4; i++) {
        int ox = cx + (i % 2) * cellW;
        int oy = contentTop + (i / 2) * cellH;
        int ccx = ox + cellW / 2;
        if (i >= n) {
            if (i == n && n < WatchManager::kMaxCards) {
                if (i == m_watchSel)
                    drawHighlight(ox + 8, oy + 6, cellW - 16, cellH - 12);
                drawText("A: Thêm mã", ccx, oy + cellH / 2 - 12,
                         i == m_watchSel ? SDL_Color{255, 255, 255, 255}
                                         : SDL_Color{100, 110, 125, 255},
                         m_fontSmall, true);
            }
            continue;
        }
        if (i == m_watchSel)
            drawHighlight(ox + 8, oy + 6, cellW - 16, cellH - 12);
        const WatchItem& it = items[i];
        SDL_Color upCol = it.up ? SDL_Color{34, 197, 94, 255}
                                : SDL_Color{248, 113, 113, 255};
        // Dòng 1: SYMBOL (+ delta %).
        int ty = oy + 14;
        if (it.ok && !it.delta.empty()) {
            int labelW = textWidth(it.symbol, m_fontSmall);
            int rowW = labelW + 12 + textWidth(it.delta, m_fontSmall);
            int lx = ccx - rowW / 2;
            drawText(it.symbol, lx, ty, {0, 180, 216, 255}, m_fontSmall,
                     false);
            drawText(it.delta, lx + labelW + 12, ty, upCol, m_fontSmall,
                     false);
        } else {
            drawText(it.symbol, ccx, ty, {0, 180, 216, 255}, m_fontSmall,
                     true);
        }
        // Giá lớn + đơn vị.
        int vy = ty + textHeight(m_fontSmall) + 8;
        if (it.ok) {
            drawText(truncateToWidth(it.price, fVal, cellW - 56), ccx, vy,
                     {255, 255, 255, 255}, fVal, true);
            std::string sub = it.label + " • " + it.unit;
            drawText(truncateToWidth(sub, m_fontSmall, cellW - 56), ccx,
                     vy + textHeight(fVal) + 8, {148, 163, 184, 255},
                     m_fontSmall, false);
        } else {
            drawText(m_watchFetching ? "Đang tải..." : "Chưa có dữ liệu", ccx,
                     vy, {100, 110, 125, 255}, m_fontSmall, true);
        }
        // Biểu đồ sparkline đáy card.
        if (it.ok && it.hist.size() >= 2) {
            const int chH = 96;
            int chy = oy + cellH - chH - 12;
            drawSparkline(it.hist, ox + 28, chy, cellW - 56, chH, upCol);
        }
    }

    drawAppFooter({{UiTheme::PadBtn::A, "Nhập mã"},
                   {UiTheme::PadBtn::X, "Xóa"},
                   {UiTheme::PadBtn::START, "Tải lại"},
                   {UiTheme::PadBtn::B, "Lùi"},
                   {UiTheme::PadBtn::L1R1, "Tab"}});
    if (m_watchModalOpen) SearchInputModal::render(m_watchModalCfg);
}

bool UIManager::handleWatchInput() {
    InputManager& input = InputManager::instance();
    // Modal nhập mã mở thì route vào modal.
    if (m_watchModalOpen) {
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
        auto res = SearchInputModal::handleInput(m_watchModalCfg, a, b, s, u,
                                                 d, l, r, x, y, l1, r1);
        if (res == SearchInputModal::Result::Commit) {
            std::string q = m_watchVk.query;
            std::string sym;
            for (char c : q) {
                if (c >= 'a' && c <= 'z') sym += (char)(c - 32);
                else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                    sym += c;
            }
            if (sym.empty()) {
                m_watchModalOpen = false; // trống = hủy
            } else {
                auto v = WatchManager::instance().symbols();
                bool adding =
                    (m_watchSel < 0 || m_watchSel >= (int)v.size());
                bool dup = false;
                for (int i = 0; i < (int)v.size(); i++)
                    if (v[i] == sym && (adding || i != m_watchSel)) dup = true;
                if (dup) {
                    // Giữ modal để sửa lại mã khác.
                    showToast("Đã có mã này", {245, 158, 11, 255}, 2000);
                } else {
                    m_watchModalOpen = false;
                    if (adding) {
                        v.push_back(sym);
                        m_watchSel = (int)v.size() - 1;
                    } else {
                        v[m_watchSel] = sym;
                    }
                    WatchManager::instance().setSymbols(v);
                    m_watchLastMs = SDL_GetTicks();
                    startWatchFetch(); // nhập mã -> load ngay
                    showToast(adding ? ("Đã thêm " + sym)
                                     : ("Đã đổi thành " + sym),
                              {34, 197, 94, 255}, 2000);
                }
            }
        } else if (res == SearchInputModal::Result::Cancel) {
            m_watchModalOpen = false;
        }
        return true;
    }
    if (input.isButtonJustPressed(Button::B)) {
        goBack();
        return true;
    }
    if (input.isButtonJustPressed(Button::START)) {
        // START: tải lại toàn bộ mã đang theo dõi.
        if (!m_watchTask.isRunning()) {
            m_watchLastMs = SDL_GetTicks();
            startWatchFetch();
            showToast("Đang tải lại...", {0, 180, 216, 255}, 1500);
        }
        return true;
    }
    bool l1Tab = input.isButtonJustPressed(Button::L1);
    bool r1Tab = input.isButtonJustPressed(Button::R1);
    if (l1Tab || r1Tab) {
        // L1: lùi tab (sang trái), R1: tới tab (sang phải).
        m_wxTab = (m_wxTab + (l1Tab ? 4 : 1)) % 5;
        if (m_wxTab == 2) {
            m_camView = true;
            m_camVisDirty = true;
            restartCamView();
        }
        return true;
    }
    auto items = WatchManager::instance().items();
    int n = (int)items.size();
    int slots = n < WatchManager::kMaxCards ? n + 1 : n;
    if (slots < 1) slots = 1;
    if (input.isButtonJustPressed(Button::LEFT)) {
        m_watchSel = (m_watchSel + slots - 1) % slots;
        return true;
    }
    if (input.isButtonJustPressed(Button::RIGHT)) {
        m_watchSel = (m_watchSel + 1) % slots;
        return true;
    }
    if (input.isButtonJustPressed(Button::UP)) {
        if (m_watchSel >= 2) m_watchSel -= 2;
        return true;
    }
    if (input.isButtonJustPressed(Button::DOWN)) {
        if (m_watchSel + 2 < slots) m_watchSel += 2;
        return true;
    }
    if (input.isButtonJustPressed(Button::Y) ||
        input.isButtonJustPressed(Button::A)) {
        // A/Y: nhập mã cho ô highlight (ô trống = thêm, có mã = đổi).
        openWatchModal();
        return true;
    }
    if (input.isButtonJustPressed(Button::X) && m_watchSel >= 0 &&
        m_watchSel < n) {
        std::string gone = items[m_watchSel].symbol;
        auto v = WatchManager::instance().symbols();
        v.erase(v.begin() + m_watchSel);
        WatchManager::instance().setSymbols(v);
        int left = (int)v.size();
        int sl = left < WatchManager::kMaxCards ? left + 1 : left;
        if (sl < 1) sl = 1;
        if (m_watchSel >= sl) m_watchSel = sl - 1;
        showToast("Đã xóa " + gone, {148, 163, 184, 255}, 1500);
        return true;
    }
    return true; // nuốt nút còn lại
}

void UIManager::openWatchModal() {
    if (!m_watchModalInit) {
        m_watchModalCfg.ui = &m_ui;
        m_watchModalCfg.vk = &m_watchVk;
        // Không dùng hàng lịch sử: UP từ bàn phím kẹt ở đó (SearchInputModal
        // chỉ về pill khi history non-empty). Nhập mã mới hoàn toàn.
        m_watchModalCfg.history = nullptr;
        m_watchModalCfg.fSmall = m_fontSmall;
        m_watchModalCfg.fMedium = m_fontMedium;
        m_watchModalCfg.fLarge = m_fontLarge;
        m_watchModalCfg.title = "NHẬP MÃ THEO DÕI";
        m_watchModalCfg.placeholder = "VD: BTC, SOL, VCB, HPG...";
        m_watchModalInit = true;
    }
    VirtualKeyboard::reset(m_watchVk, false);
    // Mở là trống để nhập mã mới (không điền mã cũ gây nhầm).
    m_watchModalOpen = true;
}

} // namespace RomCloud
