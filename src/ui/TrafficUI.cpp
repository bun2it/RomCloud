#include "UIManager.h"
#include "../camera/CameraManager.h"
#include "../input/InputManager.h"
#include "../logging/Logger.h"

#include <SDL2/SDL_image.h>
#include <sys/stat.h>
#include <cstdio>

namespace RomCloud {

void UIManager::renderTraffic() {
    // Sub-view Báo ngập (D-pad Trái/Phải chuyển qua lại với Camera).
    if (m_camSub == 1) {
        renderFlood();
        return;
    }
    CameraManager::instance().load();
    if (m_camVisDirty) rebuildCamVis();

    // Hàng pill chuyển tab: đồng nhất size 4 tab (pillWidth + 32).
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

    // Màn gộp: ảnh sát header + list địa điểm bên dưới
    size_t vtotal = camVisCount();
    if (vtotal == 0) {
        if (!m_camFilter.empty()) {
            drawAppBackground();
            drawAppHeader("GIAO THÔNG - CAMERA");
            drawPill(pillX0, 72, pillW0, 32, "THỜI TIẾT", false, m_fontSmall);
            drawPill(pillX1, 72, pillW1, 32, "ĐỒNG HỒ", false, m_fontSmall);
            drawPill(pillX2, 72, pillW2, 32, "GIAO THÔNG", true, m_fontSmall);
            drawPill(pillX3, 72, pillW3, 32, "GIÁ CẢ", false, m_fontSmall);
            drawPill(pillX4, 72, pillW4, 32, "THỊ TRƯỜNG", false, m_fontSmall);
            drawText("Không tìm thấy. Bấm X xóa lọc.", 512, 300,
                     {170, 180, 195, 255}, m_fontSmall, true);
            drawAppFooter({{UiTheme::PadBtn::X, "Xóa lọc"},
                           {UiTheme::PadBtn::B, "Lùi"},
                           {UiTheme::PadBtn::L1R1, "Tab"}});
        } else {
            drawAppBackground();
            drawAppHeader("GIAO THÔNG - CAMERA");
            drawPill(pillX0, 72, pillW0, 32, "THỜI TIẾT", false, m_fontSmall);
            drawPill(pillX1, 72, pillW1, 32, "ĐỒNG HỒ", false, m_fontSmall);
            drawPill(pillX2, 72, pillW2, 32, "GIAO THÔNG", true, m_fontSmall);
            drawPill(pillX3, 72, pillW3, 32, "GIÁ CẢ", false, m_fontSmall);
            drawPill(pillX4, 72, pillW4, 32, "THỊ TRƯỜNG", false, m_fontSmall);
            drawText("Chưa có danh sách camera.", 512, 300,
                     {170, 180, 195, 255}, m_fontSmall, true);
            drawAppFooter({{UiTheme::PadBtn::B, "Lùi"},
                           {UiTheme::PadBtn::L1R1, "Tab"}});
        }
        if (m_camModalOpen) SearchInputModal::render(m_camModalCfg);
        return;
    }
    if (m_camSel >= vtotal) m_camSel = vtotal - 1;
    size_t real = camVisToReal(m_camSel);
    const auto &cam = CameraManager::instance().at(real);
    // Title header theo chuẩn chung (drawAppHeader tự chừa cụm status),
    // pills nằm hàng dưới nên không giới hạn title.
    drawAppHeader("GIAO THÔNG - CAMERA");
    drawPill(pillX0, 72, pillW0, 32, "THỜI TIẾT", false, m_fontSmall);
    drawPill(pillX1, 72, pillW1, 32, "ĐỒNG HỒ", false, m_fontSmall);
    drawPill(pillX2, 72, pillW2, 32, "GIAO THÔNG", true, m_fontSmall);
    drawPill(pillX3, 72, pillW3, 32, "GIÁ CẢ", false, m_fontSmall);
    drawPill(pillX4, 72, pillW4, 32, "THỊ TRƯỜNG", false, m_fontSmall);

    // Ảnh full-width dưới hàng pill (cover-crop giữa)
    const int imgX = 0, imgW = 1024;
    const int imgY = 112, imgH = 420;

    std::string path = CameraManager::instance().snapPath(real);
    struct stat st;
    bool hasFile = (stat(path.c_str(), &st) == 0 && st.st_size > 2000);
    if (hasFile && (int64_t)st.st_mtime != m_camDecodedMtime) {
        // Decode lại khi file mới (main thread)
        if (m_camTex) {
            SDL_DestroyTexture(m_camTex);
            m_camTex = nullptr;
        }
        SDL_Surface *surf = IMG_Load(path.c_str());
        if (surf) {
            m_camTex = SDL_CreateTextureFromSurface(m_renderer, surf);
            SDL_FreeSurface(surf);
            if (m_camTex) m_camDecodedMtime = (int64_t)st.st_mtime;
        }
    }
    if (m_camTex) {
        int tw = 0, th = 0;
        SDL_QueryTexture(m_camTex, nullptr, nullptr, &tw, &th);
        if (tw > 0 && th > 0) {
            // Cover full-width: lấp đầy, cắt giữa phần thừa
            float sc = std::max((float)imgW / tw, (float)imgH / th);
            int sw = (int)(imgW / sc), sh = (int)(imgH / sc);
            SDL_Rect src = {(tw - sw) / 2, (th - sh) / 2, sw, sh};
            SDL_Rect dst = {imgX, imgY, imgW, imgH};
            SDL_RenderCopy(m_renderer, m_camTex, &src, &dst);
        }
    } else {
        drawText(m_camFetching ? "Đang tải ảnh..." : "Đang tải ảnh tự động...",
                 512, imgY + imgH / 2, {170, 180, 195, 255}, m_fontSmall, true);
    }

    // Tự refresh mỗi 10s (site update ~10-15s), trừ khi pause
    uint32_t nowMs = SDL_GetTicks();
    if (!m_camPaused && !m_camFetching && nowMs - m_camLastRefresh >= 10000) {
        m_camFetching = true;
        m_camTask.run([real](TaskProgress &) {
            CameraManager::instance().fetch(real);
        });
    }
    if (!m_camTask.isRunning() && m_camFetching) {
        m_camFetching = false;
        m_camLastRefresh = SDL_GetTicks();
    }

    // List địa điểm bên dưới, đúng 3 hàng (chọn là xem ngay, không cần A)
    {
        const int llx = 24, llw = 1024 - 48;
        const int listTop = imgY + imgH + 12;
        const int listH = 715 - listTop - 16;
        drawRoundedRect(llx, listTop, llw, listH, UiTheme::RADIUS_CARD,
                        {22, 28, 38, 255}, true);
        const int rowH = 48;
        int visible = 3;
        if (visible < 1) visible = 1;
        if ((int)m_camSel < m_camScroll) m_camScroll = (int)m_camSel;
        if ((int)m_camSel >= m_camScroll + visible)
            m_camScroll = (int)m_camSel - visible + 1;
        for (int i = 0; i < visible && m_camScroll + i < (int)vtotal; ++i) {
            int pos = m_camScroll + i;
            int idx = (int)camVisToReal(pos);
            int y = listTop + 4 + i * rowH;
            bool sel = (pos == (int)m_camSel);
            if (sel)
                drawHighlight(llx + 8, y + 3, llw - 16, rowH - 6);
            const auto &cc = CameraManager::instance().at(idx);
            char nbuf[8];
            snprintf(nbuf, sizeof(nbuf), "%d", idx + 1);
            drawText(nbuf, llx + 20, y + (rowH - 6 - textHeight(m_fontSmall)) / 2,
                     UiTheme::ACCENT_CYAN, m_fontSmall, false);
            int nameX = llx + 70;
            if (CameraManager::instance().isFavorite(CameraManager::instance().favKey(idx))) {
                drawIcon("favorite", llx + 62, y + (rowH - 6 - 24) / 2, 24, 24);
                nameX = llx + 92;
            }
            drawText(truncateToWidth(cc.name, m_fontMedium, llx + llw - 20 - nameX),
                     nameX, y + (rowH - 6 - textHeight(m_fontMedium)) / 2,
                     sel ? SDL_Color{255, 255, 255, 255} : UiTheme::TEXT_MAIN,
                     m_fontMedium, false);
            if (!cc.code.empty())
                drawTextRight(cc.code, llx + llw - 20,
                              y + (rowH - 6 - textHeight(m_fontSmall)) / 2,
                              UiTheme::TEXT_SUB, m_fontSmall);
        }
    }

    bool isYt = (CameraManager::instance().at(camVisToReal(m_camSel)).kind == 1);
    drawAppFooter({{UiTheme::PadBtn::A, isYt ? "Xem" : (m_camPaused ? "Tiếp tục" : "Dừng")},
                   {UiTheme::PadBtn::X, "Thích"},
                   {UiTheme::PadBtn::Y, m_camFavOnly ? "Tất cả" : "Yêu thích"},
                   {UiTheme::PadBtn::SELECT, "Tìm"},
                   {UiTheme::PadBtn::L1R1, "Tab"},
                   {UiTheme::PadBtn::B, "Lùi"}});
    if (m_camModalOpen) SearchInputModal::render(m_camModalCfg);
}

bool UIManager::handleTrafficInput() {
    InputManager& input = InputManager::instance();
    bool l1Tab = input.isButtonJustPressed(Button::L1);
    bool r1Tab = input.isButtonJustPressed(Button::R1);
    if (l1Tab || r1Tab) {
        // L1: lùi tab (sang trái), R1: tới tab (sang phải).
        m_wxTab = (m_wxTab + (l1Tab ? 4 : 1)) % 5;
        return true;
    }
    // Sub-view Báo ngập: input đi handler riêng (render đã rẽ ở renderTraffic).
    if (m_camSub == 1 && !m_camModalOpen) return handleFloodInput();
    // D-pad Trái/Phải: chuyển sang Báo ngập. Chống dội giữ phím/repeat:
    // chỉ chuyển khi đã nhả phím từ lần trước (m_camSubArmed).
    bool tLeft = input.isButtonJustPressed(Button::LEFT);
    bool tRight = input.isButtonJustPressed(Button::RIGHT);
    if (!input.isButtonPressed(Button::LEFT) &&
        !input.isButtonPressed(Button::RIGHT))
        m_camSubArmed = true;
    if (!m_camModalOpen && m_camSubArmed && (tLeft || tRight)) {
        m_camSubArmed = false;
        m_camSub = 1;
        m_floodVisDirty = true;
        showToast("Báo ngập TP.HCM", {0, 180, 216, 255}, 1200);
        return true;
    }
    // Modal tìm kiếm mở thì route vào modal
    if (m_camModalOpen) {
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
        auto res = SearchInputModal::handleInput(m_camModalCfg, a, b, s, u, d,
                                                 l, r, x, y, l1, r1);
        if (res == SearchInputModal::Result::Commit) {
            m_camModalOpen = false;
            std::string q = m_camVk.query;
            if (!q.empty()) {
                m_camHist.push_back(q);
                applyCamFilter(q);
                showToast("Tìm thấy " + std::to_string(camVisCount()) + " camera",
                          {0, 180, 216, 255}, 2000);
            } else {
                applyCamFilter("");
            }
        } else if (res == SearchInputModal::Result::Cancel) {
            m_camModalOpen = false;
        }
        return true;
    }
    size_t vtotal = camVisCount();
    if (vtotal == 0) {
        if (input.isButtonJustPressed(Button::B)) {
            if (!m_camFilter.empty()) {
                applyCamFilter("");
            } else {
                m_wxTab = 0;
            }
        } else if (input.isButtonJustPressed(Button::X)) {
            applyCamFilter("");
        }
        return true;
    }
    // Màn gộp: B về tab Thời tiết, A dừng/tiếp, UP/DOWN chuyển kênh xem ngay
    if (input.isButtonJustPressed(Button::B)) {
        if (m_camTex) {
            SDL_DestroyTexture(m_camTex);
            m_camTex = nullptr;
        }
        m_wxTab = 0;
        return true;
    }
    if (input.isButtonJustPressed(Button::A)) {
        // Kênh YouTube live: A resolve + phát (không block, xong tự play);
        // kênh ảnh: A dừng/tiếp refresh.
        size_t realA = camVisToReal(m_camSel);
        const auto &ca = CameraManager::instance().at(realA);
        if (ca.kind == 1 && !ca.vid.empty()) {
            playYouTubeVideo(ca.vid);
        } else {
            m_camPaused = !m_camPaused;
        }
    } else if (input.isButtonJustPressed(Button::UP)) {
        m_camSel = (m_camSel + vtotal - 1) % vtotal;
        restartCamView();
    } else if (input.isButtonJustPressed(Button::DOWN)) {
        m_camSel = (m_camSel + 1) % vtotal;
        restartCamView();
    } else if (input.isButtonJustPressed(Button::X)) {
        // Thích / bỏ thích kênh đang chọn (giống IPTV)
        size_t realX = camVisToReal(m_camSel);
        std::string key = CameraManager::instance().favKey(realX);
        bool wasFav = CameraManager::instance().isFavorite(key);
        CameraManager::instance().toggleFavorite(key);
        rebuildCamVis();
        showToast(wasFav ? "Đã xóa khỏi yêu thích" : "Đã thêm vào yêu thích",
                  wasFav ? SDL_Color{148, 163, 184, 255}
                         : SDL_Color{250, 204, 21, 255},
                  1500);
    } else if (input.isButtonJustPressed(Button::Y)) {
        // Lọc chỉ kênh yêu thích (giống IPTV)
        m_camFavOnly = !m_camFavOnly;
        rebuildCamVis();
        showToast(m_camFavOnly ? "Đang lọc: Kênh Yêu Thích" : "Đang lọc: Tất cả kênh",
                  {0, 180, 216, 255}, 1500);
    } else if (input.isButtonJustPressed(Button::SELECT)) {
        if (!m_camModalInit) {
            m_camModalCfg.ui = &m_ui;
            m_camModalCfg.vk = &m_camVk;
            m_camModalCfg.history = &m_camHist;
            m_camModalCfg.fSmall = m_fontSmall;
            m_camModalCfg.fMedium = m_fontMedium;
            m_camModalCfg.fLarge = m_fontLarge;
            m_camModalCfg.title = "TÌM CAMERA THEO TÊN ĐƯỜNG";
            m_camModalCfg.placeholder = "VD: Dien Bien Phu...";
            m_camModalInit = true;
        }
        VirtualKeyboard::reset(m_camVk, true);
        m_camModalOpen = true;
    } else if (input.isButtonJustPressed(Button::START)) {
        if (!m_camFilter.empty()) {
            applyCamFilter("");
            showToast("Đã xóa lọc", {148, 163, 184, 255}, 1500);
        }
    }
    return true;
}

void UIManager::restartCamView() {
    if (m_camTex) {
        SDL_DestroyTexture(m_camTex);
        m_camTex = nullptr;
    }
    m_camPaused = false;
    m_camFetching = false;
    m_camDecodedMtime = 0;
    m_camLastRefresh = 0;
}

size_t UIManager::camVisCount() const {
    return m_camVisIdx.size();
}

size_t UIManager::camVisToReal(size_t pos) const {
    return pos < m_camVisIdx.size() ? m_camVisIdx[pos] : 0;
}

void UIManager::rebuildCamVis() {
    m_camVisDirty = false;
    m_camVisIdx.clear();
    if (m_camFilter.empty()) {
        size_t total = CameraManager::instance().count();
        for (size_t i = 0; i < total; ++i) {
            if (m_camFavOnly &&
                !CameraManager::instance().isFavorite(
                    CameraManager::instance().favKey(i)))
                continue;
            m_camVisIdx.push_back(i);
        }
    } else {
        for (size_t i : m_camFilter) {
            if (m_camFavOnly &&
                !CameraManager::instance().isFavorite(
                    CameraManager::instance().favKey(i)))
                continue;
            m_camVisIdx.push_back(i);
        }
    }
    if (!m_camVisIdx.empty() && m_camSel >= m_camVisIdx.size()) {
        m_camSel = 0;
        m_camScroll = 0;
    }
}

static std::string camLower(const std::string &s) {
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s)
        o += (char)tolower(c);
    return o;
}

void UIManager::applyCamFilter(const std::string &query) {
    m_camFilter.clear();
    m_camSel = 0;
    m_camScroll = 0;
    std::string q = camLower(query);
    if (!q.empty()) {
        size_t total = CameraManager::instance().count();
        for (size_t i = 0; i < total; ++i) {
            const auto &cam = CameraManager::instance().at(i);
            std::string hay = camLower(cam.name + " " + cam.code);
            if (hay.find(q) != std::string::npos)
                m_camFilter.push_back(i);
        }
    }
    rebuildCamVis();
}

} // namespace RomCloud
