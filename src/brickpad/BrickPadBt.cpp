// BrickPadBt — port brickpad.c (D-Bus + ProfileManager + L2CAP + HIDP).
//
// Build trên máy Brick (cross aarch64): cần headers libdbus (giống Makefile
// của BrickPad):
//   -I/tmp/dbusarm/dev/usr/include/dbus-1.0
//   -I/tmp/dbusarm/dev/usr/lib/aarch64-linux-gnu/dbus-1.0/include
// Link runtime lib trên máy: /tmp/dbusarm/rt/.../libdbus-1.so.3 (SONAME
// libdbus-1.so.3, stock OS đã có sẵn).
// Build PC Simulator (không có dbus): stub — BT báo chưa sẵn sàng, UI +
// preset vẫn chạy đầy đủ để test layout.
#include "BrickPadBt.h"
#include "BrickPadStore.h"

#if __has_include(<dbus/dbus.h>)
#define BRICKPAD_HAS_DBUS 1
#include <dbus/dbus.h>
#else
#define BRICKPAD_HAS_DBUS 0
#endif

#include "../logging/Logger.h"

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace RomCloud {
namespace BrickPad {

namespace {
const char *TAG = "BrickPadBt";
__attribute__((unused)) void LOGI(const std::string &s) { Logger::info(std::string("[") + TAG + "] " + s); }
__attribute__((unused)) void LOGW(const std::string &s) { Logger::warn(std::string("[") + TAG + "] " + s); }

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_L2CAP 0
struct sockaddr_l2 {
    uint16_t family, psm;
    uint8_t bdaddr[6];
    uint16_t cid;
    uint8_t type;
};

#define HID_PSM_CTRL 0x11
#define HID_PSM_INTR 0x13
#define HID_UUID_STR "00001124-0000-1000-8000-00805f9b34fb"
#define DID_UUID_STR "00001200-0000-1000-8000-00805f9b34fb"
#define PROFILE_PATH "/com/brickpad/hid/ctrl"
#define DID_PROFILE_PATH "/com/brickpad/did"
#define AGENT_PATH "/com/brickpad/hid/agent"

// Agent NoInputNoOutput để pair JustWorks (gamepad không màn hình/PIN).
#if BRICKPAD_HAS_DBUS
DBusHandlerResult agentHandler(DBusConnection *c, DBusMessage *m, void *) {
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *iface = dbus_message_get_interface(m);
    const char *member = dbus_message_get_member(m);
    if (iface && std::strcmp(iface, "org.bluez.Agent1"))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    LOGI(std::string("agentHandler: member=") + (member ? member : "null"));
    DBusMessage *rep = nullptr;
    if (member && !std::strcmp(member, "RequestPinCode")) {
        rep = dbus_message_new_method_return(m);
        const char *pin = "0000";
        dbus_message_append_args(rep, DBUS_TYPE_STRING, &pin, DBUS_TYPE_INVALID);
    } else if (member && !std::strcmp(member, "RequestPasskey")) {
        rep = dbus_message_new_method_return(m);
        dbus_uint32_t passkey = 0;
        dbus_message_append_args(rep, DBUS_TYPE_UINT32, &passkey, DBUS_TYPE_INVALID);
    } else if (member && (!std::strcmp(member, "Release") || !std::strcmp(member, "Cancel") ||
                   !std::strcmp(member, "DisplayPinCode") || !std::strcmp(member, "DisplayPasskey") ||
                   !std::strcmp(member, "RequestConfirmation") || !std::strcmp(member, "RequestAuthorization") ||
                   !std::strcmp(member, "AuthorizeService"))) {
        // Tự động trust device nếu có device path trong method call
        const char *devPath = nullptr;
        DBusMessageIter dit;
        if (dbus_message_iter_init(m, &dit) && dbus_message_iter_get_arg_type(&dit) == DBUS_TYPE_OBJECT_PATH) {
            dbus_message_iter_get_basic(&dit, &devPath);
            if (devPath && *devPath) {
                BrickPadBt::instance().setTrusted(devPath, true);
            }
        }
        rep = dbus_message_new_method_return(m); // chấp nhận hết (JustWorks)
    } else {
        rep = dbus_message_new_error(m, "org.bluez.Error.NotSupported", "khong ho tro");
    }
    if (rep) {
        dbus_connection_send(c, rep, nullptr);
        dbus_connection_flush(c);
        dbus_message_unref(rep);
    }
    return rep ? DBUS_HANDLER_RESULT_HANDLED : DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

struct NewConnCtx {
    int *ctrlFd;
    std::string *devPath;
};

DBusHandlerResult profileHandler(DBusConnection *c, DBusMessage *m, void *ud) {
    auto *ctx = (NewConnCtx *)ud;
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *iface = dbus_message_get_interface(m);
    const char *member = dbus_message_get_member(m);
    if (!iface || std::strcmp(iface, "org.bluez.Profile1"))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    DBusMessage *rep = nullptr;
    if (member && !std::strcmp(member, "NewConnection")) {
        if (!ctx || !ctx->ctrlFd || !ctx->devPath) {
            rep = dbus_message_new_method_return(m);
        } else {
        DBusMessageIter it;
        const char *dev = "";
        int fd = -1;
        dbus_message_iter_init(m, &it);
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH) {
            dbus_message_iter_get_basic(&it, &dev);
            dbus_message_iter_next(&it);
        }
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UNIX_FD)
            dbus_message_iter_get_basic(&it, &fd);
        LOGI(std::string("NewConnection device=") + dev);
        if (fd >= 0) {
            int nf = dup(fd);
            if (nf >= 0) {
                int fl = fcntl(nf, F_GETFL, 0);
                fcntl(nf, F_SETFL, fl | O_NONBLOCK);
                if (*ctx->ctrlFd >= 0) close(*ctx->ctrlFd);
                *ctx->ctrlFd = nf;
                *ctx->devPath = dev ? dev : "";
            }
        }
        rep = dbus_message_new_method_return(m);
        }
    } else if (member && (!std::strcmp(member, "Release") || !std::strcmp(member, "RequestDisconnection"))) {
        if (!std::strcmp(member, "RequestDisconnection") && ctx && ctx->ctrlFd && *ctx->ctrlFd >= 0) {
            close(*ctx->ctrlFd);
            *ctx->ctrlFd = -1;
        }
        rep = dbus_message_new_method_return(m);
    }
    if (rep) {
        dbus_connection_send(c, rep, nullptr);
        dbus_connection_flush(c);
        dbus_message_unref(rep);
    }
    return rep ? DBUS_HANDLER_RESULT_HANDLED : DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void dictStr(DBusMessageIter *dict, const char *k, const char *v) {
    DBusMessageIter e, va;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "s", &va);
    dbus_message_iter_append_basic(&va, DBUS_TYPE_STRING, &v);
    dbus_message_iter_close_container(&e, &va);
    dbus_message_iter_close_container(dict, &e);
}
void dictBool(DBusMessageIter *dict, const char *k, int v) {
    dbus_bool_t b = v ? TRUE : FALSE;
    DBusMessageIter e, va;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "b", &va);
    dbus_message_iter_append_basic(&va, DBUS_TYPE_BOOLEAN, &b);
    dbus_message_iter_close_container(&e, &va);
    dbus_message_iter_close_container(dict, &e);
}

std::string buildHidXml(const std::string &btName, ControllerIdentity identity) {
    static const char *H = "0123456789abcdef";
    uint16_t mapLen = 0;
    const uint8_t *map = getReportMap(identity, mapLen);
    std::string hex;
    hex.reserve(mapLen * 2);
    for (size_t i = 0; i < mapLen; i++) {
        hex += H[map[i] >> 4];
        hex += H[map[i] & 15];
    }
    uint8_t subclass = (identity == ControllerIdentity::PC_LAPTOP) ? 0xc0 : 0x08;
    const char *serviceDesc = (identity == ControllerIdentity::PC_LAPTOP) ? "Keyboard/Mouse Combo" : "Gamepad";
    char *xml = (char *)std::malloc(8192);
    std::snprintf(xml, 8192,
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<record>\n"
        "  <attribute id=\"0x0001\"><sequence><uuid value=\"0x1124\"/></sequence></attribute>\n"
        "  <attribute id=\"0x0004\"><sequence>"
        "<sequence><uuid value=\"0x0100\"/><uint16 value=\"0x0011\"/></sequence>"
        "<sequence><uuid value=\"0x0011\"/></sequence></sequence></attribute>\n"
        "  <attribute id=\"0x0005\"><sequence><uuid value=\"0x1002\"/></sequence></attribute>\n"
        "  <attribute id=\"0x0006\"><sequence>"
        "<uint16 value=\"0x0409\"/><uint16 value=\"0x006a\"/><uint16 value=\"0x0100\"/>"
        "</sequence></attribute>\n"
        "  <attribute id=\"0x0009\"><sequence><sequence>"
        "<uuid value=\"0x1124\"/><uint16 value=\"0x0100\"/>"
        "</sequence></sequence></attribute>\n"
        "  <attribute id=\"0x000d\"><sequence><sequence>"
        "<sequence><uuid value=\"0x0100\"/><uint16 value=\"0x0013\"/></sequence>"
        "<sequence><uuid value=\"0x0011\"/></sequence>"
        "</sequence></sequence></attribute>\n"
        "  <attribute id=\"0x0100\"><text value=\"%s\"/></attribute>\n"
        "  <attribute id=\"0x0101\"><text value=\"%s\"/></attribute>\n"
        "  <attribute id=\"0x0102\"><text value=\"RomCloud\"/></attribute>\n"
        "  <attribute id=\"0x0200\"><uint16 value=\"0x0100\"/></attribute>\n"
        "  <attribute id=\"0x0201\"><uint16 value=\"0x0111\"/></attribute>\n"
        "  <attribute id=\"0x0202\"><uint8 value=\"0x%02x\"/></attribute>\n"
        "  <attribute id=\"0x0203\"><uint8 value=\"0x00\"/></attribute>\n"
        "  <attribute id=\"0x0204\"><boolean value=\"true\"/></attribute>\n"
        "  <attribute id=\"0x0205\"><boolean value=\"true\"/></attribute>\n"
        "  <attribute id=\"0x0206\"><sequence><sequence>"
        "<uint8 value=\"0x22\"/><text encoding=\"hex\" value=\"%s\"/>"
        "</sequence></sequence></attribute>\n"
        "  <attribute id=\"0x0207\"><sequence><sequence>"
        "<uint16 value=\"0x0409\"/><uint16 value=\"0x0100\"/>"
        "</sequence></sequence></attribute>\n"
        "  <attribute id=\"0x0209\"><boolean value=\"false\"/></attribute>\n"
        "  <attribute id=\"0x020a\"><boolean value=\"true\"/></attribute>\n"
        "  <attribute id=\"0x020b\"><uint16 value=\"0x0100\"/></attribute>\n"
        "  <attribute id=\"0x020c\"><uint16 value=\"0x0010\"/></attribute>\n"
        "  <attribute id=\"0x020d\"><boolean value=\"true\"/></attribute>\n"
        "  <attribute id=\"0x020e\"><boolean value=\"false\"/></attribute>\n"
        "</record>\n",
        btName.c_str(), serviceDesc, subclass, hex.c_str());
    std::string out = xml;
    std::free(xml);
    return out;
}

std::string buildDidXml(uint16_t vid, uint16_t pid, uint16_t version) {
    char xml[2048];
    std::snprintf(xml, sizeof(xml),
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<record>\n"
        "  <attribute id=\"0x0001\"><sequence><uuid value=\"0x1200\"/></sequence></attribute>\n"
        "  <attribute id=\"0x0004\"><sequence>"
        "<sequence><uuid value=\"0x0100\"/></sequence>"
        "<sequence><uuid value=\"0x0001\"/></sequence></sequence></attribute>\n"
        "  <attribute id=\"0x0005\"><sequence><uuid value=\"0x1002\"/></sequence></attribute>\n"
        "  <attribute id=\"0x0009\"><sequence><sequence>"
        "<uuid value=\"0x1200\"/><uint16 value=\"0x0103\"/>"
        "</sequence></sequence></attribute>\n"
        "  <attribute id=\"0x0100\"><text value=\"Device Identification Service\"/></attribute>\n"
        "  <attribute id=\"0x0201\"><uint16 value=\"0x0103\"/></attribute>\n"
        "  <attribute id=\"0x0202\"><uint16 value=\"0x%04x\"/></attribute>\n"
        "  <attribute id=\"0x0203\"><uint16 value=\"0x%04x\"/></attribute>\n"
        "  <attribute id=\"0x0204\"><uint16 value=\"0x%04x\"/></attribute>\n"
        "  <attribute id=\"0x0205\"><boolean value=\"true\"/></attribute>\n"
        "  <attribute id=\"0x0206\"><uint16 value=\"0x0002\"/></attribute>\n"
        "</record>\n",
        vid, pid, version);
    return std::string(xml);
}

} // namespace

namespace {
// D-Bus connection + profile ctx (sống suốt vòng đời init->shutdown).
DBusConnection *g_conn = nullptr;
NewConnCtx g_newConn{nullptr, nullptr};
} // namespace

BrickPadBt &BrickPadBt::instance() {
    static BrickPadBt s;
    return s;
}

void BrickPadBt::btSetupFull(const std::string &name, ControllerIdentity identity) {
    LOGI("BT setup: up + name + class + scan + pairable + low-latency lp");

    IdentityInfo info = getIdentityInfo(identity);
    char buf[2048];
    const char *cod = (identity == ControllerIdentity::PC_LAPTOP) ? "0x0025c0" : "0x002508";

    std::snprintf(buf, sizeof(buf),
        "killall -9 trimui_btmanager script 2>/dev/null; "
        "sed -i 's/^#*DeviceID =.*/DeviceID = usb:%04x:%04x:%04x/' /etc/bluetooth/main.conf 2>/dev/null; "
        "sed -i 's/^#*Class =.*/Class = %s/' /etc/bluetooth/main.conf 2>/dev/null; "
        "sed -i 's/^#*Name =.*/Name = %s/' /etc/bluetooth/main.conf 2>/dev/null; "
        "hciconfig hci0 up 2>/dev/null; "
        "hciconfig hci0 lp rswitch 2>/dev/null; "
        "hciconfig hci0 name '%s' 2>/dev/null; "
        "btmgmt name '%s' </dev/null >/dev/null 2>&1; "
        "hciconfig hci0 class %s 2>/dev/null; "
        "hciconfig hci0 sspmode 1 2>/dev/null; "
        "hciconfig hci0 inqmode 2 2>/dev/null; "
        "hciconfig hci0 inqtype 1 2>/dev/null; "
        "hciconfig hci0 inqparms 64:256 2>/dev/null; "
        "hciconfig hci0 pageparms 64:256 2>/dev/null; "
        "hciconfig hci0 piscan 2>/dev/null; "
        "btmgmt power on </dev/null >/dev/null 2>&1; "
        "btmgmt ssp on </dev/null >/dev/null 2>&1; "
        "btmgmt connectable on </dev/null >/dev/null 2>&1; "
        "hciconfig hci0 noauth 2>/dev/null; "
        "btmgmt linksec off </dev/null >/dev/null 2>&1; "
        "btmgmt bondable on </dev/null >/dev/null 2>&1; "
        "btmgmt pairable on </dev/null >/dev/null 2>&1; "
        "btmgmt discov on </dev/null >/dev/null 2>&1; "
        "bluetoothctl system-alias '%s' </dev/null >/dev/null 2>&1; "
        "bluetoothctl discoverable-timeout 0 </dev/null >/dev/null 2>&1; "
        "bluetoothctl discoverable on </dev/null >/dev/null 2>&1; "
        "bluetoothctl pairable-timeout 0 </dev/null >/dev/null 2>&1; "
        "bluetoothctl pairable on </dev/null >/dev/null 2>&1;",
        info.vid, info.pid, info.version,
        cod,
        name.c_str(),
        name.c_str(),
        name.c_str(),
        cod,
        name.c_str()
    );
    std::system(buf);
}

void BrickPadBt::maintainAdvertising() {
    // Watchdog keep-alive: bảo đảm hci0 luôn giữ PSCAN ISCAN và discoverable + pairable ở cả hci + mgmt + bluetoothctl
    std::system(
        "killall -9 trimui_btmanager script 2>/dev/null; "
        "hciconfig hci0 piscan 2>/dev/null; "
        "btmgmt connectable on </dev/null >/dev/null 2>&1; "
        "btmgmt discov on </dev/null >/dev/null 2>&1; "
        "btmgmt pairable on </dev/null >/dev/null 2>&1; "
        "bluetoothctl discoverable on </dev/null >/dev/null 2>&1; "
        "bluetoothctl pairable on </dev/null >/dev/null 2>&1;"
    );
}

void BrickPadBt::optimizeLinkPolicy(const std::string &mac) {
    std::system("hciconfig hci0 lp rswitch 2>/dev/null");
    if (!mac.empty()) {
        std::string cmd = "hcitool lp " + mac + " RSWITCH 2>/dev/null";
        std::system(cmd.c_str());
    }
}

bool BrickPadBt::init(const std::string &btName, ControllerIdentity identity) {
    if (m_ready) return true;
    dbus_threads_init_default();
    m_identity = identity;
    IdentityInfo idInfo = getIdentityInfo(m_identity);
    m_btName = btName.empty() ? idInfo.btName : btName;
    btSetupFull(m_btName, m_identity);

    DBusError err;
    dbus_error_init(&err);
    g_conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!g_conn) {
        LOGW(std::string("dbus system bus: ") + (err.message ? err.message : "?"));
        dbus_error_free(&err);
        return false;
    }
    static const DBusObjectPathVTable vt = {nullptr, profileHandler, nullptr};
    dbus_connection_unregister_object_path(g_conn, PROFILE_PATH);
    if (!dbus_connection_register_object_path(g_conn, PROFILE_PATH, &vt, &g_newConn)) {
        LOGW("register profile object path failed");
        return false;
    }
    // Agent NoInputNoOutput cho pair JustWorks.
    {
        static const DBusObjectPathVTable avt = {nullptr, agentHandler, nullptr};
        const char *apath = AGENT_PATH, *cap = "NoInputNoOutput";
        dbus_connection_unregister_object_path(g_conn, apath);
        if (dbus_connection_register_object_path(g_conn, apath, &avt, nullptr)) {
            DBusMessage *am = dbus_message_new_method_call(
                "org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent");
            DBusMessageIter ait;
            dbus_message_iter_init_append(am, &ait);
            dbus_message_iter_append_basic(&ait, DBUS_TYPE_OBJECT_PATH, &apath);
            dbus_message_iter_append_basic(&ait, DBUS_TYPE_STRING, &cap);
            DBusMessage *arep =
                dbus_connection_send_with_reply_and_block(g_conn, am, 8000, &err);
            dbus_message_unref(am);
            bool agentOk = false;
            if (arep) {
                dbus_message_unref(arep);
                agentOk = true;
            } else if (dbus_error_has_name(&err, "org.bluez.Error.AlreadyExists")) {
                agentOk = true;
                dbus_error_free(&err);
                dbus_error_init(&err);
            } else {
                dbus_error_free(&err);
                dbus_error_init(&err);
            }
            if (agentOk) {
                DBusMessage *dm = dbus_message_new_method_call(
                    "org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent");
                DBusMessageIter dit;
                dbus_message_iter_init_append(dm, &dit);
                dbus_message_iter_append_basic(&dit, DBUS_TYPE_OBJECT_PATH, &apath);
                DBusMessage *drep =
                    dbus_connection_send_with_reply_and_block(g_conn, dm, 8000, &err);
                dbus_message_unref(dm);
                if (drep) {
                    dbus_message_unref(drep);
                    LOGI("agent NoInputNoOutput san sang");
                } else {
                    dbus_error_free(&err);
                    dbus_error_init(&err);
                }
            }
        }
    }
    // 0. Huỷ đăng ký profile cũ trước nếu còn tồn tại từ phiên trước
    {
        DBusMessage *um = dbus_message_new_method_call(
            "org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "UnregisterProfile");
        const char *upath = PROFILE_PATH;
        DBusMessageIter uit;
        dbus_message_iter_init_append(um, &uit);
        dbus_message_iter_append_basic(&uit, DBUS_TYPE_OBJECT_PATH, &upath);
        DBusMessage *urep = dbus_connection_send_with_reply_and_block(g_conn, um, 1500, &err);
        dbus_message_unref(um);
        if (urep) dbus_message_unref(urep);
        else dbus_error_free(&err);
        dbus_error_init(&err);
    }
    {
        DBusMessage *um = dbus_message_new_method_call(
            "org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "UnregisterProfile");
        const char *upath = DID_PROFILE_PATH;
        DBusMessageIter uit;
        dbus_message_iter_init_append(um, &uit);
        dbus_message_iter_append_basic(&uit, DBUS_TYPE_OBJECT_PATH, &upath);
        DBusMessage *urep = dbus_connection_send_with_reply_and_block(g_conn, um, 1500, &err);
        dbus_message_unref(um);
        if (urep) dbus_message_unref(urep);
        else dbus_error_free(&err);
        dbus_error_init(&err);
    }

    std::string xml = buildHidXml(m_btName, m_identity);
    {
        DBusMessage *m = dbus_message_new_method_call(
            "org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "RegisterProfile");
        DBusMessageIter it, dict;
        const char *path = PROFILE_PATH, *uuid = HID_UUID_STR;
        dbus_message_iter_init_append(m, &it);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &path);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &uuid);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dictStr(&dict, "Name", m_btName.c_str());
        dictStr(&dict, "ServiceRecord", xml.c_str());
        dictStr(&dict, "Role", "server");
        dictBool(&dict, "RequireAuthentication", 0);
        dictBool(&dict, "RequireAuthorization", 0);
        dictBool(&dict, "AutoConnect", 1);
        // KHÔNG truyền PSM: BlueZ build này bind PSM 2 lần rồi báo EADDRINUSE.
        // App tự bind cả 2 PSM.
        dbus_message_iter_close_container(&it, &dict);
        DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, m, 10000, &err);
        dbus_message_unref(m);
        if (!rep) {
            if (dbus_error_has_name(&err, "org.bluez.Error.AlreadyExists")) {
                LOGI("RegisterProfile: Profile da ton tai, tiep tuc su dung");
                dbus_error_free(&err);
                dbus_error_init(&err);
            } else {
                LOGW(std::string("RegisterProfile: ") + (err.message ? err.message : "?"));
                dbus_error_free(&err);
                return false;
            }
        } else {
            dbus_message_unref(rep);
            LOGI("SDP: HID record da dang ky");
        }
    }
    {
        std::string didXml = buildDidXml(idInfo.vid, idInfo.pid, idInfo.version);
        DBusMessage *dm = dbus_message_new_method_call(
            "org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "RegisterProfile");
        DBusMessageIter dit, ddict;
        const char *didPath = DID_PROFILE_PATH, *didUuid = DID_UUID_STR;
        dbus_message_iter_init_append(dm, &dit);
        dbus_message_iter_append_basic(&dit, DBUS_TYPE_OBJECT_PATH, &didPath);
        dbus_message_iter_append_basic(&dit, DBUS_TYPE_STRING, &didUuid);
        dbus_message_iter_open_container(&dit, DBUS_TYPE_ARRAY, "{sv}", &ddict);
        dictStr(&ddict, "Name", "Device Identification Service");
        dictStr(&ddict, "ServiceRecord", didXml.c_str());
        dictStr(&ddict, "Role", "server");
        dictBool(&ddict, "RequireAuthentication", 0);
        dictBool(&ddict, "RequireAuthorization", 0);
        dictBool(&ddict, "AutoConnect", 0);
        dbus_message_iter_close_container(&dit, &ddict);
        DBusMessage *drep = dbus_connection_send_with_reply_and_block(g_conn, dm, 8000, &err);
        dbus_message_unref(dm);
        if (drep) {
            dbus_message_unref(drep);
            LOGI("SDP: Device ID record da dang ky");
        } else {
            dbus_error_free(&err);
            dbus_error_init(&err);
        }
    }
    m_ready = true;
    return true;
}

bool BrickPadBt::reregisterProfiles(ControllerIdentity identity) {
#if BRICKPAD_HAS_DBUS
    if (!g_conn) return false;
    m_identity = identity;
    IdentityInfo idInfo = getIdentityInfo(m_identity);
    m_btName = idInfo.btName;
    DBusError err;
    dbus_error_init(&err);

    // 1. Huỷ đăng ký profile cũ
    {
        DBusMessage *m = dbus_message_new_method_call(
            "org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "UnregisterProfile");
        DBusMessageIter it;
        const char *path = PROFILE_PATH;
        dbus_message_iter_init_append(m, &it);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &path);
        DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, m, 2000, &err);
        if (rep) dbus_message_unref(rep);
        dbus_message_unref(m);
        dbus_error_free(&err);
        dbus_error_init(&err);
    }
    {
        DBusMessage *dm = dbus_message_new_method_call(
            "org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "UnregisterProfile");
        DBusMessageIter dit;
        const char *didPath = DID_PROFILE_PATH;
        dbus_message_iter_init_append(dm, &dit);
        dbus_message_iter_append_basic(&dit, DBUS_TYPE_OBJECT_PATH, &didPath);
        DBusMessage *drep = dbus_connection_send_with_reply_and_block(g_conn, dm, 2000, &err);
        if (drep) dbus_message_unref(drep);
        dbus_message_unref(dm);
        dbus_error_free(&err);
        dbus_error_init(&err);
    }

    // 2. Đăng ký lại HID profile với Descriptor của platform mới
    std::string xml = buildHidXml(m_btName, m_identity);
    {
        DBusMessage *m = dbus_message_new_method_call(
            "org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "RegisterProfile");
        DBusMessageIter it, dict;
        const char *path = PROFILE_PATH, *uuid = HID_UUID_STR;
        dbus_message_iter_init_append(m, &it);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &path);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &uuid);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dictStr(&dict, "Name", m_btName.c_str());
        dictStr(&dict, "ServiceRecord", xml.c_str());
        dictStr(&dict, "Role", "server");
        dictBool(&dict, "RequireAuthentication", 0);
        dictBool(&dict, "RequireAuthorization", 0);
        dictBool(&dict, "AutoConnect", 1);
        dbus_message_iter_close_container(&it, &dict);
        DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, m, 5000, &err);
        dbus_message_unref(m);
        if (rep) {
            dbus_message_unref(rep);
            LOGI("SDP: HID record da cap nhat cho platform: " + m_btName);
        } else {
            dbus_error_free(&err);
            dbus_error_init(&err);
        }
    }

    // 3. Đăng ký lại DID profile với VID/PID mới
    {
        std::string didXml = buildDidXml(idInfo.vid, idInfo.pid, idInfo.version);
        DBusMessage *dm = dbus_message_new_method_call(
            "org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "RegisterProfile");
        DBusMessageIter dit, ddict;
        const char *didPath = DID_PROFILE_PATH, *didUuid = DID_UUID_STR;
        dbus_message_iter_init_append(dm, &dit);
        dbus_message_iter_append_basic(&dit, DBUS_TYPE_OBJECT_PATH, &didPath);
        dbus_message_iter_append_basic(&dit, DBUS_TYPE_STRING, &didUuid);
        dbus_message_iter_open_container(&dit, DBUS_TYPE_ARRAY, "{sv}", &ddict);
        dictStr(&ddict, "Name", "Device Identification Service");
        dictStr(&ddict, "ServiceRecord", didXml.c_str());
        dictStr(&ddict, "Role", "server");
        dictBool(&ddict, "RequireAuthentication", 0);
        dictBool(&ddict, "RequireAuthorization", 0);
        dictBool(&ddict, "AutoConnect", 0);
        dbus_message_iter_close_container(&dit, &ddict);
        DBusMessage *drep = dbus_connection_send_with_reply_and_block(g_conn, dm, 5000, &err);
        dbus_message_unref(dm);
        if (drep) {
            dbus_message_unref(drep);
            LOGI("SDP: Device ID record da cap nhat: VID=0x" + std::to_string(idInfo.vid) + " PID=0x" + std::to_string(idInfo.pid));
        } else {
            dbus_error_free(&err);
            dbus_error_init(&err);
        }
    }
    return true;
#else
    (void)identity;
    return false;
#endif
}

void BrickPadBt::shutdown() {
    if (!g_conn) {
        m_ready = false;
        return;
    }
    DBusError err;
    dbus_error_init(&err);
    {
        DBusMessage *m = dbus_message_new_method_call(
            "org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "UnregisterProfile");
        DBusMessageIter it;
        const char *path = PROFILE_PATH;
        dbus_message_iter_init_append(m, &it);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &path);
        DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, m, 5000, &err);
        if (rep) dbus_message_unref(rep);
        dbus_message_unref(m);
        dbus_error_free(&err);
        dbus_error_init(&err);
    }
    {
        DBusMessage *dm = dbus_message_new_method_call(
            "org.bluez", "/org/bluez", "org.bluez.ProfileManager1", "UnregisterProfile");
        DBusMessageIter dit;
        const char *didPath = DID_PROFILE_PATH;
        dbus_message_iter_init_append(dm, &dit);
        dbus_message_iter_append_basic(&dit, DBUS_TYPE_OBJECT_PATH, &didPath);
        DBusMessage *drep = dbus_connection_send_with_reply_and_block(g_conn, dm, 5000, &err);
        if (drep) dbus_message_unref(drep);
        dbus_message_unref(dm);
        dbus_error_free(&err);
        dbus_error_init(&err);
    }
    dbus_connection_unref(g_conn);
    g_conn = nullptr;
    m_ready = false;
}

std::vector<ScannedDev> BrickPadBt::enumerate() {
    std::vector<ScannedDev> out;
    if (!g_conn) return out;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *m = dbus_message_new_method_call(
        "org.bluez", "/org/bluez", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, m, 8000, &err);
    dbus_message_unref(m);
    if (!rep) {
        dbus_error_free(&err);
        return out;
    }
    DBusMessageIter top, oarr, oe, iarr, ie, parr, pe, pv;
    dbus_message_iter_init(rep, &top);
    if (dbus_message_iter_get_arg_type(&top) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(rep);
        return out;
    }
    dbus_message_iter_recurse(&top, &oarr);
    while (dbus_message_iter_get_arg_type(&oarr) == DBUS_TYPE_DICT_ENTRY &&
           (int)out.size() < 32) {
        dbus_message_iter_recurse(&oarr, &oe);
        const char *opath = "";
        if (dbus_message_iter_get_arg_type(&oe) == DBUS_TYPE_OBJECT_PATH)
            dbus_message_iter_get_basic(&oe, &opath);
        bool isDev = std::strstr(opath, "/dev_") != nullptr;
        dbus_message_iter_next(&oe);
        if (isDev && dbus_message_iter_get_arg_type(&oe) == DBUS_TYPE_ARRAY) {
            ScannedDev d;
            d.path = opath ? opath : "";
            std::string propName, propAlias;
            dbus_message_iter_recurse(&oe, &iarr);
            while (dbus_message_iter_get_arg_type(&iarr) == DBUS_TYPE_DICT_ENTRY) {
                dbus_message_iter_recurse(&iarr, &ie);
                const char *iface = "";
                if (dbus_message_iter_get_arg_type(&ie) == DBUS_TYPE_STRING)
                    dbus_message_iter_get_basic(&ie, &iface);
                dbus_message_iter_next(&ie);
                if (!std::strcmp(iface, "org.bluez.Device1") &&
                    dbus_message_iter_get_arg_type(&ie) == DBUS_TYPE_ARRAY) {
                    dbus_message_iter_recurse(&ie, &parr);
                    while (dbus_message_iter_get_arg_type(&parr) == DBUS_TYPE_DICT_ENTRY) {
                        dbus_message_iter_recurse(&parr, &pe);
                        const char *k = "";
                        if (dbus_message_iter_get_arg_type(&pe) == DBUS_TYPE_STRING)
                            dbus_message_iter_get_basic(&pe, &k);
                        dbus_message_iter_next(&pe);
                        if (dbus_message_iter_get_arg_type(&pe) == DBUS_TYPE_VARIANT) {
                            dbus_message_iter_recurse(&pe, &pv);
                            int at = dbus_message_iter_get_arg_type(&pv);
                            const char *vs = "";
                            if (at == DBUS_TYPE_STRING) {
                                dbus_message_iter_get_basic(&pv, &vs);
                                if (!std::strcmp(k, "Address")) d.mac = vs ? vs : "";
                                else if (!std::strcmp(k, "Name")) propName = vs ? vs : "";
                                else if (!std::strcmp(k, "Alias")) propAlias = vs ? vs : "";
                            } else if (at == DBUS_TYPE_BOOLEAN) {
                                dbus_bool_t vb = FALSE;
                                dbus_message_iter_get_basic(&pv, &vb);
                                if (!std::strcmp(k, "Paired")) d.paired = vb ? true : false;
                                else if (!std::strcmp(k, "Connected")) d.connected = vb ? true : false;
                                else if (!std::strcmp(k, "Trusted")) d.trusted = vb ? true : false;
                            } else if (at == DBUS_TYPE_INT16 && !std::strcmp(k, "RSSI")) {
                                dbus_message_iter_get_basic(&pv, &d.rssi);
                                d.hasRssi = true;
                            }
                        }
                        dbus_message_iter_next(&parr);
                    }
                }
                dbus_message_iter_next(&iarr);
            }
            if (!propName.empty()) {
                if (!propAlias.empty() && !isMacAddress(propAlias) && propAlias != propName)
                    d.name = propAlias;
                else
                    d.name = propName;
            } else if (!propAlias.empty() && !isMacAddress(propAlias)) {
                d.name = propAlias;
            } else {
                d.name = "";
            }
            out.push_back(d);
        }
        dbus_message_iter_next(&oarr);
    }
    dbus_message_unref(rep);
    return out;
}

bool BrickPadBt::startDiscovery() {
    if (!g_conn) return false;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *m = dbus_message_new_method_call(
        "org.bluez", "/org/bluez/hci0", "org.bluez.Adapter1", "StartDiscovery");
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, m, 8000, &err);
    dbus_message_unref(m);
    if (!rep) {
        dbus_error_free(&err);
        return false;
    }
    dbus_message_unref(rep);
    return true;
}

bool BrickPadBt::stopDiscovery() {
    if (!g_conn) return false;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *m = dbus_message_new_method_call(
        "org.bluez", "/org/bluez/hci0", "org.bluez.Adapter1", "StopDiscovery");
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, m, 8000, &err);
    dbus_message_unref(m);
    if (!rep) {
        dbus_error_free(&err);
        return false;
    }
    dbus_message_unref(rep);
    return true;
}

std::string BrickPadBt::devPathFor(const std::string &mac) const {
    std::string norm;
    BrickPadStore::normalizeMac(mac, norm);
    if (norm.size() != 12) return "";
    char buf[128];
    std::snprintf(buf, sizeof(buf), "/org/bluez/hci0/dev_%c%c_%c%c_%c%c_%c%c_%c%c_%c%c",
                  norm[0], norm[1], norm[2], norm[3], norm[4], norm[5],
                  norm[6], norm[7], norm[8], norm[9], norm[10], norm[11]);
    return buf;
}

std::string BrickPadBt::queryDeviceName(const std::string &mac) {
    std::string path = devPathFor(mac);
    if (path.empty() || !g_conn) return "";
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *m = dbus_message_new_method_call(
        "org.bluez", path.c_str(), "org.freedesktop.DBus.Properties", "GetAll");
    const char *iface = "org.bluez.Device1";
    dbus_message_append_args(m, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, m, 3000, &err);
    dbus_message_unref(m);
    if (!rep) {
        dbus_error_free(&err);
        return "";
    }
    std::string propName, propAlias;
    DBusMessageIter top, arr, entry, val;
    dbus_message_iter_init(rep, &top);
    if (dbus_message_iter_get_arg_type(&top) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&top, &arr);
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
            dbus_message_iter_recurse(&arr, &entry);
            const char *key = "";
            if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING)
                dbus_message_iter_get_basic(&entry, &key);
            dbus_message_iter_next(&entry);
            if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&entry, &val);
                if (dbus_message_iter_get_arg_type(&val) == DBUS_TYPE_STRING) {
                    const char *vs = "";
                    dbus_message_iter_get_basic(&val, &vs);
                    if (!std::strcmp(key, "Name")) propName = vs ? vs : "";
                    else if (!std::strcmp(key, "Alias")) propAlias = vs ? vs : "";
                }
            }
            dbus_message_iter_next(&arr);
        }
    }
    dbus_message_unref(rep);
    if (!propName.empty()) {
        if (!propAlias.empty() && !isMacAddress(propAlias) && propAlias != propName)
            return propAlias;
        return propName;
    }
    if (!propAlias.empty() && !isMacAddress(propAlias))
        return propAlias;
    return "";
}

std::string BrickPadBt::peerMacFromFd(int fd) {
    if (fd < 0) return "";
    struct sockaddr_l2 a;
    socklen_t len = sizeof(a);
    std::memset(&a, 0, sizeof(a));
    if (getpeername(fd, (struct sockaddr *)&a, &len) == 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                      a.bdaddr[5], a.bdaddr[4], a.bdaddr[3],
                      a.bdaddr[2], a.bdaddr[1], a.bdaddr[0]);
        if (std::strcmp(buf, "00:00:00:00:00:00") != 0) {
            return std::string(buf);
        }
    }
    return "";
}

std::string BrickPadBt::macFromDevPath(const std::string &path) {
    const char *p = std::strstr(path.c_str(), "/dev_");
    if (!p) return "";
    p += 5;
    if (std::strlen(p) < 17) return "";
    char mac[18];
    for (int i = 0; i < 17; i++) {
        mac[i] = (p[i] == '_') ? ':' : p[i];
    }
    mac[17] = '\0';
    return std::string(mac);
}

namespace {
struct PairJobCtx {
    std::string path;
    int timeoutMs;
};
void *pairThreadFn(void *arg) {
    auto *ctx = (PairJobCtx *)arg;
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *c = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
    bool ok = false;
    std::string errStr;
    if (!c) {
        errStr = err.message ? err.message : "dbus";
        dbus_error_free(&err);
    } else {
        DBusMessage *m = dbus_message_new_method_call(
            "org.bluez", ctx->path.c_str(), "org.bluez.Device1", "Pair");
        DBusMessage *rep =
            dbus_connection_send_with_reply_and_block(c, m, ctx->timeoutMs, &err);
        dbus_message_unref(m);
        if (!rep) {
            errStr = err.message ? err.message : "?";
            dbus_error_free(&err);
        } else {
            dbus_message_unref(rep);
            ok = true;
        }
        dbus_connection_close(c);
        dbus_connection_unref(c);
    }
    ctx->path = ok ? "" : errStr; // "" = thành công
    return nullptr;
}
} // namespace

bool BrickPadBt::pairBlocking(const std::string &devPath, std::string &errOut, int timeoutMs) {
    PairJobCtx ctx{devPath, timeoutMs};
    pthread_t th;
    if (pthread_create(&th, nullptr, pairThreadFn, &ctx) != 0) {
        errOut = "Không tạo được pair thread";
        return false;
    }
    pthread_join(th, nullptr);
    if (ctx.path.empty()) return true;
    errOut = ctx.path;
    return false;
}

bool BrickPadBt::setAlias(const std::string &devPath, const std::string &alias) {
    if (!g_conn) return false;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *m = dbus_message_new_method_call(
        "org.bluez", devPath.c_str(), "org.freedesktop.DBus.Properties", "Set");
    DBusMessageIter it, va;
    const char *iface = "org.bluez.Device1", *key = "Alias";
    const char *al = alias.c_str();
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &va);
    dbus_message_iter_append_basic(&va, DBUS_TYPE_STRING, &al);
    dbus_message_iter_close_container(&it, &va);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, m, 8000, &err);
    dbus_message_unref(m);
    if (!rep) {
        dbus_error_free(&err);
        return false;
    }
    dbus_message_unref(rep);
    return true;
}

bool BrickPadBt::setTrusted(const std::string &devPath, bool trusted) {
    if (!g_conn) return false;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *m = dbus_message_new_method_call(
        "org.bluez", devPath.c_str(), "org.freedesktop.DBus.Properties", "Set");
    DBusMessageIter it, va;
    const char *iface = "org.bluez.Device1", *key = "Trusted";
    dbus_bool_t b = trusted ? TRUE : FALSE;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "b", &va);
    dbus_message_iter_append_basic(&va, DBUS_TYPE_BOOLEAN, &b);
    dbus_message_iter_close_container(&it, &va);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, m, 8000, &err);
    dbus_message_unref(m);
    if (!rep) {
        dbus_error_free(&err);
        return false;
    }
    dbus_message_unref(rep);
    return true;
}

bool BrickPadBt::callDevice0(const std::string &devPath, const char *method) {
    if (!g_conn) return false;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *m = dbus_message_new_method_call(
        "org.bluez", devPath.c_str(), "org.bluez.Device1", method);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, m, 10000, &err);
    dbus_message_unref(m);
    if (!rep) {
        dbus_error_free(&err);
        return false;
    }
    dbus_message_unref(rep);
    return true;
}

bool BrickPadBt::removeDevice(const std::string &devPath) {
    if (!g_conn) return false;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *m = dbus_message_new_method_call(
        "org.bluez", "/org/bluez/hci0", "org.bluez.Adapter1", "RemoveDevice");
    DBusMessageIter it;
    const char *p = devPath.c_str();
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &p);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, m, 8000, &err);
    dbus_message_unref(m);
    if (!rep) {
        dbus_error_free(&err);
        return false;
    }
    dbus_message_unref(rep);
    return true;
}

int BrickPadBt::connectL2(const std::string &mac, uint16_t psm) {
    unsigned t[6];
    if (std::sscanf(mac.c_str(), "%02X:%02X:%02X:%02X:%02X:%02X",
                    &t[0], &t[1], &t[2], &t[3], &t[4], &t[5]) != 6 &&
        std::sscanf(mac.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
                    &t[0], &t[1], &t[2], &t[3], &t[4], &t[5]) != 6) {
        return -1;
    }
    int s = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (s < 0) return -1;
    struct timeval to = {5, 0};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
    struct sockaddr_l2 a;
    std::memset(&a, 0, sizeof(a));
    a.family = AF_BLUETOOTH;
    for (int i = 0; i < 6; i++) a.bdaddr[i] = (uint8_t)t[5 - i];
    a.psm = psm;
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    return s;
}

int BrickPadBt::listenPsm(uint16_t psm) {
    int s = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (s < 0) return -1;
    struct sockaddr_l2 a;
    std::memset(&a, 0, sizeof(a));
    a.family = AF_BLUETOOTH;
    a.psm = psm;
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }
    if (listen(s, 2) < 0) {
        close(s);
        return -1;
    }
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    return s;
}

bool BrickPadBt::sendGamepad(int intrFd, const uint8_t *r, size_t len) {
    if (intrFd < 0 || !r || len == 0) return false;
    uint8_t p[32];
    if (len + 1 > sizeof(p)) return false;
    p[0] = 0xA1;
    std::memcpy(p + 1, r, len);
    return write(intrFd, p, len + 1) == (ssize_t)(len + 1);
}

bool BrickPadBt::sendKb(int intrFd, uint8_t mod, const uint8_t keys[6]) {
    if (intrFd < 0) return false;
    uint8_t p[10];
    p[0] = 0xA1; p[1] = 0x02; p[2] = mod; p[3] = 0;
    std::memcpy(p + 4, keys, 6);
    return write(intrFd, p, sizeof(p)) == (ssize_t)sizeof(p);
}

bool BrickPadBt::sendMouse(int intrFd, uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel, uint8_t reportId) {
    if (intrFd < 0) return false;
    uint8_t p[6];
    p[0] = 0xA1;
    p[1] = reportId; // Mouse Report ID (0x01 cho Composite Remote)
    p[2] = buttons;
    p[3] = (uint8_t)dx;
    p[4] = (uint8_t)dy;
    p[5] = (uint8_t)wheel;
    return write(intrFd, p, sizeof(p)) == (ssize_t)sizeof(p);
}

bool BrickPadBt::sendConsumer(int intrFd, uint16_t usage) {
    if (intrFd < 0) return false;
    uint8_t p[4];
    p[0] = 0xA1;
    p[1] = 0x03; // Consumer Report ID 0x03
    p[2] = (uint8_t)(usage & 0xFF);
    p[3] = (uint8_t)(usage >> 8);
    return write(intrFd, p, sizeof(p)) == (ssize_t)sizeof(p);
}

bool BrickPadBt::kbTap(int intrFd, uint8_t mod, uint8_t usage) {
    uint8_t k[6] = {usage, 0, 0, 0, 0, 0};
    if (!sendKb(intrFd, mod, k)) return false;
    uint8_t z[6] = {0, 0, 0, 0, 0, 0};
    sendKb(intrFd, 0, z);
    return true;
}

void BrickPadBt::handleCtrl(int ctrlFd, const uint8_t *cur, size_t curLen) {
    uint8_t b[64];
    ssize_t n = read(ctrlFd, b, sizeof(b));
    if (n <= 0) return;
    uint8_t type = b[0] >> 4, param = b[0] & 0x0F;
    uint8_t rsp[32];
    int rlen = 0;
    switch (type) {
    case 0x1:
        rsp[0] = 0x00; rlen = 1;
        break;
    case 0x4: {
        uint8_t id = n > 1 ? b[1] : 0;
        if ((param & 0x03) == 1 && (id == 0 || id == 1)) {
            rsp[0] = 0xA1;
            size_t copyLen = (curLen > 30) ? 30 : curLen;
            if (cur && copyLen > 0) {
                std::memcpy(rsp + 1, cur, copyLen);
                rlen = (int)(1 + copyLen);
            } else {
                rsp[0] = 0x02; rlen = 1;
            }
        } else if (id == 2) {
            rsp[0] = 0xA1;
            rsp[1] = 0x02;
            std::memset(rsp + 2, 0, 8);
            rlen = 10;
        } else if (id == 3) {
            rsp[0] = 0xA1;
            rsp[1] = 0x03;
            std::memset(rsp + 2, 0, 4);
            rlen = 6;
        } else {
            rsp[0] = 0x02; rlen = 1;
        }
        break;
    }
    case 0x5:
        rsp[0] = 0x00; rlen = 1;
        break;
    case 0x6:
        rsp[0] = 0xA0; rsp[1] = 0x01; rlen = 2;
        break;
    case 0x7:
        rsp[0] = 0x00; rlen = 1;
        break;
    case 0x8:
        rsp[0] = 0xA0; rsp[1] = 0x00; rlen = 2;
        break;
    case 0x9:
        rsp[0] = 0x00; rlen = 1;
        break;
    default:
        rsp[0] = 0x03; rlen = 1;
        break;
    }
    if (rlen > 0) {
        ssize_t w = write(ctrlFd, rsp, (size_t)rlen);
        (void)w;
    }
}

int BrickPadBt::dbusFd() const {
    if (!g_conn) return -1;
    int fd = -1;
    if (!dbus_connection_get_unix_fd(g_conn, &fd)) return -1;
    return fd;
}

void BrickPadBt::pumpDbus() {
    if (!g_conn) return;
    dbus_connection_read_write_dispatch(g_conn, 0);
}

void BrickPadBt::bindNewConnectionSink(int *ctrlFd, std::string *devPath) {
    g_newConn.ctrlFd = ctrlFd;
    g_newConn.devPath = devPath;
}

bool BrickPadBt::applyBtName(const std::string &name) {
    m_btName = name;
    std::string cmd = "hciconfig hci0 name '" + name + "'";
    std::system(cmd.c_str());
    if (!g_conn) return true;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *m = dbus_message_new_method_call(
        "org.bluez", "/org/bluez/hci0", "org.freedesktop.DBus.Properties", "Set");
    DBusMessageIter it, va;
    const char *iface = "org.bluez.Adapter1", *key = "Alias";
    const char *nm = m_btName.c_str();
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "s", &va);
    dbus_message_iter_append_basic(&va, DBUS_TYPE_STRING, &nm);
    dbus_message_iter_close_container(&it, &va);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, m, 8000, &err);
    dbus_message_unref(m);
    if (rep) {
        dbus_message_unref(rep);
        return true;
    }
    dbus_error_free(&err);
    return false;
}

bool BrickPadBt::setPower(bool on) {
    std::string cmd = on ? "hciconfig hci0 up" : "hciconfig hci0 down";
    return std::system(cmd.c_str()) == 0;
}

} // namespace BrickPad
} // namespace RomCloud

#else
// ---- Stub cho PC Simulator (không có libdbus): BT chưa sẵn sàng ----
// (Nhánh này vẫn đang mở namespace RomCloud::BrickPad::{anonymous} từ đầu
// file vì preprocessor đã skip các dòng đóng namespace — đóng bù ở đây.)
} // namespace
} // namespace BrickPad
} // namespace RomCloud
namespace RomCloud {
namespace BrickPad {
BrickPadBt &BrickPadBt::instance() {
    static BrickPadBt s;
    return s;
}
bool BrickPadBt::init(const std::string &, ControllerIdentity) { return false; }
void BrickPadBt::shutdown() {}
std::vector<ScannedDev> BrickPadBt::enumerate() { return {}; }
bool BrickPadBt::startDiscovery() { return false; }
bool BrickPadBt::stopDiscovery() { return false; }
std::string BrickPadBt::devPathFor(const std::string &) const { return ""; }
std::string BrickPadBt::queryDeviceName(const std::string &) { return ""; }
std::string BrickPadBt::peerMacFromFd(int) { return ""; }
std::string BrickPadBt::macFromDevPath(const std::string &) { return ""; }
bool BrickPadBt::pairBlocking(const std::string &, std::string &errOut, int) {
    errOut = "Thiếu libdbus (bản PC)";
    return false;
}
bool BrickPadBt::setAlias(const std::string &, const std::string &) { return false; }
bool BrickPadBt::setTrusted(const std::string &, bool) { return false; }
bool BrickPadBt::callDevice0(const std::string &, const char *) { return false; }
bool BrickPadBt::removeDevice(const std::string &) { return false; }
int BrickPadBt::connectL2(const std::string &, uint16_t) { return -1; }
int BrickPadBt::listenPsm(uint16_t) { return -1; }
bool BrickPadBt::sendGamepad(int, const uint8_t *, size_t) { return false; }
bool BrickPadBt::sendKb(int, uint8_t, const uint8_t[6]) { return false; }
bool BrickPadBt::sendMouse(int, uint8_t, int8_t, int8_t, int8_t, uint8_t) { return false; }
bool BrickPadBt::sendConsumer(int, uint16_t) { return false; }
bool BrickPadBt::kbTap(int, uint8_t, uint8_t) { return false; }
void BrickPadBt::handleCtrl(int, const uint8_t *, size_t) {}
bool BrickPadBt::reregisterProfiles(ControllerIdentity) { return false; }
int BrickPadBt::dbusFd() const { return -1; }
void BrickPadBt::pumpDbus() {}
void BrickPadBt::bindNewConnectionSink(int *, std::string *) {}
bool BrickPadBt::applyBtName(const std::string &) { return false; }
bool BrickPadBt::setPower(bool) { return false; }
void BrickPadBt::btSetupFull(const std::string &, ControllerIdentity) {}
void BrickPadBt::maintainAdvertising() {}
void BrickPadBt::optimizeLinkPolicy(const std::string &) {}
} // namespace BrickPad
} // namespace RomCloud
#endif
