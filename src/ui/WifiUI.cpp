#include "UIManager.h"
#include "../wifi/WifiManager.h"
#include "../config/AppConfig.h"
#include "../input/InputManager.h"
#include "../logging/Logger.h"
#include "../platform/PlatformInfo.h"

#include <ctime>
#include <fstream>

namespace RomCloud {

static int wifiPct(int dbm) {
    if (dbm <= -100) return 0;
    if (dbm >= -50) return 100;
    return (dbm + 100) * 2;
}

void UIManager::startWifiScan() {
    if (m_wifiBusy || m_wifiTask.isRunning()) return;
    m_wifiBusy = true;
    m_wifiStatus = "Đang quét mạng...";
    m_wifiTask.run([](TaskProgress &) {
        WifiManager::instance().refreshBlocking();
    });
}

void UIManager::startWifiConnect(const std::string& ssid, const std::string& psk,
                                 bool hidden, bool useSaved) {
    if (m_wifiBusy || m_wifiTask.isRunning()) {
        showToast("Đang làm Wi-Fi, chờ chút...", {245, 158, 11, 255}, 1500);
        return;
    }
    m_wifiBusy = true;
    m_wifiStatus = "Đang nối " + ssid + "...";
    m_wifiTask.run([ssid, psk, hidden, useSaved](TaskProgress &) {
        std::string msg;
        bool ok;
        if (useSaved)
            ok = WifiManager::instance().enableSavedBlocking(ssid, msg);
        else
            ok = WifiManager::instance().connectBlocking(ssid, psk, hidden, msg);
        // Nối xong: kiểm tra portal luôn để báo ngay.
        if (ok) {
            PortalInfo pi = WifiManager::instance().checkPortal();
            if (pi.portal) msg += " • Có portal xác nhận (SELECT)";
            else if (pi.online) msg += " • Mạng thông";
        }
        UIManager::instance().m_wifiStatus = msg;
        UIManager::instance().showToast(msg, ok ? SDL_Color{34, 197, 94, 255}
                                                : SDL_Color{239, 68, 68, 255},
                                        3000);
        UIManager::instance().m_wifiNeedScan = true;
    });
}

void UIManager::startWifiPortal() {
    if (m_wifiBusy || m_wifiTask.isRunning()) {
        showToast("Đang làm Wi-Fi, chờ chút...", {245, 158, 11, 255}, 1500);
        return;
    }
    // Hook test: file debug_portal.txt chứa URL -> mở browser thẳng (test
    // portal giả, không cần ra quán). Xóa file là về flow thường.
    {
        std::string appRoot = AppConfig::instance().getAppRoot();
        if (appRoot.empty()) appRoot = "/mnt/SDCARD/Apps/RomCloud";
        std::ifstream f(appRoot + "/debug_portal.txt");
        std::string u;
        if (f && std::getline(f, u)) {
            while (!u.empty() && (u.back() == '\r' || u.back() == '\n' || u.back() == ' '))
                u.pop_back();
            if (!u.empty()) {
                showToast("Mở portal test...", {0, 180, 216, 255}, 1500);
                openPortal(u);
                return;
            }
        }
    }
    // Mở browser duyệt portal tay (thấy nút nào bấm nút đó).
    // Engine init TRÊN MAIN thread (WebKit kỵ tạo renderer ở worker):
    // worker chỉ check, main mở browser qua m_wifiPortalUrl.
    m_wifiBusy = true;
    m_wifiStatus = "Đang kiểm tra portal...";
    m_wifiTask.run([](TaskProgress &) {
        PortalInfo pi = WifiManager::instance().checkPortal();
        if (pi.online) {
            UIManager::instance().m_wifiStatus = "Mạng OK, không bị portal chặn";
        } else if (pi.portal) {
            UIManager::instance().m_wifiStatus = "Mở browser portal...";
            UIManager::instance().m_wifiPortalUrl = pi.url;
        } else {
            UIManager::instance().m_wifiStatus = "Chưa có mạng, kiểm tra Wi-Fi trước";
        }
    });
}

void UIManager::openWifiModal(int mode, const std::string& title,
                              const std::string& placeholder) {
    if (!m_wifiModalInit) {
        m_wifiModalCfg.ui = &m_ui;
        m_wifiModalCfg.vk = &m_wifiVk;
        m_wifiModalCfg.history = &m_wifiHist;
        m_wifiModalCfg.fSmall = m_fontSmall;
        m_wifiModalCfg.fMedium = m_fontMedium;
        m_wifiModalCfg.fLarge = m_fontLarge;
        m_wifiModalCfg.title = nullptr;
        m_wifiModalCfg.placeholder = nullptr;
        m_wifiModalInit = true;
    }
    m_wifiModalTitle = title;
    m_wifiModalPh = placeholder;
    m_wifiModalCfg.title = m_wifiModalTitle.c_str();
    m_wifiModalCfg.placeholder = m_wifiModalPh.c_str();
    VirtualKeyboard::reset(m_wifiVk, true);
    m_wifiModalMode = mode;
    m_wifiModalOpen = true;
}

void UIManager::renderWifiTab() {
    // Task xong: nhả busy, refresh trạng thái.
    if (!m_wifiTask.isRunning() && m_wifiBusy) {
        m_wifiBusy = false;
        if (m_wifiNeedScan) {
            m_wifiNeedScan = false;
            m_wifiSel = 0;
            m_wifiScroll = 0;
        }
        m_wifiStatusMs = 0; // ép cập nhật dòng trạng thái ngay
    }
    // Portal check xong ở worker -> main mở browser (đúng thread).
    if (!m_wifiPortalUrl.empty() && !m_wifiBusy && !m_wifiTask.isRunning()) {
        std::string u = m_wifiPortalUrl;
        m_wifiPortalUrl.clear();
        openPortal(u);
        return;
    }
    // Vào tab lần đầu: quét luôn.
    if (m_wifiNeedScan && !m_wifiBusy && !m_wifiTask.isRunning() && m_wifiStatus.empty()) {
        startWifiScan();
    }
    // Dòng trạng thái: cache 3s (tránh spawn wpa_cli mỗi frame).
    uint32_t nowMs = SDL_GetTicks();
    if (m_wifiStatusMs == 0 || nowMs - m_wifiStatusMs > 3000) {
        m_wifiStatusMs = nowMs;
        if (!m_wifiBusy) {
            std::string ssid = WifiManager::instance().curSsid();
            std::string st = WifiManager::instance().curState();
            std::string ip = PlatformInfo::instance().getIpAddress("wlan0");
            m_wifiCurSsid = (st == "COMPLETED") ? ssid : "";
            if (ssid.empty()) m_wifiStatus = "Chưa nối mạng nào (" + st + ")";
            else m_wifiStatus = ssid + " • " + ip + " • " + st;
        }
    }

    const int contentTop = 124;
    const int contentH = 715 - contentTop - 16;
    // Card trạng thái.
    drawRoundedRect(24, contentTop, 1024 - 48, 64, UiTheme::RADIUS_CARD,
                    {22, 28, 38, 255}, true);
    drawText(truncateToWidth(m_wifiStatus.empty() ? "Wi-Fi" : m_wifiStatus,
                             m_fontMedium, 1024 - 96),
             24 + 20, contentTop + (64 - textHeight(m_fontMedium)) / 2,
             {0, 180, 216, 255}, m_fontMedium, false);

    const auto& nets = WifiManager::instance().nets();
    const int listTop = contentTop + 64 + 12;
    const int listH = 715 - listTop - 16;
    if (nets.empty()) {
        drawText(m_wifiBusy ? "Đang quét..." : "Chưa thấy mạng nào. Bấm Y quét.",
                 512, listTop + 60, {170, 180, 195, 255}, m_fontSmall, true);
    } else {
        if (m_wifiSel >= nets.size()) m_wifiSel = nets.size() - 1;
        drawRoundedRect(24, listTop, 1024 - 48, listH, UiTheme::RADIUS_CARD,
                        {22, 28, 38, 255}, true);
        const int rowH = 48;
        int visible = (listH - 8) / rowH;
        if (visible < 1) visible = 1;
        if ((int)m_wifiSel < m_wifiScroll) m_wifiScroll = (int)m_wifiSel;
        if ((int)m_wifiSel >= m_wifiScroll + visible)
            m_wifiScroll = (int)m_wifiSel - visible + 1;
        for (int i = 0; i < visible && m_wifiScroll + i < (int)nets.size(); ++i) {
            int pos = m_wifiScroll + i;
            int y = listTop + 4 + i * rowH;
            bool sel = (pos == (int)m_wifiSel);
            if (sel) drawHighlight(24 + 8, y + 3, 1024 - 48 - 16, rowH - 6);
            const auto& n = nets[pos];
            bool connected = (!m_wifiCurSsid.empty() && n.ssid == m_wifiCurSsid);
            char nb[16];
            std::snprintf(nb, sizeof(nb), "%d", wifiPct(n.signalDbm));
            std::string right = std::string(nb) + "%" + (n.secured ? " Khóa" : " Mở") +
                                (n.saved ? " *" : "") +
                                (connected ? " • Đang dùng" : "");
            int rightW = textWidth(right, m_fontSmall) + 20;
            int nameW = 1024 - 48 - 20 - 48 - rightW - 20;
            if (nameW < 60) nameW = 60;
            drawDot(24 + 28, y + rowH / 2, 5,
                    connected ? SDL_Color{0, 180, 216, 255}
                    : (n.secured ? SDL_Color{250, 204, 21, 255} : SDL_Color{34, 197, 94, 255}));
            drawText(truncateToWidth(n.ssid, m_fontMedium, nameW),
                     24 + 48, y + (rowH - 6 - textHeight(m_fontMedium)) / 2,
                     sel ? SDL_Color{255, 255, 255, 255} : UiTheme::TEXT_MAIN,
                     m_fontMedium, false);
            drawTextRight(right, 1024 - 24 - 20,
                          y + (rowH - 6 - textHeight(m_fontSmall)) / 2,
                          UiTheme::TEXT_SUB, m_fontSmall);
        }
    }

    drawAppFooter({{UiTheme::PadBtn::A, "Nối"},
                   {UiTheme::PadBtn::Y, "Quét"},
                   {UiTheme::PadBtn::X, "SSID ẩn"},
                   {UiTheme::PadBtn::SELECT, "Portal"},
                   {UiTheme::PadBtn::START, "Quên"},
                   {UiTheme::PadBtn::B, "Lùi"}});
    if (m_wifiModalOpen) SearchInputModal::render(m_wifiModalCfg);
}

bool UIManager::handleWifiTabInput() {
    InputManager& input = InputManager::instance();
    // Modal nhập SSID/pass mở thì route vào modal.
    if (m_wifiModalOpen) {
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
        auto res = SearchInputModal::handleInput(m_wifiModalCfg, a, b, s, u, d,
                                                 l, r, x, y, l1, r1);
        if (res == SearchInputModal::Result::Commit) {
            m_wifiModalOpen = false;
            std::string q = m_wifiVk.query;
            if (m_wifiModalMode == 1) {
                // Xong SSID ẩn -> hỏi pass.
                if (!q.empty()) {
                    m_wifiPendingSsid = q;
                    openWifiModal(2, "MẬT KHẨU WI-FI (trống = mạng mở)",
                                  "Bỏ trống nếu mạng không pass...");
                }
            } else if (m_wifiModalMode == 2) {
                // Xong pass (SSID ẩn hoặc mạng khóa).
                startWifiConnect(m_wifiPendingSsid, q, m_wifiModalMode == 2 && m_wifiHidden, false);
                m_wifiPendingSsid.clear();
            }
            m_wifiModalMode = 0;
        } else if (res == SearchInputModal::Result::Cancel) {
            m_wifiModalOpen = false;
            m_wifiModalMode = 0;
            m_wifiPendingSsid.clear();
        }
        return true;
    }
    const auto& nets = WifiManager::instance().nets();
    if (input.isButtonJustPressed(Button::UP) && !nets.empty()) {
        m_wifiSel = (m_wifiSel + nets.size() - 1) % nets.size();
    } else if (input.isButtonJustPressed(Button::DOWN) && !nets.empty()) {
        m_wifiSel = (m_wifiSel + 1) % nets.size();
    } else if (input.isButtonJustPressed(Button::A) && !nets.empty()) {
        const auto& n = nets[m_wifiSel];
        if ((n.secured && !n.saved) ||
            (n.saved && n.secured && !WifiManager::instance().savedHasPsk(n.ssid))) {
            // Mạng khóa chưa lưu, hoặc đã lưu nhưng thiếu pass (như Home_2.4Ghz
            // của stock): hỏi pass rồi ghi đè.
            m_wifiPendingSsid = n.ssid;
            m_wifiHidden = false;
            openWifiModal(2, "MẬT KHẨU: " + n.ssid, "Nhập pass Wi-Fi...");
        } else if (n.saved) {
            startWifiConnect(n.ssid, "", false, true);
        } else {
            startWifiConnect(n.ssid, "", false, false); // mạng mở
        }
    } else if (input.isButtonJustPressed(Button::Y)) {
        m_wifiNeedScan = true;
        startWifiScan();
    } else if (input.isButtonJustPressed(Button::X)) {
        // SSID ẩn: nhập tên trước.
        m_wifiHidden = true;
        openWifiModal(1, "TÊN MẠNG ẨN (SSID)", "VD: CafeMayChu...");
    } else if (input.isButtonJustPressed(Button::SELECT)) {
        startWifiPortal();
    } else if (input.isButtonJustPressed(Button::START) && !nets.empty()) {
        const auto& n = nets[m_wifiSel];
        if (m_wifiBusy || m_wifiTask.isRunning()) return true;
        m_wifiBusy = true;
        m_wifiStatus = "Đang xóa " + n.ssid + "...";
        std::string ssid = n.ssid;
        m_wifiTask.run([ssid](TaskProgress &) {
            std::string msg;
            WifiManager::instance().forgetBlocking(ssid, msg);
            UIManager::instance().m_wifiStatus = msg;
            UIManager::instance().m_wifiNeedScan = true;
        });
    }
    return true;
}

} // namespace RomCloud
