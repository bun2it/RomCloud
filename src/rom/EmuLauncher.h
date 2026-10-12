#pragma once
// EmuLauncher — chạy emulator stock của TrimUI cho game local, giống
// cách MainUI làm: fork Emus/<CODE>/<launch-mặc-định> "<rom>" (script này
// tự dựng cpufreq + gọi RetroArch core / binary standalone tương ứng),
// supervise qua waitpid. "Chuẩn vàng" StockOS (verified qua adb):
//   - câu lệnh: HOME=<...> <binary> [-v] [$NET_PARAM] -L <core> "<rom>"
//     nằm TRONG script, RomCloud chỉ gọi script + truyền path ROM.
//   - script mặc định đọc từ Emus/<CODE>/config.json field "launch"
//     (vd CPS3->launch_fbneo.sh, FBNEO->launch_performance.sh,
//      PPSSPP->launch_performance_vulkan.sh, SS->launch_yabasanshiro_standalone.sh).
//   - ROM ở /mnt/SDCARD/Roms/<DIR>, save RetroArch ở
//     RetroArch/.retroarch/{saves,states}, standalone tự giữ dưới HOME
//     của nó. RomCloud KHÔNG đụng vào các path này.
// Trong lúc chơi emulator giữ toàn màn hình, RomCloud không vẽ gì
// (state GAME_PLAYING) và không bắt phím nào — thoát duy nhất bằng
// stock flow của emulator (menu/hotkey của nó rồi tự thoát).
#include <atomic>
#include <chrono>
#include <string>
#include <sys/types.h>

namespace RomCloud {

class EmuLauncher {
public:
    static EmuLauncher& instance();

    // Chạy game. False khi không có launch.sh / rom không đọc được.
    bool play(const std::string& systemCode, const std::string& romPath,
              const std::string& title);
    bool stop();
    bool isRunning();
    // True nếu vừa phát hiện đã thoát (đã reap + log).
    bool pollExited();

    std::string currentTitle() const { return m_title; }
    // Mốc bắt đầu phiên (ms steady clock) — UI dùng phát hiện emulator
    // chết tức thì (core hỏng) để báo đúng bệnh thay vì đá về list im lặng.
    uint64_t startMs() const { return m_startMs; }

private:
    EmuLauncher() = default;
    ~EmuLauncher();
    EmuLauncher(const EmuLauncher&) = delete;
    EmuLauncher& operator=(const EmuLauncher&) = delete;

    bool findLauncher(const std::string& systemCode, std::string& outScript);

    pid_t m_pid = -1;
    std::string m_title;
    uint64_t m_startMs = 0;
};

} // namespace RomCloud
