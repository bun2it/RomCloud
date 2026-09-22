#include "WebServer.h"
#include "../logging/Logger.h"
#include "../auth/AuthManager.h"
#include "../database/DatabaseManager.h"
#include "../platform/PlatformInfo.h"
#include "../sync/DriveSyncEngine.h"
#include "../ota/UpdateManager.h"
#include "../download/DownloadManager.h"
#include "../filesystem/FileSystemManager.h"
#include "../ui/BoxartScraper.h"
#include "../app/Application.h"
#include "../config/AppConfig.h"
#include "HttpClient.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <sstream>
#include <cstring>
#include <vector>
#include <algorithm>
#include <iomanip>

namespace RomCloud {

static std::string urlDecode(const std::string& in) {
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

static std::string extractFolderId(const std::string& url) {
    size_t fPos = url.find("folders/");
    if (fPos != std::string::npos) {
        std::string id = url.substr(fPos + 8);
        size_t endPos = id.find_first_of("?/#& ");
        if (endPos != std::string::npos) id = id.substr(0, endPos);
        return id;
    }
    size_t idPos = url.find("id=");
    if (idPos != std::string::npos) {
        std::string id = url.substr(idPos + 3);
        size_t endPos = id.find_first_of("?/#& ");
        if (endPos != std::string::npos) id = id.substr(0, endPos);
        return id;
    }
    if (url.find('/') == std::string::npos && url.length() >= 20) {
        return url;
    }
    return "";
}

static std::string extractPostParam(const std::string& postBody, const std::string& paramName) {
    std::string key = paramName + "=";
    size_t keyPos = 0;
    while (true) {
        keyPos = postBody.find(key, keyPos);
        if (keyPos == std::string::npos) return "";
        if (keyPos == 0 || postBody[keyPos - 1] == '&') break;
        keyPos += key.length();
    }
    std::string rawVal = postBody.substr(keyPos + key.length());
    size_t ampersand = rawVal.find('&');
    if (ampersand != std::string::npos) rawVal = rawVal.substr(0, ampersand);
    return urlDecode(rawVal);
}

static std::string extractQueryParam(const std::string& queryStr, const std::string& paramName) {
    std::string key = paramName + "=";
    size_t keyPos = 0;
    while (true) {
        keyPos = queryStr.find(key, keyPos);
        if (keyPos == std::string::npos) return "";
        if (keyPos == 0 || queryStr[keyPos - 1] == '&' || queryStr[keyPos - 1] == '?') break;
        keyPos += key.length();
    }
    std::string rawVal = queryStr.substr(keyPos + key.length());
    size_t ampersand = rawVal.find('&');
    if (ampersand != std::string::npos) rawVal = rawVal.substr(0, ampersand);
    return urlDecode(rawVal);
}

static std::string escapeJson(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 10);
    for (char c : in) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (static_cast<unsigned char>(c) < 32) {
            // drop non-printable
        } else {
            out += c;
        }
    }
    return out;
}

WebServer& WebServer::instance() {
    static WebServer instance;
    return instance;
}

WebServer::~WebServer() {
    stop();
}

bool WebServer::start(int port) {
    if (m_running) return true;
    m_port = port;

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
    address.sin_port = htons(m_port);

    if (bind(m_serverFd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        Logger::error("WebServer: Bind failed on port " + std::to_string(m_port));
        close(m_serverFd);
        m_serverFd = -1;
        return false;
    }

    if (listen(m_serverFd, 10) < 0) {
        Logger::error("WebServer: Listen failed.");
        close(m_serverFd);
        m_serverFd = -1;
        return false;
    }

    m_running = true;
    m_thread = std::thread(&WebServer::serverLoop, this);
    Logger::info("WebServer: Portal started on port " + std::to_string(m_port));
    return true;
}

void WebServer::stop() {
    if (!m_running) return;
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
        int clientFd = accept(m_serverFd, (struct sockaddr*)&clientAddr, &clientLen);
        if (clientFd < 0) {
            if (m_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            continue;
        }

        struct timeval tv;
        tv.tv_sec = 4;
        tv.tv_usec = 0;
        setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

        handleClient(clientFd);
        close(clientFd);
    }
}

std::string WebServer::buildHtmlResponse() {
    std::string ip = PlatformInfo::instance().getIpAddress("wlan0");
    if (ip.empty()) ip = "192.168.1.164";

    auto& db = DatabaseManager::instance();
    bool isLinked = AuthManager::instance().isLinked();
    std::string savedDriveUrl = isLinked ? db.getSetting("drive_folder_url", "") : "";
    std::string lastSyncTime = DriveSyncEngine::instance().getLastSyncTime();

    int totalLocal = 0, totalCloud = 0;
    if (isLinked) {
        db.getTotalGameCounts(totalLocal, totalCloud);
    }

    std::string html = R"HTML(<!DOCTYPE html>
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
      --bg: #090d16;
      --card-bg: #111827;
      --card-alt: #1a2333;
      --border: #1f293d;
      --border-hover: #374151;
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
      --radius: 12px;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Oxygen, Ubuntu, Cantarell, sans-serif;
      background: var(--bg);
      color: var(--text);
      line-height: 1.5;
      padding: 16px;
      min-height: 100vh;
    }
    .container { max-width: 1140px; margin: 0 auto; }
    
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

    /* Mobile Adaptations */
    @media (max-width: 680px) {
      .game-table th:nth-child(3), .game-table td:nth-child(3) { display: none; }
      .hide-mobile { display: none; }
    }
  </style>
</head>
<body>
  <div class="container">
    <!-- Header -->
    <header>
      <div class="logo-area">
        <span class="logo-icon">🎮</span>
        <div>
          <span class="logo-text">RomCloud</span>
          <span class="device-badge">TrimUI Brick / Smart Pro</span>
        </div>
      </div>
      <div class="header-stats">
        <div class="stat-pill" id="head-auth-pill">Drive: <b>Đang tải...</b></div>
        <div class="stat-pill" id="head-storage-pill">SD: <b>Đang tải...</b></div>
        <div class="stat-pill">Games: <b id="head-local-count">)HTML" + std::to_string(totalLocal) + R"HTML(</b> thẻ / <b id="head-cloud-count">)HTML" + std::to_string(totalCloud) + R"HTML(</b> cloud</div>
        <button class="btn btn-danger hide-mobile" id="btn-head-logout" onclick="logoutGoogleDrive()" style="display:none; padding: 4px 10px; font-size: 11px;">🚪 Đăng xuất</button>
      </div>
    </header>

    <!-- Offline / Unlinked Warning Banner -->
    <div id="unlinked-warning-banner" style="display: none; padding: 12px 18px; background: rgba(245, 158, 11, 0.12); border: 1px solid #f59e0b; border-radius: var(--radius); margin-bottom: 20px; font-size: 13px; color: #fef3c7;">
      ⚠️ <b>Thiết bị TrimUI đang ở chế độ Đăng xuất / Offline:</b> Chỉ hiển thị các game ĐÃ TẢI về thẻ nhớ. Để tìm kiếm và tải thêm kho game từ Google Drive, vui lòng <a href="javascript:void(0)" onclick="switchTab('tab-storage')" style="color: #38bdf8; font-weight: 700; text-decoration: underline;">bấm vào đây để kết nối lại Google Drive</a>.
    </div>

    <!-- OTA Push Notification Banner -->
    <div id="ota-push-banner" style="display: none; background: linear-gradient(135deg, #1e1b4b, #312e81, #1e40af); border: 1px solid #6366f1; border-radius: var(--radius); padding: 14px 20px; margin-bottom: 20px; box-shadow: 0 4px 20px rgba(99, 102, 241, 0.35);">
      <div style="display: flex; justify-content: space-between; align-items: center; gap: 15px; flex-wrap: wrap;">
        <div style="display: flex; align-items: center; gap: 12px;">
          <span style="font-size: 26px;">🎉</span>
          <div>
            <div style="font-weight: 700; font-size: 15px; color: #fff;">ĐÃ CÓ BẢN CẬP NHẬT MỚI: <span id="ota-push-version" style="color: #38bdf8;">v1.0.x</span>!</div>
            <div id="ota-push-notes" style="font-size: 12px; color: #cbd5e1; margin-top: 2px;">Bản cập nhật chứa các tính năng và sửa lỗi mới nhất.</div>
          </div>
        </div>
        <div style="display: flex; gap: 8px;">
          <button class="btn btn-primary" onclick="quickApplyOta()" style="background: var(--green); border-color: var(--green-border); font-weight: 700;">🚀 Cập nhật ngay</button>
          <button class="btn btn-secondary" onclick="document.getElementById('ota-push-banner').style.display='none';" style="padding: 6px 12px;">✕ Đóng</button>
        </div>
      </div>
    </div>

    <!-- Navigation Tabs -->
    <div class="tab-bar">
      <button class="tab-btn active" onclick="switchTab('tab-roms')">🎮 Quản lý ROM</button>
      <button class="tab-btn" onclick="switchTab('tab-queue')">📥 Hàng đợi tải <span class="badge" id="nav-queue-badge">0</span></button>
      <button class="tab-btn" onclick="switchTab('tab-storage')">☁️ Đồng bộ &amp; Thẻ nhớ</button>
      <button class="tab-btn" onclick="switchTab('tab-ota')" id="nav-tab-ota">🚀 Cập nhật OTA <span class="badge" id="ota-nav-badge" style="display: none; background: #ef4444; color: #fff; margin-left: 4px; padding: 2px 6px; border-radius: 8px; font-size: 10px;">NEW</span></button>
    </div>

    <!-- TAB 1: ROM MANAGER -->
    <div id="tab-roms" class="tab-content active">
      <div class="controls-bar">
        <div class="search-row">
          <div class="search-input-wrap">
            <input type="text" id="rom-search" class="search-input" placeholder="🔍 Nhập tên game hoặc tên file ROM để tìm kiếm ngay..." oninput="onSearchInput()">
            <button id="search-clear-btn" class="search-clear" onclick="clearSearch()">&times;</button>
          </div>
        </div>

        <div class="filter-pills-row">
          <div class="state-filter-group">
            <button class="pill-btn active" id="filter-state-all" onclick="setStateFilter(-1)">Tất cả</button>
            <button class="pill-btn" id="filter-state-local" onclick="setStateFilter(1)">🟢 Đã tải</button>
            <button class="pill-btn" id="filter-state-cloud" onclick="setStateFilter(0)">☁️ Chưa tải</button>
          </div>
          <div id="system-pills-container" style="display: flex; gap: 6px;">
            <!-- Rendered by JS -->
          </div>
        </div>
      </div>

      <div class="game-list-container">
        <div class="game-list-header">
          <span id="game-results-count">Đang tải danh sách game...</span>
          <span id="active-filters-desc" style="font-weight:400;">Tất cả hệ máy</span>
        </div>
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
            <tr><td colspan="5" style="text-align: center; padding: 30px; color: var(--text-dim);">Đang tải dữ liệu từ máy TrimUI...</td></tr>
          </tbody>
        </table>
        <div class="pagination">
          <button class="btn btn-secondary" id="btn-prev-page" onclick="changePage(-1)">&larr; Trang trước</button>
          <span id="page-indicator" style="font-size: 13px; color: var(--text-muted);">Trang 1</span>
          <button class="btn btn-secondary" id="btn-next-page" onclick="changePage(1)">Trang sau &rarr;</button>
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
          <button class="btn btn-danger" onclick="cancelActiveDownload()">✕ Hủy tải</button>
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
          <h3>📥 Hàng đợi tải về (<span id="queue-count-num">0</span>)</h3>
          <button class="btn btn-secondary" onclick="clearAllQueue()">🗑️ Xóa toàn bộ hàng đợi</button>
        </div>
        <div id="queue-list-container">
          <p style="color: var(--text-dim); text-align: center; padding: 20px;">Hàng đợi tải về hiện đang trống.</p>
        </div>
      </div>
    </div>

    <!-- TAB 3: STORAGE & CLOUD SYNC -->
    <div id="tab-storage" class="tab-content">
      <div class="grid-2">
        <div class="card">
          <h3>💾 Dung lượng thẻ nhớ MicroSD</h3>
          <div class="storage-bar">
            <div class="storage-used" id="storage-bar-fill" style="width: 30%;"></div>
          </div>
          <div style="display: flex; justify-content: space-between; font-size: 13px; margin-bottom: 12px;">
            <span>Đã dùng: <b id="storage-used-txt">-- GB</b></span>
            <span>Còn trống: <b id="storage-avail-txt" style="color: var(--green);">-- GB</b></span>
            <span>Tổng: <b id="storage-total-txt">-- GB</b></span>
          </div>
          <p style="font-size: 12px; color: var(--text-dim); line-height: 1.6;">
            ROM game tải về sẽ được đặt tại <code>/mnt/SDCARD/Roms/&lt;HỆ_MÁY&gt;/</code>. Khi rút thẻ nhớ hoặc cập nhật firmware TrimUI, toàn bộ game được giữ nguyên.
          </p>
        </div>

        <div class="card">
          <h3>☁️ Đồng bộ Google Drive</h3>
          <div id="drive-connection-status" style="margin-bottom: 12px; font-size: 13px;">
            Trạng thái: <b style="color:var(--text-dim);">Đang kiểm tra...</b>
          </div>
          <p style="font-size: 13px; color: var(--text-muted); margin-bottom: 14px;">
            Lần đồng bộ gần nhất: <b id="last-sync-time">)HTML" + lastSyncTime + R"HTML(</b>
          </p>
          <div style="display: flex; gap: 10px; margin-bottom: 16px; flex-wrap: wrap;">
            <button class="btn btn-primary" id="btn-trigger-sync" onclick="triggerSync()">🔄 Quét &amp; Đồng bộ lại ngay</button>
            <button class="btn btn-danger" id="btn-tab-logout" onclick="logoutGoogleDrive()" style="display: none;">🚪 Đăng xuất khỏi Google Drive</button>
          </div>
          <div id="sync-status-box" style="display: none; padding: 12px; background: var(--card-alt); border-radius: 8px; font-size: 13px; margin-top: 10px;">
            <div id="sync-status-txt">Đang quét thư mục Google Drive...</div>
          </div>

          <form id="form-connect-drive" onsubmit="handleConnectSubmit(event)" autocomplete="off" style="margin-top: 18px; border-top: 1px solid var(--border); padding-top: 14px;">
            <label style="font-size: 12px; font-weight: 600; color: var(--text-muted); display: block; margin-bottom: 6px;">URL Thư mục Google Drive Master:</label>
            <div style="display: flex; gap: 8px; flex-wrap: wrap;">
              <input type="text" id="input-drive-url" name="drive_url" value=")HTML" + savedDriveUrl + R"HTML(" placeholder="https://drive.google.com/drive/folders/... (Dán link vào đây)" autocomplete="off" style="flex: 1; min-width: 220px; padding: 8px 12px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 12px;" required>
              <button type="submit" class="btn btn-primary">🔗 Kết nối &amp; Quét ngay</button>
              <button type="button" class="btn btn-secondary" onclick="clearDriveInput()">✕ Xóa trắng</button>
            </div>
          </form>
        </div>
      </div>
    </div>

    <!-- TAB 4: OTA UPDATE -->
    <div id="tab-ota" class="tab-content">
      <div class="card" style="max-width: 600px; margin: 0 auto;">
        <h3>🚀 Cập nhật ứng dụng RomCloud (OTA)</h3>
        <p style="font-size: 13px; color: var(--text-muted); margin-bottom: 16px;">
          Phiên bản trên máy hiện tại: <b style="color: var(--accent);">v)HTML" + UpdateManager::instance().getCurrentVersion() + R"HTML(</b>
        </p>

        <div style="display: flex; gap: 10px; margin-bottom: 16px;">
          <button class="btn btn-primary" id="btn-ota-check" onclick="checkOtaUpdate()">Kiểm tra bản cập nhật mới</button>
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
    </div>
  </div>

  <div id="toast">Thông báo</div>

  <script>
    let currentTab = 'tab-roms';
    let isDriveLinked = false;
    let currentSystemId = 0;
    let currentStateFilter = -1;
    let currentSearch = '';
    let currentPage = 1;
    const pageSize = 50;
    let searchDebounceTimer = null;
    let dlPollTimer = null;
    let systemsCache = [];

    // Init
    window.addEventListener('DOMContentLoaded', () => {
      loadSystems();
      loadStorageInfo();
      loadGames();
      startPolling();
      setTimeout(() => checkOtaUpdate(true), 1200);
      setInterval(() => checkOtaUpdate(true), 180000);
    });

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
      if (tabId === 'tab-storage') loadStorageInfo();
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
      tbody.innerHTML = `<tr><td colspan="5" style="text-align: center; padding: 25px; color: var(--text-dim);">Đang tải danh sách game...</td></tr>`;

      const offset = (currentPage - 1) * pageSize;
      const url = `/api/games?system_id=${currentSystemId}&state=${currentStateFilter}&q=${encodeURIComponent(currentSearch)}&limit=${pageSize}&offset=${offset}`;

      try {
        const res = await fetch(url);
        const data = await res.json();
        renderGameTable(data.games, data.total);
      } catch (e) {
        tbody.innerHTML = `<tr><td colspan="5" style="text-align: center; padding: 25px; color: var(--red);">Lỗi nạp dữ liệu. Vui lòng thử lại.</td></tr>`;
      }
    }

    function renderGameTable(games, total) {
      const tbody = document.getElementById('game-table-body');
      const countSpan = document.getElementById('game-results-count');
      const pageIndicator = document.getElementById('page-indicator');
      const btnPrev = document.getElementById('btn-prev-page');
      const btnNext = document.getElementById('btn-next-page');

      if (!isDriveLinked) {
        tbody.innerHTML = `<tr><td colspan="5" style="text-align: center; padding: 50px 20px; color: var(--text-dim);">
          <div style="font-size: 38px; margin-bottom: 12px;">☁️❌</div>
          <div style="font-size: 16px; font-weight: 700; color: #fef3c7; margin-bottom: 6px;">Google Drive chưa được kết nối (Đã ngắt kết nối)</div>
          <div style="font-size: 13px; color: var(--text-muted); margin-bottom: 18px;">Chỉ khi còn kết nối Google Drive thì hệ thống mới hiển thị danh mục ROM game.</div>
          <button class="btn btn-primary" onclick="switchTab('tab-storage')">🔗 Kết nối Google Drive ngay</button>
        </td></tr>`;
        countSpan.innerHTML = `Chưa kết nối Google Drive (<b>0</b> game)`;
        pageIndicator.textContent = `Trang 0 / 0`;
        btnPrev.disabled = true;
        btnNext.disabled = true;
        return;
      }

      const maxPages = Math.ceil(total / pageSize) || 1;
      countSpan.innerHTML = `Tìm thấy <b>${total.toLocaleString()}</b> game`;
      pageIndicator.textContent = `Trang ${currentPage} / ${maxPages}`;
      btnPrev.disabled = (currentPage <= 1);
      btnNext.disabled = (currentPage >= maxPages);

      if (!games || games.length === 0) {
        tbody.innerHTML = `<tr><td colspan="5" style="text-align: center; padding: 30px; color: var(--text-dim);">Không tìm thấy game nào phù hợp với bộ lọc.</td></tr>`;
        return;
      }

      let html = '';
      for (const g of games) {
        const sysClass = 'sys-' + (g.sys_code || 'default');
        let statusBadge = '';
        let actionButtons = '';

        if (g.local_state === 1) {
          statusBadge = `<span class="status-badge status-local">🟢 Đã tải</span>`;
          actionButtons = `
            <button class="btn btn-secondary" onclick="scrapeBoxart(${g.id})" title="Tải ảnh bìa">🖼️ Bìa</button>
            <button class="btn btn-danger" onclick="deleteRom(${g.id}, '${escapeHtml(g.title)}')">🗑️ Xóa</button>
          `;
        } else if (g.in_queue) {
          statusBadge = `<span class="status-badge status-queue">⏳ Hàng đợi</span>`;
          actionButtons = `<button class="btn btn-secondary" onclick="removeFromQueue(${g.id})">✕ Bỏ</button>`;
        } else {
          statusBadge = `<span class="status-badge status-cloud">☁️ Cloud</span>`;
          actionButtons = `<button class="btn btn-primary" onclick="downloadGame(${g.id})">📥 Tải về</button>`;
        }

        html += `
          <tr>
            <td><span class="sys-tag ${sysClass}">${g.sys_code || 'GAME'}</span></td>
            <td>
              <span class="game-title">${escapeHtml(g.title)}</span>
              <span class="game-file">${escapeHtml(g.filename)}</span>
            </td>
            <td>${g.size_str || '0 B'}</td>
            <td>${statusBadge}</td>
            <td style="text-align: right;">${actionButtons}</td>
          </tr>
        `;
      }
      tbody.innerHTML = html;
    }

    function changePage(delta) {
      currentPage += delta;
      if (currentPage < 1) currentPage = 1;
      loadGames();
    }

    function escapeHtml(s) {
      if (!s) return '';
      return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
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

    async function deleteRom(gameId, title) {
      if (!confirm(`Bạn có chắc chắn muốn xóa ROM "${title}" khỏi thẻ nhớ TrimUI? (File sẽ được giải phóng khỏi thẻ, bạn vẫn có thể tải lại sau)`)) {
        return;
      }
      try {
        const res = await fetch('/api/delete_rom', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: `game_id=${gameId}`
        });
        const data = await res.json();
        showToast(data.message || 'Đã xóa ROM khỏi thẻ nhớ.');
        loadGames();
        loadStorageInfo();
        loadSystems();
      } catch (e) {
        showToast('Lỗi khi xóa ROM.');
      }
    }

    async function scrapeBoxart(gameId) {
      showToast('Đang tìm kiếm ảnh bìa trên Libretro CDN...');
      try {
        const res = await fetch('/api/scrape_cover', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: `game_id=${gameId}`
        });
        const data = await res.json();
        showToast(data.message || 'Cập nhật ảnh bìa hoàn tất.');
      } catch (e) {
        showToast('Lỗi cào ảnh bìa.');
      }
    }

    async function cancelActiveDownload() {
      if (!confirm('Bạn có chắc chắn muốn hủy lượt tải game hiện tại?')) return;
      try {
        await fetch('/api/cancel_download', { method: 'POST' });
        showToast('Đã gửi yêu cầu hủy tải.');
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
      if (!confirm('Xóa sạch toàn bộ các game đang xếp hàng tải về?')) return;
      try {
        await fetch('/api/clear_queue', { method: 'POST' });
        showToast('Đã xóa sạch hàng đợi.');
        updateDownloadQueueUI();
        loadGames();
      } catch (e) {}
    }

    async function triggerSync() {
      const btn = document.getElementById('btn-trigger-sync');
      btn.disabled = true;
      btn.textContent = '⏳ Đang quét Google Drive...';
      const box = document.getElementById('sync-status-box');
      box.style.display = 'block';

      try {
        await fetch('/api/trigger_sync', { method: 'POST' });
        showToast('Đã bắt đầu đồng bộ kho game Google Drive.');
        pollSyncProgress();
      } catch (e) {
        btn.disabled = false;
        btn.textContent = '🔄 Quét & Đồng bộ lại ngay';
      }
    }

    async function pollSyncProgress() {
      const timer = setInterval(async () => {
        try {
          const res = await fetch('/api/sync_status');
          const data = await res.json();
          const txt = document.getElementById('sync-status-txt');
          if (data.is_syncing) {
            txt.textContent = `Trạng thái: ${data.status} | Đang quét: ${data.current_platform || '...'} | Đã tìm thấy: ${data.games_found || 0} game`;
          } else {
            clearInterval(timer);
            document.getElementById('btn-trigger-sync').disabled = false;
            document.getElementById('btn-trigger-sync').textContent = '🔄 Quét & Đồng bộ lại ngay';
            txt.textContent = `Đồng bộ hoàn tất! Tổng game tìm thấy: ${data.games_found || 0}`;
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
          authPill.innerHTML = `Drive: <b style="color:var(--green);">🟢 Đã kết nối</b>`;
          btnHeadLogout.style.display = 'inline-flex';
          unlinkedBanner.style.display = 'none';
          driveConnStatus.innerHTML = `Trạng thái: <b style="color:var(--green);">🟢 Đã liên kết</b> (${data.user_email || 'Google Drive'})`;
          btnTabLogout.style.display = 'inline-flex';
          btnTriggerSync.disabled = false;
          if (driveInput && data.drive_url) driveInput.value = data.drive_url;
          if (sInput) {
            sInput.disabled = false;
            sInput.placeholder = '🔍 Nhập tên game hoặc tên file ROM để tìm kiếm ngay...';
          }
        } else {
          authPill.innerHTML = `Drive: <b style="color:var(--text-dim);">⚪ Đã đăng xuất</b>`;
          btnHeadLogout.style.display = 'none';
          unlinkedBanner.style.display = 'block';
          driveConnStatus.innerHTML = `Trạng thái: <b style="color:var(--yellow);">⚪ Chưa liên kết Google Drive (Đã đăng xuất)</b>`;
          btnTabLogout.style.display = 'none';
          btnTriggerSync.disabled = true;
          document.getElementById('head-local-count').textContent = '0';
          document.getElementById('head-cloud-count').textContent = '0';
          if (driveInput) driveInput.value = '';
          if (sInput) {
            sInput.disabled = true;
            sInput.value = '';
            sInput.placeholder = '🔒 Chưa kết nối Google Drive. Vui lòng kết nối để hiển thị ROM...';
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
        showToast('Vui lòng dán link thư mục Google Drive trước khi kết nối.');
        return;
      }
      try {
        const body = 'drive_url=' + encodeURIComponent(val);
        const res = await fetch('/connect', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: body
        });
        showToast('Đã lưu kết nối Google Drive! Đang bắt đầu quét...');
        await loadStorageInfo();
        triggerSync();
      } catch (err) {
        showToast('Lỗi khi gửi kết nối.');
      }
    }

    async function logoutGoogleDrive() {
      if (!confirm("Bạn có chắc chắn muốn ĐĂNG XUẤT khỏi Google Drive?\\n\\n- Các game đã tải về thẻ nhớ vẫn được giữ nguyên 100%.\\n- Danh mục các game cloud chưa tải sẽ được dọn sạch khỏi danh sách.")) {
        return;
      }
      try {
        const res = await fetch('/api/logout', { method: 'POST' });
        const data = await res.json();
        showToast(data.message || 'Đã đăng xuất khỏi Google Drive.');
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
          qContainer.innerHTML = `<p style="color: var(--text-dim); text-align: center; padding: 20px;">Hàng đợi tải về hiện đang trống.</p>`;
        } else {
          let qHtml = '<table class="game-table"><thead><tr><th>#</th><th>Hệ máy</th><th>Tên game</th><th>Dung lượng</th><th style="text-align:right;">Thao tác</th></tr></thead><tbody>';
          data.queue.forEach((item, idx) => {
            qHtml += `
              <tr>
                <td style="color: var(--text-dim); font-weight:700;">#${idx + 1}</td>
                <td><span class="sys-tag sys-${item.system}">${item.system}</span></td>
                <td><b>${escapeHtml(item.title)}</b></td>
                <td>${item.size_str}</td>
                <td style="text-align:right;"><button class="btn btn-secondary" onclick="removeFromQueue(${item.game_id})">✕ Bỏ</button></td>
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
            document.getElementById('ota-version-title').innerHTML = `🎉 Có bản cập nhật mới: v${remoteVer}`;
            document.getElementById('ota-release-date').textContent = `Ngày phát hành: ${data.release_date || 'Mới nhất'}`;
            document.getElementById('ota-changelog').textContent = changelog;
            document.getElementById('btn-ota-install').style.display = 'inline-flex';
          }

          if (silent) {
            showToast(`🎉 Phát hiện bản cập nhật mới v${remoteVer}!`);
          }
        } else {
          if (box) {
            document.getElementById('ota-version-title').innerHTML = `✅ Bạn đang sử dụng bản mới nhất (v${data.current_version})`;
            document.getElementById('ota-release-date').textContent = '';
            document.getElementById('ota-changelog').textContent = 'Không có bản cập nhật nào mới hơn.';
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
            document.getElementById('ota-status-txt').innerHTML = `<span style="color:var(--green);font-weight:700;">🎉 ĐÃ CẬP NHẬT THÀNH CÔNG! Đang khởi động lại ứng dụng...</span>`;
            await fetch('/ota_restart', { method: 'POST' });
            setTimeout(() => location.reload(), 5000);
          } else if (data.state === 'FAILED') {
            clearInterval(otaPollTimer);
            document.getElementById('ota-status-txt').innerHTML = `<span style="color:var(--red);">❌ Thất bại: ${data.error}</span>`;
            document.getElementById('btn-ota-install').disabled = false;
          }
        }, 1000);
      } catch (e) {}
    }
  </script>
</body>
</html>)HTML";
    return html;
}

std::string WebServer::buildSuccessResponse(const std::string& message) {
    std::string html = R"(<!DOCTYPE html>
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
    <h1>ĐÃ LƯU THÀNH CÔNG!</h1>
    <p>)" + message + R"(<br><br>Quá trình đồng bộ kho game đang diễn ra tự động!</p>
    <a href="/">&larr; Quay lại trang quản lý ROM</a>
  </div>
</body>
</html>)";
    return html;
}

void WebServer::handleClient(int clientFd) {
    char buffer[4096];
    int bytesRead = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
    if (bytesRead <= 0) return;
    buffer[bytesRead] = '\0';

    std::string req(buffer);
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
    std::string postBody = (bodyPos != std::string::npos) ? req.substr(bodyPos + 4) : "";

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        std::string body = buildHtmlResponse();
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/html; charset=UTF-8\r\n"
                          "Cache-Control: no-cache, no-store, must-revalidate, max-age=0\r\n"
                          "Pragma: no-cache\r\n"
                          "Expires: 0\r\n"
                          "Content-Length: " + std::to_string(body.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + body;
        send(clientFd, res.c_str(), res.length(), 0);
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
            const auto& s = systems[i];
            if (i > 0) json += ",";
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
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "GET" && path == "/api/games") {
        if (!AuthManager::instance().isLinked()) {
            std::string json = "{\"total\":0,\"games\":[]}";
            std::string res = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json; charset=UTF-8\r\n"
                              "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                              "Access-Control-Allow-Origin: *\r\n"
                              "Content-Length: " + std::to_string(json.length()) + "\r\n"
                              "Connection: close\r\n\r\n" + json;
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

        try { if (!sysIdStr.empty()) systemId = std::stoi(sysIdStr); } catch(...) {}
        try { if (!stateStr.empty()) stateFilter = std::stoi(stateStr); } catch(...) {}
        try { if (!limitStr.empty()) limit = std::stoi(limitStr); } catch(...) {}
        try { if (!offsetStr.empty()) offset = std::stoi(offsetStr); } catch(...) {}

        int totalCount = 0;
        auto games = DatabaseManager::instance().getGamesFiltered(systemId, stateFilter, q, limit, offset, totalCount);

        std::string json = "{\"total\":" + std::to_string(totalCount) + ",\"games\":[";
        for (size_t i = 0; i < games.size(); ++i) {
            const auto& g = games[i];
            bool inQueue = DownloadManager::instance().isInQueue(g.id);
            if (i > 0) json += ",";
            json += "{\"id\":" + std::to_string(g.id) + ",";
            json += "\"title\":\"" + escapeJson(g.title) + "\",";
            json += "\"filename\":\"" + escapeJson(g.filename) + "\",";
            json += "\"sys_code\":\"" + escapeJson(g.systemCode.empty() ? "GAME" : g.systemCode) + "\",";
            json += "\"size_str\":\"" + FileSystemManager::instance().formatBytes(g.sizeBytes) + "\",";
            json += "\"local_state\":" + std::to_string(static_cast<int>(g.localState)) + ",";
            json += "\"in_queue\":" + std::string(inQueue ? "true" : "false") + ",";
            json += "\"has_cover\":" + std::string(g.coverPath.empty() ? "false" : "true") + "}";
        }
        json += "]}";

        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
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
            const auto& g = games[i];
            SystemRecord sys;
            std::string sysCode = "GAME";
            if (DatabaseManager::instance().getSystemById(g.systemId, sys)) {
                sysCode = sys.code;
            }
            if (i > 0) json += ",";
            json += "{\"id\":" + std::to_string(g.id) + ",";
            json += "\"title\":\"" + escapeJson(g.title) + "\",";
            json += "\"filename\":\"" + escapeJson(g.filename) + "\",";
            json += "\"sys_code\":\"" + escapeJson(sysCode) + "\",";
            json += "\"size_str\":\"" + FileSystemManager::instance().formatBytes(g.sizeBytes) + "\",";
            json += "\"local_state\":" + std::to_string(static_cast<int>(g.localState)) + ",";
            json += "\"has_cover\":" + std::string(g.coverPath.empty() ? "false" : "true") + "}";
        }
        json += "]";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "GET" && path == "/api/download_status") {
        auto prog = DownloadManager::instance().getProgress();
        auto queue = DownloadManager::instance().getQueue();
        bool isDownloading = DownloadManager::instance().isDownloading();

        std::string stateStr = "IDLE";
        if (prog.state == DownloadState::INITIALIZING) stateStr = "INITIALIZING";
        else if (prog.state == DownloadState::DOWNLOADING) stateStr = "DOWNLOADING";
        else if (prog.state == DownloadState::VERIFYING) stateStr = "VERIFYING";
        else if (prog.state == DownloadState::COMPLETED) stateStr = "COMPLETED";
        else if (prog.state == DownloadState::FAILED) stateStr = "FAILED";
        else if (prog.state == DownloadState::CANCELLED) stateStr = "CANCELLED";

        std::string json = "{";
        json += "\"is_downloading\":" + std::string(isDownloading ? "true" : "false") + ",";
        json += "\"active\":{";
        json += "\"state\":\"" + stateStr + "\",";
        json += "\"game_id\":" + std::to_string(prog.gameId) + ",";
        json += "\"title\":\"" + escapeJson(prog.gameTitle) + "\",";
        json += "\"system\":\"" + escapeJson(prog.systemCode) + "\",";
        json += "\"filename\":\"" + escapeJson(prog.filename) + "\",";
        json += "\"progress_pct\":" + std::to_string(prog.progressPct) + ",";
        json += "\"speed_kbps\":" + std::to_string(prog.speedKBps) + ",";
        json += "\"bytes_downloaded\":" + std::to_string(prog.bytesDownloaded) + ",";
        json += "\"total_bytes\":" + std::to_string(prog.totalBytes) + ",";
        json += "\"eta_seconds\":" + std::to_string(prog.etaSeconds) + ",";
        json += "\"error\":\"" + escapeJson(prog.errorMessage) + "\"";
        json += "},";
        json += "\"queue_size\":" + std::to_string(queue.size()) + ",";
        json += "\"queue\":[";
        for (size_t i = 0; i < queue.size(); ++i) {
            const auto& it = queue[i];
            if (i > 0) json += ",";
            json += "{\"game_id\":" + std::to_string(it.game.id) + ",";
            json += "\"title\":\"" + escapeJson(it.game.title) + "\",";
            json += "\"system\":\"" + escapeJson(it.sys.code) + "\",";
            json += "\"size_str\":\"" + FileSystemManager::instance().formatBytes(it.game.sizeBytes) + "\"}";
        }
        json += "]}";

        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/download_game") {
        std::string gameIdStr = extractPostParam(postBody, "game_id");
        int64_t gameId = 0;
        try { gameId = std::stoll(gameIdStr); } catch(...) {}

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
        std::string json = "{\"success\":" + std::string(ok ? "true" : "false") + ",\"message\":\"" + escapeJson(msg) + "\"}";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/cancel_download") {
        DownloadManager::instance().cancelDownload();
        std::string json = "{\"success\":true,\"message\":\"Đã hủy tải lượt hiện tại.\"}";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/remove_queue") {
        std::string gameIdStr = extractPostParam(postBody, "game_id");
        int64_t gameId = 0;
        try { gameId = std::stoll(gameIdStr); } catch(...) {}
        bool ok = DownloadManager::instance().removeFromQueue(gameId);
        std::string json = "{\"success\":" + std::string(ok ? "true" : "false") + ",\"message\":\"" + (ok ? "Đã xóa khỏi hàng đợi." : "Không tìm thấy game trong hàng đợi.") + "\"}";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/clear_queue") {
        DownloadManager::instance().clearQueue();
        std::string json = "{\"success\":true,\"message\":\"Đã xóa toàn bộ hàng đợi.\"}";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/delete_rom") {
        std::string gameIdStr = extractPostParam(postBody, "game_id");
        int64_t gameId = 0;
        try { gameId = std::stoll(gameIdStr); } catch(...) {}

        bool ok = false;
        std::string msg = "Lỗi khi xóa ROM";
        if (gameId > 0) {
            GameRecord g;
            if (DatabaseManager::instance().getGameById(gameId, g)) {
                ok = DatabaseManager::instance().markGameDeletedLocally(gameId);
                if (ok) {
                    msg = "Đã xóa ROM \"" + g.title + "\" khỏi thẻ nhớ.";
                }
            }
        }
        std::string json = "{\"success\":" + std::string(ok ? "true" : "false") + ",\"message\":\"" + escapeJson(msg) + "\"}";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/scrape_cover") {
        std::string gameIdStr = extractPostParam(postBody, "game_id");
        int64_t gameId = 0;
        try { gameId = std::stoll(gameIdStr); } catch(...) {}

        bool ok = false;
        std::string coverPath;
        std::string msg = "Không thể cào boxart";
        if (gameId > 0) {
            GameRecord g;
            if (DatabaseManager::instance().getGameById(gameId, g)) {
                SystemRecord sys;
                if (DatabaseManager::instance().getSystemById(g.systemId, sys)) {
                    ok = BoxartScraper::instance().scrapeCover(g, sys, coverPath);
                    if (ok) {
                        DatabaseManager::instance().updateGameCover(gameId, coverPath);
                        msg = "Đã lưu ảnh bìa cho \"" + g.title + "\"!";
                    } else {
                        msg = "Không tìm thấy ảnh bìa trên Libretro CDN.";
                    }
                }
            }
        }
        std::string json = "{\"success\":" + std::string(ok ? "true" : "false") + ",\"cover_path\":\"" + escapeJson(coverPath) + "\",\"message\":\"" + escapeJson(msg) + "\"}";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "GET" && path == "/api/storage_info") {
        auto disk = FileSystemManager::instance().getDiskSpace(AppConfig::instance().getRomsDir());
        bool isLinked = AuthManager::instance().isLinked();
        int totalLocal = 0, totalCloud = 0;
        if (isLinked) {
            DatabaseManager::instance().getTotalGameCounts(totalLocal, totalCloud);
        }
        std::string lastSync = isLinked ? DriveSyncEngine::instance().getLastSyncTime() : "Chưa kết nối";
        std::string driveUrl = isLinked ? DatabaseManager::instance().getSetting("drive_folder_url", "") : "";

        uint64_t usedBytes = (disk.totalBytes > disk.availableBytes) ? (disk.totalBytes - disk.availableBytes) : 0;
        double usedPct = 0.0;
        if (disk.totalBytes > 0) {
            usedPct = (static_cast<double>(usedBytes) / static_cast<double>(disk.totalBytes)) * 100.0;
        }

        std::string userEmail = isLinked ? AuthManager::instance().getUserEmail() : "";

        std::string json = "{";
        json += "\"is_linked\":" + std::string(isLinked ? "true" : "false") + ",";
        json += "\"user_email\":\"" + escapeJson(userEmail) + "\",";
        json += "\"total_bytes\":" + std::to_string(disk.totalBytes) + ",";
        json += "\"avail_bytes\":" + std::to_string(disk.availableBytes) + ",";
        json += "\"used_bytes\":" + std::to_string(usedBytes) + ",";
        json += "\"total_str\":\"" + FileSystemManager::instance().formatBytes(disk.totalBytes) + "\",";
        json += "\"avail_str\":\"" + FileSystemManager::instance().formatBytes(disk.availableBytes) + "\",";
        json += "\"used_str\":\"" + FileSystemManager::instance().formatBytes(usedBytes) + "\",";
        json += "\"used_pct\":" + std::to_string(usedPct) + ",";
        json += "\"total_local\":" + std::to_string(totalLocal) + ",";
        json += "\"total_cloud\":" + std::to_string(totalCloud) + ",";
        json += "\"last_sync\":\"" + escapeJson(lastSync) + "\",";
        json += "\"drive_url\":\"" + escapeJson(driveUrl) + "\"";
        json += "}";

        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/trigger_sync") {
        bool started = DriveSyncEngine::instance().startSync();
        std::string json = "{\"success\":" + std::string(started ? "true" : "false") + "}";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "GET" && path == "/api/sync_status") {
        auto prog = DriveSyncEngine::instance().getProgress();
        bool isSyncing = DriveSyncEngine::instance().isSyncing();
        std::string statusStr = "IDLE";
        if (prog.status == SyncStatus::CONNECTING) statusStr = "CONNECTING";
        else if (prog.status == SyncStatus::DISCOVERING_FOLDERS) statusStr = "DISCOVERING_FOLDERS";
        else if (prog.status == SyncStatus::SYNCING_FILES) statusStr = "SYNCING_FILES";
        else if (prog.status == SyncStatus::COMPLETED) statusStr = "COMPLETED";
        else if (prog.status == SyncStatus::ERROR_OCCURRED) statusStr = "ERROR";

        std::string json = "{";
        json += "\"is_syncing\":" + std::string(isSyncing ? "true" : "false") + ",";
        json += "\"status\":\"" + statusStr + "\",";
        json += "\"current_platform\":\"" + escapeJson(prog.currentPlatform) + "\",";
        json += "\"games_found\":" + std::to_string(prog.cloudGamesFound) + ",";
        json += "\"games_indexed\":" + std::to_string(prog.newGamesIndexed) + ",";
        json += "\"current_system_index\":" + std::to_string(prog.currentSystemIndex) + ",";
        json += "\"total_systems\":" + std::to_string(prog.totalSystems) + ",";
        json += "\"error\":\"" + escapeJson(prog.errorMessage) + "\"";
        json += "}";

        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && (path == "/api/logout" || path == "/unlink")) {
        AuthManager::instance().logout();
        if (path == "/api/logout") {
            std::string json = "{\"success\":true,\"message\":\"Đã đăng xuất khỏi Google Drive thành công!\"}";
            std::string res = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json; charset=UTF-8\r\n"
                              "Access-Control-Allow-Origin: *\r\n"
                              "Content-Length: " + std::to_string(json.length()) + "\r\n"
                              "Connection: close\r\n\r\n" + json;
            send(clientFd, res.c_str(), res.length(), 0);
        } else {
            std::string body = buildSuccessResponse("Đã hủy liên kết Google Drive thành công!");
            std::string res = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/html; charset=UTF-8\r\n"
                              "Content-Length: " + std::to_string(body.length()) + "\r\n"
                              "Connection: close\r\n\r\n" + body;
            send(clientFd, res.c_str(), res.length(), 0);
        }
    } else if (method == "POST" && path == "/connect") {
        std::string driveUrl = extractPostParam(postBody, "drive_url");
        std::string folderId = extractFolderId(driveUrl);
        if (folderId.empty()) folderId = driveUrl;

        Logger::info("User linked Google Drive folder via Web Portal: " + driveUrl + " (Extracted Folder ID: " + folderId + ")");
        AuthManager::instance().linkPublicFolder(folderId, driveUrl);
        DriveSyncEngine::instance().startSync();

        std::string body = buildSuccessResponse("Đã lưu liên kết Google Drive vào máy TrimUI!");
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/html; charset=UTF-8\r\n"
                          "Content-Length: " + std::to_string(body.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + body;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "GET" && path == "/ota_check") {
        UpdateInfo info;
        bool hasUpdate = UpdateManager::instance().checkForUpdatesSync(info);
        std::string json = "{\"has_update\":" + std::string(hasUpdate ? "true" : "false") + ","
                           "\"current_version\":\"" + UpdateManager::instance().getCurrentVersion() + "\","
                           "\"remote_version\":\"" + info.remoteVersion + "\","
                           "\"release_date\":\"" + info.releaseDate + "\","
                           "\"changelog\":\"" + escapeJson(info.changelog) + "\"}";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/ota_start") {
        auto info = UpdateManager::instance().getLatestInfo();
        bool ok = UpdateManager::instance().startUpdate(info);
        std::string json = "{\"started\":" + std::string(ok ? "true" : "false") + "}";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "GET" && path == "/ota_status") {
        auto prog = UpdateManager::instance().getProgress();
        std::string stateStr = "IDLE";
        if (prog.state == UpdateState::CHECKING) stateStr = "CHECKING";
        else if (prog.state == UpdateState::UPDATE_AVAILABLE) stateStr = "UPDATE_AVAILABLE";
        else if (prog.state == UpdateState::UP_TO_DATE) stateStr = "UP_TO_DATE";
        else if (prog.state == UpdateState::DOWNLOADING) stateStr = "DOWNLOADING";
        else if (prog.state == UpdateState::VERIFYING) stateStr = "VERIFYING";
        else if (prog.state == UpdateState::COMPLETED) stateStr = "COMPLETED";
        else if (prog.state == UpdateState::FAILED) stateStr = "FAILED";

        std::string json = "{\"state\":\"" + stateStr + "\","
                           "\"progress_pct\":" + std::to_string(prog.progressPct) + ","
                           "\"bytes_downloaded\":" + std::to_string(prog.bytesDownloaded) + ","
                           "\"total_bytes\":" + std::to_string(prog.totalBytes) + ","
                           "\"error\":\"" + escapeJson(prog.errorMessage) + "\"}";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/ota_restart") {
        std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"status\":\"restarting\"}";
        send(clientFd, res.c_str(), res.length(), 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        Application::instance().requestRestart();
    } else {
        std::string notFound = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(clientFd, notFound.c_str(), notFound.length(), 0);
    }
}

} // namespace RomCloud
