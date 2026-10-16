#pragma once
// BrickPadBt — Bluetooth Classic HID cho TrimUI Brick, port từ BrickPad
// (brickpad.c: evdev trừ ra vì RomCloud đã sở hữu input qua SDL).
// Classic HID (BR/EDR, L2CAP PSM 0x11/0x13 + SDP qua BlueZ ProfileManager).
// Pair với Android TV / box / PC / Mac là RetroArch nhận ngay.
#include <cstdint>
#include <string>
#include <vector>
#include "BrickPadProtocol.h"

namespace RomCloud {
namespace BrickPad {

struct ScannedDev {
    std::string path; // /org/bluez/hci0/dev_XX_...
    std::string mac;
    std::string name;
    int rssi = -127;
    bool hasRssi = false;
    bool paired = false;
    bool connected = false;
    bool trusted = false;
};

inline bool isMacAddress(const std::string &s) {
    if (s.empty()) return true;
    if (s.size() == 17) {
        for (int i = 0; i < 17; i++) {
            if (i % 3 == 2) {
                if (s[i] != ':' && s[i] != '-') return false;
            } else {
                if (!std::isxdigit((unsigned char)s[i])) return false;
            }
        }
        return true;
    } else if (s.size() == 12) {
        for (char c : s) {
            if (!std::isxdigit((unsigned char)c)) return false;
        }
        return true;
    }
    return false;
}

class BrickPadBt {
public:
    static BrickPadBt &instance();

    // Mở D-Bus system, đăng ký agent + HID/DID profile, btSetup, listen 2 PSM.
    // Trả false khi không có bluetoothd (UI báo rõ, các phần còn lại vẫn chạy).
    bool init(const std::string &btName, ControllerIdentity identity = ControllerIdentity::BRICKPAD);
    void shutdown();
    bool ready() const { return m_ready; }

    // Quét / liệt kê thiết bị (qua GetManagedObjects, poll vài giây/lần).
    std::vector<ScannedDev> enumerate();
    bool startDiscovery();
    bool stopDiscovery();

    std::string devPathFor(const std::string &mac) const;
    std::string queryDeviceName(const std::string &mac);
    // Lấy MAC address từ socket fd đã kết nối (AF_BLUETOOTH L2CAP)
    static std::string peerMacFromFd(int fd);
    // Parse MAC address từ D-Bus device object path (/org/bluez/hci0/dev_XX_XX_XX_XX_XX_XX)
    static std::string macFromDevPath(const std::string &path);
    // Pair đồng bộ (blocking tới timeoutMs, gọi từ worker thread).
    bool pairBlocking(const std::string &devPath, std::string &errOut,
                      int timeoutMs = 30000);
    bool setAlias(const std::string &devPath, const std::string &alias);
    bool setTrusted(const std::string &devPath, bool trusted = true);
    bool callDevice0(const std::string &devPath, const char *method);
    bool removeDevice(const std::string &devPath);

    // L2CAP trực tiếp (Brick là peripheral; BlueZ Connect() dành cho host-role).
    int listenPsm(uint16_t psm); // socket listen nonblock, -1 = lỗi
    static int connectL2(const std::string &mac, uint16_t psm); // nonblock fd, -1 lỗi

    // HIDP: gửi report + xử lý control channel (giữ nguyên logic BrickPad).
    static bool sendGamepad(int intrFd, const uint8_t *r, size_t len = 8);
    static bool sendKb(int intrFd, uint8_t mod, const uint8_t keys[6]);
    static bool sendMouse(int intrFd, uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel, uint8_t reportId = 0x01);
    static bool sendConsumer(int intrFd, uint16_t usage);
    static bool kbTap(int intrFd, uint8_t mod, uint8_t usage);
    static void handleCtrl(int ctrlFd, const uint8_t *cur, size_t curLen = 8);

    int dbusFd() const;
    void pumpDbus(); // dispatch nonblock — gọi mỗi frame khi ready
    // Nơi nhận NewConnection (control channel từ TV). Gọi trước init().
    void bindNewConnectionSink(int *ctrlFd, std::string *devPath);

    bool applyBtName(const std::string &name);
    bool setPower(bool on);
    static void btSetupFull(const std::string &name, ControllerIdentity identity = ControllerIdentity::BRICKPAD);
    static void maintainAdvertising();
    static void optimizeLinkPolicy(const std::string &mac = "");

    ControllerIdentity identity() const { return m_identity; }
    void setIdentity(ControllerIdentity id) { m_identity = id; }
    bool reregisterProfiles(ControllerIdentity identity);

private:
    BrickPadBt() = default;
    bool m_ready = false;
    std::string m_btName = "Brick Pad";
    ControllerIdentity m_identity = ControllerIdentity::BRICKPAD;
};

} // namespace BrickPad
} // namespace RomCloud
