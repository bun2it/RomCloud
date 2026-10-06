#include "UIManager.h"
#include "../browser/PortalBrowser.h"
#include "../input/InputManager.h"
#include "../logging/Logger.h"
#include "../wifi/WifiManager.h"
#include "../config/AppConfig.h"

#include <thread>

namespace RomCloud {

static const int kPortalW = 1024;
static const int kPortalH = 540;
static const int kPageY = 112;

void UIManager::openPortal(const std::string& url) {
    closePortal();
    m_portalShowBody = false;
    m_portalBody.clear();
    std::string appRoot = AppConfig::instance().getAppRoot();
    if (appRoot.empty()) appRoot = "/mnt/SDCARD/Apps/RomCloud";
    if (!PortalBrowser::instance().init(appRoot + "/ul/resources/",
                                        appRoot + "/assets/fonts/NotoSans-Regular.ttf",
                                        kPortalW, kPortalH)) {
        showToast("Không khởi động được browser", {239, 68, 68, 255}, 2500);
        return;
    }
    m_portalPendingUrl = url;
    m_portalLoading = true;
    setState(UIState::PORTAL);
    std::string u = url;
    m_portalTask.run([u](TaskProgress &) {
        std::string body, finalUrl;
        if (PortalBrowser::instance().fetchUrl(u, body, finalUrl)) {
            UIManager::instance().m_portalBody = body;
            UIManager::instance().m_portalFinal = finalUrl;
            UIManager::instance().m_portalShowBody = true;
        } else {
            UIManager::instance().m_portalLoading = false;
            UIManager::instance().showToast("Không tải được trang portal",
                                            {239, 68, 68, 255}, 2500);
        }
    });
}

void UIManager::closePortal() {
    if (m_portalTex) {
        SDL_DestroyTexture(m_portalTex);
        m_portalTex = nullptr;
    }
    if (PortalBrowser::instance().ready())
        PortalBrowser::instance().shutdown();
    m_portalLoading = false;
    m_portalShowBody = false;
    m_portalBody.clear();
    m_portalPendingUrl.clear();
}

void UIManager::pollPortalBridge() {
    // Nav do JS (location=...) -> fetch tiếp.
    std::string nav = PortalBrowser::instance().pollNav();
    if (!nav.empty() && !m_portalLoading) {
        m_portalPendingUrl = nav;
        m_portalLoading = true;
        std::string u = nav;
        m_portalTask.run([u](TaskProgress &) {
            std::string body, finalUrl;
            if (PortalBrowser::instance().fetchUrl(u, body, finalUrl)) {
                UIManager::instance().m_portalBody = body;
                UIManager::instance().m_portalFinal = finalUrl;
                UIManager::instance().m_portalShowBody = true;
            } else {
                UIManager::instance().m_portalLoading = false;
            }
        });
        return;
    }
    // Click/form qua console bridge.
    auto msgs = PortalBrowser::instance().drainConsole();
    for (auto& m : msgs) {
        if (m_portalLoading) break;
        const std::string& t = m.text;
        if (t.rfind("PNAV:", 0) == 0) {
            std::string u = t.substr(5);
            if (!u.empty() && u != PortalBrowser::instance().currentUrl()) {
                m_portalPendingUrl = u;
                m_portalLoading = true;
                m_portalTask.run([u](TaskProgress &) {
                    std::string body, finalUrl;
                    if (PortalBrowser::instance().fetchUrl(u, body, finalUrl)) {
                        UIManager::instance().m_portalBody = body;
                        UIManager::instance().m_portalFinal = finalUrl;
                        UIManager::instance().m_portalShowBody = true;
                    } else {
                        UIManager::instance().m_portalLoading = false;
                    }
                });
            }
        } else if (t.rfind("PFORM:", 0) == 0) {
            // PFORM:method|action|query
            std::string rest = t.substr(6);
            size_t p1 = rest.find('|');
            if (p1 == std::string::npos) continue;
            size_t p2 = rest.find('|', p1 + 1);
            if (p2 == std::string::npos) continue;
            std::string method = rest.substr(0, p1);
            std::string action = rest.substr(p1 + 1, p2 - p1 - 1);
            std::string query = rest.substr(p2 + 1);
            if (action.empty()) continue;
            m_portalLoading = true;
            showToast("Đang gửi xác nhận...", {0, 180, 216, 255}, 2000);
            m_portalTask.run([method, action, query](TaskProgress &) {
                std::string body, finalUrl;
                if (PortalBrowser::instance().submitFetch(method, action, query,
                                                         body, finalUrl)) {
                    UIManager::instance().m_portalBody = body;
                    UIManager::instance().m_portalFinal = finalUrl;
                    UIManager::instance().m_portalShowBody = true;
                } else {
                    UIManager::instance().m_portalLoading = false;
                    UIManager::instance().showToast("Gửi thất bại, thử lại",
                                                    {239, 68, 68, 255}, 2500);
                }
            });
        }
    }
}

void UIManager::openPortalModal() {
    if (!m_portalModalInit) {
        m_portalModalCfg.ui = &m_ui;
        m_portalModalCfg.vk = &m_portalVk;
        m_portalModalCfg.history = &m_portalHist;
        m_portalModalCfg.fSmall = m_fontSmall;
        m_portalModalCfg.fMedium = m_fontMedium;
        m_portalModalCfg.fLarge = m_fontLarge;
        m_portalModalInit = true;
    }
    m_portalModalTitle = "NHẬP CHỮ CHO Ô ĐANG CHỌN";
    m_portalModalPh = "VD: 1990...";
    m_portalModalCfg.title = m_portalModalTitle.c_str();
    m_portalModalCfg.placeholder = m_portalModalPh.c_str();
    VirtualKeyboard::reset(m_portalVk, true);
    m_portalModalOpen = true;
}

void UIManager::renderPortal() {
    // Body fetch xong ở worker -> show trên main thread.
    if (m_portalShowBody && !m_portalTask.isRunning()) {
        m_portalShowBody = false;
        m_portalLoading = false;
        PortalBrowser::instance().showHtml(m_portalBody, m_portalFinal);
        PortalBrowser::instance().selectFirst();
        m_portalBody.clear();
    }
    drawAppBackground();
    drawAppHeader("DUYỆT PORTAL");
    if (!PortalBrowser::instance().ready()) {
        drawText("Browser chưa sẵn sàng.", 512, 300,
                 {170, 180, 195, 255}, m_fontSmall, true);
        drawAppFooter({{UiTheme::PadBtn::B, "Đóng"}});
        return;
    }
    // Pump engine theo nhịp 10fps (đủ cho countdown/animation, nhẹ CPU).
    bool dirty = false;
    {
        static uint32_t lastPump = 0;
        uint32_t nowMs = SDL_GetTicks();
        if (nowMs - lastPump >= 100) {
            lastPump = nowMs;
            dirty = PortalBrowser::instance().update();
        }
    }
    if (dirty || !m_portalTex) {
        if (!m_portalTex) {
            m_portalTex = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ARGB8888,
                                            SDL_TEXTUREACCESS_STREAMING,
                                            kPortalW, kPortalH);
            m_portalPx.assign((size_t)kPortalW * kPortalH * 4, 0);
        }
        if (m_portalTex) {
            int w = 0, h = 0;
            if (PortalBrowser::instance().copyPixels(m_portalPx.data(), w, h) &&
                w == kPortalW && h == kPortalH) {
                SDL_UpdateTexture(m_portalTex, nullptr, m_portalPx.data(),
                                  kPortalW * 4);
            }
        }
    }
    pollPortalBridge();
    // DEBUG tạm: trạng thái engine (sẽ gỡ).
    // (đã gộp vào dải URL phía dưới)
    // ELS tới sau (JS chạy) -> tự chọn ô đầu khi có.
    if (PortalBrowser::instance().selIndex() < 0 &&
        !PortalBrowser::instance().elements().empty()) {
        PortalBrowser::instance().selectFirst();
    }
    if (m_portalTex) {
        SDL_Rect dst = {0, kPageY, kPortalW, kPortalH};
        SDL_RenderCopy(m_renderer, m_portalTex, nullptr, &dst);
        // Vòng chọn ô D-pad (tọa độ ELS = tọa độ view, + offset trang).
        const auto& els = PortalBrowser::instance().elements();
        int si = PortalBrowser::instance().selIndex();
        if (si >= 0 && si < (int)els.size()) {
            const auto& e = els[si];
            drawRoundedBorder(e.x + 0 - 3, kPageY + e.y - 3, e.w + 6, e.h + 6, 8,
                              UiTheme::FOCUS_GLOW, 3);
        }
    }
    if (m_portalLoading) {
        drawText("Đang tải...", 512, kPageY + kPortalH / 2,
                 {245, 158, 11, 255}, m_fontSmall, true);
    }
    // Dải URL + debug engine (tạm, sẽ gỡ).
    {
        std::string info = PortalBrowser::instance().currentUrl();
        char db[64];
        std::snprintf(db, sizeof(db), " [paint=%d els=%zu]",
                      (int)PortalBrowser::instance().update(),
                      PortalBrowser::instance().elements().size());
        info += db;
        drawText(truncateToWidth(info, m_fontSmall, 976),
                 512, 664, UiTheme::TEXT_DIM, m_fontSmall, true);
    }
    drawAppFooter({{UiTheme::PadBtn::UPDOWN, "Chuyển ô"},
                   {UiTheme::PadBtn::A, "Bấm"},
                   {UiTheme::PadBtn::X, "Nhập chữ"},
                   {UiTheme::PadBtn::Y, "Tải lại"},
                   {UiTheme::PadBtn::L1R1, "Cuộn"},
                   {UiTheme::PadBtn::B, "Đóng"}});
    if (m_portalModalOpen) SearchInputModal::render(m_portalModalCfg);
}

bool UIManager::handlePortalInput() {
    InputManager& input = InputManager::instance();
    if (m_portalModalOpen) {
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
        auto res = SearchInputModal::handleInput(m_portalModalCfg, a, b, s, u, d,
                                                 l, r, x, y, l1, r1);
        if (res == SearchInputModal::Result::Commit) {
            m_portalModalOpen = false;
            std::string q = m_portalVk.query;
            if (!q.empty()) {
                m_portalHist.push_back(q);
                PortalBrowser::instance().typeText(q);
            }
        } else if (res == SearchInputModal::Result::Cancel) {
            m_portalModalOpen = false;
        }
        return true;
    }
    if (input.isButtonJustPressed(Button::UP)) {
        PortalBrowser::instance().moveSel(0, -1);
    } else if (input.isButtonJustPressed(Button::DOWN)) {
        PortalBrowser::instance().moveSel(0, 1);
    } else if (input.isButtonJustPressed(Button::LEFT)) {
        PortalBrowser::instance().moveSel(-1, 0);
    } else if (input.isButtonJustPressed(Button::RIGHT)) {
        PortalBrowser::instance().moveSel(1, 0);
    } else if (input.isButtonJustPressed(Button::A)) {
        // Click ô đang chọn; ô text thì mở bàn phím gõ luôn.
        const auto& els = PortalBrowser::instance().elements();
        int si = PortalBrowser::instance().selIndex();
        if (si >= 0 && si < (int)els.size() && els[si].isText()) {
            PortalBrowser::instance().clickSelected();
            openPortalModal();
        } else {
            PortalBrowser::instance().clickSelected();
        }
    } else if (input.isButtonJustPressed(Button::X)) {
        PortalBrowser::instance().clickSelected();
        openPortalModal();
    } else if (input.isButtonJustPressed(Button::L1)) {
        PortalBrowser::instance().scrollBy(0, -1);
    } else if (input.isButtonJustPressed(Button::R1)) {
        PortalBrowser::instance().scrollBy(0, 1);
    } else if (input.isButtonJustPressed(Button::Y)) {
        std::string u = PortalBrowser::instance().currentUrl();
        if (!u.empty() && !m_portalLoading) {
            m_portalLoading = true;
            m_portalTask.run([u](TaskProgress &) {
                std::string body, finalUrl;
                if (PortalBrowser::instance().fetchUrl(u, body, finalUrl)) {
                    UIManager::instance().m_portalBody = body;
                    UIManager::instance().m_portalFinal = finalUrl;
                    UIManager::instance().m_portalShowBody = true;
                } else {
                    UIManager::instance().m_portalLoading = false;
                }
            });
        }
    } else if (input.isButtonJustPressed(Button::B)) {
        closePortal();
        goBack();
        // Kiểm tra portal lại sau khi đóng (mạng có thể đã thông).
        std::thread([]() {
            PortalInfo pi = WifiManager::instance().checkPortal();
            if (pi.online)
                UIManager::instance().showToast("Mạng đã thông!",
                                                {34, 197, 94, 255}, 3000);
            else if (pi.portal)
                UIManager::instance().showToast("Vẫn bị portal chặn",
                                                {245, 158, 11, 255}, 3000);
        }).detach();
    }
    return true;
}

} // namespace RomCloud
