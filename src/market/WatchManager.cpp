#include "WatchManager.h"
#include "../network/HttpClient.h"
#include "../database/DatabaseManager.h"
#include "../logging/Logger.h"

#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cctype>

namespace RomCloud {

WatchManager& WatchManager::instance() {
    static WatchManager inst;
    return inst;
}

static const char* kCryptoMap[][2] = {
    {"BTC", "bitcoin"},   {"ETH", "ethereum"}, {"BNB", "binancecoin"},
    {"SOL", "solana"},    {"XRP", "ripple"},   {"DOGE", "dogecoin"},
    {"ADA", "cardano"},   {"TRX", "tron"},     {"TON", "toncoin"},
    {"LTC", "litecoin"},  {"LINK", "chainlink"}, {"DOT", "polkadot"},
    {"AVAX", "avalanche-2"}, {"ARB", "arbitrum"}, {"OP", "optimism"},
};
static const char* kCryptoLabel[][2] = {
    {"BTC", "Bitcoin"}, {"ETH", "Ethereum"}, {"BNB", "BNB"},
    {"SOL", "Solana"},  {"XRP", "XRP"},      {"DOGE", "Dogecoin"},
    {"ADA", "Cardano"}, {"TRX", "TRON"},     {"TON", "Toncoin"},
    {"LTC", "Litecoin"}, {"LINK", "Chainlink"}, {"DOT", "Polkadot"},
    {"AVAX", "Avalanche"}, {"ARB", "Arbitrum"}, {"OP", "Optimism"},
};
static const int kCryptoCount =
    (int)(sizeof(kCryptoMap) / sizeof(kCryptoMap[0]));

static std::string upperOf(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c >= 'a' && c <= 'z') o += (char)(c - 32);
        else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) o += c;
    }
    return o;
}

bool WatchManager::isCrypto(const std::string& sym) {
    std::string u = upperOf(sym);
    for (int i = 0; i < kCryptoCount; i++)
        if (u == kCryptoMap[i][0]) return true;
    return false;
}

std::string WatchManager::cryptoId(const std::string& sym) {
    std::string u = upperOf(sym);
    for (int i = 0; i < kCryptoCount; i++)
        if (u == kCryptoMap[i][0]) return kCryptoMap[i][1];
    return "";
}

std::string WatchManager::cryptoLabel(const std::string& sym) {
    std::string u = upperOf(sym);
    for (int i = 0; i < (int)(sizeof(kCryptoLabel) / sizeof(kCryptoLabel[0])); i++)
        if (u == kCryptoLabel[i][0]) return kCryptoLabel[i][1];
    return u;
}

std::vector<std::string> WatchManager::symbols() {
    std::string s =
        DatabaseManager::instance().getSetting("watch_symbols", "BTC,ETH,VCB,HPG");
    std::vector<std::string> out;
    std::string cur;
    for (char c : s + ",") {
        if (c == ',') {
            std::string u = upperOf(cur);
            if (!u.empty() && (int)out.size() < kMaxCards) {
                bool dup = false;
                for (auto& e : out)
                    if (e == u) dup = true;
                if (!dup) out.push_back(u);
            }
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (out.empty()) out = {"BTC", "ETH", "VCB", "HPG"};
    return out;
}

void WatchManager::setSymbols(const std::vector<std::string>& v) {
    std::string s;
    int n = 0;
    for (auto& e : v) {
        std::string u = upperOf(e);
        if (u.empty() || n >= kMaxCards) continue;
        if (!s.empty()) s += ",";
        s += u;
        n++;
    }
    DatabaseManager::instance().setSetting("watch_symbols", s);
}

std::vector<WatchItem> WatchManager::items() {
    std::vector<std::string> syms = symbols();
    std::lock_guard<std::mutex> lk(m_mtx);
    std::vector<WatchItem> out;
    for (auto& s : syms) {
        WatchItem it;
        it.symbol = s;
        it.crypto = isCrypto(s);
        it.label = it.crypto ? cryptoLabel(s) : s;
        it.unit = it.crypto ? "USD" : "×1.000 VNĐ";
        bool found = false;
        for (auto& st : m_items) {
            if (st.symbol == s) {
                it = st;
                found = true;
                break;
            }
        }
        if (!found) it.ok = false;
        out.push_back(it);
    }
    return out;
}

// Số VN "85.844" / "2.716,63" từ double.
static std::string fmtUsd(double v) {
    if (v < 0) v = 0;
    char buf[48];
    if (v >= 1000.0) {
        long long iv = (long long)(v + 0.5);
        snprintf(buf, sizeof(buf), "%lld", iv);
        std::string s = buf, out;
        int n = (int)s.size(), c = 0;
        for (int i = n - 1; i >= 0; --i) {
            out.insert(out.begin(), s[i]);
            if (++c % 3 == 0 && i > 0) out.insert(out.begin(), '.');
        }
        return out;
    }
    snprintf(buf, sizeof(buf), "%.2f", v);
    for (char* p = buf; *p; ++p)
        if (*p == '.') *p = ',';
    return buf;
}

// Số nguyên kiểu VN 63500 -> "63.500".
static std::string fmtInt(double v) {
    if (v < 0) v = 0;
    long long iv = (long long)(v + 0.5);
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", iv);
    std::string s = buf, out;
    int n = (int)s.size(), c = 0;
    for (int i = n - 1; i >= 0; --i) {
        out.insert(out.begin(), s[i]);
        if (++c % 3 == 0 && i > 0) out.insert(out.begin(), '.');
    }
    return out;
}

// Giá nghìn đồng VNDirect "63.5" -> "63,50".
static std::string fmtVnd(double v) {
    if (v < 0) v = 0;
    char buf[48];
    snprintf(buf, sizeof(buf), "%.2f", v);
    for (char* p = buf; *p; ++p)
        if (*p == '.') *p = ',';
    return buf;
}

static std::string fmtPct(double chg, bool& up) {
    up = chg >= 0;
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %.1f%%", chg >= 0 ? "▲" : "▼",
             chg >= 0 ? chg : -chg);
    std::string s = buf;
    for (char& c : s)
        if (c == '.') c = ',';
    return s;
}

// Tìm "key": rồi đọc double ngay sau (bỏ khoảng trắng).
static bool numAfter(const std::string& body, const std::string& key,
                     size_t from, double& v) {
    size_t p = body.find(key, from);
    if (p == std::string::npos) return false;
    p += key.size();
    while (p < body.size() && (body[p] == ' ' || body[p] == ':')) p++;
    if (p >= body.size()) return false;
    char* end = nullptr;
    v = strtod(body.c_str() + p, &end);
    return end != body.c_str() + p;
}

// Binance klines: [[t,"o","h","l","c",...],...] — lấy close (phần tử 4).
// Giá là chuỗi trong ngoặc kép nên scanner bỏ qua '"' rồi đọc số.
static bool binanceCloses(const std::string& body, std::vector<float>& out) {
    size_t p = body.find('[');
    if (p == std::string::npos) return false;
    int depth = 1;
    p++;
    int idx = -1; // chỉ số phần tử trong nến hiện tại
    while (p < body.size() && depth > 0 && out.size() < 90) {
        char c = body[p];
        if (c == '[') {
            depth++;
            idx = 0;
            p++;
            continue;
        }
        if (c == ']') {
            depth--;
            p++;
            continue;
        }
        if (c == ',') {
            if (depth >= 2) idx++;
            p++;
            continue;
        }
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            p++;
            continue;
        }
        if ((c < '0' || c > '9') && c != '-' && c != '.') {
            p++;
            continue;
        }
        char* end = nullptr;
        double v = strtod(body.c_str() + p, &end);
        if (end == body.c_str() + p) {
            p++;
            continue;
        }
        if (depth >= 2 && idx == 4) out.push_back((float)v);
        p = (size_t)(end - body.c_str());
    }
    return out.size() >= 2;
}

// Binance 24h ticker (không key): giá + % đổi cho SYMUSDT.
static bool binanceTicker(const std::string& sym, double& px, double& chg,
                          bool& hasChg) {
    auto t = HttpClient::instance().get(
        "https://api.binance.com/api/v3/ticker/24hr?symbol=" + sym + "USDT",
        {}, 15);
    if (!t.success || t.body.empty()) return false;
    if (!numAfter(t.body, "\"lastPrice\"", 0, px)) return false;
    hasChg = numAfter(t.body, "\"priceChangePercent\"", 0, chg);
    return true;
}

// Coinbase spot (không key): {"data":{...,"amount":"85886.12"}}.
static bool coinbaseSpot(const std::string& sym, double& px) {
    auto r = HttpClient::instance().get(
        "https://api.coinbase.com/v2/prices/" + sym + "-USD/spot", {}, 15);
    if (!r.success || r.body.empty()) return false;
    return numAfter(r.body, "\"amount\"", 0, px);
}

// Yahoo Finance chart: khai báo trước (định nghĩa sau numArray).
static bool yahooFetch(const std::string& ysym, double& last, double& prev,
                       std::vector<float>& hist);

// Đọc mảng số phẳng "key":[...] từ vị trí from, tới ']' (tối đa cap).
// Bỏ qua phiên null (CK tạm ngừng) thay vì dừng (VNDirect "c", Yahoo close).
static bool numArray(const std::string& body, const std::string& key,
                     std::vector<double>& out, size_t cap, size_t from = 0) {
    size_t p = body.find(key, from);
    if (p == std::string::npos) return false;
    p = body.find('[', p);
    if (p == std::string::npos) return false;
    p++;
    while (out.size() < cap && p < body.size()) {
        while (p < body.size() &&
               (body[p] == ' ' || body[p] == ',' || body[p] == '\n' ||
                body[p] == '\r' || body[p] == '\t'))
            p++;
        if (p >= body.size() || body[p] == ']') break;
        if (p + 4 <= body.size() && body.compare(p, 4, "null") == 0) {
            p += 4;
            continue;
        }
        if ((body[p] < '0' || body[p] > '9') && body[p] != '-' &&
            body[p] != '.')
            break;
        char* end = nullptr;
        double v = strtod(body.c_str() + p, &end);
        if (end == body.c_str() + p) break;
        out.push_back(v);
        p = (size_t)(end - body.c_str());
    }
    return !out.empty();
}

// Yahoo Finance chart (nhanh, 1 call ra giá + % + chart, không key):
// crypto "BTC-USD", CKVN "VCB.VN". Trả về last/prev/hist (bỏ phiên null).
static bool yahooFetch(const std::string& ysym, double& last, double& prev,
                       std::vector<float>& hist) {
    auto r = HttpClient::instance().get(
        "https://query1.finance.yahoo.com/v8/finance/chart/" + ysym +
            "?interval=1d&range=3mo",
        {}, 20);
    if (!r.success || r.body.empty()) return false;
    size_t q = r.body.find("\"quote\":");
    if (q == std::string::npos) return false;
    std::vector<double> closes;
    if (!numArray(r.body, "\"close\":", closes, 200, q) || closes.size() < 2)
        return false;
    if (closes.size() > 60)
        closes.erase(closes.begin(), closes.end() - 60);
    last = closes.back();
    prev = closes[closes.size() - 2];
    if (last <= 0) return false;
    for (double c : closes) hist.push_back((float)c);
    return true;
}

bool WatchManager::fetchAll() {
    std::vector<std::string> syms = symbols();
    std::vector<std::string> cryptos, stocks;
    for (auto& s : syms) {
        if (isCrypto(s)) cryptos.push_back(s);
        else stocks.push_back(s);
    }
    std::vector<WatchItem> out;
    for (auto& s : syms) {
        WatchItem it;
        it.symbol = s;
        it.crypto = isCrypto(s);
        it.label = it.crypto ? cryptoLabel(s) : s;
        it.unit = it.crypto ? "USD" : "×1.000 VNĐ";
        out.push_back(it);
    }
    auto findItem = [&](const std::string& s) -> WatchItem* {
        for (auto& it : out)
            if (it.symbol == s) return &it;
        return nullptr;
    };

    // Crypto: Yahoo (BTC-USD) trước — 1 call ra giá + % + chart.
    // Rớt thì CoinGecko/Binance/Coinbase như cũ.
    if (!cryptos.empty()) {
        for (auto& s : cryptos) {
            WatchItem* it = findItem(s);
            if (!it) continue;
            double last = 0, prev = 0;
            std::vector<float> hist;
            if (yahooFetch(s + "-USD", last, prev, hist)) {
                it->ok = true;
                it->price = fmtUsd(last);
                double chg = prev != 0 ? (last - prev) / prev * 100.0 : 0;
                it->delta = fmtPct(chg, it->up);
                it->hist = hist;
                continue;
            }
            // Fallback CoinGecko single price.
            double px = 0, chg = 0;
            bool hasDelta = false;
            auto pr = HttpClient::instance().get(
                "https://api.coingecko.com/api/v3/simple/price?ids=" +
                    cryptoId(s) +
                    "&vs_currencies=usd&include_24hr_change=true",
                {}, 20);
            bool got = false;
            if (pr.success && !pr.body.empty()) {
                std::string obj = "\"" + cryptoId(s) + "\":";
                size_t base = pr.body.find(obj);
                if (base != std::string::npos &&
                    numAfter(pr.body, "\"usd\"", base, px)) {
                    hasDelta =
                        numAfter(pr.body, "\"usd_24h_change\"", base, chg);
                    got = true;
                }
            }
            if (!got) {
                bool hc = false;
                if (binanceTicker(s, px, chg, hc)) {
                    got = true;
                    hasDelta = hc;
                } else if (coinbaseSpot(s, px)) {
                    got = true; // không có % đổi
                } else {
                    Logger::warn("Watch: price " + s + " all sources failed");
                    continue;
                }
            }
            it->ok = true;
            it->price = fmtUsd(px);
            if (hasDelta) it->delta = fmtPct(chg, it->up);
        }
        for (auto& s : cryptos) {
            WatchItem* it = findItem(s);
            if (!it || !it->hist.empty()) continue; // Yahoo đã có chart
            // Chart 14 ngày CoinGecko, rớt thì nến ngày Binance.
            auto ch = HttpClient::instance().get(
                "https://api.coingecko.com/api/v3/coins/" + cryptoId(s) +
                    "/market_chart?vs_currency=usd&days=14",
                {}, 20);
            size_t p0 = ch.success ? ch.body.find("\"prices\":") : std::string::npos;
            if (p0 == std::string::npos) continue;
            // prices:[[ts,px],...] — quét theo độ sâu ngoặc, lấy px (số thứ 2
            // mỗi cặp). Không dừng ở ']' trong mà hết mảng ngoài mới thôi.
            std::vector<float> pts;
            size_t p = ch.body.find('[', p0);
            if (p == std::string::npos) continue;
            int depth = 1;
            p++;
            int pair = 0;
            while (p < ch.body.size() && depth > 0) {
                char c = ch.body[p];
                if (c == '[') {
                    depth++;
                    p++;
                    continue;
                }
                if (c == ']') {
                    depth--;
                    p++;
                    continue;
                }
                if (c == ' ' || c == ',' || c == '\n' || c == '\r' ||
                    c == '\t') {
                    p++;
                    continue;
                }
                if ((c < '0' || c > '9') && c != '-' && c != '.') {
                    p++;
                    continue;
                }
                char* end = nullptr;
                double v = strtod(ch.body.c_str() + p, &end);
                if (end == ch.body.c_str() + p) {
                    p++;
                    continue;
                }
                if (depth >= 2 && pair % 2 == 1)
                    pts.push_back((float)v); // px (bỏ ts)
                if (depth >= 2) pair++;
                p = (size_t)(end - ch.body.c_str());
            }
            if (pts.size() >= 2) {
                // Downsample về <=60 điểm (giữ điểm cuối).
                if (pts.size() > 60) {
                    std::vector<float> ds;
                    float step = (float)(pts.size() - 1) / 59.0f;
                    for (int i = 0; i < 60; i++)
                        ds.push_back(pts[(size_t)(i * step)]);
                    pts.swap(ds);
                }
                it->hist = pts;
            } else {
                // Fallback: nến ngày Binance (close 60 phiên).
                auto kl = HttpClient::instance().get(
                    "https://api.binance.com/api/v3/klines?symbol=" + s +
                        "USDT&interval=1d&limit=60",
                    {}, 20);
                std::vector<float> cl;
                if (kl.success && binanceCloses(kl.body, cl))
                    it->hist = cl;
            }
        }
    }

    // Cổ phiếu VN: Yahoo (VCB.VN, giá VNĐ) trước, rớt thì VNDirect (nghìn).
    if (!stocks.empty()) {
        int64_t now = (int64_t)std::time(nullptr);
        int64_t from = now - 120LL * 86400;
        char url[256];
        for (auto& s : stocks) {
            WatchItem* it = findItem(s);
            if (!it) continue;
            double last = 0, prev = 0;
            std::vector<float> hist;
            if (yahooFetch(s + ".VN", last, prev, hist)) {
                it->ok = true;
                it->price = fmtInt(last);
                it->unit = "VNĐ";
                double chg = prev != 0 ? (last - prev) / prev * 100.0 : 0;
                it->delta = fmtPct(chg, it->up);
                it->hist = hist;
                continue;
            }
            snprintf(url, sizeof(url),
                     "https://dchart-api.vndirect.com.vn/dchart/history"
                     "?resolution=D&symbol=%s&from=%lld&to=%lld",
                     s.c_str(), (long long)from, (long long)now);
            auto r = HttpClient::instance().get(url, {}, 20);
            if (!r.success || r.body.empty()) {
                Logger::warn("Watch: vndirect " + s + " failed");
                continue;
            }
            std::vector<double> closes;
            if (!numArray(r.body, "\"c\":", closes, 200) || closes.size() < 2)
                continue;
            if (closes.size() > 60)
                closes.erase(closes.begin(), closes.end() - 60);
            last = closes.back();
            prev = closes[closes.size() - 2];
            double chg = prev != 0 ? (last - prev) / prev * 100.0 : 0;
            it->ok = true;
            it->price = fmtVnd(last);
            it->delta = fmtPct(chg, it->up);
            it->hist.clear();
            for (double c : closes) it->hist.push_back((float)c);
        }
    }

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_items = out;
    }
    return true;
}

} // namespace RomCloud
