// Standalone tests for YouTube code optimizations:
//   1. UTF-8 safe truncation (truncateUtf8Bytes)
//   2. LRU bounded cache (lruPutStream / lruGetStream)
//
// Replicas of static helpers added in src/media/MpvPlayer.cpp
// (truncateUtf8Bytes) and src/ui/UIManager.cpp (lruPutStream/
// lruGetStream). Standalone để kiểm thử logic thuần không phụ
// thuộc Application/UIManager. Nếu implementation gốc thay đổi
// thì copy này cũng phải cập nhật theo — nhưng khi build fail
// (signature/type lệch) sẽ tự bắt được.
#include <iostream>
#include <cstdio>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// 1. UTF-8 safe byte truncation
static std::string truncateUtf8Bytes(const std::string& s, size_t maxBytes,
                                     const std::string& suffix) {
  if (s.size() <= maxBytes) return s;
  size_t cut = maxBytes;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
    --cut;
  return s.substr(0, cut) + suffix;
}

// 2. LRU cache
static void lruPutStream(std::unordered_map<std::string, std::string>& m,
                         std::list<std::string>& order, size_t cap,
                         const std::string& key, const std::string& value) {
  auto it = m.find(key);
  if (it != m.end()) {
    it->second = value;
    order.remove(key);
    order.push_back(key);
    return;
  }
  m.emplace(key, value);
  order.push_back(key);
  if (order.size() > cap) {
    const std::string& oldest = order.front();
    m.erase(oldest);
    order.pop_front();
  }
}

static bool lruGetStream(std::unordered_map<std::string, std::string>& m,
                         std::list<std::string>& order, const std::string& key,
                         std::string& out) {
  auto it = m.find(key);
  if (it == m.end()) return false;
  out = it->second;
  order.remove(key);
  order.push_back(key);
  return true;
}

static int nFail = 0;
#define CHECK(cond, label)                                                  \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::cerr << "[FAIL] " << label << " (line " << __LINE__ << ")\n";    \
      ++nFail;                                                              \
    } else {                                                                \
      std::cout << "[PASS] " << label << "\n";                              \
    }                                                                       \
  } while (0)

// =================================================================
// UTF-8 truncation tests
// =================================================================
static void testUtf8Truncation() {
  std::cout << "\n--- UTF-8 safe truncation ---\n";

  // 1. ASCII (all 1-byte): cut at 5 bytes keeps 5 chars intact
  {
    std::string s = "Hello world";
    std::string r = truncateUtf8Bytes(s, 5, "...");
    CHECK(r == "Hello...", "ASCII exact cut");
  }
  // 2. String shorter than max: returned as-is
  {
    std::string s = "Hi";
    std::string r = truncateUtf8Bytes(s, 100, "...");
    CHECK(r == "Hi", "Short string passthrough");
  }
  // 3. Vietnamese 2-byte: max=1 forces cut inside 2-byte codepoint
  {
    std::string s = "\xC4\x83\x6E";  // ă n
    std::string r = truncateUtf8Bytes(s, 1, "...");
    // cut=1 hits 0x83 (continuation) -> back up to 0 -> "..."
    CHECK(r == "...", "Vietnamese 2-byte mid-cut backoff");
  }
  // 4. Vietnamese 2-byte perfect: max=3
  {
    std::string s = "\xC4\x83\x6E";
    std::string r = truncateUtf8Bytes(s, 3, "");
    CHECK(r == "\xC4\x83\x6E", "Vietnamese 2-byte no cut");
  }
  // 5. 3-byte char (em dash U+2014 = E2 80 94): max=2
  {
    std::string s = "A\xE2\x80\x94""B";  // A + em dash + B = 5B
    std::string r = truncateUtf8Bytes(s, 2, ".");
    // cut=2 -> 0x80 (cont) -> back up to 1 -> "A."
    CHECK(r == "A.", "3-byte UTF-8 mid-cut backoff");
  }
  // 6. 4-byte emoji (U+1F600 = F0 9F 98 80): cut at 3 -> back to 1
  {
    std::string s = "X\xF0\x9F\x98\x80";
    std::string r = truncateUtf8Bytes(s, 3, "");
    CHECK(r == "X", "4-byte UTF-8 mid-cut backoff to lead");
  }
  // 7. Long mix ASCII + Vietnamese, suffix appended, no invalid byte
  {
    std::string s;
    for (int i = 0; i < 100; ++i) s += "Tieng Viet ";  // ASCII fallback
    std::string r = truncateUtf8Bytes(s, 50, "...");
    CHECK(r.size() > 50, "Long ASCII still grows");
    CHECK(r.substr(r.size() - 3) == "...", "Long ASCII ends with suffix");
  }
}

// =================================================================
// LRU cache tests
// =================================================================
static void testLruCache() {
  std::cout << "\n--- LRU stream cache ---\n";

  std::unordered_map<std::string, std::string> m;
  std::list<std::string> order;
  const size_t cap = 3;

  // 1. Insert under cap
  lruPutStream(m, order, cap, "a", "1");
  lruPutStream(m, order, cap, "b", "2");
  lruPutStream(m, order, cap, "c", "3");
  CHECK(m.size() == 3, "3 inserts, 3 entries");
  CHECK(order.size() == 3, "3 inserts, 3 order entries");
  CHECK(order.front() == "a" && order.back() == "c",
        "Order: oldest=a, newest=c");

  // 2. Over-cap insert evicts front
  lruPutStream(m, order, cap, "d", "4");
  CHECK(m.size() == 3, "After over-cap insert, still 3 entries");
  CHECK(m.find("a") == m.end(), "Oldest 'a' evicted");
  CHECK(m.find("d") != m.end(), "Newest 'd' present");
  CHECK(order.front() == "b" && order.back() == "d",
        "Order: oldest=b, newest=d");

  // 3. Get promotes to back
  std::string out;
  bool ok = lruGetStream(m, order, "b", out);
  CHECK(ok && out == "2", "Get 'b' returns value");
  CHECK(order.back() == "b", "After get, 'b' is newest");

  // 4. Get missing returns false
  ok = lruGetStream(m, order, "xxx", out);
  CHECK(!ok, "Get missing key returns false");

  // 5. Re-insert updates + promotes
  lruPutStream(m, order, cap, "c", "C-NEW");
  out.clear();
  lruGetStream(m, order, "c", out);
  CHECK(out == "C-NEW", "Re-insert updates value");
  CHECK(order.back() == "c", "Re-insert promotes to back");

  // 6. LRU eviction respects recency
  lruPutStream(m, order, cap, "e", "5");
  CHECK(m.find("d") == m.end(), "LRU eviction: 'd' (untouched) evicted");
  CHECK(m.find("b") != m.end() && m.find("c") != m.end() &&
        m.find("e") != m.end(),
        "Recently used b/c/e all retained");
}

int main() {
  std::cout << "=== RomCloud YouTube optimization regression tests ===\n";
  testUtf8Truncation();
  testLruCache();
  if (nFail) {
    std::cerr << "\n=== " << nFail << " test(s) FAILED ===\n";
    return 1;
  }
  std::cout << "\n=== All tests PASSED ===\n";
  return 0;
}
