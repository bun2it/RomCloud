#pragma once
// P1-3: MpvPlayer — fork mpv + IPC dung chung cho IPTV/YouTube.
// IPTVManager chi cung cap URL + mapping nut; khong fork truc tiep.
#include <string>
#include <vector>
#include <sys/types.h>

namespace RomCloud {

class MpvPlayer {
public:
    static MpvPlayer& instance();

    // Bat mpv moi cho url. extraArgs them sau flags handheld chuan.
    // sockPath: /tmp/mpv_iptv.sock | /tmp/mpv_youtube.sock ...
    bool play(const std::string& url,
              const std::vector<std::string>& extraArgs = {},
              const std::string& sockPath = "/tmp/mpv_iptv.sock",
              const std::string& logFile = "");
    bool isPlaying() const { return m_pid > 0; }
    pid_t pid() const { return m_pid; }
    const std::string& sockPath() const { return m_sockPath; }

    // Gui lenh JSON IPC. sockOverride "" = dung m_sockPath hien tai.
    bool sendCmd(const std::string& json,
                 std::string* response = nullptr,
                 const std::string& sockOverride = "");
    bool showText(const std::string& text, int ms,
                  const std::string& sockOverride = "");
    bool seekRelative(int seconds, const std::string& sockOverride = "");
    bool cyclePause(const std::string& sockOverride = "");
    bool showOverlayIcon(const std::string& appRoot,
                         const std::string& iconName, unsigned durationMs,
                         const std::string& sockOverride = "");

    // true neu tien trinh da thoat (da reap, m_pid=-1).
    bool pollExited();
    bool waitForSocket(int timeoutMs = 2500);
    bool stop();

    static std::string findBinary(const std::string& appRoot,
                                  const std::string& sdRoot);
    static std::string resolveOsdFont(const std::string& appRoot);

private:
    MpvPlayer() = default;
    ~MpvPlayer() { stop(); }
    MpvPlayer(const MpvPlayer&) = delete;
    MpvPlayer& operator=(const MpvPlayer&) = delete;

    static bool sendOneShot(const std::string& sock,
                            const std::string& json, std::string* response);
    bool sendPersistent(const std::string& json);

    pid_t m_pid = -1;
    std::string m_sockPath = "/tmp/mpv_iptv.sock";
    int m_sock = -1;
    unsigned m_overlayExpireMs = 0;
};

} // namespace RomCloud
