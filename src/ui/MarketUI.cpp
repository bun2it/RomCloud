#include "UIManager.h"
#include "../market/MarketManager.h"
#include "../weather/WeatherManager.h"
#include "../input/InputManager.h"

#include <ctime>
#include <cstdio>

namespace RomCloud {

void UIManager::startMarketFetch() {
    if (m_mkTask.isRunning()) return;
    m_mkFetching = true;
    WxPlace pl = WeatherManager::instance().place();
    std::string prov = pl.valid ? pl.province : "Hà Nội";
    m_mkTask.run([prov](TaskProgress&) {
        MarketManager::instance().fetch(prov);
    });
}

void UIManager::pollMarket() {
    if (m_currentState != UIState::WEATHER || m_wxTab != 3) return;
    if (m_mkTask.isRunning()) return;
    uint32_t now = SDL_GetTicks();
    if (m_mkLastMs == 0 || now - m_mkLastMs >= 600000) {
        m_mkLastMs = now; // live: vào tab tải ngay, sau đó 10 phút/lần
        startMarketFetch();
    }
}

void UIManager::renderMarket(int contentTop, int contentH) {
    if (!m_mkTask.isRunning() && m_mkFetching) m_mkFetching = false;
    MarketData d = MarketManager::instance().data();
    TTF_Font* fVal = m_fontClockSm ? m_fontClockSm : m_fontLarge;

    // 1 card 4 khối 2x2, vạch chia giữa.
    const int cx = 24, cw = 1024 - 48;
    drawRoundedRect(cx, contentTop, cw, contentH, UiTheme::RADIUS_CARD,
                    {22, 28, 38, 255}, true);
    const int cellW = cw / 2, cellH = contentH / 2;
    drawRect(cx + cellW - 1, contentTop + 16, 1, contentH - 32,
             {30, 41, 59, 255}, true);
    drawRect(cx + 16, contentTop + cellH - 1, cw - 32, 1,
             {30, 41, 59, 255}, true);

    bool anyOk = d.xang.ok || d.dau.ok || d.vang.ok || d.usd.ok;
    if (!anyOk && !m_mkFetching) {
        drawText("Chưa có dữ liệu. Bấm A tải lại.", 512,
                 contentTop + contentH / 2, {170, 180, 195, 255},
                 m_fontSmall, true);
    } else if (!anyOk) {
        drawText("Đang tải giá...", 512, contentTop + contentH / 2,
                 {245, 158, 11, 255}, m_fontSmall, true);
    } else {
        struct Cell {
            const char* label;
            const MarketQuote* q;
        };
        Cell cells[4] = {{"XĂNG RON 95", &d.xang},
                         {"DẦU DO", &d.dau},
                         {"VÀNG SJC", &d.vang},
                         {"ĐÔ LA (USD)", &d.usd}};
        for (int i = 0; i < 4; i++) {
            int ox = cx + (i % 2) * cellW;
            int oy = contentTop + (i / 2) * cellH;
            int ccx = ox + cellW / 2; // tâm ngang của ô
            const MarketQuote& q = *cells[i].q;
            const std::string label = cells[i].label;

            const int hLabel = textHeight(m_fontSmall);
            const int hVal = textHeight(fVal);
            const int hSub = textHeight(m_fontSmall);
            const int gap = 10;
            // Chiều cao khối text theo nội dung thật để căn giữa dọc ô
            int totalH = hLabel + gap + (q.ok ? hVal + gap + hSub : hSub);
            int ty = oy + (cellH - totalH) / 2;

            // Dòng 1: label (+ delta) căn giữa ngang
            if (!q.delta.empty()) {
                int labelW = textWidth(label, m_fontSmall);
                int rowW = labelW + 12 + textWidth(q.delta, m_fontSmall);
                int lx = ccx - rowW / 2;
                drawText(label, lx, ty, {0, 180, 216, 255}, m_fontSmall, false);
                drawText(q.delta, lx + labelW + 12, ty,
                         q.up ? SDL_Color{248, 113, 113, 255}
                              : SDL_Color{34, 197, 94, 255},
                         m_fontSmall, false);
            } else {
                drawText(label, ccx, ty, {0, 180, 216, 255}, m_fontSmall, true);
            }
            if (q.ok) {
                int vy = ty + hLabel + gap;
                drawText(truncateToWidth(q.value, fVal, cellW - 56), ccx, vy,
                         {255, 255, 255, 255}, fVal, true);
                std::string sub = q.unit;
                if (!q.sub.empty()) sub += " • " + q.sub;
                // (Không hiện q.updated: bỏ giờ/ngày theo yêu cầu.)
                drawText(truncateToWidth(sub, m_fontSmall, cellW - 56), ccx,
                         vy + hVal + gap, {148, 163, 184, 255}, m_fontSmall,
                         true);
            } else {
                drawText(m_mkFetching ? "Đang tải..." : "—", ccx,
                         ty + hLabel + gap, {100, 110, 125, 255}, m_fontSmall,
                         true);
            }
        }
    }

    drawAppFooter({{UiTheme::PadBtn::A, "Tải lại"},
                   {UiTheme::PadBtn::B, "Lùi"},
                   {UiTheme::PadBtn::L1R1, "Tab"}});
}

bool UIManager::handleMarketInput() {
    InputManager& input = InputManager::instance();
    if (input.isButtonJustPressed(Button::B)) {
        goBack();
        return true;
    }
    if (input.isButtonJustPressed(Button::START)) {
        setState(UIState::MENU);
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
    if (input.isButtonJustPressed(Button::A)) {
        if (!m_mkTask.isRunning()) {
            m_mkLastMs = SDL_GetTicks();
            startMarketFetch();
        }
        return true;
    }
    return true; // ▲▼◀▶ trong tab giá: không dùng, nuốt nút
}

} // namespace RomCloud
