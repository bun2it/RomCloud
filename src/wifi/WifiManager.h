#pragma once
// RomCloud WifiManager — quản lý Wi-Fi stock qua wpa_supplicant (root).
// - Quét + nối mạng WPA2-PSK (kể cả SSID ẩn: scan_ssid=1).
// - Phát hiện captive portal (204-check) + tự bấm chấp nhận form đơn giản.
// Mọi hàm blocking đều chạy nền (BackgroundTask), KHÔNG gọi trên UI thread.
// Config stock: /etc/wifi/wpa_supplicant.conf (ctrl /etc/wifi/sockets).
// Backup trước khi ghi lần đầu vào data dir.
#include <functional>
#include <string>
#include <vector>

namespace RomCloud {

struct WifiNet {
    std::string ssid;
    std::string bssid;
    int freq = 0;
    int signalDbm = -100;
    std::string flags;
    bool secured = true;
    bool saved = false;
};

struct PortalInfo {
    bool online = false;   // 204 ok, mạng thật
    bool portal = false;   // bị chặn portal
    std::string url;       // effectiveUrl (trang portal khi bị chặn)
    std::string title;
};

class WifiManager {
public:
    static WifiManager& instance();

    bool available(); // có wpa_cli + socket không
    // Trạng thái hiện tại (nhanh, không scan).
    std::string curSsid();
    std::string curState(); // wpa_state: COMPLETED / SCANNING / ...
    std::string curIp();

    // Quét blocking (~4-6s): scan + chờ + đọc results vào cache.
    bool refreshBlocking();
    const std::vector<WifiNet>& nets() const { return m_nets; }

    // Nối mạng blocking (tới ~20s). hidden=true cho SSID ẩn.
    // Trả về thông điệp tiếng Việt để toast/log.
    bool connectBlocking(const std::string& ssid, const std::string& psk,
                         bool hidden, std::string& outMsg);
    // Bật lại mạng đã lưu (không nhập pass lại).
    bool enableSavedBlocking(const std::string& ssid, std::string& outMsg);
    // Mạng đã lưu có pass chưa (thiếu pass thì phải hỏi lại).
    bool savedHasPsk(const std::string& ssid);
    bool forgetBlocking(const std::string& ssid, std::string& outMsg);
    bool reconnectBlocking();

    // Captive portal: kiểm tra + thử tự chấp nhận (blocking).
    PortalInfo checkPortal();
    bool acceptPortalBlocking(std::string& outMsg);
    // Hỏi người dùng điền 1 ô form portal (vd năm sinh). prompt(fieldLabel)
    // chạy trên worker thread, block tới khi UI trả lời qua answerPortal().
    // Trả về "" nếu user hủy/timeout.
    void setPromptHandler(std::function<std::string(const std::string& field)> h) { m_prompt = h; }
    void clearPromptHandler() { m_prompt = nullptr; }

private:
    WifiManager() = default;
    std::vector<WifiNet> m_nets;
    std::function<std::string(const std::string&)> m_prompt;
    void backupConf();
    static std::string cli(const std::string& args);
};

} // namespace RomCloud
