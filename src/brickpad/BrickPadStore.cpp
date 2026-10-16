// BrickPadStore — engine preset + JSON mini-DOM (port ui_fb.h, giữ nguyên
// schema để tương thích presets.json của BrickPad standalone).
#include "BrickPadStore.h"
#include "BrickPadBt.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <ctime>

namespace RomCloud {
namespace BrickPad {

// ---- mini JSON DOM (đủ đọc schema của mình, UTF-8 giữ nguyên) ----
struct JVal {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    long num = 0; bool boolean = false; std::string str;
    std::vector<std::pair<std::string, JVal>> pairs;
    std::vector<JVal> items;
};

namespace {
struct Parser {
    const char *p;
    void skip() { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; }
    JVal val() {
        skip();
        JVal v;
        if (*p == '{') {
            p++; v.t = JVal::OBJ; skip();
            if (*p == '}') { p++; return v; }
            while (true) {
                skip();
                if (*p != '"') break;
                std::string k = str();
                skip();
                if (*p == ':') p++;
                v.pairs.emplace_back(k, val());
                skip();
                if (*p == ',') { p++; continue; }
                if (*p == '}') { p++; break; }
                break;
            }
            return v;
        }
        if (*p == '[') {
            p++; v.t = JVal::ARR; skip();
            if (*p == ']') { p++; return v; }
            while (true) {
                v.items.push_back(val());
                skip();
                if (*p == ',') { p++; continue; }
                if (*p == ']') { p++; break; }
                break;
            }
            return v;
        }
        if (*p == '"') { v.t = JVal::STR; v.str = str(); return v; }
        if (!std::strncmp(p, "true", 4)) { p += 4; v.t = JVal::BOOL; v.boolean = true; return v; }
        if (!std::strncmp(p, "false", 5)) { p += 5; v.t = JVal::BOOL; v.boolean = false; return v; }
        if (!std::strncmp(p, "null", 4)) { p += 4; return v; }
        char *e = nullptr; long n = std::strtol(p, &e, 10);
        if (e != p) { p = e; v.t = JVal::NUM; v.num = n; return v; }
        return v;
    }
    std::string str() { // p đang ở '"'
        p++;
        std::string o;
        while (*p && *p != '"') {
            char c = *p;
            if (c == '\\') {
                p++;
                char e = *p;
                if (e == 'u') {
                    auto hex = [](char h) -> unsigned {
                        if (h >= '0' && h <= '9') return (unsigned)(h - '0');
                        if (h >= 'a' && h <= 'f') return (unsigned)(h - 'a' + 10);
                        if (h >= 'A' && h <= 'F') return (unsigned)(h - 'A' + 10);
                        return 0;
                    };
                    unsigned cp = (hex(p[1]) << 12) | (hex(p[2]) << 8) |
                                  (hex(p[3]) << 4) | hex(p[4]);
                    p += 4;
                    if (cp < 0x80) o += (char)cp;
                    else if (cp < 0x800) {
                        o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63));
                    } else {
                        o += (char)(0xE0 | (cp >> 12));
                        o += (char)(0x80 | ((cp >> 6) & 63));
                        o += (char)(0x80 | (cp & 63));
                    }
                    p++;
                    continue;
                }
                c = (e == 'n') ? '\n' : (e == 't') ? '\t' : (e == 'r') ? '\r' : e;
                p++;
            } else {
                p++;
            }
            o += c;
        }
        if (*p == '"') p++;
        return o;
    }
};

const JVal *jGet(const JVal &o, const char *k) {
    if (o.t != JVal::OBJ) return nullptr;
    for (auto &pr : o.pairs)
        if (pr.first == k) return &pr.second;
    return nullptr;
}
std::string jStr(const JVal &o, const char *k, const char *def) {
    const JVal *v = jGet(o, k);
    return (v && v->t == JVal::STR) ? v->str : def;
}
long jNum(const JVal &o, const char *k, long def) {
    const JVal *v = jGet(o, k);
    if (!v) return def;
    if (v->t == JVal::NUM) return v->num;
    if (v->t == JVal::BOOL) return v->boolean ? 1 : 0;
    return def;
}
// Clamp enum load từ JSON về range hợp lệ (file tay có thể ghi số bậy).
ControllerIdentity clampIdentity(long v) {
    if (v < 0 || v > (long)ControllerIdentity::PC_LAPTOP)
        return ControllerIdentity::XBOX;
    return (ControllerIdentity)v;
}
void jEsc(FILE *f, const std::string &s) {
    fputc('"', f);
    for (char c : s) {
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c == '\n') fputs("\\n", f);
        else fputc(c, f);
    }
    fputc('"', f);
}
} // namespace

void BrickPadStore::normalizeMac(const std::string &in, std::string &out) {
    out.clear();
    for (char c : in) {
        if (std::isxdigit((unsigned char)c) && out.size() < 12)
            out += (char)std::toupper((unsigned char)c);
    }
}

void BrickPadStore::addSystem(BtUser &u, const std::string &system,
                              const std::string &label, const int8_t *map,
                              bool toHat, bool asStick) {
    if ((int)u.systems.size() >= kMaxSystems) return;
    PresetSystem s;
    s.system = system; s.label = label;
    if (map) std::memcpy(s.km.b, map, P_COUNT);
    else for (int i = 0; i < P_COUNT; i++) s.km.b[i] = (int8_t)i;
    s.km.dpadToHat = toHat; s.km.dpadAsLeftStick = asStick;
    u.systems.push_back(s);
}

void BrickPadStore::resetDefaults() {
    users.clear(); devices.clear();
    version = 2;
    activeUser = "tai";
    const int8_t id[P_COUNT]  = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14};
    const int8_t nin[P_COUNT] = {1,0,2,3,4,5,6,7,8,9,10,11,12,13,14};
    const int8_t xb[P_COUNT]  = {1,0,3,2,4,5,6,7,8,9,10,11,12,13,14};
    BtUser t; t.name = "tai";
    t.defaultIdentity = ControllerIdentity::BRICKPAD;
    t.autoConnect = true;
    addSystem(t, "snes", "Mặc định", id, true, false);
    addSystem(t, "nintendo", "Nintendo", nin, true, false);
    addSystem(t, "n64", "N64", id, true, true);
    addSystem(t, "xbox", "XBOX", xb, true, false);
    addSystem(t, "psx", "PLAYSTATION", xb, true, false);
    users.push_back(t);
    activeSys[0] = "snes";
    BtUser k; k.name = "khach";
    k.defaultIdentity = ControllerIdentity::BRICKPAD;
    k.autoConnect = true;
    addSystem(k, "snes", "Mặc định", id, true, false);
    users.push_back(k);
    activeSys[1] = "snes";
    for (int i = 2; i < kMaxUsers; i++) activeSys[i] = "snes";
    mouseSpeed = 3;
}

bool BrickPadStore::load(const std::string &path) {
    resetDefaults();
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return false; // lần đầu: dùng mặc định (save sau)
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 256 * 1024) { std::fclose(f); return false; }
    std::string txt((size_t)sz, '\0');
    if (std::fread(&txt[0], 1, (size_t)sz, f) != (size_t)sz) {
        std::fclose(f); return false;
    }
    std::fclose(f);
    Parser ps{txt.c_str()};
    JVal root = ps.val();
    if (root.t != JVal::OBJ) return false;
    version = (int)jNum(root, "version", 1);
    activeUser = jStr(root, "activeUser", "tai");
    const JVal *au = jGet(root, "activeSystem");
    const JVal *ju = jGet(root, "users");
    if (ju && ju->t == JVal::ARR && !ju->items.empty()) {
        users.clear();
        for (auto &u : ju->items) {
            if (u.t != JVal::OBJ || (int)users.size() >= kMaxUsers) continue;
            BtUser bu;
            bu.name = jStr(u, "name", "?");
            bu.defaultIdentity = clampIdentity(jNum(u, "defaultIdentity", (long)ControllerIdentity::XBOX));
            bu.autoConnect = (jNum(u, "autoConnect", 1) != 0);
            const JVal *sy = jGet(u, "systems");
            if (sy && sy->t == JVal::ARR) {
                for (auto &s : sy->items) {
                    if (s.t != JVal::OBJ || (int)bu.systems.size() >= kMaxSystems) continue;
                    PresetSystem psys;
                    psys.system = jStr(s, "system", "?");
                    psys.label = jStr(s, "label", psys.system.c_str());
                    for (int b = 0; b < P_COUNT; b++) psys.km.b[b] = (int8_t)b;
                    psys.km.dpadToHat = true; psys.km.dpadAsLeftStick = false;
                    const JVal *km = jGet(s, "keymap");
                    if (km && km->t == JVal::OBJ) {
                        const JVal *bb = jGet(*km, "buttons");
                        if (bb && bb->t == JVal::OBJ) {
                            for (int b = 0; b < P_COUNT; b++) {
                                const JVal *v = jGet(*bb, kPhysKeys[b]);
                                if (!v) continue;
                                if (v->t == JVal::NUM && v->num >= 0 && v->num <= 15)
                                    psys.km.b[b] = (int8_t)v->num;
                                else if (v->t == JVal::STR && !v->str.empty() &&
                                         (v->str[0] == 'X' || v->str[0] == 'x'))
                                    psys.km.b[b] = -1;
                            }
                        }
                        psys.km.dpadToHat = jNum(*km, "dpadToHat", 1) != 0;
                        psys.km.dpadAsLeftStick = jNum(*km, "dpadAsLeftStick", 0) != 0;
                    }
                    bu.systems.push_back(psys);
                }
            }
            users.push_back(bu);
        }
        if (au && au->t == JVal::OBJ) {
            for (auto &pr : au->pairs) {
                int ui = userIndex(pr.first);
                if (ui >= 0 && pr.second.t == JVal::STR)
                    activeSys[ui] = pr.second.str;
            }
        }
    }
    lastPlatform = clampIdentity(jNum(root, "lastPlatform", (long)ControllerIdentity::XBOX));
    lastMode = (int)jNum(root, "lastMode", 0);
    if (lastMode < 0 || lastMode > 1) lastMode = 0;
    mouseSpeed = (int)jNum(root, "mouseSpeed", 3);
    if (mouseSpeed < 1) mouseSpeed = 1;
    if (mouseSpeed > 10) mouseSpeed = 10;
    const JVal *jd = jGet(root, "devices");
    if (jd && jd->t == JVal::ARR) {
        devices.clear();
        for (auto &d : jd->items) {
            if (d.t != JVal::OBJ || (int)devices.size() >= kMaxDevs) continue;
            BtDevice dev;
            dev.address = jStr(d, "address", "");
            dev.alias = jStr(d, "alias", "");
            dev.system = jStr(d, "system", "");
            dev.lastConnected = jStr(d, "lastConnected", "");
            dev.identity = clampIdentity(jNum(d, "identity", (long)ControllerIdentity::XBOX));
            dev.mode = (int)jNum(d, "mode", 0);
            devices.push_back(dev);
        }
    }
    // Migrate 1 lần: hệ xbox cũ bật dpadAsLeftStick để RetroArch nhận cần analog.
    // Chỉ chạy khi version < 2 (file cũ), rồi nâng version để không ghi đè user.
    if (version < 2) {
        for (auto &u : users) {
            for (auto &sy : u.systems) {
                if (sy.system == "xbox") {
                    sy.km.dpadToHat = true;
                    sy.km.dpadAsLeftStick = true;
                }
            }
        }
        version = 2;
    }
    return true;
}

bool BrickPadStore::save(const std::string &path) const {
    FILE *f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "{\n  \"version\": %d,\n  ", version);
    jEsc(f, "activeUser"); std::fprintf(f, ": ");
    jEsc(f, activeUser);
    std::fprintf(f, ",\n  \"activeSystem\": {");
    for (size_t i = 0; i < users.size(); i++) {
        if (i) std::fprintf(f, ", ");
        jEsc(f, users[i].name); std::fprintf(f, ": ");
        jEsc(f, activeSys[i]);
    }
    std::fprintf(f, "},\n  \"users\": [\n");
    for (size_t i = 0; i < users.size(); i++) {
        const BtUser &u = users[i];
        std::fprintf(f, "    {\"name\": ");
        jEsc(f, u.name);
        std::fprintf(f, ", \"defaultIdentity\": %d, \"autoConnect\": %s, \"systems\": [\n",
                     (int)u.defaultIdentity, u.autoConnect ? "true" : "false");
        for (size_t s = 0; s < u.systems.size(); s++) {
            const PresetSystem &sy = u.systems[s];
            std::fprintf(f, "      {\"system\": ");
            jEsc(f, sy.system);
            std::fprintf(f, ", \"label\": ");
            jEsc(f, sy.label);
            std::fprintf(f, ", \"keymap\": {\"buttons\": {");
            for (int b = 0; b < P_COUNT; b++) {
                if (b) std::fprintf(f, ", ");
                std::fprintf(f, "\"%s\": ", kPhysKeys[b]);
                if (sy.km.b[b] < 0) std::fprintf(f, "\"X\"");
                else std::fprintf(f, "%d", sy.km.b[b]);
            }
            std::fprintf(f, "}, \"dpadToHat\": %s, \"dpadAsLeftStick\": %s}}%s\n",
                    sy.km.dpadToHat ? "true" : "false",
                    sy.km.dpadAsLeftStick ? "true" : "false",
                    s + 1 < u.systems.size() ? "," : "");
        }
        std::fprintf(f, "    ]}%s\n", i + 1 < users.size() ? "," : "");
    }
    std::fprintf(f, "  ],\n  \"devices\": [\n");
    for (size_t i = 0; i < devices.size(); i++) {
        const BtDevice &d = devices[i];
        std::fprintf(f, "    {\"address\": ");
        jEsc(f, d.address);
        std::fprintf(f, ", \"alias\": ");
        jEsc(f, d.alias);
        std::fprintf(f, ", \"system\": ");
        jEsc(f, d.system);
        std::fprintf(f, ", \"lastConnected\": ");
        jEsc(f, d.lastConnected);
        std::fprintf(f, ", \"identity\": %d, \"mode\": %d", (int)d.identity, d.mode);
        std::fprintf(f, "}%s\n", i + 1 < devices.size() ? "," : "");
    }
    std::fprintf(f, "  ],\n  \"lastPlatform\": %d,\n  \"lastMode\": %d,\n  \"mouseSpeed\": %d\n}\n",
                 (int)lastPlatform, lastMode, mouseSpeed);
    std::fclose(f);
    return true;
}

int BrickPadStore::userIndex(const std::string &name) const {
    for (size_t i = 0; i < users.size(); i++)
        if (users[i].name == name) return (int)i;
    return -1;
}
int BrickPadStore::activeUserIdx() const {
    int i = userIndex(activeUser);
    return (i >= 0) ? i : 0;
}
int BrickPadStore::sysIndex(int userIdx, const std::string &system) const {
    if (userIdx < 0 || userIdx >= (int)users.size()) return -1;
    for (size_t i = 0; i < users[userIdx].systems.size(); i++)
        if (users[userIdx].systems[i].system == system) return (int)i;
    return -1;
}
int BrickPadStore::activeSysIdx() const {
    int ui = activeUserIdx();
    if (ui < 0 || ui >= (int)users.size()) return -1;
    int si = sysIndex(ui, activeSys[ui]);
    return (si >= 0) ? si : 0;
}
const Keymap &BrickPadStore::activeKeymap() const {
    int ui = activeUserIdx(), si = activeSysIdx();
    static Keymap identity;
    static bool init = false;
    if (!init) {
        for (int i = 0; i < P_COUNT; i++) identity.b[i] = (int8_t)i;
        identity.dpadToHat = true; identity.dpadAsLeftStick = false;
        init = true;
    }
    if (ui < 0 || si < 0) return identity;
    return users[(size_t)ui].systems[(size_t)si].km;
}
int BrickPadStore::systemCount() const {
    int ui = activeUserIdx();
    return (ui >= 0 && ui < (int)users.size()) ? (int)users[(size_t)ui].systems.size() : 0;
}
const char *BrickPadStore::sysLabelAt(int i) const {
    int ui = activeUserIdx();
    if (ui < 0 || ui >= (int)users.size()) return "?";
    if (i < 0 || i >= (int)users[(size_t)ui].systems.size()) return "?";
    return users[(size_t)ui].systems[(size_t)i].label.c_str();
}
const char *BrickPadStore::sysKeyAt(int i) const {
    int ui = activeUserIdx();
    if (ui < 0 || ui >= (int)users.size()) return "";
    if (i < 0 || i >= (int)users[(size_t)ui].systems.size()) return "";
    return users[(size_t)ui].systems[(size_t)i].system.c_str();
}
void BrickPadStore::setActiveSystem(int i) {
    int ui = activeUserIdx();
    if (ui < 0 || ui >= (int)users.size()) return;
    if (i >= 0 && i < (int)users[(size_t)ui].systems.size())
        activeSys[ui] = users[(size_t)ui].systems[(size_t)i].system;
}
void BrickPadStore::setActiveUser(int i) {
    if (i >= 0 && i < (int)users.size()) activeUser = users[(size_t)i].name;
}
bool BrickPadStore::addSystemClone(int userIdx) {
    if (userIdx < 0 || userIdx >= (int)users.size()) return false;
    BtUser &u = users[(size_t)userIdx];
    if ((int)u.systems.size() >= kMaxSystems) return false;
    int si = activeSysIdx();
    // Sinh key không trùng (đếm custom* hiện có, tránh trùng sau removeSystem).
    int customN = 0;
    for (auto &s : u.systems) {
        if (s.system.compare(0, 6, "custom") == 0)
            customN = std::max(customN, std::atoi(s.system.c_str() + 6) + 1);
    }
    char key[24], label[48];
    std::snprintf(key, sizeof(key), "custom%d", customN);
    std::snprintf(label, sizeof(label), "Tùy biến %d", customN);
    if (si >= 0 && userIdx == activeUserIdx()) {
        const PresetSystem &src = u.systems[(size_t)si];
        addSystem(u, key, label, src.km.b,
                  src.km.dpadToHat, src.km.dpadAsLeftStick);
    } else {
        addSystem(u, key, label, nullptr, true, false);
    }
    return true;
}

int BrickPadStore::bindLookup(const std::string &mac) const {
    std::string want; normalizeMac(mac, want);
    if (want.size() != 12) return -1;
    for (auto &d : devices) {
        std::string have; normalizeMac(d.address, have);
        if (have == want && !d.system.empty()) {
            int ui = activeUserIdx();
            if (ui >= 0) {
                int si = sysIndex(ui, d.system);
                if (si >= 0) return si;
            }
            return -2; // có bind nhưng system không thuộc user hiện tại
        }
    }
    return -1;
}
bool BrickPadStore::bindSet(const std::string &mac, const std::string &syskey) {
    std::string want; normalizeMac(mac, want);
    if (want.size() != 12) return false;
    for (auto &d : devices) {
        std::string have; normalizeMac(d.address, have);
        if (have == want) { d.system = syskey; return true; }
    }
    if ((int)devices.size() >= kMaxDevs) return false;
    BtDevice d;
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
                  want[0], want[1], want[2], want[3], want[4], want[5],
                  want[6], want[7], want[8], want[9], want[10], want[11]);
    d.address = buf; d.system = syskey;
    devices.push_back(d);
    return true;
}
bool BrickPadStore::bindClear(const std::string &mac) {
    std::string want; normalizeMac(mac, want);
    for (auto &d : devices) {
        std::string have; normalizeMac(d.address, have);
        if (have == want) { d.system.clear(); return true; }
    }
    return false;
}
void BrickPadStore::devTouch(const std::string &mac, const std::string &alias) {
    std::string want; normalizeMac(mac, want);
    if (want.size() != 12) return;
    BtDevice *d = nullptr;
    for (auto &dev : devices) {
        std::string have; normalizeMac(dev.address, have);
        if (have == want) { d = &dev; break; }
    }
    if (!d) {
        if ((int)devices.size() >= kMaxDevs) return;
        devices.emplace_back();
        d = &devices.back();
        char buf[20];
        std::snprintf(buf, sizeof(buf), "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
                      want[0], want[1], want[2], want[3], want[4], want[5],
                      want[6], want[7], want[8], want[9], want[10], want[11]);
        d->address = buf;
        d->identity = lastPlatform;
        d->mode = lastMode;
    }
    if (!alias.empty() && !isMacAddress(alias)) d->alias = alias;
    else if (d->alias.empty()) d->alias = d->address;
    std::time_t t = std::time(nullptr);
    struct tm *tm = std::localtime(&t);
    if (tm) {
        char ts[32];
        std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", tm);
        d->lastConnected = ts;
    }
}
bool BrickPadStore::renameDevice(const std::string &mac, const std::string &alias) {
    std::string want; normalizeMac(mac, want);
    for (auto &d : devices) {
        std::string have; normalizeMac(d.address, have);
        if (have == want) { d.alias = alias; return true; }
    }
    return false;
}

bool BrickPadStore::removeDevice(const std::string &mac) {
    std::string want; normalizeMac(mac, want);
    for (auto it = devices.begin(); it != devices.end(); ++it) {
        std::string have; normalizeMac(it->address, have);
        if (have == want) {
            devices.erase(it);
            return true;
        }
    }
    return false;
}

ControllerIdentity BrickPadStore::deviceIdentity(const std::string &mac) const {
    std::string want; normalizeMac(mac, want);
    for (const auto &d : devices) {
        std::string have; normalizeMac(d.address, have);
        if (have == want) return d.identity;
    }
    return lastPlatform;
}

bool BrickPadStore::setDeviceIdentity(const std::string &mac, ControllerIdentity id) {
    std::string want; normalizeMac(mac, want);
    if (want.size() != 12) return false;
    for (auto &d : devices) {
        std::string have; normalizeMac(d.address, have);
        if (have == want) {
            d.identity = id;
            return true;
        }
    }
    devTouch(mac, "");
    for (auto &d : devices) {
        std::string have; normalizeMac(d.address, have);
        if (have == want) {
            d.identity = id;
            return true;
        }
    }
    return false;
}

int BrickPadStore::deviceMode(const std::string &mac) const {
    std::string want; normalizeMac(mac, want);
    for (const auto &d : devices) {
        std::string have; normalizeMac(d.address, have);
        if (have == want) return d.mode;
    }
    return lastMode;
}

bool BrickPadStore::setDeviceMode(const std::string &mac, int mode) {
    std::string want; normalizeMac(mac, want);
    if (want.size() != 12) return false;
    for (auto &d : devices) {
        std::string have; normalizeMac(d.address, have);
        if (have == want) {
            d.mode = mode;
            return true;
        }
    }
    devTouch(mac, "");
    for (auto &d : devices) {
        std::string have; normalizeMac(d.address, have);
        if (have == want) {
            d.mode = mode;
            return true;
        }
    }
    return false;
}

const BtDevice *BrickPadStore::lastConnectedDevice() const {
    if (devices.empty()) return nullptr;
    const BtDevice *best = nullptr;
    for (const auto &d : devices) {
        if (!d.address.empty()) {
            if (!best || d.lastConnected > best->lastConnected) {
                best = &d;
            }
        }
    }
    return best;
}

bool BrickPadStore::removeSystem(int userIdx, int sysIdx) {
    if (userIdx < 0 || userIdx >= (int)users.size()) return false;
    BtUser &u = users[(size_t)userIdx];
    if ((int)u.systems.size() <= 1 || sysIdx < 0 || sysIdx >= (int)u.systems.size()) return false;
    std::string removedKey = u.systems[(size_t)sysIdx].system;
    u.systems.erase(u.systems.begin() + sysIdx);
    if (activeSys[userIdx] == removedKey) {
        activeSys[userIdx] = u.systems[0].system;
    }
    return true;
}

bool BrickPadStore::renameSystem(int userIdx, int sysIdx, const std::string &newLabel) {
    if (newLabel.empty() || userIdx < 0 || userIdx >= (int)users.size()) return false;
    BtUser &u = users[(size_t)userIdx];
    if (sysIdx < 0 || sysIdx >= (int)u.systems.size()) return false;
    u.systems[(size_t)sysIdx].label = newLabel;
    return true;
}

bool BrickPadStore::addUser(const std::string &name, ControllerIdentity identity) {
    if ((int)users.size() >= kMaxUsers || name.empty()) return false;
    for (const auto &u : users) {
        if (u.name == name) return false;
    }
    BtUser u;
    u.name = name;
    u.defaultIdentity = identity;
    u.autoConnect = true;
    const int8_t id[P_COUNT]  = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14};
    const int8_t xb[P_COUNT]  = {1,0,3,2,4,5,6,7,8,9,10,11,12,13,14};
    addSystem(u, "xbox", "XBOX (RetroArch)", xb, true, true);
    addSystem(u, "snes", "Mặc định (SNES)", id, true, true);
    int newIdx = (int)users.size();
    users.push_back(u);
    activeSys[newIdx] = "xbox";
    return true;
}

bool BrickPadStore::removeUser(int userIdx) {
    if ((int)users.size() <= 1 || userIdx < 0 || userIdx >= (int)users.size()) return false;
    std::string removedName = users[(size_t)userIdx].name;
    users.erase(users.begin() + userIdx);
    for (int i = userIdx; i < kMaxUsers - 1; i++) {
        activeSys[i] = activeSys[i + 1];
    }
    activeSys[users.size()] = "xbox";
    if (activeUser == removedName) {
        activeUser = users[0].name;
    }
    return true;
}

bool BrickPadStore::renameUser(int userIdx, const std::string &newName) {
    if (newName.empty() || userIdx < 0 || userIdx >= (int)users.size()) return false;
    for (size_t i = 0; i < users.size(); i++) {
        if ((int)i != userIdx && users[i].name == newName) return false;
    }
    if (activeUser == users[(size_t)userIdx].name) {
        activeUser = newName;
    }
    users[(size_t)userIdx].name = newName;
    return true;
}

void BrickPadStore::resetUserDefaults(int userIdx) {
    if (userIdx < 0 || userIdx >= (int)users.size()) return;
    BtUser &u = users[(size_t)userIdx];
    u.systems.clear();
    const int8_t id[P_COUNT]  = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14};
    const int8_t nin[P_COUNT] = {1,0,2,3,4,5,6,7,8,9,10,11,12,13,14};
    const int8_t xb[P_COUNT]  = {1,0,3,2,4,5,6,7,8,9,10,11,12,13,14};
    addSystem(u, "xbox", "XBOX (RetroArch)", xb, true, true);
    addSystem(u, "snes", "Mặc định (SNES)", id, true, true);
    addSystem(u, "nintendo", "Nintendo", nin, true, true);
    addSystem(u, "n64", "N64", id, true, true);
    addSystem(u, "psx", "PLAYSTATION", xb, true, true);
    activeSys[userIdx] = "xbox";
}

void applyKeymap(const BrickInput &in, const Keymap &km, uint8_t r[8]) {
    r[0] = 0x01;
    r[1] = km.dpadToHat ? hatOf(in.dpadUp, in.dpadDown, in.dpadLeft, in.dpadRight) : 8;
    uint16_t btn = 0;
    bool p[P_COUNT];
    physFromInput(in, p);
    for (int i = 0; i < P_COUNT; i++) {
        if (!p[i]) continue;
        int bit = km.b[i] < 0 ? i : km.b[i];
        if (bit >= 0 && bit <= 15) btn |= (uint16_t)(1u << bit);
    }
    r[2] = (uint8_t)(btn & 0xFF);
    r[3] = (uint8_t)(btn >> 8);
    // Left Stick: ưu tiên giá trị analog thật từ cần gạt vật lý;
    // nếu cần ở giữa mà D-pad được bấm (và dpadAsLeftStick bật), hỗ trợ D-pad đẩy stick
    if (in.stickLX < 110 || in.stickLX > 146 || in.stickLY < 110 || in.stickLY > 146) {
        r[4] = in.stickLX;
        r[5] = in.stickLY;
    } else if (km.dpadAsLeftStick && (in.dpadLeft || in.dpadRight || in.dpadUp || in.dpadDown)) {
        r[4] = in.dpadLeft ? 0 : (in.dpadRight ? 255 : 128);
        r[5] = in.dpadUp ? 0 : (in.dpadDown ? 255 : 128);
    } else {
        r[4] = in.stickLX;
        r[5] = in.stickLY;
    }
    r[6] = in.stickRX;
    r[7] = in.stickRY;
}

BrickInput remapInputForKeymap(const Keymap &km, const BrickInput &in) {
    // Nguồn vật lý cho mỗi generic bit: phys j có km.b[j]==t; -1 = giữ nguyên.
    bool p[P_COUNT];
    physFromInput(in, p);
    bool q[P_COUNT] = {false};
    for (int t = 0; t < P_COUNT; t++) {
        int src = -1;
        for (int j = 0; j < P_COUNT; j++) {
            if (km.b[j] == t) { src = j; break; }
        }
        if (src < 0 && km.b[t] < 0) src = t;
        q[t] = (src >= 0) ? p[src] : false;
    }
    BrickInput o = in; // dpad + stick đi thẳng (keymap không có entry)
    o.btnA = q[0]; o.btnB = q[1]; o.btnX = q[2]; o.btnY = q[3];
    o.btnL1 = q[4]; o.btnR1 = q[5]; o.btnL2 = q[6]; o.btnR2 = q[7];
    o.btnSelect = q[8]; o.btnStart = q[9];
    o.btnL3 = q[10]; o.btnR3 = q[11];
    o.btnHome = q[12]; o.btnFN1 = q[13]; o.btnFN2 = q[14];
    return o;
}

} // namespace BrickPad
} // namespace RomCloud
