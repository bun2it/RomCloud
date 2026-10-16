// BrickPadManager — nav engine + HID sender, port brickpad.c main loop.
#include "BrickPadManager.h"
#include "BrickPadBt.h"
#include "../config/AppConfig.h"
#include "../input/InputManager.h"
#include "../logging/Logger.h"

#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <cmath>

namespace RomCloud {
namespace BrickPad {

static void LOGI(const std::string &s) { Logger::info("[BrickPadManager] " + s); }

long nowMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

BrickPadManager &BrickPadManager::instance() {
    static BrickPadManager s;
    return s;
}

bool BrickPadManager::init() {
    if (m_inited) return true;
    m_inited = true;

    m_storePath = AppConfig::instance().getDataDir() + "/brickpad_presets.json";
    m_btNamePath = AppConfig::instance().getDataDir() + "/brickpad_btname.txt";

    bool had = m_store.load(m_storePath);
    if (!had) saveStore(); // lần đầu: ghi defaults

    // Cập nhật identity và mode: Xbox luôn là GAMEPAD; PC_LAPTOP lấy theo store
    m_identity = m_store.lastPlatform;
    if (m_identity == ControllerIdentity::XBOX) {
        m_mode = PadMode::GAMEPAD;
        m_store.lastMode = 0;
    } else {
        m_mode = (PadMode)m_store.lastMode;
    }

    IdentityInfo idInfo = getIdentityInfo(m_identity);
    m_btName = idInfo.btName;
    m_lastReportLen = packNeutralReport(m_identity, m_lastReport, sizeof(m_lastReport));

    // Load thiết bị đã lưu từ store để hiển thị ngay lập tức (không block UI)
    refreshDevs();

    // Khởi tạo phần cứng Bluetooth và L2CAP trong background thread
    if (!m_initStarted.load()) {
        m_initStarted.store(true);
        std::thread([this]() {
            // Kill legacy gamecast_d một lần (tính năng cũ đã gỡ).
            std::system("killall -9 gamecast_d 2>/dev/null; rm -f /tmp/gamecast.pid");

            // Khôi phục BlueZ link keys từ SD card nếu có (cả /var/lib/bluetooth và /etc/bluetooth/keys)
            std::string keyDir = AppConfig::instance().getDataDir() + "/bluetooth_keys";
            std::string restoreCmd = "if [ -d '" + keyDir + "' ]; then "
                                     "mkdir -p /var/lib/bluetooth /etc/bluetooth/keys 2>/dev/null; "
                                     "cp -af '" + keyDir + "/.' /var/lib/bluetooth/ 2>/dev/null; "
                                     "cp -af '" + keyDir + "/.' /etc/bluetooth/keys/ 2>/dev/null; sync; fi";
            std::system(restoreCmd.c_str());

            BrickPadBt::instance().bindNewConnectionSink(&m_ctrlFd, &m_newConnPath);
            m_btReady = BrickPadBt::instance().init(m_btName, m_identity);
            if (m_btReady) {
                m_lsCtrl = BrickPadBt::instance().listenPsm(0x11);
                m_lsIntr = BrickPadBt::instance().listenPsm(0x13);
                if (m_lsCtrl < 0 || m_lsIntr < 0)
                    toast("L2CAP listen thất bại", 2);
            } else {
                toast("Bluetooth chưa sẵn sàng", 2);
            }
            refreshDevs();
            triggerAutoConnect();
            m_initStarted.store(false);
        }).detach();
    }

    return true;
}

void BrickPadManager::shutdown() {
    leaveScan();
    disconnectHost();
    if (m_lsCtrl >= 0) { close(m_lsCtrl); m_lsCtrl = -1; }
    if (m_lsIntr >= 0) { close(m_lsIntr); m_lsIntr = -1; }
    BrickPadBt::instance().shutdown();
    saveStore();
    m_inited = false;
}

void BrickPadManager::saveStore() {
    // Đảm bảo data dir tồn tại (thẻ nhớ).
    mkdir(AppConfig::instance().getDataDir().c_str(), 0755);
    m_store.save(m_storePath);
    std::string cmd = "sync";
    (void)cmd;
}

void BrickPadManager::goScreen(BpScreen s) {
    if (s == BpScreen::HOME || s == BpScreen::SESSION || s == BpScreen::PRESET || s == BpScreen::KEYMAP) {
        s = BpScreen::DEVICES;
    }
    m_screen = s;
    m_cursor = 0;
    scrollTop = 0;
    forgetArm = -1;
    setChooser(0);
    resetArm = false;
    closeQuickMenu();
    m_vkOpen = false;
    pokeActivity();
}

void BrickPadManager::update() {
    if (!m_inited) return;
    if (m_btReady) BrickPadBt::instance().pumpDbus();
    pumpSockets();
    pumpScan();
    pumpHid();
    pumpCombo();

    // Watchdog keep-alive: bảo đảm BlueZ luôn giữ quảng bá khi chưa có thiết bị kết nối
    if (m_btReady && m_intrFd < 0 && m_btOn) {
        long now = nowMs();
        if (now - m_lastAdvWatchdogMs >= 3500) {
            m_lastAdvWatchdogMs = now;
            if (!m_advThreadRunning.load()) {
                m_advThreadRunning.store(true);
                std::thread([this]() {
                    BrickPadBt::maintainAdvertising();
                    m_advThreadRunning.store(false);
                }).detach();
            }
        }
    }

    // Quick menu tự đóng sau 10s.
    if (m_qmOpen && nowMs() - m_qmOpenedMs > 10000) closeQuickMenu();
    // Đen màn PHIÊN sau 30s không thao tác (tiết kiệm pin).
    if (m_screen == BpScreen::SESSION && connected() && !m_blanked &&
        nowMs() - m_lastActMs > 30000) {
        m_blanked = true;
    }
}

void BrickPadManager::pumpSockets() {
    if (!m_btReady) return;
    fd_set rf;
    FD_ZERO(&rf);
    int mx = -1;
    auto add = [&](int fd) {
        if (fd >= 0) { FD_SET(fd, &rf); if (fd > mx) mx = fd; }
    };
    add(m_lsCtrl); add(m_lsIntr); add(m_ctrlFd);
    if (mx < 0) return;
    struct timeval tv = {0, 0};
    int r = select(mx + 1, &rf, nullptr, nullptr, &tv);
    if (r <= 0) return;
    if (m_lsCtrl >= 0 && FD_ISSET(m_lsCtrl, &rf)) {
        int c = accept(m_lsCtrl, nullptr, nullptr);
        if (c >= 0) {
            int fl = fcntl(c, F_GETFL, 0);
            fcntl(c, F_SETFL, fl | O_NONBLOCK);
            if (m_ctrlFd >= 0) close(m_ctrlFd);
            m_ctrlFd = c;
            std::string pm = BrickPadBt::peerMacFromFd(c);
            if (!pm.empty()) m_peerMac = pm;
        }
    }
    if (m_lsIntr >= 0 && FD_ISSET(m_lsIntr, &rf)) {
        int c = accept(m_lsIntr, nullptr, nullptr);
        if (c >= 0) {
            int fl = fcntl(c, F_GETFL, 0);
            fcntl(c, F_SETFL, fl | O_NONBLOCK);
            int sndbuf = 512;
            setsockopt(c, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
            if (m_intrFd >= 0) close(m_intrFd);
            m_intrFd = c;
            std::string pm = BrickPadBt::peerMacFromFd(c);
            if (!pm.empty()) m_peerMac = pm;
            // Có interrupt = TV/Laptop đã vào: xác nhận kết nối.
            onConnected(m_peerMac);
        }
    }
    // NewConnection qua ProfileManager (control channel do BlueZ accept).
    if (!m_newConnPath.empty()) {
        std::string p = m_newConnPath;
        m_newConnPath.clear();
        std::string pm = BrickPadBt::macFromDevPath(p);
        if (!pm.empty()) m_peerMac = pm;
        else if (m_ctrlFd >= 0 && m_peerMac.empty()) {
            m_peerMac = BrickPadBt::peerMacFromFd(m_ctrlFd);
        }
    }
    if (m_ctrlFd >= 0 && FD_ISSET(m_ctrlFd, &rf)) {
        // Đọc thử 1 byte để phát hiện drop (handleCtrl đọc hết).
        BrickPadBt::handleCtrl(m_ctrlFd, m_lastReport, m_lastReportLen > 0 ? m_lastReportLen : 8);
        // Kiểm tra drop: poll lỗi đọc sẽ lộ ở lần gửi sau.
    }
    // Phát hiện drop khi cả 2 fd lỗi gửi.
    if ((m_ctrlFd >= 0 || m_intrFd >= 0) && m_intrFd >= 0) {
        // gửi keep-alive nhẹ: không — tránh spam; drop lộ khi send fail.
    }
}

void BrickPadManager::onConnected(const std::string &mac) {
    std::string useMac = mac;
    if (useMac.empty()) useMac = m_peerMac;
    if (useMac.empty() && m_intrFd >= 0) useMac = BrickPadBt::peerMacFromFd(m_intrFd);
    if (useMac.empty() && m_ctrlFd >= 0) useMac = BrickPadBt::peerMacFromFd(m_ctrlFd);
    if (useMac.empty() && m_btReady) {
        // TV tự nối vào (inbound): tìm MAC đang Connected qua BlueZ.
        auto devs = BrickPadBt::instance().enumerate();
        for (auto &d : devs) {
            if (d.connected) { useMac = d.mac; break; }
        }
    }
    if (!useMac.empty()) {
        m_peerMac = useMac;
        std::string realName = BrickPadBt::instance().queryDeviceName(useMac);
        if (realName.empty() || isMacAddress(realName)) realName = peerName();
        if (isMacAddress(realName)) realName = "";
        m_store.devTouch(useMac, realName);
        saveStore();
        refreshDevs();
        std::string nm = peerName();
        toast("Đã kết nối " + (nm.empty() ? useMac : nm), 1);
    } else {
        toast("Thiết bị đã kết nối", 1);
    }
    m_discBanner = false;
    if (m_scanning) leaveScan();
    goScreen(BpScreen::DEVICES);
    m_cursor = 0;
    // Tự động trust thiết bị đã kết nối để BlueZ cho phép kết nối lại ngay lập tức
    if (!useMac.empty()) {
        std::string devPath = BrickPadBt::instance().devPathFor(useMac);
        if (!devPath.empty()) {
            BrickPadBt::instance().setTrusted(devPath, true);
        }
    }
    // Gửi report trung lập hiện tại ngay để host nhận diện sạch sẽ
    sendNeutral();
    BrickPadBt::optimizeLinkPolicy(useMac);
    syncBluetoothKeys();
}

void BrickPadManager::onDisconnected(bool userInitiated) {
    if (m_ctrlFd >= 0) { close(m_ctrlFd); m_ctrlFd = -1; }
    if (m_intrFd >= 0) { close(m_intrFd); m_intrFd = -1; }
    m_reportValid = false;
    if (!userInitiated && !m_peerMac.empty()) {
        m_discBanner = true;
        toast("Mất kết nối — vào Thiết bị đã lưu, bấm A để nối lại", 2);
    }
    refreshDevs();
}

std::string BrickPadManager::peerName() const {
    for (auto &d : m_devs) {
        if (!strcasecmp(d.mac.c_str(), m_peerMac.c_str()) && !d.name.empty() && !isMacAddress(d.name))
            return d.name;
    }
    for (const auto &sd : m_store.devices) {
        std::string a, b;
        BrickPadStore::normalizeMac(sd.address, a);
        BrickPadStore::normalizeMac(m_peerMac, b);
        if (a == b && !sd.alias.empty() && !isMacAddress(sd.alias))
            return sd.alias;
    }
    if (m_btReady && !m_peerMac.empty()) {
        std::string qn = BrickPadBt::instance().queryDeviceName(m_peerMac);
        if (!qn.empty() && !isMacAddress(qn)) return qn;
    }
    return m_peerMac;
}

int BrickPadManager::pairedCount() const {
    std::lock_guard<std::mutex> lk(m_devsMutex);
    int n = 0;
    for (auto &d : m_devs) if (d.paired) n++;
    return n;
}

// ---- scan ----
void BrickPadManager::enterScan() {
    if (!m_btReady) {
        toast("Bluetooth chưa sẵn sàng", 2);
        return;
    }
    BrickPadBt::instance().startDiscovery();
    m_scanning = true;
    m_scanEndMs = nowMs() + 12000;
    m_lastEnumMs = 0;
    refreshDevs();
}

void BrickPadManager::leaveScan() {
    if (m_scanning && m_btReady) BrickPadBt::instance().stopDiscovery();
    m_scanning = false;
}

void BrickPadManager::refreshDevs() {
    auto devs = m_btReady ? BrickPadBt::instance().enumerate() : std::vector<ScannedDev>{};
    m_devs.clear();
    std::vector<std::string> seenMacs;

    for (auto &d : devs) {
        // Bỏ qua thiết bị không tên khi đang quét (đúng doc BrickPad).
        if (m_scanning && (d.name.empty() || isMacAddress(d.name))) continue;
        DevRow r;
        r.mac = d.mac;
        r.name = d.name;
        r.paired = d.paired;
        r.rssi = d.rssi;
        r.hasRssi = d.hasRssi;
        // Đánh dấu connected nếu MAC trùng peer đang nối.
        std::string a, b;
        BrickPadStore::normalizeMac(d.mac, a);
        BrickPadStore::normalizeMac(m_peerMac, b);
        r.connected = (!a.empty() && a == b && connected());
        if (r.connected) {
            r.paired = true;
            m_store.devTouch(d.mac, r.name);
        } else if (d.paired) {
            m_store.devTouch(d.mac, r.name);
        }

        // Tra cứu alias từ SD store
        for (const auto &sd : m_store.devices) {
            std::string smac;
            BrickPadStore::normalizeMac(sd.address, smac);
            if (smac == a) {
                if (!sd.alias.empty() && !isMacAddress(sd.alias)) r.name = sd.alias;
                r.paired = true; // Thiết bị đã lưu trên SD luôn được coi là đã paired
                break;
            }
        }
        if (r.name.empty() || isMacAddress(r.name)) {
            if (r.paired) {
                std::string qn = BrickPadBt::instance().queryDeviceName(d.mac);
                if (!qn.empty() && !isMacAddress(qn)) {
                    r.name = qn;
                    m_store.devTouch(d.mac, qn);
                }
            }
        }
        if (r.name.empty()) r.name = d.mac;
        seenMacs.push_back(a);
        m_devs.push_back(r);
    }

    // Nếu không trong chế độ scanning, đưa TẤT CẢ thiết bị trong store vào danh sách
    if (!m_scanning) {
        for (const auto &sd : m_store.devices) {
            std::string smac;
            BrickPadStore::normalizeMac(sd.address, smac);
            bool seen = false;
            for (const auto &m : seenMacs) {
                if (m == smac) { seen = true; break; }
            }
            if (!seen && !sd.address.empty()) {
                DevRow r;
                r.mac = sd.address;
                r.name = (!sd.alias.empty() && !isMacAddress(sd.alias)) ? sd.alias : "";
                if (r.name.empty()) {
                    std::string qn = BrickPadBt::instance().queryDeviceName(sd.address);
                    if (!qn.empty() && !isMacAddress(qn)) {
                        r.name = qn;
                    } else {
                        r.name = sd.address;
                    }
                }
                r.paired = true;
                std::string b;
                BrickPadStore::normalizeMac(m_peerMac, b);
                r.connected = (smac == b && connected());
                m_devs.push_back(r);
                seenMacs.push_back(smac);
            }
        }

        // Nếu đang kết nối nhưng m_peerMac chưa có trong danh sách
        if (connected() && !m_peerMac.empty()) {
            std::string pmac;
            BrickPadStore::normalizeMac(m_peerMac, pmac);
            bool seen = false;
            for (const auto &m : seenMacs) {
                if (m == pmac) { seen = true; break; }
            }
            if (!seen) {
                DevRow r;
                r.mac = m_peerMac;
                r.name = peerName();
                if (r.name.empty() || isMacAddress(r.name)) {
                    std::string qn = BrickPadBt::instance().queryDeviceName(m_peerMac);
                    if (!qn.empty() && !isMacAddress(qn)) r.name = qn;
                    else r.name = m_peerMac;
                }
                r.paired = true;
                r.connected = true;
                m_devs.push_back(r);
                seenMacs.push_back(pmac);
                m_store.devTouch(m_peerMac, r.name);
                saveStore();
            }
        }
    }
}

void BrickPadManager::pumpScan() {
    if (!m_scanning) return;
    long now = nowMs();
    if (now >= m_scanEndMs) {
        leaveScan();
        toast(m_devs.empty() ? "Không thấy thiết bị nào" : "Hết giờ quét", 3);
        return;
    }
    if (now - m_lastEnumMs > 2500) {
        m_lastEnumMs = now;
        refreshDevs();
    }
}

long BrickPadManager::scanMsLeft() const {
    if (!m_scanning) return 0;
    long d = m_scanEndMs - nowMs();
    return d > 0 ? d : 0;
}

// ---- pair ----
void BrickPadManager::startPair(const std::string &mac, const std::string &name) {
    if (m_pairBusy.load()) return;
    std::string path = BrickPadBt::instance().devPathFor(mac);
    if (path.empty()) {
        toast("MAC không hợp lệ", 2);
        return;
    }
    m_pairBusy.store(true);
    m_pairHasResult.store(false);
    m_pairMac = mac;
    m_pairName = name;
    std::thread([this, path]() {
        std::string err;
        bool ok = BrickPadBt::instance().pairBlocking(path, err, 30000);
        // Trust + lưu alias sau pair (JustWorks).
        if (ok) {
            BrickPadBt::instance().setTrusted(path, true);
            std::string realName = BrickPadBt::instance().queryDeviceName(m_pairMac);
            if (realName.empty() || isMacAddress(realName)) realName = m_pairName;
            if (!realName.empty() && !isMacAddress(realName)) {
                m_store.devTouch(m_pairMac, realName);
                saveStore();
                m_pairName = realName;
            }
            syncBluetoothKeys();
        }
        m_pairOk = ok;
        m_pairMsg = ok ? ("Đã pair với " + m_pairName)
                       : ("Pair thất bại: " + err);
        m_pairHasResult.store(true);
        m_pairBusy.store(false);
    }).detach();
}

bool BrickPadManager::pollPairResult(bool &ok, std::string &msg) {
    if (!m_pairHasResult.load()) return false;
    m_pairHasResult.store(false);
    ok = m_pairOk;
    msg = m_pairMsg;
    return true;
}

// ---- connect ----
bool BrickPadManager::connectHost(const std::string &mac, std::string &err) {
    if (!m_btReady) { err = "Bluetooth chưa sẵn sàng"; return false; }
    int c1 = BrickPadBt::connectL2(mac, 0x11);
    int c2 = BrickPadBt::connectL2(mac, 0x13);
    if (c1 < 0 || c2 < 0) {
        if (c1 >= 0) close(c1);
        if (c2 >= 0) close(c2);
        err = "Kết nối thất bại (pair trước từ TV?)";
        return false;
    }
    if (m_ctrlFd >= 0) close(m_ctrlFd);
    if (m_intrFd >= 0) close(m_intrFd);
    if (c2 >= 0) {
        int sndbuf = 512;
        setsockopt(c2, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    }
    m_ctrlFd = c1;
    m_intrFd = c2;
    m_peerMac = mac;
    onConnected(mac);
    return true;
}

void BrickPadManager::disconnectHost() {
    if (!connected()) return;
    if (!m_peerMac.empty()) {
        std::string path = BrickPadBt::instance().devPathFor(m_peerMac);
        if (!path.empty()) BrickPadBt::instance().callDevice0(path, "Disconnect");
    }
    onDisconnected(true);
    toast("Đã ngắt kết nối", 0);
}

bool BrickPadManager::removePaired(const std::string &mac) {
    std::string want, have;
    BrickPadStore::normalizeMac(mac, want);
    BrickPadStore::normalizeMac(m_peerMac, have);
    if (!want.empty() && want == have && connected()) disconnectHost();
    std::string path = BrickPadBt::instance().devPathFor(mac);
    if (!path.empty() && m_btReady) {
        BrickPadBt::instance().callDevice0(path, "Disconnect");
        BrickPadBt::instance().removeDevice(path);
    }
    m_store.removeDevice(mac);
    m_store.bindClear(mac);
    saveStore();
    syncBluetoothKeys();
    refreshDevs();
    return true;
}

bool BrickPadManager::renameDevice(const std::string &mac, const std::string &alias) {
    std::string path = BrickPadBt::instance().devPathFor(mac);
    if (!path.empty() && m_btReady) BrickPadBt::instance().setAlias(path, alias);
    m_store.renameDevice(mac, alias);
    m_store.devTouch(mac, alias);
    saveStore();
    refreshDevs();
    return true;
}

// ---- HID pump ----
static BrickInput sampleInput() {
    BrickInput in;
    auto &im = InputManager::instance();
    in.dpadUp = im.isPureDpadUp();
    in.dpadDown = im.isPureDpadDown();
    in.dpadLeft = im.isPureDpadLeft();
    in.dpadRight = im.isPureDpadRight();
    in.btnA = im.isButtonPressed(Button::A);
    in.btnB = im.isButtonPressed(Button::B);
    in.btnX = im.isButtonPressed(Button::X);
    in.btnY = im.isButtonPressed(Button::Y);
    in.btnL1 = im.isButtonPressed(Button::L1);
    in.btnR1 = im.isButtonPressed(Button::R1);
    in.btnL2 = im.isButtonPressed(Button::L2);
    in.btnR2 = im.isButtonPressed(Button::R2);
    in.btnSelect = im.isButtonPressed(Button::SELECT);
    in.btnStart = im.isButtonPressed(Button::START);
    in.btnHome = im.isButtonPressed(Button::MENU);
    in.stickLX = im.getStickLX();
    in.stickLY = im.getStickLY();
    in.stickRX = im.getStickRX();
    in.stickRY = im.getStickRY();
    return in;
}

void BrickPadManager::sendNeutral() {
    if (m_intrFd < 0) return;
    uint8_t r[32];
    size_t len = packNeutralReport(m_identity, r, sizeof(r));
    if (len > 0) {
        BrickPadBt::sendGamepad(m_intrFd, r, len);
        std::memcpy(m_lastReport, r, len);
        m_lastReportLen = len;
        m_reportValid = true;
    }
    uint8_t mouseReportId = (m_identity == ControllerIdentity::PC_LAPTOP) ? 0x01 : 0x03;
    BrickPadBt::sendMouse(m_intrFd, 0, 0, 0, 0, mouseReportId);
    uint8_t z[6] = {0};
    BrickPadBt::sendKb(m_intrFd, 0, z);
    if (m_identity == ControllerIdentity::PC_LAPTOP) {
        BrickPadBt::sendConsumer(m_intrFd, 0);
    }
    m_lastMouseButtons = 0;
    std::memset(m_lastKbKeys, 0, 6);
    m_lastConsumerUsage = 0;
    m_lastWheelMs = 0;
    m_mouseFilterX = 0.0f;
    m_mouseFilterY = 0.0f;
    m_mouseAccumX = 0.0f;
    m_mouseAccumY = 0.0f;
}

void BrickPadManager::pumpHid() {
    bool conn = (m_intrFd >= 0);
    if (!conn || m_vkOpen || m_qmOpen) {
        m_comboStartMs = 0;
        m_l1r1StartMs = 0;
        return;
    }

    // Bấm nút FN trái: ngắt kết nối HID và quay về RomCloud UI
    if (InputManager::instance().isFnLeftTriggered()) {
        disconnectHost();
        sendNeutral();
        toast("Đã ngắt kết nối HID", 0);
        return;
    }

    BrickInput cur = sampleInput();
    m_lastSentInput = cur;

    // Giữ SELECT + START 1 giây để chuyển đổi chế độ Tay cầm / Bàn phím
    if (cur.btnSelect && cur.btnStart) {
        long now = nowMs();
        if (m_comboStartMs == 0) {
            m_comboStartMs = now;
        } else if (now - m_comboStartMs >= 1000) {
            toggleMode();
            m_comboStartMs = now + 500;
            sendNeutral();
            return;
        }
    } else {
        m_comboStartMs = 0;
    }

    // Platform PC / Laptop: Bộ Chuột + Bàn phím đa phương tiện hoàn chỉnh
    // Report ID 0x01: Chuột (A = Left, B = Right, D-pad Up/Down = Wheel, Stick = Pointer)
    // Chỉ vào chế độ Chuột + Bàn phím KHI user chọn PadMode::KEYBOARD!
    // Khi ở PadMode::GAMEPAD, tuyệt đối gửi Gamepad report chuẩn.
    if (m_mode == PadMode::KEYBOARD && m_identity == ControllerIdentity::PC_LAPTOP) {
        // 1. Cần Analog Trái: di chuyển chuột mượt mà, chống trôi lố (Instant Brake + Coalescing)
        int rawX = (int)cur.stickLX - 128;
        int rawY = (int)cur.stickLY - 128;
        if (std::abs(rawX) <= 12 && std::abs(rawY) <= 12) {
            int rx = (int)cur.stickRX - 128;
            int ry = (int)cur.stickRY - 128;
            if (std::abs(rx) > 12 || std::abs(ry) > 12) {
                rawX = rx;
                rawY = ry;
            }
        }

        // Kiểm tra vùng nghỉ phần cứng: nếu cần ở tâm thì phanh khựng tức thì
        bool stickAtCenter = (std::abs(rawX) <= 15 && std::abs(rawY) <= 15);
        int8_t dx = 0;
        int8_t dy = 0;

        if (stickAtCenter) {
            m_mouseFilterX = 0.0f;
            m_mouseFilterY = 0.0f;
            m_mouseAccumX = 0.0f;
            m_mouseAccumY = 0.0f;
        } else {
            // EMA Low-Pass Filter: lọc rung lắc ADC chỉ khi đang di chuyển
            m_mouseFilterX = 0.65f * (float)rawX + 0.35f * m_mouseFilterX;
            m_mouseFilterY = 0.65f * (float)rawY + 0.35f * m_mouseFilterY;

            // Chuẩn hóa Vector 2D [-1.0 .. 1.0]
            float normX = m_mouseFilterX / 127.0f;
            float normY = m_mouseFilterY / 127.0f;
            float mag = std::sqrt(normX * normX + normY * normY);

            const float kDeadzone = 0.12f; // ~15 counts / 127 bảo đảm buông tay dừng ngay
            if (mag > kDeadzone) {
                if (mag > 1.0f) mag = 1.0f;
                float normalizedMag = (mag - kDeadzone) / (1.0f - kDeadzone);
                float dirX = normX / mag;
                float dirY = normY / mag;

                int speedSetting = m_store.mouseSpeed;
                if (speedSetting < 1) speedSetting = 1;
                if (speedSetting > 10) speedSetting = 10;

                // Vận tốc tối đa tính theo setting: 5 px/frame (speed 1) đến 25 px/frame (speed 10)
                float maxSpeed = 5.0f + (float)(speedSetting - 1) * 2.2f;

                // Đường cong lũy thừa 1.8: đẩy nhẹ cực chính xác từng pixel, đẩy mạnh lướt nhanh
                float curvedMag = std::pow(normalizedMag, 1.8f);
                float targetDx = dirX * curvedMag * maxSpeed;
                float targetDy = dirY * curvedMag * maxSpeed;

                // Sub-pixel Accumulator: tích lũy số dư thập phân qua từng frame
                m_mouseAccumX += targetDx;
                m_mouseAccumY += targetDy;

                dx = (int8_t)m_mouseAccumX;
                dy = (int8_t)m_mouseAccumY;

                m_mouseAccumX -= (float)dx;
                m_mouseAccumY -= (float)dy;
            }
        }

        // 2. Nút B: Chuột trái (0x01), Nút A: Chuột phải (0x02)
        uint8_t mouseBtns = 0;
        if (cur.btnB) mouseBtns |= 0x01; // Left Click
        if (cur.btnA) mouseBtns |= 0x02; // Right Click

        // 3. Chuột (Report ID 0x01):
        int8_t wheel = 0;
        uint8_t mouseReportId = (m_identity == ControllerIdentity::PC_LAPTOP) ? 0x01 : 0x03;
        if (dx != 0 || dy != 0 || wheel != 0 || mouseBtns != m_lastMouseButtons) {
            if (BrickPadBt::sendMouse(m_intrFd, mouseBtns, dx, dy, wheel, mouseReportId)) {
                m_lastMouseButtons = mouseBtns;
            } else {
                // Nếu buffer socket tạm thời bận, giữ lại delta để gộp vào frame sau (Coalescing)
                m_mouseAccumX += (float)dx;
                m_mouseAccumY += (float)dy;
            }
        }

        // 4. Bàn phím (Report ID 0x02):
        // D-pad Lên/Xuống/Trái/Phải: 4 phím mũi tên điều hướng
        // L1: Page Up, R1: Page Down
        // Nút X: Backspace
        // Nút Y: Space
        // Nút Select: Enter, Nút Start: Spacebar
        uint8_t kbKeys[6] = {0, 0, 0, 0, 0, 0};
        int ki = 0;
        if (cur.dpadUp && ki < 6)    kbKeys[ki++] = KU_UP;
        if (cur.dpadDown && ki < 6)  kbKeys[ki++] = KU_DOWN;
        if (cur.dpadLeft && ki < 6)  kbKeys[ki++] = KU_LEFT;
        if (cur.dpadRight && ki < 6) kbKeys[ki++] = KU_RIGHT;
        if (cur.btnL1 && ki < 6)     kbKeys[ki++] = KU_PGUP;
        if (cur.btnR1 && ki < 6)     kbKeys[ki++] = KU_PGDN;
        if (cur.btnX && ki < 6)      kbKeys[ki++] = KU_BKSP;
        if (cur.btnY && ki < 6)      kbKeys[ki++] = KU_SPACE;
        if (!(cur.btnSelect && cur.btnStart)) {
            if (cur.btnSelect && ki < 6) kbKeys[ki++] = KU_ENTER;
            if (cur.btnStart && ki < 6)  kbKeys[ki++] = KU_SPACE;
        }

        if (std::memcmp(kbKeys, m_lastKbKeys, 6) != 0) {
            BrickPadBt::sendKb(m_intrFd, 0, kbKeys);
            std::memcpy(m_lastKbKeys, kbKeys, 6);
        }

        // 5. Điều khiển đa phương tiện (Consumer Report ID 0x03 - chỉ có trên PC_LAPTOP):
        // L2: Vol +, R2: Vol - (Phím MENU dành cho mở Bàn phím ảo)
        if (m_identity == ControllerIdentity::PC_LAPTOP) {
            uint16_t consumerUsage = CU_NONE;
            if (cur.btnL2) consumerUsage = CU_VOL_UP;
            else if (cur.btnR2) consumerUsage = CU_VOL_DOWN;

            if (consumerUsage != m_lastConsumerUsage) {
                BrickPadBt::sendConsumer(m_intrFd, consumerUsage);
                m_lastConsumerUsage = consumerUsage;
            }
        }

        m_prevInput = cur;
        return;
    }

    // Report gamepad theo Platform đã chọn (PlayStation DualShock 4, Xbox One S, Nintendo, BrickPad)
    uint8_t r[32];
    size_t len = packGamepadReport(m_identity, cur, r, sizeof(r));

    m_lastHat = hatOf(cur.dpadUp, cur.dpadDown, cur.dpadLeft, cur.dpadRight);
    if (!m_reportValid || len != m_lastReportLen || std::memcmp(r, m_lastReport, len) != 0) {
        if (!BrickPadBt::sendGamepad(m_intrFd, r, len)) {
            onDisconnected(false);
        } else {
            std::memcpy(m_lastReport, r, len);
            m_lastReportLen = len;
            m_reportValid = true;
        }
    }
    m_prevInput = cur;
}

void BrickPadManager::pumpCombo() {
    // Giữ chỗ: combo đã xử lý trong pumpHid (cần input snapshot).
}

// ---- bàn phím ảo RomCloud -> HID ----
void BrickPadManager::typeText(const std::string &s) {
    if (m_intrFd < 0) { toast("Chưa kết nối", 2); return; }
    int sent = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        int nb = 1;
        uint32_t cp = c;
        if (c < 0x80) { cp = c; nb = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            cp = ((uint32_t)(c & 0x1F) << 6) | (s[i + 1] & 0x3F); nb = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            cp = ((uint32_t)(c & 0x0F) << 12) |
                 ((uint32_t)(s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
            nb = 3;
        } else {
            i++;
            continue;
        }
        i += (size_t)nb;
        if (cp >= 128) continue; // HID chỉ gửi Latin — bỏ dấu TV
        uint8_t mod = 0;
        int u = kbUsageFor((char)cp, false, mod);
        if (u >= 0) {
            BrickPadBt::kbTap(m_intrFd, mod, (uint8_t)u);
            sent++;
        }
        usleep(8000); // host kịp xử lý từng phím
    }
    if (sent > 0) toast("Đã gõ " + std::to_string(sent) + " ký tự", 1);
}

void BrickPadManager::setMode(PadMode m) {
    m_mode = m;
    m_store.lastMode = (int)m_mode;
    saveStore();
}

// ---- quick menu ----
void BrickPadManager::toggleMode() {
    m_mode = (m_mode == PadMode::GAMEPAD) ? PadMode::KEYBOARD : PadMode::GAMEPAD;
    m_store.lastMode = (int)m_mode;
    saveStore();
    toast(m_mode == PadMode::KEYBOARD ? "Chế độ: Bàn phím" : "Chế độ: Tay cầm", 0);
}

void BrickPadManager::openQuickMenu() {
    m_qmOpen = true;
    m_qmRow = 0;
    m_qmOpenedMs = nowMs();
    pokeActivity();
    sendNeutral();
}

void BrickPadManager::closeQuickMenu() {
    m_qmOpen = false;
    m_l1r1StartMs = 0;
}

void BrickPadManager::quickMenuNextPreset() {
    int n = m_store.systemCount();
    if (n <= 0) return;
    int ni = (m_store.activeSysIdx() + 1) % n;
    m_store.setActiveSystem(ni);
    if (!m_peerMac.empty()) m_store.bindSet(m_peerMac, m_store.sysKeyAt(ni));
    saveStore();
    m_qmOpenedMs = nowMs(); // thao tác = còn dùng menu
    toast(std::string("Preset: ") + m_store.sysLabelAt(ni), 1);
}

// ---- preset edit ----
void BrickPadManager::editBegin(int user, int sys) {
    editUser = user; editSys = sys;
    if (user >= 0 && user < (int)m_store.users.size() &&
        sys >= 0 && sys < (int)m_store.users[(size_t)user].systems.size()) {
        editKm = m_store.users[(size_t)user].systems[(size_t)sys].km;
        editValid = true;
    } else {
        editValid = false;
    }
}

void BrickPadManager::editSave() {
    if (!editValid) return;
    m_store.users[(size_t)editUser].systems[(size_t)editSys].km = editKm;
    saveStore();
    editValid = false;
}

// ---- BT name / power ----
void BrickPadManager::setBtName(const std::string &n) {
    if (n.empty()) return;
    m_btName = n;
    FILE *f = std::fopen(m_btNamePath.c_str(), "w");
    if (f) { std::fprintf(f, "%s\n", m_btName.c_str()); std::fclose(f); }
    if (m_btReady) BrickPadBt::instance().applyBtName(m_btName);
    toast("Tên Bluetooth: " + m_btName, 1);
}

void BrickPadManager::setBtOn(bool on) {
    m_btOn = on;
    if (m_btReady) {
        BrickPadBt::instance().setPower(on);
        if (on) {
            applyPlatformLiveAsync();
        }
    }
    if (!on && connected()) disconnectHost();
}

// ---- toast ----
void BrickPadManager::toast(const std::string &t, int kind) {
    if (m_toasts.size() > 4) m_toasts.erase(m_toasts.begin());
    m_toasts.push_back({t, kind});
}

bool BrickPadManager::pollToast(ToastMsg &out) {
    if (m_toasts.empty()) return false;
    out = m_toasts.front();
    m_toasts.erase(m_toasts.begin());
    return true;
}

void BrickPadManager::onInputEvent() {
    if (m_intrFd >= 0 && !m_qmOpen && !m_vkOpen) {
        pumpHid();
    }
}

void BrickPadManager::applyPlatformLiveAsync() {
    if (!m_btReady) return;
    std::thread([this]() {
        IdentityInfo info = getIdentityInfo(m_identity);
        BrickPadBt::instance().setIdentity(m_identity);
        BrickPadBt::instance().applyBtName(info.btName);
        BrickPadBt::btSetupFull(info.btName, m_identity);
        BrickPadBt::instance().reregisterProfiles(m_identity);
        BrickPadBt::maintainAdvertising();
    }).detach();
}

void BrickPadManager::setIdentity(ControllerIdentity id, bool applyLive) {
    bool changed = (m_identity != id);
    if (changed && connected()) {
        disconnectHost();
    }
    m_identity = id;
    m_store.lastPlatform = m_identity;
    IdentityInfo info = getIdentityInfo(m_identity);
    m_btName = info.btName;
    FILE *f = std::fopen(m_btNamePath.c_str(), "w");
    if (f) {
        std::fprintf(f, "%s\n", m_btName.c_str());
        std::fclose(f);
    }
    if (m_btReady && changed && applyLive) {
        BrickPadBt::instance().setIdentity(m_identity);
        BrickPadBt::instance().applyBtName(m_btName);
        BrickPadBt::btSetupFull(m_btName, m_identity);
        BrickPadBt::instance().reregisterProfiles(m_identity);
    }

    saveStore();
}

void BrickPadManager::syncDevicePlatform(const std::string &mac) {
    if (mac.empty()) return;
    ControllerIdentity devId = m_store.deviceIdentity(mac);
    if (m_identity != devId) {
        setIdentity(devId, false);
        applyPlatformLiveAsync();
    }
}

const std::vector<std::string> &BrickPadManager::renameSuggest() {
    static const std::vector<std::string> v = {
        "TV phòng khách", "MoGo văn phòng", "TV phòng ngủ",
        "Box phòng khách", "PC", "Mac", "Giữ nguyên"};
    return v;
}

const std::vector<std::string> &BrickPadManager::btNameSuggest() {
    static const std::vector<std::string> v = {
        "Brick Pad", "BrickPad", "TrimUI Brick", "Xbox Wireless Controller",
        "Wireless Controller", "Gamepad"};
    return v;
}

void BrickPadManager::startConnect(const std::string &mac) {
    if (m_connectBusy.load() || connected() || mac.empty()) return;
    m_connectBusy.store(true);
    std::thread([this, mac]() {
        std::string err;
        LOGI("startConnect: connecting to " + mac);
        bool ok = connectHost(mac, err);
        m_connectBusy.store(false);
        if (ok) {
            toast("Đã kết nối " + peerName(), 1);
        } else {
            toast("Không thể nối: " + err, 2);
        }
    }).detach();
}

void BrickPadManager::triggerAutoConnect() {
    if (!m_btReady || connected() || m_connectBusy.load()) return;
    int ui = m_store.activeUserIdx();
    if (ui >= 0 && ui < (int)m_store.users.size()) {
        if (!m_store.users[ui].autoConnect) return;
    }
    const BtDevice *last = m_store.lastConnectedDevice();
    if (!last || last->address.empty()) return;

    std::string mac = last->address;
    std::string name = last->alias.empty() ? mac : last->alias;
    LOGI("Auto-connect triggered for " + name + " (" + mac + ")");
    startConnect(mac);
}

bool BrickPadManager::autoConnectEnabled() const {
    int ui = m_store.activeUserIdx();
    if (ui >= 0 && ui < (int)m_store.users.size()) {
        return m_store.users[ui].autoConnect;
    }
    return true;
}

void BrickPadManager::setAutoConnectEnabled(bool en) {
    int ui = m_store.activeUserIdx();
    if (ui >= 0 && ui < (int)m_store.users.size()) {
        m_store.users[ui].autoConnect = en;
        saveStore();
        toast(en ? "Tự động kết nối: BẬT" : "Tự động kết nối: TẮT", 0);
    }
}

std::string BrickPadManager::userNameAt(int i) const {
    if (i >= 0 && i < (int)m_store.users.size()) return m_store.users[i].name;
    return "";
}

void BrickPadManager::switchUser(int i) {
    if (i >= 0 && i < (int)m_store.users.size()) {
        m_store.setActiveUser(i);
        m_identity = m_store.users[i].defaultIdentity;
        saveStore();
        if (m_btReady) {
            applyPlatformLiveAsync();
        }
        toast("Người dùng: " + m_store.users[i].name, 1);
    }
}

bool BrickPadManager::addUser(const std::string &name) {
    bool ok = m_store.addUser(name, ControllerIdentity::XBOX);
    if (ok) {
        saveStore();
        toast("Đã thêm người dùng: " + name, 1);
    } else {
        toast("Không thể thêm người dùng", 2);
    }
    return ok;
}

bool BrickPadManager::removeUser(int i) {
    bool ok = m_store.removeUser(i);
    if (ok) {
        saveStore();
        toast("Đã xóa người dùng", 1);
    } else {
        toast("Không thể xóa người dùng", 2);
    }
    return ok;
}

bool BrickPadManager::renameUser(int i, const std::string &name) {
    bool ok = m_store.renameUser(i, name);
    if (ok) {
        saveStore();
        toast("Đã đổi tên: " + name, 1);
    }
    return ok;
}

bool BrickPadManager::removePreset(int sysIdx) {
    int ui = m_store.activeUserIdx();
    bool ok = m_store.removeSystem(ui, sysIdx);
    if (ok) {
        saveStore();
        toast("Đã xóa preset", 1);
    }
    return ok;
}

bool BrickPadManager::renamePreset(int sysIdx, const std::string &label) {
    int ui = m_store.activeUserIdx();
    bool ok = m_store.renameSystem(ui, sysIdx, label);
    if (ok) {
        saveStore();
        toast("Đã đổi tên preset: " + label, 1);
    }
    return ok;
}

void BrickPadManager::resetCurrentPresets() {
    int ui = m_store.activeUserIdx();
    m_store.resetUserDefaults(ui);
    saveStore();
    toast("Đã khôi phục preset về mặc định", 1);
}

void BrickPadManager::syncBluetoothKeys() {
    std::string keyDir = AppConfig::instance().getDataDir() + "/bluetooth_keys";
    std::string cmd = "mkdir -p '" + keyDir + "' 2>/dev/null; "
                      "if [ -d /var/lib/bluetooth ]; then cp -af /var/lib/bluetooth/. '" + keyDir + "/' 2>/dev/null; fi; "
                      "if [ -d /etc/bluetooth/keys ]; then cp -af /etc/bluetooth/keys/. '" + keyDir + "/' 2>/dev/null; fi; "
                      "sync";
    std::system(cmd.c_str());
}

} // namespace BrickPad
} // namespace RomCloud
