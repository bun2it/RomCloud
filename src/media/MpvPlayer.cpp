#include "MpvPlayer.h"
#include "../config/AppConfig.h"
#include "../logging/Logger.h"
#include "../platform/PlatformInfo.h"
#include <SDL2/SDL.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <cstdio>

namespace RomCloud {

MpvPlayer& MpvPlayer::instance() {
    static MpvPlayer inst;
    return inst;
}

std::string MpvPlayer::findBinary(const std::string& appRoot,
                                  const std::string& sdRoot) {
    std::vector<std::string> cands = {
        appRoot + "/bin/mpv", sdRoot + "/System/bin/mpv",
        "/usr/trimui/bin/mpv", "/usr/bin/mpv",
    };
    for (auto& c : cands) {
        if (access(c.c_str(), X_OK) == 0) return c;
    }
    return "";
}

std::string MpvPlayer::resolveOsdFont(const std::string& appRoot) {
    const std::string noto = appRoot + "/assets/fonts/NotoSans-Regular.ttf";
    if (access(noto.c_str(), R_OK) == 0) return noto;
    return appRoot + "/assets/fonts/font.ttf";
}

bool MpvPlayer::sendOneShot(const std::string& sock,
                            const std::string& json, std::string* response) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return false;
    struct sockaddr_un a; memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, sock.c_str(), sizeof(a.sun_path) - 1);
    struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 250000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) < 0) { close(s); return false; }
    std::string cmd = json + "\n";
    if (send(s, cmd.c_str(), cmd.length(), 0) < 0) { close(s); return false; }
    if (response) {
        char buf[2048]; ssize_t n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n > 0) { buf[n] = '\0'; *response = std::string(buf); }
    }
    close(s);
    return true;
}

bool MpvPlayer::sendPersistent(const std::string& json) {
    if (m_sock < 0) {
        if (access(m_sockPath.c_str(), F_OK) != 0) return false;
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s < 0) return false;
        struct sockaddr_un a; memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        strncpy(a.sun_path, m_sockPath.c_str(), sizeof(a.sun_path) - 1);
        if (connect(s, (struct sockaddr*)&a, sizeof(a)) < 0) { close(s); return false; }
        m_sock = s;
    }
    std::string cmd = json + "\n";
    ssize_t sent = send(m_sock, cmd.c_str(), cmd.length(), MSG_NOSIGNAL);
    if (sent != (ssize_t)cmd.length()) {
        close(m_sock); m_sock = -1; return false;
    }
    char drain[1024];
    while (recv(m_sock, drain, sizeof(drain), MSG_DONTWAIT) > 0) {}
    return true;
}

bool MpvPlayer::sendCmd(const std::string& json, std::string* response,
                        const std::string& sockOverride) {
    std::string target = sockOverride.empty() ? m_sockPath : sockOverride;
    if (response || !sockOverride.empty() || target != m_sockPath)
        return sendOneShot(target, json, response);
    if (sendPersistent(json)) return true;
    return sendOneShot(target, json, nullptr);
}
bool MpvPlayer::seekRelative(int seconds, const std::string& sockOverride) {
    return sendCmd("{\"command\":[\"seek\"," + std::to_string(seconds) +
                   ",\"relative\"]}", nullptr, sockOverride);
}

bool MpvPlayer::cyclePause(const std::string& sockOverride) {
    return sendCmd("{\"command\":[\"cycle\",\"pause\"]}", nullptr, sockOverride);
}

bool MpvPlayer::showOverlayIcon(const std::string& appRoot,
                                const std::string& iconName, unsigned dMs,
                                const std::string& sockOverride) {
    std::string raw = appRoot + "/assets/player_icons/" + iconName + ".raw";
    if (access(raw.c_str(), R_OK) != 0) return false;
    int sw = 1024, sh = 768; float ar = 4.0f / 3.0f;
    PlatformInfo::instance().getDisplayMetrics(sw, sh, ar);
    if (sw <= 0) sw = 1024; if (sh <= 0) sh = 768;
    const int iw = 128, ix = (sw - iw) / 2, iy = (sh - 128) / 2;
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "{\"command\":[\"overlay-add\",0,%d,%d,\"%s\",0,\"bgra\",%d,%d,%d]}",
        ix, iy, raw.c_str(), iw, 128, iw * 4);
    bool ok = sendCmd(cmd, nullptr, sockOverride);
    m_overlayExpireMs = SDL_GetTicks() + dMs;
    (void)ok;
    return true;
}

bool MpvPlayer::showText(const std::string& text, int ms,
                         const std::string& sockOverride) {
    return sendCmd("{\"command\":[\"show-text\",\"" + text + "\"," +
                   std::to_string(ms) + "]}", nullptr, sockOverride);
}

bool MpvPlayer::pollExited() {
    if (m_pid <= 0) return true;
    int status = 0;
    if (waitpid(m_pid, &status, WNOHANG) != 0) { m_pid = -1; return true; }
    if (m_overlayExpireMs && SDL_GetTicks() >= m_overlayExpireMs) {
        sendCmd("{\"command\":[\"overlay-remove\",0]}");
        m_overlayExpireMs = 0;
    }
    return false;
}

bool MpvPlayer::waitForSocket(int timeoutMs) {
    int waited = 0, status = 0;
    while (waited < timeoutMs) {
        if (m_pid > 0 && waitpid(m_pid, &status, WNOHANG) != 0) {
            m_pid = -1; return false;
        }
        if (access(m_sockPath.c_str(), F_OK) == 0) { SDL_Delay(150); return true; }
        SDL_Delay(100); waited += 100;
    }
    return access(m_sockPath.c_str(), F_OK) == 0;
}

bool MpvPlayer::play(const std::string& url,
                     const std::vector<std::string>& extraArgs,
                     const std::string& sockPath,
                     const std::string& logFile) {
    stop();
    if (url.empty()) return false;
    std::string appRoot = AppConfig::instance().getAppRoot();
    if (appRoot.empty()) appRoot = "/mnt/SDCARD/Apps/RomCloud";
    std::string sdRoot = AppConfig::instance().getSdRoot();
    std::string bin = findBinary(appRoot, sdRoot);
    if (bin.empty()) { Logger::error("MpvPlayer: no mpv binary"); return false; }
    m_sockPath = sockPath.empty() ? "/tmp/mpv_iptv.sock" : sockPath;
    unlink(m_sockPath.c_str());
    FILE* fw = fopen("/tmp/stay_awake", "w");
    if (fw) { fputs("1\n", fw); fclose(fw); }
    pid_t pid = fork();
    if (pid != 0) {
        if (pid < 0) return false;
        m_pid = pid;
        return waitForSocket(2500);
    }
    setpgid(0, 0);
    if (!logFile.empty()) {
        int fd = open(logFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
    }
    std::string libP = appRoot + "/lib:" + sdRoot + "/Emu/MEDIA/lib64:" +
        sdRoot + "/Emu/MEDIA/lib32:" + sdRoot + "/System/lib:/usr/lib:/lib";
    setenv("LD_LIBRARY_PATH", libP.c_str(), 1);
    setenv("HOME", appRoot.c_str(), 1);
    std::vector<std::string> args = { bin, url,
        "--input-ipc-server=" + m_sockPath, "--fullscreen", "--keepaspect=yes",
        "--hwdec=auto", "--vd-lavc-threads=4", "--framedrop=vo",
        "--terminal=no", "--tls-verify=no", "--osd-level=2" };
    std::string font = resolveOsdFont(appRoot);
    if (access(font.c_str(), R_OK) == 0) args.push_back("--osd-font=" + font);
    std::string ic = appRoot + "/config/input.conf";
    if (access(ic.c_str(), R_OK) == 0) args.push_back("--input-conf=" + ic);
    for (auto& e : extraArgs) args.push_back(e);
    std::vector<char*> cA;
    for (auto& a : args) cA.push_back(const_cast<char*>(a.c_str()));
    cA.push_back(nullptr);
    execv(bin.c_str(), cA.data());
    _exit(1);
}

bool MpvPlayer::stop() {
    if (m_sock >= 0) { close(m_sock); m_sock = -1; }
    if (m_pid > 0) {
        sendOneShot(m_sockPath, "{\"command\":[\"quit\"]}", nullptr);
        int status = 0; bool done = false;
        for (int i = 0; i < 15; ++i) {
            if (waitpid(m_pid, &status, WNOHANG) > 0) { done = true; break; }
            usleep(20000);
        }
        if (!done && kill(m_pid, 0) == 0) {
            kill(-m_pid, SIGTERM); kill(m_pid, SIGTERM);
            for (int i = 0; i < 20; ++i) {
                if (waitpid(m_pid, &status, WNOHANG) > 0) { done = true; break; }
                usleep(20000);
            }
        }
        if (!done && kill(m_pid, 0) == 0) {
            kill(-m_pid, SIGKILL); kill(m_pid, SIGKILL);
            waitpid(m_pid, &status, 0);
        }
        m_pid = -1;
    }
    unlink(m_sockPath.c_str());
    unlink("/tmp/stay_awake");
    m_overlayExpireMs = 0;
    return true;
}
} // namespace RomCloud
