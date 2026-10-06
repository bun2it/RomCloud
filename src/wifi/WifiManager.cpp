#include "WifiManager.h"
#include "../config/AppConfig.h"
#include "../logging/Logger.h"
#include "../network/HttpClient.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <regex>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

namespace RomCloud {

WifiManager& WifiManager::instance() {
    static WifiManager inst;
    return inst;
}

std::string WifiManager::cli(const std::string& args) {
    std::string cmd = "wpa_cli -p /etc/wifi/sockets -i wlan0 " + args + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    std::string out;
    char buf[1024];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    return out;
}

bool WifiManager::available() {
    if (access("/etc/wifi/sockets", F_OK) != 0) return false;
    std::string s = cli("ping");
    return s.find("PONG") != std::string::npos;
}

// "key=value\n" trong status (key phải ở đầu dòng để không dính "ssid"
// trong "bssid").
static std::string statusVal(const std::string& st, const char* key) {
    std::string pat = std::string(key) + "=";
    size_t from = 0;
    while (true) {
        size_t p = st.find(pat, from);
        if (p == std::string::npos) return "";
        if (p == 0 || st[p - 1] == '\n') {
            size_t v = p + pat.size();
            size_t e = st.find('\n', v);
            std::string r = st.substr(v, e == std::string::npos ? e : e - v);
            while (!r.empty() && (r.back() == '\r' || r.back() == ' ')) r.pop_back();
            return r;
        }
        from = p + 1;
    }
}

std::string WifiManager::curSsid() {
    return statusVal(cli("status"), "ssid");
}

std::string WifiManager::curState() {
    std::string s = statusVal(cli("status"), "wpa_state");
    return s.empty() ? "UNKNOWN" : s;
}

std::string WifiManager::curIp() {
    return statusVal(cli("status"), "ip_address");
}

void WifiManager::backupConf() {
    std::string dst = AppConfig::instance().getDataDir() + "/wpa_supplicant.conf.bak";
    struct stat st;
    if (stat(dst.c_str(), &st) == 0) return; // chỉ backup 1 lần
    std::string cmd = "cp -f /etc/wifi/wpa_supplicant.conf \"" + dst + "\" 2>/dev/null";
    if (system(cmd.c_str()) == 0)
        Logger::info("WifiManager: backup conf -> " + dst);
}

// wpa_cli escape \xNN (SSID tiếng Việt) về byte UTF-8.
static std::string unescapeSsid(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] == 'x') {
            char h0 = s[i + 2], h1 = s[i + 3];
            auto hv = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int a = hv(h0), b = hv(h1);
            if (a >= 0 && b >= 0) {
                o += (char)(a * 16 + b);
                i += 4;
                continue;
            }
        }
        if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == '"' || s[i + 1] == '\\')) {
            o += s[i + 1];
            i += 2;
            continue;
        }
        o += s[i++];
    }
    return o;
}

static std::vector<std::string> splitTab(const std::string& line) {
    std::vector<std::string> o;
    size_t s = 0;
    while (true) {
        size_t t = line.find('\t', s);
        o.push_back(line.substr(s, t == std::string::npos ? t : t - s));
        if (t == std::string::npos) break;
        s = t + 1;
    }
    return o;
}

bool WifiManager::refreshBlocking() {
    if (!available()) return false;
    cli("scan");
    // Chờ scan xong (poll tới 8s).
    for (int i = 0; i < 8; ++i) {
        sleep(1);
        std::string r = cli("scan_results");
        if (r.find('\n') != r.rfind('\n')) break; // >1 dòng = có data
    }
    std::string res = cli("scan_results");
    std::string saved = cli("list_networks");
    std::vector<WifiNet> v;
    bool first = true;
    size_t pos = 0;
    while (pos < res.size()) {
        size_t e = res.find('\n', pos);
        std::string line = res.substr(pos, e == std::string::npos ? e : e - pos);
        pos = e == std::string::npos ? res.size() : e + 1;
        if (first) { first = false; continue; } // header
        if (line.size() < 10) continue;
        auto cols = splitTab(line);
        if (cols.size() < 5) continue;
        WifiNet n;
        n.bssid = cols[0];
        n.freq = std::atoi(cols[1].c_str());
        n.signalDbm = std::atoi(cols[2].c_str());
        n.flags = cols[3];
        n.ssid = unescapeSsid(cols[4]);
        if (n.ssid.empty()) continue; // SSID ẩn không hiện ở đây
        n.secured = (n.flags.find("WPA") != std::string::npos ||
                     n.flags.find("WEP") != std::string::npos);
        if (saved.find(n.ssid) != std::string::npos) n.saved = true;
        // Gộp trùng SSID: giữ sóng mạnh nhất.
        bool dup = false;
        for (auto& x : v) {
            if (x.ssid == n.ssid) {
                dup = true;
                if (n.signalDbm > x.signalDbm) x = n;
                break;
            }
        }
        if (!dup) v.push_back(n);
    }
    m_nets = std::move(v);
    Logger::info("WifiManager: scan found " + std::to_string(m_nets.size()) + " SSIDs");
    return true;
}

static std::string qesc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\' || c == '"') o += '\\';
        o += c;
    }
    return o;
}

bool WifiManager::connectBlocking(const std::string& ssid, const std::string& psk,
                                  bool hidden, std::string& outMsg) {
    if (ssid.empty()) { outMsg = "Chưa nhập tên mạng"; return false; }
    if (!available()) { outMsg = "Không thấy wpa_supplicant"; return false; }
    backupConf();
    // Xóa network trùng SSID cũ (tránh 2 entry đánh nhau).
    {
        std::string list = cli("list_networks");
        size_t pos = 0;
        bool first = true;
        while (pos < list.size()) {
            size_t e = list.find('\n', pos);
            std::string line = list.substr(pos, e == std::string::npos ? e : e - pos);
            pos = e == std::string::npos ? list.size() : e + 1;
            if (first) { first = false; continue; }
            auto cols = splitTab(line);
            if (cols.size() >= 2 && cols[1] == ssid) {
                cli("remove_network " + cols[0]);
            }
        }
    }
    std::string id = cli("add_network");
    // add_network trả "12\n" (id) hoặc "FAIL".
    size_t e = id.find('\n');
    id = id.substr(0, e);
    while (!id.empty() && (id.back() < '0' || id.back() > '9')) id.pop_back();
    if (id.empty()) { outMsg = "Không tạo được profile mạng"; return false; }
    cli("set_network " + id + " ssid '\"" + qesc(ssid) + "\"'");
    if (psk.empty()) {
        cli("set_network " + id + " key_mgmt NONE");
    } else {
        cli("set_network " + id + " psk '\"" + qesc(psk) + "\"'");
    }
    if (hidden) cli("set_network " + id + " scan_ssid 1");
    cli("enable_network " + id);
    cli("save_config");
    cli("reconnect");
    // Chờ COMPLETED tới ~20s.
    for (int i = 0; i < 20; ++i) {
        sleep(1);
        std::string st = cli("status");
        std::string ws = statusVal(st, "wpa_state");
        if (ws == "COMPLETED") {
            std::string ip = statusVal(st, "ip_address");
            outMsg = "Đã nối " + ssid + (ip.empty() ? "" : " (" + ip + ")");
            Logger::info("WifiManager: " + outMsg);
            return true;
        }
    }
    outMsg = "Chưa nối được " + ssid + " (sai pass hoặc sóng yếu)";
    Logger::warn("WifiManager: " + outMsg);
    return false;
}

bool WifiManager::savedHasPsk(const std::string& ssid) {
    if (!available()) return false;
    std::string list = cli("list_networks");
    size_t pos = 0;
    bool first = true;
    while (pos < list.size()) {
        size_t e = list.find('\n', pos);
        std::string line = list.substr(pos, e == std::string::npos ? e : e - pos);
        pos = e == std::string::npos ? list.size() : e + 1;
        if (first) { first = false; continue; }
        auto cols = splitTab(line);
        if (cols.size() >= 2 && cols[1] == ssid) {
            std::string psk = cli("get_network " + cols[0] + " psk");
            while (!psk.empty() && (psk.back() == '\n' || psk.back() == '\r' || psk.back() == ' '))
                psk.pop_back();
            return !psk.empty() && psk != "FAIL" && psk != "null";
        }
    }
    return false;
}

bool WifiManager::enableSavedBlocking(const std::string& ssid, std::string& outMsg) {
    if (!available()) { outMsg = "Không thấy wpa_supplicant"; return false; }
    std::string list = cli("list_networks");
    size_t pos = 0;
    bool first = true, found = false;
    while (pos < list.size()) {
        size_t e = list.find('\n', pos);
        std::string line = list.substr(pos, e == std::string::npos ? e : e - pos);
        pos = e == std::string::npos ? list.size() : e + 1;
        if (first) { first = false; continue; }
        auto cols = splitTab(line);
        if (cols.size() >= 2 && cols[1] == ssid) {
            cli("enable_network " + cols[0]);
            found = true;
        }
    }
    if (!found) { outMsg = "Không thấy " + ssid + " trong đã lưu"; return false; }
    cli("reconnect");
    for (int i = 0; i < 20; ++i) {
        sleep(1);
        std::string st = cli("status");
        if (statusVal(st, "wpa_state") == "COMPLETED") {
            std::string ip = statusVal(st, "ip_address");
            outMsg = "Đã nối " + ssid + (ip.empty() ? "" : " (" + ip + ")");
            return true;
        }
    }
    outMsg = "Chưa nối được " + ssid;
    return false;
}

bool WifiManager::forgetBlocking(const std::string& ssid, std::string& outMsg) {
    if (!available()) { outMsg = "Không thấy wpa_supplicant"; return false; }
    backupConf();
    std::string list = cli("list_networks");
    size_t pos = 0;
    bool first = true, found = false;
    while (pos < list.size()) {
        size_t e = list.find('\n', pos);
        std::string line = list.substr(pos, e == std::string::npos ? e : e - pos);
        pos = e == std::string::npos ? list.size() : e + 1;
        if (first) { first = false; continue; }
        auto cols = splitTab(line);
        if (cols.size() >= 2 && cols[1] == ssid) {
            cli("remove_network " + cols[0]);
            found = true;
        }
    }
    if (found) {
        cli("save_config");
        cli("reconnect");
        outMsg = "Đã xóa " + ssid;
        return true;
    }
    outMsg = "Không thấy " + ssid + " trong đã lưu";
    return false;
}

bool WifiManager::reconnectBlocking() {
    if (!available()) return false;
    cli("reconnect");
    return true;
}

// ---- Captive portal ----

static std::string tagTitle(const std::string& html) {
    size_t p = html.find("<title>");
    if (p == std::string::npos) return "";
    size_t e = html.find("</title>", p);
    if (e == std::string::npos) return "";
    std::string t = html.substr(p + 7, e - p - 7);
    if (t.size() > 80) t = t.substr(0, 80);
    return t;
}

PortalInfo WifiManager::checkPortal() {
    PortalInfo pi;
    const char* probes[] = {
        "http://connectivitycheck.gstatic.com/generate_204",
        "http://www.google.com",
    };
    for (const char* u : probes) {
        HttpResponse r = HttpClient::instance().get(u, {}, 10);
        if (!r.success) continue; // chưa có mạng / timeout -> thử probe sau
        if (r.statusCode == 204 && r.body.size() < 64) {
            pi.online = true;
            return pi;
        }
        // Mọi thứ khác (200 có body, redirect tới trang login) = portal.
        pi.portal = true;
        pi.url = r.effectiveUrl.empty() ? u : r.effectiveUrl;
        pi.title = tagTitle(r.body);
        Logger::info("WifiManager: captive portal -> " + pi.url);
        return pi;
    }
    return pi; // offline hẳn (cả 2 probe đều fail)
}


// ---- Portal multi-step: form model ----

struct PortalField {
    std::string name;
    std::string value;
    std::string type; // lower: text/tel/number/hidden/submit/checkbox/...
    std::string label; // placeholder || text nút || name
    bool isSubmit = false;
};

struct PortalForm {
    std::string action;
    std::string method = "post";
    std::vector<PortalField> fields;
    bool hasPassword = false;
    bool hasAccept = false; // có nút/submit mang nghĩa chấp nhận
};

static std::string attrVal(const std::string& tag, const char* attr) {
    try {
        std::regex r(std::string(attr) + "\\s*=\\s*\"([^\"]*)\"", std::regex::icase);
        std::smatch m;
        if (std::regex_search(tag, m, r)) return m[1];
    } catch (...) {}
    return "";
}

static bool isAcceptText(const std::string& s) {
    std::string low = s;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    return low.find("accept") != std::string::npos ||
           low.find("agree") != std::string::npos ||
           low.find("connect") != std::string::npos ||
           low.find("continue") != std::string::npos ||
           low.find("submit") != std::string::npos ||
           s.find("Đồng ý") != std::string::npos ||
           s.find("đồng ý") != std::string::npos ||
           s.find("Tiếp tục") != std::string::npos ||
           s.find("tiếp tục") != std::string::npos ||
           s.find("Xác nhận") != std::string::npos ||
           s.find("xác nhận") != std::string::npos ||
           s.find("Kết nối") != std::string::npos ||
           s.find("kết nối") != std::string::npos ||
           s.find("Truy cập") != std::string::npos;
}

static std::vector<PortalForm> parsePortalForms(const std::string& html) {
    std::vector<PortalForm> forms;
    try {
        std::regex formRe("<form[^>]*>", std::regex::icase);
        std::regex inpRe("<input[^>]*>", std::regex::icase);
        std::regex btnRe("<button[^>]*>([^<]*)</button>", std::regex::icase);
        std::sregex_iterator it(html.begin(), html.end(), formRe), end;
        for (; it != end; ++it) {
            std::string tag = it->str();
            size_t fpos = it->position();
            size_t fend = html.find("</form>", fpos);
            if (fend == std::string::npos) continue;
            std::string inner = html.substr(fpos, fend - fpos);
            PortalForm f;
            f.action = attrVal(tag, "action");
            std::string mt = attrVal(tag, "method");
            if (!mt.empty()) {
                for (auto& c : mt) c = (char)tolower((unsigned char)c);
                f.method = mt;
            }
            std::sregex_iterator ii(inner.begin(), inner.end(), inpRe), ie;
            for (; ii != ie; ++ii) {
                std::string t = ii->str();
                std::string ty = attrVal(t, "type");
                for (auto& c : ty) c = (char)tolower((unsigned char)c);
                if (ty.empty()) ty = "text";
                if (ty == "password") f.hasPassword = true;
                PortalField fd;
                fd.name = attrVal(t, "name");
                fd.value = attrVal(t, "value");
                fd.type = ty;
                fd.label = attrVal(t, "placeholder");
                if (fd.label.empty()) fd.label = fd.name;
                if (ty == "submit" || ty == "button" || ty == "image") {
                    fd.isSubmit = true;
                    if (isAcceptText(fd.value)) f.hasAccept = true;
                }
                if (ty == "checkbox") {
                    if (fd.value.empty()) fd.value = "on";
                }
                if (!fd.name.empty() || fd.isSubmit) f.fields.push_back(fd);
            }
            // <button name=..>Text</button>
            std::sregex_iterator bi(inner.begin(), inner.end(), btnRe), be;
            for (; bi != be; ++bi) {
                std::string bt = (*bi)[0];
                std::string nm = attrVal(bt, "name");
                std::string tx = (*bi)[1];
                if (nm.empty()) continue;
                PortalField fd;
                fd.name = nm;
                fd.value = attrVal(bt, "value");
                if (fd.value.empty()) fd.value = tx;
                fd.type = "submit";
                fd.label = tx.empty() ? nm : tx;
                fd.isSubmit = true;
                if (isAcceptText(tx) || isAcceptText(fd.value)) f.hasAccept = true;
                f.fields.push_back(fd);
            }
            forms.push_back(std::move(f));
            if (forms.size() > 8) break;
        }
    } catch (...) {}
    return forms;
}

static std::string urlDirBase(const std::string& u) {
    std::string b = u;
    size_t q = b.find('?');
    if (q != std::string::npos) b = b.substr(0, q);
    size_t s = b.rfind('/');
    size_t scheme = b.find("://");
    if (s != std::string::npos && scheme != std::string::npos && s > scheme + 2)
        b = b.substr(0, s + 1);
    return b;
}

bool WifiManager::acceptPortalBlocking(std::string& outMsg) {
    std::map<std::string, std::string> answers; // ô đã hỏi -> không hỏi lại
    bool waitedAd = false;
    for (int step = 0; step < 6; ++step) {
        PortalInfo pi = checkPortal();
        if (pi.online) {
            outMsg = "Đã xác nhận portal, mạng thông rồi";
            Logger::info("WifiManager: portal accepted");
            return true;
        }
        if (!pi.portal) {
            outMsg = "Chưa có mạng, kiểm tra Wi-Fi trước";
            return false;
        }
        HttpResponse r = HttpClient::instance().get(pi.url, {}, 15);
        if (!r.success || r.body.empty()) {
            outMsg = "Không tải được trang xác nhận";
            return false;
        }
        std::string pageUrl = r.effectiveUrl.empty() ? pi.url : r.effectiveUrl;
        // Meta refresh (trang đếm ngược quảng cáo tự chuyển).
        {
            std::string low = r.body;
            for (auto& c : low) c = (char)tolower((unsigned char)c);
            size_t mp = low.find("http-equiv=\"refresh\"");
            if (mp != std::string::npos) {
                size_t cp = low.find("content=\"", mp);
                if (cp != std::string::npos) {
                    size_t ce = low.find('"', cp + 9);
                    std::string cv = low.substr(cp + 9, ce - cp - 9);
                    int secs = std::atoi(cv.c_str());
                    size_t up = cv.find("url=");
                    std::string dest = (up == std::string::npos) ? "" : cv.substr(up + 4);
                    if (secs > 0 && secs <= 30) {
                        Logger::info("WifiManager: portal refresh, wait " + std::to_string(secs) + "s");
                        sleep((unsigned int)secs);
                        if (!dest.empty()) {
                            if (dest.rfind("http", 0) != 0) dest = urlDirBase(pageUrl) + dest;
                            HttpClient::instance().get(dest, {}, 15);
                        }
                        continue; // không tính là 1 step form
                    }
                }
            }
        }
        auto forms = parsePortalForms(r.body);
        // Chọn form: ưu tiên có nút accept, rồi tới form có ô cần điền.
        PortalForm* pick = nullptr;
        for (auto& f : forms) {
            if (f.hasPassword) continue; // form login user/pass: bỏ
            if (f.hasAccept) { pick = &f; break; }
        }
        if (!pick) {
            for (auto& f : forms) {
                if (f.hasPassword) continue;
                for (auto& fd : f.fields) {
                    if (!fd.isSubmit && (fd.type == "text" || fd.type == "tel" ||
                                         fd.type == "number" || fd.type == "date")) {
                        pick = &f;
                        break;
                    }
                }
                if (pick) break;
            }
        }
        if (!pick) {
            // Không còn form: trang chờ/quảng cáo thuần -> đợi 1 lần 15s.
            if (!waitedAd) {
                waitedAd = true;
                Logger::info("WifiManager: portal no form, wait ad 15s");
                sleep(15);
                continue;
            }
            outMsg = "Portal phức tạp quá, cần xác nhận tay trên thiết bị khác";
            return false;
        }
        // Điền ô trống: hỏi user 1 lần/ô (vd năm sinh).
        std::unordered_map<std::string, std::string> kv;
        std::string acceptName, acceptVal;
        bool aborted = false;
        for (auto& fd : pick->fields) {
            if (fd.isSubmit) {
                if (acceptName.empty() && (isAcceptText(fd.value) || isAcceptText(fd.label))) {
                    acceptName = fd.name;
                    acceptVal = fd.value;
                }
                continue;
            }
            if (fd.name.empty()) continue;
            std::string v = fd.value;
            if (v.empty() && (fd.type == "text" || fd.type == "tel" ||
                              fd.type == "number" || fd.type == "date")) {
                auto it = answers.find(fd.name);
                if (it == answers.end()) {
                    std::string label = fd.label.empty() ? fd.name : fd.label;
                    v = m_prompt ? m_prompt(label) : "";
                    answers[fd.name] = v;
                } else {
                    v = it->second;
                }
                if (v.empty()) { aborted = true; break; } // user hủy
            }
            kv[fd.name] = v;
        }
        if (aborted) {
            outMsg = "Đã hủy xác nhận portal";
            return false;
        }
        if (!acceptName.empty()) kv[acceptName] = acceptVal;
        std::string target = pick->action;
        if (target.empty() || target[0] == '#') target = pageUrl;
        else if (target.rfind("http", 0) != 0) target = urlDirBase(pageUrl) + target;
        if (pick->method == "get") {
            std::string q;
            for (auto& p : kv) {
                if (!q.empty()) q += "&";
                q += p.first + "=" + p.second;
            }
            HttpClient::instance().get(target + (target.find('?') == std::string::npos ? "?" : "&") + q, {}, 15);
        } else {
            HttpClient::instance().postForm(target, kv, {}, 15);
        }
        sleep(2); // chờ server portal xử lý rồi vòng sau kiểm tra 204
    }
    outMsg = "Portal nhiều bước quá, thử lại hoặc xác nhận tay";
    return false;
}

} // namespace RomCloud
