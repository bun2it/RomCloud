#pragma once
// ChromeBridge — giám sát gói trình duyệt RC-Web của chính RomCloud.
//
// Kiến trúc (2026-10-10):
//   RomCloud fork launch.sh của gói rc-web (nằm trong Apps/RomCloud/rc-web,
//   do chrome/fetch-rc-web.sh ráp từ nguồn mở) và supervise qua waitpid
//   (mẫu MpvPlayer). Weston chiếm toàn màn hình lúc duyệt; thoát phiên là
//   SIGTERM script -> trap cleanup trong launch.sh dọn weston/electron/
//   gptokeyb -> RAM thu hồi toàn bộ.
//
// Deep-link khi sửa địa chỉ (START): CDP :9222 của Electron,
//   PUT /json/new?<URL> qua socket trần (không cần curl trên máy).
// URL mở đầu: bridge ghi /tmp/rc-web-start-url + biến RC_WEB_URL,
//   electron main của ta tự đọc (CDP chỉ dùng cho các lần sau).
//
// Tay cầm trong phiên: mapping gptokeyb của RomCloud (chrome/browser.gptk);
//   RomCloud chỉ giữ B (thoát theo tầng qua /tmp/rc-web-ui-mode).
#include <SDL2/SDL.h>
#include <atomic>
#include <string>
#include <sys/types.h>

namespace RomCloud {

class ChromeBridge {
public:
    static ChromeBridge& instance();

    // Mở URL: chưa chạy thì fork launch.sh của rc-web; đang chạy thì
    // deep-link CDP vào phiên hiện tại. False khi chưa ráp gói rc-web
    // (caller fallback LITE + toast).
    bool openUrl(const std::string& url);
    bool openPortal(const std::string& portalUrl);

    // Vòng đời phiên ngoài
    bool start(const std::string& url);
    bool stop();
    bool isRunning();      // script còn sống (kể cả đang boot)
    bool isReady() const { return m_ready.load(); }
    bool failed() const { return m_failed.load(); }
    bool pollExited();     // true nếu vừa phát hiện đã thoát (đã reap + log)

    // Vẽ màn chờ (browser thật do weston takeover, ta chỉ vẽ khi chưa lên).
    void render(SDL_Renderer* renderer);

    // Deep-link trang mới vào phiên đang chạy (CDP /json/new).
    bool deepLink(const std::string& url);

    // Quét xác phiên web theo tên (dùng cho reaper nền khi không phiên
    // nào chạy — xem UIManager::update).
    static void sweepStrays();

private:
    ChromeBridge();
    ~ChromeBridge();
    ChromeBridge(const ChromeBridge&) = delete;
    ChromeBridge& operator=(const ChromeBridge&) = delete;

    // Gói rc-web của RomCloud: <appRoot>/rc-web (launch.sh + electron/).
    bool findWebDir(std::string& outDir);
    static bool cdpPut(const std::string& path);
    static bool cdpReady();

    pid_t m_pid = -1;
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_failed{false};
    std::string m_currentUrl;
};

} // namespace RomCloud
