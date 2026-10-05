#pragma once
// ClockStore: báo thức (lưu DB settings) + beep SDL.
#include <string>
#include <vector>

namespace RomCloud {

struct ClockAlarm {
    int hour = 7, minute = 0; // 24h
    bool enabled = true;
    int mode = 0; // 0=Chuông+Rung, 1=Chuông, 2=Rung (riêng từng báo thức)
    std::string label;
    bool operator==(const ClockAlarm& o) const {
        return hour == o.hour && minute == o.minute;
    }
};

// Thành phố giờ thế giới (luôn đúng 6 ô, lưới 3x2).
struct WorldCity {
    std::string name; // tên hiển thị (UTF-8)
    std::string tz;   // tzid IANA, vd "Asia/Ho_Chi_Minh"
};

class ClockStore {
public:
    static ClockStore& instance();
    std::vector<ClockAlarm> alarms();
    void saveAlarms(const std::vector<ClockAlarm>& v);
    void addAlarm(int h, int m);
    void removeAlarm(size_t idx);
    void toggleAlarm(size_t idx);
    void nudgeAlarm(size_t idx, int dh, int dm); // chỉnh giờ/phút, wrap
    std::vector<WorldCity> worldCities(); // 6 TP giờ thế giới (thiếu -> mặc định)
    void saveWorldCities(const std::vector<WorldCity>& v); // đủ 6 mới lưu
};

// Beep sine 880Hz nền (không cần file âm thanh). Idempotent.
// times <= 0 = kêu liên tục tới khi beepStop (chuông báo thức modal).
void beepStart(int times = 3);
void beepStop();
bool beepPlaying();

// Rung motor (gpio227 + /sys/class/motor/voltage). Pattern nền, dừng được.
// times <= 0 = rung liên tục tới khi vibrateStop.
void vibrateStart(int times = 3);
void vibrateStop();
bool vibratePlaying();

// Chế độ báo thức: 0 = Chuông+Rung, 1 = chỉ Chuông, 2 = chỉ Rung.
int alarmMode();
// Mở DAC volume trên Brick (driver ALSA mặc định = 0 -> câm toàn bộ).
// Gọi khi boot + mỗi lần beep để báo thức/mpv luôn phát ra tiếng.
void ensureAlarmVolume();
void setAlarmMode(int mode);
const char* alarmModeLabel(int mode);

} // namespace RomCloud
