#define _GNU_SOURCE  // POSIX_SPAWN_SETSID cho posix_spawn
#include "ChromeBridge.h"
#include "../../config/AppConfig.h"
#include "../../logging/Logger.h"
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace RomCloud {

namespace {
// ── WEBTRACE: vết từng bước của phiên RC-Web (nằm trong debug.log) ──
// Mã bước: B = bridge (file này), L = launch.sh trên máy (rc-web.log),
// J = electron main (rc-web.log). Mốc +ms tính từ lúc start() bắt đầu,
// để đối chiếu 2 file log theo thời gian tuyệt đối của Logger.
std::chrono::steady_clock::time_point g_t0 =
    std::chrono::steady_clock::now();

long long traceMs() {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - g_t0)
        .count();
}

void trace(const char* step, const std::string& msg) {
    Logger::info("[WEBTRACE] " + std::string(step) + " (+" +
                 std::to_string(traceMs()) + "ms) " + msg);
}

void traceWarn(const char* step, const std::string& msg) {
    Logger::warn("[WEBTRACE] " + std::string(step) + " (+" +
                 std::to_string(traceMs()) + "ms) " + msg);
}

// ── Nhường/lấy lại DRM master quanh phiên weston ─────────────────────
// RomCloud (SDL/KMSDRM) giữ DRM master nên weston drm-backend không
// modeset đè lên được: nó vẫn render nhưng output nằm dưới frame của
// RomCloud (cảm giác freeze ở UI RomCloud). Chạy root nên weston có
// thể steal, nhưng chắc ăn nhất là RomCloud tự DROP_MASTER ngay trước
// khi fork launch.sh và SET_MASTER lại sau stop(). Không cần libdrm:
// _IO('d', nr) với nr từ drm.h (SET_MASTER=0x11, DROP_MASTER=0x1F).
static int drmOpenCard() {
    for (int i = 0; i < 4; ++i) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) return fd;
    }
    return -1;
}

static void drmDropMaster() {
    int fd = drmOpenCard();
    if (fd < 0) {
        trace("B02", "không thấy /dev/dri/card* -> bỏ qua DROP_MASTER");
        return;
    }
    if (ioctl(fd, _IO('d', 0x1F)) == 0)
        trace("B02", "đã DROP_MASTER DRM (nhường màn cho weston)");
    else
        trace("B02", std::string("DROP_MASTER lỗi (có thể vốn không giữ): ") +
                         strerror(errno));
    close(fd);
}

static void drmSetMaster() {
    int fd = drmOpenCard();
    if (fd < 0) return;
    if (ioctl(fd, _IO('d', 0x11)) == 0)
        trace("B09", "đã SET_MASTER DRM lại (RomCloud giữ màn)");
    else
        traceWarn("B09",
                  std::string("SET_MASTER DRM lỗi: ") + strerror(errno));
    close(fd);
}

// Quét xác phiên web theo TÊN tiến trình (electron/weston/gptokeyb2/
// Xwayland): m_pid chỉ theo dõi script launch.sh; con cháu double-fork
// hoặc sót sau ECHILD sẽ thoát lưới reap — sót 1 weston là 2 bên cùng
// vẽ fb0 (flicker) hoặc "thấy Chrome ẩn dưới" ở menu/Lite. Chỉ chạy trên
// máy (không bao giờ đụng tiến trình Mac khi build simulator).
static void killWebProcesses() {
#ifndef PC_SIMULATOR_MODE
    static const char* kProcs[] = {"electron", "weston", "gptokeyb2",
                                   "Xwayland"};
    for (const char* p : kProcs) {
        std::string cmd = std::string("killall -9 ") + p +
                          " >/dev/null 2>&1";
        int rc = system(cmd.c_str());
        (void)rc;
    }
    trace("B09", "đã quét xác phiên web theo tên tiến trình");
#endif
}
}  // namespace

ChromeBridge& ChromeBridge::instance() {
    static ChromeBridge inst;
    return inst;
}

ChromeBridge::ChromeBridge() = default;

ChromeBridge::~ChromeBridge() {
    stop();
}

// ── Tìm gói trình duyệt của chính RomCloud ───────────────────────────
// Bố cục khi đã ráp (chrome/fetch-rc-web.sh) và đẩy lên máy:
//   Apps/RomCloud/rc-web/launch.sh         (script của ta, có quyền X)
//   Apps/RomCloud/rc-web/electron/electron (Electron từ electronjs.org)
bool ChromeBridge::findWebDir(std::string& outDir) {
    std::string appRoot = AppConfig::instance().getAppRoot();
    if (appRoot.empty()) appRoot = "/mnt/SDCARD/Apps/RomCloud";
    std::string d = appRoot + "/rc-web";
    bool hasLauncher = access((d + "/launch.sh").c_str(), X_OK) == 0;
    bool hasElectron =
        access((d + "/electron/electron").c_str(), X_OK) == 0;
    trace("B01", "kiểm gói rc-web tại " + d + ": launch.sh=" +
              std::string(hasLauncher ? "OK" : "THIẾU/không X") +
              " electron=" + (hasElectron ? "OK" : "THIẾU/không X"));
    if (hasLauncher && hasElectron) {
        outDir = d;
        return true;
    }
    return false;
}

bool ChromeBridge::openUrl(const std::string& url) {
    return start(url);
}

bool ChromeBridge::openPortal(const std::string& portalUrl) {
    trace("B00", "openPortal: " + portalUrl);
    return start(portalUrl);
}

// Env cho tiến trình con: chép environ của RomCloud nhưng bỏ các biến
// nạp thư viện (LD_LIBRARY_PATH trỏ vào lib/ của RomCloud sẽ làm
// electron/weston nạp nhầm .so), rồi thêm URL mở đầu. Gói rc-web tự đặt
// LD_LIBRARY_PATH của riêng nó trong launch.sh.
static std::vector<std::string> sanitizedEnv(const std::string& startUrl) {
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
        std::string s = *e;
        if (s.rfind("LD_LIBRARY_PATH=", 0) == 0) continue;
        if (s.rfind("LD_PRELOAD=", 0) == 0) continue;
        if (s.rfind("LD_AUDIT=", 0) == 0) continue;
        if (s.rfind("RC_WEB_URL=", 0) == 0) continue;
        env.push_back(s);
    }
    env.push_back("RC_WEB_URL=" + startUrl);
    return env;
}

bool ChromeBridge::start(const std::string& url) {
    g_t0 = std::chrono::steady_clock::now();
    trace("B00", "yêu cầu mở phiên RC-Web, url='" + url + "'" +
              (isRunning() ? " -> đang có phiên, sẽ deep-link"
                           : " -> mở phiên mới"));
    if (isRunning()) {
        // Đang trong phiên: mở URL mới bằng deep-link CDP.
        if (!url.empty()) deepLink(url);
        return true;
    }
    // Dọn xác phiên cũ (reap + log mã thoát) trước khi mở phiên mới.
    pollExited();
    stop();
    m_ready.store(false);
    m_failed.store(false);

    std::string dir;
    if (!findWebDir(dir)) {
        traceWarn("B01", "chưa có gói rc-web dùng được "
                     "(chạy chrome/fetch-rc-web.sh trên Mac rồi đẩy vào "
                     "Apps/RomCloud/rc-web) -> hủy mở phiên");
        return false;
    }
    trace("B02", "sẽ fork launch.sh trong " + dir);

    // Xóa socket wayland sót từ phiên trước: renderChromeState() dừng vẽ
    // màn chờ ngay khi socket xuất hiện, nên socket cũ phải đi trước khi
    // fork (không thì màn chờ tắt oan + weston cũng tự rm lại).
    unlink("/tmp/rc-web-run/wayland-1");
    unlink("/tmp/rc-web-run/wayland-1.lock");

    std::string target = url.empty() ? "https://duckduckgo.com/" : url;
    m_currentUrl = target;

    // URL mở đầu cho electron main của ta (nó tự đọc lúc khởi động;
    // CDP khi đó chưa lên).
    bool urlFileOk = false;
    if (FILE* f = fopen("/tmp/rc-web-start-url", "w")) {
        fputs(target.c_str(), f);
        fclose(f);
        urlFileOk = true;
    }
    trace("B02", "URL mở đầu='" + target + "' ghi /tmp/rc-web-start-url: " +
              std::string(urlFileOk ? "OK" : "THẤT BẠI (main còn biến "
                                             "RC_WEB_URL để dùng)"));

    // Nhả DRM master TRƯỚC khi fork: weston drm-backend modeset đè lên
    // được ngay, không còn cảnh render mà không hiện (freeze ở UI cũ).
    drmDropMaster();

    std::string cmd = "cd \"" + dir + "\" && exec ./launch.sh";
    char* argv[] = {(char*)"/bin/sh", (char*)"-c", (char*)cmd.c_str(),
                    nullptr};

    std::vector<std::string> envStore = sanitizedEnv(target);
    std::vector<char*> envp;
    envp.reserve(envStore.size() + 1);
    for (auto& s : envStore) envp.push_back(s.data());
    envp.push_back(nullptr);

    // Session riêng cho phiên duyệt web: tín hiệu gửi tới process group
    // của RomCloud (thoát app, kill từ MainUI...) không lan sang weston,
    // và stop() giết được trọn nhóm của phiên.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
#endif
    pid_t pid = -1;
    trace("B03", "posix_spawn /bin/sh -c '" + cmd + "' (setsid: " +
#ifdef POSIX_SPAWN_SETSID
              std::string("có")
#else
              std::string("không — header thiếu POSIX_SPAWN_SETSID")
#endif
              + ")");
    int rc = posix_spawn(&pid, "/bin/sh", nullptr, &attr, argv, envp.data());
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        traceWarn("B03", "fork launch.sh rc-web THẤT BẠI: rc=" +
                      std::to_string(rc) + " " + std::string(strerror(rc)));
        drmSetMaster();  // fork hỏng: lấy lại màn ngay
        return false;
    }
    m_pid = pid;
    trace("B03", "đã fork phiên RC-Web, pid con=" + std::to_string(pid));

    // Luồng nền chờ CDP (dấu hiệu electron đã lên và nhận điều khiển).
    // URL đầu không deep-link: main đã tự mở từ RC_WEB_URL nên vòng này
    // chỉ đặt cờ sẵn sàng; quá 30s thì báo failed để UI tự quay về menu
    // (trước đây 90s: màn hình đứng yên quá lâu khi electron chết/OOM).
    try {
        std::thread([] {
            ChromeBridge& self = ChromeBridge::instance();
            trace("B04", "bắt đầu chờ CDP :9222 (tối đa 30s)");
            for (int i = 0; i < 60; ++i) {  // 60 x 500ms = 30s
                if (self.m_pid <= 0) {
                    trace("B04", "huỷ chờ CDP: phiên đã đóng (pid<=0)");
                    return;
                }
                if (ChromeBridge::cdpReady()) {
                    self.m_ready.store(true);
                    trace("B05", "CDP :9222 đã trả lời — phiên sẵn sàng");
                    return;
                }
                if (i > 0 && i % 10 == 0) {  // nhịp tim mỗi 5s
                    trace("B04", "đang chờ CDP... đã " +
                              std::to_string(i / 2) + "s");
                }
                usleep(500 * 1000);
            }
            self.m_failed.store(true);
            traceWarn("B06", "quá 30s vẫn chưa thấy CDP :9222 -> failed, "
                             "UI sẽ báo mở thất bại");
        }).detach();
    } catch (...) {
        // Hiếm: máy hết RAM không tạo được thread. UI vẫn poll trạng thái
        // tiến trình con nên không kẹt; chỉ mất mốc "sẵn sàng".
        Logger::warn("ChromeBridge: không tạo được thread chờ CDP");
    }
    return true;
}

bool ChromeBridge::isRunning() {
    if (m_pid <= 0) return false;
    // Chỉ kiểm sự tồn tại, KHÔNG reap ở đây: pollExited() là nơi reap
    // duy nhất để còn ghi được mã thoát/tín hiệu vào log. (Con đã chết
    // thành zombie vẫn trả lời kill(,0)==0 cho tới khi pollExited reap.)
    return kill(m_pid, 0) == 0 || errno == EPERM;
}

bool ChromeBridge::pollExited() {
    if (m_pid <= 0) return true;
    int status = 0;
    pid_t r = waitpid(m_pid, &status, WNOHANG);
    if (r == m_pid) {
        // Ghi rõ cách phiên kết thúc: thoát sạch / mã lỗi / bị tín hiệu
        // (9=SIGKILL thường là hết RAM). Trước đây status bị vứt bỏ nên
        // không phân biệt được trình duyệt tự đóng hay bị hệ thống giết.
        if (WIFEXITED(status)) {
            trace("B08", "tiến trình con đã thoát SẠCH, exit code " +
                         std::to_string(WEXITSTATUS(status)) +
                         " (thời lượng phiên xem mốc +ms ở trên)" +
                         (WEXITSTATUS(status) == 0
                              ? ""
                              : " — xem rc-web.log đoạn cuối để biết bước nào lỗi"));
        } else if (WIFSIGNALED(status)) {
            traceWarn("B08", "tiến trình con bị TÍN HIỆU " +
                         std::to_string(WTERMSIG(status)) +
                         " (9=SIGKILL thường là hết RAM, 15=SIGTERM bị dừng từ ngoài)" +
                         " — xem rc-web.log xem electron chết ở bước nào");
        } else {
            trace("B08", "tiến trình con kết thúc, raw status " +
                         std::to_string(status));
        }
        m_pid = -1;
        m_ready.store(false);
        drmSetMaster();  // phiên tự chết: lấy lại màn cho RomCloud
        return true;
    }
    if (r < 0 && errno == ECHILD) {  // đã bị reap ở nơi khác
        trace("B08", "tiến trình con đã được reap ở nơi khác (ECHILD)");
        m_pid = -1;
        m_ready.store(false);
        drmSetMaster();
        return true;
    }
    return false;
}

bool ChromeBridge::stop() {
    if (m_pid <= 0) {
        killWebProcesses();  // không pid nào theo dõi: vẫn quét xác sót
        return true;
    }
    trace("B09", "stop(): gửi SIGTERM tới pid con " + std::to_string(m_pid) +
              " (chờ tối đa 3s cho trap cleanup trong launch.sh chạy)");
    // SIGTERM tới đúng tiến trình script: trap cleanup trong launch.sh
    // của ta dọn weston/electron/gptokeyb rồi trả màn hình cho MainUI.
    kill(m_pid, SIGTERM);
    for (int i = 0; i < 150; ++i) {  // tối đa ~3s
        if (pollExited()) {
            trace("B09", "phiên đã đóng sau ~" +
                      std::to_string(i * 20) + "ms kể từ SIGTERM");
            drmSetMaster();  // weston chết rồi: lấy lại màn cho RomCloud
            killWebProcesses();  // quét xác sót theo tên (tránh 2 weston vẽ chồng)
            return true;
        }
        usleep(20 * 1000);
    }
    // Quá hạn: leo thang tới cả nhóm tiến trình của phiên (script đã
    // setsid khi fork nên nhóm là -pid; không đụng nhóm của RomCloud).
    traceWarn("B10", "quá 3s chưa thoát -> SIGKILL cả nhóm tiến trình phiên");
    kill(-m_pid, SIGKILL);
    kill(m_pid, SIGKILL);
    int status = 0;
    waitpid(m_pid, &status, 0);
    traceWarn("B10", "đã SIGKILL xong, raw status " +
                  std::to_string(status));
    m_pid = -1;
    m_ready.store(false);
    drmSetMaster();
    killWebProcesses();
    return true;
}

// ── CDP qua socket trần (không phụ thuộc curl trên máy) ─────────────
static int cdpHttp(const std::string& request) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv;
    tv.tv_sec = 4;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9222);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (send(fd, request.data(), request.size(), 0) < 0) {
        close(fd);
        return -1;
    }
    char buf[512];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    const char* sp = strchr(buf, ' ');  // "HTTP/1.1 200 ..."
    if (!sp) return -1;
    return atoi(sp + 1);
}

bool ChromeBridge::cdpReady() {
    std::string req = "GET /json/version HTTP/1.1\r\n"
                      "Host: 127.0.0.1:9222\r\nConnection: close\r\n\r\n";
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9222);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = false;
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0 &&
        send(fd, req.data(), req.size(), 0) >= 0) {
        char buf[512];
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            ok = strstr(buf, "200") != nullptr;
        }
    }
    close(fd);
    return ok;
}

bool ChromeBridge::cdpPut(const std::string& path) {
    std::string req = "PUT " + path +
        " HTTP/1.1\r\nHost: 127.0.0.1:9222\r\nContent-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    int code = cdpHttp(req);
    return code >= 200 && code < 300;
}

// GET /json/list (body có thể >512B nên đọc tới hết, không như cdpHttp
// chỉ đọc 1 recv). Trả body JSON, rỗng khi lỗi.
static std::string cdpGetBody(const std::string& path) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return "";
    struct timeval tv;
    tv.tv_sec = 4;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9222);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string resp;
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        std::string req = "GET " + path +
            " HTTP/1.1\r\nHost: 127.0.0.1:9222\r\n"
            "Connection: close\r\n\r\n";
        if (send(fd, req.data(), req.size(), 0) >= 0) {
            char buf[4096];
            ssize_t n;
            while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
                resp.append(buf, (size_t)n);
                if (resp.size() > 65536) break;  // list target không bao giờ to vậy
            }
        }
    }
    close(fd);
    if (resp.compare(0, 5, "HTTP/") != 0) return "";
    const char* sp = strchr(resp.c_str(), ' ');
    if (!sp || atoi(sp + 1) < 200 || atoi(sp + 1) >= 300) return "";
    size_t hdrEnd = resp.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) return "";
    return resp.substr(hdrEnd + 4);
}

// Thu id các target type=page trong body /json/list (format ổn định của
// Electron: ..."id":"HEX","title":"...","type":"page","url":"...").
static std::vector<std::string> cdpPageIds(const std::string& body,
                                           const std::string& wantUrl) {
    std::vector<std::string> ids;
    size_t pos = 0;
    while ((pos = body.find("\"id\":\"", pos)) != std::string::npos) {
        size_t s = pos + 6;
        size_t e = body.find('"', s);
        if (e == std::string::npos || e - s > 64) break;
        std::string id = body.substr(s, e - s);
        std::string window = body.substr(e, e + 2000 > body.size()
                                                ? std::string::npos
                                                : 2000);
        bool isPage = window.find("\"type\":\"page\"") != std::string::npos;
        bool urlOk = wantUrl.empty() ||
                     window.find("\"url\":\"" + wantUrl + "\"") !=
                         std::string::npos;
        if (isPage && urlOk && !id.empty()) ids.push_back(id);
        pos = e + 1;
    }
    return ids;
}

// Reaper nền: gọi định kỳ khi không phiên nào chạy để dọn weston/
// electron sót (launcher chết -9/OOM, trap cleanup không chạy).
// Không bao giờ chạm phiên đang chạy (caller kiểm isRunning trước).
void ChromeBridge::sweepStrays() {
    if (instance().isRunning()) return;
    killWebProcesses();
}

bool ChromeBridge::deepLink(const std::string& url) {
    if (url.empty()) return false;
    trace("B07", "deep-link (CDP PUT /json/new): " + url +
              " — phiên đang " +
              (m_ready.load() ? "sẵn sàng" : "CHƯA sẵn sàng"));
    // CDP /json/new?<URL>: chỉ khoảng trắng là bắt buộc thoát trong thực
    // tế dùng (ô địa chỉ của ta không sinh ký tự điều khiển).
    std::string enc;
    enc.reserve(url.size());
    for (char c : url) enc += (c == ' ') ? "%20" : std::string(1, c);
    // Chụp id các page đang có TRƯỚC khi mở mới: /json/new tạo target
    // mới mà không đóng cũ -> renderer tích lũy -> hết RAM -> freeze
    // (signal 9). Sau khi mở OK thì đóng các page cũ cùng URL trước đó.
    std::string oldUrl = m_currentUrl;
    std::vector<std::string> preIds =
        oldUrl.empty() ? std::vector<std::string>()
                       : cdpPageIds(cdpGetBody("/json/list"), oldUrl);
    bool ok = cdpPut("/json/new?" + enc);
    if (ok) {
        m_currentUrl = url;
        trace("B07", "deep-link OK");
        int closed = 0;
        for (const auto& id : preIds) {
            if (cdpPut("/json/close/" + id)) closed++;
        }
        if (closed > 0)
            trace("B07", "đã đóng " + std::to_string(closed) +
                          " page cũ (" + oldUrl + ")");
    } else {
        traceWarn("B07", "deep-link THẤT BẠI (CDP không trả 2xx): " + url);
    }
    return ok;
}

void ChromeBridge::render(SDL_Renderer*) {
    // Browser thật do weston hiển thị toàn màn hình; UIManager tự vẽ
    // màn chờ trong lúc boot nên ở đây không vẽ gì.
}

} // namespace RomCloud
