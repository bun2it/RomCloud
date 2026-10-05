#include "MarketManager.h"
#include "../network/HttpClient.h"
#include "../logging/Logger.h"

#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace RomCloud {

MarketManager& MarketManager::instance() {
    static MarketManager inst;
    return inst;
}

MarketData MarketManager::data() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_data;
}

int MarketManager::fuelZone(const std::string& province) {
    // Vùng giá xăng/dầu theo tỉnh (1/2). Xấp xỉ cấp tỉnh (danh sách Vùng 2
    // chính thức của Petrolimex quy về 34 tỉnh), chưa tới đảo/xã lẻ.
    static const char* z2[] = {
        "Cao Bằng", "Tuyên Quang", "Điện Biên", "Lai Châu", "Sơn La",
        "Lào Cai", "Thái Nguyên", "Lạng Sơn", "Bắc Ninh", "Phú Thọ",
        "Hưng Yên", "Ninh Bình", "Thanh Hóa", "Nghệ An", "Hà Tĩnh",
        "Quảng Trị", "Huế", "Quảng Ngãi", "Gia Lai", "Khánh Hòa",
        "Đắk Lắk", "Lâm Đồng", "Đồng Tháp", "An Giang", "Cần Thơ", "Cà Mau",
    };
    for (auto z : z2)
        if (province == z) return 2;
    return 1;
}

std::string MarketManager::areaFor(const std::string& province) {
    std::string prov = province.empty() ? "Hà Nội" : province;
    return prov + " • Vùng " + std::to_string(fuelZone(prov));
}

bool MarketManager::isSouth(const std::string& province) {
    static const char* south[] = {"Đồng Nai", "Hồ Chí Minh", "Tây Ninh",
                                  "Đồng Tháp", "Vĩnh Long", "An Giang",
                                  "Cần Thơ", "Cà Mau"};
    for (auto s : south)
        if (province == s) return true;
    return false;
}

// "27.080" / "143.500.000" từ số nguyên.
static std::string fmtMoney(long long v) {
    if (v < 0) v = 0;
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", v);
    std::string s = buf, out;
    int n = (int)s.size(), c = 0;
    for (int i = n - 1; i >= 0; --i) {
        out.insert(out.begin(), s[i]);
        if (++c % 3 == 0 && i > 0) out.insert(out.begin(), '.');
    }
    return out;
}

// Số VN "27.180" -> 27180 (bỏ dấu chấm nghìn).
static long parseVnNum(const std::string& s) {
    std::string t;
    for (char c : s)
        if ((c >= '0' && c <= '9')) t += c;
    return t.empty() ? 0 : atol(t.c_str());
}

// Hai ô <td>số sau marker (bảng chogia: tên | V1 | V2).
// Ô có thể mang class (<td class="price ">) nên match prefix "<td".
static bool twoCells(const std::string& body, size_t from, long& a, long& b) {
    size_t p = body.find("<td", from);
    if (p == std::string::npos) return false;
    size_t q = body.find("</td>", p);
    if (q == std::string::npos) return false;
    size_t p2 = body.find("<td", q);
    if (p2 == std::string::npos) return false;
    // Ô giá có thể mang class: <td class="price ">...
    size_t q2 = body.find("</td>", p2);
    if (q2 == std::string::npos) return false;
    auto num = [](const std::string& cell) -> long {
        size_t gt = cell.find('>');
        std::string s = (gt == std::string::npos) ? cell : cell.substr(gt + 1);
        return parseVnNum(s);
    };
    a = num(body.substr(p, q - p));
    b = num(body.substr(p2, q2 - p2));
    return a > 0 && b > 0;
}

// "HH:MM dd/mm" lúc fetch (giờ Brick).
static std::string nowStamp() {
    std::time_t t = std::time(nullptr);
    struct tm* l = std::localtime(&t);
    if (!l) return "";
    char b[16];
    snprintf(b, sizeof(b), "%02d:%02d %02d/%02d", l->tm_hour, l->tm_min,
             l->tm_mday, l->tm_mon + 1);
    return b;
}

bool MarketManager::fetch(const std::string& province) {
    std::string prov = province.empty() ? "Hà Nội" : province;
    int zone = fuelZone(prov);
    bool south = isSouth(prov);
    MarketData d;
    d.area = areaFor(prov);

    // Nguồn duy nhất: chogia.vn (số khớp bảng Petrolimex, đủ 2 vùng,
    // vàng theo miền, USD mua/bán — không key).
    HttpResponse home = HttpClient::instance().get("https://chogia.vn/", {}, 25);
    if (home.success && home.statusCode == 200 && !home.body.empty()) {
        const std::string& b = home.body;
        // 1) Xăng RON95-III: <strong>Xăng E10 RON95-III</strong></td><td>V1</td><td>V2
        size_t px = b.find("RON95-III");
        if (px != std::string::npos) {
            long v1 = 0, v2 = 0;
            if (twoCells(b, px, v1, v2)) {
                d.xang.ok = true;
                d.xang.value = fmtMoney(zone == 2 ? v2 : v1);
                d.xang.unit = "đ/lít";
                d.xang.sub = "RON 95 • Vùng " + std::to_string(zone);
                d.xang.updated = nowStamp();
            }
        }
        // 2) Dầu DO 0.05S.
        size_t pd = b.find("Dầu DO 0.05S");
        if (pd == std::string::npos) pd = b.find("DO 0.05S");
        if (pd != std::string::npos) {
            long v1 = 0, v2 = 0;
            if (twoCells(b, pd, v1, v2)) {
                d.dau.ok = true;
                d.dau.value = fmtMoney(zone == 2 ? v2 : v1);
                d.dau.unit = "đ/lít";
                d.dau.sub = "DO 0,05S • Vùng " + std::to_string(zone);
                d.dau.updated = nowStamp();
            }
        }
        // 4) USD: <strong>USD</strong> + 2 ô mua/bán.
        size_t pu = b.find("<strong>USD</strong>");
        if (pu != std::string::npos) {
            long mua = 0, ban = 0;
            if (twoCells(b, pu, mua, ban)) {
                d.usd.ok = true;
                d.usd.value = fmtMoney(ban);
                d.usd.unit = "đ";
                d.usd.sub = "Mua " + fmtMoney(mua) + " • Vietcombank";
                d.usd.updated = nowStamp();
            }
        }
    }
    // 3) Vàng SJC theo miền (nghìn đồng -> đồng).
    {
        HttpResponse r = HttpClient::instance().get(
            "https://chogia.vn/gia-vang/gia-vang-sjc/", {}, 25);
        if (r.success && r.statusCode == 200 && !r.body.empty()) {
            const char* region = south ? "MIỀN TÂY" : "MIỀN BẮC";
            size_t pr = r.body.find(region);
            size_t ps = std::string::npos;
            if (pr != std::string::npos)
                ps = r.body.find("Vàng SJC 1L", pr);
            if (ps == std::string::npos)
                ps = r.body.find("Vàng SJC 1L");
            if (ps != std::string::npos) {
                long mua = 0, ban = 0;
                if (twoCells(r.body, ps, mua, ban)) {
                    d.vang.ok = true;
                    d.vang.value = fmtMoney(ban * 1000);
                    d.vang.unit = "đ/lượng";
                    char sb[96];
                    snprintf(sb, sizeof(sb), "SJC • Mua %s",
                             fmtMoney(mua * 1000).c_str());
                    d.vang.sub = sb;
                    d.vang.updated = nowStamp();
                }
            }
        }
    }

    d.fetchedAt = (int64_t)std::time(nullptr);
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        // Giữ giá trị cũ khi fetch rớt từng món (offline vẫn có số).
        if (d.xang.ok) m_data.xang = d.xang;
        if (d.dau.ok) m_data.dau = d.dau;
        if (d.vang.ok) m_data.vang = d.vang;
        if (d.usd.ok) m_data.usd = d.usd;
        m_data.area = d.area;
        if (d.xang.ok || d.dau.ok || d.vang.ok || d.usd.ok)
            m_data.fetchedAt = d.fetchedAt;
    }
    bool any = d.xang.ok || d.dau.ok || d.vang.ok || d.usd.ok;
    Logger::info(std::string("MarketManager: fetch ") + (any ? "ok" : "fail"));
    return any;
}

} // namespace RomCloud
