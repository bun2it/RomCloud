#pragma once
#include <string>
#include <cstdint>
#include <functional>

namespace RomCloud {

class CastManager {
public:
    static CastManager& instance();

    bool init();
    void update();
    bool isRunning();
    bool start(bool nativeRes = false);
    bool stop();
    bool toggle();

    bool isHdMode() const { return m_nativeRes; }
    void setHdMode(bool hd);

    std::string getCastUrl();
    std::string getIpAddress();

    // Hardware FN switch state (GPIO 243)
    bool isFnSwitchOn();

    // P3-C: stream elapsed time (ms since start). 0 neu khong chay.
    uint32_t getStreamElapsedMs() const;

    // P1-2: Callback khi FN switch ON/OFF hoặc A press. Tham so bool = running
    // sau khi toggle. UI dang ky de show toast. Callback chay tren UI thread.
    void setFnToggleCallback(std::function<void(bool running, bool viaFnSwitch)> cb) {
        m_fnToggleCb = std::move(cb);
    }

    // P3-D: Callback đặc bi Neu user gat FN = ON nhưng Wi-Fi mất (IP="192.168.x.x").
    // CastManager bỏ qua start() để khong spawn daemon vo ich, callback này để
    // UI hiện toast cảnh báo.
    void setNoWifiFnCallback(std::function<void()> cb) {
        m_noWifiFnCb = std::move(cb);
    }

    // P3-B: Callback khi daemon crash và đang tự khởi động lại.
    // Tham số: succeeded (true = restart thành công, false = restart thất bại).
    // UI dung để hiện toast thông báo user.
    void setAutoReconnectCallback(std::function<void(bool succeeded)> cb) {
        m_autoReconnectCb = std::move(cb);
    }

private:
    CastManager() = default;
    ~CastManager() = default;

    int getRunningPid();
    // P3-B: health check trong update() — neu user expected running mà daemon
    // chết, tự restart với backoff.
    void checkAutoReconnect();

    bool m_nativeRes = false;
    int m_lastFnState = -1;
    uint32_t m_lastFnPollTicks = 0;
    // P3-C: SDL_GetTicks() luc bat dau stream (khoi tao = 0 khi khong chay).
    uint32_t m_streamStartedAt = 0;
    // P3-B: user "wanted" running state. Set = true trong start() (khi bat dau
    // stream), = false trong stop() (user chủ động stop). checkAutoReconnect()
    // so sánh với isRunning() để detect crash → tự restart.
    bool m_wantRunning = false;
    uint32_t m_lastReconnectCheckTicks = 0;
    int m_consecutiveRestartFailures = 0;
    std::function<void(bool running, bool /* viaFnSwitch */)> m_fnToggleCb;
    std::function<void()> m_noWifiFnCb;
    std::function<void(bool /* succeeded */)> m_autoReconnectCb;
};

} // namespace RomCloud
