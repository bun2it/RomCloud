// LocalSendManager.cpp
// Xem LocalSendManager.h để hiểu kiến trúc.

#include "LocalSendManager.h"
#include "../config/AppConfig.h"
#include "../database/DatabaseManager.h"
#include "../database/RomIndexer.h"
#include "../logging/Logger.h"
#include "../rom/RomOrganizer.h"
#include "../ui/UIManager.h"


#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <ifaddrs.h>
#include <iostream>
#include <net/if.h>
#include <netinet/in.h>
#include <random>
#include <signal.h>
#include <sstream>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

namespace RomCloud {

// =============================================================
// Singleton
// =============================================================
LocalSendManager &LocalSendManager::instance() {
  static LocalSendManager inst;
  return inst;
}

LocalSendManager::~LocalSendManager() { stop(); }

// =============================================================
// Lifecycle
// =============================================================
bool LocalSendManager::start() {
  if (m_running.exchange(true))
    return true; // already running

  // 1. Load fingerprint (persist từ /mnt/SDCARD/.romcloud/localsend_fp)
  m_selfFingerprint = AppConfig::instance().getOrCreateLocalSendFingerprint();

  // 2. Alias
  m_selfAlias = AppConfig::instance().getLocalSendAlias();

  // 3. Lấy IP wlan0
  m_ownIp = LsUtil::getOwnIp("wlan0");
  if (m_ownIp.empty())
    m_ownIp = "0.0.0.0";

  Logger::info("LocalSend: start alias=" + m_selfAlias +
               " fp=" + m_selfFingerprint + " ip=" + m_ownIp);

  // 4. Setup UDP multicast
  if (!setupUdpSocket()) {
    Logger::warn("LocalSend: UDP setup failed (continuing without discovery)");
  }

  // 5. Setup TCP listen
  if (!setupTcpSocket()) {
    Logger::error("LocalSend: TCP setup failed");
    stop();
    return false;
  }

  // 6. Spawn threads
  m_discoveryThread = std::thread(&LocalSendManager::discoveryLoop, this);
  m_transferThread = std::thread(&LocalSendManager::transferLoop, this);

  Logger::info("LocalSend: started (port " +
               std::to_string(LocalSendProto::kPort) + ")");
  return true;
}

void LocalSendManager::stop() {
  if (!m_running.exchange(false))
    return;

  Logger::info("LocalSend: stopping...");

  // Đóng socket để thread accept/recv thoát
  if (m_udpFd >= 0) {
    shutdown(m_udpFd, SHUT_RDWR);
    close(m_udpFd);
    m_udpFd = -1;
  }
  if (m_tcpFd >= 0) {
    shutdown(m_tcpFd, SHUT_RDWR);
    close(m_tcpFd);
    m_tcpFd = -1;
  }

  if (m_discoveryThread.joinable())
    m_discoveryThread.join();
  if (m_transferThread.joinable())
    m_transferThread.join();

  // Dọn pending
  {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pending.clear();
  }

  Logger::info("LocalSend: stopped");
}

// =============================================================
// Fingerprint persistence
// =============================================================
// Note: thực ra getOrCreateLocalSendFingerprint là method của AppConfig (xem
// bên dưới). Placeholder ở đây, implementation thực tế trong
// LocalSendManager.cpp tiếp theo.

// =============================================================
// LsJson: parse JSON thủ công (không dùng lib)
// =============================================================
namespace LsJson {

bool getString(const std::string &json, const std::string &key,
               std::string &out) {
  // Tolerant: cho phép khoảng trắng trước/sau ':'  ("key" : "val")
  std::string qkey = "\"" + key + "\"";
  auto kpos = json.find(qkey);
  if (kpos == std::string::npos)
    return false;
  auto pos = kpos + qkey.size();
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                               json[pos] == '\r' || json[pos] == '\n'))
    ++pos;
  if (pos >= json.size() || json[pos] != ':')
    return false;
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                               json[pos] == '\r' || json[pos] == '\n'))
    ++pos;
  if (pos >= json.size() || json[pos] != '"')
    return false;
  ++pos;
  std::string val;
  for (; pos < json.size(); ++pos) {
    char c = json[pos];
    if (c == '\\' && pos + 1 < json.size()) {
      val += json[pos + 1];
      ++pos;
      continue;
    }
    if (c == '"') {
      out = val;
      return true;
    }
    val += c;
  }
  return false;
}

bool getUint64(const std::string &json, const std::string &key, uint64_t &out) {
  std::string qkey = "\"" + key + "\"";
  auto kpos = json.find(qkey);
  if (kpos == std::string::npos)
    return false;
  auto pos = kpos + qkey.size();
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                               json[pos] == '\r' || json[pos] == '\n'))
    ++pos;
  if (pos >= json.size() || json[pos] != ':')
    return false;
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
    ++pos;
  char *endp = nullptr;
  out = std::strtoull(json.c_str() + pos, &endp, 10);
  return endp != json.c_str() + pos;
}

bool getBool(const std::string &json, const std::string &key, bool &out) {
  std::string qkey = "\"" + key + "\"";
  auto kpos = json.find(qkey);
  if (kpos == std::string::npos)
    return false;
  auto pos = kpos + qkey.size();
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                               json[pos] == '\r' || json[pos] == '\n'))
    ++pos;
  if (pos >= json.size() || json[pos] != ':')
    return false;
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
    ++pos;
  if (json.compare(pos, 4, "true") == 0) {
    out = true;
    return true;
  }
  if (json.compare(pos, 5, "false") == 0) {
    out = false;
    return true;
  }
  return false;
}

bool getObject(const std::string &json, const std::string &key,
               std::string &out) {
  std::string qkey = "\"" + key + "\"";
  auto kpos = json.find(qkey);
  if (kpos == std::string::npos)
    return false;
  auto pos = kpos + qkey.size();
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                               json[pos] == '\r' || json[pos] == '\n'))
    ++pos;
  if (pos >= json.size() || json[pos] != ':')
    return false;
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                               json[pos] == '\r' || json[pos] == '\n'))
    ++pos;
  if (pos >= json.size() || json[pos] != '{')
    return false;
  int depth = 0;
  size_t start = pos;
  bool inString = false;
  for (; pos < json.size(); ++pos) {
    char c = json[pos];
    if (c == '"' && (pos == 0 || json[pos - 1] != '\\'))
      inString = !inString;
    if (inString)
      continue;
    if (c == '{')
      ++depth;
    else if (c == '}') {
      --depth;
      if (depth == 0) {
        out = json.substr(start, pos - start + 1);
        return true;
      }
    }
  }
  return false;
}

std::string escape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      out += c;
    }
  }
  return out;
}

} // namespace LsJson

// =============================================================
// LsUtil: helper chung
// =============================================================
namespace LsUtil {

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string makeUuid() { return randomHex(32); }

std::string randomHex(int n) {
  static std::mt19937_64 rng(
      std::chrono::steady_clock::now().time_since_epoch().count() ^
      std::hash<std::thread::id>{}(std::this_thread::get_id()));
  std::string out;
  out.reserve(n);
  const char hex[] = "0123456789abcdef";
  while (n-- > 0)
    out += hex[rng() & 0xF];
  return out;
}

std::string basenameOf(const std::string &path) {
  auto pos = path.find_last_of('/');
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string dirnameOf(const std::string &path) {
  auto pos = path.find_last_of('/');
  if (pos == std::string::npos)
    return ".";
  if (pos == 0)
    return "/";
  return path.substr(0, pos);
}

std::string lowerExt(const std::string &path) {
  auto base = basenameOf(path);
  auto pos = base.find_last_of('.');
  if (pos == std::string::npos)
    return "";
  std::string ext = base.substr(pos);
  for (auto &c : ext)
    c = std::tolower((unsigned char)c);
  return ext;
}

std::string getOwnIp(const std::string &iface) {
  struct ifaddrs *ifaddr = nullptr;
  if (getifaddrs(&ifaddr) < 0)
    return "";
  std::string out;
  for (auto *p = ifaddr; p; p = p->ifa_next) {
    if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
      continue;
    if (!iface.empty() && std::strcmp(p->ifa_name, iface.c_str()) != 0)
      continue;
    char buf[INET_ADDRSTRLEN] = {0};
    auto *a = (struct sockaddr_in *)p->ifa_addr;
    if (inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf))) {
      out = buf;
      break;
    }
  }
  freeifaddrs(ifaddr);
  return out;
}

bool mkdirRecursive(const std::string &path, mode_t mode) {
  std::string accum;
  bool ok = true;
  for (size_t i = 0; i < path.size(); ++i) {
    accum += path[i];
    if (path[i] == '/' || i == path.size() - 1) {
      if (accum.empty() || accum == "/")
        continue;
      if (mkdir(accum.c_str(), mode) != 0 && errno != EEXIST) {
        Logger::warn("LocalSend: mkdir(" + accum +
                     ") failed: " + std::strerror(errno));
        ok = false;
      }
    }
  }
  return ok;
}

std::string sha256OfFile(const std::string &path) {
  std::string cmd = "sha256sum '" + path + "' 2>/dev/null | cut -d' ' -f1";
  FILE *p = popen(cmd.c_str(), "r");
  if (!p)
    return "";
  char buf[128] = {0};
  if (!fgets(buf, sizeof(buf), p)) {
    pclose(p);
    return "";
  }
  pclose(p);
  std::string s = buf;
  while (!s.empty() &&
         (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
    s.pop_back();
  return s;
}

std::string humanSize(uint64_t bytes) {
  const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  double v = (double)bytes;
  int u = 0;
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    ++u;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
  return std::string(buf);
}

std::string humanSpeed(uint32_t bps) { return humanSize(bps) + "/s"; }

bool isPathSafe(const std::string &absPath) {
  const std::string root = "/mnt/SDCARD";
  if (absPath.compare(0, root.size(), root) != 0)
    return false;
  // Phải là đúng root hoặc root + '/' (tránh /mnt/SDCARDEvil)
  if (absPath.size() > root.size() && absPath[root.size()] != '/')
    return false;
  if (absPath.find("..") != std::string::npos)
    return false;
  if (absPath.find("//") != std::string::npos)
    return false;
  // Không cho phép ký tự null / control
  for (char c : absPath) {
    if (c == '\0' || (c >= 0 && c < 32))
      return false;
  }
  return true;
}

uint64_t sdFreeBytes(const std::string &path) {
  struct statvfs sv{};
  if (::statvfs(path.c_str(), &sv) != 0)
    return 0;
  return (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
}

} // namespace LsUtil

// =============================================================
// UDP Discovery
// =============================================================
bool LocalSendManager::setupUdpSocket() {
  m_udpFd = socket(AF_INET, SOCK_DGRAM, 0);
  if (m_udpFd < 0) {
    Logger::error("LocalSend: UDP socket failed: " +
                  std::string(std::strerror(errno)));
    return false;
  }
  int yes = 1;
  setsockopt(m_udpFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  setsockopt(m_udpFd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

  // Bind to all interfaces on port 53317
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(LocalSendProto::kPort);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(m_udpFd, (sockaddr *)&addr, sizeof(addr)) < 0) {
    Logger::error("LocalSend: UDP bind failed: " +
                  std::string(std::strerror(errno)));
    close(m_udpFd);
    m_udpFd = -1;
    return false;
  }

  // Join multicast group 224.0.0.167
  ip_mreq mreq{};
  mreq.imr_multiaddr.s_addr = inet_addr(LocalSendProto::kMulticastAddr);
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  if (setsockopt(m_udpFd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) <
      0) {
    Logger::warn(
        "LocalSend: IP_ADD_MEMBERSHIP failed (discovery may not work): " +
        std::string(std::strerror(errno)));
  }

  // Set TTL để multicast không thoát khỏi LAN
  int ttl = 4;
  setsockopt(m_udpFd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

  // Loopback disable để không nhận lại gói của chính mình
  int loop = 0;
  setsockopt(m_udpFd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

  Logger::info("LocalSend: UDP multicast joined " +
               std::string(LocalSendProto::kMulticastAddr));
  return true;
}

void LocalSendManager::sendMulticastAnnounce() {
  if (m_udpFd < 0)
    return;
  std::string json = buildDeviceInfoJson(true);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(LocalSendProto::kPort);
  addr.sin_addr.s_addr = inet_addr(LocalSendProto::kMulticastAddr);
  ssize_t sent = sendto(m_udpFd, json.data(), json.size(), 0, (sockaddr *)&addr,
                        sizeof(addr));
  // Fallback broadcast: một số router/AP chặn multicast → gửi kèm broadcast
  // để laptop/Brick khác trong cùng subnet vẫn thấy.
  {
    sockaddr_in bcast{};
    bcast.sin_family = AF_INET;
    bcast.sin_port = htons(LocalSendProto::kPort);
    bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(m_udpFd, json.data(), json.size(), 0, (sockaddr *)&bcast,
           sizeof(bcast));
  }
  // Diagnostic: log mỗi lần announce để verify TrimUI có thực sự gửi đi
  // (issue #1: laptop không thấy TrimUI - giúp user confirm TrimUI→multicast
  // OK)
  static auto lastLog =
      std::chrono::steady_clock::now() - std::chrono::seconds(60);
  auto now = std::chrono::steady_clock::now();
  if (now - lastLog >= std::chrono::seconds(30) || sent < 0) {
    lastLog = now;
    if (sent < 0) {
      Logger::warn("LocalSend: sendMulticastAnnounce sendto failed: " +
                   std::string(std::strerror(errno)));
    } else {
      Logger::info("LocalSend: announce → 224.0.0.167:" +
                   std::to_string(LocalSendProto::kPort) +
                   " alias=" + m_selfAlias + " ip=" + m_ownIp +
                   " bytes=" + std::to_string(sent));
    }
  }
}

void LocalSendManager::sendUnicastAnnounce(const std::string &ip, int port,
                                           bool announce) {
  if (m_udpFd < 0 || ip.empty())
    return;
  std::string json = buildDeviceInfoJson(announce);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1)
    return;
  sendto(m_udpFd, json.data(), json.size(), 0, (sockaddr *)&addr, sizeof(addr));
}

void LocalSendManager::pruneStaleDevices(int64_t maxAgeMs) {
  int64_t now = LsUtil::nowMs();
  std::lock_guard<std::mutex> lock(m_knownMutex);
  m_knownDevices.erase(std::remove_if(m_knownDevices.begin(),
                                      m_knownDevices.end(),
                                      [&](const LsDeviceInfo &d) {
                                        return (now - d.lastSeenMs) > maxAgeMs;
                                      }),
                       m_knownDevices.end());
}

void LocalSendManager::refreshDiscovery() {
  // Fix rescan mat device: GIU list cu lam cache (khong prune o day),
  // dong thoi unicast thang toi cac IP da biet + burst multicast.
  // Ly do: multicast tren nhieu AP/router bi rot goi lan 2 tro di,
  // chi gui multicast thi peer khong nghe thay -> khong reply -> list trang.
  // Unicast toi IP cu thi ti le toi gan nhu 100% -> peer reply ngay.
  std::vector<std::string> cachedIps;
  std::vector<int> cachedPorts;
  {
    std::lock_guard<std::mutex> lock(m_knownMutex);
    for (auto &d : m_knownDevices) {
      if (!d.ip.empty()) {
        cachedIps.push_back(d.ip);
        cachedPorts.push_back(d.port);
      }
    }
  }
  std::thread([this, cachedIps, cachedPorts]() {
    // 1. Unicast go thang may quen (kich peer reply unicast ve ngay)
    for (size_t i = 0; i < cachedIps.size() && m_running; ++i) {
      int p = (i < cachedPorts.size() && cachedPorts[i] > 0)
                  ? cachedPorts[i]
                  : LocalSendProto::kPort;
      sendUnicastAnnounce(cachedIps[i], p, true);
    }
    // 2. Burst 3 goi multicast cach nhau 150ms de vuot qua loss WiFi/router
    // chan multicast lan dau — chay nen de khong block UI.
    for (int i = 0; i < 3 && m_running; ++i) {
      sendMulticastAnnounce();
      if (i < 2)
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
  }).detach();
}

void LocalSendManager::handleDiscoveryPacket(const char *json, size_t len,
                                             const std::string &fromIp) {
  LsDeviceInfo dev;
  std::string jstr(json, len);
  if (!LsJson::getString(jstr, "alias", dev.alias))
    return;
  // Bỏ qua gói của chính mình: so fingerprint (không so alias vì 2 Brick
  // mặc định có thể trùng alias → trước đây lọc nhầm nhau nên lúc thấy lúc
  // không).
  LsJson::getString(jstr, "fingerprint", dev.fingerprint);
  if (!dev.fingerprint.empty() && dev.fingerprint == m_selfFingerprint)
    return;
  if (fromIp == m_ownIp)
    return;
  LsJson::getString(jstr, "version", dev.version);
  LsJson::getString(jstr, "deviceModel", dev.deviceModel);
  LsJson::getString(jstr, "deviceType", dev.deviceType);
  LsJson::getString(jstr, "fingerprint", dev.fingerprint);
  LsJson::getString(jstr, "protocol", dev.protocol);
  LsJson::getBool(jstr, "download", dev.download);
  LsJson::getBool(jstr, "announce", dev.announce);
  uint64_t p = 0;
  if (LsJson::getUint64(jstr, "port", p))
    dev.port = (int)p;
  dev.ip = fromIp;
  dev.lastSeenMs = LsUtil::nowMs();
  addOrUpdateKnownDevice(dev);

  if (dev.announce)
    sendUnicastAnnounce(dev.ip, dev.port);
}

void LocalSendManager::addOrUpdateKnownDevice(const LsDeviceInfo &dev) {
  std::lock_guard<std::mutex> lock(m_knownMutex);
  for (auto &d : m_knownDevices) {
    if (!dev.fingerprint.empty() && d.fingerprint == dev.fingerprint) {
      d = dev;
      return;
    }
    if (d.ip == dev.ip && d.port == dev.port) {
      d = dev;
      return;
    }
  }
  m_knownDevices.push_back(dev);
  if ((int)m_knownDevices.size() > LocalSendProto::kMaxKnownDevices) {
    m_knownDevices.erase(m_knownDevices.begin());
  }
}

std::vector<LsDeviceInfo> LocalSendManager::knownDevices() {
  std::lock_guard<std::mutex> lock(m_knownMutex);
  return m_knownDevices;
}

void LocalSendManager::clearKnownDevices() {
  std::lock_guard<std::mutex> lock(m_knownMutex);
  m_knownDevices.clear();
}

// =============================================================
// buildDeviceInfoJson - dùng cho announce và /info response
// =============================================================
namespace { // anonymous namespace cho helper
std::string DeviceInfoJson(const LocalSendManager &self, bool announce) {
  std::ostringstream ss;
  ss << "{";
  ss << "\"alias\":\"" << LsJson::escape(self.alias()) << "\",";
  ss << "\"version\":\"" << LocalSendProto::kVersion << "\",";
  ss << "\"deviceModel\":\"TrimUI Brick Pro\",";
  ss << "\"deviceType\":\"" << LocalSendProto::kDeviceType << "\",";
  ss << "\"fingerprint\":\"" << self.fingerprint() << "\",";
  ss << "\"port\":" << LocalSendProto::kPort << ",";
  ss << "\"protocol\":\"" << LocalSendProto::kProtocol << "\",";
  ss << "\"download\":false,";
  ss << "\"announce\":" << (announce ? "true" : "false");
  ss << "}";
  return ss.str();
}
} // anonymous namespace

std::string LocalSendManager::buildDeviceInfoJson(bool announce) const {
  return DeviceInfoJson(*this, announce);
}

// =============================================================
// TCP Transfer Setup
// =============================================================
bool LocalSendManager::setupTcpSocket() {
  m_tcpFd = socket(AF_INET, SOCK_STREAM, 0);
  if (m_tcpFd < 0) {
    Logger::error("LocalSend: TCP socket failed: " +
                  std::string(std::strerror(errno)));
    return false;
  }
  int yes = 1;
  setsockopt(m_tcpFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(LocalSendProto::kPort);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(m_tcpFd, (sockaddr *)&addr, sizeof(addr)) < 0) {
    Logger::error("LocalSend: TCP bind failed: " +
                  std::string(std::strerror(errno)));
    close(m_tcpFd);
    m_tcpFd = -1;
    return false;
  }
  if (listen(m_tcpFd, 8) < 0) {
    Logger::error("LocalSend: TCP listen failed: " +
                  std::string(std::strerror(errno)));
    close(m_tcpFd);
    m_tcpFd = -1;
    return false;
  }
  Logger::info("LocalSend: TCP listening on :" +
               std::to_string(LocalSendProto::kPort));
  return true;
}

void LocalSendManager::transferLoop() {
  while (m_running) {
    sockaddr_in cli{};
    socklen_t cl = sizeof(cli);
    int fd = accept(m_tcpFd, (sockaddr *)&cli, &cl);
    if (fd < 0) {
      if (m_running)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    // Log incoming TCP connection (verify laptop/peer đã kết nối vào TrimUI)
    char peerIp[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &cli.sin_addr, peerIp, sizeof(peerIp));
    Logger::info("LocalSend: TCP accepted from " + std::string(peerIp) + ":" +
                 std::to_string(ntohs(cli.sin_port)));

    // Per-client socket timeout
    timeval tv{30, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::thread([this, fd]() {
      handleHttpClient(fd);
      close(fd);
    }).detach();
  }
}

void LocalSendManager::discoveryLoop() {
  auto lastPing = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  auto lastPrune = std::chrono::steady_clock::now();
  while (m_running) {
    auto now = std::chrono::steady_clock::now();
    if (now - lastPing >=
        std::chrono::seconds(LocalSendProto::kDiscoveryPingIntervalSec)) {
      sendMulticastAnnounce();
      lastPing = now;
    }
    // Dọn thiết bị quá 20s không thấy announce (tránh list ma lúc thấy lúc
    // không)
    if (now - lastPrune >= std::chrono::seconds(5)) {
      pruneStaleDevices(20000);
      lastPrune = now;
    }
    if (m_udpFd < 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    timeval tv{0, LocalSendProto::kDiscoveryRecvTimeoutMs * 1000};
    setsockopt(m_udpFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[2048];
    sockaddr_in from{};
    socklen_t fl = sizeof(from);
    ssize_t n =
        recvfrom(m_udpFd, buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &fl);
    if (n > 0) {
      buf[n] = 0;
      char ip[INET_ADDRSTRLEN] = {0};
      inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
      handleDiscoveryPacket(buf, (size_t)n, ip);
    }
  }
}

// =============================================================
// HTTP Request/Response (raw socket, không dùng lib)
// =============================================================
static std::string readLine(int fd) {
  std::string line;
  char c;
  while (true) {
    ssize_t n = recv(fd, &c, 1, 0);
    if (n <= 0)
      return line;
    if (c == '\r')
      continue;
    if (c == '\n')
      break;
    line += c;
    if (line.size() > 8192)
      break;
  }
  return line;
}

bool LocalSendManager::readHttpRequest(
    int fd, std::string &method, std::string &path,
    std::map<std::string, std::string> &headers, std::string &body) {
  // Request line: METHOD SP PATH SP HTTP/1.1 CRLF
  std::string reqLine = readLine(fd);
  if (reqLine.empty())
    return false;
  auto sp1 = reqLine.find(' ');
  if (sp1 == std::string::npos)
    return false;
  auto sp2 = reqLine.find(' ', sp1 + 1);
  if (sp2 == std::string::npos)
    return false;
  method = reqLine.substr(0, sp1);
  path = reqLine.substr(sp1 + 1, sp2 - sp1 - 1);

  // Headers
  int contentLength = 0;
  while (true) {
    std::string line = readLine(fd);
    if (line.empty())
      break;
    auto colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    std::string k = line.substr(0, colon);
    std::string v = line.substr(colon + 1);
    // trim
    while (!v.empty() && v.front() == ' ')
      v.erase(v.begin());
    while (!v.empty() && v.back() == '\r')
      v.pop_back();
    while (!v.empty() && v.back() == '\n')
      v.pop_back();
    {
      std::string lk = k;
      for (auto &c : lk)
        c = std::tolower((unsigned char)c);
      if (lk == "content-length")
        contentLength = std::atoi(v.c_str());
      headers[lk] = v;
      headers[k] = v;
    }
  }

  // Spec interop: Dart/Flutter HttpClient (và OkHttp) có thể gửi
  // "Expect: 100-continue" và chờ server trả "100 Continue" trước khi
  // gửi body. Không trả lời → client đợi timeout rồi mới gửi body (hoặc
  // gửi body rỗng) → prepare-upload nhận body="" → 400 "missing files".
  {
    auto it = headers.find("expect");
    if (it != headers.end()) {
      std::string ev = it->second;
      for (auto &c : ev)
        c = std::tolower((unsigned char)c);
      if (ev.find("100-continue") != std::string::npos) {
        const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
        ::send(fd, cont, std::strlen(cont), MSG_NOSIGNAL);
      }
    }
  }

  // Body (hỗ trợ Content-Length + Transfer-Encoding: chunked của
  // Dart HttpClient/OkHttp — thiếu chunked → body rỗng → 400 "missing files")
  {
    std::string te;
    auto it = headers.find("transfer-encoding");
    if (it != headers.end())
      te = it->second;
    std::string tel = te;
    for (auto &c : tel)
      c = std::tolower((unsigned char)c);
    if (tel.find("chunked") != std::string::npos) {
      body.clear();
      while (true) {
        std::string szLine = readLine(fd);
        auto semi = szLine.find(';');
        if (semi != std::string::npos)
          szLine = szLine.substr(0, semi);
        while (!szLine.empty() && szLine.front() == ' ')
          szLine.erase(szLine.begin());
        while (!szLine.empty() &&
               (szLine.back() == ' ' || szLine.back() == '\r'))
          szLine.pop_back();
        unsigned long chunk =
            szLine.empty() ? 0 : std::strtoul(szLine.c_str(), nullptr, 16);
        if (chunk == 0) {
          readLine(fd);
          break;
        }
        if (body.size() + chunk > (size_t)64 * 1024 * 1024)
          break;
        size_t base = body.size();
        body.resize(base + chunk);
        size_t got = 0;
        while (got < chunk) {
          ssize_t n = recv(fd, body.data() + base + got, chunk - got, 0);
          if (n <= 0)
            break;
          got += n;
        }
        body.resize(base + got);
        readLine(fd); // CRLF cuối chunk
        if (got < chunk)
          break;
      }
    } else if (contentLength > 0 && contentLength < 64 * 1024 * 1024) {
      body.resize(contentLength);
      size_t got = 0;
      while (got < (size_t)contentLength) {
        ssize_t n = recv(fd, body.data() + got, contentLength - got, 0);
        if (n <= 0)
          break;
        got += n;
      }
      body.resize(got);
    }
  }
  return true;
}

void LocalSendManager::sendJsonResponse(int fd, int statusCode,
                                        const std::string &jsonBody) {
  const char *statusText = "OK";
  switch (statusCode) {
  case 200:
    statusText = "OK";
    break;
  case 400:
    statusText = "Bad Request";
    break;
  case 403:
    statusText = "Forbidden";
    break;
  case 404:
    statusText = "Not Found";
    break;
  case 408:
    statusText = "Request Timeout";
    break;
  case 413:
    statusText = "Payload Too Large";
    break;
  case 422:
    statusText = "Unprocessable Entity";
    break;
  case 500:
    statusText = "Internal Server Error";
    break;
  case 503:
    statusText = "Service Unavailable";
    break;
  }
  std::ostringstream ss;
  ss << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n";
  ss << "Content-Type: application/json\r\n";
  ss << "Content-Length: " << jsonBody.size() << "\r\n";
  ss << "Connection: close\r\n";
  ss << "\r\n";
  ss << jsonBody;
  std::string resp = ss.str();
  ::send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
}

void LocalSendManager::sendEmptyResponse(int fd, int statusCode) {
  sendJsonResponse(fd, statusCode, "{}");
}

// =============================================================
// HTTP Routing
// =============================================================
void LocalSendManager::handleHttpClient(int fd) {
  std::string method, path, body;
  std::map<std::string, std::string> headers;
  if (!readHttpRequest(fd, method, path, headers, body)) {
    Logger::warn("LocalSend: HTTP read failed from peer");
    sendEmptyResponse(fd, 400);
    return;
  }

  // Tách path & query
  std::string route = path;
  std::string query;
  auto qpos = path.find('?');
  if (qpos != std::string::npos) {
    route = path.substr(0, qpos);
    query = path.substr(qpos + 1);
  }

  std::string fromIp;
  {
    sockaddr_in peer{};
    socklen_t pl = sizeof(peer);
    if (getpeername(fd, (sockaddr *)&peer, &pl) == 0) {
      char buf[INET_ADDRSTRLEN] = {0};
      inet_ntop(AF_INET, &peer.sin_addr, buf, sizeof(buf));
      fromIp = buf;
    }
  }

  Logger::info("LocalSend: HTTP " + method + " " + route + " from " + fromIp +
               " body=" + std::to_string(body.size()) + "B");

  if (method == "GET" && route == "/api/localsend/v2/info") {
    handleInfo(fd);
  } else if (method == "POST" && route == "/api/localsend/v2/register") {
    handleRegister(fd, body);
  } else if (method == "POST" && route == "/api/localsend/v2/prepare-upload") {
    handlePrepareUpload(fd, body, fromIp);
  } else if (method == "POST" && route == "/api/localsend/v2/upload") {
    handleFileUpload(fd, query, headers, body);
  } else if (method == "POST" && route == "/api/localsend/v2/cancel") {
    handleCancel(fd, query);
  } else {
    Logger::warn("LocalSend: HTTP 404 " + method + " " + route);
    sendJsonResponse(fd, 404, "{\"error\":\"not found\"}");
  }
}

void LocalSendManager::handleInfo(int fd) {
  sendJsonResponse(fd, 200, buildDeviceInfoJson(false));
}

void LocalSendManager::handleRegister(int fd, const std::string &body) {
  // Spec §3.1 HTTP fallback discovery: client gửi info của nó, ta lưu + reply.
  LsDeviceInfo dev;
  if (LsJson::getString(body, "alias", dev.alias) && dev.alias != m_selfAlias) {
    LsJson::getString(body, "version", dev.version);
    LsJson::getString(body, "deviceModel", dev.deviceModel);
    LsJson::getString(body, "deviceType", dev.deviceType);
    LsJson::getString(body, "fingerprint", dev.fingerprint);
    LsJson::getString(body, "protocol", dev.protocol);
    LsJson::getBool(body, "download", dev.download);
    uint64_t p = 0;
    if (LsJson::getUint64(body, "port", p))
      dev.port = (int)p;
    sockaddr_in peer{};
    socklen_t pl = sizeof(peer);
    if (getpeername(fd, (sockaddr *)&peer, &pl) == 0) {
      char ip[INET_ADDRSTRLEN] = {0};
      inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
      dev.ip = ip;
    }
    dev.lastSeenMs = LsUtil::nowMs();
    addOrUpdateKnownDevice(dev);
  }
  sendJsonResponse(fd, 200, buildDeviceInfoJson(false));
}

// =============================================================
// prepare-upload (spec §4.1)
// =============================================================
void LocalSendManager::handlePrepareUpload(int fd, const std::string &body,
                                           const std::string &fromIp) {
  LsUploadRequest req;
  req.sessionId = LsUtil::makeUuid();
  req.fromIp = fromIp;
  req.lastUpdateMs = LsUtil::nowMs();

  // Parse "info":{...} → fromAlias
  std::string infoObj;
  if (LsJson::getObject(body, "info", infoObj)) {
    LsJson::getString(infoObj, "alias", req.fromAlias);
  }
  if (req.fromAlias.empty())
    req.fromAlias = fromIp;

  // Parse "files":{...} → lấy file đầu tiên (RomCloud hỗ trợ 1 file/lần)
  std::string filesObj;
  if (!LsJson::getObject(body, "files", filesObj)) {
    sendJsonResponse(fd, 400, "{\"error\":\"missing files\"}");
    return;
  }

  // Tìm "fid":{...} đầu tiên
  auto fidPos = filesObj.find('"');
  if (fidPos == std::string::npos) {
    sendJsonResponse(fd, 400, "{\"error\":\"empty files\"}");
    return;
  }
  auto fidEnd = filesObj.find('"', fidPos + 1);
  if (fidEnd == std::string::npos) {
    sendJsonResponse(fd, 400, "{\"error\":\"bad file id\"}");
    return;
  }
  req.fileId = filesObj.substr(fidPos + 1, fidEnd - fidPos - 1);

  // Tìm {...} ngay sau fid
  auto fileObjStart = filesObj.find('{', fidEnd);
  if (fileObjStart == std::string::npos) {
    sendJsonResponse(fd, 400, "{\"error\":\"no file body\"}");
    return;
  }
  int depth = 1;
  size_t fileObjPos = fileObjStart + 1;
  bool inString = false;
  size_t fileObjEnd = std::string::npos;
  for (; fileObjPos < filesObj.size(); ++fileObjPos) {
    char c = filesObj[fileObjPos];
    if (c == '"' && (fileObjPos == 0 || filesObj[fileObjPos - 1] != '\\'))
      inString = !inString;
    if (inString)
      continue;
    if (c == '{')
      ++depth;
    else if (c == '}') {
      --depth;
      if (depth == 0) {
        fileObjEnd = fileObjPos;
        break;
      }
    }
  }
  if (fileObjEnd == std::string::npos) {
    sendJsonResponse(fd, 400, "{\"error\":\"bad file body\"}");
    return;
  }
  std::string fileObj =
      filesObj.substr(fileObjStart, fileObjEnd - fileObjStart + 1);

  LsJson::getString(fileObj, "id", req.file.id);
  if (req.file.id.empty())
    req.file.id = req.fileId;
  LsJson::getString(fileObj, "fileName", req.file.fileName);
  LsJson::getString(fileObj, "relativePath", req.file.relativePath);
  LsJson::getString(fileObj, "fileType", req.file.fileType);
  LsJson::getString(fileObj, "sha256", req.file.sha256);
  LsJson::getString(fileObj, "preview", req.file.preview);
  LsJson::getUint64(fileObj, "size", req.file.size);

  // Mở rộng RomCloud: parse game metadata (sender gửi từ DB).
  // Receiver sẽ render dialog duyệt với cover art + tên game.
  {
    uint64_t u64 = 0;
    if (LsJson::getUint64(fileObj, "gameId", u64))
      req.file.gameId = (int64_t)u64;
    if (LsJson::getUint64(fileObj, "systemId", u64))
      req.file.systemId = (int)u64;
  }
  LsJson::getString(fileObj, "systemCode", req.file.systemCode);
  LsJson::getString(fileObj, "systemName", req.file.systemName);
  LsJson::getString(fileObj, "gameTitle", req.file.gameTitle);
  LsJson::getString(fileObj, "coverPath", req.file.coverPath);

  if (req.file.id.empty())
    req.file.id = req.fileId;

  if (req.file.fileName.empty()) {
    sendJsonResponse(fd, 400, "{\"error\":\"missing fileName\"}");
    return;
  }

  // Resolve target path (3-tier)
  std::string target = resolveTargetPath(req.file);
  if (target.empty()) {
    sendJsonResponse(fd, 403, "{\"error\":\"unsafe path\"}");
    return;
  }
  req.savedPath = target;

  // Spec §4.1 response: { sessionId, files: { fileId: token } }
  // token phải sinh TRƯỚC khi push pending để bản lưu có token.
  req.fileToken = LsUtil::makeUuid();

  // Lưu pending
  {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    // Evict cũ nếu vượt quá
    if ((int)m_pending.size() >= LocalSendProto::kMaxPendingRequests) {
      m_pending.erase(m_pending.begin());
    }
    m_pending.push_back(req);
  }

  // Bắn callback cho UI
  {
    std::lock_guard<std::mutex> lock(m_cbMutex);
    if (m_onUserPrompt)
      m_onUserPrompt(req);
  }

  std::ostringstream ss;
  ss << "{\"sessionId\":\"" << req.sessionId << "\",\"files\":{"
     << "\"" << LsJson::escape(req.fileId) << "\":\"" << req.fileToken
     << "\"}}";
  sendJsonResponse(fd, 200, ss.str());

  Logger::info("LocalSend: prepare-upload from " + req.fromAlias + " file=" +
               req.file.fileName + " size=" + std::to_string(req.file.size) +
               " saved=" + req.savedPath);
}

// =============================================================
// upload (spec §4.2) - streaming write binary
// =============================================================
void LocalSendManager::handleFileUpload(
    int fd, const std::string &query,
    const std::map<std::string, std::string> &headers,
    const std::string &initialBody) {
  std::map<std::string, std::string> qparams;
  size_t pos = 0;
  while (pos < query.size()) {
    auto amp = query.find('&', pos);
    std::string pair = query.substr(
        pos, amp == std::string::npos ? std::string::npos : amp - pos);
    auto eq = pair.find('=');
    if (eq != std::string::npos)
      qparams[pair.substr(0, eq)] = pair.substr(eq + 1);
    if (amp == std::string::npos)
      break;
    pos = amp + 1;
  }
  std::string sessionId = qparams["sessionId"];
  std::string fileId = qparams["fileId"];
  std::string token = qparams["token"];

  // Spec §4.2: client Flutter/Localsend gửi token từ prepare-upload.
  // RomCloud-to-RomCloud cũng gửi token (sender parse files[fileId]).
  int reqIdx = -1;
  {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    for (size_t i = 0; i < m_pending.size(); ++i) {
      auto &r = m_pending[i];
      if (r.sessionId == sessionId && r.fileId == fileId) {
        reqIdx = (int)i;
        break;
      }
    }
  }
  if (reqIdx < 0) {
    sendJsonResponse(fd, 404, "{\"error\":\"unknown session\"}");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    LsUploadRequest &r = m_pending[(size_t)reqIdx];
    // Chấp nhận cả client cũ không gửi token, nhưng nếu pending có token
    // mà client gửi token sai → 403 (tránh nhầm session).
    if (!r.fileToken.empty() && !token.empty() && r.fileToken != token) {
      sendJsonResponse(fd, 403, "{\"error\":\"invalid token\"}");
      return;
    }
  }
  // Dùng index (không giữ raw pointer) vì m_pending có thể push thêm
  // từ connection khác gây reallocate vector trong lúc chờ approve.
  auto getState = [&]() -> LsUploadRequest::State {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    return m_pending[(size_t)reqIdx].state;
  };
  auto setState = [&](LsUploadRequest::State s) {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pending[(size_t)reqIdx].state = s;
  };

  // Đợi user approve (poll 200ms, tối đa 60s)
  const int maxWait = LocalSendProto::kUploadApprovalTimeoutSec * 5;
  for (int i = 0; i < maxWait; ++i) {
    auto s = getState();
    if (s == LsUploadRequest::APPROVED)
      break;
    if (s == LsUploadRequest::REJECTED) {
      sendJsonResponse(fd, 403, "{\"error\":\"rejected\"}");
      return;
    }
    if (!m_running) {
      sendJsonResponse(fd, 503, "{\"error\":\"shutting down\"}");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  if (getState() != LsUploadRequest::APPROVED) {
    sendJsonResponse(fd, 408, "{\"error\":\"timeout waiting approval\"}");
    setState(LsUploadRequest::FAILED);
    return;
  }

  std::string savedPath;
  std::string wantSha;
  std::string wantName;
  uint64_t wantSize = 0;
  {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    savedPath = m_pending[(size_t)reqIdx].savedPath;
    wantSha = m_pending[(size_t)reqIdx].file.sha256;
    wantName = m_pending[(size_t)reqIdx].file.fileName;
    wantSize = m_pending[(size_t)reqIdx].file.size;
  }

  if (!LsUtil::mkdirRecursive(LsUtil::dirnameOf(savedPath))) {
    sendJsonResponse(fd, 500, "{\"error\":\"mkdir failed\"}");
    setState(LsUploadRequest::FAILED);
    return;
  }

  // Check dung lượng trống trước khi ghi (tránh đầy thẻ giữa chừng)
  if (wantSize > 0) {
    uint64_t freeB = LsUtil::sdFreeBytes("/mnt/SDCARD");
    if (freeB < wantSize) {
      Logger::warn("LocalSend: not enough space for " + wantName + " need=" +
                   std::to_string(wantSize) + " free=" + std::to_string(freeB));
      setState(LsUploadRequest::FAILED);
      LsUploadRequest snap;
      {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        snap = m_pending[(size_t)reqIdx];
      }
      std::lock_guard<std::mutex> lock(m_cbMutex);
      if (m_onComplete)
        m_onComplete(snap);
      sendJsonResponse(fd, 413, "{\"error\":\"not enough space\"}");
      return;
    }
  }

  // Độ dài kỳ vọng: ưu tiên Content-Length header, fallback = size từ prepare
  uint64_t expectedLen = wantSize;
  {
    auto it = headers.find("content-length");
    if (it != headers.end()) {
      uint64_t cl = (uint64_t)std::strtoull(it->second.c_str(), nullptr, 10);
      if (cl > 0)
        expectedLen = cl;
    }
  }

  setState(LsUploadRequest::RECEIVING);
  std::ofstream out(savedPath, std::ios::binary | std::ios::trunc);
  if (!out) {
    sendJsonResponse(fd, 500, "{\"error\":\"cannot open file for write\"}");
    setState(LsUploadRequest::FAILED);
    return;
  }

  char buf[LocalSendProto::kUploadChunkSize];
  uint64_t totalRead = 0;
  uint64_t lastTickBytes = 0;
  int64_t lastTickMs = LsUtil::nowMs();

  auto pumpProgress = [&](bool force) {
    int64_t now = LsUtil::nowMs();
    if (!force && now - lastTickMs < 500)
      return;
    uint32_t bps = (lastTickMs == now)
                       ? 0
                       : (uint32_t)((totalRead - lastTickBytes) * 1000 /
                                    (uint64_t)(now - lastTickMs));
    LsUploadRequest snap;
    {
      std::lock_guard<std::mutex> lock(m_pendingMutex);
      m_pending[(size_t)reqIdx].receivedBytes = totalRead;
      m_pending[(size_t)reqIdx].bytesPerSec = bps;
      m_pending[(size_t)reqIdx].lastUpdateMs = now;
      snap = m_pending[(size_t)reqIdx];
    }
    lastTickMs = now;
    lastTickBytes = totalRead;
    std::lock_guard<std::mutex> lock(m_cbMutex);
    if (m_onProgress)
      m_onProgress(snap);
  };

  // readHttpRequest đã đọc sẵn 1 phần body (initialBody) — ghi trước để
  // không mất bytes đầu (fix treo/kẹt + file thiếu đầu).
  if (!initialBody.empty()) {
    out.write(initialBody.data(), (std::streamsize)initialBody.size());
    if (!out) {
      sendJsonResponse(fd, 500, "{\"error\":\"disk write failed\"}");
      setState(LsUploadRequest::FAILED);
      return;
    }
    totalRead += initialBody.size();
    pumpProgress(true);
  }

  // Đọc cho tới khi đủ expectedLen (nếu biết) rồi DỪNG — không chờ EOF.
  // Fix kẹt "processing" mãi: trước đây recv tới close trong khi sender
  // giữ connection chờ response → 2 bên chờ nhau.
  while (expectedLen == 0 || totalRead < expectedLen) {
    size_t want = sizeof(buf);
    if (expectedLen > 0) {
      uint64_t remain = expectedLen - totalRead;
      if (remain < want)
        want = (size_t)remain;
    }
    ssize_t n = recv(fd, buf, want, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        Logger::warn("LocalSend: recv timeout for " + wantName +
                     " got=" + std::to_string(totalRead) +
                     " expected=" + std::to_string(expectedLen));
      } else {
        Logger::warn("LocalSend: recv error: " +
                     std::string(std::strerror(errno)));
      }
      break;
    }
    if (n == 0)
      break;
    out.write(buf, n);
    if (!out) {
      sendJsonResponse(fd, 500, "{\"error\":\"disk write failed\"}");
      setState(LsUploadRequest::FAILED);
      return;
    }
    totalRead += (uint64_t)n;
    pumpProgress(false);
  }
  out.close();

  // Thiếu bytes (sender ngắt giữa chừng / timeout) -> fail rõ ràng
  if (expectedLen > 0 && totalRead < expectedLen) {
    Logger::warn("LocalSend: incomplete upload " + wantName +
                 " got=" + std::to_string(totalRead) +
                 " expected=" + std::to_string(expectedLen));
    ::unlink(savedPath.c_str());
    setState(LsUploadRequest::FAILED);
    LsUploadRequest snap;
    {
      std::lock_guard<std::mutex> lock(m_pendingMutex);
      snap = m_pending[(size_t)reqIdx];
    }
    std::lock_guard<std::mutex> lock(m_cbMutex);
    if (m_onComplete)
      m_onComplete(snap);
    sendJsonResponse(fd, 400, "{\"error\":\"incomplete upload\"}");
    return;
  }
  // Thừa bytes (không nên xảy ra) -> cắt log để dễ debug
  if (expectedLen > 0 && totalRead > expectedLen) {
    Logger::warn("LocalSend: oversize upload " + wantName +
                 " got=" + std::to_string(totalRead) +
                 " expected=" + std::to_string(expectedLen));
  }
  pumpProgress(true);

  if (!wantSha.empty()) {
    std::string actual = LsUtil::sha256OfFile(savedPath);
    if (actual != wantSha) {
      Logger::warn("LocalSend: SHA256 mismatch for " + wantName);
      ::unlink(savedPath.c_str());
      setState(LsUploadRequest::FAILED);
      LsUploadRequest snap;
      {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        snap = m_pending[(size_t)reqIdx];
      }
      std::lock_guard<std::mutex> lock(m_cbMutex);
      if (m_onComplete)
        m_onComplete(snap);
      sendJsonResponse(fd, 422, "{\"error\":\"sha256 mismatch\"}");
      return;
    }
  }

  setState(LsUploadRequest::DONE);
  Logger::info("LocalSend: received " + wantName + " (" +
               std::to_string(totalRead) + " bytes) -> " + savedPath);

  LsUploadRequest snapshot;
  {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    snapshot = m_pending[(size_t)reqIdx];
  }
  {
    std::lock_guard<std::mutex> lock(m_cbMutex);
    if (m_onComplete)
      m_onComplete(snapshot);
  }

  std::thread([this, snapshot]() { postProcessUpload(snapshot); }).detach();

  sendJsonResponse(fd, 200, "{\"success\":true}");
}

void LocalSendManager::handleCancel(int fd, const std::string &query) {
  std::map<std::string, std::string> qparams;
  size_t pos = 0;
  while (pos < query.size()) {
    auto amp = query.find('&', pos);
    std::string pair = query.substr(
        pos, amp == std::string::npos ? std::string::npos : amp - pos);
    auto eq = pair.find('=');
    if (eq != std::string::npos)
      qparams[pair.substr(0, eq)] = pair.substr(eq + 1);
    if (amp == std::string::npos)
      break;
    pos = amp + 1;
  }
  std::string sessionId = qparams["sessionId"];
  std::lock_guard<std::mutex> lock(m_pendingMutex);
  for (auto &r : m_pending) {
    if (r.sessionId == sessionId) {
      r.state = LsUploadRequest::FAILED;
      if (!r.savedPath.empty())
        ::unlink(r.savedPath.c_str());
      break;
    }
  }
  sendJsonResponse(fd, 200, "{\"cancelled\":true}");
}

// =============================================================
// approve/reject từ UI
// =============================================================
void LocalSendManager::approveUpload(const std::string &sessionId) {
  std::lock_guard<std::mutex> lock(m_pendingMutex);
  for (auto &r : m_pending) {
    if (r.sessionId == sessionId && r.state == LsUploadRequest::PENDING) {
      r.state = LsUploadRequest::APPROVED;
      Logger::info("LocalSend: approved " + r.file.fileName);
      return;
    }
  }
}

void LocalSendManager::rejectUpload(const std::string &sessionId) {
  std::lock_guard<std::mutex> lock(m_pendingMutex);
  for (auto &r : m_pending) {
    if (r.sessionId == sessionId && r.state == LsUploadRequest::PENDING) {
      r.state = LsUploadRequest::REJECTED;
      Logger::info("LocalSend: rejected " + r.file.fileName);
      return;
    }
  }
}

// =============================================================
// Callback setters
// =============================================================
void LocalSendManager::setOnUserPrompt(
    std::function<void(const LsUploadRequest &)> cb) {
  std::lock_guard<std::mutex> lock(m_cbMutex);
  m_onUserPrompt = cb;
}
void LocalSendManager::setOnProgress(
    std::function<void(const LsUploadRequest &)> cb) {
  std::lock_guard<std::mutex> lock(m_cbMutex);
  m_onProgress = cb;
}
void LocalSendManager::setOnComplete(
    std::function<void(const LsUploadRequest &)> cb) {
  std::lock_guard<std::mutex> lock(m_cbMutex);
  m_onComplete = cb;
}

// =============================================================
// Target folder picker
// =============================================================
void LocalSendManager::setTargetFolder(const std::string &folder) {
  std::lock_guard<std::mutex> lock(m_targetMutex);
  m_targetFolder = folder;
  if (!m_targetFolder.empty() && m_targetFolder.back() != '/')
    m_targetFolder += '/';
}

std::string LocalSendManager::currentTargetFolder() const {
  std::lock_guard<std::mutex> lock(m_targetMutex);
  return m_targetFolder;
}

std::string
LocalSendManager::setPendingTargetFolder(const std::string &sessionId,
                                         const std::string &folderRel) {
  LsFileMeta fm;
  bool found = false, alreadyDecided = false;
  std::string currentSaved;
  {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    for (auto &r : m_pending) {
      if (r.sessionId != sessionId)
        continue;
      found = true;
      if (r.state != LsUploadRequest::PENDING) {
        alreadyDecided = true;
        currentSaved = r.savedPath;
      } else {
        fm = r.file;
      }
      break;
    }
  }
  if (!found)
    return "";
  if (alreadyDecided)
    return currentSaved;
  std::string base = LsUtil::basenameOf(fm.fileName);
  if (base.empty())
    base = fm.id;
  base = LsUtil::basenameOf(base);
  if (base.empty() || base == "." || base == "..")
    return "";
  std::string target;
  if (folderRel.empty()) {
    target = resolveTargetPath(fm);
  } else {
    std::string s = folderRel;
    const std::string root = "/mnt/SDCARD";
    if (s.compare(0, root.size(), root) == 0)
      s = s.substr(root.size());
    std::string out, seg;
    auto flush = [&]() {
      if (seg.empty() || seg == ".") {
        seg.clear();
        return;
      }
      if (seg == "..") {
        seg.clear();
        return;
      }
      if (!out.empty() && out.back() != '/')
        out += '/';
      out += seg;
      seg.clear();
    };
    for (size_t i = 0; i <= s.size(); ++i) {
      char c = (i < s.size()) ? s[i] : '/';
      if (c == '/' || c == '\\')
        flush();
      else if (c >= 32)
        seg += c;
    }
    if (!out.empty() && out.back() != '/')
      out += '/';
    if (!out.empty())
      target = "/mnt/SDCARD/" + out + base;
  }
  if (target.empty() || !LsUtil::isPathSafe(target))
    return "";
  {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    for (auto &r : m_pending) {
      if (r.sessionId == sessionId && r.state == LsUploadRequest::PENDING) {
        r.savedPath = target;
        return target;
      }
    }
  }
  return "";
}

// =============================================================
// Path Resolution: 3-tier
//   1. Pending target folder (user chọn trong UI)
//   2. relativePath từ sender (RomCloud-to-RomCloud)
//   3. fileName có '/' (encode)
//   4. Heuristic extension mapping (fallback cho Flutter client)
// =============================================================
std::string LocalSendManager::resolveTargetPath(const LsFileMeta &file) {
  std::string base = LsUtil::basenameOf(file.fileName);
  if (base.empty())
    base = file.id;
  // Base không được chứa '/' hay '..' (chỉ tên file thuần)
  base = LsUtil::basenameOf(base);
  if (base.empty() || base == "." || base == "..")
    return "";

  // Chuẩn hoá 1 đoạn path tương đối: bỏ prefix /mnt/SDCARD, collapse '//',
  // loại bỏ '.' / '..', trim '/' đầu, giữ '/' cuối cho dir.
  auto normalizeRel = [](std::string s, bool isDir) -> std::string {
    const std::string root = "/mnt/SDCARD";
    if (s.compare(0, root.size(), root) == 0)
      s = s.substr(root.size());
    std::string out;
    std::string seg;
    auto flushSeg = [&]() {
      if (seg.empty() || seg == ".") {
        seg.clear();
        return;
      }
      if (seg == "..") {
        seg.clear();
        return;
      } // bỏ traversal
      if (!out.empty() && out.back() != '/')
        out += '/';
      out += seg;
      seg.clear();
    };
    for (size_t i = 0; i <= s.size(); ++i) {
      char c = (i < s.size()) ? s[i] : '/';
      if (c == '/' || c == '\\')
        flushSeg();
      else if (c >= 32)
        seg += c;
    }
    if (isDir && !out.empty() && out.back() != '/')
      out += '/';
    return out;
  };

  std::string target;

  // Tier 1: user-chosen target folder (highest priority)
  {
    std::lock_guard<std::mutex> lock(m_targetMutex);
    if (!m_targetFolder.empty()) {
      std::string dir = normalizeRel(m_targetFolder, true);
      if (!dir.empty())
        target = "/mnt/SDCARD/" + dir + base;
    }
  }

  // Tier 2: relativePath từ sender (RomCloud sender) — giữ đúng path gửi tới
  if (target.empty() && !file.relativePath.empty()) {
    std::string dir = normalizeRel(file.relativePath, true);
    if (!dir.empty())
      target = "/mnt/SDCARD/" + dir + base;
    else
      target = "/mnt/SDCARD/Inbox/" + base;
  }

  // Tier 3: fileName có '/' (sender encode path vào fileName)
  if (target.empty() && file.fileName.find('/') != std::string::npos) {
    std::string rel = normalizeRel(file.fileName, false);
    if (!rel.empty())
      target = "/mnt/SDCARD/" + rel;
  }

  // Tier 4: heuristic mapping
  if (target.empty()) {
    target = heuristicMap(file.fileName);
  }

  if (target.empty())
    return "";

  // Path traversal guard
  if (!LsUtil::isPathSafe(target)) {
    Logger::warn("LocalSend: reject unsafe path: " + target);
    return "";
  }

  return target;
}

// =============================================================
// Heuristic mapping extension -> folder (fallback)
// =============================================================
std::string LocalSendManager::heuristicMap(const std::string &fileName) {
  std::string base = LsUtil::basenameOf(fileName);
  if (base.empty())
    base = fileName;
  std::string ext = LsUtil::lowerExt(fileName);

  struct Rule {
    std::initializer_list<const char *> exts;
    const char *dir;
    bool extract;
  };

  static const std::vector<Rule> rules = {
      // GBA family
      {{".gba", ".gbc", ".gb"}, "Roms/GBA/", false},
      // NES / Famicom
      {{".nes", ".fds", ".unf", ".nsf"}, "Roms/NES/", false},
      // SNES
      {{".smc", ".sfc", ".fig", ".swc", ".bs"}, "Roms/SNES/", false},
      // N64
      {{".n64", ".z64", ".v64", ".u64"}, "Roms/N64/", false},
      // Genesis / MegaDrive
      {{".md", ".gen", ".smd", ".bin"}, "Roms/MD/", false},
      // PS1
      {{".cue", ".iso", ".pbp", ".chd", ".ps1", ".m3u"}, "Roms/PS1/", false},
      // NDS
      {{".nds"}, "Roms/NDS/", false},
      // Misc retro
      {{".pce", ".ngp", ".ngpc", ".ws", ".wsc", ".lynx", ".a26", ".a78", ".col",
        ".sg", ".cv", ".int"},
       "Roms/Misc/",
       false},
      // Arcade
      {{".zip", ".fba"}, "Roms/Arcade/", false},
      // Saves / states
      {{".srm", ".sav", ".state", ".st"}, "Roms/Saves/", false},
      // RetroArch config / overlays / cheats
      {{".cfg", ".cht", ".opt", ".lpl", ".rpx"}, "RetroArch/", false},
      // BIOS
      {{".bin"}, "RetroArch/system/", false},
      // Archives (zip cần extract)
      {{".zip"}, "Inbox/", true},
      // Media
      {{".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp"}, "Inbox/", false},
      {{".mp4", ".mkv", ".avi", ".mov", ".webm"}, "Imgs/", false},
      {{".mp3", ".ogg", ".wav", ".flac", ".opus"}, "Imgs/", false},
      {{".txt", ".md", ".pdf", ".epub"}, "Books/", false},
      {{".json", ".xml", ".yml", ".yaml", ".conf"}, "RetroArch/", false},
      // Default
      {{}, "Inbox/", false},
  };

  for (const auto &rule : rules) {
    if (rule.exts.size() == 0) { // default
      return std::string("/mnt/SDCARD/") + rule.dir + base;
    }
    for (auto e : rule.exts) {
      if (ext == e)
        return std::string("/mnt/SDCARD/") + rule.dir + base;
    }
  }
  return "/mnt/SDCARD/Inbox/" + base;
}

// =============================================================
// Post-process: extract zip + organize + rescan
// Chạy detached thread (không block HTTP response).
// =============================================================
void LocalSendManager::postProcessUpload(const LsUploadRequest &req) {
  if (req.state != LsUploadRequest::DONE)
    return;
  if (req.savedPath.empty())
    return;

  Logger::info("LocalSend: postProcess " + req.savedPath);

  std::string ext = LsUtil::lowerExt(req.savedPath);

  // 1. Extract .zip vào Inbox/ (RomOrganizer sẽ lo tiếp)
  if (ext == ".zip" && req.savedPath.find("/Inbox/") != std::string::npos) {
    std::string unzipCmd = "unzip -o -qq '" + req.savedPath + "' -d '" +
                           LsUtil::dirnameOf(req.savedPath) + "'";
    Logger::info("LocalSend: extract -> " + unzipCmd);
    int rc = std::system(unzipCmd.c_str());
    if (rc == 0) {
      ::unlink(req.savedPath.c_str()); // xóa zip sau khi extract
      Logger::info("LocalSend: extract OK, removed " + req.savedPath);
    } else {
      Logger::warn("LocalSend: extract failed rc=" + std::to_string(rc));
    }
  }

  // 2. Trigger RomOrganizer (extension mapping → Roms/<sys>/)
  try {
    RomOrganizer::instance().organizeDirectory(
        RomOrganizer::instance().getInboxDir());
  } catch (const std::exception &e) {
    Logger::warn(std::string("LocalSend: organize failed: ") + e.what());
  }

  // 3. Refresh RomIndexer (cho UI game list)
  try {
    RomIndexer::instance().scanAllSystems(AppConfig::instance().getRomsDir(),
                                          nullptr);
    UIManager::instance().setNeedLibraryRefresh(true);
  } catch (...) {
  }

  // 4. Mở rộng RomCloud: nếu sender gửi kèm game metadata (gameId > 0),
  // upsert game vào DB local với title/system/cover đầy đủ để UI hiển thị ngay.
  if (req.file.gameId > 0) {
    try {
      GameRecord g;
      g.id = req.file.gameId;
      g.systemId = req.file.systemId;
      g.title =
          req.file.gameTitle.empty() ? req.file.fileName : req.file.gameTitle;
      g.filename = req.file.fileName;
      g.localPath = req.savedPath;
      g.coverPath = req.file.coverPath;
      g.localState = GameState::LOCAL;
      struct stat st{};
      if (::stat(req.savedPath.c_str(), &st) == 0)
        g.sizeBytes = (uint64_t)st.st_size;
      // cloudFileId giữ nguyên (nếu sender gửi kèm) — nhưng protocol chưa
      // truyền, nên để rỗng → game này sẽ là LOCAL-only.

      int64_t outId = 0;
      if (DatabaseManager::instance().upsertGame(g, &outId)) {
        if (outId > 0 && outId != req.file.gameId) {
          // Sender dùng gameId riêng → DB local có id khác, OK.
          Logger::info("LocalSend: upsertGame '" + g.title +
                       "' id=" + std::to_string(outId) +
                       " (sender id=" + std::to_string(req.file.gameId) + ")");
        }
        UIManager::instance().setNeedLibraryRefresh(true);
      } else {
        Logger::warn("LocalSend: upsertGame failed for '" + g.title + "'");
      }
    } catch (const std::exception &e) {
      Logger::warn(std::string("LocalSend: game upsert failed: ") + e.what());
    }
  }

  Logger::info("LocalSend: postProcess DONE");
}

// =============================================================
// HTTP Client (sender side)
// =============================================================
static int connectTcp(const std::string &ip, int port, int timeoutSec = 10) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
    close(fd);
    return -1;
  }

  // Non-blocking connect with timeout
#ifndef _WIN32
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#else
  u_long nonblock = 1;
  ioctlsocket(fd, FIONBIO, &nonblock);
#endif

  int rc = connect(fd, (sockaddr *)&addr, sizeof(addr));
  if (rc < 0 && errno != EINPROGRESS) {
    close(fd);
    return -1;
  }

  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(fd, &wfds);
  timeval tv{timeoutSec, 0};
  rc = select(fd + 1, nullptr, &wfds, nullptr, &tv);
  if (rc <= 0) {
    close(fd);
    return -1;
  }

  // Restore blocking
#ifndef _WIN32
  fcntl(fd, F_SETFL, flags);
#else
  nonblock = 0;
  ioctlsocket(fd, FIONBIO, &nonblock);
#endif
  return fd;
}

bool LocalSendManager::httpPostJson(const std::string &ip, int port,
                                    const std::string &path,
                                    const std::string &jsonBody,
                                    std::string &respBody, int timeoutSec) {
  int fd = connectTcp(ip, port, timeoutSec);
  if (fd < 0)
    return false;

  std::ostringstream req;
  req << "POST " << path << " HTTP/1.1\r\n";
  req << "Host: " << ip << "\r\n";
  req << "Content-Type: application/json\r\n";
  req << "Content-Length: " << jsonBody.size() << "\r\n";
  req << "Connection: close\r\n\r\n";
  req << jsonBody;

  std::string s = req.str();
  ::send(fd, s.data(), s.size(), 0);

  char buf[4096];
  std::string resp;
  while (true) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0)
      break;
    resp.append(buf, n);
  }
  close(fd);

  auto bodyPos = resp.find("\r\n\r\n");
  if (bodyPos == std::string::npos)
    return false;
  // Spec §4.1: receiver có thể trả 400/401/403/500 → phải fail, không coi như
  // OK. Trích status code từ response line "HTTP/1.1 <code> ...".
  int statusCode = 0;
  {
    auto sp1 = resp.find(' ');
    if (sp1 != std::string::npos)
      statusCode = std::atoi(resp.c_str() + sp1 + 1);
  }
  respBody = resp.substr(bodyPos + 4);
  if (statusCode != 200) {
    Logger::warn("LocalSend: POST " + path + " -> HTTP " +
                 std::to_string(statusCode) +
                 " body=" + respBody.substr(0, 200));
    return false;
  }
  return true;
}

bool LocalSendManager::httpPostBinary(const std::string &ip, int port,
                                      const std::string &path,
                                      const std::string &filePath,
                                      LsSendProgress &prog) {
  int fd = connectTcp(ip, port, 30);
  if (fd < 0)
    return false;

  std::ifstream in(filePath, std::ios::binary);
  if (!in) {
    close(fd);
    return false;
  }

  // Headers first
  std::ostringstream h;
  h << "POST " << path << " HTTP/1.1\r\n";
  h << "Host: " << ip << "\r\n";
  h << "Content-Type: application/octet-stream\r\n";
  h << "Content-Length: " << prog.totalBytes << "\r\n";
  h << "Connection: close\r\n\r\n";
  std::string hs = h.str();
  ::send(fd, hs.data(), hs.size(), 0);

  // Stream body
  char buf[LocalSendProto::kUploadChunkSize];
  uint64_t sent = 0;
  int64_t lastTick = LsUtil::nowMs();
  uint64_t lastTickBytes = 0;
  while (in) {
    in.read(buf, sizeof(buf));
    ssize_t got = in.gcount();
    if (got <= 0)
      break;
    ssize_t s = send(fd, buf, got, MSG_NOSIGNAL);
    if (s < 0) {
      close(fd);
      return false;
    }
    sent += s;
    prog.sentBytes = sent;

    int64_t now = LsUtil::nowMs();
    if (now - lastTick >= 500) {
      uint32_t bps =
          (uint32_t)((sent - lastTickBytes) * 1000 / (now - lastTick));
      prog.bytesPerSec = bps;
      lastTick = now;
      lastTickBytes = sent;
    }
  }
  // Read response — spec §4.2: receiver trả 200 + No body (Flutter),
  // RomCloud receiver trả 200 {"success":true}. Chấp nhận mọi 2xx.
  char rbuf[4096];
  std::string resp;
  while (true) {
    ssize_t n = recv(fd, rbuf, sizeof(rbuf), 0);
    if (n <= 0)
      break;
    resp.append(rbuf, n);
  }
  close(fd);
  auto eol = resp.find("\r\n");
  std::string statusLine =
      (eol == std::string::npos) ? resp : resp.substr(0, eol);
  int code = 0;
  {
    auto sp = statusLine.find(' ');
    if (sp != std::string::npos)
      code = std::atoi(statusLine.c_str() + sp + 1);
  }
  if (code == 200 || code == 201 || code == 204)
    return true;
  // Fallback: bản cũ / receiver lạ không có status line chuẩn
  if (code == 0 && resp.find("\"success\":true") != std::string::npos)
    return true;
  return false;
}

// =============================================================
// Sender API - gửi file sang TrimUI khác (RomCloud-to-RomCloud)
// =============================================================
std::string LocalSendManager::sendFile(const std::string &absPath,
                                       const LsDeviceInfo &target) {
  LsFileMeta meta;
  meta.fileName = LsUtil::basenameOf(absPath);
  meta.size = 0; // sẽ fill lại bên trong
  return sendFileMeta(meta, absPath, target);
}

std::string LocalSendManager::sendFileMeta(const LsFileMeta &inMeta,
                                           const std::string &absPath,
                                           const LsDeviceInfo &target) {
  if (!m_running)
    return "";

  // Validate path
  if (absPath.empty() || absPath[0] != '/') {
    Logger::warn("LocalSend: sendFileMeta path not absolute: " + absPath);
    return "";
  }
  if (!LsUtil::isPathSafe(absPath)) {
    Logger::warn("LocalSend: sendFileMeta unsafe path: " + absPath);
    return "";
  }

  // Get file size
  struct stat st{};
  if (stat(absPath.c_str(), &st) != 0) {
    Logger::warn("LocalSend: sendFileMeta stat failed: " + absPath);
    return "";
  }

  LsFileMeta meta = inMeta; // copy để có thể fill size
  meta.fileName = LsUtil::basenameOf(absPath);
  meta.size = (uint64_t)st.st_size;

  auto prog = std::make_shared<LsSendProgress>();
  prog->sessionId = LsUtil::makeUuid();
  prog->toAlias = target.alias;
  prog->toIp = target.ip;
  prog->fileName = meta.fileName;
  prog->absPath = absPath;
  prog->totalBytes = meta.size;
  prog->state = LsSendProgress::NEGOTIATING;

  {
    std::lock_guard<std::mutex> lock(m_sendMutex);
    if (m_sends.size() >= 16)
      m_sends.erase(m_sends.begin());
    m_sends.push_back(prog);
  }

  // Build relativePath từ /mnt/SDCARD/ (chỉ khi meta chưa có)
  if (meta.relativePath.empty()) {
    const std::string prefix = "/mnt/SDCARD/";
    if (absPath.compare(0, prefix.size(), prefix) == 0) {
      std::string sub = absPath.substr(prefix.size());
      auto slash = sub.find_last_of('/');
      meta.relativePath =
          (slash == std::string::npos) ? "" : sub.substr(0, slash + 1);
    }
  }

  // Build fileId
  std::string fileId = LsUtil::makeUuid();
  meta.id = fileId;

  // Compute SHA256 của file (sender cung cấp để receiver verify)
  std::string sha = LsUtil::sha256OfFile(absPath);
  if (!sha.empty())
    meta.sha256 = sha;
  if (meta.fileType.empty())
    meta.fileType = "application/octet-stream";

  // Build prepare-upload JSON
  std::ostringstream prep;
  prep << "{";
  prep << "\"info\":{";
  prep << "\"alias\":\"" << LsJson::escape(m_selfAlias) << "\",";
  prep << "\"version\":\"" << LocalSendProto::kVersion << "\",";
  prep << "\"deviceModel\":\"TrimUI Brick Pro\",";
  prep << "\"deviceType\":\"" << LocalSendProto::kDeviceType << "\",";
  prep << "\"fingerprint\":\"" << m_selfFingerprint << "\",";
  prep << "\"port\":" << LocalSendProto::kPort << ",";
  prep << "\"protocol\":\"" << LocalSendProto::kProtocol << "\",";
  prep << "\"download\":false";
  prep << "},";
  prep << "\"files\":{";
  prep << "\"" << fileId << "\":{";
  prep << "\"id\":\"" << fileId << "\",";
  prep << "\"fileName\":\"" << LsJson::escape(meta.fileName) << "\",";
  if (!meta.relativePath.empty()) {
    prep << "\"relativePath\":\"" << LsJson::escape(meta.relativePath) << "\",";
  }
  prep << "\"size\":" << meta.size << ",";
  prep << "\"fileType\":\"" << LsJson::escape(meta.fileType) << "\",";
  if (!meta.sha256.empty())
    prep << "\"sha256\":\"" << LsJson::escape(meta.sha256) << "\",";

  // Mở rộng RomCloud: nếu là game trong DB, truyền kèm metadata để receiver
  // render dialog duyệt với cover art + tên game đẹp.
  if (meta.gameId > 0) {
    prep << "\"gameId\":" << meta.gameId << ",";
    prep << "\"systemId\":" << meta.systemId << ",";
    if (!meta.systemCode.empty())
      prep << "\"systemCode\":\"" << LsJson::escape(meta.systemCode) << "\",";
    if (!meta.systemName.empty())
      prep << "\"systemName\":\"" << LsJson::escape(meta.systemName) << "\",";
    if (!meta.gameTitle.empty())
      prep << "\"gameTitle\":\"" << LsJson::escape(meta.gameTitle) << "\",";
    if (!meta.coverPath.empty())
      prep << "\"coverPath\":\"" << LsJson::escape(meta.coverPath) << "\",";
  }

  // Cắt dấu phẩy cuối cùng nếu có
  std::string body = prep.str();
  if (body.size() > 1 && body.back() == ',')
    body.pop_back();
  body += "}}}";

  // POST prepare-upload
  std::string respBody;
  if (!httpPostJson(target.ip, target.port, "/api/localsend/v2/prepare-upload",
                    body, respBody)) {
    prog->state = LsSendProgress::FAILED;
    prog->errorMessage = "prepare-upload network error";
    Logger::warn("LocalSend: prepare-upload failed to " + target.alias);
    return prog->sessionId;
  }

  // Parse response để lấy sessionId + token (§4.1: files = { fileId: token })
  std::string sessionId;
  LsJson::getString(respBody, "sessionId", sessionId);
  if (sessionId.empty()) {
    prog->state = LsSendProgress::FAILED;
    prog->errorMessage = "receiver rejected";
    Logger::warn("LocalSend: receiver rejected " + prog->fileName);
    return prog->sessionId;
  }

  // Token: parse object "files" rồi lấy files[fileId] (string token).
  // Tương thích ngược: receiver cũ có thể trả files[fileId] = {...} object
  // → lúc đó token rỗng, upload không kèm token (server vẫn chấp nhận).
  std::string fileToken;
  {
    std::string filesObj;
    if (LsJson::getObject(respBody, "files", filesObj)) {
      LsJson::getString(filesObj, fileId, fileToken);
    }
  }

  // POST file binary (spec §4.2: query sessionId + fileId + token)
  prog->state = LsSendProgress::UPLOADING;
  std::string path =
      "/api/localsend/v2/upload?sessionId=" + sessionId + "&fileId=" + fileId;
  if (!fileToken.empty())
    path += "&token=" + fileToken;
  if (!httpPostBinary(target.ip, target.port, path, absPath, *prog)) {
    prog->state = LsSendProgress::FAILED;
    prog->errorMessage = "upload failed";
    Logger::warn("LocalSend: upload failed: " + prog->fileName);
    return prog->sessionId;
  }

  prog->state = LsSendProgress::DONE;
  Logger::info("LocalSend: sent " + prog->fileName + " (" +
               LsUtil::humanSize(prog->totalBytes) + ") to " + target.alias);
  return prog->sessionId;
}

std::string LocalSendManager::sendFileMetaAsync(const LsFileMeta &inMeta,
                                                const std::string &absPath,
                                                const LsDeviceInfo &target) {
  if (!m_running)
    return "";
  if (absPath.empty() || absPath[0] != '/' || !LsUtil::isPathSafe(absPath))
    return "";
  LsFileMeta metaCopy = inMeta;
  std::string token = "queued-" + LsUtil::makeUuid();
  // Chay nen: sendFileMeta (sync) se tu tao LsSendProgress that trong m_sends.
  // Tra ve ngay de UI chuyen sang PROGRESS va poll
  // sendProgresses()/receiveProgresses().
  std::thread([this, metaCopy, absPath, target]() {
    this->sendFileMeta(metaCopy, absPath, target);
  }).detach();
  return token;
}

std::vector<LsSendProgress> LocalSendManager::sendProgresses() {
  std::vector<LsSendProgress> out;
  std::lock_guard<std::mutex> lock(m_sendMutex);
  out.reserve(m_sends.size());
  for (auto &p : m_sends) {
    LsSendProgress copy;
    copy.sessionId = p->sessionId;
    copy.toAlias = p->toAlias;
    copy.toIp = p->toIp;
    copy.fileName = p->fileName;
    copy.absPath = p->absPath;
    copy.totalBytes = p->totalBytes;
    copy.sentBytes = p->sentBytes;
    copy.bytesPerSec = p->bytesPerSec;
    copy.state = p->state;
    copy.errorMessage = p->errorMessage;
    out.push_back(std::move(copy));
  }
  return out;
}

std::vector<LsUploadRequest> LocalSendManager::receiveProgresses() {
  std::vector<LsUploadRequest> out;
  std::lock_guard<std::mutex> lock(m_pendingMutex);
  out.reserve(m_pending.size());
  for (auto &r : m_pending)
    out.push_back(r);
  return out;
}

} // namespace RomCloud
