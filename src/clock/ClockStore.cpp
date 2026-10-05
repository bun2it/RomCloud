#include "ClockStore.h"
#include "../database/DatabaseManager.h"
#include "../logging/Logger.h"

#include <SDL2/SDL.h>
#include <sstream>
#include <cmath>
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <sys/wait.h>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace RomCloud {

ClockStore& ClockStore::instance() {
    static ClockStore inst;
    return inst;
}

std::vector<ClockAlarm> ClockStore::alarms() {
    std::vector<ClockAlarm> out;
    std::string s = DatabaseManager::instance().getSetting("clock_alarms", "");
    int defMode = alarmMode();
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ';')) {
        if (item.size() < 5) continue;
        ClockAlarm a;
        try {
            a.hour = std::stoi(item.substr(0, 2));
            a.minute = std::stoi(item.substr(3, 2));
        } catch (...) { continue; }
        // Format: HH:MM|on|mode|label (mode mới, thiếu thì lấy mặc định)
        std::vector<std::string> parts;
        {
            std::stringstream ps(item.size() > 6 ? item.substr(6) : "");
            std::string p;
            while (std::getline(ps, p, '|')) parts.push_back(p);
        }
        a.enabled = parts.empty() || parts[0] != "0";
        a.mode = defMode;
        size_t li = 0;
        if (parts.size() >= 2 && parts[1].size() == 1 &&
            parts[1][0] >= '0' && parts[1][0] <= '2') {
            a.mode = parts[1][0] - '0';
            li = 2;
        } else {
            li = 1;
        }
        std::string lab;
        for (size_t i = li; i < parts.size(); ++i) {
            if (i > li) lab += "|";
            lab += parts[i];
        }
        a.label = lab;
        if (a.hour < 0 || a.hour > 23 || a.minute < 0 || a.minute > 59) continue;
        out.push_back(a);
    }
    return out;
}

void ClockStore::saveAlarms(const std::vector<ClockAlarm>& v) {
    std::ostringstream out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out << ';';
        char buf[16];
        int m = v[i].mode;
        if (m < 0 || m > 2) m = 0;
        snprintf(buf, sizeof(buf), "%02d:%02d|%d|%d|", v[i].hour, v[i].minute,
                 v[i].enabled ? 1 : 0, m);
        out << buf << v[i].label;
    }
    DatabaseManager::instance().setSetting("clock_alarms", out.str());
}

void ClockStore::addAlarm(int h, int m) {
    auto v = alarms();
    if (v.size() >= 10) return;
    ClockAlarm a;
    a.hour = h;
    a.minute = m;
    v.push_back(a);
    saveAlarms(v);
}

void ClockStore::removeAlarm(size_t idx) {
    auto v = alarms();
    if (idx < v.size()) {
        v.erase(v.begin() + idx);
        saveAlarms(v);
    }
}

void ClockStore::toggleAlarm(size_t idx) {
    auto v = alarms();
    if (idx < v.size()) {
        v[idx].enabled = !v[idx].enabled;
        saveAlarms(v);
    }
}

void ClockStore::nudgeAlarm(size_t idx, int dh, int dm) {
    auto v = alarms();
    if (idx >= v.size()) return;
    v[idx].hour = (v[idx].hour + dh + 24) % 24;
    v[idx].minute = (v[idx].minute + dm + 60) % 60;
    saveAlarms(v);
}

std::vector<WorldCity> ClockStore::worldCities() {
    std::vector<WorldCity> def = {
        {"Hà Nội", "Asia/Ho_Chi_Minh"}, {"Tokyo", "Asia/Tokyo"},
        {"Sydney", "Australia/Sydney"}, {"Paris", "Europe/Paris"},
        {"London", "Europe/London"}, {"New York", "America/New_York"},
    };
    std::string s = DatabaseManager::instance().getSetting("clock_cities", "");
    if (s.empty()) return def;
    std::vector<WorldCity> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ';')) {
        if (item.empty()) continue;
        size_t p = item.find('|');
        if (p == std::string::npos || p == 0 || p + 1 >= item.size()) continue;
        WorldCity w;
        w.name = item.substr(0, p);
        w.tz = item.substr(p + 1);
        if (w.tz.size() > 64) continue;
        out.push_back(w);
    }
    if (out.size() != 6) return def;
    return out;
}

void ClockStore::saveWorldCities(const std::vector<WorldCity>& v) {
    if (v.size() != 6) return;
    std::ostringstream out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out << ';';
        std::string nm = v[i].name, tz = v[i].tz;
        for (char& ch : nm) if (ch == '|' || ch == ';') ch = ' ';
        for (char& ch : tz) if (ch == '|' || ch == ';') ch = '_';
        if (tz.empty()) tz = "UTC";
        out << nm << '|' << tz;
    }
    DatabaseManager::instance().setSetting("clock_cities", out.str());
}

// ---- Beep: phat file WAV qua aplay ----
// (SDL ALSA underrun trien mien tren driver audiocodec cua Brick:
// device mo OK, callback chay nhung appl_ptr dung yen -> cam.
// aplay da verify keu ro nen dung tien trinh ngoai, loop toi khi stop.)
// times <= 0 = keu lien tuc toi khi beepStop.
static std::atomic<bool> s_beepStop{false};
static std::atomic<bool> s_beepRunning{false};
static std::atomic<pid_t> s_beepPid{-1};

static bool haveFile(const char* p) {
    FILE* f = fopen(p, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

// Chon backend: uu tien alarm.wav qua aplay (path da verify keu,
// khoi dong tuc thi), fallback alarm.mp3 qua mpv.
static bool beepPick(std::string& player, std::string& file) {
    static const char* wavs[] = {
        "/mnt/SDCARD/Apps/RomCloud/assets/sounds/alarm.wav",
        "assets/sounds/alarm.wav",
    };
    static const char* mp3s[] = {
        "/mnt/SDCARD/Apps/RomCloud/assets/sounds/alarm.mp3",
        "assets/sounds/alarm.mp3",
    };
    for (auto p : wavs) {
        if (haveFile(p)) { player = "aplay"; file = p; return true; }
    }
    for (auto p : mp3s) {
        if (haveFile(p)) {
            if (haveFile("/mnt/SDCARD/Apps/RomCloud/bin/mpv"))
                player = "/mnt/SDCARD/Apps/RomCloud/bin/mpv";
            else
                player = "mpv";
            file = p;
            return true;
        }
    }
    return false;
}

static void beepThreadMain(int times) {
    ensureAlarmVolume(); // DAC có thể bị mute lại sau reboot/cắm tai nghe
    std::string player, file;
    if (!beepPick(player, file)) {
        Logger::warn("beep: missing alarm sound");
        s_beepRunning = false;
        return;
    }
    bool isMpv = (player.find("mpv") != std::string::npos);
    Logger::warn(std::string("beep: using player=") + player + " file=" + file);
    int n = 0;
    while (!s_beepStop.load() && (times <= 0 || n < times)) {
        uint32_t t0 = SDL_GetTicks();
        pid_t pid = fork();
        if (pid == 0) {
            // Gom stderr của player vào file để chẩn đoán trên máy.
            FILE* el = freopen("/tmp/beep_err.log", "a", stderr);
            (void)el;
            if (isMpv) {
                execlp(player.c_str(), player.c_str(), "--no-video",
                       "--really-quiet", "--no-terminal", file.c_str(),
                       (char*)nullptr);
            } else {
                // Absolute path trước (launcher có thể không có /usr/bin
                // trong PATH), fallback execlp.
                execl("/usr/bin/aplay", "aplay", "-q", file.c_str(),
                      (char*)nullptr);
                execlp("aplay", "aplay", "-q", file.c_str(), (char*)nullptr);
            }
            _exit(127); // khong co player
        }
        if (pid < 0) break;
        s_beepPid = pid;
        int st = 0;
        for (;;) {
            pid_t r = waitpid(pid, &st, WNOHANG);
            if (r == pid) break;
            if (s_beepStop.load()) {
                kill(pid, SIGKILL);
                waitpid(pid, &st, 0);
                break;
            }
            usleep(50000);
        }
        s_beepPid = pid_t(-1);
        // player chet ngay (<0.5s) ma chua ai stop = khong co backend: nghi.
        uint32_t dur = SDL_GetTicks() - t0;
        if (!s_beepStop.load()) {
            int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            if (dur < 500 || code != 0) {
                char eb[128];
                snprintf(eb, sizeof(eb),
                         "beep: child exit dur=%ums code=%d (xem /tmp/beep_err.log)",
                         dur, code);
                Logger::warn(eb);
            }
            if (dur < 500) break;
        }
        n++;
    }
    s_beepRunning = false;
}

void beepStart(int times) {
    if (times < 0) times = 0;
    if (times > 10) times = 10;
    if (s_beepRunning.load()) return; // dang keu thi thoi
    s_beepStop = false;
    s_beepRunning = true;
    std::thread(beepThreadMain, times).detach();
}

void beepStop() {
    s_beepStop = true;
    pid_t pid = s_beepPid.load();
    if (pid > 0) kill(pid, SIGKILL);
}

bool beepPlaying() {
    return s_beepRunning.load();
}

void ensureAlarmVolume() {
#ifndef PC_SIMULATOR_MODE
    // Brick audiocodec: DAC volume mặc định 0 (= câm toàn bộ, kể cả mpv).
    // Mở max; chỉ gọi khi boot + beep nên rẻ. Không đụng volume Headphone
    // (giữ mức người dùng đang nghe).
    int rc = system("amixer sset 'DAC volume' 255 >/dev/null 2>&1");
    if (rc != 0) Logger::warn("ensureAlarmVolume: amixer rc=" + std::to_string(rc));
#endif
}

// ---- Rung motor: gpio227 enable + voltage 3.3V, nhịp 0.6s/0.4s ----
static std::atomic<int> s_vibLeft{0};
static std::atomic<bool> s_vibStop{false};
static std::atomic<bool> s_vibLoop{false};
static std::atomic<bool> s_vibRunning{false};

static void vibWrite(bool on) {
    FILE* f = fopen("/sys/class/gpio/gpio227/value", "w");
    if (f) {
        fputc(on ? '1' : '0', f);
        fclose(f);
    }
    f = fopen("/sys/class/motor/voltage", "w");
    if (f) {
        fputs(on ? "3300000" : "0", f);
        fclose(f);
    }
}

void vibrateStart(int times) {
    if (s_vibRunning.load()) return; // đang rung thì thôi
    if (times < 0) times = 0;
    if (times > 10) times = 10;
    s_vibLeft = times;
    s_vibLoop = (times == 0); // 0 = rung liên tục tới khi vibrateStop
    s_vibStop = false;
    s_vibRunning = true;
    std::thread([]() {
        while (!s_vibStop.load() &&
               (s_vibLoop.load() || s_vibLeft.load() > 0)) {
            vibWrite(true);
            for (int i = 0; i < 6 && !s_vibStop.load(); i++)
                usleep(100000); // 0.6s
            vibWrite(false);
            if (!s_vibLoop.load()) s_vibLeft--;
            for (int i = 0; i < 4 && !s_vibStop.load(); i++)
                usleep(100000); // 0.4s nghỉ
        }
        vibWrite(false);
        s_vibLeft = 0;
        s_vibRunning = false;
    }).detach();
}

void vibrateStop() {
    s_vibStop = true;
    s_vibLoop = false;
    s_vibLeft = 0;
    vibWrite(false);
}

bool vibratePlaying() {
    return s_vibRunning.load();
}

int alarmMode() {
    std::string s = DatabaseManager::instance().getSetting("clock_alarm_mode", "0");
    int m = 0;
    try { m = std::stoi(s); } catch (...) {}
    if (m < 0 || m > 2) m = 0;
    return m;
}

void setAlarmMode(int mode) {
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    DatabaseManager::instance().setSetting("clock_alarm_mode", std::to_string(mode));
}

const char* alarmModeLabel(int mode) {
    if (mode == 1) return "Chuông";
    if (mode == 2) return "Rung";
    return "Chuông+Rung";
}

} // namespace RomCloud
