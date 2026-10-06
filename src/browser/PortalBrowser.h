#pragma once
// RomCloud PortalBrowser — browser tối giản cho captive portal (nhánh portal-browser).
// Kiến trúc hybrid (đã verify POC trên Brick):
// - Tải trang bằng HttpClient (curl, chạy tốt) + cookie jar riêng.
// - Render/tương tác bằng Ultralight CPU (LoadHTML, network nội tắt).
// - Click/form chặn qua console bridge: JS báo về, app fetch tiếp.
// RAM rule: lazy-init khi mở portal, shutdown() free sạch khi đóng.
#include <string>
#include <vector>

namespace RomCloud {

struct PortalConsoleMsg {
    std::string text;
};

// Ô focusable trên trang (từ ELS bridge). Điều hướng không gian cho D-pad.
struct PortalEl {
    std::string tag;  // INPUT/BUTTON/A/...
    std::string type; // text/submit/checkbox/... (INPUT)
    std::string name;
    int x = 0, y = 0, w = 0, h = 0;
    bool isText() const {
        if (tag == "TEXTAREA") return true;
        if (tag != "INPUT") return false;
        return type == "text" || type == "password" || type == "tel" ||
               type == "number" || type == "date" || type == "email" ||
               type == "search" || type == "url" || type.empty();
    }
    bool isClickable() const { return true; }
};

class PortalBrowser {
public:
    static PortalBrowser& instance();
    // Khởi tạo engine (nặng ~50MB, chỉ gọi khi mở portal).
    // resDir: chỗ chứa resources/ Ultralight; fontPath: TTF tiếng Việt.
    bool init(const std::string& resDir, const std::string& fontPath,
              int width, int height);
    void shutdown(); // free toàn bộ
    bool ready() const { return m_ready; }

    // Tải URL qua HttpClient + nạp vào view. Trả về true nếu fetch ok.
    bool navigate(const std::string& url);
    // Tách thread: fetch nền (an toàn) rồi showHtml trên main thread.
    bool fetchUrl(const std::string& url, std::string& outBody, std::string& outFinalUrl);
    bool showHtml(const std::string& body, const std::string& url);
    // Submit form (chạy nền): trả body + finalUrl, KHÔNG LoadHTML.
    bool submitFetch(const std::string& method, const std::string& action,
                     const std::string& query, std::string& outBody, std::string& outFinal);
    const std::string& currentUrl() const { return m_url; }

    // Pump engine mỗi frame. Trả về true nếu bitmap đổi (cần blit lại).
    bool update();
    // Copy pixels BGRA ra dst (dst đủ w*h*4). Trả về false nếu chưa có gì.
    bool copyPixels(unsigned char* dst, int& outW, int& outH);

    // D-pad: Tab/Shift-Tab chuyển focus, arrows cuộn/di chuyển, Enter click.
    void pressTab(bool shift = false);
    void pressEnter();
    void pressArrow(int dx, int dy); // dx,dy ∈ {-1,0,1}
    // Click chuột tại tọa độ view (dự phòng khi Tab không tới).
    void clickAt(int x, int y);
    // Điều hướng không gian theo rect ELS (cho D-pad, thay Tab).
    const std::vector<PortalEl>& elements();
    int selIndex() const { return m_sel; }
    bool moveSel(int dx, int dy); // true nếu đổi ô
    void selectFirst();
    bool clickSelected(); // click ô đang chọn (focus + click)
    void scrollBy(int dx, int dy);
    // Gõ text vào ô đang focus (từ bàn phím ảo).
    void typeText(const std::string& utf8);

    // Điều hướng do JS/location trong trang (OnChangeURL). pollNav() lấy
    // URL chờ (rỗng nếu không có); app fetch tiếp rồi navigate lại.
    std::string pollNav();
    // Lịch sử duyệt (L1 lui / R1 tới). historyGo trả URL cần tải.
    void pushHistory(const std::string& url);
    bool historyGo(int delta, std::string& outUrl);
    void clearHistory();
    // Console JS kể từ lần đọc trước (bridge click/form).
    std::vector<PortalConsoleMsg> drainConsole();

private:
    PortalBrowser() = default;
    ~PortalBrowser() { shutdown(); }
    bool m_ready = false;
    int m_w = 0, m_h = 0;
    std::string m_url;
    std::string m_cookieJar; // "a=b; c=d"
    std::vector<PortalConsoleMsg> m_console;
    std::string m_lastEls;
    std::vector<PortalEl> m_elsCache;
    std::string m_elsParsedFrom;
    int m_sel = -1;
    std::vector<std::string> m_hist;
    int m_histIdx = -1;
    std::string m_pendingNav;
    void* m_impl = nullptr; // UltralightImpl* (giấu header SDK khỏi app)

    void storeCookies(const std::string& setCookieLine);
    std::string cookieHeader() const;
    // Tải URL qua HttpClient (kèm cookie), trả body (rỗng nếu lỗi).
    std::string fetchPage(const std::string& url, std::string& outFinalUrl);
    // Nối URL tương đối với base.
    static std::string resolveUrl(const std::string& base, const std::string& ref);
    // Nội tuyến CSS/JS/ảnh (portal nhẹ, giới hạn size/số file).
    std::string inlineResources(const std::string& html, const std::string& baseUrl);
};

} // namespace RomCloud
