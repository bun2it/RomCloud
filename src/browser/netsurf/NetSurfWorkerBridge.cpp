#include "NetSurfWorkerBridge.h"
#include "../../config/AppConfig.h"
#include "../../logging/Logger.h"
#include "../../network/HttpClient.h"
#include "../HtmlRenderer.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <SDL2/SDL_image.h>
#include <cctype>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>
#include <signal.h>
#include <cstring>
#include <algorithm>

namespace RomCloud {

NetSurfWorkerBridge& NetSurfWorkerBridge::instance() {
    static NetSurfWorkerBridge inst;
    return inst;
}

NetSurfWorkerBridge::NetSurfWorkerBridge() {
    m_lastRenderedSeq = 0xFFFFFFFF;
}

NetSurfWorkerBridge::~NetSurfWorkerBridge() {
    stop();
    if (m_viewportTexture != nullptr) {
        SDL_DestroyTexture(m_viewportTexture);
        m_viewportTexture = nullptr;
    }
}

bool NetSurfWorkerBridge::initShm() {
    cleanupShm();

    m_shmSize = sizeof(NetSurfShmHeader) + (VIEWPORT_W * VIEWPORT_H * 4);

    // Try normal file in /tmp first (standard on Linux/TrimUI)
    m_shmFd = open(NETSURF_SHM_FILE, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (m_shmFd < 0) {
        // Fallback to POSIX shm
        m_shmFd = shm_open(NETSURF_SHM_NAME, O_RDWR | O_CREAT | O_TRUNC, 0666);
    }

    if (m_shmFd < 0) {
        Logger::error("NetSurfWorkerBridge: Failed to create SHM file/segment");
        return false;
    }

    if (ftruncate(m_shmFd, (off_t)m_shmSize) != 0) {
        Logger::error("NetSurfWorkerBridge: Failed to ftruncate SHM to size: " + std::to_string(m_shmSize));
        close(m_shmFd);
        m_shmFd = -1;
        return false;
    }

    void* mem = mmap(NULL, m_shmSize, PROT_READ | PROT_WRITE, MAP_SHARED, m_shmFd, 0);
    if (mem == MAP_FAILED) {
        Logger::error("NetSurfWorkerBridge: Failed to mmap SHM buffer");
        close(m_shmFd);
        m_shmFd = -1;
        return false;
    }

    std::memset(mem, 0, m_shmSize);
    m_shmHdr = static_cast<NetSurfShmHeader*>(mem);
    m_shmHdr->magic = NETSURF_SHM_MAGIC;
    m_shmHdr->width = VIEWPORT_W;
    m_shmHdr->height = VIEWPORT_H;
    m_shmHdr->stride = VIEWPORT_W * 4;
    m_shmHdr->format = 0; // ARGB8888
    m_shmHdr->frame_seq = 0;
    m_shmHdr->event_write_idx = 0;
    m_shmHdr->event_read_idx = 0;
    m_shmHdr->ready = 0;
    m_shmHdr->quit_requested = 0;

    m_pixelBuffer = static_cast<uint8_t*>(mem) + sizeof(NetSurfShmHeader);
    // Initialize canvas to pure opaque white (0xFF) to prevent uninitialized memory
    std::memset(m_pixelBuffer, 0xFF, VIEWPORT_W * VIEWPORT_H * 4);
    Logger::info("NetSurfWorkerBridge: SHM initialized successfully (" + std::to_string(m_shmSize) + " bytes)");
    return true;
}

void NetSurfWorkerBridge::cleanupShm() {
    if (m_shmHdr != nullptr) {
        munmap(m_shmHdr, m_shmSize);
        m_shmHdr = nullptr;
        m_pixelBuffer = nullptr;
    }
    if (m_shmFd >= 0) {
        close(m_shmFd);
        m_shmFd = -1;
    }
    unlink(NETSURF_SHM_FILE);
    shm_unlink(NETSURF_SHM_NAME);
}

bool NetSurfWorkerBridge::start(const std::string& initialUrl) {
    if (isRunning()) {
        stop();
    }

    if (!initShm()) {
        return false;
    }

    m_currentUrl = initialUrl.empty() ? "https://google.com" : initialUrl;
    m_lastRenderedSeq = 0xFFFFFFFF;

    // Shim lazy-image: fetch + viết lại HTML cho daemon không-JS.
    // Trả về file:// đã xử lý, hoặc URL gốc khi fetch fail / không phải HTML.
    // (Block tối đa ~15s khi mạng xấu — chấp nhận ở v1, đọc full đo sức máy.)
    std::string daemonUrl = prepareDaemonUrl(m_currentUrl);

    // Detect netsurf binary path
    std::string binPath = "bin/netsurf";
    if (access("/mnt/SDCARD/Apps/RomCloud/bin/netsurf", X_OK) == 0) {
        binPath = "/mnt/SDCARD/Apps/RomCloud/bin/netsurf";
    } else if (access("bin/netsurf", X_OK) == 0) {
        binPath = "bin/netsurf";
    } else {
        // Check for netsurf in various locations
        const char* paths[] = {
            "/mnt/SDCARD/Apps/RomCloud/netsurf",
            "/mnt/SDCARD/netsurf",
            "./netsurf",
            "/usr/local/bin/netsurf",
            NULL
        };
        for (int i = 0; paths[i]; i++) {
            if (access(paths[i], X_OK) == 0) {
                binPath = paths[i];
                break;
            }
        }
    }

    // Verify binary exists and is executable
    if (access(binPath.c_str(), F_OK) != 0) {
        Logger::error("NetSurfWorkerBridge: Binary not found: " + binPath);
        cleanupShm();
        return false;
    }
    if (access(binPath.c_str(), X_OK) != 0) {
        Logger::error("NetSurfWorkerBridge: Binary not executable: " + binPath);
        cleanupShm();
        return false;
    }

    Logger::info("NetSurfWorkerBridge: Spawning " + binPath + " for URL: " + m_currentUrl);

    // P23: ensure persistent choices dir exists before fork so the child
    // can hand its full path to netsurf -f file:... .
    {
        std::string choicesDir = AppConfig::instance().getDataDir() +
                                 "/NetSurf";
        // mkdir -p: walk path components.
        for (size_t pos = 1; pos < choicesDir.size(); ++pos) {
            if (choicesDir[pos] == '/') {
                std::string sub = choicesDir.substr(0, pos);
                mkdir(sub.c_str(), 0700);  // ignore EEXIST
            }
        }
        mkdir(choicesDir.c_str(), 0700);
    }

    pid_t pid = fork();
    if (pid < 0) {
        Logger::error("NetSurfWorkerBridge: fork failed");
        cleanupShm();
        return false;
    }

    if (pid == 0) {
        // Child process: redirect stdout/stderr to /tmp/netsurf.log for diagnosis
        int logFd = open("/tmp/netsurf.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (logFd >= 0) {
            dup2(logFd, STDOUT_FILENO);
            dup2(logFd, STDERR_FILENO);
            close(logFd);
        }

        setenv("NETSURF_SHM", NETSURF_SHM_FILE, 1);
        const char* oldLd = getenv("LD_LIBRARY_PATH");
        std::string newLd = "/mnt/SDCARD/Apps/RomCloud/lib:lib:/mnt/SDCARD/System/lib:/usr/lib:/lib";
        if (oldLd) newLd += std::string(":") + oldLd;
        setenv("LD_LIBRARY_PATH", newLd.c_str(), 1);

        // Mobile-UA shim (LD_PRELOAD uname override): NetSurf dựng UA desktop
        // cứng, shim này đổi sysname -> "Linux; Android 13; Mobile" để site
        // trả bản mobile. Bỏ qua lặng lẽ nếu thiếu file.
        {
            std::string shim = "/mnt/SDCARD/Apps/RomCloud/lib/netsurf_ua_shim.so";
            if (access(shim.c_str(), R_OK) != 0) {
                std::string appRoot = AppConfig::instance().getAppRoot();
                if (!appRoot.empty())
                    shim = appRoot + "/lib/netsurf_ua_shim.so";
            }
            if (access(shim.c_str(), R_OK) == 0) {
                const char* oldPre = getenv("LD_PRELOAD");
                std::string newPre = shim;
                if (oldPre && *oldPre) {
                    newPre += ":";
                    newPre += oldPre;
                }
                setenv("LD_PRELOAD", newPre.c_str(), 1);
            }
        }

        if (access("/mnt/SDCARD/Apps/RomCloud/assets/netsurf", F_OK) == 0) {
            setenv("NETSURFRES", "/mnt/SDCARD/Apps/RomCloud/assets/netsurf/", 1);
        } else {
            setenv("NETSURFRES", "assets/netsurf/", 1);
        }

        // Font path cho NetSurf framebuffer
        setenv("NETSURF_FB_FONTPATH", "/mnt/SDCARD/Apps/RomCloud/assets/fonts:/usr/trimui/res", 1);

        char wStr[16], hStr[16];
        snprintf(wStr, sizeof(wStr), "%d", VIEWPORT_W);
        snprintf(hStr, sizeof(hStr), "%d", VIEWPORT_H);

        // NetSurf framebuffer nhận -f là tên surface frontend (bắt buộc là "ram" để kích hoạt SHM)
        execl(binPath.c_str(), "netsurf",
              "-v",
              "-f", "ram",
              "-b", "32",
              "-w", wStr,
              "-h", hStr,
              daemonUrl.c_str(),
              (char*)NULL);

        // If execl returns, it failed
        _exit(127);
    }

    // Parent process
    m_childPid = pid;
    Logger::info("NetSurfWorkerBridge: Spawned NetSurf daemon with PID " + std::to_string(m_childPid));

    // Wait for daemon readiness (up to 1000ms)
    for (int i = 0; i < 100; ++i) {
        if (m_shmHdr && m_shmHdr->ready == 1) {
            Logger::info("NetSurfWorkerBridge: NetSurf daemon confirmed ready in " + std::to_string(i * 10) + "ms");
            break;
        }
        usleep(10000);
    }

    return true;
}

void NetSurfWorkerBridge::stop() {
    if (m_childPid > 0) {
        Logger::info("NetSurfWorkerBridge: Stopping NetSurf daemon PID " + std::to_string(m_childPid));
        if (m_shmHdr) {
            m_shmHdr->quit_requested = 1;
            NetSurfShmEvent qEvent;
            qEvent.type = NETSURF_SHM_EVENT_CONTROL_QUIT;
            pushEvent(qEvent);
        }

        // Wait up to 300ms for clean exit
        int status = 0;
        bool exited = false;
        for (int i = 0; i < 30; ++i) {
            pid_t res = waitpid(m_childPid, &status, WNOHANG);
            if (res == m_childPid) {
                exited = true;
                break;
            }
            usleep(10000);
        }

        if (!exited) {
            Logger::warn("NetSurfWorkerBridge: NetSurf did not exit cleanly, sending SIGTERM");
            kill(m_childPid, SIGTERM);
            usleep(50000);
            waitpid(m_childPid, &status, WNOHANG);
        }

        m_childPid = -1;
    }

    // Keep m_viewportTexture allocated across page transitions to eliminate GPU VRAM reallocation flicker
    cleanupShm();
}

bool NetSurfWorkerBridge::isRunning() const {
    if (m_childPid <= 0) return false;
    int status = 0;
    pid_t res = waitpid(m_childPid, &status, WNOHANG);
    return (res == 0);
}

namespace {

// --- Lazy-image shim cho daemon (NetSurf build không JS: DUKTAPE=NO) ---
// Daemon tự fetch URL sẽ thấy <img> placeholder mà không bao giờ được swap.
// Shim fetch HTML trước qua HttpClient, viết lại data-src/srcset -> src,
// chèn <base>, lưu /tmp và đưa daemon URL file://.

std::string shimLower(std::string s) {
    for (char& c : s) c = (char)::tolower((unsigned char)c);
    return s;
}

std::string shimTrim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= ' ') b++;
    while (e > b && (unsigned char)s[e - 1] <= ' ') e--;
    return s.substr(b, e - b);
}

// Lấy giá trị attr (hỗ trợ nháy đôi/đơn/không nháy), tên attr case-insensitive.
std::string shimGetAttr(const std::string& tag, const std::string& name) {
    std::string low = shimLower(tag);
    std::string ln = shimLower(name);
    size_t p = low.find(ln);
    while (p != std::string::npos) {
        // Ranh giới tên: trước là space/</? và sau là space/=/>/
        bool preOk = (p == 0) || low[p - 1] == ' ' || low[p - 1] == '\t' ||
                     low[p - 1] == '\n' || low[p - 1] == '<';
        size_t q = p + ln.size();
        bool postOk = (q < low.size()) &&
                      (low[q] == ' ' || low[q] == '\t' || low[q] == '\n' ||
                       low[q] == '=' || low[q] == '/' || low[q] == '>');
        if (preOk && postOk) {
            size_t v = q;
            while (v < tag.size() &&
                   (tag[v] == ' ' || tag[v] == '\t' || tag[v] == '\n'))
                v++;
            if (v < tag.size() && tag[v] == '=') {
                v++;
                while (v < tag.size() &&
                       (tag[v] == ' ' || tag[v] == '\t' || tag[v] == '\n'))
                    v++;
                if (v < tag.size() && (tag[v] == '"' || tag[v] == '\'')) {
                    char qc = tag[v++];
                    size_t e = tag.find(qc, v);
                    if (e == std::string::npos) e = tag.size();
                    return tag.substr(v, e - v);
                }
                size_t e = v;
                while (e < tag.size() && tag[e] != ' ' && tag[e] != '\t' &&
                       tag[e] != '\n' && tag[e] != '>' && tag[e] != '/')
                    e++;
                return tag.substr(v, e - v);
            }
            return ""; // attr boolean
        }
        p = low.find(ln, p + 1);
    }
    return "";
}

// Thay src hiện có (giữ kiểu nháy) hoặc chèn src mới trước '>' / '/>'.
std::string shimSetSrc(const std::string& tag, const std::string& newSrc) {
    std::string low = shimLower(tag);
    size_t p = low.find("src");
    while (p != std::string::npos) {
        bool preOk = (p == 0) || low[p - 1] == ' ' || low[p - 1] == '\t' ||
                     low[p - 1] == '\n' || low[p - 1] == '<';
        // Tránh nhầm data-src/srcset: "src" phải đứng riêng, không dính liền chữ
        size_t q = p + 3;
        bool postOk = (q < low.size()) &&
                      (low[q] == ' ' || low[q] == '\t' || low[q] == '\n' ||
                       low[q] == '=' || low[q] == '/' || low[q] == '>');
        if (preOk && postOk) {
            size_t v = q;
            while (v < tag.size() &&
                   (tag[v] == ' ' || tag[v] == '\t' || tag[v] == '\n'))
                v++;
            if (v < tag.size() && tag[v] == '=') {
                v++;
                while (v < tag.size() &&
                       (tag[v] == ' ' || tag[v] == '\t' || tag[v] == '\n'))
                    v++;
                if (v < tag.size() && (tag[v] == '"' || tag[v] == '\'')) {
                    char qc = tag[v];
                    size_t e = tag.find(qc, v + 1);
                    if (e == std::string::npos) return tag;
                    std::string out = tag;
                    out.replace(v + 1, e - v - 1, newSrc);
                    return out;
                }
                size_t e = v;
                while (e < tag.size() && tag[e] != ' ' && tag[e] != '\t' &&
                       tag[e] != '\n' && tag[e] != '>' && tag[e] != '/')
                    e++;
                std::string out = tag;
                out.replace(v, e - v, newSrc);
                return out;
            }
        }
        p = low.find("src", p + 1);
    }
    // Không có src: chèn trước '>' cuối (tôn trọng '/>')
    size_t gt = tag.rfind('>');
    if (gt == std::string::npos) return tag;
    std::string out = tag;
    out.insert(gt, " src=\"" + newSrc + "\"");
    return out;
}

// URL đầu tiên trong srcset ("u1 500w, u2 1000w" -> "u1").
std::string shimFirstSrcset(const std::string& srcset) {
    std::string s = shimTrim(srcset);
    size_t comma = s.find(',');
    std::string first = (comma == std::string::npos) ? s : s.substr(0, comma);
    first = shimTrim(first);
    size_t sp = first.find(' ');
    if (sp != std::string::npos) first = first.substr(0, sp);
    return shimTrim(first);
}

struct ShimBase {
    std::string schemeHost; // "https://host"
    std::string dir;        // "https://host/dir/"
};

ShimBase shimSplitBase(const std::string& base) {
    ShimBase b;
    size_t pe = base.find("://");
    size_t hs = (pe == std::string::npos) ? 0 : pe + 3;
    size_t he = base.find('/', hs);
    b.schemeHost = (he == std::string::npos) ? base : base.substr(0, he);
    std::string noQ = base;
    size_t qq = noQ.find_first_of("?#");
    if (qq != std::string::npos) noQ = noQ.substr(0, qq);
    size_t ls = noQ.rfind('/');
    b.dir = (ls == std::string::npos) ? base + "/" : noQ.substr(0, ls + 1);
    return b;
}

std::string shimAbs(const ShimBase& b, const std::string& ref) {
    std::string r = shimTrim(ref);
    if (r.empty()) return r;
    if (r.compare(0, 7, "http://") == 0 || r.compare(0, 8, "https://") == 0 ||
        r.compare(0, 5, "data:") == 0 || r.compare(0, 5, "blob:") == 0)
        return r;
    if (r.compare(0, 2, "//") == 0) return "https:" + r;
    if (!r.empty() && r[0] == '/') return b.schemeHost + r;
    return b.dir + r;
}

bool shimSrcBad(const std::string& src) {
    if (src.empty()) return true;
    std::string l = shimLower(src);
    if (l.compare(0, 5, "data:") == 0 || l.compare(0, 5, "blob:") == 0)
        return true;
    static const char* kJunk[] = {"blank", "placeholder", "1x1", "transparent",
                                  "grey", "gray", "pixel", "spacer", "lazyload"};
    for (const char* k : kJunk) {
        if (l.find(k) != std::string::npos) return true;
    }
    return false;
}

// NetSurf 3.11 không có CSS grid: mọi item grid chồng cùng vị trí.
// Viết lại display:grid -> block, inline-grid -> inline-block để xếp dọc đọc
// được. Chỉ khớp khi "display" đứng ở vị trí property (sau { ; } hoặc đầu,
// kế tiếp là ':') nên không chạm selector dạng .display-grid.
std::string shimDegridCss(const std::string& css) {
    std::string low = shimLower(css);
    std::string out;
    out.reserve(css.size());
    size_t i = 0;
    while (i < css.size()) {
        size_t d = low.find("display", i);
        if (d == std::string::npos) {
            out += css.substr(i);
            break;
        }
        char prev = (d == 0) ? '{' : low[d - 1];
        size_t q = d + 7;
        while (q < low.size() &&
               (low[q] == ' ' || low[q] == '\t' || low[q] == '\n'))
            q++;
        bool propPos = (prev == '{' || prev == ';' || prev == '}' ||
                        prev == ' ' || prev == '\t' || prev == '\n') &&
                       q < low.size() && low[q] == ':';
        if (!propPos) {
            out += css.substr(i, d - i + 1);
            i = d + 1;
            continue;
        }
        size_t v = q + 1;
        while (v < low.size() &&
               (low[v] == ' ' || low[v] == '\t' || low[v] == '\n'))
            v++;
        if (low.compare(v, 11, "inline-grid") == 0) {
            out += css.substr(i, v - i);
            out += "inline-block";
            i = v + 11;
        } else if (low.compare(v, 4, "grid") == 0) {
            size_t after = v + 4;
            bool boundary = (after >= low.size()) ||
                            (low[after] != '-' &&
                             !(low[after] >= 'a' && low[after] <= 'z') &&
                             !(low[after] >= '0' && low[after] <= '9'));
            if (boundary) {
                out += css.substr(i, v - i);
                out += "block";
                i = v + 4;
            } else {
                out += css.substr(i, d - i + 1);
                i = d + 1;
            }
        } else {
            out += css.substr(i, d - i + 1);
            i = d + 1;
        }
    }
    return out;
}

// Đặt/thay attr bất kỳ trong tag (giữ kiểu nháy), dùng cho style="...".
std::string shimSetAttr(const std::string& tag, const std::string& name,
                        const std::string& value) {
    std::string low = shimLower(tag);
    std::string ln = shimLower(name);
    size_t p = low.find(ln);
    while (p != std::string::npos) {
        bool preOk = (p == 0) || low[p - 1] == ' ' || low[p - 1] == '\t' ||
                     low[p - 1] == '\n' || low[p - 1] == '<';
        size_t q = p + ln.size();
        bool postOk = (q < low.size()) &&
                      (low[q] == ' ' || low[q] == '\t' || low[q] == '\n' ||
                       low[q] == '=' || low[q] == '/' || low[q] == '>');
        if (preOk && postOk) {
            size_t v = q;
            while (v < tag.size() &&
                   (tag[v] == ' ' || tag[v] == '\t' || tag[v] == '\n'))
                v++;
            if (v < tag.size() && tag[v] == '=') {
                v++;
                while (v < tag.size() &&
                       (tag[v] == ' ' || tag[v] == '\t' || tag[v] == '\n'))
                    v++;
                if (v < tag.size() && (tag[v] == '"' || tag[v] == '\'')) {
                    char qc = tag[v];
                    size_t e = tag.find(qc, v + 1);
                    if (e == std::string::npos) return tag;
                    std::string out = tag;
                    out.replace(v + 1, e - v - 1, value);
                    return out;
                }
            }
        }
        p = low.find(ln, p + 1);
    }
    size_t gt = tag.rfind('>');
    if (gt == std::string::npos) return tag;
    std::string out = tag;
    out.insert(gt, " " + name + "=\"" + value + "\"");
    return out;
}

// Xóa cache webp->png của trang trước (ns_img_*.png trong /tmp).
void shimCleanupWebpCache() {
    DIR* dir = opendir("/tmp");
    if (!dir) return;
    struct dirent* e;
    while ((e = readdir(dir)) != nullptr) {
        std::string n = e->d_name ? e->d_name : "";
        if (n.compare(0, 7, "ns_img_") == 0) {
            std::string suf = n.substr(n.size() > 4 ? n.size() - 4 : 0);
            for (char& c : suf) c = (char)::tolower((unsigned char)c);
            if (suf == ".png") unlink(("/tmp/" + n).c_str());
        }
    }
    closedir(dir);
}

// NetSurf build tắt WEBP: tải webp về, decode bằng SDL_image, lưu PNG /tmp,
// trả về URL file://. Tối đa 12 ảnh/trang, 2MB/ảnh.
std::string shimTranscodeWebp(const std::string& url, int& counter) {
    if (counter >= 12) return "";
    std::string pathOnly = url;
    size_t qq = pathOnly.find_first_of("?#");
    if (qq != std::string::npos) pathOnly = pathOnly.substr(0, qq);
    std::string lp = shimLower(pathOnly);
    if (lp.size() < 5 || lp.compare(lp.size() - 5, 5, ".webp") != 0) return "";
    HttpResponse ir;
    try {
        ir = HttpClient::instance().get(
            url,
            {"User-Agent: Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 "
             "(KHTML, like Gecko) Chrome/124.0.0.0 Mobile Safari/537.36"},
            8);
    } catch (...) {
        return "";
    }
    if (!ir.success || ir.body.empty() || ir.body.size() > 2 * 1024 * 1024)
        return "";
    SDL_Surface* surf = nullptr;
    {
        SDL_RWops* rw =
            SDL_RWFromConstMem(ir.body.data(), (int)ir.body.size());
        if (rw) {
            surf = IMG_Load_RW(rw, 1);
        }
    }
    if (!surf) return "";
    char outPath[64];
    snprintf(outPath, sizeof(outPath), "/tmp/ns_img_%d.png", counter);
    int rc = IMG_SavePNG(surf, outPath);
    SDL_FreeSurface(surf);
    if (rc != 0) {
        unlink(outPath);
        return "";
    }
    counter++;
    return std::string("file://") + outPath;
}

} // namespace

std::string NetSurfWorkerBridge::prepareDaemonUrl(const std::string& url) {
    static const char* kShimFile = "/tmp/romcloud_ns_page.html";
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0)
        return url; // file://, about:, ...: daemon tự xử
    HttpResponse resp;
    try {
        resp = HttpClient::instance().get(url, HtmlRenderer::getBrowserHeaders(), 15);
    } catch (...) {
        return url;
    }
    if (!resp.success || resp.body.empty())
        return url;
    // Đọc full: không cap 2MB ở đây nữa (HttpClient đã chặn 10MB).
    // Đo khả năng giải mã của máy trước, phần thừa sẽ tắt dần sau.
    std::string head = shimLower(resp.body.substr(0, 4096));
    if (head.find("<html") == std::string::npos &&
        head.find("<head") == std::string::npos &&
        head.find("<body") == std::string::npos &&
        head.find("<div") == std::string::npos &&
        head.find("<!doctype") == std::string::npos)
        return url; // không phải HTML

    std::string base = resp.effectiveUrl.empty() ? url : resp.effectiveUrl;
    ShimBase b = shimSplitBase(base);
    std::string html = resp.body;
    shimCleanupWebpCache();

    // Pass 0: grid -> block trong mọi <style> (NetSurf 3.11 không có grid,
    // item chồng cùng vị trí). Chỉ xử lý nội dung style, không chạm text.
    int degridBlocks = 0;
    {
        size_t sp = 0;
        while (sp < html.size()) {
            std::string low7 = shimLower(html.substr(sp, 7));
            size_t st = low7.find("<style");
            if (st == std::string::npos) break;
            st += sp;
            // Tìm '>' của thẻ mở style
            bool sq = false, dq = false;
            size_t go = st + 1;
            for (; go < html.size(); ++go) {
                char c = html[go];
                if (c == '\'' && !dq) sq = !sq;
                else if (c == '"' && !sq) dq = !dq;
                else if (c == '>' && !sq && !dq) break;
            }
            if (go >= html.size()) break;
            size_t ce = shimLower(html.substr(go)).find("</style>");
            if (ce == std::string::npos) break;
            ce += go;
            std::string inner = html.substr(go + 1, ce - go - 1);
            std::string fixed_css = shimDegridCss(inner);
            if (fixed_css != inner) {
                html.replace(go + 1, ce - go - 1, fixed_css);
                degridBlocks++;
                sp = go + 1 + fixed_css.size();
            } else {
                sp = ce + 8;
            }
        }
    }

    static const char* kLazyAttrs[] = {"data-src",     "data-original",
                                       "data-lazy-src", "data-actualsrc",
                                       "data-url",      "data-hi-res-src",
                                       "data-full-src", "data-src-retina",
                                       "data-echo",     "data-lazy",
                                       "data-orig",     "data-img"};
    int fixed = 0;
    int webpCount = 0;
    size_t pos = 0;
    while (pos < html.size()) {
        size_t lt = html.find('<', pos);
        if (lt == std::string::npos) break;
        std::string low4 = shimLower(html.substr(lt, 7));
        bool isImg = (low4.compare(0, 4, "<img") == 0);
        bool isSrc = (low4.compare(0, 7, "<source") == 0);
        if (!isImg && !isSrc) {
            pos = lt + 1;
            continue;
        }
        // Tìm '>' tôn trọng nháy đơn/đôi
        bool sq = false, dq = false;
        size_t gt = lt + 1;
        for (; gt < html.size(); ++gt) {
            char c = html[gt];
            if (c == '\'' && !dq) sq = !sq;
            else if (c == '"' && !sq) dq = !dq;
            else if (c == '>' && !sq && !dq) break;
        }
        if (gt >= html.size()) break;
        std::string tag = html.substr(lt, gt - lt + 1);
        std::string src = shimGetAttr(tag, "src");
        std::string cand;
        if (shimSrcBad(src)) {
            for (const char* a : kLazyAttrs) {
                cand = shimGetAttr(tag, a);
                if (!cand.empty() && cand.compare(0, 5, "data:") != 0) break;
                cand.clear();
            }
            if (cand.empty()) {
                std::string ss = shimGetAttr(tag, "srcset");
                if (!ss.empty()) cand = shimFirstSrcset(ss);
            }
        }
        if (!cand.empty()) {
            std::string abs = shimAbs(b, cand);
            if (!abs.empty() && abs != src) {
                tag = shimSetSrc(tag, abs);
                src = abs;
                fixed++;
            }
        }
        // style="display:grid..." ngay trên thẻ -> block (chống overlap)
        {
            std::string st = shimGetAttr(tag, "style");
            if (!st.empty()) {
                std::string dg = shimDegridCss(st);
                if (dg != st) {
                    tag = shimSetAttr(tag, "style", dg);
                    fixed++;
                }
            }
        }
        // webp NetSurf không decode -> tải + transcode PNG /tmp
        if (!src.empty()) {
            std::string png = shimTranscodeWebp(shimAbs(b, src), webpCount);
            if (!png.empty()) {
                tag = shimSetSrc(tag, png);
                fixed++;
            }
        }
        std::string rebuilt = tag;
        if (rebuilt != html.substr(lt, gt - lt + 1)) {
            html.replace(lt, gt - lt + 1, rebuilt);
            pos = lt + 4;
            continue;
        }
        pos = gt + 1;
        // Đọc full: không giới hạn số thẻ xử lý (đo sức máy trước).
    }

    // Chèn <base> để resource tương đối còn lại resolve đúng.
    // Đồng thời GIỮ CHARSET: file:// không có HTTP header nên NetSurf mất
    // charset gốc -> NSERROR_BAD_ENCODING với trang chỉ khai báo charset qua
    // header HTTP. Nếu HTML chưa có meta charset mà HTTP có -> chèn thêm.
    std::string httpCharset;
    for (const auto& kv : resp.headers) {
        if (shimLower(kv.first) == "content-type") {
            std::string vlow = shimLower(kv.second);
            size_t cp = vlow.find("charset=");
            if (cp != std::string::npos) {
                httpCharset = shimTrim(kv.second.substr(cp + 8));
                while (!httpCharset.empty() &&
                       (httpCharset.back() == '"' || httpCharset.back() == '\'' ||
                        httpCharset.back() == ';' || httpCharset.back() == ' '))
                    httpCharset.pop_back();
                break;
            }
        }
    }
    {
        std::string lowHtml = shimLower(html.substr(0, 8192));
        // Hubbub chỉ quét 512 byte đầu tìm meta charset (libhubbub
        // charset/detect.c: scan_meta end = pos + min(512, len)). VnExpress
        // đặt meta ở byte ~666 -> miss -> NSERROR_BAD_ENCODING qua file://
        // (không còn HTTP header). Nên chỉ cần 512B đầu thiếu là chèn.
        std::string first512 = shimLower(html.substr(0, 512));
        std::string metaTag;
        if (!httpCharset.empty() && first512.find("charset") == std::string::npos)
            metaTag = "<meta charset=\"" + httpCharset + "\">";
        std::string inject = metaTag.empty()
                                 ? "<base href=\"" + base + "\">"
                                 : "<base href=\"" + base + "\">" + metaTag;
        size_t hp = lowHtml.find("<head");
        if (hp != std::string::npos) {
            size_t he = html.find('>', hp);
            if (he != std::string::npos && he < 8192) {
                html.insert(he + 1, inject);
                if (!metaTag.empty())
                    Logger::info("NetSurfWorkerBridge: shim kept HTTP charset " +
                                 httpCharset + " for " + base);
            }
        } else {
            size_t xp = lowHtml.find("<html");
            if (xp != std::string::npos) {
                size_t xe = html.find('>', xp);
                if (xe != std::string::npos && xe < 8192)
                    html.insert(xe + 1, "<head>" + inject + "</head>");
            }
        }
    }

    std::ofstream out(kShimFile, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return url;
    out.write(html.data(), (std::streamsize)html.size());
    out.close();
    Logger::info("NetSurfWorkerBridge: shim HTML (" +
                 std::to_string(html.size()) + " bytes, " +
                 std::to_string(fixed) + " ảnh fixed, " +
                 std::to_string(webpCount) + " webp->png, " +
                 std::to_string(degridBlocks) + " style degrid) -> " + kShimFile);
    return std::string("file://") + kShimFile;
}

void NetSurfWorkerBridge::pushEvent(const NetSurfShmEvent& event) {
    if (!m_shmHdr) return;
    uint32_t writeIdx = m_shmHdr->event_write_idx;
    uint32_t ringPos = writeIdx % NETSURF_EVENT_RING_SIZE;
    m_shmHdr->events[ringPos] = event;
    m_shmHdr->event_write_idx = writeIdx + 1;
}

void NetSurfWorkerBridge::sendMouseMove(int x, int y) {
    NetSurfShmEvent ev;
    ev.type = NETSURF_SHM_EVENT_MOVE_ABS;
    ev.x = std::clamp(x, 0, VIEWPORT_W - 1);
    ev.y = std::clamp(y, 0, VIEWPORT_H - 1);
    ev.keycode = 0;
    pushEvent(ev);
}

void NetSurfWorkerBridge::sendClick(int x, int y, int mouseButton) {
    sendMouseMove(x, y);

    NetSurfShmEvent down;
    down.type = NETSURF_SHM_EVENT_KEY_DOWN;
    down.keycode = mouseButton;
    down.x = x;
    down.y = y;
    pushEvent(down);

    NetSurfShmEvent up;
    up.type = NETSURF_SHM_EVENT_KEY_UP;
    up.keycode = mouseButton;
    up.x = x;
    up.y = y;
    pushEvent(up);
}

void NetSurfWorkerBridge::scrollBy(int deltaY) {
    int button = (deltaY < 0) ? NS_SHM_KEY_MOUSE_4 : NS_SHM_KEY_MOUSE_5;
    int steps = std::max(1, std::abs(deltaY) / 40);
    for (int i = 0; i < steps; ++i) {
        sendClick(m_cursorX, m_cursorY, button);
    }
}

bool NetSurfWorkerBridge::handleInput() {
    InputManager& input = InputManager::instance();

    // Note: SELECT button mode toggle is handled upstream by UIManager with toast notifications.

    // L1 / R1 smooth fast scroll
    if (input.isButtonJustPressed(Button::L1) || input.isButtonRepeat(Button::L1)) {
        scrollBy(-120);
        return true;
    }
    if (input.isButtonJustPressed(Button::R1) || input.isButtonRepeat(Button::R1)) {
        scrollBy(120);
        return true;
    }

    // L2 / R2 large page scroll
    if (input.isButtonJustPressed(Button::L2)) {
        scrollBy(-350);
        return true;
    }
    if (input.isButtonJustPressed(Button::R2)) {
        scrollBy(350);
        return true;
    }

    static uint32_t lastScrollMs = 0;
    static int moveHeldFrames = 0;
    uint32_t now = SDL_GetTicks();

    if (m_cursorMode) {
        // Pointer navigation via D-Pad with progressive acceleration
        int dx = 0;
        int dy = 0;

        if (input.isButtonPressed(Button::UP)) dy -= 1;
        if (input.isButtonPressed(Button::DOWN)) dy += 1;
        if (input.isButtonPressed(Button::LEFT)) dx -= 1;
        if (input.isButtonPressed(Button::RIGHT)) dx += 1;

        if (dx != 0 || dy != 0) {
            moveHeldFrames++;
            int speed = (moveHeldFrames > 18) ? 16 : (moveHeldFrames > 8 ? 11 : 7);
            m_cursorX = std::clamp(m_cursorX + (dx * speed), 0, VIEWPORT_W - 1);
            m_cursorY = std::clamp(m_cursorY + (dy * speed), 0, VIEWPORT_H - 1);
            sendMouseMove(m_cursorX, m_cursorY);

            // Edge auto-scrolling when pointer reaches top or bottom
            if (m_cursorY <= 20 && dy < 0) {
                if (now - lastScrollMs > 40) {
                    scrollBy(-40);
                    lastScrollMs = now;
                }
            } else if (m_cursorY >= VIEWPORT_H - 25 && dy > 0) {
                if (now - lastScrollMs > 40) {
                    scrollBy(40);
                    lastScrollMs = now;
                }
            }
            return true;
        } else {
            moveHeldFrames = 0;
        }

        // Button A: Click / Select
        if (input.isButtonJustPressed(Button::A)) {
            sendClick(m_cursorX, m_cursorY, NS_SHM_KEY_MOUSE_1);
            return true;
        }
    } else {
        // Scroll / Viewport Navigation mode via D-Pad (accelerating scroll + horizontal positioning)
        static int scrollHeldFrames = 0;
        int scrollDir = 0;
        if (input.isButtonPressed(Button::UP)) scrollDir = -1;
        if (input.isButtonPressed(Button::DOWN)) scrollDir = 1;

        if (scrollDir != 0) {
            scrollHeldFrames++;
            int scrollStep = (scrollHeldFrames > 15) ? 120 : (scrollHeldFrames > 6 ? 75 : 40);
            if (now - lastScrollMs > 35) {
                scrollBy(scrollDir * scrollStep);
                lastScrollMs = now;
            }
            return true;
        } else {
            scrollHeldFrames = 0;
        }

        // D-Pad Left/Right positions the targeting reticle horizontally across content
        if (input.isButtonPressed(Button::LEFT)) {
            m_cursorX = std::clamp(m_cursorX - 14, 16, VIEWPORT_W - 16);
            sendMouseMove(m_cursorX, m_cursorY);
            return true;
        }
        if (input.isButtonPressed(Button::RIGHT)) {
            m_cursorX = std::clamp(m_cursorX + 14, 16, VIEWPORT_W - 16);
            sendMouseMove(m_cursorX, m_cursorY);
            return true;
        }

        // Button A in scroll/navigation mode clicks at current target position
        if (input.isButtonJustPressed(Button::A)) {
            sendClick(m_cursorX, m_cursorY, NS_SHM_KEY_MOUSE_1);
            return true;
        }
    }

    return false;
}

void NetSurfWorkerBridge::drawVirtualCursor(SDL_Renderer* renderer, int screenX, int screenY) {
    // Draw modern sleek pointer arrow (cyan accent with dark border)
    SDL_Color accent = {0, 220, 240, 255};
    SDL_Color border = {20, 20, 30, 255};

    // Draw shadow/outline first
    SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, border.a);
    for (int o = -1; o <= 1; ++o) {
        SDL_RenderDrawLine(renderer, screenX + o, screenY + o, screenX + 16 + o, screenY + 12 + o);
        SDL_RenderDrawLine(renderer, screenX + o, screenY + o, screenX + 6 + o, screenY + 18 + o);
        SDL_RenderDrawLine(renderer, screenX + 6 + o, screenY + 18 + o, screenX + 16 + o, screenY + 12 + o);
    }

    // Draw filled inner arrow
    SDL_SetRenderDrawColor(renderer, accent.r, accent.g, accent.b, accent.a);
    SDL_RenderDrawLine(renderer, screenX, screenY, screenX + 15, screenY + 11);
    SDL_RenderDrawLine(renderer, screenX, screenY, screenX + 5, screenY + 17);
    SDL_RenderDrawLine(renderer, screenX + 5, screenY + 17, screenX + 15, screenY + 11);

    // Cross-point dot for precise pointing
    SDL_Rect tip = {screenX - 1, screenY - 1, 3, 3};
    SDL_RenderFillRect(renderer, &tip);
}

void NetSurfWorkerBridge::drawTargetReticle(SDL_Renderer* renderer, int screenX, int screenY) {
    // Elegant aiming reticle for D-Pad viewport navigation
    SDL_Color ringCol = {0, 200, 240, 200};
    SDL_Color shadowCol = {15, 15, 25, 180};

    // Outer shadow ring (radius ~9)
    SDL_SetRenderDrawColor(renderer, shadowCol.r, shadowCol.g, shadowCol.b, shadowCol.a);
    SDL_Rect shadowBox = {screenX - 9, screenY - 9, 19, 19};
    SDL_RenderDrawRect(renderer, &shadowBox);

    // Inner reticle box (radius ~7)
    SDL_SetRenderDrawColor(renderer, ringCol.r, ringCol.g, ringCol.b, ringCol.a);
    SDL_Rect ringBox = {screenX - 8, screenY - 8, 17, 17};
    SDL_RenderDrawRect(renderer, &ringBox);

    // 4 crosshair ticks (top, bottom, left, right)
    SDL_RenderDrawLine(renderer, screenX, screenY - 12, screenX, screenY - 5);
    SDL_RenderDrawLine(renderer, screenX, screenY + 5, screenX, screenY + 12);
    SDL_RenderDrawLine(renderer, screenX - 12, screenY, screenX - 5, screenY);
    SDL_RenderDrawLine(renderer, screenX + 5, screenY, screenX + 12, screenY);

    // Center dot (bright white)
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_Rect dot = {screenX - 1, screenY - 1, 3, 3};
    SDL_RenderFillRect(renderer, &dot);
}

void NetSurfWorkerBridge::render(SDL_Renderer* renderer) {
    if (!renderer) return;

    // If SHM is gone (NetSurf crashed or stopped), ensure clean white screen
    if (m_shmHdr == nullptr || m_pixelBuffer == nullptr) {
        // Force white texture to eliminate any stale GPU artifacts (black/white diagonal stripes)
        if (m_viewportTexture != nullptr) {
            // Pre-fill with solid white to eliminate Mali GPU tiled memory artifacts
            uint32_t* whiteBuf = new uint32_t[VIEWPORT_W * VIEWPORT_H];
            for (int i = 0; i < VIEWPORT_W * VIEWPORT_H; ++i) {
                whiteBuf[i] = 0xFFFFFFFF; // ARGB white
            }
            SDL_UpdateTexture(m_viewportTexture, NULL, whiteBuf, VIEWPORT_W * 4);
            delete[] whiteBuf;
        }

        // Draw white background directly
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_Rect dstRect = {VIEWPORT_X, VIEWPORT_Y, VIEWPORT_W, VIEWPORT_H};
        SDL_RenderFillRect(renderer, &dstRect);
        return;
    }

    if (m_viewportTexture == nullptr || m_lastRenderer != renderer) {
        if (m_viewportTexture != nullptr) {
            SDL_DestroyTexture(m_viewportTexture);
        }
        m_viewportTexture = SDL_CreateTexture(renderer,
                                              SDL_PIXELFORMAT_ARGB8888,
                                              SDL_TEXTUREACCESS_STREAMING,
                                              VIEWPORT_W, VIEWPORT_H);
        if (m_viewportTexture != nullptr) {
            // CRITICAL: Disable blend mode so 0x00 in byte 3 is never treated as transparent
            SDL_SetTextureBlendMode(m_viewportTexture, SDL_BLENDMODE_NONE);
            // Immediately fill texture to eliminate Mali GPU tiled uninitialized memory artifacts (black & white diagonal stripes)
            if (m_pixelBuffer != nullptr) {
                SDL_UpdateTexture(m_viewportTexture, NULL, m_pixelBuffer, VIEWPORT_W * 4);
            }
        }
        m_lastRenderer = renderer;
    }

    // Update texture if new frame rendered by NetSurf
    if (m_lastRenderedSeq != m_shmHdr->frame_seq) {
        // Enforce exact viewport row pitch (VIEWPORT_W * 4 = 4096)
        SDL_UpdateTexture(m_viewportTexture, NULL, m_pixelBuffer, VIEWPORT_W * 4);
        m_lastRenderedSeq = m_shmHdr->frame_seq;
    }

    SDL_Rect dstRect = {VIEWPORT_X, VIEWPORT_Y, VIEWPORT_W, VIEWPORT_H};
    SDL_RenderCopy(renderer, m_viewportTexture, NULL, &dstRect);

    // Draw cursor based on active mode
    if (m_cursorMode) {
        drawVirtualCursor(renderer, VIEWPORT_X + m_cursorX, VIEWPORT_Y + m_cursorY);
    } else {
        drawTargetReticle(renderer, VIEWPORT_X + m_cursorX, VIEWPORT_Y + m_cursorY);
    }
}

} // namespace RomCloud
