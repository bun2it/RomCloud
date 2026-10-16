#pragma once
// BrickPadStore — preset theo user + hệ máy, port từ BrickPad (ui_fb.h store
// engine). 1 file JSON duy nhất trên thẻ nhớ (reboot không mất).
// Schema: {version, activeUser, activeSystem:{user:syskey},
//          users:[{name, systems:[{system, label, keymap}]}],
//          devices:[{address, alias, system, lastConnected}]}
#include <string>
#include <vector>
#include "BrickPadProtocol.h"

namespace RomCloud {
namespace BrickPad {

static const int kMaxUsers = 4;
static const int kMaxSystems = 10;
static const int kMaxDevs = 16;

struct Keymap {
    int8_t b[P_COUNT]; // HID bit 0..15, -1 = giữ nguyên (identity)
    bool dpadToHat = true;
    bool dpadAsLeftStick = false;
};

struct PresetSystem {
    std::string system; // key: snes, nintendo, n64, xbox, psx...
    std::string label;  // tên hiện UI
    Keymap km;
};

struct BtUser {
    std::string name;
    ControllerIdentity defaultIdentity = ControllerIdentity::XBOX;
    bool autoConnect = true;
    std::vector<PresetSystem> systems;
};

struct BtDevice {
    std::string address; // AA:BB:CC:DD:EE:FF
    std::string alias;
    std::string system; // system key đã bind ("" = chưa)
    std::string lastConnected;
    ControllerIdentity identity = ControllerIdentity::XBOX;
    int mode = 0; // 0: GAMEPAD, 1: KEYBOARD
};

class BrickPadStore {
public:
    bool load(const std::string &path);
    bool save(const std::string &path) const;

    int userIndex(const std::string &name) const;
    int activeUserIdx() const;
    int sysIndex(int userIdx, const std::string &system) const;
    int activeSysIdx() const;
    const Keymap &activeKeymap() const;
    int systemCount() const;
    const char *sysLabelAt(int i) const;
    const char *sysKeyAt(int i) const;
    void setActiveSystem(int i);
    void setActiveUser(int i);
    bool addSystemClone(int userIdx); // clone hệ đang dùng -> customN
    bool removeSystem(int userIdx, int sysIdx);
    bool renameSystem(int userIdx, int sysIdx, const std::string &newLabel);
    bool addUser(const std::string &name, ControllerIdentity identity = ControllerIdentity::XBOX);
    bool removeUser(int userIdx);
    bool renameUser(int userIdx, const std::string &newName);
    void resetDefaults();
    void resetUserDefaults(int userIdx);

    // Bind MAC <-> preset (devices[].system). Trả về sysIdx hoặc <0.
    int bindLookup(const std::string &mac) const;
    bool bindSet(const std::string &mac, const std::string &syskey);
    bool bindClear(const std::string &mac);
    void devTouch(const std::string &mac, const std::string &alias);
    bool renameDevice(const std::string &mac, const std::string &alias);
    bool removeDevice(const std::string &mac);
    ControllerIdentity deviceIdentity(const std::string &mac) const;
    bool setDeviceIdentity(const std::string &mac, ControllerIdentity id);
    int deviceMode(const std::string &mac) const;
    bool setDeviceMode(const std::string &mac, int mode);
    const BtDevice *lastConnectedDevice() const;

    std::string activeUser;
    std::string activeSys[kMaxUsers];
    std::vector<BtUser> users;
    std::vector<BtDevice> devices;
    ControllerIdentity lastPlatform = ControllerIdentity::XBOX;
    int lastMode = 0; // 0: GAMEPAD, 1: KEYBOARD
    int mouseSpeed = 3; // 1..10 (tốc độ chuột)
    int version = 1;

    static void normalizeMac(const std::string &in, std::string &out);

private:
    void addSystem(BtUser &u, const std::string &system, const std::string &label,
                   const int8_t *map, bool toHat, bool asStick);
};

// Áp dụng keymap vào report 8 byte (layout generic). Layer duy nhất được đổi nút.
void applyKeymap(const BrickInput &in, const Keymap &km, uint8_t r[8]);

// Hoán vị nút vật lý theo keymap TRƯỚC KHI pack theo identity.
// Keymap value = HID bit layout generic (0:A 1:B 2:X 3:Y 4:L1 5:R1 6:L2 7:R2
// 8:SEL 9:START 10:L3 11:R3 12:HOME 13:FN1 14:FN2, -1 = giữ nguyên).
// D-pad + stick đi thẳng (keymap không có entry cho chúng).
BrickInput remapInputForKeymap(const Keymap &km, const BrickInput &in);

} // namespace BrickPad
} // namespace RomCloud
