#include "WebServer.h"
#include "../app/Application.h"
#include "../auth/AuthManager.h"
#include "../calendar/CalManager.h"
#include "../clock/ClockStore.h"
#include "../config/AppConfig.h"
#include "../database/DatabaseManager.h"
#include "../database/RomIndexer.h"
#include "../download/DownloadManager.h"
#include "../filesystem/FileSystemManager.h"
#include "../iptv/IPTVManager.h"
#include "../logging/Logger.h"
#include "../ota/UpdateManager.h"
#include "../platform/PlatformInfo.h"
#include "../rom/RomDetector.h"
#include "../rom/RomOrganizer.h"
#include "../sync/DriveSyncEngine.h"
#include "../sync/UploadManager.h"
#include "../ui/BoxartScraper.h"
#include "HttpClient.h"
#include "../market/WatchManager.h"
#include "../sync/OneDriveSync.h"


#include <algorithm>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <set>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>


namespace RomCloud {

static std::string urlDecode(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%') {
      if (i + 2 < in.size()) {
        int hexVal = 0;
        std::istringstream hexStream(in.substr(i + 1, 2));
        if (hexStream >> std::hex >> hexVal) {
          out += static_cast<char>(hexVal);
          i += 2;
        } else {
          out += in[i];
        }
      } else {
        out += in[i];
      }
    } else if (in[i] == '+') {
      out += ' ';
    } else {
      out += in[i];
    }
  }
  return out;
}

static std::string extractFolderId(const std::string &url) {
  size_t fPos = url.find("folders/");
  if (fPos != std::string::npos) {
    std::string id = url.substr(fPos + 8);
    size_t endPos = id.find_first_of("?/#& ");
    if (endPos != std::string::npos)
      id = id.substr(0, endPos);
    return id;
  }
  size_t idPos = url.find("id=");
  if (idPos != std::string::npos) {
    std::string id = url.substr(idPos + 3);
    size_t endPos = id.find_first_of("?/#& ");
    if (endPos != std::string::npos)
      id = id.substr(0, endPos);
    return id;
  }
  if (url.find('/') == std::string::npos && url.length() >= 20) {
    return url;
  }
  return "";
}

static std::string extractPostParam(const std::string &postBody,
                                    const std::string &paramName) {
  std::string key = paramName + "=";
  size_t keyPos = 0;
  while (true) {
    keyPos = postBody.find(key, keyPos);
    if (keyPos == std::string::npos)
      return "";
    if (keyPos == 0 || postBody[keyPos - 1] == '&')
      break;
    keyPos += key.length();
  }
  std::string rawVal = postBody.substr(keyPos + key.length());
  size_t ampersand = rawVal.find('&');
  if (ampersand != std::string::npos)
    rawVal = rawVal.substr(0, ampersand);
  return urlDecode(rawVal);
}

static std::string extractQueryParam(const std::string &queryStr,
                                     const std::string &paramName) {
  std::string key = paramName + "=";
  size_t keyPos = 0;
  while (true) {
    keyPos = queryStr.find(key, keyPos);
    if (keyPos == std::string::npos)
      return "";
    if (keyPos == 0 || queryStr[keyPos - 1] == '&' ||
        queryStr[keyPos - 1] == '?')
      break;
    keyPos += key.length();
  }
  std::string rawVal = queryStr.substr(keyPos + key.length());
  size_t ampersand = rawVal.find('&');
  if (ampersand != std::string::npos)
    rawVal = rawVal.substr(0, ampersand);
  return urlDecode(rawVal);
}

static std::string escapeJson(const std::string &in) {
  std::string out;
  out.reserve(in.size() + 10);
  for (char c : in) {
    if (c == '"')
      out += "\\\"";
    else if (c == '\\')
      out += "\\\\";
    else if (c == '\b')
      out += "\\b";
    else if (c == '\f')
      out += "\\f";
    else if (c == '\n')
      out += "\\n";
    else if (c == '\r')
      out += "\\r";
    else if (c == '\t')
      out += "\\t";
    else if (static_cast<unsigned char>(c) < 32) {
      // drop non-printable
    } else {
      out += c;
    }
  }
  return out;
}

WebServer &WebServer::instance() {
  static WebServer instance;
  return instance;
}

WebServer::~WebServer() { stop(); }

bool WebServer::start(int port) {
  if (m_running)
    return true;

  // Safe portal port pool: 8888, 8889, 8890, 8081, 8082, 8085, 9000, 9999.
  // NEVER use 8080 (held by stock SFTPGo) or 8090 (held by GameCast daemon).
  const int candidates[] = {port, 8888, 8889, 8890, 8081, 8082, 8085, 9000, 9999};
  int tried[12] = {0};
  for (int cand : candidates) {
    if (cand <= 0 || cand > 65535 || cand == 8080 || cand == 8090)
      continue;
    bool dup = false;
    for (int t : tried) {
      if (t == cand) { dup = true; break; }
    }
    if (dup)
      continue;
    for (int &t : tried) {
      if (t == 0) { t = cand; break; }
    }

    m_serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverFd < 0) {
      Logger::error("WebServer: Failed to create socket.");
      return false;
    }

    int opt = 1;
    setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(cand);

    if (bind(m_serverFd, (struct sockaddr *)&address, sizeof(address)) < 0) {
      Logger::error("WebServer: Port " + std::to_string(cand) +
                    " busy; trying next safe port...");
      close(m_serverFd);
      m_serverFd = -1;
      continue;
    }

    if (listen(m_serverFd, 10) < 0) {
      Logger::error("WebServer: Listen failed on port " +
                    std::to_string(cand));
      close(m_serverFd);
      m_serverFd = -1;
      continue;
    }

    m_port = cand;
    m_running = true;
    m_thread = std::thread(&WebServer::serverLoop, this);
    if (cand != port) {
      Logger::info("WebServer: Portal started on port " +
                   std::to_string(m_port) + " (fallback, requested " +
                   std::to_string(port) + " was busy)");
    } else {
      Logger::info("WebServer: Portal started on port " +
                   std::to_string(m_port));
    }
    return true;
  }

  Logger::error("WebServer: All candidate ports busy, portal disabled.");
  return false;
}

int WebServer::cycleNextPort() {
  static const int kPool[] = {8888, 8889, 8890, 8081, 8082, 8085, 9000, 9999};
  constexpr size_t kPoolSize = sizeof(kPool) / sizeof(kPool[0]);

  int curPort = m_port;
  int nextTarget = kPool[0];
  for (size_t i = 0; i < kPoolSize; i++) {
    if (kPool[i] == curPort) {
      nextTarget = kPool[(i + 1) % kPoolSize];
      break;
    }
  }

  stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  if (!start(nextTarget)) {
    Logger::error("WebServer: Failed to cycle to port " + std::to_string(nextTarget));
  }
  return m_port;
}

std::string WebServer::getPortalUrl() const {
  if (!m_running)
    return "";
  std::string ip = PlatformInfo::instance().getIpAddress("wlan0");
  if (ip.empty() || ip == "Disconnected")
    ip = "192.168.1.164";
  return "http://" + ip + ":" + std::to_string(m_port);
}

void WebServer::stop() {
  if (!m_running)
    return;
  m_running = false;

  if (m_serverFd >= 0) {
    shutdown(m_serverFd, SHUT_RDWR);
    close(m_serverFd);
    m_serverFd = -1;
  }

  if (m_thread.joinable()) {
    m_thread.join();
  }
  Logger::info("WebServer: Stopped.");
}

void WebServer::serverLoop() {
  while (m_running) {
    struct sockaddr_in clientAddr;
    socklen_t clientLen = sizeof(clientAddr);
    int clientFd =
        accept(m_serverFd, (struct sockaddr *)&clientAddr, &clientLen);
    if (clientFd < 0) {
      if (m_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      continue;
    }

    struct timeval tv;
    tv.tv_sec = 4;
    tv.tv_usec = 0;
    setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv,
               sizeof(tv));
    setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv,
               sizeof(tv));

    handleClient(clientFd);
    close(clientFd);
  }
}

std::string WebServer::buildHtmlResponse() {
  std::string ip = PlatformInfo::instance().getIpAddress("wlan0");
  if (ip.empty())
    ip = "192.168.1.164";

  auto &db = DatabaseManager::instance();
  bool isLinked = AuthManager::instance().isLinked();
  std::string savedDriveUrl =
      isLinked ? db.getSetting("drive_folder_url", "") : "";
  std::string lastSyncTime = DriveSyncEngine::instance().getLastSyncTime();

  int totalLocal = 0, totalCloud = 0;
  if (isLinked) {
    db.getTotalGameCounts(totalLocal, totalCloud);
  }

  std::string html =
      R"HTML(<!DOCTYPE html>
<html lang="vi">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta http-equiv="Cache-Control" content="no-cache, no-store, must-revalidate">
  <meta http-equiv="Pragma" content="no-cache">
  <meta http-equiv="Expires" content="0">
  <title>RomCloud - TrimUI ROM Manager</title>
  <style>
    :root {
      --bg: #05070f;
      --card-bg: rgba(17, 25, 40, 0.55);
      --card-alt: rgba(30, 41, 59, 0.5);
      --border: rgba(148, 163, 184, 0.22);
      --border-hover: rgba(56, 189, 248, 0.55);
      --primary: #0284c7;
      --primary-hover: #0369a1;
      --accent: #38bdf8;
      --text: #f8fafc;
      --text-muted: #94a3b8;
      --text-dim: #64748b;
      --green: #10b981;
      --green-bg: #064e3b;
      --green-border: #059669;
      --red: #ef4444;
      --red-bg: #7f1d1d;
      --yellow: #f59e0b;
      --yellow-bg: #78350f;
      --purple: #8b5cf6;
      --radius: 16px;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Oxygen, Ubuntu, Cantarell, sans-serif;
      background:
        radial-gradient(900px 500px at 10% -5%, rgba(56,189,248,0.14), transparent 60%),
        radial-gradient(800px 500px at 95% 10%, rgba(139,92,246,0.12), transparent 60%),
        radial-gradient(700px 600px at 50% 110%, rgba(16,185,129,0.08), transparent 60%),
        var(--bg);
      background-attachment: fixed;
      color: var(--text);
      line-height: 1.5;
      padding: 16px;
      min-height: 100vh;
    }
    .container { max-width: 1600px; margin: 0 auto; }
    
    /* Header */
    header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      flex-wrap: wrap;
      gap: 12px;
      padding: 14px 20px;
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: var(--radius);
      margin-bottom: 20px;
    }
    .logo-area { display: flex; align-items: center; gap: 10px; }
    .logo-icon { font-size: 26px; }
    .logo-text { font-size: 22px; font-weight: 800; color: var(--accent); letter-spacing: -0.5px; }
    .device-badge {
      font-size: 11px;
      padding: 3px 8px;
      border-radius: 6px;
      background: #0284c722;
      color: var(--accent);
      border: 1px solid #0284c744;
      font-weight: 600;
    }
    .header-stats {
      display: flex;
      align-items: center;
      gap: 12px;
      flex-wrap: wrap;
    }
    .stat-pill {
      font-size: 12px;
      padding: 4px 10px;
      border-radius: 8px;
      background: var(--card-alt);
      border: 1px solid var(--border);
      color: var(--text-muted);
    }
    .stat-pill b { color: var(--text); }
    .stat-pill.active-download {
      background: #0284c722;
      border-color: var(--primary);
      color: var(--accent);
    }

    /* Tabs */
    .tab-bar {
      display: flex;
      gap: 8px;
      margin-bottom: 20px;
      border-bottom: 1px solid var(--border);
      padding-bottom: 8px;
      overflow-x: auto;
    }
    .tab-btn {
      padding: 10px 18px;
      background: transparent;
      border: 1px solid transparent;
      border-radius: 8px;
      color: var(--text-muted);
      font-weight: 600;
      font-size: 14px;
      cursor: pointer;
      display: flex;
      align-items: center;
      gap: 8px;
      white-space: nowrap;
      transition: all 0.15s ease;
    }
    .tab-btn:hover { color: var(--text); background: var(--card-alt); }
    .tab-btn.active {
      background: var(--primary);
      color: #fff;
      border-color: var(--primary-hover);
      box-shadow: 0 4px 12px rgba(2, 132, 199, 0.3);
    }
    .badge {
      display: inline-block;
      padding: 2px 7px;
      border-radius: 10px;
      font-size: 11px;
      font-weight: 700;
      background: rgba(255,255,255,0.2);
    }

    /* Tab Content */
    .tab-content { display: none; }
    .tab-content.active { display: block; animation: fadeIn 0.2s ease; }
    @keyframes fadeIn { from { opacity: 0; transform: translateY(4px); } to { opacity: 1; transform: translateY(0); } }

    /* Controls Bar */
    .controls-bar {
      display: flex;
      flex-direction: column;
      gap: 14px;
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: var(--radius);
      padding: 16px;
      margin-bottom: 20px;
    }
    .search-row {
      display: flex;
      gap: 10px;
      align-items: center;
    }
    .search-input-wrap {
      position: relative;
      flex: 1;
    }
    .search-input {
      width: 100%;
      background: var(--bg);
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 10px 36px 10px 14px;
      font-size: 14px;
      color: #fff;
      outline: none;
      transition: border 0.15s ease;
    }
    .search-input:focus { border-color: var(--primary); }
    .search-clear {
      position: absolute;
      right: 10px;
      top: 50%;
      transform: translateY(-50%);
      background: none;
      border: none;
      color: var(--text-dim);
      font-size: 16px;
      cursor: pointer;
      display: none;
    }
    
    /* System & State Pills */
    .filter-pills-row {
      display: flex;
      align-items: center;
      gap: 6px;
      overflow-x: auto;
      padding-bottom: 4px;
    }
    .pill-btn {
      padding: 6px 12px;
      border-radius: 20px;
      border: 1px solid var(--border);
      background: var(--card-alt);
      color: var(--text-muted);
      font-size: 12px;
      font-weight: 600;
      cursor: pointer;
      white-space: nowrap;
      transition: all 0.15s;
    }
    .pill-btn:hover { color: var(--text); border-color: var(--text-dim); }
    .pill-btn.active {
      background: var(--accent);
      color: #090d16;
      border-color: var(--accent);
      font-weight: 700;
    }
    .state-filter-group {
      display: flex;
      gap: 6px;
      border-right: 1px solid var(--border);
      padding-right: 10px;
      margin-right: 4px;
    }

    /* Game Table / Cards */
    .game-list-container {
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: var(--radius);
      overflow: hidden;
    }
    .game-list-header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 12px 18px;
      background: var(--card-alt);
      border-bottom: 1px solid var(--border);
      font-size: 13px;
      color: var(--text-muted);
      font-weight: 600;
    }
    .game-table {
      width: 100%;
      border-collapse: collapse;
      font-size: 13px;
    }
    .game-table th {
      text-align: left;
      padding: 10px 16px;
      background: var(--card-alt);
      color: var(--text-dim);
      font-weight: 600;
      border-bottom: 1px solid var(--border);
    }
    .game-table td {
      padding: 12px 16px;
      border-bottom: 1px solid var(--border);
      vertical-align: middle;
    }
    .game-table tr:hover td {
      background: rgba(255,255,255,0.02);
    }
    .game-title {
      font-weight: 600;
      color: var(--text);
      display: block;
      margin-bottom: 2px;
    }
    .game-file {
      font-size: 11px;
      color: var(--text-dim);
      font-family: monospace;
    }
    .sys-tag {
      display: inline-block;
      padding: 3px 8px;
      border-radius: 6px;
      font-size: 11px;
      font-weight: 700;
      letter-spacing: 0.3px;
    }
    .sys-GBA { background: #7c2d12; color: #fdba74; }
    .sys-FC, .sys-NES { background: #991b1b; color: #fca5a5; }
    .sys-SFC, .sys-SNES { background: #1e3a8a; color: #bfdbfe; }
    .sys-PS, .sys-PSX { background: #374151; color: #e5e7eb; }
    .sys-MD { background: #14532d; color: #86efac; }
    .sys-N64 { background: #581c87; color: #e9d5ff; }
    .sys-NDS { background: #064e3b; color: #6ee7b7; }
    .sys-PSP { background: #164e63; color: #a5f3fc; }
    .sys-ARCADE, .sys-NEOGEO { background: #831843; color: #fbcfe8; }
    .sys-default { background: #334155; color: #cbd5e1; }

    .status-badge {
      display: inline-flex;
      align-items: center;
      gap: 5px;
      padding: 4px 9px;
      border-radius: 12px;
      font-size: 11px;
      font-weight: 600;
    }
    .status-local { background: var(--green-bg); color: var(--green); border: 1px solid var(--green-border); }
    .status-cloud { background: #1e293b; color: var(--text-dim); border: 1px solid var(--border); }
    .status-queue { background: var(--yellow-bg); color: var(--yellow); border: 1px solid var(--yellow); }

    /* View Toggle and Page Size Controls */
    .view-toggle-group {
      display: inline-flex;
      background: var(--bg);
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 2px;
      gap: 2px;
    }
    .view-toggle-btn {
      border: none;
      background: none;
      color: var(--text-dim);
      padding: 5px 12px;
      font-size: 12px;
      font-weight: 600;
      cursor: pointer;
      border-radius: 6px;
      transition: all 0.15s ease;
      display: inline-flex;
      align-items: center;
      gap: 5px;
    }
    .view-toggle-btn:hover { color: var(--text); }
    .view-toggle-btn.active {
      background: var(--card-alt);
      color: var(--accent);
      box-shadow: 0 1px 3px rgba(0,0,0,0.4);
      font-weight: 700;
    }

    /* Grid View Styles */
    .game-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(200px, 1fr));
      gap: 16px;
      padding: 18px;
    }
    .game-card {
      background: var(--card-alt);
      border: 1px solid var(--border);
      border-radius: var(--radius);
      overflow: hidden;
      display: flex;
      flex-direction: column;
      transition: transform 0.18s ease, border-color 0.18s ease, box-shadow 0.18s ease;
    }
    .game-card:hover {
      transform: translateY(-4px);
      border-color: var(--border-hover);
      box-shadow: 0 8px 24px rgba(0,0,0,0.45);
    }
    .card-cover-wrap {
      position: relative;
      width: 100%;
      height: 160px;
      background: #0b0f19;
      display: flex;
      align-items: center;
      justify-content: center;
      overflow: hidden;
      border-bottom: 1px solid var(--border);
    }
    .card-cover-img {
      width: 100%;
      height: 100%;
      object-fit: cover;
      transition: transform 0.25s ease;
    }
    .game-card:hover .card-cover-img {
      transform: scale(1.06);
    }
    .card-cover-placeholder {
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      gap: 6px;
      width: 100%;
      height: 100%;
      background: linear-gradient(135deg, #111827 0%, #1e293b 100%);
      color: var(--text-dim);
    }
    .card-cover-icon {
      font-size: 38px;
      opacity: 0.8;
    }
    .card-sys-tag {
      position: absolute;
      top: 8px;
      left: 8px;
      box-shadow: 0 2px 8px rgba(0,0,0,0.6);
      z-index: 2;
    }
    .card-status-pill {
      position: absolute;
      top: 8px;
      right: 8px;
      font-size: 10px;
      padding: 2px 7px;
      border-radius: 6px;
      font-weight: 700;
      box-shadow: 0 2px 8px rgba(0,0,0,0.6);
      z-index: 2;
    }
    .card-body {
      padding: 12px;
      display: flex;
      flex-direction: column;
      flex: 1;
      justify-content: space-between;
      gap: 8px;
    }
    .card-title {
      font-size: 13px;
      font-weight: 700;
      color: #fff;
      line-height: 1.4;
      display: -webkit-box;
      -webkit-line-clamp: 2;
      -webkit-box-orient: vertical;
      overflow: hidden;
      min-height: 36px;
    }
    .card-file {
      font-size: 11px;
      color: var(--text-dim);
      font-family: monospace;
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }
    .card-meta {
      display: flex;
      justify-content: space-between;
      align-items: center;
      font-size: 11px;
      color: var(--text-muted);
      margin-top: 2px;
    }
    .card-actions {
      display: flex;
      gap: 6px;
      padding-top: 10px;
      border-top: 1px solid rgba(255,255,255,0.06);
    }
    .card-actions .btn {
      flex: 1;
      justify-content: center;
      padding: 6px 4px;
      font-size: 11px;
    }

    /* Action Buttons */
    .btn {
      padding: 6px 12px;
      border-radius: 6px;
      font-size: 12px;
      font-weight: 600;
      cursor: pointer;
      border: none;
      display: inline-flex;
      align-items: center;
      gap: 5px;
      transition: all 0.15s;
    }
    .btn-primary { background: var(--primary); color: #fff; }
    .btn-primary:hover { background: var(--primary-hover); }
    .btn-danger { background: var(--red-bg); color: var(--red); border: 1px solid var(--red); }
    .btn-danger:hover { background: var(--red); color: #fff; }
    .btn-secondary { background: var(--card-alt); color: var(--text-muted); border: 1px solid var(--border); }
    .btn-secondary:hover { color: var(--text); border-color: var(--text-dim); }

    /* Pagination */
    .pagination {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 14px 18px;
      background: var(--card-alt);
    }

    /* Queue & Active Download Card */
    .active-download-card {
      background: linear-gradient(135deg, #0c4a6e 0%, #0f172a 100%);
      border: 1px solid #0284c766;
      border-radius: var(--radius);
      padding: 20px;
      margin-bottom: 20px;
      box-shadow: 0 10px 25px rgba(2, 132, 199, 0.15);
    }
    .progress-track {
      height: 10px;
      background: rgba(0,0,0,0.5);
      border-radius: 5px;
      overflow: hidden;
      margin: 12px 0 8px 0;
    }
    .progress-fill {
      height: 100%;
      background: linear-gradient(90deg, #38bdf8, #22c55e);
      border-radius: 5px;
      transition: width 0.3s ease;
      width: 0%;
    }
    .dl-metrics {
      display: flex;
      justify-content: space-between;
      font-size: 12px;
      color: #94a3b8;
    }

    /* Cards Grid */
    .grid-2 { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 16px; margin-bottom: 20px; }
    .card {
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: var(--radius);
      padding: 20px;
    }
    .card h3 { font-size: 16px; margin-bottom: 12px; display: flex; align-items: center; gap: 8px; color: var(--accent); }
    
    /* Storage Meter */
    .storage-bar {
      height: 12px;
      background: #1e293b;
      border-radius: 6px;
      overflow: hidden;
      margin: 10px 0;
      display: flex;
    }
    .storage-used { background: var(--accent); height: 100%; }

    /* Toast Notification */
    #toast {
      position: fixed;
      bottom: 24px;
      right: 24px;
      background: #1e293b;
      border: 1px solid var(--primary);
      color: #fff;
      padding: 12px 20px;
      border-radius: 8px;
      box-shadow: 0 10px 25px rgba(0,0,0,0.6);
      font-size: 14px;
      font-weight: 500;
      opacity: 0;
      transform: translateY(20px);
      transition: all 0.25s ease;
      z-index: 9999;
      pointer-events: none;
    }
    #toast.show { opacity: 1; transform: translateY(0); }

    /* Storage Architecture Banner */
    .storage-architecture-banner {
      display: grid;
      grid-template-columns: 1fr auto 1fr auto 1.15fr;
      align-items: stretch;
      gap: 12px;
      margin-bottom: 20px;
      padding: 16px;
      background: linear-gradient(135deg, rgba(17, 24, 39, 0.95), rgba(15, 23, 42, 0.95));
      border: 1px solid var(--border);
      border-radius: var(--radius);
      box-shadow: 0 4px 20px rgba(0,0,0,0.3);
    }
    .arch-card {
      background: var(--card-alt);
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 14px;
      display: flex;
      flex-direction: column;
      justify-content: space-between;
      gap: 8px;
    }
    .arch-public { border-color: rgba(2, 132, 199, 0.45); }
    .arch-sd { border-color: rgba(16, 185, 129, 0.45); }
    .arch-private { border-color: rgba(168, 85, 247, 0.45); }
    .arch-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 8px;
      flex-wrap: wrap;
    }
    .arch-header h4 {
      margin: 0;
      font-size: 13px;
      font-weight: 700;
      color: #fff;
      display: flex;
      align-items: center;
      gap: 6px;
    }
    .arch-card p {
      margin: 0;
      font-size: 11.5px;
      color: var(--text-muted);
      line-height: 1.5;
    }
    .arch-status {
      font-size: 11px;
      color: var(--text-dim);
      padding: 6px 8px;
      background: var(--bg);
      border-radius: 6px;
      border: 1px solid var(--border);
      word-break: break-all;
    }
    .arch-arrow {
      display: flex;
      align-items: center;
      justify-content: center;
      font-size: 18px;
      color: var(--text-dim);
      font-weight: bold;
    }

    /* Active Upload Floating Bar */
    #active-upload-floating {
      position: fixed;
      bottom: 24px;
      left: 24px;
      background: linear-gradient(135deg, #2e1065, #1e1b4b);
      border: 1px solid #9333ea;
      box-shadow: 0 10px 30px rgba(0,0,0,0.65);
      border-radius: 10px;
      padding: 14px 18px;
      color: #fff;
      z-index: 9998;
      max-width: 440px;
      width: calc(100% - 48px);
      display: none;
    }

    /* Mobile Adaptations */
    @media (max-width: 960px) {
      .storage-architecture-banner {
        grid-template-columns: 1fr;
      }
      .arch-arrow {
        transform: rotate(90deg);
        padding: 4px 0;
      }
    }
    @media (max-width: 680px) {
      .game-table th:nth-child(3), .game-table td:nth-child(3) { display: none; }
      .hide-mobile { display: none; }
    }

    /* Technology glassmorphism: kính mờ + viền sáng + glow */
    header, .controls-bar, .game-list-container, .card, .arch-card,
    .active-download-card {
      background: linear-gradient(135deg, rgba(17,25,40,0.62), rgba(15,23,42,0.45)) !important;
      -webkit-backdrop-filter: blur(14px);
      backdrop-filter: blur(14px);
      border: 1px solid rgba(148,163,184,0.22) !important;
      box-shadow: 0 8px 32px rgba(2,8,23,0.45), inset 0 1px 0 rgba(255,255,255,0.06);
    }
    .tab-btn.active {
      background: linear-gradient(135deg, #0284c7, #7c3aed);
      box-shadow: 0 4px 18px rgba(56,189,248,0.45);
      border-color: transparent;
    }
    .btn-primary {
      background: linear-gradient(135deg, #0284c7, #7c3aed);
    }
    .game-card:hover, .tab-btn:hover, .pill-btn:hover { border-color: var(--border-hover); }
    .logo-text {
      background: linear-gradient(90deg, #38bdf8, #a78bfa);
      -webkit-background-clip: text;
      background-clip: text;
      color: transparent;
    }
    ::-webkit-scrollbar { width: 10px; height: 10px; }
    ::-webkit-scrollbar-thumb { background: rgba(56,189,248,0.3); border-radius: 8px; }
    ::-webkit-scrollbar-track { background: transparent; }
  </style>
</head>
<body>
  <div class="container">
    <!-- Header -->
    <header>
      <div class="logo-area">
        <div>
          <span class="logo-text">RomCloud</span>
          <span class="device-badge">TrimUI Brick / Smart Pro</span>
        </div>
      </div>
      <div class="header-stats">
        <div class="stat-pill" id="head-auth-pill">Drive: <b>Đang tải...</b></div>
        <div class="stat-pill" id="head-storage-pill">SD: <b>Đang tải...</b></div>
        <div class="stat-pill">Game: <b id="head-local-count">)HTML" +
      std::to_string(totalLocal) +
      R"HTML(</b> trên thẻ · <b id="head-cloud-count">)HTML" +
      std::to_string(totalCloud) + R"HTML(</b> trên Drive</div>
        <button class="btn btn-danger hide-mobile" id="btn-head-logout" onclick="logoutGoogleDrive()" style="display:none; padding: 4px 10px; font-size: 11px;">Đăng xuất</button>
      </div>
    </header>

    <!-- Offline / Unlinked Warning Banner -->
    <div id="unlinked-warning-banner" style="display: none; padding: 12px 18px; background: rgba(245, 158, 11, 0.12); border: 1px solid #f59e0b; border-radius: var(--radius); margin-bottom: 20px; font-size: 13px; color: #fef3c7;">
      <b>Chưa kết nối Google Drive:</b> chỉ hiện game đã có trên thẻ nhớ. <a href="javascript:void(0)" onclick="switchTab('tab-storage')" style="color: #38bdf8; font-weight: 700; text-decoration: underline;">Kết nối Google Drive</a> để xem và tải thêm game.
    </div>

    <!-- OTA Push Notification Banner -->
    <div id="ota-push-banner" style="display: none; background: linear-gradient(135deg, #1e1b4b, #312e81, #1e40af); border: 1px solid #6366f1; border-radius: var(--radius); padding: 14px 20px; margin-bottom: 20px; box-shadow: 0 4px 20px rgba(99, 102, 241, 0.35);">
      <div style="display: flex; justify-content: space-between; align-items: center; gap: 15px; flex-wrap: wrap;">
        <div style="display: flex; align-items: center; gap: 12px;">
          <div>
            <div style="font-weight: 700; font-size: 15px; color: #fff;">Có bản cập nhật mới: <span id="ota-push-version" style="color: #38bdf8;">v1.0.x</span></div>
            <div id="ota-push-notes" style="font-size: 12px; color: #cbd5e1; margin-top: 2px;">Gồm tính năng mới và các sửa lỗi.</div>
          </div>
        </div>
        <div style="display: flex; gap: 8px;">
          <button class="btn btn-primary" onclick="quickApplyOta()" style="background: var(--green); border-color: var(--green-border); font-weight: 700;">Cập nhật ngay</button>
          <button class="btn btn-secondary" onclick="document.getElementById('ota-push-banner').style.display='none';" style="padding: 6px 12px;">Đóng</button>
        </div>
      </div>
    </div>

    <!-- Navigation Tabs -->
    <div class="tab-bar">
      <button class="tab-btn active" onclick="switchTab('tab-roms')">Quản lý ROM</button>
      <button class="tab-btn" onclick="switchTab('tab-queue')">Hàng đợi tải <span class="badge" id="nav-queue-badge">0</span></button>
      <button class="tab-btn" onclick="switchTab('tab-storage')">Đồng bộ &amp; Thẻ nhớ</button>
      <button class="tab-btn" onclick="switchTab('tab-iptv')">IPTV</button>
      <button class="tab-btn" onclick="switchTab('tab-saver')">Hình nền</button>
      <button class="tab-btn" onclick="switchTab('tab-ota')" id="nav-tab-ota">Cập nhật <span class="badge" id="ota-nav-badge" style="display: none; background: #ef4444; color: #fff; margin-left: 4px; padding: 2px 6px; border-radius: 8px; font-size: 10px;">Mới</span></button>
      <button class="tab-btn" onclick="switchTab('tab-cal')">Lịch</button>
      <button class="tab-btn" onclick="switchTab('tab-alarm')">Báo thức</button>
      <button class="tab-btn" onclick="switchTab('tab-watch')">Theo dõi giá</button>
    </div>

    <!-- TAB 1: ROM MANAGER -->
    <div id="tab-roms" class="tab-content active">
      <!-- Cloud & Storage Architecture Banner -->
      <div class="storage-architecture-banner">
        <div class="arch-card arch-public">
          <div class="arch-header">
            <h4>1. Kho game chung</h4>
            <span class="sys-tag" style="background:#075985;color:#bae6fd;">CHỈ TẢI VỀ</span>
          </div>
          <p>Kho game dùng chung qua link Google Drive, không cần đăng nhập. Tìm game rồi tải về máy.</p>
          <div class="arch-status" id="arch-public-status">
            Nguồn: <b id="arch-public-url" style="color:var(--accent);">Đang tải...</b>
          </div>
        </div>

        <div class="arch-arrow">➔</div>

        <div class="arch-card arch-sd">
          <div class="arch-header">
            <h4>2. Thẻ nhớ trên máy</h4>
            <span class="sys-tag" style="background:#065f46;color:#a7f3d0;">CÓ TRÊN MÁY</span>
          </div>
          <p>Game đã tải nằm ở <code>/mnt/SDCARD/Roms/</code>. Chơi ngay trên máy, hoặc sao lưu lên Drive cá nhân.</p>
          <div class="arch-status">
            Trạng thái: <b id="arch-local-status" style="color:var(--green);">-- game trên thẻ</b>
          </div>
          <div style="display:flex; gap:6px; margin-top:4px;">
            <button class="btn btn-secondary" onclick="startAutoScrapeSd()" style="font-size:11.5px;padding:6px 10px;flex:1;border-color:#0284c7;color:#38bdf8;" title="Tự lấy ảnh bìa và thông tin cho mọi game trên thẻ">Lấy ảnh bìa thẻ SD</button>
            <button class="btn btn-secondary" onclick="openDoctorModal()" style="font-size:11.5px;padding:6px 10px;flex:1;border-color:#10b981;color:#34d399;font-weight:600;" title="Tìm ROM đặt sai thư mục và chuyển về đúng hệ máy">Sửa ROM sai thư mục</button>
          </div>
        </div>

        <div class="arch-arrow">➔</div>

        <div class="arch-card arch-private">
          <div class="arch-header">
            <h4>3. Drive cá nhân (sao lưu)</h4>
            <span class="sys-tag" style="background:#5b21b6;color:#ddd6fe;">SAO LƯU RIÊNG</span>
          </div>
          <p>Sao lưu toàn bộ game trên thẻ vào thư mục <code>Roms</code> trên Drive của bạn, phòng khi thẻ hỏng.</p>
          <div class="arch-status" id="arch-backup-status-wrap">
            Quyền: <b id="arch-backup-badge" style="color:var(--yellow);">Đang kiểm tra...</b>
          </div>
          <div style="display:flex; gap:6px; margin-top:4px;">
            <button class="btn btn-primary" id="btn-arch-backup-all" onclick="uploadAllGamesToDrive()" style="background:var(--purple);border-color:#9333ea;font-size:11.5px;padding:6px 10px;flex:1;">Sao lưu cả thẻ lên Drive</button>
            <button class="btn btn-secondary" onclick="switchTab('tab-storage')" style="font-size:11.5px;padding:6px 10px;" title="Cài đặt sao lưu Drive">Cài đặt</button>
          </div>
        </div>
      </div>

      <div class="controls-bar">
        <div class="search-row">
          <div class="search-input-wrap">
            <input type="text" id="rom-search" class="search-input" placeholder="Tìm theo tên game hoặc tên file..." oninput="onSearchInput()">
            <button id="search-clear-btn" class="search-clear" onclick="clearSearch()">&times;</button>
          </div>
        </div>

        <div class="filter-pills-row">
          <div class="state-filter-group">
            <button class="pill-btn active" id="filter-state-all" onclick="setStateFilter(-1)">Tất cả</button>
            <button class="pill-btn" id="filter-state-local" onclick="setStateFilter(1)">Trên thẻ SD</button>
            <button class="pill-btn" id="filter-state-cloud" onclick="setStateFilter(0)">Kho chung (chưa tải)</button>
          </div>
          <div id="system-pills-container" style="display: flex; gap: 6px;">
            <!-- Rendered by JS -->
          </div>
        </div>
      </div>

      <div class="game-list-container">
        <div class="game-list-header">
          <div style="display: flex; align-items: center; gap: 12px; flex-wrap: wrap;">
            <span id="game-results-count">Đang tải danh sách game...</span>
            <span id="active-filters-desc" style="font-weight:400; font-size: 12px; color: var(--text-dim);">Tất cả hệ máy</span>
          </div>

          <div style="display: flex; align-items: center; gap: 12px; flex-wrap: wrap;">
            <div style="display: flex; align-items: center; gap: 6px; font-size: 12px; color: var(--text-dim);">
              <span>Mỗi trang:</span>
              <select id="select-page-size" onchange="changePageSize(this.value)" style="background: var(--bg); border: 1px solid var(--border); color: #fff; border-radius: 6px; padding: 4px 6px; font-size: 12px;">
                <option value="24">24</option>
                <option value="48" selected>48</option>
                <option value="96">96</option>
                <option value="120">120</option>
              </select>
            </div>

            <button class="btn btn-secondary" onclick="batchScrapeCurrentPage()" id="btn-batch-scrape" style="font-size: 12px; padding: 6px 12px;" title="Lấy ảnh bìa và thông tin cho các game ở trang này">Lấy bìa trang này</button>
            <button class="btn btn-secondary" onclick="startAutoScrapeSd()" id="btn-auto-scrape-sd" style="font-size: 12px; padding: 6px 12px; border-color: #0284c7; color: #38bdf8;" title="Lấy ảnh bìa và thông tin cho mọi game trên thẻ (chạy ngầm)">Lấy bìa cả thẻ SD</button>
            <button class="btn btn-secondary" onclick="uploadAllGamesToDrive()" id="btn-batch-backup" style="font-size: 12px; padding: 6px 12px; border-color: #9333ea; color: #c084fc;" title="Sao lưu mọi game trên thẻ lên Drive của bạn (thư mục Roms)">Sao lưu thẻ lên Drive</button>
            <button class="btn btn-secondary" onclick="openDoctorModal()" id="btn-toolbar-doctor" style="font-size: 12px; padding: 6px 12px; border-color: #10b981; color: #34d399; font-weight: 600;" title="Tìm ROM đặt sai thư mục và chuyển về đúng hệ máy">Sửa ROM sai thư mục</button>

            <div class="view-toggle-group">
              <button class="view-toggle-btn active" id="btn-view-list" onclick="setViewMode('list')" title="Xem dạng bảng">Bảng</button>
              <button class="view-toggle-btn" id="btn-view-grid" onclick="setViewMode('grid')" title="Xem dạng lưới">Lưới</button>
            </div>
          </div>
        </div>

        <!-- List View Table -->
        <div id="game-table-wrap" style="overflow-x: auto;">
          <table class="game-table">
            <thead>
              <tr>
                <th style="width: 100px;">Hệ máy</th>
                <th>Tên game / Tên file</th>
                <th style="width: 110px;">Dung lượng</th>
                <th style="width: 130px;">Trạng thái</th>
                <th style="width: 160px; text-align: right;">Thao tác</th>
              </tr>
            </thead>
            <tbody id="game-table-body">
              <tr><td colspan="5" style="text-align: center; padding: 30px; color: var(--text-dim);">Đang tải dữ liệu...</td></tr>
            </tbody>
          </table>
        </div>

        <!-- Grid View Cards -->
        <div id="game-grid-wrap" class="game-grid" style="display: none;">
          <!-- Rendered by JS -->
        </div>

        <!-- Pagination Controls -->
        <div class="pagination" style="display: flex; justify-content: space-between; align-items: center; padding: 12px 18px; border-top: 1px solid var(--border); flex-wrap: wrap; gap: 10px;">
          <div style="display: flex; gap: 6px;">
            <button class="btn btn-secondary" id="btn-first-page" onclick="goToPage(1)" title="Về trang đầu">&laquo;</button>
            <button class="btn btn-secondary" id="btn-prev-page" onclick="changePage(-1)">&larr; Trang trước</button>
          </div>

          <div style="display: flex; align-items: center; gap: 8px; font-size: 13px; color: var(--text-muted);">
            <span>Trang</span>
            <input type="number" id="input-jump-page" min="1" max="1" value="1" onchange="goToPage(parseInt(this.value))" onkeydown="if(event.key==='Enter') goToPage(parseInt(this.value))" style="width: 55px; text-align: center; background: var(--bg); border: 1px solid var(--border); color: #fff; border-radius: 6px; padding: 4px; font-size: 13px;">
            <span id="page-total-indicator">/ 1</span>
          </div>

          <div style="display: flex; gap: 6px;">
            <button class="btn btn-secondary" id="btn-next-page" onclick="changePage(1)">Trang sau &rarr;</button>
            <button class="btn btn-secondary" id="btn-last-page" onclick="goToPage(maxPagesCache)" title="Tới trang cuối">&raquo;</button>
          </div>
        </div>
      </div>
    </div>

    <!-- TAB 2: DOWNLOAD QUEUE -->
    <div id="tab-queue" class="tab-content">
      <div id="active-download-section" style="display: none;" class="active-download-card">
        <div style="display: flex; justify-content: space-between; align-items: flex-start; margin-bottom: 6px;">
          <div>
            <span class="sys-tag sys-default" id="dl-sys-tag">GBA</span>
            <span style="font-size: 18px; font-weight: 700; margin-left: 8px;" id="dl-title">Pokemon Emerald</span>
          </div>
          <button class="btn btn-danger" onclick="cancelActiveDownload()">Hủy tải</button>
        </div>
        <div style="font-size: 12px; color: #cbd5e1; font-family: monospace;" id="dl-file">pokemon.gba</div>
        <div class="progress-track">
          <div class="progress-fill" id="dl-progress-fill"></div>
        </div>
        <div class="dl-metrics">
          <span id="dl-speed">0 KB/s</span>
          <span id="dl-percent">0%</span>
          <span id="dl-bytes">0 / 0 MB</span>
        </div>
      </div>

      <div class="card">
        <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 14px;">
          <h3>Hàng đợi tải về (<span id="queue-count-num">0</span>)</h3>
          <button class="btn btn-secondary" onclick="clearAllQueue()">Xóa hết hàng đợi</button>
        </div>
        <div id="queue-list-container">
          <p style="color: var(--text-dim); text-align: center; padding: 20px;">Chưa có game nào đang chờ tải.</p>
        </div>
      </div>
    </div>

    <!-- TAB 3: STORAGE & CLOUD SYNC -->
    <div id="tab-storage" class="tab-content">
      <div class="grid-2">
        <div class="card">
          <h3>Dung lượng thẻ nhớ</h3>
          <div class="storage-bar">
            <div class="storage-used" id="storage-bar-fill" style="width: 30%;"></div>
          </div>
          <div style="display: flex; justify-content: space-between; font-size: 13px; margin-bottom: 12px;">
            <span>Đã dùng: <b id="storage-used-txt">-- GB</b></span>
            <span>Còn trống: <b id="storage-avail-txt" style="color: var(--green);">-- GB</b></span>
            <span>Tổng: <b id="storage-total-txt">-- GB</b></span>
          </div>
          <p style="font-size: 12px; color: var(--text-dim); line-height: 1.6;">
            Game tải về lưu trong <code>/mnt/SDCARD/Roms/&lt;HỆ_MÁY&gt;/</code>. Rút thẻ hay cập nhật máy, game vẫn còn nguyên.
          </p>
        </div>

        <div class="card">
          <h3>Đồng bộ Google Drive</h3>
          <div id="drive-connection-status" style="margin-bottom: 12px; font-size: 13px;">
            Trạng thái: <b style="color:var(--text-dim);">Đang kiểm tra...</b>
          </div>
          <p style="font-size: 13px; color: var(--text-muted); margin-bottom: 14px;">
            Lần đồng bộ gần nhất: <b id="last-sync-time">)HTML" +
      lastSyncTime + R"HTML(</b>
          </p>
          <div style="display: flex; gap: 10px; margin-bottom: 16px; flex-wrap: wrap;">
            <button class="btn btn-primary" id="btn-trigger-sync" onclick="triggerSync()">Quét và đồng bộ ngay</button>
            <button class="btn btn-danger" id="btn-tab-logout" onclick="logoutGoogleDrive()" style="display: none;">Đăng xuất Google Drive</button>
          </div>
          <div id="sync-status-box" style="display: none; padding: 12px; background: var(--card-alt); border-radius: 8px; font-size: 13px; margin-top: 10px;">
            <div id="sync-status-txt">Đang quét thư mục Drive...</div>
          </div>

          <form id="form-connect-drive" onsubmit="handleConnectSubmit(event)" autocomplete="off" style="margin-top: 18px; border-top: 1px solid var(--border); padding-top: 14px;">
            <label style="font-size: 12px; font-weight: 600; color: var(--accent); display: block; margin-bottom: 4px;">1. Kho game tải về (link Drive chia sẻ):</label>
            <p style="font-size: 11px; color: var(--text-muted); margin-bottom: 8px;">Dán link thư mục Drive chứa game để xem và tải về máy. Miễn phí, không cần đăng nhập.</p>
            <div style="display: flex; gap: 8px; flex-wrap: wrap;">
              <input type="text" id="input-drive-url" name="drive_url" value=")HTML" +
      savedDriveUrl +
      R"HTML(" placeholder="https://drive.google.com/drive/folders/... (Dán link vào đây)" autocomplete="off" style="flex: 1; min-width: 220px; padding: 8px 12px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 12px;" required>
              <button type="submit" class="btn btn-primary">Kết nối và quét</button>
              <button type="button" class="btn btn-secondary" onclick="clearDriveInput()">Xóa</button>
            </div>
          </form>

          <div style="margin-top: 20px; border-top: 1px solid var(--border); padding-top: 16px;">
            <label style="font-size: 13px; font-weight: 700; color: var(--purple); display: block; margin-bottom: 6px;">2. Sao lưu lên Drive của bạn:</label>
            <p style="font-size: 12px; color: var(--text-muted); margin-bottom: 12px; line-height: 1.5;">
              Muốn sao lưu game và file save từ thẻ lên Drive riêng, hãy đăng nhập Google:
            </p>

            <div id="backup-perm-status" style="font-size: 12px; margin-bottom: 14px; padding: 10px 14px; background: var(--bg); border: 1px solid var(--border); border-radius: 8px;">
              Quyền sao lưu: <b id="backup-perm-badge" style="color:var(--yellow);">Đang kiểm tra...</b>
            </div>

            <!-- Khối Đăng nhập Google 1 chạm chính thức -->
            <div id="google-auth-box" style="background: var(--card-alt); border: 1px solid rgba(168, 85, 247, 0.4); border-radius: 10px; padding: 16px; margin-bottom: 16px;">
              <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 10px;">
                <div>
                  <h4 style="margin: 0 0 4px 0; color: #fff; font-size: 14px; display: flex; align-items: center; gap: 8px;">
                    <svg width="18" height="18" viewBox="0 0 24 24"><path fill="#4285F4" d="M22.56 12.25c0-.78-.07-1.53-.2-2.25H12v4.26h5.92c-.26 1.37-1.04 2.53-2.21 3.31v2.77h3.57c2.08-1.92 3.28-4.74 3.28-8.09z"/><path fill="#34A853" d="M12 23c2.97 0 5.46-.98 7.28-2.66l-3.57-2.77c-.98.66-2.23 1.06-3.71 1.06-2.86 0-5.29-1.93-6.16-4.53H2.18v2.84C3.99 20.53 7.7 23 12 23z"/><path fill="#FBBC05" d="M5.84 14.09c-.22-.66-.35-1.36-.35-2.09s.13-1.43.35-2.09V7.06H2.18C1.43 8.55 1 10.22 1 12s.43 3.45 1.18 4.94l2.85-2.22.81-.63z"/><path fill="#EA4335" d="M12 5.38c1.62 0 3.06.56 4.21 1.64l3.15-3.15C17.45 2.09 14.97 1 12 1 7.7 1 3.99 3.47 2.18 7.06l3.66 2.84c.87-2.6 3.3-4.52 6.16-4.52z"/></svg>
                    Đăng nhập Google
                  </h4>
                  <p style="margin: 0; font-size: 12px; color: var(--text-muted);">RomCloud đã được Google phê duyệt. Đăng nhập để lưu game và file save vào Drive của bạn.</p>
                </div>
                <div style="display: flex; gap: 8px;">
                  <button type="button" class="btn btn-primary" id="btn-google-login-start" onclick="startGoogleDeviceLogin()" style="background: linear-gradient(135deg, #4285F4, #34A853); font-weight: 600; padding: 10px 18px; border: none; box-shadow: 0 4px 12px rgba(66, 133, 244, 0.35);">
                    Đăng nhập Google
                  </button>
                  <button type="button" class="btn btn-secondary" id="btn-clear-personal-token" onclick="clearPersonalToken()" style="display: none;">Hủy quyền sao lưu</button>
                </div>
              </div>

              <!-- Trạng thái chờ User Code -->
              <div id="google-device-prompt" style="display: none; margin-top: 14px; padding: 14px; background: rgba(66, 133, 244, 0.08); border: 1px solid rgba(66, 133, 244, 0.3); border-radius: 8px;">
                <div style="font-size: 13px; font-weight: 600; color: #93c5fd; margin-bottom: 6px;">Bước 1: Mở trang xác nhận của Google:</div>
                <div style="display: flex; gap: 10px; align-items: center; margin-bottom: 12px; flex-wrap: wrap;">
                  <a id="google-verify-link" href="https://www.google.com/device" target="_blank" rel="noopener noreferrer" class="btn btn-primary" style="text-decoration: none; padding: 7px 14px; font-size: 12px; background: #2563eb;">
                    Mở google.com/device
                  </a>
                  <span style="font-size: 12px; color: var(--text-dim);">hoặc mở trên điện thoại: <code>https://www.google.com/device</code></span>
                </div>
                <div style="font-size: 13px; font-weight: 600; color: #93c5fd; margin-bottom: 6px;">Bước 2: Nhập mã này:</div>
                <div style="display: flex; gap: 12px; align-items: center; flex-wrap: wrap;">
                  <div id="google-user-code-display" style="font-size: 26px; font-weight: 800; letter-spacing: 4px; color: #fbbf24; background: rgba(0,0,0,0.4); padding: 6px 16px; border-radius: 6px; border: 1px dashed rgba(251, 191, 36, 0.6); user-select: all;">----</div>
                  <button type="button" class="btn btn-secondary" onclick="copyUserCode()" style="padding: 6px 12px; font-size: 12px;">Chép mã</button>
                  <button type="button" class="btn btn-danger" onclick="cancelGoogleDeviceLogin()" style="padding: 6px 12px; font-size: 12px;">Hủy</button>
                </div>
                <div id="google-login-timer" style="margin-top: 10px; font-size: 11px; color: var(--text-dim);">Đang chờ bạn xác nhận trên Google. Xong sẽ tự đăng nhập.</div>
              </div>
            </div>

            <!-- Tùy chọn nâng cao: Dán Token thủ công -->
            <details style="margin-bottom: 16px; background: var(--bg); border: 1px solid var(--border); border-radius: 8px; padding: 10px 14px;">
              <summary style="font-size: 12px; font-weight: 600; color: var(--text-dim); cursor: pointer; user-select: none;">
                Nâng cao: nhập token thủ công
              </summary>
              <div style="margin-top: 10px;">
                <p style="font-size: 11px; color: var(--text-muted); margin-bottom: 8px;">Tự dán Access Token (ya29...) hoặc Refresh Token:</p>
                <div style="display: flex; gap: 8px; flex-wrap: wrap;">
                  <input type="text" id="input-personal-token" placeholder="Dán Access Token (ya29...) hoặc Refresh Token (1//...) vào đây" autocomplete="off" onkeydown="if(event.key==='Enter') savePersonalToken()" style="flex: 1; min-width: 240px; padding: 8px 12px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 12px;">
                  <button type="button" class="btn btn-secondary" id="btn-save-personal-token" onclick="savePersonalToken()">Lưu token</button>
                </div>
                <div id="personal-token-status" style="margin-top: 6px; font-size: 11px;"></div>
              </div>
            </details>
          </div>
        </div>

        <div class="card" style="margin-top: 20px;">
          <h3>Gửi log khi gặp lỗi</h3>
          <p style="font-size: 13px; color: var(--text-muted); margin-bottom: 14px;">
            Gặp lỗi (mạng, đồng bộ, phát video...)? Tải file <code>debug.log</code> dưới đây và gửi cho người phát triển để được hỗ trợ.
          </p>
          <div style="display: flex; gap: 10px; align-items: center; flex-wrap: wrap;">
            <a href="/debug.log" download="debug.log" class="btn btn-primary" style="text-decoration: none; display: inline-flex; align-items: center; gap: 6px;">
              Tải debug.log
            </a>
            <span style="font-size: 12px; color: var(--text-dim);">Hoặc lấy trực tiếp trên thẻ: <code>Apps/RomCloud/debug.log</code></span>
          </div>
        </div>
      </div>
    </div>

    <!-- TAB 4: IPTV -->
    <div id="tab-iptv" class="tab-content">
      <div style="max-width: 900px; margin: 0 auto;">
        <div class="card" style="margin-bottom: 20px;">
          <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px;">
            <h3 style="margin: 0;">Nguồn kênh IPTV</h3>
            <span id="iptv-total-summary" style="font-size: 13px; font-weight: 600; color: var(--accent); background: rgba(0, 180, 216, 0.1); padding: 4px 10px; border-radius: 6px;">Đang tải...</span>
          </div>
          <p style="font-size: 13px; color: var(--text-muted); margin-bottom: 16px;">
            Thêm, sửa hoặc xóa nguồn kênh (.m3u, .m3u8 hoặc link). Tên nguồn hiện ở cột bên phải trong danh sách kênh trên máy.
          </p>

          <!-- Current Sources Table -->
          <div style="margin-top: 15px;">
            <h4 style="margin-bottom: 10px; display: flex; align-items: center; gap: 8px;">
              Nguồn kênh đã thêm
              <button class="btn btn-secondary" onclick="loadIptvSources()" style="padding: 3px 8px; font-size: 11px;">Làm mới</button>
            </h4>
            <div id="iptv-sources-container" style="background: var(--card-alt); border-radius: 10px; border: 1px solid var(--border); overflow: hidden;">
              <div style="padding: 20px; text-align: center; color: var(--text-muted);">Đang tải...</div>
            </div>
          </div>
        </div>

        <!-- Add Source Grid -->
        <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(360px, 1fr)); gap: 20px;">
          <!-- Quick Add URL -->
          <div class="card">
            <h4 style="margin-bottom: 8px;">Thêm nguồn từ link</h4>
            <p style="font-size: 12px; color: var(--text-muted); margin-bottom: 14px;">
              Dán link danh sách kênh M3U/M3U8 từ nhà cung cấp.
            </p>
            <div style="margin-bottom: 10px;">
              <label style="font-size: 12px; font-weight: 600; color: var(--text-muted); display: block; margin-bottom: 4px;">Tên nguồn (không bắt buộc):</label>
              <input type="text" id="iptv-url-name" placeholder="Ví dụ: Kênh Việt Nam, Thể thao..." class="search-input" style="width: 100%; box-sizing: border-box; padding: 9px 12px;">
            </div>
            <div style="margin-bottom: 14px;">
              <label style="font-size: 12px; font-weight: 600; color: var(--text-muted); display: block; margin-bottom: 4px;">Link danh sách kênh:</label>
              <input type="text" id="iptv-url" placeholder="https://example.com/playlist.m3u" class="search-input" style="width: 100%; box-sizing: border-box; padding: 9px 12px;">
            </div>
            <button class="btn btn-primary" id="btn-add-iptv-url" onclick="addIptvUrl()" style="width: 100%; padding: 10px; font-weight: 600;">Tải và thêm nguồn</button>
            <div id="iptv-url-status" style="margin-top: 10px; font-size: 12.5px; display: none;"></div>
          </div>

          <!-- Upload Form -->
          <div class="card">
            <h4 style="margin-bottom: 8px;">Tải lên file danh sách (.m3u, .m3u8)</h4>
            <p style="font-size: 12px; color: var(--text-muted); margin-bottom: 14px;">
              Chuyển file danh sách kênh từ máy tính hoặc điện thoại lên máy.
            </p>
            <div style="margin-bottom: 10px;">
              <label style="font-size: 12px; font-weight: 600; color: var(--text-muted); display: block; margin-bottom: 4px;">Tên nguồn (không bắt buộc):</label>
              <input type="text" id="iptv-file-name" placeholder="Để trống sẽ dùng tên file" class="search-input" style="width: 100%; box-sizing: border-box; padding: 9px 12px;">
            </div>
            <div style="margin-bottom: 14px;">
              <label style="font-size: 12px; font-weight: 600; color: var(--text-muted); display: block; margin-bottom: 4px;">Chọn file M3U/M3U8:</label>
              <input type="file" id="iptv-file" accept=".m3u,.m3u8" style="width: 100%; box-sizing: border-box; padding: 8px; background: var(--bg); border: 1px solid var(--border); border-radius: 8px; color: var(--text);">
            </div>
            <button class="btn btn-primary" id="btn-upload-iptv" onclick="uploadIptvPlaylist()" style="width: 100%; padding: 10px; font-weight: 600;">Tải lên</button>
            <div id="iptv-file-status" style="margin-top: 10px; font-size: 12.5px; display: none;"></div>
          </div>
        </div>
      </div>

      <script>
        function formatBytes(bytes) {
          if (!bytes || bytes === 0) return '0 B';
          const k = 1024;
          const sizes = ['B', 'KB', 'MB', 'GB'];
          const i = Math.floor(Math.log(bytes) / Math.log(k));
          return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i];
        }

        function loadIptvSources() {
          const container = document.getElementById('iptv-sources-container');
          const summaryEl = document.getElementById('iptv-total-summary');
          if (!container) return;
          fetch('/api/iptv/sources')
            .then(r => r.json())
            .then(d => {
              if (!d.success || !d.sources || d.sources.length === 0) {
                container.innerHTML = '<div style="padding: 24px; text-align: center; color: var(--text-muted);">'
                  + '<b>Chưa có nguồn kênh nào</b><br>'
                  + '<span style="font-size: 12px;">Thêm nguồn bằng link hoặc tải file danh sách kênh ở dưới.</span></div>';
                if (summaryEl) summaryEl.textContent = '0 nguồn • 0 kênh';
                return;
              }

              if (summaryEl) {
                summaryEl.textContent = d.sources.length + ' nguồn • ' + (d.total_channels || 0) + ' kênh';
              }

              let html = '<table style="width: 100%; border-collapse: collapse; text-align: left; font-size: 13px;">'
                + '<thead style="background: rgba(255,255,255,0.03); border-bottom: 1px solid var(--border); color: var(--text-muted); font-size: 12px;">'
                + '<tr>'
                + '<th style="padding: 10px 14px;">TÊN NGUỒN</th>'
                + '<th style="padding: 10px 14px;">LOẠI</th>'
                + '<th style="padding: 10px 14px; text-align: center;">SỐ KÊNH</th>'
                + '<th style="padding: 10px 14px; text-align: right;">DUNG LƯỢNG</th>'
                + '<th style="padding: 10px 14px; text-align: center;">THAO TÁC</th>'
                + '</tr>'
                + '</thead><tbody>';

              d.sources.forEach((s) => {
                const isUrl = s.type === 'url';
                const typeBadge = isUrl
                  ? '<span style="background: #1e3a5f; color: #38bdf8; padding: 2px 7px; border-radius: 4px; font-size: 11px; font-weight: 600;">Link</span>'
                  : '<span style="background: #1e293b; color: #94a3b8; padding: 2px 7px; border-radius: 4px; font-size: 11px; font-weight: 600;">File tải lên</span>';
                
                const subText = isUrl ? (s.url || s.filename) : s.filename;

                html += '<tr style="border-bottom: 1px solid var(--border);">'
                  + '<td style="padding: 12px 14px;">'
                  + '<div style="font-weight: 600; color: var(--text); font-size: 14px;">' + escapeHtml(s.name) + '</div>'
                  + '<div style="font-size: 11.5px; color: var(--text-muted); word-break: break-all; max-width: 320px;" title="' + escapeHtml(subText) + '">' + escapeHtml(subText) + '</div>'
                  + '</td>'
                  + '<td style="padding: 12px 14px;">' + typeBadge + '</td>'
                  + '<td style="padding: 12px 14px; text-align: center; font-weight: 600; color: var(--accent);">' + s.channels + '</td>'
                  + '<td style="padding: 12px 14px; text-align: right; color: var(--text-muted);">' + formatBytes(s.size) + '</td>'
                  + '<td style="padding: 12px 14px; text-align: center;">'
                  + '<div style="display: flex; gap: 6px; justify-content: center;">';
                
                if (isUrl && s.url) {
                  html += '<button class="btn btn-secondary" onclick="reloadIptvUrl(\'' + escapeHtml(s.url) + '\', \'' + escapeHtml(s.name) + '\')" style="padding: 4px 8px; font-size: 11.5px;" title="Tải lại kênh từ link">Tải lại</button>';
                }
                
                html += '<button class="btn btn-danger" onclick="deleteIptvSource(\'' + escapeHtml(s.filename) + '\', \'' + escapeHtml(s.name) + '\')" style="padding: 4px 10px; font-size: 11.5px;">Xóa</button>'
                  + '</div>'
                  + '</td>'
                  + '</tr>';
              });

              html += '</tbody></table>';
              container.innerHTML = html;
            })
            .catch(err => {
              if (container) container.innerHTML = '<div style="padding: 16px; color: var(--red); text-align: center;">Lỗi tải danh sách: ' + err + '</div>';
            });
        }

        function deleteIptvSource(filename, name) {
          if (!confirm('Bạn có chắc muốn xóa nguồn kênh "' + name + '" (' + filename + ') không?')) {
            return;
          }
          fetch('/api/iptv/delete?file=' + encodeURIComponent(filename))
            .then(r => r.json())
            .then(d => {
              if (d.success) {
                loadIptvSources();
              } else {
                alert('Lỗi xóa nguồn: ' + (d.error || 'Thất bại'));
              }
            })
            .catch(e => alert('Lỗi kết nối: ' + e));
        }

        function reloadIptvUrl(url, name) {
          if (!confirm('Tải lại danh sách kênh mới nhất từ "' + name + '"?')) return;
          const statusEl = document.getElementById('iptv-url-status');
          if (statusEl) {
            statusEl.style.display = 'block';
            statusEl.style.color = 'var(--accent)';
            statusEl.textContent = '⏳ Đang làm mới nguồn kênh...';
          }
          fetch('/api/iptv/add?url=' + encodeURIComponent(url) + '&name=' + encodeURIComponent(name))
            .then(r => r.json())
            .then(d => {
              if (d.success) {
                if (statusEl) {
                  statusEl.style.color = 'var(--green)';
                  statusEl.textContent = 'Đã cập nhật: ' + d.channels + ' kênh.';
                }
                loadIptvSources();
              } else {
                if (statusEl) {
                  statusEl.style.color = 'var(--red)';
                  statusEl.textContent = 'Lỗi: ' + (d.error || 'Cập nhật thất bại');
                }
              }
            })
            .catch(e => {
              if (statusEl) {
                statusEl.style.color = 'var(--red)';
                statusEl.textContent = 'Lỗi: ' + e;
              }
            });
        }

        function addIptvUrl() {
          const urlInput = document.getElementById('iptv-url');
          const nameInput = document.getElementById('iptv-url-name');
          const btn = document.getElementById('btn-add-iptv-url');
          const statusEl = document.getElementById('iptv-url-status');

          const url = (urlInput.value || '').trim();
          const name = (nameInput.value || '').trim();

          if (!url) {
            alert('Hãy dán link danh sách kênh');
            urlInput.focus();
            return;
          }

          btn.disabled = true;
          btn.textContent = 'Đang tải kênh...';
          statusEl.style.display = 'block';
          statusEl.style.color = 'var(--accent)';
          statusEl.textContent = 'Đang tải danh sách kênh...';

          fetch('/api/iptv/add?url=' + encodeURIComponent(url) + '&name=' + encodeURIComponent(name))
            .then(r => r.json())
            .then(d => {
              btn.disabled = false;
              btn.textContent = 'Tải và thêm nguồn';
              if (d.success) {
                statusEl.style.color = 'var(--green)';
                statusEl.textContent = 'Đã thêm. Tìm thấy ' + d.channels + ' kênh.';
                urlInput.value = '';
                nameInput.value = '';
                loadIptvSources();
              } else {
                statusEl.style.color = 'var(--red)';
                statusEl.textContent = 'Thất bại: ' + (d.error || 'Không tải được danh sách');
              }
            })
            .catch(err => {
              btn.disabled = false;
              btn.textContent = 'Tải và thêm nguồn';
              statusEl.style.color = 'var(--red)';
              statusEl.textContent = 'Lỗi kết nối: ' + err;
            });
        }

        function uploadIptvPlaylist() {
          const fileInput = document.getElementById('iptv-file');
          const nameInput = document.getElementById('iptv-file-name');
          const btn = document.getElementById('btn-upload-iptv');
          const statusEl = document.getElementById('iptv-file-status');

          if (!fileInput.files[0]) {
            alert('Hãy chọn file .m3u hoặc .m3u8');
            return;
          }

          const file = fileInput.files[0];
          const name = (nameInput.value || '').trim();

          btn.disabled = true;
          btn.textContent = 'Đang tải lên...';
          statusEl.style.display = 'block';
          statusEl.style.color = 'var(--accent)';
          statusEl.textContent = 'Đang gửi file lên máy...';

          const formData = new FormData();
          formData.append('file', file);
          if (name) formData.append('name', name);

          fetch('/api/iptv/upload', { method: 'POST', body: formData })
            .then(r => r.json())
            .then(d => {
              btn.disabled = false;
              btn.textContent = 'Tải lên';
              if (d.success) {
                statusEl.style.color = 'var(--green)';
                statusEl.textContent = 'Đã tải lên: ' + d.channels + ' kênh.';
                fileInput.value = '';
                nameInput.value = '';
                loadIptvSources();
              } else {
                statusEl.style.color = 'var(--red)';
                statusEl.textContent = 'Lỗi: ' + (d.error || 'Tải lên thất bại');
              }
            })
            .catch(err => {
              btn.disabled = false;
              btn.textContent = 'Tải lên';
              statusEl.style.color = 'var(--red)';
              statusEl.textContent = 'Lỗi kết nối: ' + err;
            });
        }

        loadIptvSources();
      </script>
    </div>

    <!-- TAB: HÌNH NỀN (screensaver slideshow) -->
    <div id="tab-saver" class="tab-content">
      <div class="card">
        <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 4px;">
          <h3 style="margin: 0;">Ảnh trình chiếu (Picture frame)</h3>
          <span id="saver-total-summary" style="font-size: 13px; font-weight: 600; color: var(--accent); background: rgba(0, 180, 216, 0.1); padding: 4px 10px; border-radius: 6px;">Đang tải...</span>
        </div>
        <p style="font-size: 12px; color: var(--text-muted); margin-bottom: 14px;">
          Ảnh trong thư mục <b>screensaver/photos</b> trên thẻ nhớ. Mở app Hình nền &gt; Picture frame trên máy để xem slideshow.
        </p>
        <div id="saver-photos-container" style="background: var(--card-alt); border-radius: 10px; border: 1px solid var(--border); overflow: hidden;">
        </div>
      </div>

      <div class="card">
        <h4 style="margin-bottom: 8px;">Tải ảnh lên (.jpg, .jpeg, .png)</h4>
        <p style="font-size: 12px; color: var(--text-muted); margin-bottom: 14px;">
          Kéo &amp; thả ảnh vào khung dưới, hoặc bấm để chọn nhiều file cùng lúc.
        </p>
        <div id="saver-dropzone" style="border: 2px dashed var(--border); border-radius: 10px; padding: 28px 16px; text-align: center; color: var(--text-muted); font-size: 13px; cursor: pointer; margin-bottom: 14px; transition: border-color 0.15s, background 0.15s;">
          Kéo &amp; thả ảnh vào đây<br>
          <span style="font-size: 12px;">hoặc bấm để chọn file</span>
          <div id="saver-pending" style="margin-top: 8px; font-size: 12px; color: var(--accent);"></div>
        </div>
        <input type="file" id="saver-file" accept=".jpg,.jpeg,.png" multiple style="display: none;">
        <button class="btn btn-primary" id="btn-upload-saver" onclick="uploadSaverPhotos()" style="width: 100%; padding: 10px; font-weight: 600;">Tải lên</button>
        <div id="saver-upload-status" style="margin-top: 10px; font-size: 12.5px; display: none;"></div>
      </div>

      <script>
        let saverPendingFiles = [];

        function saverDropzone() { return document.getElementById('saver-dropzone'); }

        function saverShowPending() {
          const el = document.getElementById('saver-pending');
          if (!el) return;
          el.textContent = saverPendingFiles.length > 0
            ? ('Đã chọn ' + saverPendingFiles.length + ' ảnh: ' + saverPendingFiles.slice(0, 3).map(f => f.name).join(', ') + (saverPendingFiles.length > 3 ? '...' : ''))
            : '';
        }

        function initSaverDropzone() {
          const dz = saverDropzone();
          const fi = document.getElementById('saver-file');
          if (!dz || !fi || dz.dataset.bound) return;
          dz.dataset.bound = '1';
          dz.addEventListener('click', () => fi.click());
          fi.addEventListener('change', () => {
            saverPendingFiles = Array.from(fi.files || []);
            saverShowPending();
          });
          ['dragenter', 'dragover'].forEach(ev => dz.addEventListener(ev, (e) => {
            e.preventDefault();
            dz.style.borderColor = 'var(--accent)';
            dz.style.background = 'rgba(0, 180, 216, 0.08)';
          }));
          ['dragleave', 'drop'].forEach(ev => dz.addEventListener(ev, (e) => {
            e.preventDefault();
            dz.style.borderColor = 'var(--border)';
            dz.style.background = 'transparent';
          }));
          dz.addEventListener('drop', (e) => {
            const files = Array.from((e.dataTransfer && e.dataTransfer.files) || []);
            saverPendingFiles = files.filter(f => /\.(jpe?g|png)$/i.test(f.name));
            saverShowPending();
          });
        }

        function loadSaverPhotos() {
          const container = document.getElementById('saver-photos-container');
          const summaryEl = document.getElementById('saver-total-summary');
          if (!container) return;
          fetch('/api/saver/photos')
            .then(r => r.json())
            .then(d => {
              if (!d.success || !d.photos || d.photos.length === 0) {
                container.innerHTML = '<div style="padding: 24px; text-align: center; color: var(--text-muted);">'
                  + '<b>Chưa có ảnh nào</b><br>'
                  + '<span style="font-size: 12px;">Kéo &amp; thả ảnh vào khung bên dưới để tải lên.</span></div>';
                if (summaryEl) summaryEl.textContent = '0 ảnh';
                return;
              }
              if (summaryEl) summaryEl.textContent = d.photos.length + ' ảnh';
              let html = '<table style="width: 100%; border-collapse: collapse; text-align: left; font-size: 13px;">'
                + '<thead style="background: rgba(255,255,255,0.03); border-bottom: 1px solid var(--border); color: var(--text-muted); font-size: 12px;">'
                + '<tr><th style="padding: 10px 14px;">TÊN FILE</th>'
                + '<th style="padding: 10px 14px; text-align: right;">DUNG LƯỢNG</th>'
                + '<th style="padding: 10px 14px; text-align: center;">THAO TÁC</th></tr>'
                + '</thead><tbody>';
              d.photos.forEach((p) => {
                html += '<tr style="border-bottom: 1px solid var(--border);">'
                  + '<td style="padding: 12px 14px; font-weight: 600; color: var(--text); font-size: 14px; word-break: break-all;">' + escapeHtml(p.file) + '</td>'
                  + '<td style="padding: 12px 14px; text-align: right; color: var(--text-muted);">' + formatBytes(p.size) + '</td>'
                  + '<td style="padding: 12px 14px; text-align: center;">'
                  + '<button class="btn btn-secondary" style="font-size: 12px; padding: 5px 12px; border-color: var(--red); color: var(--red);" onclick="deleteSaverPhoto(\'' + escapeHtml(p.file).replace(/'/g, "\\'") + '\')">Xóa</button>'
                  + '</td></tr>';
              });
              container.innerHTML = html + '</tbody></table>';
            })
            .catch(() => {
              if (summaryEl) summaryEl.textContent = 'Lỗi tải';
            });
        }

        function deleteSaverPhoto(file) {
          if (!confirm('Xóa ảnh "' + file + '" khỏi máy?')) return;
          fetch('/api/saver/delete?file=' + encodeURIComponent(file))
            .then(r => r.json())
            .then(d => {
              if (d.success) loadSaverPhotos();
              else alert('Không xóa được: ' + (d.error || 'lỗi không rõ'));
            })
            .catch(err => alert('Lỗi kết nối: ' + err));
        }

        async function uploadSaverPhotos() {
          const fileInput = document.getElementById('saver-file');
          const btn = document.getElementById('btn-upload-saver');
          const statusEl = document.getElementById('saver-upload-status');
          let files = saverPendingFiles.length > 0
            ? saverPendingFiles
            : Array.from((fileInput && fileInput.files) || []);
          files = files.filter(f => /\.(jpe?g|png)$/i.test(f.name));
          if (files.length === 0) {
            alert('Hãy chọn hoặc kéo thả ít nhất 1 ảnh .jpg / .png');
            return;
          }
          btn.disabled = true;
          statusEl.style.display = 'block';
          let ok = 0, fail = 0;
          for (let i = 0; i < files.length; i++) {
            statusEl.style.color = 'var(--accent)';
            statusEl.textContent = 'Đang gửi ' + (i + 1) + '/' + files.length + ': ' + files[i].name + '...';
            const formData = new FormData();
            formData.append('file', files[i]);
            try {
              const r = await fetch('/api/saver/upload', { method: 'POST', body: formData });
              const d = await r.json();
              if (d.success) ok++; else fail++;
            } catch (err) { fail++; }
          }
          btn.disabled = false;
          btn.textContent = 'Tải lên';
          saverPendingFiles = [];
          if (fileInput) fileInput.value = '';
          saverShowPending();
          statusEl.style.color = fail === 0 ? 'var(--green)' : 'var(--red)';
          statusEl.textContent = 'Xong: ' + ok + ' ảnh thành công' + (fail > 0 ? ', ' + fail + ' ảnh lỗi.' : '.');
          loadSaverPhotos();
        }

        initSaverDropzone();
        loadSaverPhotos();
      </script>
    </div>

    <!-- TAB 5: OTA UPDATE -->
    <div id="tab-ota" class="tab-content">
      <div class="card" style="max-width: 600px; margin: 0 auto;">
        <h3>Cập nhật RomCloud</h3>
        <p style="font-size: 13px; color: var(--text-muted); margin-bottom: 16px;">
          Phiên bản trên máy hiện tại: <b style="color: var(--accent);">v)HTML" +
      UpdateManager::instance().getCurrentVersion() + R"HTML(</b>
        </p>

        <div style="display: flex; gap: 10px; margin-bottom: 16px;">
          <button class="btn btn-primary" id="btn-ota-check" onclick="checkOtaUpdate()">Kiểm tra cập nhật</button>
          <button class="btn btn-primary" id="btn-ota-install" onclick="startOtaUpdate()" style="display: none; background: var(--green);">Cập nhật ngay</button>
        </div>

        <div id="ota-info-box" style="display: none; background: var(--card-alt); border-radius: 8px; padding: 14px; margin-bottom: 14px; font-size: 13px;">
          <div style="font-weight: 700; color: var(--accent); margin-bottom: 4px;" id="ota-version-title"></div>
          <div style="color: var(--text-dim); font-size: 12px; margin-bottom: 8px;" id="ota-release-date"></div>
          <div id="ota-changelog" style="color: #cbd5e1; line-height: 1.5; white-space: pre-wrap;"></div>
        </div>

        <div id="ota-progress-box" style="display: none; margin-top: 10px;">
          <div class="progress-track"><div class="progress-fill" id="ota-progress-fill"></div></div>
          <div style="font-size: 12px; color: var(--text-dim);" id="ota-status-txt">Đang tải bản cập nhật...</div>
        </div>
      </div>

  <!-- MODAL: GAME DETAIL & METADATA -->
  <div id="game-detail-modal" style="display: none; position: fixed; inset: 0; background: rgba(0,0,0,0.8); z-index: 1000; align-items: center; justify-content: center; padding: 20px; backdrop-filter: blur(4px);">
    <div style="background: var(--card-bg); border: 1px solid var(--border); border-radius: 12px; max-width: 680px; width: 100%; max-height: 90vh; overflow-y: auto; box-shadow: 0 20px 25px -5px rgba(0, 0, 0, 0.5);">
      <div style="padding: 16px 20px; border-bottom: 1px solid var(--border); display: flex; justify-content: space-between; align-items: center;">
        <h3 id="modal-game-title" style="margin: 0; font-size: 17px; color: #fff; font-weight: 700;">Chi tiết game</h3>
        <button type="button" class="btn btn-secondary" onclick="closeGameModal()" style="padding: 4px 10px; font-size: 14px;">Đóng</button>
      </div>
      <div style="padding: 20px; display: flex; gap: 20px; flex-wrap: wrap;">
        <!-- Left: Cover Art -->
        <div style="flex: 0 0 160px; text-align: center;">
          <div id="modal-cover-wrap" style="width: 160px; height: 210px; background: var(--bg); border: 1px solid var(--border); border-radius: 8px; overflow: hidden; display: flex; align-items: center; justify-content: center;">
            <img id="modal-cover-img" src="" alt="Cover" style="width: 100%; height: 100%; object-fit: contain; display: none;">
            <div id="modal-cover-placeholder" style="font-size: 14px; color: var(--text-dim);">Chưa có bìa</div>
          </div>
          <span id="modal-sys-tag" class="sys-tag sys-default" style="margin-top: 10px; display: inline-block;">GBA</span>
        </div>
        <!-- Right: Metadata & Info -->
        <div style="flex: 1; min-width: 260px;">
          <div style="font-size: 12px; color: var(--text-dim); font-family: monospace; margin-bottom: 10px;" id="modal-game-filename">filename.gba</div>
          <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-bottom: 14px; font-size: 12px;">
            <div style="background: var(--card-alt); padding: 8px 10px; border-radius: 6px;">
              <span style="color: var(--text-dim); display: block; font-size: 11px;">Năm phát hành</span>
              <b id="modal-release-year" style="color: #fff;">--</b>
            </div>
            <div style="background: var(--card-alt); padding: 8px 10px; border-radius: 6px;">
              <span style="color: var(--text-dim); display: block; font-size: 11px;">Thể loại</span>
              <b id="modal-genre" style="color: #38bdf8;">--</b>
            </div>
            <div style="background: var(--card-alt); padding: 8px 10px; border-radius: 6px;">
              <span style="color: var(--text-dim); display: block; font-size: 11px;">Nhà phát triển</span>
              <b id="modal-developer" style="color: #cbd5e1;">--</b>
            </div>
            <div style="background: var(--card-alt); padding: 8px 10px; border-radius: 6px;">
              <span style="color: var(--text-dim); display: block; font-size: 11px;">Dung lượng / Trạng thái</span>
              <b id="modal-status" style="color: #fff;">--</b>
            </div>
          </div>
          <div style="margin-bottom: 6px; font-size: 12px; font-weight: 600; color: var(--text-muted);">Giới thiệu:</div>
          <div id="modal-description" style="font-size: 12px; color: #cbd5e1; line-height: 1.6; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; padding: 10px 12px; max-height: 140px; overflow-y: auto;">
            Chưa có giới thiệu. Bấm "Lấy lại bìa và thông tin" ở dưới để tự tìm.
          </div>

          <!-- MANUAL SCRAPER REFINEMENT SEARCH (LIKE SCRAPE-EDIT) -->
          <div style="margin-top: 14px; border-top: 1px solid var(--border); padding-top: 10px;">
            <div style="font-size: 11.5px; font-weight: 600; color: #38bdf8; margin-bottom: 6px;">Tìm bìa khác (Libretro):</div>
            <div style="display: flex; gap: 6px;">
              <input type="text" id="modal-search-query" placeholder="Nhập tên game để tìm..." style="flex: 1; padding: 6px 10px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 12px;">
              <button type="button" class="btn btn-secondary" onclick="searchModalScraperCandidates()" style="font-size: 11px; padding: 6px 12px; border-color: #0284c7; color: #38bdf8;">Tìm</button>
            </div>
            <div id="modal-candidates-list" style="margin-top: 8px; max-height: 180px; overflow-y: auto; display: none; flex-direction: column; gap: 6px;"></div>
          </div>
        </div>
      </div>
      <div style="padding: 12px 20px; border-top: 1px solid var(--border); display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 8px;">
        <div style="display: flex; gap: 8px; flex-wrap: wrap;">
          <button type="button" class="btn btn-primary" id="btn-modal-scrape" onclick="scrapeCurrentModalGame()" style="background: var(--purple);">Lấy lại bìa và thông tin</button>
          <button type="button" class="btn btn-primary" id="btn-modal-backup" onclick="uploadCurrentModalGame()" style="background: #7c3aed; border-color: #6d28d9; display: none;">Sao lưu lên Drive</button>
        </div>
        <button type="button" class="btn btn-secondary" onclick="closeGameModal()">Đóng</button>
      </div>
    </div>
  </div>

  <!-- MODAL: BÁC SĨ ROM (AUTO-FIX MISPLACED ROMS) -->
  <div id="doctor-modal" style="display: none; position: fixed; inset: 0; background: rgba(0,0,0,0.85); z-index: 1050; align-items: center; justify-content: center; padding: 20px; backdrop-filter: blur(5px);">
    <div style="background: var(--card-bg); border: 1px solid #10b981; border-radius: 12px; width: 100%; max-width: 680px; max-height: 85vh; display: flex; flex-direction: column; overflow: hidden; box-shadow: 0 20px 40px rgba(0,0,0,0.6);">
      <div style="padding: 14px 20px; border-bottom: 1px solid var(--border); display: flex; justify-content: space-between; align-items: center; background: rgba(16,185,129,0.1);">
        <div style="display: flex; align-items: center; gap: 10px;">
          <div>
            <h3 style="margin: 0; font-size: 16px; color: #fff; font-weight: 700;">Sửa ROM sai thư mục</h3>
            <span style="font-size: 11.5px; color: var(--text-dim);">Tìm ROM đặt sai thư mục và chuyển về đúng hệ máy</span>
          </div>
        </div>
        <button type="button" class="btn btn-secondary" onclick="closeDoctorModal()" style="padding: 4px 10px; font-size: 14px;">Đóng</button>
      </div>

      <div style="padding: 16px 20px; overflow-y: auto; flex: 1;">
        <!-- Status / Audit Card -->
        <div id="doctor-loading" style="display: none; text-align: center; padding: 30px;">
          <div style="font-size: 28px; animation: spin 1.5s linear infinite; display: inline-block;">…</div>
          <div style="margin-top: 10px; color: #e2e8f0; font-size: 14px;">Đang quét thư mục game trên thẻ...</div>
        </div>

        <div id="doctor-results" style="display: block;">
          <div id="doctor-summary-box" style="padding: 12px 14px; border-radius: 8px; margin-bottom: 14px; font-size: 13px; line-height: 1.5;"></div>

          <!-- List of detected misplaced ROMs -->
          <div id="doctor-items-wrap" style="max-height: 280px; overflow-y: auto; display: flex; flex-direction: column; gap: 8px;"></div>
        </div>

        <!-- Drag & Drop Upload Zone -->
        <div style="margin-top: 16px; padding: 14px; border: 2px dashed rgba(16,185,129,0.4); border-radius: 8px; background: rgba(16,185,129,0.04); text-align: center;">
          <div style="font-size: 13px; font-weight: 600; color: #34d399; margin-bottom: 4px;">Thêm ROM mới (từ máy tính / điện thoại)</div>
          <div style="font-size: 11.5px; color: var(--text-dim); margin-bottom: 10px;">Thả file ROM vào đây, máy tự nhận diện và đặt vào đúng thư mục.</div>
          <input type="file" id="doctor-file-input" style="display: none;" onchange="uploadAutoRomFile(this.files[0])">
          <button type="button" class="btn btn-secondary" onclick="document.getElementById('doctor-file-input').click()" style="font-size: 12px; padding: 6px 14px; border-color: #10b981; color: #34d399;">Chọn file ROM</button>
          <div id="doctor-upload-status" style="margin-top: 8px; font-size: 12px; display: none;"></div>
        </div>
      </div>

      <div style="padding: 12px 20px; border-top: 1px solid var(--border); display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.2);">
        <button type="button" class="btn btn-secondary" onclick="runDoctorAudit()">Quét lại</button>
        <div style="display: flex; gap: 8px;">
          <button type="button" class="btn btn-primary" id="btn-doctor-fix" onclick="executeDoctorFix()" style="background: #10b981; border-color: #059669; font-weight: 600;">Chuyển về đúng thư mục</button>
          <button type="button" class="btn btn-secondary" onclick="closeDoctorModal()">Đóng</button>
        </div>
      </div>
    </div>
  </div>

    <div id="tab-cal" class="tab-content">
      <div class="card" style="max-width: 720px; margin: 0 auto;">
        <h3>Lịch từ điện thoại</h3>
        <p style="font-size: 12.5px; color: var(--text-muted); line-height: 1.6; margin-bottom: 14px;">
          Dán link lịch (Google Calendar: Cài đặt → Tích hợp → Địa chỉ bí mật iCal; iCloud: chia sẻ lịch → link công khai). Máy tự cập nhật mỗi ngày.
          Cũng có thể dán nội dung file <b>.ics</b> hoặc thêm việc ở dưới.
        </p>
        <div style="display: flex; gap: 8px; margin-bottom: 10px;">
          <input type="text" id="cal-url-name" placeholder="Tên (vd Lịch Google)" style="flex: 0 0 180px; padding: 8px 10px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 12px;">
          <input type="text" id="cal-url" placeholder="https://.../basic.ics" style="flex: 1; padding: 8px 10px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 12px;">
          <button class="btn btn-primary" onclick="calAddUrl()">Thêm link</button>
        </div>
        <div style="margin-bottom: 10px;">
          <label style="font-size: 11.5px; color: var(--text-dim); display: block; margin-bottom: 4px;">Dán nội dung file .ics:</label>
          <textarea id="cal-ics" rows="3" placeholder="BEGIN:VCALENDAR..." style="width: 100%; padding: 8px 10px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 11px; font-family: monospace;"></textarea>
          <div style="display: flex; gap: 8px; margin-top: 6px;">
            <input type="text" id="cal-ics-name" placeholder="Tên nguồn" style="flex: 0 0 180px; padding: 8px 10px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 12px;">
            <button class="btn btn-secondary" onclick="calImport()">Nhập ICS</button>
            <button class="btn btn-secondary" onclick="calRefresh()">Làm mới link</button>
          </div>
        </div>
        <div style="display: flex; gap: 8px; margin-bottom: 14px; align-items: end;">
          <div><label style="font-size: 11px; color: var(--text-dim); display: block; margin-bottom: 3px;">Ngày</label>
          <input type="date" id="cal-m-date" style="padding: 7px 8px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 12px;"></div>
          <div><label style="font-size: 11px; color: var(--text-dim); display: block; margin-bottom: 3px;">Giờ (trống = cả ngày)</label>
          <input type="time" id="cal-m-time" style="padding: 7px 8px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 12px;"></div>
          <div style="flex: 1;"><label style="font-size: 11px; color: var(--text-dim); display: block; margin-bottom: 3px;">Việc</label>
          <input type="text" id="cal-m-title" placeholder="Tiêu đề" style="width: 100%; padding: 8px 10px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 12px;"></div>
          <button class="btn btn-primary" onclick="calAddManual()">Thêm</button>
        </div>
        <h4 style="font-size: 13px; margin: 10px 0 6px;">Nguồn lịch</h4>
        <div id="cal-sources" style="font-size: 12px; display: flex; flex-direction: column; gap: 6px; margin-bottom: 12px;"></div>
        <h4 style="font-size: 13px; margin: 10px 0 6px;">Sự kiện sắp tới</h4>
        <div id="cal-events" style="font-size: 12px; display: flex; flex-direction: column; gap: 6px; max-height: 260px; overflow-y: auto;"></div>
      </div>
    </div>
    <div id="tab-alarm" class="tab-content">
      <div class="card" style="max-width: 720px; margin: 0 auto;">
        <h3>Báo thức</h3>
        <p style="font-size: 12.5px; color: var(--text-muted); line-height: 1.6; margin-bottom: 14px;">
          Thêm báo thức tại đây cho nhanh. Tối đa 10 báo thức.
          Kiểu báo: 0 = chuông + rung, 1 = chuông, 2 = rung.
        </p>
        <div style="display: flex; gap: 8px; margin-bottom: 14px; align-items: end;">
          <div><label style="font-size: 11px; color: var(--text-dim); display: block; margin-bottom: 3px;">Giờ</label>
          <input type="number" id="alarm-hour" min="0" max="23" value="7" style="width: 80px; padding: 8px 10px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 13px;"></div>
          <div><label style="font-size: 11px; color: var(--text-dim); display: block; margin-bottom: 3px;">Phút</label>
          <input type="number" id="alarm-min" min="0" max="59" value="0" style="width: 80px; padding: 8px 10px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 13px;"></div>
          <button class="btn btn-primary" onclick="alarmAdd()">Thêm</button>
        </div>
        <div id="alarm-list" style="font-size: 13px; display: flex; flex-direction: column; gap: 8px;"></div>
      </div>
    </div>
    <div id="tab-watch" class="tab-content">
      <div class="card" style="max-width: 720px; margin: 0 auto;">
        <h3>Theo dõi giá (coin và chứng khoán)</h3>
        <p style="font-size: 12.5px; color: var(--text-muted); line-height: 1.6; margin-bottom: 14px;">
          Tối đa 4 mã, hiện trên mục Thị trường của máy.
          Coin: BTC, ETH, SOL... Cổ phiếu VN: mã sàn HOSE như VCB, HPG, FPT.
        </p>
        <div style="display: flex; gap: 8px; margin-bottom: 14px; align-items: end;">
          <div style="flex: 1;"><label style="font-size: 11px; color: var(--text-dim); display: block; margin-bottom: 3px;">Các mã (cách nhau dấu phẩy)</label>
          <input type="text" id="watch-symbols" placeholder="BTC,ETH,VCB,HPG" style="width: 100%; padding: 8px 10px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 13px; text-transform: uppercase;"></div>
          <button class="btn btn-primary" onclick="watchSave()">Lưu</button>
        </div>
        <div id="watch-list" style="font-size: 13px; display: flex; gap: 8px; flex-wrap: wrap;"></div>
      </div>
    </div>

  <!-- FLOATING ACTIVE UPLOAD CARD -->
  <div id="active-upload-floating">
    <div style="display: flex; justify-content: space-between; align-items: flex-start; margin-bottom: 6px;">
      <div>
        <span class="badge" style="background:#9333ea;color:#fff;font-size:10px;padding:2px 6px;">ĐANG SAO LƯU</span>
        <span style="font-size: 13.5px; font-weight: 700; margin-left: 6px; color:#fff;" id="upload-float-title">Game Title</span>
      </div>
      <button class="btn btn-secondary" onclick="cancelActiveUpload()" style="padding: 2px 8px; font-size: 11px;">Hủy</button>
    </div>
    <div style="font-size: 11px; color: #cbd5e1; font-family: monospace; margin-bottom: 4px; overflow:hidden; text-overflow:ellipsis; white-space:nowrap;" id="upload-float-file">filename.bin</div>
    <div class="progress-track" style="margin: 6px 0; height: 8px; background: rgba(0,0,0,0.4);">
      <div class="progress-fill" id="upload-float-progress-fill" style="background: var(--purple); width: 0%;"></div>
    </div>
    <div style="display: flex; justify-content: space-between; font-size: 11px; color: #e2e8f0;">
      <span id="upload-float-speed">0 KB/s</span>
      <span id="upload-float-percent">0%</span>
      <span id="upload-float-bytes">0 / 0 MB</span>
    </div>
  </div>

  <!-- FLOATING ACTIVE AUTO-SCRAPE CARD -->
  <div id="active-scrape-floating" style="display: none; position: fixed; bottom: 20px; left: 20px; z-index: 9999; background: rgba(15, 23, 42, 0.95); border: 1px solid #0284c7; border-radius: 10px; padding: 12px 16px; width: 340px; box-shadow: 0 10px 25px rgba(0,0,0,0.6); backdrop-filter: blur(8px);">
    <div style="display: flex; justify-content: space-between; align-items: flex-start; margin-bottom: 6px;">
      <div>
        <span class="badge" style="background:#0284c7;color:#fff;font-size:10px;padding:2px 6px;">ĐANG LẤY THÔNG TIN</span>
        <span style="font-size: 13px; font-weight: 700; margin-left: 6px; color:#fff;" id="scrape-float-title">Game Title</span>
      </div>
      <button class="btn btn-secondary" onclick="cancelAutoScrape()" style="padding: 2px 8px; font-size: 11px;">Hủy</button>
    </div>
    <div style="font-size: 11px; color: #94a3b8; margin-bottom: 4px;" id="scrape-float-sys">Hệ máy: SYS</div>
    <div class="progress-track" style="margin: 6px 0; height: 8px; background: rgba(0,0,0,0.4);">
      <div class="progress-fill" id="scrape-float-progress-fill" style="background: #0284c7; width: 0%;"></div>
    </div>
    <div style="display: flex; justify-content: space-between; font-size: 11px; color: #e2e8f0;">
      <span id="scrape-float-count">0 / 0</span>
      <span id="scrape-float-pct">0%</span>
    </div>
  </div>

  <div id="toast">Thông báo</div>

  <script>
    let currentTab = 'tab-roms';
    let isDriveLinked = false;
    let canUploadGlobal = false;
    let currentViewMode = localStorage.getItem('romcloud_view_mode') || 'grid';
    let currentSystemId = 0;
    let currentStateFilter = -1;
    let currentSearch = '';
    let currentPage = 1;
    let pageSize = parseInt(localStorage.getItem('romcloud_page_size')) || 48;
    let maxPagesCache = 1;
    let searchDebounceTimer = null;
    let dlPollTimer = null;
    let uploadPollTimer = null;
    let scrapePollTimer = null;
    let systemsCache = [];
    let lastLoadedGames = null;

    // Init
    window.addEventListener('DOMContentLoaded', () => {
      const pSel = document.getElementById('select-page-size');
      if (pSel) pSel.value = pageSize;
      setViewMode(currentViewMode, false);
      loadSystems();
      loadStorageInfo();
      loadGames();
      startPolling();
      pollUploadProgress();
      pollScrapeProgress();
      setTimeout(() => checkOtaUpdate(true), 1200);
      setInterval(() => checkOtaUpdate(true), 180000);
    });

    function setViewMode(mode, reload = true) {
      currentViewMode = mode;
      localStorage.setItem('romcloud_view_mode', mode);
      const btnList = document.getElementById('btn-view-list');
      const btnGrid = document.getElementById('btn-view-grid');
      const tableWrap = document.getElementById('game-table-wrap');
      const gridWrap = document.getElementById('game-grid-wrap');
      if (btnList) btnList.classList.toggle('active', mode === 'list');
      if (btnGrid) btnGrid.classList.toggle('active', mode === 'grid');
      if (tableWrap) tableWrap.style.display = (mode === 'list') ? 'block' : 'none';
      if (gridWrap) gridWrap.style.display = (mode === 'grid') ? 'grid' : 'none';
      if (reload && lastLoadedGames) {
        renderGames(lastLoadedGames.games, lastLoadedGames.total);
      }
    }

    function changePageSize(sz) {
      pageSize = parseInt(sz) || 48;
      localStorage.setItem('romcloud_page_size', pageSize);
      currentPage = 1;
      loadGames();
    }

    function goToPage(p) {
      if (isNaN(p) || p < 1) p = 1;
      if (p > maxPagesCache) p = maxPagesCache;
      currentPage = p;
      loadGames();
    }

    function showToast(msg) {
      const t = document.getElementById('toast');
      t.textContent = msg;
      t.classList.add('show');
      setTimeout(() => t.classList.remove('show'), 3000);
    }

    function switchTab(tabId) {
      document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
      document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
      
      const targetBtn = Array.from(document.querySelectorAll('.tab-btn')).find(b => b.getAttribute('onclick').includes(tabId));
      if (targetBtn) targetBtn.classList.add('active');
      
      const targetContent = document.getElementById(tabId);
      if (targetContent) targetContent.classList.add('active');
      currentTab = tabId;

      if (tabId === 'tab-queue') updateDownloadQueueUI();
      if (tabId === 'tab-storage') {
        loadStorageInfo();
        }
      if (tabId === 'tab-iptv' && typeof loadIptvSources === 'function') {
        loadIptvSources();
      }
      if (tabId === 'tab-saver' && typeof loadSaverPhotos === 'function') {
        loadSaverPhotos();
      }
      if (tabId === 'tab-cal' && typeof loadCalSources === 'function') {
        loadCalSources();
        loadCalEvents();
      }
      if (tabId === 'tab-alarm') loadAlarms();
      if (tabId === 'tab-watch') loadWatch();
    }

    async function loadSystems() {
      try {
        const res = await fetch('/api/systems');
        systemsCache = await res.json();
        renderSystemPills();
      } catch (e) {}
    }

    function renderSystemPills() {
      const c = document.getElementById('system-pills-container');
      if (!isDriveLinked) {
        c.innerHTML = '<span style="font-size: 12px; color: var(--text-dim); padding: 4px 8px;">(Chưa kết nối Google Drive)</span>';
        return;
      }
      let html = `<button class="pill-btn ${currentSystemId === 0 ? 'active' : ''}" onclick="setSystemFilter(0)">Tất cả</button>`;
      for (const s of systemsCache) {
        if (s.cloud_count === 0 && s.local_count === 0) continue;
        const active = (currentSystemId === s.id) ? 'active' : '';
        html += `<button class="pill-btn ${active}" onclick="setSystemFilter(${s.id})">${s.code} (${s.local_count + s.cloud_count})</button>`;
      }
      c.innerHTML = html;
    }

    function setSystemFilter(sysId) {
      currentSystemId = sysId;
      currentPage = 1;
      renderSystemPills();
      loadGames();
    }

    function setStateFilter(state) {
      currentStateFilter = state;
      currentPage = 1;
      document.getElementById('filter-state-all').classList.toggle('active', state === -1);
      document.getElementById('filter-state-local').classList.toggle('active', state === 1);
      document.getElementById('filter-state-cloud').classList.toggle('active', state === 0);
      loadGames();
    }

    function onSearchInput() {
      const val = document.getElementById('rom-search').value.trim();
      document.getElementById('search-clear-btn').style.display = val ? 'block' : 'none';
      clearTimeout(searchDebounceTimer);
      searchDebounceTimer = setTimeout(() => {
        currentSearch = val;
        currentPage = 1;
        loadGames();
      }, 250);
    }

    function clearSearch() {
      document.getElementById('rom-search').value = '';
      document.getElementById('search-clear-btn').style.display = 'none';
      currentSearch = '';
      currentPage = 1;
      loadGames();
    }

    async function loadGames() {
      const tbody = document.getElementById('game-table-body');
      const gridWrap = document.getElementById('game-grid-wrap');
      if (tbody) tbody.innerHTML = `<tr><td colspan="5" style="text-align: center; padding: 25px; color: var(--text-dim);">Đang tải danh sách game...</td></tr>`;
      if (gridWrap) gridWrap.innerHTML = `<div style="grid-column: 1 / -1; text-align: center; padding: 40px; color: var(--text-dim);">Đang tải danh sách game...</div>`;

      const offset = (currentPage - 1) * pageSize;
      const url = `/api/games?system_id=${currentSystemId}&state=${currentStateFilter}&q=${encodeURIComponent(currentSearch)}&limit=${pageSize}&offset=${offset}`;

      try {
        const res = await fetch(url);
        const data = await res.json();
        lastLoadedGames = { games: data.games, total: data.total };
        renderGames(data.games, data.total);
      } catch (e) {
        if (tbody) tbody.innerHTML = `<tr><td colspan="5" style="text-align: center; padding: 25px; color: var(--red);">Lỗi tải dữ liệu. Thử lại nhé.</td></tr>`;
        if (gridWrap) gridWrap.innerHTML = `<div style="grid-column: 1 / -1; text-align: center; padding: 40px; color: var(--red);">Lỗi tải dữ liệu. Thử lại nhé.</div>`;
      }
    }

    function renderGames(games, total) {
      const tbody = document.getElementById('game-table-body');
      const gridWrap = document.getElementById('game-grid-wrap');
      const countSpan = document.getElementById('game-results-count');
      const jumpInput = document.getElementById('input-jump-page');
      const totalInd = document.getElementById('page-total-indicator');
      const btnFirst = document.getElementById('btn-first-page');
      const btnPrev = document.getElementById('btn-prev-page');
      const btnNext = document.getElementById('btn-next-page');
      const btnLast = document.getElementById('btn-last-page');

      if (!isDriveLinked) {
        const unlinkedMsg = `
          <div style="font-size: 16px; font-weight: 700; color: #fef3c7; margin-bottom: 6px;">Chưa kết nối Google Drive</div>
          <div style="font-size: 13px; color: var(--text-muted); margin-bottom: 18px;">Kết nối Google Drive để xem danh sách game.</div>
          <button class="btn btn-primary" onclick="switchTab('tab-storage')">Kết nối Google Drive</button>
        `;
        if (tbody) tbody.innerHTML = `<tr><td colspan="5" style="text-align: center; padding: 50px 20px; color: var(--text-dim);">${unlinkedMsg}</td></tr>`;
        if (gridWrap) gridWrap.innerHTML = `<div style="grid-column: 1 / -1; text-align: center; padding: 60px 20px; color: var(--text-dim);">${unlinkedMsg}</div>`;
        if (countSpan) countSpan.innerHTML = `Chưa kết nối Google Drive (<b>0</b> game)`;
        if (jumpInput) { jumpInput.value = 1; jumpInput.max = 1; }
        if (totalInd) totalInd.textContent = `/ 1`;
        if (btnFirst) btnFirst.disabled = true;
        if (btnPrev) btnPrev.disabled = true;
        if (btnNext) btnNext.disabled = true;
        if (btnLast) btnLast.disabled = true;
        return;
      }

      const maxPages = Math.ceil(total / pageSize) || 1;
      maxPagesCache = maxPages;
      if (countSpan) countSpan.innerHTML = `Tìm thấy <b>${total.toLocaleString()}</b> game`;
      if (jumpInput) { jumpInput.value = currentPage; jumpInput.max = maxPages; }
      if (totalInd) totalInd.textContent = `/ ${maxPages}`;
      if (btnFirst) btnFirst.disabled = (currentPage <= 1);
      if (btnPrev) btnPrev.disabled = (currentPage <= 1);
      if (btnNext) btnNext.disabled = (currentPage >= maxPages);
      if (btnLast) btnLast.disabled = (currentPage >= maxPages);

      if (!games || games.length === 0) {
        if (tbody) tbody.innerHTML = `<tr><td colspan="5" style="text-align: center; padding: 30px; color: var(--text-dim);">Không có game nào khớp bộ lọc.</td></tr>`;
        if (gridWrap) gridWrap.innerHTML = `<div style="grid-column: 1 / -1; text-align: center; padding: 40px 20px; color: var(--text-dim);">Không có game nào khớp bộ lọc.</div>`;
        return;
      }

      let tableHtml = '';
      let gridHtml = '';
      for (const g of games) {
        const sysClass = 'sys-' + (g.sys_code || 'default');
        let statusBadge = '';
        let statusPillClass = '';
        let statusPillText = '';
        const detailBtn = `<button class="btn btn-secondary" onclick="openGameModal(${g.id})" title="Xem chi tiết game">Chi tiết</button>`;
        const scrapeBtn = `<button class="btn btn-secondary" onclick="scrapeBoxart(${g.id})" title="Lấy ảnh bìa và thông tin game">Lấy bìa</button>`;
        let tableActions = '';
        let gridActions = '';

        if (g.local_state === 1) {
          statusBadge = `<span class="status-badge status-local" title="Đã có trên thẻ nhớ">Trên thẻ SD</span>`;
          statusPillClass = 'status-local';
          statusPillText = 'Trên thẻ SD';
          const backupBtn = `<button class="btn btn-primary" onclick="uploadGameToDrive(${g.id}, '${escapeHtml(g.title)}')" style="background:var(--purple);border-color:#9333ea;" title="Sao lưu game này lên Drive của bạn (thư mục Roms)">Sao lưu</button>`;
          tableActions = `
            ${backupBtn}
            ${detailBtn}
            ${scrapeBtn}
            <button class="btn btn-danger" onclick="deleteRom(${g.id}, '${escapeHtml(g.title)}')">Xóa</button>
          `;
          gridActions = `
            ${backupBtn}
            ${detailBtn}
            ${scrapeBtn}
            <button class="btn btn-danger" onclick="deleteRom(${g.id}, '${escapeHtml(g.title)}')">Xóa</button>
          `;
        } else if (g.in_queue) {
          statusBadge = `<span class="status-badge status-queue" title="Đang chờ tải về máy">Đang tải về</span>`;
          statusPillClass = 'status-queue';
          statusPillText = 'Đang tải';
          tableActions = `${detailBtn} ${scrapeBtn} <button class="btn btn-secondary" onclick="removeFromQueue(${g.id})">Bỏ</button>`;
          gridActions = `${detailBtn} ${scrapeBtn} <button class="btn btn-secondary" onclick="removeFromQueue(${g.id})">Bỏ</button>`;
        } else {
          statusBadge = `<span class="status-badge status-cloud" title="Trên kho Drive chung, chưa tải về thẻ">Kho chung</span>`;
          statusPillClass = 'status-cloud';
          statusPillText = 'Kho chung';
          tableActions = `${detailBtn} ${scrapeBtn} <button class="btn btn-primary" onclick="downloadGame(${g.id})" title="Tải game này về thẻ nhớ">Tải về máy</button>`;
          gridActions = `${detailBtn} ${scrapeBtn} <button class="btn btn-primary" onclick="downloadGame(${g.id})" title="Tải game này về thẻ nhớ">Tải về máy</button>`;
        }

        let metaBadges = '';
        if (g.release_year) metaBadges += `<span class="badge" style="background:#334155;color:#94a3b8;font-size:10px;padding:2px 6px;margin-right:4px;">${escapeHtml(g.release_year)}</span>`;
        if (g.genre) metaBadges += `<span class="badge" style="background:#1e293b;border:1px solid #475569;color:#38bdf8;font-size:10px;padding:2px 6px;">${escapeHtml(g.genre)}</span>`;

        let coverHtml = '';
        if (g.has_cover) {
          coverHtml = `<img class="card-cover-img" src="/api/cover?game_id=${g.id}" alt="${escapeHtml(g.title)}" loading="lazy" onerror="this.onerror=null;this.parentElement.innerHTML='<div class=\\'card-cover-placeholder\\'><div class=\\'card-cover-icon\\'>ROM</div><span style=\\'font-size:11px;\\'>${escapeHtml(g.sys_code || 'GAME')}</span></div>';">`;
        } else {
          coverHtml = `
            <div class="card-cover-placeholder">
              <div class="card-cover-icon">ROM</div>
              <span style="font-size: 11px; font-weight: 600; letter-spacing: 0.5px;">${escapeHtml(g.sys_code || 'GAME')}</span>
            </div>
          `;
        }

        tableHtml += `
          <tr>
            <td><span class="sys-tag ${sysClass}">${escapeHtml(g.sys_code || 'GAME')}</span></td>
            <td>
              <span class="game-title" style="cursor: pointer;" onclick="openGameModal(${g.id})">${escapeHtml(g.title)}</span>
              <span class="game-file">${escapeHtml(g.filename)}</span>
              ${metaBadges ? `<div style="margin-top: 4px;">${metaBadges}</div>` : ''}
            </td>
            <td>${g.size_str || '0 B'}</td>
            <td>${statusBadge}</td>
            <td style="text-align: right;">${tableActions}</td>
          </tr>
        `;

        gridHtml += `
          <div class="game-card">
            <div class="card-cover-wrap" style="cursor: pointer;" onclick="openGameModal(${g.id})">
              <span class="sys-tag ${sysClass} card-sys-tag">${escapeHtml(g.sys_code || 'GAME')}</span>
              <span class="card-status-pill ${statusPillClass}">${statusPillText}</span>
              ${coverHtml}
            </div>
            <div class="card-body">
              <div>
                <div class="card-title" title="${escapeHtml(g.title)}" style="cursor: pointer;" onclick="openGameModal(${g.id})">${escapeHtml(g.title)}</div>
                <div class="card-file" title="${escapeHtml(g.filename)}">${escapeHtml(g.filename)}</div>
                <div class="card-meta">
                  <span>${g.size_str || '0 B'}</span>
                  ${g.release_year ? `<span style="margin-left: 6px; color: #94a3b8;">${escapeHtml(g.release_year)}</span>` : ''}
                </div>
                ${g.genre ? `<div style="font-size: 11px; color: #38bdf8; margin-top: 3px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;">${escapeHtml(g.genre)}</div>` : ''}
              </div>
              <div class="card-actions">
                ${gridActions}
              </div>
            </div>
          </div>
        `;
      }

      if (tbody) tbody.innerHTML = tableHtml;
      if (gridWrap) gridWrap.innerHTML = gridHtml;
    }

    function changePage(delta) {
      goToPage(currentPage + delta);
    }

    function escapeHtml(s) {
      if (!s) return '';
      return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
    }

    // Modal Actions
    let currentModalGameId = 0;
    async function openGameModal(gameId) {
      currentModalGameId = gameId;
      const modal = document.getElementById('game-detail-modal');
      if (!modal) return;
      modal.style.display = 'flex';

      document.getElementById('modal-game-title').textContent = 'Đang tải thông tin...';
      document.getElementById('modal-game-filename').textContent = '';
      document.getElementById('modal-release-year').textContent = 'Đang tải...';
      document.getElementById('modal-genre').textContent = 'Đang tải...';
      document.getElementById('modal-developer').textContent = 'Đang tải...';
      document.getElementById('modal-status').textContent = 'Đang tải...';
      document.getElementById('modal-description').textContent = 'Đang tải thông tin tóm tắt và cốt truyện...';
      document.getElementById('modal-cover-img').style.display = 'none';
      document.getElementById('modal-cover-placeholder').style.display = 'block';

      try {
        const res = await fetch(`/api/game_detail?game_id=${gameId}`);
        const g = await res.json();
        if (g.error) {
          showToast('Không tìm thấy thông tin game.');
          closeGameModal();
          return;
        }

        document.getElementById('modal-game-title').textContent = g.title || 'Game';
        document.getElementById('modal-game-filename').textContent = g.filename || '';
        document.getElementById('modal-sys-tag').textContent = g.sys_code || 'GAME';
        document.getElementById('modal-release-year').textContent = g.release_year || 'Chưa rõ';
        document.getElementById('modal-genre').textContent = g.genre || 'Chưa phân loại';
        document.getElementById('modal-developer').textContent = g.developer || 'Chưa rõ';
        document.getElementById('modal-status').innerHTML = `${g.size_str || '0 B'} • ${g.local_state === 1 ? '<span style="color:var(--green);">Đã tải về máy</span>' : '<span style="color:var(--accent);">Trên Drive</span>'}`;
        document.getElementById('modal-description').textContent = g.description || 'Chưa có giới thiệu. Bấm "Lấy lại bìa và thông tin" để tự tìm.';

        const img = document.getElementById('modal-cover-img');
        const placeholder = document.getElementById('modal-cover-placeholder');
        if (g.has_cover) {
          img.src = `/api/cover?game_id=${g.id}&t=${Date.now()}`;
          img.style.display = 'block';
          placeholder.style.display = 'none';
        } else {
          img.style.display = 'none';
          placeholder.style.display = 'block';
        }

        const backupModalBtn = document.getElementById('btn-modal-backup');
        if (backupModalBtn) {
          backupModalBtn.style.display = (g.local_state === 1) ? 'inline-block' : 'none';
        }

        const sQuery = document.getElementById('modal-search-query');
        if (sQuery) sQuery.value = g.title || g.filename || '';
        const cList = document.getElementById('modal-candidates-list');
        if (cList) { cList.innerHTML = ''; cList.style.display = 'none'; }
      } catch (e) {
        showToast('Lỗi khi tải chi tiết game.');
      }
    }

    function closeGameModal() {
      const modal = document.getElementById('game-detail-modal');
      if (modal) modal.style.display = 'none';
    }

    let modalCandidatesCache = [];
    async function searchModalScraperCandidates() {
      const qInput = document.getElementById('modal-search-query');
      const query = (qInput ? qInput.value : '').trim();
      const sysTag = document.getElementById('modal-sys-tag')?.textContent || '';
      if (!query) { showToast('Hãy nhập tên game để tìm.'); return; }

      const cList = document.getElementById('modal-candidates-list');
      if (cList) {
        cList.style.display = 'flex';
        cList.innerHTML = '<div style="color:var(--text-dim); padding:10px; text-align:center;">Đang tìm trên Libretro...</div>';
      }

      try {
        const res = await fetch('/api/search_scraper', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: `query=${encodeURIComponent(query)}&system_code=${encodeURIComponent(sysTag)}`
        });
        const data = await res.json();
        modalCandidatesCache = data.candidates || [];

        if (!modalCandidatesCache.length) {
          if (cList) cList.innerHTML = '<div style="color:var(--text-dim); padding:10px; text-align:center;">Không tìm thấy. Thử tên ngắn hơn.</div>';
          return;
        }

        let html = '';
        modalCandidatesCache.forEach((c, idx) => {
          html += `
            <div style="display:flex; gap:10px; align-items:center; background:var(--bg); border:1px solid var(--border); border-radius:6px; padding:8px 10px;">
              <div style="width:40px; height:50px; flex-shrink:0; background:#000; border-radius:4px; overflow:hidden; display:flex; align-items:center; justify-content:center;">
                ${c.cover_url ? `<img src="${c.cover_url}" style="width:100%; height:100%; object-fit:cover;" onerror="this.style.display='none';">` : ''}
              </div>
              <div style="flex:1; min-width:0;">
                <div style="font-size:12px; font-weight:600; color:#fff; overflow:hidden; text-overflow:ellipsis; white-space:nowrap;">${escapeHtml(c.title)}</div>
                <div style="font-size:11px; color:var(--text-dim);">${escapeHtml(c.source || '')} ${c.release_year ? '• ' + c.release_year : ''}</div>
              </div>
              <button type="button" class="btn btn-primary" onclick="applySelectedCandidate(${idx})" style="font-size:11px; padding:4px 10px; white-space:nowrap;">Dùng bìa này</button>
            </div>
          `;
        });
        if (cList) cList.innerHTML = html;
      } catch (e) {
        if (cList) cList.innerHTML = '<div style="color:var(--red); padding:10px; text-align:center;">Lỗi tìm kiếm.</div>';
      }
    }

    async function applySelectedCandidate(idx) {
      const c = modalCandidatesCache[idx];
      if (!c || !currentModalGameId) return;

      showToast('Đang áp dụng ảnh bìa...');
      try {
        const params = new URLSearchParams();
        params.append('game_id', currentModalGameId);
        params.append('cover_url', c.cover_url || '');
        params.append('title', c.title || '');
        params.append('year', c.release_year || '');
        params.append('genre', c.genre || '');
        params.append('developer', c.developer || '');
        params.append('description', c.description || '');

        const res = await fetch('/api/apply_candidate', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: params.toString()
        });
        const data = await res.json();
        showToast(data.message || 'Đã áp dụng bìa và thông tin!');
        await openGameModal(currentModalGameId);
        loadGames();
      } catch (e) {
        showToast('Lỗi khi áp dụng bìa.');
      }
    }

    async function scrapeCurrentModalGame() {
      if (!currentModalGameId) return;
      const btn = document.getElementById('btn-modal-scrape');
      const origText = btn ? btn.innerHTML : 'Lấy lại bìa và thông tin';
      if (btn) { btn.disabled = true; btn.innerHTML = 'Đang lấy dữ liệu...'; }
      showToast('Đang tìm bìa và thông tin trên Libretro...');

      try {
        const res = await fetch('/api/scrape_cover', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: `game_id=${currentModalGameId}`
        });
        const data = await res.json();
        showToast(data.message || 'Đã lấy bìa và thông tin xong!');
        await openGameModal(currentModalGameId);
        loadGames();
      } catch (e) {
        showToast('Lỗi khi lấy dữ liệu game.');
      } finally {
        if (btn) { btn.disabled = false; btn.innerHTML = origText; }
      }
    }

    async function uploadCurrentModalGame() {
      if (!currentModalGameId) return;
      const title = document.getElementById('modal-game-title').textContent || 'ROM';
      await uploadGameToDrive(currentModalGameId, title);
    }

    // Actions
    async function downloadGame(gameId) {
      try {
        const res = await fetch('/api/download_game', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: `game_id=${gameId}`
        });
        const data = await res.json();
        showToast(data.message || 'Đã thêm vào hàng tải.');
        loadGames();
        updateDownloadQueueUI();
      } catch (e) {
        showToast('Lỗi khi thêm vào hàng tải.');
      }
    }

    // DOCTOR ROM FUNCTIONS
    function openDoctorModal() {
      const modal = document.getElementById('doctor-modal');
      if (modal) modal.style.display = 'flex';
      runDoctorAudit();
    }

    function closeDoctorModal() {
      const modal = document.getElementById('doctor-modal');
      if (modal) modal.style.display = 'none';
    }

    let doctorAuditItems = [];
    async function runDoctorAudit() {
      const loading = document.getElementById('doctor-loading');
      const results = document.getElementById('doctor-results');
      const summaryBox = document.getElementById('doctor-summary-box');
      const itemsWrap = document.getElementById('doctor-items-wrap');
      const fixBtn = document.getElementById('btn-doctor-fix');

      if (loading) loading.style.display = 'block';
      if (results) results.style.display = 'none';
      if (itemsWrap) itemsWrap.innerHTML = '';

      try {
        const res = await fetch('/api/audit_roms', { method: 'POST' });
        const data = await res.json();
        doctorAuditItems = data.items || [];

        if (loading) loading.style.display = 'none';
        if (results) results.style.display = 'block';

        if (doctorAuditItems.length === 0) {
          if (summaryBox) {
            summaryBox.style.background = 'rgba(16,185,129,0.15)';
            summaryBox.style.border = '1px solid #10b981';
            summaryBox.style.color = '#34d399';
            summaryBox.innerHTML = '<b>Mọi game đều đúng thư mục.</b> Không có ROM nào đặt sai chỗ.';
          }
          if (fixBtn) fixBtn.style.display = 'none';
        } else {
          if (summaryBox) {
            summaryBox.style.background = 'rgba(245,158,11,0.15)';
            summaryBox.style.border = '1px solid #f59e0b';
            summaryBox.style.color = '#fbbf24';
            summaryBox.innerHTML = `<b>Có ${doctorAuditItems.length} ROM đặt sai thư mục.</b><br>Các game này có thể không chạy được. Bấm nút xanh ở dưới để tự chuyển về đúng chỗ.`;
          }
          if (fixBtn) {
            fixBtn.style.display = 'inline-block';
            fixBtn.disabled = false;
            fixBtn.innerHTML = `Chuyển ${doctorAuditItems.length} ROM về đúng thư mục`;
          }

          if (itemsWrap) {
            itemsWrap.innerHTML = doctorAuditItems.map(item => `
              <div style="background: var(--bg); border: 1px solid var(--border); border-radius: 8px; padding: 10px 12px; display: flex; justify-content: space-between; align-items: center; gap: 10px;">
                <div style="min-width: 0; flex: 1;">
                  <div style="font-weight: 600; color: #fff; font-size: 13px; text-overflow: ellipsis; overflow: hidden; white-space: nowrap;">${escapeHtml(item.filename)}</div>
                  <div style="font-size: 11.5px; color: var(--text-dim); margin-top: 3px;">
                    Hiện tại: <span style="color: #f87171; background: rgba(239,68,68,0.15); padding: 1px 6px; border-radius: 4px; font-weight: 600;">/Roms/${escapeHtml(item.current_system)}</span>
                    Chuyển về: <span style="color: #34d399; background: rgba(16,185,129,0.15); padding: 1px 6px; border-radius: 4px; font-weight: 600;">/Roms/${escapeHtml(item.detected_system)} (${escapeHtml(item.detected_name)})</span>
                  </div>
                  <div style="font-size: 11px; color: #94a3b8; margin-top: 2px;">Lý do: ${escapeHtml(item.reason)}</div>
                </div>
              </div>
            `).join('');
          }
        }
      } catch (e) {
        if (loading) loading.style.display = 'none';
        if (results) results.style.display = 'block';
        if (summaryBox) {
          summaryBox.style.background = 'rgba(239,68,68,0.15)';
          summaryBox.style.border = '1px solid #ef4444';
          summaryBox.style.color = '#f87171';
          summaryBox.innerHTML = 'Lỗi khi kiểm tra thư mục trên thẻ.';
        }
      }
    }

    async function executeDoctorFix() {
      const fixBtn = document.getElementById('btn-doctor-fix');
      if (fixBtn) {
        fixBtn.disabled = true;
        fixBtn.innerHTML = 'Đang chuyển file và ảnh bìa...';
      }
      showToast('Đang chuyển game về đúng thư mục...');

      try {
        const res = await fetch('/api/fix_misplaced_roms', { method: 'POST' });
        const data = await res.json();
        const fixedItems = data.items || [];
        const successCount = fixedItems.filter(i => i.fixed).length;

        const summaryBox = document.getElementById('doctor-summary-box');
        if (summaryBox) {
          summaryBox.style.background = 'rgba(16,185,129,0.2)';
          summaryBox.style.border = '1px solid #10b981';
          summaryBox.style.color = '#34d399';
          summaryBox.innerHTML = `<b>Xong!</b> Đã chuyển ${successCount} ROM về đúng thư mục. Chơi được ngay trên máy.`;
        }

        const itemsWrap = document.getElementById('doctor-items-wrap');
        if (itemsWrap) {
          itemsWrap.innerHTML = fixedItems.map(item => `
            <div style="background: rgba(16,185,129,0.06); border: 1px solid rgba(16,185,129,0.3); border-radius: 8px; padding: 8px 12px; font-size: 12px; color: #e2e8f0; display: flex; align-items: center; justify-content: space-between;">
              <div>
                <b>${escapeHtml(item.filename)}</b>
                <div style="font-size: 11px; color: #34d399;">${escapeHtml(item.status)}</div>
              </div>
              <span style="font-size: 14px; color: #34d399; font-weight: 700;">Xong</span>
            </div>
          `).join('');
        }

        if (fixBtn) fixBtn.style.display = 'none';
        showToast(`Đã sửa ${successCount} ROM đặt sai thư mục!`);
        loadGames();
        loadStorageInfo();
      } catch (e) {
        showToast('Lỗi khi chuyển ROM.');
        if (fixBtn) {
          fixBtn.disabled = false;
          fixBtn.innerHTML = 'Thử lại';
        }
      }
    }

    async function uploadAutoRomFile(file) {
      if (!file) return;
      const statusDiv = document.getElementById('doctor-upload-status');
      if (statusDiv) {
        statusDiv.style.display = 'block';
        statusDiv.style.color = '#38bdf8';
        statusDiv.innerHTML = `Đang tải lên và nhận diện hệ máy cho <b>${escapeHtml(file.name)}</b> (${(file.size / 1024 / 1024).toFixed(2)} MB)...`;
      }

      try {
        const res = await fetch(`/api/upload_rom_auto?filename=${encodeURIComponent(file.name)}`, {
          method: 'POST',
          body: file
        });
        const data = await res.json();
        if (data.success) {
          if (statusDiv) {
            statusDiv.style.color = '#34d399';
            statusDiv.innerHTML = `${escapeHtml(data.message || 'Đã phân loại xong!')}`;
          }
          showToast(`Đã nhận diện: ${data.system_name || 'ROM'}. Đã đặt vào đúng thư mục.`);
          setTimeout(() => {
            runDoctorAudit();
            loadGames();
            loadStorageInfo();
          }, 1000);
        } else {
          if (statusDiv) {
            statusDiv.style.color = '#f87171';
            statusDiv.innerHTML = `${escapeHtml(data.error || 'Lỗi xử lý file')}`;
          }
        }
      } catch (e) {
        if (statusDiv) {
          statusDiv.style.color = '#f87171';
          statusDiv.innerHTML = 'Lỗi khi tải file lên máy.';
        }
      }
    }

    async function deleteRom(gameId, title) {
      if (!confirm(`Xóa "${title}" khỏi thẻ? Sau này vẫn tải lại được.`)) {
        return;
      }
      try {
        const res = await fetch('/api/delete_rom', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: `game_id=${gameId}`
        });
        const data = await res.json();
        showToast(data.message || 'Đã xóa khỏi thẻ nhớ.');
        loadGames();
        loadStorageInfo();
        loadSystems();
      } catch (e) {
        showToast('Lỗi khi xóa ROM.');
      }
    }

    async function scrapeBoxart(gameId) {
      showToast('Đang tìm bìa và thông tin...');
      try {
        const res = await fetch('/api/scrape_cover', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: `game_id=${gameId}`
        });
        const data = await res.json();
        if (data.success) {
          showToast(data.message || 'Đã cập nhật bìa và thông tin!');
          loadGames();
        } else {
          showToast(data.message || 'Không tìm thấy dữ liệu.');
        }
      } catch (e) {
        showToast('Lỗi khi lấy thông tin game.');
      }
    }

    let isBatchScraping = false;
    async function batchScrapeCurrentPage() {
      if (isBatchScraping) {
        showToast('Đang lấy thông tin, chờ một lát...');
        return;
      }
      if (!lastLoadedGames || !lastLoadedGames.games || lastLoadedGames.games.length === 0) {
        showToast('Trang này không có game nào để lấy bìa.');
        return;
      }
      const gamesToScrape = lastLoadedGames.games;
      if (!confirm(`Lấy ảnh bìa và thông tin cho tất cả ${gamesToScrape.length} game ở trang này?`)) return;

      isBatchScraping = true;
      const btn = document.getElementById('btn-batch-scrape');
      const origText = btn ? btn.innerHTML : 'Lấy bìa trang này';
      if (btn) { btn.disabled = true; btn.innerHTML = 'Đang lấy...'; }

      let successCount = 0;
      for (let i = 0; i < gamesToScrape.length; i++) {
        const g = gamesToScrape[i];
        showToast(`Đang lấy bìa (${i + 1}/${gamesToScrape.length}): ${g.title.substring(0, 20)}...`);
        try {
          const res = await fetch('/api/scrape_cover', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: `game_id=${g.id}`
          });
          const data = await res.json();
          if (data.success) successCount++;
        } catch (e) {}
      }

      showToast(`Đã lấy thông tin cho ${successCount}/${gamesToScrape.length} game!`);
      if (btn) { btn.disabled = false; btn.innerHTML = origText; }
      isBatchScraping = false;
      loadGames();
    }

    // Auto-scrape all SD Card ROMs in background
    async function startAutoScrapeSd(force = false) {
      if (!confirm('Tự lấy ảnh bìa và thông tin cho mọi game trên thẻ (chạy ngầm)?\n\nNguồn: Libretro Thumbnails.')) {
        return;
      }
      try {
        const res = await fetch('/api/auto_scrape_sd', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: `force=${force ? '1' : '0'}`
        });
        const data = await res.json();
        showToast(data.message || 'Đã bắt đầu lấy bìa tự động!');
        startScrapePolling();
      } catch (e) {
        showToast('Lỗi khi bắt đầu lấy bìa tự động.');
      }
    }

    function startScrapePolling() {
      if (scrapePollTimer) return;
      pollScrapeProgress();
      scrapePollTimer = setInterval(pollScrapeProgress, 1200);
    }

    async function pollScrapeProgress() {
      try {
        const res = await fetch('/api/auto_scrape_status');
        const data = await res.json();
        const card = document.getElementById('active-scrape-floating');
        if (!data.is_scraping) {
          if (card) card.style.display = 'none';
          if (scrapePollTimer) {
            clearInterval(scrapePollTimer);
            scrapePollTimer = null;
          }
          return;
        }

        if (card) card.style.display = 'block';
        const titleEl = document.getElementById('scrape-float-title');
        const sysEl = document.getElementById('scrape-float-sys');
        const fillEl = document.getElementById('scrape-float-progress-fill');
        const countEl = document.getElementById('scrape-float-count');
        const pctEl = document.getElementById('scrape-float-pct');

        if (titleEl) titleEl.textContent = data.current_game || 'Game';
        if (sysEl) sysEl.textContent = `Hệ máy: ${data.current_sys || 'SYS'} (${data.scraped}/${data.total} game)`;
        if (fillEl) fillEl.style.width = (data.progress_pct || 0) + '%';
        if (countEl) countEl.textContent = `Đã lấy: ${data.scraped} / ${data.total} (${data.success} xong)`;
        if (pctEl) pctEl.textContent = `${data.progress_pct || 0}%`;
      } catch (e) {}
    }

    async function cancelAutoScrape() {
      if (!confirm('Hủy việc lấy bìa tự động trên thẻ?')) return;
      try {
        await fetch('/api/cancel_auto_scrape', { method: 'POST' });
        showToast('Đã yêu cầu hủy.');
        const card = document.getElementById('active-scrape-floating');
        if (card) card.style.display = 'none';
        if (scrapePollTimer) {
          clearInterval(scrapePollTimer);
          scrapePollTimer = null;
        }
      } catch (e) {}
    }

    async function cancelActiveDownload() {
      if (!confirm('Hủy lượt tải hiện tại?')) return;
      try {
        await fetch('/api/cancel_download', { method: 'POST' });
        showToast('Đã yêu cầu hủy tải.');
        updateDownloadQueueUI();
        loadGames();
      } catch (e) {}
    }

    async function removeFromQueue(gameId) {
      try {
        const res = await fetch('/api/remove_queue', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: `game_id=${gameId}`
        });
        const data = await res.json();
        showToast(data.message || 'Đã xóa khỏi hàng đợi.');
        updateDownloadQueueUI();
        loadGames();
      } catch (e) {}
    }

    async function clearAllQueue() {
      if (!confirm('Xóa hết game đang chờ tải?')) return;
      try {
        await fetch('/api/clear_queue', { method: 'POST' });
        showToast('Đã xóa hết hàng đợi.');
        updateDownloadQueueUI();
        loadGames();
      } catch (e) {}
    }

    // Google Drive Personal Backup Actions
    async function uploadGameToDrive(gameId, title) {
      if (!canUploadGlobal) {
        if (confirm('Bạn chưa đăng nhập Google để sao lưu. Mở mục Đồng bộ để đăng nhập?')) {
          switchTab('tab-storage');
        }
        return;
      }

      if (!confirm(`Sao lưu "${title}" lên Drive của bạn (thư mục Roms)?`)) {
        return;
      }

      try {
        const res = await fetch('/api/upload_game', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: `game_id=${gameId}`
        });
        const data = await res.json();
        if (data.success) {
          showToast(data.message || 'Đã bắt đầu sao lưu!');
          startUploadPolling();
        } else {
          showToast(data.message || 'Không thể bắt đầu sao lưu.');
        }
      } catch (e) {
        showToast('Lỗi khi gửi yêu cầu sao lưu.');
      }
    }

    async function uploadAllGamesToDrive() {
      if (!canUploadGlobal) {
        if (confirm('Bạn chưa đăng nhập Google để sao lưu. Mở mục Đồng bộ để đăng nhập?')) {
          switchTab('tab-storage');
        }
        return;
      }

      if (!confirm('Sao lưu toàn bộ game trên thẻ lên Drive của bạn (thư mục Roms)? Việc này chạy ngầm, lần lượt từng game.')) {
        return;
      }

      try {
        const res = await fetch('/api/upload_all', { method: 'POST' });
        const data = await res.json();
        if (data.success) {
          showToast(data.message || 'Đã bắt đầu sao lưu cả thẻ!');
          startUploadPolling();
        } else {
          showToast(data.message || 'Không thể bắt đầu sao lưu.');
        }
      } catch (e) {
        showToast('Lỗi khi gửi yêu cầu sao lưu.');
      }
    }

    function startUploadPolling() {
      if (uploadPollTimer) return;
      pollUploadProgress();
      uploadPollTimer = setInterval(pollUploadProgress, 1200);
    }

    async function pollUploadProgress() {
      try {
        const res = await fetch('/api/upload_status');
        const data = await res.json();
        const floatCard = document.getElementById('active-upload-floating');
        if (!data.is_uploading) {
          if (floatCard) floatCard.style.display = 'none';
          if (uploadPollTimer) {
            clearInterval(uploadPollTimer);
            uploadPollTimer = null;
            if (data.state === 'FAILED') {
              showToast(data.error || 'Sao lưu thất bại.', 'error');
            } else if (data.state === 'COMPLETED') {
              if (data.games_failed > 0) {
                showToast(`Sao lưu xong: ${data.games_uploaded} thành công, ${data.games_failed} thất bại.${data.error ? ' ' + data.error : ''}`, 'error');
              } else if (data.total_games === 0) {
                showToast(data.error || 'Không có game nào để sao lưu.', 'error');
              } else {
                showToast(`Sao lưu xong: ${data.games_uploaded} game đã lên Drive.`);
              }
            } else if (data.state === 'CANCELLED') {
              showToast('Đã hủy sao lưu.');
            }
          }
          return;
        }

        if (floatCard) floatCard.style.display = 'block';
        const titleEl = document.getElementById('upload-float-title');
        const fileEl = document.getElementById('upload-float-file');
        const fillEl = document.getElementById('upload-float-progress-fill');
        const speedEl = document.getElementById('upload-float-speed');
        const pctEl = document.getElementById('upload-float-percent');
        const bytesEl = document.getElementById('upload-float-bytes');

        const pct = Math.round(data.progress_pct || 0);
        const fmtMB = (b) => ((b || 0) / 1048576).toFixed(1) + ' MB';
        if (titleEl) titleEl.textContent = data.game_title || 'Game';
        if (fileEl) fileEl.textContent = `${data.filename || ''} (${data.current_index}/${data.total_games} game)`;
        if (fillEl) fillEl.style.width = pct + '%';
        if (speedEl) speedEl.textContent = `${Math.round(data.speed_kbps || 0)} KB/s`;
        if (pctEl) pctEl.textContent = `${pct}%`;
        if (bytesEl) bytesEl.textContent = `${fmtMB(data.bytes_uploaded)} / ${fmtMB(data.total_bytes)}`;
      } catch (e) {}
    }

    async function cancelActiveUpload() {
      if (!confirm('Hủy sao lưu đang chạy?')) return;
      try {
        await fetch('/api/cancel_upload', { method: 'POST' });
        showToast('Đã yêu cầu hủy sao lưu.');
        const floatCard = document.getElementById('active-upload-floating');
        if (floatCard) floatCard.style.display = 'none';
        if (uploadPollTimer) {
          clearInterval(uploadPollTimer);
          uploadPollTimer = null;
        }
      } catch (e) {}
    }

    async function triggerSync() {
      const btn = document.getElementById('btn-trigger-sync');
      btn.disabled = true;
      btn.textContent = 'Đang quét Drive...';
      const box = document.getElementById('sync-status-box');
      box.style.display = 'block';

      try {
        await fetch('/api/trigger_sync', { method: 'POST' });
        showToast('Đã bắt đầu đồng bộ.');
        pollSyncProgress();
      } catch (e) {
        btn.disabled = false;
        btn.textContent = 'Quét và đồng bộ ngay';
      }
    }

    async function pollSyncProgress() {
      const timer = setInterval(async () => {
        try {
          const res = await fetch('/api/sync_status');
          const data = await res.json();
          const txt = document.getElementById('sync-status-txt');
          if (data.is_syncing) {
            txt.textContent = `${data.status} · Đang quét: ${data.current_platform || '...'} · Tìm thấy: ${data.games_found || 0} game`;
          } else {
            clearInterval(timer);
            document.getElementById('btn-trigger-sync').disabled = false;
            document.getElementById('btn-trigger-sync').textContent = 'Quét và đồng bộ ngay';
            txt.textContent = `Đồng bộ xong. Tìm thấy: ${data.games_found || 0} game.`;
            loadSystems();
            loadGames();
            loadStorageInfo();
          }
        } catch (e) {
          clearInterval(timer);
        }
      }, 1500);
    }

    async function loadStorageInfo() {
      try {
        const res = await fetch('/api/storage_info');
        const data = await res.json();
        document.getElementById('storage-bar-fill').style.width = (data.used_pct || 0) + '%';
        document.getElementById('storage-used-txt').textContent = data.used_str || '--';
        document.getElementById('storage-avail-txt').textContent = data.avail_str || '--';
        document.getElementById('storage-total-txt').textContent = data.total_str || '--';
        document.getElementById('head-storage-pill').innerHTML = `SD: <b>${data.avail_str || '--'}</b> trống`;
        document.getElementById('head-local-count').textContent = data.total_local || 0;
        document.getElementById('head-cloud-count').textContent = data.total_cloud || 0;
        if (data.last_sync) document.getElementById('last-sync-time').textContent = data.last_sync;

        const wasLinked = isDriveLinked;
        isDriveLinked = !!data.is_linked;

        const authPill = document.getElementById('head-auth-pill');
        const btnHeadLogout = document.getElementById('btn-head-logout');
        const unlinkedBanner = document.getElementById('unlinked-warning-banner');
        const driveConnStatus = document.getElementById('drive-connection-status');
        const btnTabLogout = document.getElementById('btn-tab-logout');
        const btnTriggerSync = document.getElementById('btn-trigger-sync');
        const driveInput = document.getElementById('input-drive-url');
        const sInput = document.getElementById('rom-search');

        if (data.is_linked) {
          authPill.innerHTML = `Drive: <b style="color:var(--green);">Đã kết nối</b>`;
          btnHeadLogout.style.display = 'inline-flex';
          unlinkedBanner.style.display = 'none';
          driveConnStatus.innerHTML = `Trạng thái: <b style="color:var(--green);">Đã liên kết</b> (${data.user_email || 'Google Drive'})`;
          btnTabLogout.style.display = 'inline-flex';
          btnTriggerSync.disabled = false;
          if (driveInput && data.drive_url) driveInput.value = data.drive_url;
          if (sInput) {
            sInput.disabled = false;
            sInput.placeholder = 'Tìm theo tên game hoặc tên file...';
          }
        } else {
          authPill.innerHTML = `Drive: <b style="color:var(--text-dim);">Chưa kết nối</b>`;
          btnHeadLogout.style.display = 'none';
          unlinkedBanner.style.display = 'block';
          driveConnStatus.innerHTML = `Trạng thái: <b style="color:var(--yellow);">Chưa liên kết Google Drive</b>`;
          btnTabLogout.style.display = 'none';
          btnTriggerSync.disabled = true;
          document.getElementById('head-local-count').textContent = '0';
          document.getElementById('head-cloud-count').textContent = '0';
          if (driveInput) driveInput.value = '';
          if (sInput) {
            sInput.disabled = true;
            sInput.value = '';
            sInput.placeholder = 'Chưa kết nối Google Drive...';
          }
        }

        const backupBadge = document.getElementById('backup-perm-badge');
        const btnClearToken = document.getElementById('btn-clear-personal-token');
        const btnLoginStart = document.getElementById('btn-google-login-start');
        if (backupBadge) {
          if (data.can_upload) {
            backupBadge.innerHTML = `<span style="color:var(--green);">Đã bật</span> (${escapeHtml(data.user_email || 'Drive của bạn')} · thư mục Roms)`;
            if (btnClearToken) btnClearToken.style.display = 'inline-block';
            if (btnLoginStart) btnLoginStart.innerHTML = 'Đổi tài khoản Google';
          } else {
            backupBadge.innerHTML = `<span style="color:var(--yellow);">Chưa bật</span> (hiện chỉ tải về được)`;
            if (btnClearToken) btnClearToken.style.display = 'none';
            if (btnLoginStart) btnLoginStart.innerHTML = 'Đăng nhập Google';
          }
        }

        canUploadGlobal = !!data.can_upload;

        // Update Architecture Banner cards in Tab 1
        const archPublicUrl = document.getElementById('arch-public-url');
        if (archPublicUrl) {
          archPublicUrl.textContent = data.is_linked ? (data.drive_url || 'Đã liên kết') : 'Chưa kết nối';
        }
        const archLocalStatus = document.getElementById('arch-local-status');
        if (archLocalStatus) {
          archLocalStatus.innerHTML = `${data.total_local || 0} game trên thẻ • <span style="color:var(--text-dim);">${data.avail_str || '--'} trống</span>`;
        }
        const archBackupBadge = document.getElementById('arch-backup-badge');
        if (archBackupBadge) {
          if (data.can_upload) {
            archBackupBadge.innerHTML = `<span style="color:var(--green);">Sẵn sàng sao lưu</span> (${escapeHtml(data.user_email || 'Drive của bạn')})`;
          } else {
            archBackupBadge.innerHTML = `<span style="color:var(--yellow);">Chưa đăng nhập</span>`;
          }
        }

        if (wasLinked !== isDriveLinked) {
          renderSystemPills();
          loadGames();
        }
      } catch (e) {}
    }

    function clearDriveInput() {
      const el = document.getElementById('input-drive-url');
      if (el) {
        el.value = '';
        el.focus();
      }
    }

    async function handleConnectSubmit(e) {
      e.preventDefault();
      const input = document.getElementById('input-drive-url');
      const val = (input ? input.value : '').trim();
      if (!val) {
        showToast('Hãy dán link thư mục Drive trước.');
        return;
      }
      try {
        const body = 'drive_url=' + encodeURIComponent(val);
        const res = await fetch('/connect', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: body
        });
        showToast('Đã lưu. Đang quét...');
        await loadStorageInfo();
        triggerSync();
      } catch (err) {
        showToast('Lỗi khi gửi kết nối.');
      }
    }

    async function savePersonalToken() {
      const input = document.getElementById('input-personal-token');
      const btn = document.getElementById('btn-save-personal-token');
      const statusBox = document.getElementById('personal-token-status');
      const val = (input ? input.value : '').trim();

      if (!val) {
        showToast('Hãy dán Access Token hoặc Refresh Token.');
        if (input) input.focus();
        return;
      }

      const origText = btn ? btn.innerHTML : 'Lưu token';
      if (btn) {
        btn.disabled = true;
        btn.innerHTML = 'Đang kiểm tra...';
      }
      if (statusBox) statusBox.innerHTML = '<span style="color:var(--accent);">Đang kiểm tra token...</span>';

      try {
        const body = 'token=' + encodeURIComponent(val);
        const res = await fetch('/api/set_personal_auth', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: body
        });
        const data = await res.json();
        if (data.success) {
          showToast(data.message || 'Đã bật sao lưu!');
          if (statusBox) statusBox.innerHTML = `<span style="color:var(--green);">${escapeHtml(data.message || 'Đã bật sao lưu!')}</span>`;
          if (input) input.value = '';
          await loadStorageInfo();
        } else {
          showToast(data.error || 'Token không đúng.');
          if (statusBox) statusBox.innerHTML = `<span style="color:var(--red);">${escapeHtml(data.error || 'Token không đúng.')}</span>`;
        }
      } catch (e) {
        showToast('Lỗi kết nối máy chủ.');
        if (statusBox) statusBox.innerHTML = '<span style="color:var(--red);">Lỗi kết nối máy chủ.</span>';
      } finally {
        if (btn) {
          btn.disabled = false;
          btn.innerHTML = origText;
        }
      }
    }

    let googlePollInterval = null;

    async function startGoogleDeviceLogin() {
      const btn = document.getElementById('btn-google-login-start');
      const promptBox = document.getElementById('google-device-prompt');
      const origText = btn ? btn.innerHTML : '';
      if (btn) { btn.disabled = true; btn.innerHTML = 'Đang lấy mã...'; }

      try {
        const res = await fetch('/api/device_flow_start', { method: 'POST' });
        const data = await res.json();
        if (data.success && data.user_code) {
          if (promptBox) promptBox.style.display = 'block';
          const codeDisp = document.getElementById('google-user-code-display');
          if (codeDisp) codeDisp.textContent = data.user_code;
          const linkEl = document.getElementById('google-verify-link');
          if (linkEl && data.verification_url) linkEl.href = data.verification_url;

          showToast('Đã có mã. Mở trang Google và nhập mã nhé.');

          // Bắt đầu polling kiểm tra trạng thái
          if (googlePollInterval) clearInterval(googlePollInterval);
          googlePollInterval = setInterval(pollGoogleDeviceStatus, (data.interval || 5) * 1000);
        } else {
          showToast('Không lấy được mã đăng nhập: ' + (data.error || 'lỗi không rõ'));
        }
      } catch (err) {
        showToast('Lỗi kết nối máy chủ.');
      } finally {
        if (btn) { btn.disabled = false; btn.innerHTML = origText; }
      }
    }

    async function pollGoogleDeviceStatus() {
      try {
        const res = await fetch('/api/device_flow_status');
        const data = await res.json();
        if (data.is_linked && data.can_upload) {
          if (googlePollInterval) { clearInterval(googlePollInterval); googlePollInterval = null; }
          const promptBox = document.getElementById('google-device-prompt');
          if (promptBox) promptBox.style.display = 'none';
          showToast('Đã đăng nhập Google ' + (data.email || '') + '. Sao lưu đã bật.');
          await loadStorageInfo();
        } else if (data.status === 'ERROR') {
          if (googlePollInterval) { clearInterval(googlePollInterval); googlePollInterval = null; }
          showToast(data.error || 'Lỗi xác thực Google.');
        }
      } catch (err) {
        // bỏ qua lỗi mạng tạm thời trong lúc poll
      }
    }

    async function cancelGoogleDeviceLogin() {
      if (googlePollInterval) { clearInterval(googlePollInterval); googlePollInterval = null; }
      const promptBox = document.getElementById('google-device-prompt');
      if (promptBox) promptBox.style.display = 'none';
      try {
        await fetch('/api/device_flow_cancel', { method: 'POST' });
      } catch(e) {}
      showToast('Đã hủy phiên đăng nhập Google.');
    }

    function copyUserCode() {
      const codeDisp = document.getElementById('google-user-code-display');
      if (codeDisp && codeDisp.textContent) {
        navigator.clipboard.writeText(codeDisp.textContent.trim()).then(() => {
          showToast('Đã chép mã: ' + codeDisp.textContent.trim());
        }).catch(() => {
          showToast('Mã xác nhận: ' + codeDisp.textContent.trim());
        });
      }
    }

    async function clearPersonalToken() {
      if (!confirm('Hủy quyền sao lưu? Sau đó chỉ tải game về được.')) return;
      try {
        const res = await fetch('/api/set_personal_auth', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'action=clear'
        });
        const data = await res.json();
        showToast(data.message || 'Đã hủy quyền sao lưu.');
        const statusBox = document.getElementById('personal-token-status');
        if (statusBox) statusBox.innerHTML = '';
        await loadStorageInfo();
      } catch (e) {
        showToast('Lỗi khi hủy token.');
      }
    }

    async function logoutGoogleDrive() {
      if (!confirm("Đăng xuất Google Drive?\\n\\n- Game đã tải về thẻ vẫn còn nguyên.\\n- Danh sách game trên Drive sẽ ẩn đi.")) {
        return;
      }
      try {
        const res = await fetch('/api/logout', { method: 'POST' });
        const data = await res.json();
        showToast(data.message || 'Đã đăng xuất.');
        const driveInput = document.getElementById('input-drive-url');
        if (driveInput) driveInput.value = '';
        await loadStorageInfo();
        await loadSystems();
        await loadGames();
      } catch(e) {
        showToast('Lỗi khi đăng xuất.');
      }
    }

    async function updateDownloadQueueUI() {
      try {
        const res = await fetch('/api/download_status');
        const data = await res.json();

        // Queue Badge
        document.getElementById('nav-queue-badge').textContent = data.queue_size || 0;
        document.getElementById('queue-count-num').textContent = data.queue_size || 0;

        // Active Download Section
        const activeSec = document.getElementById('active-download-section');
        if (data.is_downloading && data.active) {
          activeSec.style.display = 'block';
          document.getElementById('dl-sys-tag').textContent = data.active.system || 'GAME';
          document.getElementById('dl-title').textContent = data.active.title || '';
          document.getElementById('dl-file').textContent = data.active.filename || '';
          document.getElementById('dl-progress-fill').style.width = (data.active.progress_pct || 0) + '%';
          document.getElementById('dl-percent').textContent = (data.active.progress_pct || 0).toFixed(1) + '%';
          document.getElementById('dl-speed').textContent = (data.active.speed_kbps > 1024) 
            ? (data.active.speed_kbps / 1024).toFixed(1) + ' MB/s' 
            : Math.round(data.active.speed_kbps) + ' KB/s';
          
          const curMB = (data.active.bytes_downloaded / (1024*1024)).toFixed(1);
          const totMB = (data.active.total_bytes / (1024*1024)).toFixed(1);
          document.getElementById('dl-bytes').textContent = `${curMB} / ${totMB} MB`;
        } else {
          activeSec.style.display = 'none';
        }

        // Queue List
        const qContainer = document.getElementById('queue-list-container');
        if (!data.queue || data.queue.length === 0) {
          qContainer.innerHTML = `<p style="color: var(--text-dim); text-align: center; padding: 20px;">Chưa có game nào đang chờ tải.</p>`;
        } else {
          let qHtml = '<table class="game-table"><thead><tr><th>#</th><th>Hệ máy</th><th>Tên game</th><th>Dung lượng</th><th style="text-align:right;">Thao tác</th></tr></thead><tbody>';
          data.queue.forEach((item, idx) => {
            qHtml += `
              <tr>
                <td style="color: var(--text-dim); font-weight:700;">#${idx + 1}</td>
                <td><span class="sys-tag sys-${item.system}">${item.system}</span></td>
                <td><b>${escapeHtml(item.title)}</b></td>
                <td>${item.size_str}</td>
                <td style="text-align:right;"><button class="btn btn-secondary" onclick="removeFromQueue(${item.game_id})">Bỏ</button></td>
              </tr>
            `;
          });
          qHtml += '</tbody></table>';
          qContainer.innerHTML = qHtml;
        }
      } catch (e) {}
    }

    function startPolling() {
      dlPollTimer = setInterval(() => {
        updateDownloadQueueUI();
      }, 1500);
    }

    function compareVer(v1, v2) {
      const p1 = (v1 || '').replace(/^v/i, '').split('.').map(n => parseInt(n) || 0);
      const p2 = (v2 || '').replace(/^v/i, '').split('.').map(n => parseInt(n) || 0);
      for (let i = 0; i < Math.max(p1.length, p2.length); i++) {
        const n1 = p1[i] || 0, n2 = p2[i] || 0;
        if (n1 > n2) return 1;
        if (n1 < n2) return -1;
      }
      return 0;
    }

    function quickApplyOta() {
      switchTab('tab-ota');
      const box = document.getElementById('ota-info-box');
      if (box) box.style.display = 'block';
      startOtaUpdate();
    }

    // OTA
    let otaPollTimer = null;
    async function checkOtaUpdate(silent = false) {
      const btn = document.getElementById('btn-ota-check');
      if (!silent && btn) {
        btn.disabled = true;
        btn.textContent = 'Đang kiểm tra...';
      }
      try {
        const res = await fetch('/ota_check');
        const data = await res.json();

        let hasUpdate = data.has_update;
        let remoteVer = data.remote_version;
        let changelog = data.changelog;
        let curVer = data.current_version;

        // Query GitHub API directly in browser for real-time instant notification
        try {
          const ghRes = await fetch('https://api.github.com/repos/bun2it/RomCloud/releases/latest');
          if (ghRes.ok) {
            const gh = await ghRes.json();
            const tagVer = (gh.tag_name || '').replace(/^v/i, '');
            if (tagVer && compareVer(tagVer, curVer) > 0) {
              hasUpdate = true;
              remoteVer = tagVer;
              if (gh.body) changelog = gh.body;
            }
          }
        } catch(e) {}

        if (!silent && btn) {
          btn.disabled = false;
          btn.textContent = 'Kiểm tra lại';
        }

        const box = document.getElementById('ota-info-box');
        if (box && !silent) box.style.display = 'block';

        if (hasUpdate) {
          // Push banner at top
          const banner = document.getElementById('ota-push-banner');
          if (banner) {
            banner.style.display = 'block';
            document.getElementById('ota-push-version').textContent = 'v' + remoteVer;
            if (changelog) {
              const shortChg = changelog.split('\n')[0] || changelog;
              document.getElementById('ota-push-notes').textContent = shortChg;
            }
          }
          // OTA tab badge
          const badge = document.getElementById('ota-nav-badge');
          if (badge) {
            badge.style.display = 'inline-block';
            badge.textContent = 'v' + remoteVer;
          }

          if (box) {
            document.getElementById('ota-version-title').innerHTML = `Có bản cập nhật mới: v${remoteVer}`;
            document.getElementById('ota-release-date').textContent = `Ngày phát hành: ${data.release_date || 'Mới nhất'}`;
            document.getElementById('ota-changelog').textContent = changelog;
            document.getElementById('btn-ota-install').style.display = 'inline-flex';
          }

          if (silent) {
            showToast(`Có bản cập nhật mới v${remoteVer}!`);
          }
        } else {
          if (box) {
            document.getElementById('ota-version-title').innerHTML = `Bạn đang dùng bản mới nhất (v${data.current_version})`;
            document.getElementById('ota-release-date').textContent = '';
            document.getElementById('ota-changelog').textContent = 'Chưa có bản nào mới hơn.';
            document.getElementById('btn-ota-install').style.display = 'none';
          }
        }
      } catch (e) {
        if (!silent && btn) {
          btn.disabled = false;
          btn.textContent = 'Kiểm tra lại';
        }
      }
    }

    async function startOtaUpdate() {
      document.getElementById('btn-ota-install').disabled = true;
      document.getElementById('btn-ota-check').disabled = true;
      document.getElementById('ota-progress-box').style.display = 'block';

      try {
        await fetch('/ota_start', { method: 'POST' });
        otaPollTimer = setInterval(async () => {
          const res = await fetch('/ota_status');
          const data = await res.json();
          document.getElementById('ota-progress-fill').style.width = (data.progress_pct || 0) + '%';
          document.getElementById('ota-status-txt').textContent = `Đang tải: ${(data.progress_pct || 0).toFixed(1)}%`;

          if (data.state === 'COMPLETED') {
            clearInterval(otaPollTimer);
            document.getElementById('ota-status-txt').innerHTML = `<span style="color:var(--green);font-weight:700;">Đã cập nhật xong. Đang khởi động lại...</span>`;
            await fetch('/ota_restart', { method: 'POST' });
            setTimeout(() => location.reload(), 5000);
          } else if (data.state === 'FAILED') {
            clearInterval(otaPollTimer);
            document.getElementById('ota-status-txt').innerHTML = `<span style="color:var(--red);">Thất bại: ${data.error}</span>`;
            document.getElementById('btn-ota-install').disabled = false;
          }
        }, 1000);
      } catch (e) {}
    }

    async function postForm(path, params) {
      const body = new URLSearchParams(params).toString();
      const res = await fetch(path, { method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body });
      return await res.json();
    }
    function escHtml(s) {
      return String(s == null ? '' : s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
    }
    function fmtTs(ts) {
      const d = new Date(ts * 1000);
      const p = (n) => String(n).padStart(2, '0');
      return `${p(d.getDate())}/${p(d.getMonth() + 1)} ${p(d.getHours())}:${p(d.getMinutes())}`;
    }
    async function loadCalSources() {
      try {
        const res = await fetch('/api/cal/sources');
        const data = await res.json();
        const box = document.getElementById('cal-sources');
        if (!data.sources || !data.sources.length) {
          box.innerHTML = '<span style="color: var(--text-dim);">Chưa có nguồn nào.</span>';
        } else {
          box.innerHTML = data.sources.map(s =>
            `<div style="display: flex; gap: 8px; align-items: center; background: var(--card-alt); border-radius: 6px; padding: 6px 10px;">
               <span style="flex: 1;"><b>${escHtml(s.name)}</b></span>
               <button class="btn btn-secondary" style="padding: 3px 10px; font-size: 11px;" onclick="calDelSource(${s.id})">Xóa</button>
             </div>`).join('');
        }
      } catch (e) {}
    }
    async function loadCalEvents() {
      try {
        const now = Math.floor(Date.now() / 1000);
        const res = await fetch(`/api/cal/events?from=${now - 86400}&to=${now + 60 * 86400}`);
        const data = await res.json();
        const box = document.getElementById('cal-events');
        if (!data.events || !data.events.length) {
          box.innerHTML = '<span style="color: var(--text-dim);">Chưa có sự kiện nào.</span>';
        } else {
          box.innerHTML = data.events.slice(0, 60).map(e =>
            `<div style="display: flex; gap: 8px; align-items: center; background: var(--card-alt); border-radius: 6px; padding: 6px 10px;">
               <span style="color: var(--accent); font-size: 11px; white-space: nowrap;">${fmtTs(e.start)}</span>
               <span style="flex: 1;">${escHtml(e.title)} <span style="color: var(--text-dim); font-size: 10px;">(${escHtml(e.source)})</span></span>
               <button class="btn btn-secondary" style="padding: 3px 10px; font-size: 11px;" onclick="calToggleRemind(${e.id}, ${e.remind ? 0 : 1})">${e.remind ? 'Tắt nhắc' : 'Nhắc'}</button>
               <button class="btn btn-secondary" style="padding: 3px 10px; font-size: 11px;" onclick="calDelEvent(${e.id})">Xóa</button>
             </div>`).join('');
        }
      } catch (e) {}
    }
    async function calAddUrl() {
      const name = document.getElementById('cal-url-name').value.trim();
      const url = document.getElementById('cal-url').value.trim();
      if (!url) { showToast('Chưa nhập link ICS'); return; }
      const d = await postForm('/api/cal/add_url', { name, url });
      showToast(d.success ? 'Đã thêm và tải lịch!' : ('Lỗi: ' + (d.error || 'không rõ')));
      loadCalSources(); loadCalEvents();
    }
    async function calImport() {
      const name = document.getElementById('cal-ics-name').value.trim();
      const ics = document.getElementById('cal-ics').value;
      if (!ics.trim()) { showToast('Chưa dán nội dung ICS'); return; }
      const d = await postForm('/api/cal/import', { name, ics });
      showToast(d.success ? `Đã nhập ${d.count} sự kiện!` : ('Lỗi: ' + (d.error || 'không rõ')));
      loadCalSources(); loadCalEvents();
    }
    async function calAddManual() {
      const date = document.getElementById('cal-m-date').value;
      const time = document.getElementById('cal-m-time').value;
      const title = document.getElementById('cal-m-title').value.trim();
      if (!date || !title) { showToast('Thiếu ngày/tiêu đề'); return; }
      const d = await postForm('/api/cal/add_manual', { date, time, title });
      showToast(d.success ? 'Đã thêm!' : 'Lỗi thêm việc');
      loadCalEvents();
    }
    async function calDelSource(id) {
      if (!confirm('Xóa nguồn lịch này (kèm toàn bộ sự kiện của nó)?')) return;
      await postForm('/api/cal/del_source', { id: String(id) });
      loadCalSources(); loadCalEvents();
    }
    async function calDelEvent(id) {
      await postForm('/api/cal/del_event', { id: String(id) });
      loadCalEvents();
    }
    async function calToggleRemind(id, on) {
      await postForm('/api/cal/remind', { id: String(id), remind: String(on) });
      loadCalEvents();
    }
    async function calRefresh() {
      const d = await postForm('/api/cal/refresh', {});
      showToast(`Đã làm mới ${d.refreshed || 0} nguồn!`);
      loadCalEvents();
    }

    const ALARM_MODES = ['Chuông+Rung', 'Chuông', 'Rung'];
    async function loadAlarms() {
      try {
        const d = await (await fetch('/api/clock_alarms')).json();
        const box = document.getElementById('alarm-list');
        if (!d.alarms || !d.alarms.length) {
          box.innerHTML = '<div style="color:var(--text-dim);font-size:12px">Chưa có báo thức nào.</div>';
          return;
        }
        box.innerHTML = d.alarms.map(a => `
          <div style="display:flex;align-items:center;gap:10px;background:var(--card-alt);border:1px solid var(--border);border-radius:8px;padding:8px 12px;">
            <b style="font-size:16px;min-width:56px;${a.enabled ? '' : 'color:var(--text-dim);text-decoration:line-through;'}">${a.time}</b>
            <span class="status-badge ${a.enabled ? 'status-local' : 'status-cloud'}">${a.enabled ? 'BẬT' : 'TẮT'}</span>
            <button class="btn btn-secondary" style="padding:4px 10px;font-size:11px" onclick="alarmMode(${a.idx})" title="Đổi kiểu báo">${ALARM_MODES[a.mode] || ''}</button>
            <span style="flex:1"></span>
            <button class="btn btn-secondary" style="padding:4px 10px;font-size:11px" onclick="alarmToggle(${a.idx})">${a.enabled ? 'Tắt' : 'Bật'}</button>
            <button class="btn btn-danger" style="padding:4px 10px;font-size:11px" onclick="alarmDel(${a.idx})">Xóa</button>
          </div>`).join('');
      } catch (e) { showToast('Không tải được báo thức'); }
    }
    async function alarmAdd() {
      const h = Math.max(0, Math.min(23, parseInt(document.getElementById('alarm-hour').value || '7', 10)));
      const m = Math.max(0, Math.min(59, parseInt(document.getElementById('alarm-min').value || '0', 10)));
      await postForm('/api/clock_alarm', { action: 'add', hour: String(h), minute: String(m) });
      showToast(`Đã thêm báo thức ${h < 10 ? '0' : ''}${h}:${m < 10 ? '0' : ''}${m}`);
      loadAlarms();
    }
    async function alarmToggle(i) { await postForm('/api/clock_alarm', { action: 'toggle', idx: String(i) }); loadAlarms(); }
    async function alarmDel(i) { await postForm('/api/clock_alarm', { action: 'del', idx: String(i) }); loadAlarms(); }
    async function alarmMode(i) {
      const d = await (await fetch('/api/clock_alarms')).json();
      const cur = (d.alarms && d.alarms[i]) ? d.alarms[i].mode : 0;
      await postForm('/api/clock_alarm', { action: 'mode', idx: String(i), mode: String((cur + 1) % 3) });
      loadAlarms();
    }
    async function loadWatch() {
      try {
        const d = await (await fetch('/api/watch_symbols')).json();
        document.getElementById('watch-symbols').value = (d.symbols || []).join(',');
        document.getElementById('watch-list').innerHTML = (d.symbols || []).map(s =>
          `<span class="status-badge status-local">${s}</span>`).join('') ||
          '<div style="color:var(--text-dim);font-size:12px">Trống. Máy sẽ dùng mặc định BTC,ETH,VCB,HPG.</div>';
      } catch (e) { showToast('Không tải được danh sách mã'); }
    }
    async function watchSave() {
      const v = document.getElementById('watch-symbols').value;
      const d = await postForm('/api/watch_symbols', { symbols: v });
      showToast(d.success ? 'Đã lưu! Mở mục Thị trường trên máy để xem.' : 'Lỗi lưu');
      loadWatch();
    }
  </script>
</body>
</html>)HTML";
  return html;
}

std::string WebServer::buildSaverHtml() {
  return R"HTML(<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Hình nền - RomCloud</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:sans-serif;background:#111;color:#eee;padding:16px}
h1{font-size:20px;margin-bottom:16px}
.upload{margin-bottom:20px;padding:16px;background:#222;border-radius:8px}
.upload button{background:#4CAF50;color:#fff;border:none;padding:10px 20px;border-radius:4px;cursor:pointer}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:12px}
.card{background:#222;border-radius:8px;overflow:hidden}
.card img{width:100%;height:120px;object-fit:cover;display:block}
.card .info{padding:8px}
.card .name{font-size:12px;margin-bottom:8px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.card .btns{display:flex;gap:6px}
.card button{flex:1;padding:6px;border:none;border-radius:4px;cursor:pointer;font-size:12px}
.btn-toggle{background:#2196F3;color:#fff}
.btn-del{background:#f44336;color:#fff}
</style>
</head>
<body>
<h1>Ảnh hình nền</h1>
<div class="upload">
<input type="file" id="fileInput" accept="image/jpeg,image/png" multiple>
<button onclick="uploadFiles()">Tải ảnh lên</button>
<div id="progress"></div>
</div>
<div id="grid" class="grid"></div>
<script>
async function loadPhotos(){
const r=await fetch('/api/saver/photos');
const photos=await r.json();
const g=document.getElementById('grid');
g.innerHTML=photos.map(p=>`<div class="card"><img src="/saver/photo/${p.name}" loading="lazy"><div class="info"><div class="name">${p.name}</div><div class="btns"><button class="btn-toggle" onclick="toggle('${p.name}')">${p.hidden?'Hiện':'Ẩn'}</button><button class="btn-del" onclick="del('${p.name}')">Xóa</button></div></div></div>`).join('');
}
async function toggle(n){await fetch('/api/saver/toggle',{method:'POST',body:'name='+n});loadPhotos();}
async function del(n){if(confirm('Xóa ảnh này?')){await fetch('/api/saver/delete',{method:'POST',body:'name='+n});loadPhotos();}}
async function uploadFiles(){
const inp=document.getElementById('fileInput');
for(const f of inp.files){const fd=new FormData();fd.append('file',f);await fetch('/api/saver/upload',{method:'POST',body:fd});}
loadPhotos();
}
loadPhotos();
</script>
</body>
</html>)HTML";
}
std::string WebServer::buildSuccessResponse(const std::string &message) {
  std::string html =
      R"(<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>RomCloud - Kết nối thành công</title>
  <style>
    body { font-family: -apple-system, sans-serif; background: #090d16; color: #f8fafc; padding: 40px 20px; text-align: center; }
    .card { background: #111827; border-radius: 16px; padding: 30px 20px; max-width: 440px; margin: 40px auto; border: 1px solid #10b981; box-shadow: 0 10px 25px rgba(0,0,0,0.5); }
    .icon { font-size: 54px; margin-bottom: 15px; color: #10b981; }
    h1 { font-size: 22px; color: #10b981; margin: 0 0 15px 0; }
    p { font-size: 15px; color: #cbd5e1; line-height: 1.6; margin-bottom: 25px; }
    a { display: inline-block; padding: 12px 24px; background: #1f293d; color: #38bdf8; text-decoration: none; border-radius: 8px; font-weight: 600; font-size: 14px; }
  </style>
</head>
<body>
  <div class="card">
    <div class="icon">&#10004;</div>
    <h1>Đã lưu xong</h1>
    <p>)" +
      message +
      R"(<br><br>Kho game đang được đồng bộ tự động.</p>
    <a href="/">&larr; Quay lại trang quản lý</a>
  </div>
</body>
</html>)";
  return html;
}

void WebServer::handleClient(int clientFd) {
  char buffer[4096];
  int bytesRead = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
  if (bytesRead <= 0)
    return;
  buffer[bytesRead] = '\0';

  // Binary-safe: body upload (ảnh/jpg) chứa byte NUL — dựng string theo độ
  // dài thực, không dừng ở NUL đầu tiên (bản cũ mất phần còn lại của body).
  std::string req(buffer, bytesRead);
  std::string method, fullPath;
  std::istringstream iss(req);
  iss >> method >> fullPath;

  std::string path = fullPath;
  std::string queryString = "";
  size_t qPos = fullPath.find('?');
  if (qPos != std::string::npos) {
    path = fullPath.substr(0, qPos);
    queryString = fullPath.substr(qPos + 1);
  }

  size_t bodyPos = req.find("\r\n\r\n");
  std::string postBody =
      (bodyPos != std::string::npos) ? req.substr(bodyPos + 4) : "";

  if (method == "GET" && (path == "/" || path == "/index.html")) {
    std::string body = buildHtmlResponse();
    std::string res =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate, max-age=0\r\n"
        "Pragma: no-cache\r\n"
        "Expires: 0\r\n"
        "Content-Length: " +
        std::to_string(body.length()) +
        "\r\n"
        "Connection: close\r\n\r\n" +
        body;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "GET" && (path == "/cast" || path == "/stream")) {
    std::string ip = PlatformInfo::instance().getIpAddress();
    std::string target = "http://" + ip + ":8090" + path;
    std::string res = "HTTP/1.1 302 Found\r\nLocation: " + target + "\r\nConnection: close\r\n\r\n";
    send(clientFd, res.c_str(), res.length(), 0);
    return;
  } else if (method == "GET" &&
             (path == "/debug.log" || path == "/api/debug_log")) {
    // Ho tro query ?lines=N de tail N dong cuoi (browser preview)
    int tailLines = 0;
    if (!queryString.empty()) {
      auto eq = queryString.find("lines=");
      if (eq != std::string::npos) {
        try { tailLines = std::stoi(queryString.substr(eq + 6)); } catch (...) {}
      }
    }

    std::string logPath = AppConfig::instance().getDebugLogPath();
    std::ifstream file(logPath, std::ios::binary);
    std::string content;
    if (file.is_open()) {
      if (tailLines > 0) {
        // Tail N dong: doc het -> dem newline nguoc tu cuoi
        content.assign((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
        file.close();
        int newlines = 0;
        long cutAt = -1;
        for (long i = (long)content.size() - 1; i >= 0; --i) {
          if (content[i] == '\n') {
            newlines++;
            if (newlines > tailLines) { cutAt = i + 1; break; }
          }
        }
        if (cutAt >= 0) content = content.substr(cutAt);
        // prepend note
        content = "[RomCloud tail " + std::to_string(tailLines) +
                  " lines from " + logPath + "]\n" + content;
      } else {
        content.assign((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
        file.close();
      }
    } else {
      content = "Chưa có lỗi nào được ghi nhận trong debug.log. Log path: " + logPath;
    }
    std::string res =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain; charset=UTF-8\r\n"
        "Content-Disposition: attachment; filename=\"debug.log\"\r\n"
        "Cache-Control: no-cache, no-store\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Length: " +
        std::to_string(content.length()) +
        "\r\n"
        "Connection: close\r\n\r\n" +
        content;
    send(clientFd, res.c_str(), res.length(), 0);
    return;
  } else if (method == "GET" && path == "/api/systems") {
    if (!AuthManager::instance().isLinked()) {
      std::string res = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json; charset=UTF-8\r\n"
                        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Content-Length: 2\r\n"
                        "Connection: close\r\n\r\n[]";
      send(clientFd, res.c_str(), res.length(), 0);
      return;
    }
    auto systems = DatabaseManager::instance().getSystems(true);
    std::string json = "[";
    for (size_t i = 0; i < systems.size(); ++i) {
      const auto &s = systems[i];
      if (i > 0)
        json += ",";
      json += "{\"id\":" + std::to_string(s.id) + ",";
      json += "\"code\":\"" + escapeJson(s.code) + "\",";
      json += "\"name\":\"" + escapeJson(s.name) + "\",";
      json += "\"local_count\":" + std::to_string(s.localCount) + ",";
      json += "\"cloud_count\":" + std::to_string(s.cloudCount) + "}";
    }
    json += "]";
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "GET" && path == "/api/games") {
    if (!AuthManager::instance().isLinked()) {
      std::string json = "{\"total\":0,\"games\":[]}";
      std::string res = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json; charset=UTF-8\r\n"
                        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Content-Length: " +
                        std::to_string(json.length()) +
                        "\r\n"
                        "Connection: close\r\n\r\n" +
                        json;
      send(clientFd, res.c_str(), res.length(), 0);
      return;
    }
    std::string sysIdStr = extractQueryParam(queryString, "system_id");
    std::string stateStr = extractQueryParam(queryString, "state");
    std::string q = extractQueryParam(queryString, "q");
    std::string limitStr = extractQueryParam(queryString, "limit");
    std::string offsetStr = extractQueryParam(queryString, "offset");

    int systemId = 0;
    int stateFilter = -1;
    int limit = 50;
    int offset = 0;

    try {
      if (!sysIdStr.empty())
        systemId = std::stoi(sysIdStr);
    } catch (...) {
    }
    try {
      if (!stateStr.empty())
        stateFilter = std::stoi(stateStr);
    } catch (...) {
    }
    try {
      if (!limitStr.empty())
        limit = std::stoi(limitStr);
    } catch (...) {
    }
    try {
      if (!offsetStr.empty())
        offset = std::stoi(offsetStr);
    } catch (...) {
    }

    int totalCount = 0;
    auto games = DatabaseManager::instance().getGamesFiltered(
        systemId, stateFilter, q, limit, offset, totalCount);

    std::string json =
        "{\"total\":" + std::to_string(totalCount) + ",\"games\":[";
    for (size_t i = 0; i < games.size(); ++i) {
      const auto &g = games[i];
      bool inQueue = DownloadManager::instance().isInQueue(g.id);
      if (i > 0)
        json += ",";
      json += "{\"id\":" + std::to_string(g.id) + ",";
      json += "\"title\":\"" + escapeJson(g.title) + "\",";
      json += "\"filename\":\"" + escapeJson(g.filename) + "\",";
      json += "\"sys_code\":\"" +
              escapeJson(g.systemCode.empty() ? "GAME" : g.systemCode) + "\",";
      json += "\"size_str\":\"" +
              FileSystemManager::instance().formatBytes(g.sizeBytes) + "\",";
      json +=
          "\"local_state\":" + std::to_string(static_cast<int>(g.localState)) +
          ",";
      json += "\"in_queue\":" + std::string(inQueue ? "true" : "false") + ",";
      json += "\"has_cover\":" +
              std::string(g.coverPath.empty() ? "false" : "true") + ",";
      json += "\"release_year\":\"" + escapeJson(g.releaseYear) + "\",";
      json += "\"genre\":\"" + escapeJson(g.genre) + "\",";
      json += "\"developer\":\"" + escapeJson(g.developer) + "\",";
      json += "\"description\":\"" + escapeJson(g.description) + "\"}";
    }
    json += "]}";

    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "GET" && path == "/api/game_detail") {
    std::string gameIdStr = extractQueryParam(queryString, "game_id");
    int64_t gameId = 0;
    try {
      gameId = std::stoll(gameIdStr);
    } catch (...) {
    }
    GameRecord g;
    if (gameId > 0 && DatabaseManager::instance().getGameById(gameId, g)) {
      SystemRecord sys;
      DatabaseManager::instance().getSystemById(g.systemId, sys);
      bool inQueue = DownloadManager::instance().isInQueue(g.id);
      std::string json = "{";
      json += "\"id\":" + std::to_string(g.id) + ",";
      json += "\"title\":\"" + escapeJson(g.title) + "\",";
      json += "\"filename\":\"" + escapeJson(g.filename) + "\",";
      json += "\"system_id\":" + std::to_string(g.systemId) + ",";
      json += "\"sys_code\":\"" +
              escapeJson(sys.code.empty() ? g.systemCode : sys.code) + "\",";
      json += "\"sys_name\":\"" + escapeJson(sys.name) + "\",";
      json += "\"size_str\":\"" +
              FileSystemManager::instance().formatBytes(g.sizeBytes) + "\",";
      json +=
          "\"local_state\":" + std::to_string(static_cast<int>(g.localState)) +
          ",";
      json += "\"in_queue\":" + std::string(inQueue ? "true" : "false") + ",";
      json += "\"has_cover\":" +
              std::string(g.coverPath.empty() ? "false" : "true") + ",";
      json += "\"release_year\":\"" + escapeJson(g.releaseYear) + "\",";
      json += "\"genre\":\"" + escapeJson(g.genre) + "\",";
      json += "\"developer\":\"" + escapeJson(g.developer) + "\",";
      json += "\"description\":\"" + escapeJson(g.description) + "\"";
      json += "}";
      std::string res = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json; charset=UTF-8\r\n"
                        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Content-Length: " +
                        std::to_string(json.length()) +
                        "\r\n"
                        "Connection: close\r\n\r\n" +
                        json;
      send(clientFd, res.c_str(), res.length(), 0);
      return;
    } else {
      std::string json = "{\"error\":\"Game not found\"}";
      std::string res = "HTTP/1.1 404 Not Found\r\n"
                        "Content-Type: application/json; charset=UTF-8\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Content-Length: " +
                        std::to_string(json.length()) +
                        "\r\n"
                        "Connection: close\r\n\r\n" +
                        json;
      send(clientFd, res.c_str(), res.length(), 0);
      return;
    }
  } else if (method == "GET" && path == "/api/search") {
    if (!AuthManager::instance().isLinked()) {
      std::string res = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json; charset=UTF-8\r\n"
                        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Content-Length: 2\r\n"
                        "Connection: close\r\n\r\n[]";
      send(clientFd, res.c_str(), res.length(), 0);
      return;
    }
    std::string q = extractQueryParam(queryString, "q");
    auto games = DatabaseManager::instance().searchAllGames(q, 60);
    std::string json = "[";
    for (size_t i = 0; i < games.size(); ++i) {
      const auto &g = games[i];
      SystemRecord sys;
      std::string sysCode = "GAME";
      if (DatabaseManager::instance().getSystemById(g.systemId, sys)) {
        sysCode = sys.code;
      }
      if (i > 0)
        json += ",";
      json += "{\"id\":" + std::to_string(g.id) + ",";
      json += "\"title\":\"" + escapeJson(g.title) + "\",";
      json += "\"filename\":\"" + escapeJson(g.filename) + "\",";
      json += "\"sys_code\":\"" + escapeJson(sysCode) + "\",";
      json += "\"size_str\":\"" +
              FileSystemManager::instance().formatBytes(g.sizeBytes) + "\",";
      json +=
          "\"local_state\":" + std::to_string(static_cast<int>(g.localState)) +
          ",";
      json += "\"has_cover\":" +
              std::string(g.coverPath.empty() ? "false" : "true") + "}";
    }
    json += "]";
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "GET" && path == "/api/cover") {
    std::string gameIdStr = extractQueryParam(queryString, "game_id");
    int64_t gameId = 0;
    try {
      if (!gameIdStr.empty())
        gameId = std::stoll(gameIdStr);
    } catch (...) {
    }

    GameRecord game;
    bool found =
        (gameId > 0 && DatabaseManager::instance().getGameById(gameId, game) &&
         !game.coverPath.empty());
    if (found) {
      std::ifstream file(game.coverPath, std::ios::binary);
      if (file.is_open()) {
        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        file.close();
        std::string mime = "image/png";
        if (game.coverPath.rfind(".jpg") != std::string::npos ||
            game.coverPath.rfind(".jpeg") != std::string::npos) {
          mime = "image/jpeg";
        } else if (game.coverPath.rfind(".webp") != std::string::npos) {
          mime = "image/webp";
        }
        std::string header = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: " +
                             mime +
                             "\r\n"
                             "Cache-Control: public, max-age=86400\r\n"
                             "Access-Control-Allow-Origin: *\r\n"
                             "Content-Length: " +
                             std::to_string(content.size()) +
                             "\r\n"
                             "Connection: close\r\n\r\n";
        send(clientFd, header.c_str(), header.length(), 0);
        send(clientFd, content.data(), content.size(), 0);
        return;
      }
    }
    std::string notFound = "HTTP/1.1 404 Not Found\r\nContent-Length: "
                           "0\r\nConnection: close\r\n\r\n";
    send(clientFd, notFound.c_str(), notFound.length(), 0);
    return;
  } else if (method == "GET" && path == "/api/download_status") {
    auto prog = DownloadManager::instance().getProgress();
    auto queue = DownloadManager::instance().getQueue();
    bool isDownloading = DownloadManager::instance().isDownloading();

    std::string stateStr = "IDLE";
    if (prog.state == DownloadState::INITIALIZING)
      stateStr = "INITIALIZING";
    else if (prog.state == DownloadState::DOWNLOADING)
      stateStr = "DOWNLOADING";
    else if (prog.state == DownloadState::VERIFYING)
      stateStr = "VERIFYING";
    else if (prog.state == DownloadState::COMPLETED)
      stateStr = "COMPLETED";
    else if (prog.state == DownloadState::FAILED)
      stateStr = "FAILED";
    else if (prog.state == DownloadState::CANCELLED)
      stateStr = "CANCELLED";

    std::string json = "{";
    json +=
        "\"is_downloading\":" + std::string(isDownloading ? "true" : "false") +
        ",";
    json += "\"active\":{";
    json += "\"state\":\"" + stateStr + "\",";
    json += "\"game_id\":" + std::to_string(prog.gameId) + ",";
    json += "\"title\":\"" + escapeJson(prog.gameTitle) + "\",";
    json += "\"system\":\"" + escapeJson(prog.systemCode) + "\",";
    json += "\"filename\":\"" + escapeJson(prog.filename) + "\",";
    json += "\"progress_pct\":" + std::to_string(prog.progressPct) + ",";
    json += "\"speed_kbps\":" + std::to_string(prog.speedKBps) + ",";
    json +=
        "\"bytes_downloaded\":" + std::to_string(prog.bytesDownloaded) + ",";
    json += "\"total_bytes\":" + std::to_string(prog.totalBytes) + ",";
    json += "\"eta_seconds\":" + std::to_string(prog.etaSeconds) + ",";
    json += "\"error\":\"" + escapeJson(prog.errorMessage) + "\"";
    json += "},";
    json += "\"queue_size\":" + std::to_string(queue.size()) + ",";
    json += "\"queue\":[";
    for (size_t i = 0; i < queue.size(); ++i) {
      const auto &it = queue[i];
      if (i > 0)
        json += ",";
      json += "{\"game_id\":" + std::to_string(it.game.id) + ",";
      json += "\"title\":\"" + escapeJson(it.game.title) + "\",";
      json += "\"system\":\"" + escapeJson(it.sys.code) + "\",";
      json += "\"size_str\":\"" +
              FileSystemManager::instance().formatBytes(it.game.sizeBytes) +
              "\"}";
    }
    json += "]}";

    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/download_game") {
    std::string gameIdStr = extractPostParam(postBody, "game_id");
    int64_t gameId = 0;
    try {
      gameId = std::stoll(gameIdStr);
    } catch (...) {
    }

    bool ok = false;
    std::string msg = "Không tìm thấy game";
    if (gameId > 0) {
      GameRecord g;
      if (DatabaseManager::instance().getGameById(gameId, g)) {
        SystemRecord sys;
        if (DatabaseManager::instance().getSystemById(g.systemId, sys)) {
          ok = DownloadManager::instance().addToQueue(g, sys);
          if (ok) {
            msg = "Đã thêm \"" + g.title + "\" vào hàng tải.";
            if (!DownloadManager::instance().isDownloading()) {
              DownloadManager::instance().processNextInQueue();
            }
          } else {
            msg = "Game đã có trong danh sách hoặc đã tải về thẻ.";
          }
        }
      }
    }
    std::string json = "{\"success\":" + std::string(ok ? "true" : "false") +
                       ",\"message\":\"" + escapeJson(msg) + "\"}";
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/cancel_download") {
    DownloadManager::instance().cancelDownload();
    std::string json =
        "{\"success\":true,\"message\":\"Đã hủy tải.\"}";
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/remove_queue") {
    std::string gameIdStr = extractPostParam(postBody, "game_id");
    int64_t gameId = 0;
    try {
      gameId = std::stoll(gameIdStr);
    } catch (...) {
    }
    bool ok = DownloadManager::instance().removeFromQueue(gameId);
    std::string json =
        "{\"success\":" + std::string(ok ? "true" : "false") +
        ",\"message\":\"" +
        (ok ? "Đã xóa khỏi hàng đợi." : "Không tìm thấy game trong hàng đợi.") +
        "\"}";
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/clear_queue") {
    DownloadManager::instance().clearQueue();
    std::string json =
        "{\"success\":true,\"message\":\"Đã xóa hết hàng đợi.\"}";
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/delete_rom") {
    std::string gameIdStr = extractPostParam(postBody, "game_id");
    int64_t gameId = 0;
    try {
      gameId = std::stoll(gameIdStr);
    } catch (...) {
    }

    bool ok = false;
    std::string msg = "Lỗi khi xóa ROM";
    if (gameId > 0) {
      GameRecord g;
      if (DatabaseManager::instance().getGameById(gameId, g)) {
        ok = DatabaseManager::instance().markGameDeletedLocally(gameId);
        if (ok) {
          msg = "Đã xóa \"" + g.title + "\" khỏi thẻ.";
        }
      }
    }
    std::string json = "{\"success\":" + std::string(ok ? "true" : "false") +
                       ",\"message\":\"" + escapeJson(msg) + "\"}";
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" &&
             (path == "/api/scrape_cover" || path == "/api/scrape_game")) {
    std::string gameIdStr = extractPostParam(postBody, "game_id");
    int64_t gameId = 0;
    try {
      gameId = std::stoll(gameIdStr);
    } catch (...) {
    }

    bool ok = false;
    std::string coverPath;
    std::string releaseYear;
    std::string genre;
    std::string developer;
    std::string desc;
    std::string title;
    std::string msg = "Không lấy được thông tin game";

    if (gameId > 0) {
      GameRecord g;
      if (DatabaseManager::instance().getGameById(gameId, g)) {
        SystemRecord sys;
        if (DatabaseManager::instance().getSystemById(g.systemId, sys)) {
          auto scrapeRes = BoxartScraper::instance().scrapeGameInfo(g, sys);
          ok = scrapeRes.success;
          coverPath = scrapeRes.coverPath;
          releaseYear = scrapeRes.releaseYear;
          genre = scrapeRes.genre;
          developer = scrapeRes.developer;
          desc = scrapeRes.description;
          title = scrapeRes.title;
          if (ok) {
            msg = "Đã lấy bìa và thông tin cho \"" +
                  (title.empty() ? g.title : title) + "\"!";
          } else {
            msg = "Không tìm thấy dữ liệu trên Libretro.";
          }
        }
      }
    }
    std::string json = "{";
    json += "\"success\":" + std::string(ok ? "true" : "false") + ",";
    json += "\"cover_path\":\"" + escapeJson(coverPath) + "\",";
    json += "\"title\":\"" + escapeJson(title) + "\",";
    json += "\"release_year\":\"" + escapeJson(releaseYear) + "\",";
    json += "\"genre\":\"" + escapeJson(genre) + "\",";
    json += "\"developer\":\"" + escapeJson(developer) + "\",";
    json += "\"description\":\"" + escapeJson(desc) + "\",";
    json += "\"message\":\"" + escapeJson(msg) + "\"";
    json += "}";
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/auto_scrape_sd") {
    bool force = (extractPostParam(postBody, "force") == "1");
    bool started = BoxartScraper::instance().startAutoScrapeSdCard(force);
    std::string msg =
        started
            ? "Đã bắt đầu tự lấy ảnh bìa và thông tin trên thẻ."
            : "Không có game nào cần lấy bìa, hoặc đang chạy rồi.";
    std::string json =
        "{\"success\":" + std::string(started ? "true" : "false") +
        ",\"message\":\"" + escapeJson(msg) + "\"}";
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "GET" && path == "/api/auto_scrape_status") {
    auto st = BoxartScraper::instance().getAutoScrapeStatus();
    std::string json = "{";
    json += "\"is_scraping\":" + std::string(st.isScraping ? "true" : "false") +
            ",";
    json += "\"total\":" + std::to_string(st.totalGames) + ",";
    json += "\"scraped\":" + std::to_string(st.scrapedCount) + ",";
    json += "\"success\":" + std::to_string(st.successCount) + ",";
    json += "\"progress_pct\":" + std::to_string(st.progressPct) + ",";
    json += "\"current_game\":\"" + escapeJson(st.currentGame) + "\",";
    json += "\"current_sys\":\"" + escapeJson(st.currentSystem) + "\",";
    json += "\"message\":\"" + escapeJson(st.lastMessage) + "\"";
    json += "}";
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/cancel_auto_scrape") {
    BoxartScraper::instance().cancelAutoScrape();
    std::string json =
        "{\"success\":true,\"message\":\"Đã yêu cầu hủy.\"}";
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/search_scraper") {
    std::string query = extractPostParam(postBody, "query");
    std::string sysCode = extractPostParam(postBody, "system_code");
    auto cands = BoxartScraper::instance().searchCandidates(query, sysCode);

    std::string json = "{\"success\":true,\"candidates\":[";
    for (size_t i = 0; i < cands.size(); ++i) {
      if (i > 0)
        json += ",";
      json += "{";
      json += "\"title\":\"" + escapeJson(cands[i].title) + "\",";
      json += "\"release_year\":\"" + escapeJson(cands[i].releaseYear) + "\",";
      json += "\"developer\":\"" + escapeJson(cands[i].developer) + "\",";
      json += "\"genre\":\"" + escapeJson(cands[i].genre) + "\",";
      json += "\"description\":\"" + escapeJson(cands[i].description) + "\",";
      json += "\"cover_url\":\"" + escapeJson(cands[i].coverUrl) + "\",";
      json += "\"source\":\"" + escapeJson(cands[i].source) + "\"";
      json += "}";
    }
    json += "]}";

    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/apply_candidate") {
    std::string gameIdStr = extractPostParam(postBody, "game_id");
    int64_t gameId = 0;
    try {
      gameId = std::stoll(gameIdStr);
    } catch (...) {
    }

    ScrapeCandidate cand;
    cand.title = extractPostParam(postBody, "title");
    cand.releaseYear = extractPostParam(postBody, "year");
    cand.genre = extractPostParam(postBody, "genre");
    cand.developer = extractPostParam(postBody, "developer");
    cand.description = extractPostParam(postBody, "description");
    cand.coverUrl = extractPostParam(postBody, "cover_url");

    bool ok = false;
    if (gameId > 0) {
      ok = BoxartScraper::instance().applyCandidate(gameId, cand);
    }

    std::string json = "{\"success\":" + std::string(ok ? "true" : "false") +
                       ",\"message\":\"" +
                       (ok ? "Đã áp dụng bìa và thông tin!"
                           : "Lỗi khi áp dụng.") +
                       "\"}";
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/audit_roms") {
    auto list = RomOrganizer::instance().auditMisplacedRoms();
    std::string json =
        "{\"success\":true,\"count\":" + std::to_string(list.size()) +
        ",\"items\":[";
    for (size_t i = 0; i < list.size(); ++i) {
      if (i > 0)
        json += ",";
      json += "{";
      json += "\"filename\":\"" + escapeJson(list[i].filename) + "\",";
      json += "\"original_path\":\"" + escapeJson(list[i].originalPath) + "\",";
      json += "\"current_system\":\"" + escapeJson(list[i].currentSystemCode) +
              "\",";
      json += "\"detected_system\":\"" +
              escapeJson(list[i].detectedSystemCode) + "\",";
      json += "\"detected_name\":\"" + escapeJson(list[i].detectedSystemName) +
              "\",";
      json += "\"target_path\":\"" + escapeJson(list[i].targetPath) + "\",";
      json += "\"confidence\":\"" + escapeJson(list[i].confidence) + "\",";
      json += "\"reason\":\"" + escapeJson(list[i].reason) + "\"";
      json += "}";
    }
    json += "]}";
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/fix_misplaced_roms") {
    auto list = RomOrganizer::instance().fixMisplacedRoms();
    // Re-index ROMs so library updates immediately
    std::string romsDir = AppConfig::instance().getRomsDir();
    std::thread([romsDir]() {
      RomIndexer::instance().scanAllSystems(romsDir, nullptr);
    }).detach();
    std::string json =
        "{\"success\":true,\"count\":" + std::to_string(list.size()) +
        ",\"items\":[";
    for (size_t i = 0; i < list.size(); ++i) {
      if (i > 0)
        json += ",";
      json += "{";
      json += "\"filename\":\"" + escapeJson(list[i].filename) + "\",";
      json += "\"current_system\":\"" + escapeJson(list[i].currentSystemCode) +
              "\",";
      json += "\"detected_system\":\"" +
              escapeJson(list[i].detectedSystemCode) + "\",";
      json += "\"detected_name\":\"" + escapeJson(list[i].detectedSystemName) +
              "\",";
      json +=
          "\"fixed\":" + std::string(list[i].fixed ? "true" : "false") + ",";
      json += "\"status\":\"" + escapeJson(list[i].statusMessage) + "\"";
      json += "}";
    }
    json += "]}";
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/organize_inbox") {
    auto list = RomOrganizer::instance().organizeDirectory(
        RomOrganizer::instance().getInboxDir());
    std::string json =
        "{\"success\":true,\"count\":" + std::to_string(list.size()) + "}";
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/upload_rom_auto") {
    std::string fname = extractQueryParam(queryString, "filename");
    if (fname.empty())
      fname = "uploaded_rom.bin";

    std::string inboxPath =
        RomOrganizer::instance().getInboxDir() + "/" + fname;
    std::ofstream outFile(inboxPath, std::ios::binary);
    if (outFile.is_open()) {
      if (!postBody.empty()) {
        outFile.write(postBody.data(), postBody.size());
      }

      size_t clPos = req.find("Content-Length: ");
      if (clPos != std::string::npos) {
        size_t clEnd = req.find("\r\n", clPos);
        long totalCl = 0;
        try {
          totalCl = std::stol(req.substr(clPos + 16, clEnd - (clPos + 16)));
        } catch (...) {
        }

        long bytesReadSoFar = static_cast<long>(postBody.size());
        char chunk[32768];
        while (bytesReadSoFar < totalCl) {
          long toRead = std::min<long>(sizeof(chunk), totalCl - bytesReadSoFar);
          int n = recv(clientFd, chunk, toRead, 0);
          if (n <= 0)
            break;
          outFile.write(chunk, n);
          bytesReadSoFar += n;
        }
      }
      outFile.close();
      sync();

      RomDetectionResult det = RomDetector::instance().detectSystem(inboxPath);
      std::string sysCode = det.detected ? det.systemCode : "GBA";
      std::string sysName = det.detected ? det.systemName : "Game";

      SystemRecord targetSys;
      std::string finalDst;
      std::string message;
      if (DatabaseManager::instance().getSystemByCode(sysCode, targetSys)) {
        std::string targetDir =
            AppConfig::instance().getRomsDir() + "/" + targetSys.romDir;
        FileSystemManager::instance().createDirectoryRecursive(targetDir);
        std::string targetFile = targetDir + "/" + fname;
        RomOrganizer::instance().safeMoveFile(inboxPath, targetFile, finalDst);

        GameRecord g;
        g.systemId = targetSys.id;
        g.filename = fname;
        size_t dot = fname.rfind('.');
        g.title = (dot != std::string::npos) ? fname.substr(0, dot) : fname;
        g.localPath = finalDst;
        g.localState = GameState::LOCAL;
        struct stat st;
        if (::stat(finalDst.c_str(), &st) == 0)
          g.sizeBytes = st.st_size;
        DatabaseManager::instance().upsertGame(g);

        message = "Đã nhận diện: " + sysName + " (" + sysCode +
                  "). Đã lưu vào /Roms/" + targetSys.romDir + "/";
      } else {
        message = "Chưa rõ hệ máy, đã lưu vào thư mục _INBOX.";
      }

      std::string json = "{\"success\":true,\"filename\":\"" +
                         escapeJson(fname) + "\",\"system_code\":\"" +
                         escapeJson(sysCode) + "\",\"system_name\":\"" +
                         escapeJson(sysName) + "\",\"message\":\"" +
                         escapeJson(message) + "\"}";
      std::string res = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json; charset=UTF-8\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Content-Length: " +
                        std::to_string(json.length()) +
                        "\r\n"
                        "Connection: close\r\n\r\n" +
                        json;
      send(clientFd, res.c_str(), res.length(), 0);
    } else {
      std::string json =
          "{\"success\":false,\"error\":\"Không thể ghi file vào thẻ nhớ.\"}";
      std::string res = "HTTP/1.1 500 Internal Server Error\r\n"
                        "Content-Type: application/json; charset=UTF-8\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Content-Length: " +
                        std::to_string(json.length()) +
                        "\r\n"
                        "Connection: close\r\n\r\n" +
                        json;
      send(clientFd, res.c_str(), res.length(), 0);
    }
  } else if (method == "GET" && path == "/api/storage_info") {
    auto disk = FileSystemManager::instance().getDiskSpace(
        AppConfig::instance().getRomsDir());
    bool isLinked = AuthManager::instance().isLinked();
    bool canUpload = AuthManager::instance().canUpload();
    bool isPublicOnly = AuthManager::instance().isPublicOnly();
    int totalLocal = 0, totalCloud = 0;
    if (isLinked) {
      DatabaseManager::instance().getTotalGameCounts(totalLocal, totalCloud);
    }
    std::string lastSync = isLinked
                               ? DriveSyncEngine::instance().getLastSyncTime()
                               : "Chưa kết nối";
    std::string driveUrl =
        isLinked
            ? DatabaseManager::instance().getSetting("drive_folder_url", "")
            : "";

    uint64_t usedBytes = (disk.totalBytes > disk.availableBytes)
                             ? (disk.totalBytes - disk.availableBytes)
                             : 0;
    double usedPct = 0.0;
    if (disk.totalBytes > 0) {
      usedPct = (static_cast<double>(usedBytes) /
                 static_cast<double>(disk.totalBytes)) *
                100.0;
    }

    std::string userEmail =
        isLinked ? AuthManager::instance().getUserEmail() : "";

    std::string json = "{";
    json += "\"is_linked\":" + std::string(isLinked ? "true" : "false") + ",";
    json += "\"can_upload\":" + std::string(canUpload ? "true" : "false") + ",";
    json +=
        "\"is_public_only\":" + std::string(isPublicOnly ? "true" : "false") +
        ",";
    json += "\"user_email\":\"" + escapeJson(userEmail) + "\",";
    json += "\"total_bytes\":" + std::to_string(disk.totalBytes) + ",";
    json += "\"avail_bytes\":" + std::to_string(disk.availableBytes) + ",";
    json += "\"used_bytes\":" + std::to_string(usedBytes) + ",";
    json += "\"total_str\":\"" +
            FileSystemManager::instance().formatBytes(disk.totalBytes) + "\",";
    json += "\"avail_str\":\"" +
            FileSystemManager::instance().formatBytes(disk.availableBytes) +
            "\",";
    json += "\"used_str\":\"" +
            FileSystemManager::instance().formatBytes(usedBytes) + "\",";
    json += "\"used_pct\":" + std::to_string(usedPct) + ",";
    json += "\"total_local\":" + std::to_string(totalLocal) + ",";
    json += "\"total_cloud\":" + std::to_string(totalCloud) + ",";
    json += "\"last_sync\":\"" + escapeJson(lastSync) + "\",";
    json += "\"drive_url\":\"" + escapeJson(driveUrl) + "\"";
    json += "}";

    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/trigger_sync") {
    bool started = DriveSyncEngine::instance().startSync();
    std::string json =
        "{\"success\":" + std::string(started ? "true" : "false") + "}";
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "GET" && path == "/api/sync_status") {
    auto prog = DriveSyncEngine::instance().getProgress();
    bool isSyncing = DriveSyncEngine::instance().isSyncing();
    std::string statusStr = "IDLE";
    if (prog.status == SyncStatus::CONNECTING)
      statusStr = "CONNECTING";
    else if (prog.status == SyncStatus::DISCOVERING_FOLDERS)
      statusStr = "DISCOVERING_FOLDERS";
    else if (prog.status == SyncStatus::SYNCING_FILES)
      statusStr = "SYNCING_FILES";
    else if (prog.status == SyncStatus::COMPLETED)
      statusStr = "COMPLETED";
    else if (prog.status == SyncStatus::ERROR_OCCURRED)
      statusStr = "ERROR";

    std::string json = "{";
    json += "\"is_syncing\":" + std::string(isSyncing ? "true" : "false") + ",";
    json += "\"status\":\"" + statusStr + "\",";
    json +=
        "\"current_platform\":\"" + escapeJson(prog.currentPlatform) + "\",";
    json += "\"games_found\":" + std::to_string(prog.cloudGamesFound) + ",";
    json += "\"games_indexed\":" + std::to_string(prog.newGamesIndexed) + ",";
    json +=
        "\"current_system_index\":" + std::to_string(prog.currentSystemIndex) +
        ",";
    json += "\"total_systems\":" + std::to_string(prog.totalSystems) + ",";
    json += "\"error\":\"" + escapeJson(prog.errorMessage) + "\"";
    json += "}";

    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && (path == "/api/logout" || path == "/unlink")) {
    AuthManager::instance().logout();
    if (path == "/api/logout") {
      std::string json = "{\"success\":true,\"message\":\"Đã đăng xuất Google Drive.\"}";
      std::string res = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json; charset=UTF-8\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Content-Length: " +
                        std::to_string(json.length()) +
                        "\r\n"
                        "Connection: close\r\n\r\n" +
                        json;
      send(clientFd, res.c_str(), res.length(), 0);
    } else {
      std::string body =
          buildSuccessResponse("Đã hủy liên kết Drive.");
      std::string res = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/html; charset=UTF-8\r\n"
                        "Content-Length: " +
                        std::to_string(body.length()) +
                        "\r\n"
                        "Connection: close\r\n\r\n" +
                        body;
      send(clientFd, res.c_str(), res.length(), 0);
    }
  } else if (method == "POST" && path == "/connect") {
    std::string driveUrl = extractPostParam(postBody, "drive_url");
    std::string folderId = extractFolderId(driveUrl);
    if (folderId.empty())
      folderId = driveUrl;

    Logger::info("User linked Google Drive folder via Web Portal: " + driveUrl +
                 " (Extracted Folder ID: " + folderId + ")");
    if (oneDriveIsShareLink(driveUrl)) {
      DatabaseManager::instance().setSetting("cloud_provider", "onedrive");
      DatabaseManager::instance().setSetting("onedrive_share_url", driveUrl);
      AuthManager::instance().linkPublicFolder("onedrive", driveUrl);
      Logger::info("User linked OneDrive share via Web Portal: " + driveUrl);
    } else {
      DatabaseManager::instance().setSetting("cloud_provider", "drive");
      AuthManager::instance().linkPublicFolder(folderId, driveUrl);
    }
    DriveSyncEngine::instance().startSync();

    std::string body =
        buildSuccessResponse("Đã lưu. Đang quét Drive...");
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/html; charset=UTF-8\r\n"
                      "Content-Length: " +
                      std::to_string(body.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      body;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/set_personal_auth") {
    std::string action = extractPostParam(postBody, "action");
    if (action == "clear") {
      AuthManager::instance().clearPersonalTokens();
      std::string json = "{\"success\":true,\"message\":\"Đã hủy quyền sao lưu.\"}";
      std::string res = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json; charset=UTF-8\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Content-Length: " +
                        std::to_string(json.length()) +
                        "\r\n"
                        "Connection: close\r\n\r\n" +
                        json;
      send(clientFd, res.c_str(), res.length(), 0);
      return;
    }

    std::string token = extractPostParam(postBody, "token");
    std::string refreshToken = extractPostParam(postBody, "refresh_token");
    std::string email = extractPostParam(postBody, "email");
    if (token.empty() && refreshToken.empty()) {
      std::string json = "{\"success\":false,\"error\":\"Hãy dán Access "
                         "Token hoặc Refresh Token.\"}";
      std::string res = "HTTP/1.1 400 Bad Request\r\n"
                        "Content-Type: application/json; charset=UTF-8\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Content-Length: " +
                        std::to_string(json.length()) +
                        "\r\n"
                        "Connection: close\r\n\r\n" +
                        json;
      send(clientFd, res.c_str(), res.length(), 0);
      return;
    }
    auto result =
        AuthManager::instance().setPersonalTokens(token, refreshToken, email);
    if (result.success) {
      std::string json = "{\"success\":true,\"message\":\"" +
                         escapeJson(result.message) + "\",\"email\":\"" +
                         escapeJson(result.userEmail) + "\"}";
      std::string res = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json; charset=UTF-8\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Content-Length: " +
                        std::to_string(json.length()) +
                        "\r\n"
                        "Connection: close\r\n\r\n" +
                        json;
      send(clientFd, res.c_str(), res.length(), 0);
    } else {
      std::string json = "{\"success\":false,\"error\":\"" +
                         escapeJson(result.message) + "\"}";
      std::string res = "HTTP/1.1 400 Bad Request\r\n"
                        "Content-Type: application/json; charset=UTF-8\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "Content-Length: " +
                        std::to_string(json.length()) +
                        "\r\n"
                        "Connection: close\r\n\r\n" +
                        json;
      send(clientFd, res.c_str(), res.length(), 0);
    }
  } else if (method == "POST" && path == "/api/device_flow_start") {
    bool ok = AuthManager::instance().startDeviceFlow();
    if (ok) {
      DeviceCodeResponse info = AuthManager::instance().getDeviceCodeInfo();
      std::string json = "{\"success\":true,\"user_code\":\"" + escapeJson(info.userCode) +
                         "\",\"verification_url\":\"" + escapeJson(info.verificationUrl) +
                         "\",\"expires_in\":" + std::to_string(info.expiresIn) +
                         ",\"interval\":" + std::to_string(info.interval) + "}";
      std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " +
                        std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
      send(clientFd, res.c_str(), res.length(), 0);
    } else {
      std::string err = AuthManager::instance().getErrorMessage();
      std::string json = "{\"success\":false,\"error\":\"" + escapeJson(err) + "\"}";
      std::string res = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json; charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " +
                        std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
      send(clientFd, res.c_str(), res.length(), 0);
    }
    return;
  } else if (method == "GET" && path == "/api/device_flow_status") {
    AuthState st = AuthManager::instance().getState();
    std::string stStr = "UNLINKED";
    if (st == AuthState::LINKED) stStr = "LINKED";
    else if (st == AuthState::AWAITING_USER) stStr = "AWAITING_USER";
    else if (st == AuthState::REQUESTING_CODE) stStr = "REQUESTING_CODE";
    else if (st == AuthState::ERROR_OCCURRED) stStr = "ERROR";

    std::string json = "{\"status\":\"" + stStr + "\",\"is_linked\":" + (AuthManager::instance().isLinked() ? "true" : "false") +
                       ",\"can_upload\":" + (AuthManager::instance().canUpload() ? "true" : "false") +
                       ",\"email\":\"" + escapeJson(AuthManager::instance().getUserEmail()) +
                       "\",\"error\":\"" + escapeJson(AuthManager::instance().getErrorMessage()) + "\"}";
    std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " +
                      std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
    return;
  } else if (method == "POST" && path == "/api/device_flow_cancel") {
    AuthManager::instance().cancelDeviceFlow();
    std::string json = "{\"success\":true}";
    std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " +
                      std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
    return;
  } else if (method == "POST" && path == "/api/upload_game") {
    if (!AuthManager::instance().canUpload()) {
      std::string json = "{\"success\":false,\"error\":\"Chưa bật sao lưu. "
                         "Vào mục Đồng bộ & Thẻ nhớ để đăng nhập Google trước.\"}";
      std::string res =
          "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json; "
          "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
          "*\r\nContent-Length: " +
          std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" +
          json;
      send(clientFd, res.c_str(), res.length(), 0);
    } else {
      std::string gameIdStr = extractPostParam(postBody, "game_id");
      int64_t gameId = 0;
      try {
        gameId = std::stoll(gameIdStr);
      } catch (...) {
      }
      if (gameId > 0) {
        UploadManager::instance().startUploadGames({gameId});
        std::string json = "{\"success\":true,\"message\":\"Đang sao lưu lên Drive...\"}";
        std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
                          "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                          "*\r\nContent-Length: " +
                          std::to_string(json.length()) +
                          "\r\nConnection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
      } else {
        std::string json =
            "{\"success\":false,\"error\":\"ID game không hợp lệ.\"}";
        std::string res =
            "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json; "
            "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
            "*\r\nContent-Length: " +
            std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" +
            json;
        send(clientFd, res.c_str(), res.length(), 0);
      }
    }
  } else if (method == "POST" && path == "/api/upload_all") {
    if (!AuthManager::instance().canUpload()) {
      std::string json = "{\"success\":false,\"error\":\"Chưa bật sao lưu. "
                         "Vào mục Đồng bộ & Thẻ nhớ để đăng nhập Google trước.\"}";
      std::string res =
          "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json; "
          "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
          "*\r\nContent-Length: " +
          std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" +
          json;
      send(clientFd, res.c_str(), res.length(), 0);
    } else {
      UploadManager::instance().startReverseSync();
      std::string json =
          "{\"success\":true,\"message\":\"Đang sao lưu cả thẻ lên Drive...\"}";
      std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
                        "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                        "*\r\nContent-Length: " +
                        std::to_string(json.length()) +
                        "\r\nConnection: close\r\n\r\n" + json;
      send(clientFd, res.c_str(), res.length(), 0);
    }
  } else if (method == "GET" && path == "/api/upload_status") {
    auto prog = UploadManager::instance().getProgress();
    bool isUp = UploadManager::instance().isUploading();
    std::string stateStr = "IDLE";
    if (prog.state == UploadState::PREPARING)
      stateStr = "PREPARING";
    else if (prog.state == UploadState::UPLOADING)
      stateStr = "UPLOADING";
    else if (prog.state == UploadState::COMPLETED)
      stateStr = "COMPLETED";
    else if (prog.state == UploadState::FAILED)
      stateStr = "FAILED";
    else if (prog.state == UploadState::CANCELLED)
      stateStr = "CANCELLED";

    std::string json = "{";
    json += "\"is_uploading\":" + std::string(isUp ? "true" : "false") + ",";
    json += "\"state\":\"" + stateStr + "\",";
    json += "\"game_title\":\"" + escapeJson(prog.gameTitle) + "\",";
    json += "\"filename\":\"" + escapeJson(prog.filename) + "\",";
    json += "\"system\":\"" + escapeJson(prog.systemCode) + "\",";
    json += "\"progress_pct\":" + std::to_string(prog.progressPct) + ",";
    json += "\"bytes_uploaded\":" + std::to_string(prog.bytesUploaded) + ",";
    json += "\"total_bytes\":" + std::to_string(prog.totalBytes) + ",";
    json += "\"speed_kbps\":" + std::to_string(prog.speedKBps) + ",";
    json += "\"total_games\":" + std::to_string(prog.totalGames) + ",";
    json += "\"current_index\":" + std::to_string(prog.currentIndex) + ",";
    json += "\"games_uploaded\":" + std::to_string(prog.gamesUploaded) + ",";
    json += "\"games_failed\":" + std::to_string(prog.gamesFailed) + ",";
    json += "\"error\":\"" + escapeJson(prog.errorMessage) + "\"";
    json += "}";

    std::string res =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
        "charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " +
        std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/cancel_upload") {
    UploadManager::instance().cancel();
    std::string json =
        "{\"success\":true,\"message\":\"Đã yêu cầu hủy sao lưu.\"}";
    std::string res =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
        "charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " +
        std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "GET" && path == "/ota_check") {
    UpdateInfo info;
    bool hasUpdate = UpdateManager::instance().checkForUpdatesSync(info);
    std::string json =
        "{\"has_update\":" + std::string(hasUpdate ? "true" : "false") +
        ","
        "\"current_version\":\"" +
        UpdateManager::instance().getCurrentVersion() +
        "\","
        "\"remote_version\":\"" +
        info.remoteVersion +
        "\","
        "\"release_date\":\"" +
        info.releaseDate +
        "\","
        "\"changelog\":\"" +
        escapeJson(info.changelog) + "\"}";
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/ota_start") {
    auto info = UpdateManager::instance().getLatestInfo();
    bool ok = UpdateManager::instance().startUpdate(info);
    std::string json =
        "{\"started\":" + std::string(ok ? "true" : "false") + "}";
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "GET" && path == "/ota_status") {
    auto prog = UpdateManager::instance().getProgress();
    std::string stateStr = "IDLE";
    if (prog.state == UpdateState::CHECKING)
      stateStr = "CHECKING";
    else if (prog.state == UpdateState::UPDATE_AVAILABLE)
      stateStr = "UPDATE_AVAILABLE";
    else if (prog.state == UpdateState::UP_TO_DATE)
      stateStr = "UP_TO_DATE";
    else if (prog.state == UpdateState::DOWNLOADING)
      stateStr = "DOWNLOADING";
    else if (prog.state == UpdateState::VERIFYING)
      stateStr = "VERIFYING";
    else if (prog.state == UpdateState::INSTALLING)
      stateStr = "INSTALLING";
    else if (prog.state == UpdateState::DOWNLOADING_DEPS)
      stateStr = "DOWNLOADING_DEPS";
    else if (prog.state == UpdateState::INSTALLING_DEPS)
      stateStr = "INSTALLING_DEPS";
    else if (prog.state == UpdateState::COMPLETED)
      stateStr = "COMPLETED";
    else if (prog.state == UpdateState::FAILED)
      stateStr = "FAILED";

    std::string json = "{\"state\":\"" + stateStr +
                       "\","
                       "\"progress_pct\":" +
                       std::to_string(prog.progressPct) +
                       ","
                       "\"bytes_downloaded\":" +
                       std::to_string(prog.bytesDownloaded) +
                       ","
                       "\"total_bytes\":" +
                       std::to_string(prog.totalBytes) +
                       ","
                       "\"speed_kbps\":" +
                       std::to_string(prog.speedKBps) +
                       ","
                       "\"current_step\":\"" +
                       escapeJson(prog.currentStep) +
                       "\","
                       "\"error\":\"" +
                       escapeJson(prog.errorMessage) + "\"}";
    std::string res = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=UTF-8\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: " +
                      std::to_string(json.length()) +
                      "\r\n"
                      "Connection: close\r\n\r\n" +
                      json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/ota_restart") {
    std::string res = "HTTP/1.1 200 OK\r\nContent-Type: "
                      "application/json\r\n\r\n{\"status\":\"restarting\"}";
    send(clientFd, res.c_str(), res.length(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    Application::instance().requestRestart();
  } else if (method == "GET" && path == "/api/iptv/sources") {
    // Return structured IPTV sources with channel counts & metadata
    auto sources = IPTVManager::instance().getSources();
    size_t totalChannels = IPTVManager::instance().getChannels().size();
    std::string json = "{\"success\":true,\"total_channels\":" +
                       std::to_string(totalChannels) + ",\"sources\":[";
    for (size_t i = 0; i < sources.size(); i++) {
      if (i > 0)
        json += ",";
      json += "{\"name\":\"" + escapeJson(sources[i].name) + "\"," +
              "\"filename\":\"" + escapeJson(sources[i].filename) + "\"," +
              "\"type\":\"" + escapeJson(sources[i].type) + "\"," +
              "\"url\":\"" + escapeJson(sources[i].url) + "\"," +
              "\"channels\":" + std::to_string(sources[i].channelCount) + "," +
              "\"size\":" + std::to_string(sources[i].fileSize) + "}";
    }
    json += "]}";
    std::string res =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
        "charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " +
        std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "GET" && path == "/api/iptv/list") {
    // List IPTV playlists (compatibility endpoint)
    auto sources = IPTVManager::instance().getSources();
    std::string json = "{\"playlists\":[";
    for (size_t i = 0; i < sources.size(); i++) {
      if (i > 0)
        json += ",";
      json += "\"" + escapeJson(sources[i].filename) + "\"";
    }
    json += "]}";
    std::string res =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
        "charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " +
        std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/iptv/upload") {
    // Read full POST body according to Content-Length
    size_t clPos = req.find("Content-Length: ");
    if (clPos == std::string::npos)
      clPos = req.find("content-length: ");
    long totalCl = 0;
    if (clPos != std::string::npos) {
      size_t clEnd = req.find("\r\n", clPos);
      try {
        totalCl = std::stol(req.substr(clPos + 16, clEnd - (clPos + 16)));
      } catch (...) {
      }
    }

    std::string fullBody = postBody;
    if (totalCl > 0 && static_cast<long>(fullBody.size()) < totalCl) {
      char chunk[16384];
      while (static_cast<long>(fullBody.size()) < totalCl) {
        long toRead = std::min<long>(
            sizeof(chunk), totalCl - static_cast<long>(fullBody.size()));
        int n = recv(clientFd, chunk, toRead, 0);
        if (n <= 0)
          break;
        fullBody.append(chunk, n);
      }
    }

    // Parse multipart filename: filename="..."
    std::string filename = "uploaded.m3u";
    size_t fnPos = fullBody.find("filename=\"");
    if (fnPos != std::string::npos) {
      fnPos += 10;
      size_t fnEnd = fullBody.find("\"", fnPos);
      if (fnEnd != std::string::npos && fnEnd > fnPos) {
        filename = fullBody.substr(fnPos, fnEnd - fnPos);
        size_t slash = filename.find_last_of("/\\");
        if (slash != std::string::npos)
          filename = filename.substr(slash + 1);
      }
    }
    if (filename.empty() || filename == "." || filename == "..") {
      filename = "playlist_" + std::to_string(std::time(nullptr)) + ".m3u";
    }
    if (filename.find(".m3u") == std::string::npos &&
        filename.find(".m3u8") == std::string::npos) {
      filename += ".m3u";
    }

    // Parse optional custom source name
    std::string customName;
    size_t nmPos = fullBody.find("name=\"name\"");
    if (nmPos != std::string::npos) {
      size_t dbl = fullBody.find("\r\n\r\n", nmPos);
      if (dbl != std::string::npos) {
        dbl += 4;
        size_t endNm = fullBody.find("\r\n--", dbl);
        if (endNm != std::string::npos) {
          customName = fullBody.substr(dbl, endNm - dbl);
        }
      }
    }

    // Extract content after multipart headers (double CRLF)
    std::string fileContent;
    size_t dataStart = fullBody.find("\r\n\r\n");
    if (dataStart != std::string::npos &&
        fullBody.find("Content-Disposition") != std::string::npos) {
      dataStart += 4;
      size_t dataEnd = fullBody.find("\r\n--", dataStart);
      if (dataEnd != std::string::npos) {
        fileContent = fullBody.substr(dataStart, dataEnd - dataStart);
      } else {
        fileContent = fullBody.substr(dataStart);
      }
    } else {
      fileContent = fullBody;
    }

    if (!fileContent.empty()) {
      std::string iptvDir = IPTVManager::instance().getIptvDir();
      if (iptvDir.empty()) {
        iptvDir = AppConfig::instance().getAppRoot() + "/iptv";
      }
      FileSystemManager::instance().createDirectoryRecursive(iptvDir);

      std::string outPath = iptvDir + "/" + filename;
      std::ofstream out(outPath, std::ios::binary);
      if (out.is_open()) {
        out.write(fileContent.data(), fileContent.size());
        out.close();
        sync();
        Logger::info("IPTV: Playlist uploaded successfully to " + outPath +
                     " (" + std::to_string(fileContent.size()) + " bytes)");

        IPTVManager::instance().addSourceFromFile(filename, customName);
        size_t count = IPTVManager::instance().getChannels().size();
        std::string json = "{\"success\":true,\"file\":\"" +
                           escapeJson(filename) +
                           "\",\"channels\":" + std::to_string(count) + "}";
        std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
                          "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                          "*\r\nContent-Length: " +
                          std::to_string(json.length()) +
                          "\r\nConnection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
      } else {
        Logger::error("IPTV: Cannot write uploaded file to " + outPath);
        std::string json =
            "{\"success\":false,\"error\":\"Cannot write file: " +
            escapeJson(filename) + "\"}";
        std::string res = "HTTP/1.1 500 OK\r\nContent-Type: application/json; "
                          "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                          "*\r\nContent-Length: " +
                          std::to_string(json.length()) +
                          "\r\nConnection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
      }
    } else {
      std::string json = "{\"success\":false,\"error\":\"No content\"}";
      std::string res = "HTTP/1.1 400 OK\r\nContent-Type: application/json; "
                        "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                        "*\r\nContent-Length: " +
                        std::to_string(json.length()) +
                        "\r\nConnection: close\r\n\r\n" + json;
      send(clientFd, res.c_str(), res.length(), 0);
    }
  } else if ((method == "GET" && path.find("/api/iptv/add") == 0) ||
             (method == "POST" && path == "/api/iptv/add")) {
    // Download and add playlist from URL
    std::string url;
    std::string name;
    if (method == "GET") {
      url = extractQueryParam(queryString, "url");
      name = extractQueryParam(queryString, "name");
      if (url.empty())
        url = extractQueryParam(fullPath, "url");
      if (name.empty())
        name = extractQueryParam(fullPath, "name");
    } else {
      url = extractPostParam(postBody, "url");
      name = extractPostParam(postBody, "name");
      if (url.empty()) {
        size_t uPos = postBody.find("\"url\":\"");
        if (uPos != std::string::npos) {
          uPos += 7;
          size_t uEnd = postBody.find("\"", uPos);
          if (uEnd != std::string::npos)
            url = postBody.substr(uPos, uEnd - uPos);
        }
        size_t nPos = postBody.find("\"name\":\"");
        if (nPos != std::string::npos) {
          nPos += 8;
          size_t nEnd = postBody.find("\"", nPos);
          if (nEnd != std::string::npos)
            name = postBody.substr(nPos, nEnd - nPos);
        }
      }
    }

    if (!url.empty()) {
      std::string error;
      std::string filename;
      size_t chanCount = 0;
      if (IPTVManager::instance().addSourceFromUrl(url, name, error, filename,
                                                   chanCount)) {
        size_t total = IPTVManager::instance().getChannels().size();
        std::string json = "{\"success\":true,\"file\":\"" +
                           escapeJson(filename) +
                           "\",\"channels\":" + std::to_string(chanCount) +
                           ",\"total_channels\":" + std::to_string(total) + "}";
        std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
                          "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                          "*\r\nContent-Length: " +
                          std::to_string(json.length()) +
                          "\r\nConnection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
      } else {
        std::string json =
            "{\"success\":false,\"error\":\"" + escapeJson(error) + "\"}";
        std::string res = "HTTP/1.1 400 OK\r\nContent-Type: application/json; "
                          "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                          "*\r\nContent-Length: " +
                          std::to_string(json.length()) +
                          "\r\nConnection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
      }
    } else {
      std::string json =
          "{\"success\":false,\"error\":\"Chưa có link\"}";
      std::string res = "HTTP/1.1 400 OK\r\nContent-Type: application/json; "
                        "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                        "*\r\nContent-Length: " +
                        std::to_string(json.length()) +
                        "\r\nConnection: close\r\n\r\n" + json;
      send(clientFd, res.c_str(), res.length(), 0);
    }
  } else if ((method == "GET" && (path == "/api/iptv/delete" ||
                                  fullPath.find("/api/iptv/delete") == 0)) ||
             (method == "POST" && path == "/api/iptv/delete")) {    // Delete IPTV source file
    std::string file;
    if (method == "GET") {
      file = extractQueryParam(queryString, "file");
      if (file.empty())
        file = extractQueryParam(fullPath, "file");
    } else {
      file = extractPostParam(postBody, "file");
      if (file.empty())
        file = extractPostParam(postBody, "filename");
      if (file.empty()) {
        size_t fnPos = postBody.find("\"file\":\"");
        if (fnPos != std::string::npos) {
          fnPos += 8;
          size_t fnEnd = postBody.find("\"", fnPos);
          if (fnEnd != std::string::npos)
            file = postBody.substr(fnPos, fnEnd - fnPos);
        }
      }
    }

    std::string error;
    if (IPTVManager::instance().deleteSource(file, error)) {
      size_t count = IPTVManager::instance().getChannels().size();
      std::string json = "{\"success\":true,\"deleted\":\"" + escapeJson(file) +
                         "\",\"channels\":" + std::to_string(count) + "}";
      std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
                        "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                        "*\r\nContent-Length: " +
                        std::to_string(json.length()) +
                        "\r\nConnection: close\r\n\r\n" + json;
      send(clientFd, res.c_str(), res.length(), 0);
    } else {
      std::string json =
          "{\"success\":false,\"error\":\"" +
          escapeJson(error.empty() ? "Không xóa được nguồn" : error) + "\"}";
      std::string res = "HTTP/1.1 400 OK\r\nContent-Type: application/json; "
                        "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                        "*\r\nContent-Length: " +
                        std::to_string(json.length()) +
                        "\r\nConnection: close\r\n\r\n" + json;
      send(clientFd, res.c_str(), res.length(), 0);
    }
  // Ảnh screensaver (Picture frame)
  } else if (method == "GET" && path == "/api/saver/photos") {
    std::string photoDir = AppConfig::instance().getAppRoot() + "/screensaver/photos";
    FileSystemManager::instance().createDirectoryRecursive(photoDir);
    std::string json = "{\"success\":true,\"photos\":[";
    bool first = true;
    size_t total = 0;
    DIR* dir = opendir(photoDir.c_str());
    if (dir) {
      struct dirent* e;
      while ((e = readdir(dir)) != nullptr) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string low = name;
        for (char& c : low) c = (char)::tolower((unsigned char)c);
        bool isImg = (low.size() >= 4 &&
                      (low.compare(low.size() - 4, 4, ".jpg") == 0 ||
                       low.compare(low.size() - 4, 4, ".png") == 0)) ||
                     (low.size() >= 5 &&
                      low.compare(low.size() - 5, 5, ".jpeg") == 0);
        if (!isImg) continue;
        struct stat st;
        if (::stat((photoDir + "/" + name).c_str(), &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;
        if (!first) json += ",";
        first = false;
        json += "{\"file\":\"" + escapeJson(name) + "\",\"size\":" +
                std::to_string((long long)st.st_size) + "}";
        total++;
      }
      closedir(dir);
    }
    json += "],\"total\":" + std::to_string(total) + "}";
    std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
                      "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                      "*\r\nContent-Length: " +
                      std::to_string(json.length()) +
                      "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/saver/upload") {
    // Đọc full POST body theo Content-Length (1 ảnh/lần; portal gửi tuần tự)
    size_t clPos = req.find("Content-Length: ");
    if (clPos == std::string::npos)
      clPos = req.find("content-length: ");
    long totalCl = 0;
    if (clPos != std::string::npos) {
      size_t clEnd = req.find("\r\n", clPos);
      try {
        totalCl = std::stol(req.substr(clPos + 16, clEnd - (clPos + 16)));
      } catch (...) {
      }
    }
    const long kMaxPhoto = 25L * 1024 * 1024;
    std::string json;
    int httpCode = 200;
    if (totalCl > kMaxPhoto) {
      httpCode = 413;
      json = "{\"success\":false,\"error\":\"File quá lớn (tối đa 25MB)\"}";
    } else {
      std::string fullBody = postBody;
      if (totalCl > 0 && static_cast<long>(fullBody.size()) < totalCl) {
        char chunk[16384];
        while (static_cast<long>(fullBody.size()) < totalCl) {
          long toRead = std::min<long>(
              sizeof(chunk), totalCl - static_cast<long>(fullBody.size()));
          int n = recv(clientFd, chunk, toRead, 0);
          if (n <= 0) break;
          fullBody.append(chunk, n);
        }
      }
      std::string filename = "photo.jpg";
      size_t fnPos = fullBody.find("filename=\"");
      if (fnPos != std::string::npos) {
        fnPos += 10;
        size_t fnEnd = fullBody.find("\"", fnPos);
        if (fnEnd != std::string::npos && fnEnd > fnPos) {
          filename = fullBody.substr(fnPos, fnEnd - fnPos);
          size_t slash = filename.find_last_of("/\\");
          if (slash != std::string::npos)
            filename = filename.substr(slash + 1);
        }
      }
      // Làm sạch tên: bỏ ký tự nguy hiểm, chặn traversal
      std::string safe;
      for (char c : filename) {
        unsigned char u = (unsigned char)c;
        if (u == '/' || u == '\\' || u < 32 || u == 127) continue;
        safe += c;
      }
      filename = safe;
      if (filename.empty() || filename == "." || filename == "..") {
        filename = "photo_" + std::to_string(std::time(nullptr)) + ".jpg";
      }
      std::string low = filename;
      for (char& c : low) c = (char)::tolower((unsigned char)c);
      bool okExt = (low.size() >= 4 &&
                    (low.compare(low.size() - 4, 4, ".jpg") == 0 ||
                     low.compare(low.size() - 4, 4, ".png") == 0)) ||
                   (low.size() >= 5 &&
                    low.compare(low.size() - 5, 5, ".jpeg") == 0);
      if (!okExt) {
        httpCode = 400;
        json = "{\"success\":false,\"error\":\"Chỉ nhận file .jpg / .jpeg / .png\"}";
      } else {
        std::string fileContent;
        size_t dataStart = fullBody.find("\r\n\r\n");
        if (dataStart != std::string::npos &&
            fullBody.find("Content-Disposition") != std::string::npos) {
          dataStart += 4;
          size_t dataEnd = fullBody.find("\r\n--", dataStart);
          fileContent = (dataEnd != std::string::npos)
                            ? fullBody.substr(dataStart, dataEnd - dataStart)
                            : fullBody.substr(dataStart);
        } else {
          fileContent = fullBody;
        }
        if (fileContent.empty() || (long)fileContent.size() > kMaxPhoto) {
          httpCode = 400;
          json = "{\"success\":false,\"error\":\"File rỗng hoặc quá lớn\"}";
        } else {
          std::string photoDir =
              AppConfig::instance().getAppRoot() + "/screensaver/photos";
          FileSystemManager::instance().createDirectoryRecursive(photoDir);
          // Trùng tên -> thêm hậu tố _1, _2...
          std::string outPath = photoDir + "/" + filename;
          if (access(outPath.c_str(), F_OK) == 0) {
            size_t dot = filename.find_last_of('.');
            std::string stem = (dot == std::string::npos) ? filename
                                                          : filename.substr(0, dot);
            std::string ext = (dot == std::string::npos) ? ""
                                                         : filename.substr(dot);
            for (int i = 1; i < 1000; i++) {
              std::string cand = photoDir + "/" + stem + "_" +
                                 std::to_string(i) + ext;
              if (access(cand.c_str(), F_OK) != 0) {
                outPath = cand;
                filename = stem + "_" + std::to_string(i) + ext;
                break;
              }
            }
          }
          std::ofstream out(outPath, std::ios::binary);
          if (out.is_open()) {
            out.write(fileContent.data(), fileContent.size());
            out.close();
            sync();
            Logger::info("Saver: photo uploaded to " + outPath + " (" +
                         std::to_string(fileContent.size()) + " bytes)");
            json = "{\"success\":true,\"file\":\"" + escapeJson(filename) +
                   "\",\"size\":" + std::to_string(fileContent.size()) + "}";
          } else {
            httpCode = 500;
            json = "{\"success\":false,\"error\":\"Không ghi được file\"}";
          }
        }
      }
    }
    std::string res = "HTTP/1.1 " + std::to_string(httpCode) +
                      " OK\r\nContent-Type: application/json; "
                      "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                      "*\r\nContent-Length: " +
                      std::to_string(json.length()) +
                      "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if ((method == "GET" && (path == "/api/saver/delete" ||
                                  fullPath.find("/api/saver/delete") == 0)) ||
             (method == "POST" && path == "/api/saver/delete")) {
    std::string file;
    if (method == "GET") {
      file = extractQueryParam(queryString, "file");
      if (file.empty()) file = extractQueryParam(fullPath, "file");
    } else {
      file = extractPostParam(postBody, "file");
      if (file.empty()) file = extractPostParam(postBody, "filename");
    }
    size_t slash = file.find_last_of("/\\");
    if (slash != std::string::npos) file = file.substr(slash + 1);
    std::string json;
    int httpCode = 200;
    if (file.empty() || file == "." || file == "..") {
      httpCode = 400;
      json = "{\"success\":false,\"error\":\"Tên file không hợp lệ\"}";
    } else {
      std::string photoDir =
          AppConfig::instance().getAppRoot() + "/screensaver/photos";
      std::string target = photoDir + "/" + file;
      if (unlink(target.c_str()) == 0) {
        sync();
        Logger::info("Saver: photo deleted: " + target);
        json = "{\"success\":true,\"deleted\":\"" + escapeJson(file) + "\"}";
      } else {
        httpCode = 400;
        json = "{\"success\":false,\"error\":\"Không xóa được file\"}";
      }
    }
    std::string res = "HTTP/1.1 " + std::to_string(httpCode) +
                      " OK\r\nContent-Type: application/json; "
                      "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                      "*\r\nContent-Length: " +
                      std::to_string(json.length()) +
                      "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  // Lịch phone (CalManager)
  } else if (method == "GET" && path == "/api/cal/sources") {
    CalManager::instance().ensureTables();
    std::string json = "{\"sources\":[";
    bool first = true;
    for (const auto& s : CalManager::instance().sources()) {
      if (!first) json += ",";
      first = false;
      json += "{\"id\":" + std::to_string(s.id) + ",\"name\":\"" +
              escapeJson(s.name) + "\",\"url\":\"" + escapeJson(s.url) + "\"}";
    }
    json += "]}";
    std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
                      "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                      "*\r\nContent-Length: " +
                      std::to_string(json.length()) +
                      "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "GET" && fullPath.find("/api/cal/events") == 0) {
    CalManager::instance().ensureTables();
    int64_t from = 0, to = 0;
    try {
      from = std::stoll(extractQueryParam(queryString, "from"));
      to = std::stoll(extractQueryParam(queryString, "to"));
    } catch (...) {}
    if (to <= 0) to = from + 60LL * 86400;
    std::string json = "{\"events\":[";
    bool first = true;
    for (const auto& e : CalManager::instance().eventsForRange(from, to)) {
      if (!first) json += ",";
      first = false;
      json += "{\"id\":" + std::to_string(e.id) + ",\"title\":\"" +
              escapeJson(e.title) + "\",\"start\":" + std::to_string(e.startTs) +
              ",\"end\":" + std::to_string(e.endTs) + ",\"source\":\"" +
              escapeJson(e.source) + "\",\"remind\":" + (e.remind ? "1" : "0") + "}";
    }
    json += "]}";
    std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
                      "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                      "*\r\nContent-Length: " +
                      std::to_string(json.length()) +
                      "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/cal/add_url") {
    CalManager::instance().ensureTables();
    std::string name = extractPostParam(postBody, "name");
    std::string url = extractPostParam(postBody, "url");
    if (url.rfind("webcal://", 0) == 0) url = "https://" + url.substr(9);
    std::string json;
    if (url.empty()) {
      json = "{\"success\":false,\"error\":\"Chưa nhập link ICS\"}";
    } else {
      if (name.empty()) name = "Lịch phone";
      int64_t id = CalManager::instance().addUrlSource(name, url);
      CalManager::instance().refreshStale();
      json = "{\"success\":true,\"id\":" + std::to_string(id) + "}";
    }
    std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
                      "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                      "*\r\nContent-Length: " +
                      std::to_string(json.length()) +
                      "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/cal/import") {
    CalManager::instance().ensureTables();
    std::string name = extractPostParam(postBody, "name");
    std::string ics = extractPostParam(postBody, "ics");
    std::string json;
    if (ics.empty()) {
      json = "{\"success\":false,\"error\":\"Chưa dán nội dung ICS\"}";
    } else {
      if (name.empty()) name = "Lịch nhập tay";
      int n = CalManager::instance().importIcsText(ics, name);
      json = "{\"success\":" + std::string(n >= 0 ? "true" : "false") +
             ",\"count\":" + std::to_string(n >= 0 ? n : 0) + "}";
    }
    std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
                      "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                      "*\r\nContent-Length: " +
                      std::to_string(json.length()) +
                      "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/cal/add_manual") {
    CalManager::instance().ensureTables();
    std::string date = extractPostParam(postBody, "date");
    std::string time = extractPostParam(postBody, "time");
    std::string title = extractPostParam(postBody, "title");
    int64_t id = CalManager::instance().addManual(date, time, title);
    std::string json = "{\"success\":" + std::string(id >= 0 ? "true" : "false") + "}";
    std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
                      "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                      "*\r\nContent-Length: " +
                      std::to_string(json.length()) +
                      "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/cal/del_source") {
    int64_t id = -1;
    try { id = std::stoll(extractPostParam(postBody, "id")); } catch (...) {}
    bool ok = CalManager::instance().deleteSource(id);
    std::string json = "{\"success\":" + std::string(ok ? "true" : "false") + "}";
    std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
                      "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                      "*\r\nContent-Length: " +
                      std::to_string(json.length()) +
                      "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/cal/del_event") {
    int64_t id = -1;
    try { id = std::stoll(extractPostParam(postBody, "id")); } catch (...) {}
    bool ok = CalManager::instance().deleteEvent(id);
    std::string json = "{\"success\":" + std::string(ok ? "true" : "false") + "}";
    std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
                      "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                      "*\r\nContent-Length: " +
                      std::to_string(json.length()) +
                      "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/cal/remind") {
    int64_t id = -1;
    bool on = false;
    try { id = std::stoll(extractPostParam(postBody, "id")); } catch (...) {}
    on = extractPostParam(postBody, "remind") == "1";
    bool ok = CalManager::instance().setRemind(id, on);
    std::string json = "{\"success\":" + std::string(ok ? "true" : "false") + "}";
    std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
                      "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                      "*\r\nContent-Length: " +
                      std::to_string(json.length()) +
                      "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/cal/refresh") {
    CalManager::instance().ensureTables();
    int n = CalManager::instance().refreshAll();
    std::string json = "{\"success\":true,\"refreshed\":" + std::to_string(n) + "}";
    std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
                      "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                      "*\r\nContent-Length: " +
                      std::to_string(json.length()) +
                      "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "GET" && path == "/api/clock_alarms") {
    auto alarms = ClockStore::instance().alarms();
    std::string json = "{\"alarms\":[";
    for (size_t i = 0; i < alarms.size(); ++i) {
      if (i) json += ",";
      char t[8];
      snprintf(t, sizeof(t), "%02d:%02d", alarms[i].hour, alarms[i].minute);
      json += "{\"idx\":" + std::to_string(i) + ",\"time\":\"" + t +
              "\",\"enabled\":" + (alarms[i].enabled ? "true" : "false") +
              ",\"mode\":" + std::to_string(alarms[i].mode) + ",\"label\":\"" +
              escapeJson(alarms[i].label) + "\"}";
    }
    json += "]}";
    std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
                      "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                      "*\r\nContent-Length: " +
                      std::to_string(json.length()) +
                      "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/clock_alarm") {
    std::string action = extractPostParam(postBody, "action");
    size_t idx = (size_t)atoi(extractPostParam(postBody, "idx").c_str());
    if (action == "add") {
      int h = atoi(extractPostParam(postBody, "hour").c_str());
      int m = atoi(extractPostParam(postBody, "minute").c_str());
      if (h < 0) h = 0;
      if (h > 23) h = 23;
      if (m < 0) m = 0;
      if (m > 59) m = 59;
      ClockStore::instance().addAlarm(h, m);
    } else if (action == "toggle") {
      ClockStore::instance().toggleAlarm(idx);
    } else if (action == "del") {
      ClockStore::instance().removeAlarm(idx);
    } else if (action == "mode") {
      int mode = atoi(extractPostParam(postBody, "mode").c_str());
      if (mode < 0) mode = 0;
      if (mode > 2) mode = 2;
      auto v = ClockStore::instance().alarms();
      if (idx < v.size()) {
        v[idx].mode = mode;
        ClockStore::instance().saveAlarms(v);
      }
    }
    std::string json = "{\"success\":true}";
    std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
                      "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                      "*\r\nContent-Length: " +
                      std::to_string(json.length()) +
                      "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "GET" && path == "/api/watch_symbols") {
    auto syms = WatchManager::instance().symbols();
    std::string json = "{\"symbols\":[";
    for (size_t i = 0; i < syms.size(); ++i) {
      if (i) json += ",";
      json += "\"" + escapeJson(syms[i]) + "\"";
    }
    json += "],\"max\":4}";
    std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
                      "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                      "*\r\nContent-Length: " +
                      std::to_string(json.length()) +
                      "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "POST" && path == "/api/watch_symbols") {
    // symbols=BTC,ETH,VCB (tối đa 4, chuẩn hóa trong setSymbols)
    std::string csv = extractPostParam(postBody, "symbols");
    std::vector<std::string> v;
    std::string cur;
    for (char c : csv + ",") {
      if (c == ',') {
        if (!cur.empty()) v.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    WatchManager::instance().setSymbols(v);
    std::string json = "{\"success\":true}";
    std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; "
                      "charset=UTF-8\r\nAccess-Control-Allow-Origin: "
                      "*\r\nContent-Length: " +
                      std::to_string(json.length()) +
                      "\r\nConnection: close\r\n\r\n" + json;
    send(clientFd, res.c_str(), res.length(), 0);
  } else if (method == "GET" && path == "/saver.html") {
    std::string body = buildSaverHtml();
    std::string res="HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: "+std::to_string(body.length())+"\r\nConnection: close\r\n\r\n"+body;
    send(clientFd,res.c_str(),res.length(),0);
  } else if (method == "GET" && path == "/api/saver/photos") {
    std::string dir = "/mnt/SDCARD/Apps/RomCloud/screensaver/photos";
    std::string hf_path = "/mnt/SDCARD/Apps/RomCloud/screensaver/hidden.txt";
    std::set<std::string> hidden;
    { std::ifstream hf(hf_path); std::string l; while (std::getline(hf,l)) if(!l.empty()) hidden.insert(l); }
    std::string json = "[";
    DIR* d = opendir(dir.c_str());
    bool first = true;
    if (d) {
      struct dirent* e;
      while ((e = readdir(d)) != nullptr) {
        std::string n = e->d_name;
        if (n=="."||n=="..") continue;
        if(!first) json+=","; first=false;
        json+="{\"name\":\""+escapeJson(n)+"\",\"hidden\":"+(hidden.count(n)?"true":"false")+"}";
      }
      closedir(d);
    }
    json += "]";
    std::string res2="HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+std::to_string(json.length())+"\r\nConnection: close\r\n\r\n"+json;
    send(clientFd,res2.c_str(),res2.length(),0);
  } else if (method == "GET" && path.rfind("/saver/photo/",0)==0) {
    std::string fn=path.substr(13);
    if(fn.find("..")!=std::string::npos||fn.find("/")!=std::string::npos){
      std::string r400="HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
      send(clientFd,r400.c_str(),r400.length(),0);
    } else {
      std::string fp="/mnt/SDCARD/Apps/RomCloud/screensaver/photos/"+fn;
      std::ifstream f(fp,std::ios::binary);
      if(!f){
        std::string r404="HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
        send(clientFd,r404.c_str(),r404.length(),0);
      } else {
        std::string data((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
        std::string r200="HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: "+std::to_string(data.size())+"\r\nConnection: close\r\n\r\n";
        send(clientFd,r200.c_str(),r200.length(),0);
        send(clientFd,data.c_str(),data.size(),0);
      }
    }
  } else if (method == "POST" && path == "/api/saver/upload") {
    size_t clP=req.find("Content-Length: ");
    long tCl=0;
    if(clP!=std::string::npos){ size_t e=req.find("\r\n",clP); try{tCl=std::stol(req.substr(clP+16,e-clP-16));}catch(...){} }
    std::string body=postBody;
    if(tCl>0&&(long)body.size()<tCl){ char ch[16384]; while((long)body.size()<tCl){ long tr=std::min<long>(sizeof(ch),tCl-body.size()); int n=recv(clientFd,ch,tr,0); if(n<=0)break; body.append(ch,n);} }
    std::string fn="photo.jpg";
    size_t fp=body.find("filename=\"");
    if(fp!=std::string::npos){ fp+=10; size_t fe=body.find("\"",fp); if(fe!=std::string::npos) fn=body.substr(fp,fe-fp); }
    std::string json;
    size_t ds=body.find("\r\n\r\n");
    if(ds!=std::string::npos){
      ds+=4; size_t de=body.find("\r\n--",ds);
      std::string fc=(de!=std::string::npos)?body.substr(ds,de-ds):body.substr(ds);
      std::string dir="/mnt/SDCARD/Apps/RomCloud/screensaver/photos";
      mkdir(dir.c_str(),0755);
      std::string fp2=dir+"/"+fn;
      std::ofstream out(fp2,std::ios::binary); out.write(fc.c_str(),fc.size()); out.close();
      json="{\"ok\":true}";
    } else json="{\"ok\":false}";
    std::string r="HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+std::to_string(json.length())+"\r\nConnection: close\r\n\r\n"+json;
    send(clientFd,r.c_str(),r.length(),0);
  } else if (method == "POST" && path == "/api/saver/toggle") {
    std::string name; size_t p=postBody.find("name="); if(p!=std::string::npos){ name=postBody.substr(p+5); size_t a=name.find("&"); if(a!=std::string::npos) name=name.substr(0,a); }
    std::string hfp="/mnt/SDCARD/Apps/RomCloud/screensaver/hidden.txt";
    std::set<std::string> hs; { std::ifstream hf(hfp); std::string l; while(std::getline(hf,l)) if(!l.empty()) hs.insert(l); }
    if(hs.count(name)) hs.erase(name); else hs.insert(name);
    { std::ofstream hf(hfp,std::ios::trunc); for(auto&h:hs) hf<<h<<"\n"; }
    std::string j="{\"ok\":true}";
    std::string rr="HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+std::to_string(j.length())+"\r\nConnection: close\r\n\r\n"+j;
    send(clientFd,rr.c_str(),rr.length(),0);
  } else if (method == "POST" && path == "/api/saver/delete") {
    std::string name; size_t p=postBody.find("name="); if(p!=std::string::npos){ name=postBody.substr(p+5); size_t a=name.find("&"); if(a!=std::string::npos) name=name.substr(0,a); }
    std::string j;
    if(name.find("..")!=std::string::npos||name.find("/")!=std::string::npos) j="{\"ok\":false}";
    else { unlink(("/mnt/SDCARD/Apps/RomCloud/screensaver/photos/"+name).c_str()); j="{\"ok\":true}"; }
    std::string rr="HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+std::to_string(j.length())+"\r\nConnection: close\r\n\r\n"+j;
    send(clientFd,rr.c_str(),rr.length(),0);
  } else {
    std::string notFound = "HTTP/1.1 404 Not Found\r\nContent-Length: "
                           "0\r\nConnection: close\r\n\r\n";
    send(clientFd, notFound.c_str(), notFound.length(), 0);
  }
}

} // namespace RomCloud
