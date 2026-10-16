#pragma once
// BrickPadManager — facade tính năng BrickPad trong RomCloud (thay thế
// GameCast). Sở hữu: preset store, Bluetooth Classic HID, HID sender,
// nav state của 8 màn hình, scan/pair jobs. UI (BrickPadUI.cpp) chỉ đọc
// state và gọi ops — mọi logic port từ BrickPad (brickpad.c nav engine).
//
// Flow bắt buộc: RomCloud > BrickPad > chọn thiết bị đã lưu để nối (máy cũ)
// hoặc pair (máy mới). Nối dùng platform global user đã chọn. Chỉ SAU KHI
// nối mới gửi HID (mọi nút phục vụ gamepad, trừ F1 thoát HID về BrickPad).
// Chưa nối: nút chỉ điều khiển UI như app RomCloud bình thường.
//
// Chống freeze: init nặng (btSetup + D-Bus + listen) chạy worker thread,
// UI thread không bao giờ block quá timeout D-Bus ngắn (2.5-5s).
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "BrickPadProtocol.h"
#include "BrickPadStore.h"

namespace RomCloud {
namespace BrickPad {

enum class BpScreen { HOME, SCAN, DEVICES, PRESET, KEYMAP, SETTINGS, SESSION };
enum class PadMode { GAMEPAD, KEYBOARD };

long nowMs();

struct DevRow {
    std::string mac;
    std::string name;
    bool paired = false;
    bool connected = false;
    int rssi = -127;
    bool hasRssi = false;
};

struct ToastMsg {
    std::string text;
    int kind = 0; // 0 info, 1 ok, 2 err, 3 warn
};

class BrickPadManager {
public:
    static BrickPadManager &instance();

    bool init(); // load store + btName, kill legacy gamecast_d, BT bring-up
    void update(); // gọi mỗi frame: pump BT, accept, HID send, timers
    void shutdown();

    // --- state cho UI ---
    BpScreen screen() const { return m_screen; }
    void goScreen(BpScreen s);
    int cursor() const { return m_cursor; }
    void setCursor(int c) { m_cursor = c; }
    PadMode mode() const { return m_mode; }
    void setMode(PadMode m);
    const char *modeLabel() const { return m_mode == PadMode::KEYBOARD ? "Bàn phím + Chuột" : "Tay cầm chơi game"; }
    std::string getDeviceModeLabelFor(const std::string &mac) const {
        return m_store.deviceMode(mac) == 1 ? "Bàn phím + Chuột" : "Tay cầm chơi game";
    }

    BrickPadStore &store() { return m_store; }
    bool btReady() const { return m_btReady; }
    bool btOn() const { return m_btOn; }
    void setBtOn(bool on);
    bool connected() const { return m_ctrlFd >= 0 || m_intrFd >= 0; }
    std::string peerMac() const { return m_peerMac; }
    std::string peerName() const;
    std::string peerNameLocked() const; // gọi khi đã giữ m_devsMutex
    const BrickInput &lastInput() const { return m_lastSentInput; }
    uint8_t lastHat() const { return m_lastHat; }
    int pairedCount() const;

    // --- scan ---
    void enterScan();
    void leaveScan();
    bool scanning() const { return m_scanning; }
    long scanMsLeft() const;
    void refreshDevs();
    std::vector<DevRow> scanDevs() const {
        std::lock_guard<std::mutex> lk(m_devsMutex);
        return m_devs;
    }

    // --- pair (async, poll kết quả) ---
    void startPair(const std::string &mac, const std::string &name);
    bool pairBusy() const { return m_pairBusy.load(); }
    // Trả true khi có kết quả mới (lấy 1 lần).
    bool pollPairResult(bool &ok, std::string &msg);

    // --- connect ---
    bool connectHost(const std::string &mac, std::string &err);
    void disconnectHost();
    bool removePaired(const std::string &mac); // quên: remove bond + xóa bind
    bool renameDevice(const std::string &mac, const std::string &alias);

    // --- preset edit buffer (KEYMAP) ---
    Keymap editKm;
    bool editValid = false;
    int editUser = -1, editSys = -1;
    void editBegin(int user, int sys);
    void editSave();

    // --- chooser overlay: 0 tắt, 1 rename device, 2 BT name ---
    int chooser() const { return m_chooser; }
    void setChooser(int c) { m_chooser = c; m_chCursor = 0; }
    int chooserCursor() const { return m_chCursor; }
    void setChooserCursor(int c) { m_chCursor = c; }
    std::string chooserCtx() const { return m_chooserCtx; }
    void setChooserCtx(const std::string &s) { m_chooserCtx = s; }

    // --- bàn phím ảo RomCloud (mở ở chế độ Bàn phím, commit -> gửi HID) ---
    bool vkOpen() const { return m_vkOpen; }
    void setVkOpen(bool o) { m_vkOpen = o; }
    void sendNeutral();
    // Gõ chuỗi Latin qua HID (ký tự không hỗ trợ sẽ bỏ qua).
    void typeText(const std::string &s);

    // --- quick menu (giữ L1+R1 2s trên màn PHIÊN) ---
    bool qmOpen() const { return m_qmOpen; }
    int qmRow() const { return m_qmRow; }
    void setQmRow(int r) { m_qmRow = r; }
    void openQuickMenu();
    void closeQuickMenu();
    void pokeQuickMenu() { m_qmOpenedMs = nowMs(); pokeActivity(); }
    void quickMenuNextPreset(); // A ở hàng "Đổi preset": sang hệ kế tiếp

    // --- tiết kiệm pin: đen màn PHIÊN sau 30s, L1/R1 để mở lại ---
    bool blanked() const { return m_blanked; }
    void pokeActivity() { m_lastActMs = nowMs(); m_blanked = false; }
    void unblank() { m_blanked = false; m_lastActMs = nowMs(); }
    void toggleMode();
    // F1: thoát HID (ngắt kết nối, giữ nguyên màn hình).
    // Bấm F1 khi chưa nối thì không làm gì.

    // --- xác nhận 2 nhịp ---
    int forgetArm = -1; // DEVICES: index đang chờ xác nhận quên
    bool resetArm = false; // SETTINGS: chờ xác nhận khôi phục mặc định
    int scrollTop = 0; // offset cửa sổ cuộn cho list dài

    // --- toast (UI poll mỗi frame) ---
    void toast(const std::string &t, int kind = 0);
    bool pollToast(ToastMsg &out);

    // Tên gợi ý (không nhập text tự do — đúng chuẩn BrickPad).
    static const std::vector<std::string> &renameSuggest();
    static const std::vector<std::string> &btNameSuggest();

    std::string btName() const { return m_btName; }
    void setBtName(const std::string &n);

    ControllerIdentity identity() const { return m_identity; }
    void setIdentity(ControllerIdentity id, bool applyLive = false);

    std::string platformNameOf(ControllerIdentity id) const {
        if (id == ControllerIdentity::PC_LAPTOP) return "PC / Laptop";
        return "Xbox Wireless Controller";
    }

    std::string platformName() const {
        return platformNameOf(m_identity);
    }

    std::string getPlatformNameFor(const std::string &mac) const {
        return platformNameOf(m_store.deviceIdentity(mac));
    }

    void syncDevicePlatform(const std::string &mac);

    void cyclePlatform(bool reverse = false) {
        (void)reverse;
        if (m_identity == ControllerIdentity::XBOX) {
            setIdentity(ControllerIdentity::PC_LAPTOP, false);
            setMode(PadMode::KEYBOARD);
        } else {
            setIdentity(ControllerIdentity::XBOX, false);
            setMode(PadMode::GAMEPAD);
        }
    }
    void applyPlatformLiveAsync();

    int mouseSpeed() const { return m_store.mouseSpeed; }
    void setMouseSpeed(int speed) {
        if (speed < 1) speed = 1;
        if (speed > 10) speed = 10;
        m_store.mouseSpeed = speed;
        saveStore();
    }

    void onInputEvent();

    // --- User management (Tier 1) ---
    int userCount() const { return (int)m_store.users.size(); }
    std::string userNameAt(int i) const;
    void switchUser(int i);
    bool addUser(const std::string &name);
    bool removeUser(int i);
    bool renameUser(int i, const std::string &name);

    // --- Preset management (Tier 2) ---
    bool removePreset(int sysIdx);
    bool renamePreset(int sysIdx, const std::string &label);
    void resetCurrentPresets();

    // --- Auto-connect (2-way) ---
    void triggerAutoConnect();
    void startConnect(const std::string &mac);
    bool connectBusy() const { return m_connectBusy.load(); }
    bool autoConnectEnabled() const;
    void setAutoConnectEnabled(bool en);

    // --- Persistent keys sync ---
    void syncBluetoothKeys();

    bool discBanner() const { return m_discBanner; }
    void clearDiscBanner() { m_discBanner = false; }

    std::string storePath() const { return m_storePath; }
    void saveStore();

private:
    BrickPadManager() = default;
    void pumpSockets(); // accept + handleCtrl (nonblock)
    void pumpHid(); // đọc InputManager -> gửi HID (chỉ khi hidAllowed)
    // Gate HID duy nhất: SESSION + UIState BRICK_PAD + không QM/VK/blank.
    bool hidAllowed() const;
    void pumpScan();
    void pumpCombo(); // SELECT+START giữ 1s đổi mode
    void onConnected(const std::string &mac);
    void onDisconnected(bool userInitiated);

    BrickPadStore m_store;
    std::string m_storePath;
    std::string m_btNamePath;
    std::string m_btName = "Brick Pad";
    ControllerIdentity m_identity = ControllerIdentity::XBOX;

    BpScreen m_screen = BpScreen::DEVICES;
    int m_cursor = 0;
    PadMode m_mode = PadMode::GAMEPAD;
    bool m_vkOpen = false;
    bool m_qmOpen = false;
    int m_qmRow = 0;
    long m_qmOpenedMs = 0;
    long m_l1r1StartMs = 0;
    bool m_blanked = false;
    long m_lastActMs = 0;
    bool m_prevFn = false; // cạnh lên F1 (isFnLeftTriggered là level)
    uint8_t m_lastMouseButtons = 0;
    uint8_t m_lastKbKeys[6] = {0, 0, 0, 0, 0, 0};
    uint16_t m_lastConsumerUsage = 0;
    uint32_t m_lastWheelMs = 0;

    bool m_inited = false; // chỉ UI thread chạm (worker set cuối cùng)
    std::atomic<bool> m_initStarted{false}; // init worker chỉ chạy 1 lần
    std::atomic<bool> m_btReady{false}; // worker ghi, UI + worker đọc
    long m_lastAdvWatchdogMs = 0;
    std::atomic<bool> m_advThreadRunning{false};
    mutable std::mutex m_devsMutex; // m_devs: UI thread + worker/pair/connect threads
    std::mutex m_toastMutex; // m_toasts: nhiều threads toast
    // NOTE: m_store chỉ mutate trên UI thread + refreshDevs (worker); các op
    // nhỏ, chưa ghi nhận crash — không lock để tránh deadlock, sẽ bổ sung nếu cần.
    bool m_btOn = true;
    int m_lsCtrl = -1, m_lsIntr = -1;
    int m_ctrlFd = -1, m_intrFd = -1;
    std::string m_peerMac;
    std::string m_newConnPath;
    bool m_discBanner = false;

    bool m_scanning = false;
    long m_scanEndMs = 0;
    long m_lastEnumMs = 0;
    std::vector<DevRow> m_devs;

    std::atomic<bool> m_pairBusy{false};
    std::atomic<bool> m_pairHasResult{false};
    std::atomic<bool> m_connectBusy{false};
    bool m_pairOk = false;
    std::string m_pairMsg;
    std::string m_pairMac, m_pairName;

    BrickInput m_lastSentInput;
    BrickInput m_prevInput;
    uint8_t m_lastHat = 8;
    uint8_t m_lastReport[32] = {0};
    size_t m_lastReportLen = 0;
    bool m_reportValid = false;
    long m_comboStartMs = 0;

    int m_chooser = 0, m_chCursor = 0;
    std::string m_chooserCtx;

    std::vector<ToastMsg> m_toasts;
};

} // namespace BrickPad
} // namespace RomCloud
