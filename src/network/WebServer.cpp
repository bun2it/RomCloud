#include "WebServer.h"
#include "../logging/Logger.h"
#include "../auth/AuthManager.h"
#include "../database/DatabaseManager.h"
#include "../platform/PlatformInfo.h"
#include "../sync/DriveSyncEngine.h"
#include "../ota/UpdateManager.h"
#include "../download/DownloadManager.h"
#include "../sync/UploadManager.h"
#include "../filesystem/FileSystemManager.h"
#include "../ui/BoxartScraper.h"
#include "../rom/RomOrganizer.h"
#include "../rom/RomDetector.h"
#include "../app/Application.h"
#include "../config/AppConfig.h"
#include "../iptv/IPTVManager.h"
#include "../database/RomIndexer.h"
#include "HttpClient.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sstream>
#include <cstring>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <fstream>

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
      <button class="tab-btn" onclick="switchTab('tab-iptv')">📺 IPTV</button>
      <button class="tab-btn" onclick="switchTab('tab-ota')" id="nav-tab-ota">🚀 Cập nhật OTA <span class="badge" id="ota-nav-badge" style="display: none; background: #ef4444; color: #fff; margin-left: 4px; padding: 2px 6px; border-radius: 8px; font-size: 10px;">NEW</span></button>
    </div>

    <!-- TAB 1: ROM MANAGER -->
    <div id="tab-roms" class="tab-content active">
      <!-- Cloud & Storage Architecture Banner -->
      <div class="storage-architecture-banner">
        <div class="arch-card arch-public">
          <div class="arch-header">
            <h4>🌐 1. Kho Game Công cộng (Public Drive)</h4>
            <span class="sys-tag" style="background:#075985;color:#bae6fd;">CHỈ TẢI VỀ</span>
          </div>
          <p>Kho ROM chung tải qua link chia sẻ Google Drive (không cần đăng nhập). Bạn có thể tìm kiếm và bấm <b>[📥 Tải về]</b> bất kỳ game nào về máy TrimUI.</p>
          <div class="arch-status" id="arch-public-status">
            Nguồn: <b id="arch-public-url" style="color:var(--accent);">Đang tải...</b>
          </div>
        </div>

        <div class="arch-arrow">➔</div>

        <div class="arch-card arch-sd">
          <div class="arch-header">
            <h4>💾 2. Thẻ nhớ MicroSD (Máy TrimUI)</h4>
            <span class="sys-tag" style="background:#065f46;color:#a7f3d0;">ĐÃ CÓ TRÊN MÁY</span>
          </div>
          <p>Các ROM đã tải về nằm tại <code>/mnt/SDCARD/Roms/</code>. Bạn có thể mở máy lên chơi ngay hoặc bấm <b>[📤 Sao lưu]</b> để đưa lên Drive cá nhân.</p>
          <div class="arch-status">
            Trạng thái: <b id="arch-local-status" style="color:var(--green);">-- game trên thẻ</b>
          </div>
          <div style="display:flex; gap:6px; margin-top:4px;">
            <button class="btn btn-secondary" onclick="startAutoScrapeSd()" style="font-size:11.5px;padding:6px 10px;flex:1;border-color:#0284c7;color:#38bdf8;" title="Tự động cào ảnh bìa và thông tin cho toàn bộ ROM trên thẻ nhớ (ScreenScraper / Libretro)">🎨 Tự động cào ảnh thẻ SD</button>
            <button class="btn btn-secondary" onclick="openDoctorModal()" style="font-size:11.5px;padding:6px 10px;flex:1;border-color:#10b981;color:#34d399;font-weight:600;" title="Bác sĩ ROM: Tự động phát hiện ROM đặt sai thư mục Emulator và chuyển về đúng hệ máy">🩺 Sửa ROM lạc chỗ</button>
          </div>
        </div>

        <div class="arch-arrow">➔</div>

        <div class="arch-card arch-private">
          <div class="arch-header">
            <h4>☁️ 3. Google Drive Cá nhân (Sao lưu)</h4>
            <span class="sys-tag" style="background:#5b21b6;color:#ddd6fe;">BACKUP RIÊNG</span>
          </div>
          <p>Lưu trữ dự phòng toàn bộ ROM thẻ nhớ vào thư mục <code>RomCloud_Backup</code> trên Drive cá nhân (qua Access/Refresh Token) để phòng khi hỏng thẻ.</p>
          <div class="arch-status" id="arch-backup-status-wrap">
            Quyền: <b id="arch-backup-badge" style="color:var(--yellow);">Đang kiểm tra...</b>
          </div>
          <div style="display:flex; gap:6px; margin-top:4px;">
            <button class="btn btn-primary" id="btn-arch-backup-all" onclick="uploadAllGamesToDrive()" style="background:var(--purple);border-color:#9333ea;font-size:11.5px;padding:6px 10px;flex:1;">📤 Sao lưu toàn bộ thẻ lên Drive</button>
            <button class="btn btn-secondary" onclick="switchTab('tab-storage')" style="font-size:11.5px;padding:6px 10px;" title="Cài đặt hoặc kiểm tra Token sao lưu">⚙️ Token</button>
          </div>
        </div>
      </div>

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
            <button class="pill-btn" id="filter-state-local" onclick="setStateFilter(1)">🟢 Trên thẻ SD (Có thể Sao lưu)</button>
            <button class="pill-btn" id="filter-state-cloud" onclick="setStateFilter(0)">☁️ Kho Public (Chưa tải về)</button>
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

            <button class="btn btn-secondary" onclick="batchScrapeCurrentPage()" id="btn-batch-scrape" style="font-size: 12px; padding: 6px 12px;" title="Cào tự động toàn bộ ảnh bìa và thông tin cốt truyện cho các game ở trang này">🎨 Cào trang này</button>
            <button class="btn btn-secondary" onclick="startAutoScrapeSd()" id="btn-auto-scrape-sd" style="font-size: 12px; padding: 6px 12px; border-color: #0284c7; color: #38bdf8;" title="Tự động cào ảnh bìa và thông tin cho tất cả ROM đang có trên thẻ nhớ TrimUI (chạy ngầm)">🎨 Cào toàn bộ thẻ SD</button>
            <button class="btn btn-secondary" onclick="uploadAllGamesToDrive()" id="btn-batch-backup" style="font-size: 12px; padding: 6px 12px; border-color: #9333ea; color: #c084fc;" title="Sao lưu tất cả ROM hiện có trên thẻ nhớ TrimUI lên Google Drive cá nhân (/RomCloud_Backup)">📤 Sao lưu thẻ lên Drive</button>
            <button class="btn btn-secondary" onclick="openDoctorModal()" id="btn-toolbar-doctor" style="font-size: 12px; padding: 6px 12px; border-color: #10b981; color: #34d399; font-weight: 600;" title="Bác sĩ ROM: Tự động phát hiện ROM đặt sai thư mục Emulator và chuyển về đúng hệ máy để chơi được ngay">🩺 Bác sĩ ROM</button>

            <div class="view-toggle-group">
              <button class="view-toggle-btn active" id="btn-view-list" onclick="setViewMode('list')" title="Chế độ Danh sách (List View)">☰ Bảng</button>
              <button class="view-toggle-btn" id="btn-view-grid" onclick="setViewMode('grid')" title="Chế độ Lưới bìa game (Grid View)">☷ Lưới thẻ</button>
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
              <tr><td colspan="5" style="text-align: center; padding: 30px; color: var(--text-dim);">Đang tải dữ liệu từ máy TrimUI...</td></tr>
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
            <label style="font-size: 12px; font-weight: 600; color: var(--accent); display: block; margin-bottom: 4px;">📥 1. Kho ROM Tải về (Link Google Drive Công khai):</label>
            <p style="font-size: 11px; color: var(--text-muted); margin-bottom: 8px;">Dán link thư mục Google Drive chứa game để duyệt và tải ROM về máy TrimUI (hoàn toàn miễn phí, không cần đăng nhập).</p>
            <div style="display: flex; gap: 8px; flex-wrap: wrap;">
              <input type="text" id="input-drive-url" name="drive_url" value=")HTML" + savedDriveUrl + R"HTML(" placeholder="https://drive.google.com/drive/folders/... (Dán link vào đây)" autocomplete="off" style="flex: 1; min-width: 220px; padding: 8px 12px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 12px;" required>
              <button type="submit" class="btn btn-primary">🔗 Kết nối &amp; Quét ngay</button>
              <button type="button" class="btn btn-secondary" onclick="clearDriveInput()">✕ Xóa trắng</button>
            </div>
          </form>

          <div style="margin-top: 20px; border-top: 1px solid var(--border); padding-top: 16px;">
            <label style="font-size: 13px; font-weight: 700; color: var(--purple); display: block; margin-bottom: 6px;">📤 2. Nơi Sao lưu Cá nhân (Upload / Backup lên Google Drive):</label>
            <p style="font-size: 12px; color: var(--text-muted); margin-bottom: 12px; line-height: 1.5;">
              Thư mục công cộng chỉ cho phép tải ROM về máy. Để sao lưu ROM và file save từ thẻ nhớ máy TrimUI lên tài khoản Google Drive của riêng bạn, bạn cần cấp Access Token hoặc Refresh Token cá nhân:
            </p>

            <div id="backup-perm-status" style="font-size: 12px; margin-bottom: 14px; padding: 10px 14px; background: var(--bg); border: 1px solid var(--border); border-radius: 8px;">
              Trạng thái quyền sao lưu: <b id="backup-perm-badge" style="color:var(--yellow);">Đang kiểm tra...</b>
            </div>

            <!-- Hướng dẫn chi tiết Google OAuth 2.0 Playground -->
            <details style="margin-bottom: 16px; background: var(--card-alt); border: 1px solid rgba(168, 85, 247, 0.35); border-radius: 8px; padding: 12px 14px;" open>
              <summary style="font-size: 13px; font-weight: 700; color: #c084fc; cursor: pointer; user-select: none; outline: none;">
                📖 Hướng dẫn chi tiết lấy Token qua Google OAuth 2.0 Playground (Bấm để xem/thu gọn)
              </summary>
              <div style="margin-top: 12px; font-size: 12px; line-height: 1.6; color: #cbd5e1;">
                <p style="margin-bottom: 10px;">
                  Để lấy <b>Refresh Token</b> hoặc <b>Access Token</b> của Google Drive cá nhân (thường dùng cho các ứng dụng hoặc script tự động sao lưu), cách nhanh nhất và phổ biến nhất là sử dụng công cụ <b>Google OAuth 2.0 Playground</b>:
                </p>

                <div style="background: var(--bg); border-left: 3px solid #38bdf8; padding: 8px 12px; margin-bottom: 10px; border-radius: 0 6px 6px 0;">
                  <b style="color: #38bdf8;">1. Truy cập OAuth 2.0 Playground:</b><br>
                  <b>Bước 1:</b> Mở trình duyệt và truy cập vào trang web chính thức: 
                  <a href="https://developers.google.com/oauthplayground" target="_blank" rel="noopener noreferrer" style="color: #38bdf8; text-decoration: underline; font-weight: 600;">https://developers.google.com/oauthplayground ↗</a><br>
                  <span style="color: var(--text-dim);">🔍 <i>Cách kiểm tra thành công:</i> Bạn sẽ thấy giao diện gồm các bước cấu hình (Step 1, Step 2, Step 3) ở cột bên trái.</span>
                </div>

                <div style="background: var(--bg); border-left: 3px solid #38bdf8; padding: 8px 12px; margin-bottom: 10px; border-radius: 0 6px 6px 0;">
                  <b style="color: #38bdf8;">2. Chọn phạm vi quyền (Scope) của Google Drive:</b><br>
                  <b>Bước 2:</b> Ở cột bên trái, tìm và mở rộng mục <b>Drive API v3</b>.<br>
                  Tìm và tích chọn quyền: <code style="background: rgba(255,255,255,0.08); padding: 2px 5px; border-radius: 4px; color: #93c5fd;">https://www.googleapis.com/auth/drive</code> (hoặc <code style="background: rgba(255,255,255,0.08); padding: 2px 5px; border-radius: 4px; color: #93c5fd;">https://www.googleapis.com/auth/drive.file</code> tùy thuộc vào việc bạn muốn cấp quyền truy cập toàn bộ Drive hay chỉ các file do ứng dụng tạo).<br>
                  <span style="color: var(--text-dim);">🔍 <i>Cách kiểm tra thành công:</i> Ô vuông tương ứng đã được tích chọn và hiển thị trong danh sách scope được chọn.</span>
                </div>

                <div style="background: var(--bg); border-left: 3px solid #38bdf8; padding: 8px 12px; margin-bottom: 10px; border-radius: 0 6px 6px 0;">
                  <b style="color: #38bdf8;">3. Cấp quyền truy cập (Authorize APIs):</b><br>
                  <b>Bước 3:</b> Nhấn vào nút <b>Authorize APIs</b> màu xanh dương ở phía dưới danh sách scope.<br>
                  - Đăng nhập bằng tài khoản Google của bạn.<br>
                  - Nếu xuất hiện cảnh báo <i>"Google hasn't verified this app"</i> (Ứng dụng chưa được Google xác minh), bạn bấm vào chữ <b>Advanced</b> (Nâng cao) rồi chọn <b>Go to unknown app</b> (Đi tới ứng dụng - không an toàn).<br>
                  - Nhấn <b>Allow</b> (Cho phép) để cấp quyền cho ứng dụng.<br>
                  <span style="color: var(--text-dim);">🔍 <i>Cách kiểm tra thành công:</i> Trình duyệt tự động chuyển hướng ngược lại trang OAuth Playground và giao diện tự động nhảy sang bước tiếp theo (Step 2).</span>
                </div>

                <div style="background: var(--bg); border-left: 3px solid #38bdf8; padding: 8px 12px; margin-bottom: 10px; border-radius: 0 6px 6px 0;">
                  <b style="color: #38bdf8;">4. Trao đổi mã để lấy Token:</b><br>
                  <b>Bước 4:</b> Ở mục <b>Step 2 (Exchange authorization code for tokens)</b>, nhấn vào nút <b>Exchange authorization code for tokens</b>.<br>
                  <span style="color: var(--text-dim);">🔍 <i>Cách kiểm tra thành công:</i> Chuyển sang Step 3, các ô thông tin sẽ xuất hiện đầy đủ gồm <b>Access Token</b> (chuỗi bắt đầu bằng <code>ya29...</code>) và <b>Refresh Token</b> (chuỗi bắt đầu bằng <code>1//...</code>). Bạn chỉ cần sao chép (copy) chuỗi mã này để dán vào ô bên dưới.</span>
                </div>

                <div style="background: rgba(239, 68, 68, 0.1); border: 1px solid rgba(239, 68, 68, 0.35); padding: 10px 14px; border-radius: 6px; margin-top: 10px;">
                  <b style="color: #fca5a5;">⚠️ Lưu ý bảo mật quan trọng:</b>
                  <ul style="margin: 4px 0 0 18px; padding: 0; color: #fecaca; font-size: 11px; line-height: 1.5;">
                    <li>Refresh Token cho phép truy cập liên tục vào Google Drive của bạn mà không cần đăng nhập lại, do đó tuyệt đối không chia sẻ mã này cho người khác hoặc công khai lên mạng.</li>
                    <li>Nếu ứng dụng của bạn trên Google Cloud đang ở chế độ kiểm thử (Testing), Refresh Token có thể hết hạn sau 7 ngày. Hãy chuyển trạng thái ứng dụng sang <b>In Production</b> (Đang hoạt động) trong Google Cloud Console nếu muốn dùng lâu dài.</li>
                    <li><b>Gợi ý nhanh:</b> Bạn có thể copy ô <b>Access token</b> (chuỗi bắt đầu bằng <code>ya29...</code>) dán vào ô bên dưới rồi bấm <b>"Kích hoạt Sao lưu"</b> để dùng ngay tức thì!</li>
                  </ul>
                </div>
              </div>
            </details>

            <div style="display: flex; gap: 8px; flex-wrap: wrap;">
              <input type="text" id="input-personal-token" placeholder="Dán Access Token (ya29...) hoặc Refresh Token (1//...) vào đây" autocomplete="off" onkeydown="if(event.key==='Enter') savePersonalToken()" style="flex: 1; min-width: 240px; padding: 10px 14px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 13px;">
              <button type="button" class="btn btn-primary" id="btn-save-personal-token" onclick="savePersonalToken()" style="background: var(--purple);">💾 Kích hoạt Sao lưu</button>
              <button type="button" class="btn btn-secondary" id="btn-clear-personal-token" onclick="clearPersonalToken()" style="display: none;">✕ Hủy sao lưu cá nhân</button>
            </div>
            <div id="personal-token-status" style="margin-top: 8px; font-size: 12px;"></div>
          </div>
        </div>
      </div>
    </div>

    <!-- TAB 4: IPTV -->
    <div id="tab-iptv" class="tab-content">
      <div class="card" style="max-width: 700px; margin: 0 auto;">
        <h3>📺 Quản lý IPTV</h3>
        <p style="font-size: 13px; color: var(--text-muted); margin-bottom: 16px;">
          Upload file playlist .m3u để xem TV trực tuyến trên RomCloud.
        </p>

        <!-- Upload Form -->
        <div style="background: var(--card-alt); border-radius: 12px; padding: 20px; margin-bottom: 20px; border: 2px dashed var(--border);">
          <h4 style="margin-bottom: 12px;">📤 Upload Playlist M3U</h4>
          <input type="file" id="iptv-file" accept=".m3u,.m3u8" style="width: 100%; padding: 10px; background: var(--bg); border: 1px solid var(--border); border-radius: 8px; color: var(--text); margin-bottom: 12px;">
          <button class="btn btn-primary" onclick="uploadIptvPlaylist()" style="width: 100%;">Upload Playlist</button>
        </div>

        <!-- Quick Add URL -->
        <div style="background: var(--card-alt); border-radius: 12px; padding: 20px; margin-bottom: 20px;">
          <h4 style="margin-bottom: 12px;">🔗 Thêm URL Playlist</h4>
          <input type="text" id="iptv-url" placeholder="https://example.com/playlist.m3u" style="width: 100%; padding: 10px; background: var(--bg); border: 1px solid var(--border); border-radius: 8px; color: var(--text); margin-bottom: 12px;">
          <button class="btn btn-primary" onclick="addIptvUrl()" style="width: 100%;">Thêm URL</button>
        </div>

        <!-- Current Playlists -->
        <div style="background: var(--card-alt); border-radius: 12px; padding: 20px;">
          <h4 style="margin-bottom: 12px;">📋 Playlist hiện có</h4>
          <div id="iptv-playlist-list" style="color: var(--text-muted);">Đang tải...</div>
        </div>
      </div>

      <script>
        function uploadIptvPlaylist() {
          const fileInput = document.getElementById('iptv-file');
          if (!fileInput.files[0]) {
            alert('Vui lòng chọn file playlist');
            return;
          }
          const formData = new FormData();
          formData.append('file', fileInput.files[0]);
          fetch('/api/iptv/upload', { method: 'POST', body: formData })
            .then(r => r.json())
            .then(d => {
              if (d.success) {
                alert('Upload thành công!');
                loadIptvPlaylists();
              } else {
                alert('Lỗi: ' + d.error);
              }
            });
        }
        function addIptvUrl() {
          const url = document.getElementById('iptv-url').value;
          if (!url) return;
          fetch('/api/iptv/add?url=' + encodeURIComponent(url))
            .then(r => r.json())
            .then(d => {
              if (d.success) {
                alert('Đã thêm URL!');
                loadIptvPlaylists();
              } else {
                alert('Lỗi: ' + d.error);
              }
            });
        }
        function loadIptvPlaylists() {
          fetch('/api/iptv/list')
            .then(r => r.json())
            .then(d => {
              const el = document.getElementById('iptv-playlist-list');
              if (d.playlists && d.playlists.length > 0) {
                el.innerHTML = d.playlists.map(p => '<div style="padding: 8px; border-bottom: 1px solid var(--border);">' + p + '</div>').join('');
              } else {
                el.innerHTML = '<i>Chưa có playlist nào</i>';
              }
            });
        }
        loadIptvPlaylists();
      </script>
    </div>

    <!-- TAB 5: OTA UPDATE -->
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

      <!-- CARD: SCREENSCRAPER.FR CONFIGURATION -->
      <div class="card" style="margin-top: 20px; border-color: rgba(2, 132, 199, 0.4);">
        <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px;">
          <h3>🎨 Cấu hình ScreenScraper.fr (Bộ Cào Ảnh Bìa & Thông Tin Game)</h3>
          <span class="badge" style="background:#0284c7; color:#fff; font-size:11px; padding:3px 8px;">CHUẨN QUỐC TẾ</span>
        </div>
        <p style="font-size: 12.5px; color: var(--text-muted); line-height: 1.5; margin-bottom: 12px;">
          RomCloud kết hợp cơ sở dữ liệu chuyên biệt <b>ScreenScraper.fr</b> (chuẩn quốc tế của Skraper trên PC, Skyscraper, Batocera) và kho ảnh chính thức <b>Libretro Thumbnails</b>. Hệ thống hoàn toàn không dùng Wikipedia để bảo đảm ảnh bìa và cốt truyện chính xác 100%.
        </p>

        <form id="form-screenscraper-config" onsubmit="saveScreenScraperConfig(event)" style="display: flex; flex-direction: column; gap: 10px;">
          <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 12px;">
            <div>
              <label style="font-size: 11.5px; color: var(--text-dim); display: block; margin-bottom: 4px;">Tên tài khoản ScreenScraper (ssid):</label>
              <input type="text" id="input-ss-user" placeholder="Ví dụ: myusername" style="width: 100%; padding: 8px 10px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 12px;">
            </div>
            <div>
              <label style="font-size: 11.5px; color: var(--text-dim); display: block; margin-bottom: 4px;">Mật khẩu ScreenScraper (sspassword):</label>
              <input type="password" id="input-ss-pass" placeholder="•••••••• (để trống nếu không đổi)" style="width: 100%; padding: 8px 10px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 12px;">
            </div>
          </div>

          <details style="font-size: 11.5px; color: var(--text-dim);">
            <summary style="cursor: pointer; color: #38bdf8;">⚙️ Tùy chọn Developer ID & Dev Password (nâng cao - không bắt buộc)</summary>
            <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-top: 8px;">
              <div>
                <label style="font-size: 11px; display: block; margin-bottom: 3px;">Developer ID (devid):</label>
                <input type="text" id="input-ss-devid" placeholder="Mặc định: bun2it" style="width: 100%; padding: 6px 8px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 11px;">
              </div>
              <div>
                <label style="font-size: 11px; display: block; margin-bottom: 3px;">Developer Password (devpassword):</label>
                <input type="password" id="input-ss-devpass" placeholder="Mặc định của RomCloud" style="width: 100%; padding: 6px 8px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 11px;">
              </div>
            </div>
          </details>

          <div style="display: flex; gap: 8px; margin-top: 6px; flex-wrap: wrap;">
            <button type="submit" class="btn btn-primary" style="font-size: 12px; padding: 7px 14px;">💾 Lưu cấu hình ScreenScraper</button>
            <button type="button" class="btn btn-secondary" onclick="testScreenScraperConn()" style="font-size: 12px; padding: 7px 14px;">🔌 Kiểm tra kết nối</button>
            <a href="https://www.screenscraper.fr/" target="_blank" rel="noopener noreferrer" class="btn btn-secondary" style="font-size: 12px; padding: 7px 14px; text-decoration: none;">🌐 Đăng ký tài khoản miễn phí ↗</a>
          </div>
          <div id="ss-test-status" style="font-size: 12px; margin-top: 4px; display: none;"></div>
        </form>
      </div>
    </div>
  <!-- MODAL: GAME DETAIL & METADATA -->
  <div id="game-detail-modal" style="display: none; position: fixed; inset: 0; background: rgba(0,0,0,0.8); z-index: 1000; align-items: center; justify-content: center; padding: 20px; backdrop-filter: blur(4px);">
    <div style="background: var(--card-bg); border: 1px solid var(--border); border-radius: 12px; max-width: 680px; width: 100%; max-height: 90vh; overflow-y: auto; box-shadow: 0 20px 25px -5px rgba(0, 0, 0, 0.5);">
      <div style="padding: 16px 20px; border-bottom: 1px solid var(--border); display: flex; justify-content: space-between; align-items: center;">
        <h3 id="modal-game-title" style="margin: 0; font-size: 17px; color: #fff; font-weight: 700;">Chi tiết ROM Game</h3>
        <button type="button" class="btn btn-secondary" onclick="closeGameModal()" style="padding: 4px 10px; font-size: 14px;">✕</button>
      </div>
      <div style="padding: 20px; display: flex; gap: 20px; flex-wrap: wrap;">
        <!-- Left: Cover Art -->
        <div style="flex: 0 0 160px; text-align: center;">
          <div id="modal-cover-wrap" style="width: 160px; height: 210px; background: var(--bg); border: 1px solid var(--border); border-radius: 8px; overflow: hidden; display: flex; align-items: center; justify-content: center;">
            <img id="modal-cover-img" src="" alt="Cover" style="width: 100%; height: 100%; object-fit: contain; display: none;">
            <div id="modal-cover-placeholder" style="font-size: 40px; color: var(--text-dim);">🎮</div>
          </div>
          <span id="modal-sys-tag" class="sys-tag sys-default" style="margin-top: 10px; display: inline-block;">GBA</span>
        </div>
        <!-- Right: Metadata & Info -->
        <div style="flex: 1; min-width: 260px;">
          <div style="font-size: 12px; color: var(--text-dim); font-family: monospace; margin-bottom: 10px;" id="modal-game-filename">filename.gba</div>
          <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-bottom: 14px; font-size: 12px;">
            <div style="background: var(--card-alt); padding: 8px 10px; border-radius: 6px;">
              <span style="color: var(--text-dim); display: block; font-size: 11px;">📅 Năm phát hành</span>
              <b id="modal-release-year" style="color: #fff;">--</b>
            </div>
            <div style="background: var(--card-alt); padding: 8px 10px; border-radius: 6px;">
              <span style="color: var(--text-dim); display: block; font-size: 11px;">🏷️ Thể loại</span>
              <b id="modal-genre" style="color: #38bdf8;">--</b>
            </div>
            <div style="background: var(--card-alt); padding: 8px 10px; border-radius: 6px;">
              <span style="color: var(--text-dim); display: block; font-size: 11px;">🏢 Nhà phát triển</span>
              <b id="modal-developer" style="color: #cbd5e1;">--</b>
            </div>
            <div style="background: var(--card-alt); padding: 8px 10px; border-radius: 6px;">
              <span style="color: var(--text-dim); display: block; font-size: 11px;">💾 Dung lượng / Trạng thái</span>
              <b id="modal-status" style="color: #fff;">--</b>
            </div>
          </div>
          <div style="margin-bottom: 6px; font-size: 12px; font-weight: 600; color: var(--text-muted);">📖 Tóm tắt &amp; Cốt truyện game:</div>
          <div id="modal-description" style="font-size: 12px; color: #cbd5e1; line-height: 1.6; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; padding: 10px 12px; max-height: 140px; overflow-y: auto;">
            Chưa có dữ liệu mô tả cho game này. Bấm "Cào lại Bìa &amp; Thông tin" bên dưới để tìm nạp tự động.
          </div>

          <!-- MANUAL SCRAPER REFINEMENT SEARCH (LIKE SCRAPE-EDIT) -->
          <div style="margin-top: 14px; border-top: 1px solid var(--border); padding-top: 10px;">
            <div style="font-size: 11.5px; font-weight: 600; color: #38bdf8; margin-bottom: 6px;">🔍 Tìm kiếm &amp; Chọn bìa thủ công (ScreenScraper / Libretro):</div>
            <div style="display: flex; gap: 6px;">
              <input type="text" id="modal-search-query" placeholder="Nhập tên game để tìm..." style="flex: 1; padding: 6px 10px; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; color: #fff; font-size: 12px;">
              <button type="button" class="btn btn-secondary" onclick="searchModalScraperCandidates()" style="font-size: 11px; padding: 6px 12px; border-color: #0284c7; color: #38bdf8;">🔎 Tìm kiếm</button>
            </div>
            <div id="modal-candidates-list" style="margin-top: 8px; max-height: 180px; overflow-y: auto; display: none; flex-direction: column; gap: 6px;"></div>
          </div>
        </div>
      </div>
      <div style="padding: 12px 20px; border-top: 1px solid var(--border); display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 8px;">
        <div style="display: flex; gap: 8px; flex-wrap: wrap;">
          <button type="button" class="btn btn-primary" id="btn-modal-scrape" onclick="scrapeCurrentModalGame()" style="background: var(--purple);">🎨 Cào lại Bìa &amp; Thông tin</button>
          <button type="button" class="btn btn-primary" id="btn-modal-backup" onclick="uploadCurrentModalGame()" style="background: #7c3aed; border-color: #6d28d9; display: none;">📤 Sao lưu lên Drive cá nhân</button>
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
          <span style="font-size: 22px;">🩺</span>
          <div>
            <h3 style="margin: 0; font-size: 16px; color: #fff; font-weight: 700;">Bác Sĩ ROM - Tự Động Sửa ROM Đặt Sai Emulator</h3>
            <span style="font-size: 11.5px; color: var(--text-dim);">Tự động phát hiện ROM bỏ nhầm thư mục và dời về đúng hệ máy</span>
          </div>
        </div>
        <button type="button" class="btn btn-secondary" onclick="closeDoctorModal()" style="padding: 4px 10px; font-size: 14px;">✕</button>
      </div>

      <div style="padding: 16px 20px; overflow-y: auto; flex: 1;">
        <!-- Status / Audit Card -->
        <div id="doctor-loading" style="display: none; text-align: center; padding: 30px;">
          <div style="font-size: 28px; animation: spin 1.5s linear infinite; display: inline-block;">⏳</div>
          <div style="margin-top: 10px; color: #e2e8f0; font-size: 14px;">Đang khám và quét toàn bộ thư mục ROM trên thẻ nhớ TrimUI...</div>
        </div>

        <div id="doctor-results" style="display: block;">
          <div id="doctor-summary-box" style="padding: 12px 14px; border-radius: 8px; margin-bottom: 14px; font-size: 13px; line-height: 1.5;"></div>

          <!-- List of detected misplaced ROMs -->
          <div id="doctor-items-wrap" style="max-height: 280px; overflow-y: auto; display: flex; flex-direction: column; gap: 8px;"></div>
        </div>

        <!-- Drag & Drop Upload Zone -->
        <div style="margin-top: 16px; padding: 14px; border: 2px dashed rgba(16,185,129,0.4); border-radius: 8px; background: rgba(16,185,129,0.04); text-align: center;">
          <div style="font-size: 13px; font-weight: 600; color: #34d399; margin-bottom: 4px;">📥 Nạp ROM Mới Tự Động (Chọn file từ Máy tính / Điện thoại)</div>
          <div style="font-size: 11.5px; color: var(--text-dim); margin-bottom: 10px;">Thả file ROM bất kỳ vào đây, hệ thống sẽ tự nhận diện và cất vào đúng thư mục Emulator trên máy!</div>
          <input type="file" id="doctor-file-input" style="display: none;" onchange="uploadAutoRomFile(this.files[0])">
          <button type="button" class="btn btn-secondary" onclick="document.getElementById('doctor-file-input').click()" style="font-size: 12px; padding: 6px 14px; border-color: #10b981; color: #34d399;">📁 Chọn File ROM</button>
          <div id="doctor-upload-status" style="margin-top: 8px; font-size: 12px; display: none;"></div>
        </div>
      </div>

      <div style="padding: 12px 20px; border-top: 1px solid var(--border); display: flex; justify-content: space-between; align-items: center; background: rgba(0,0,0,0.2);">
        <button type="button" class="btn btn-secondary" onclick="runDoctorAudit()">🔄 Khám lại</button>
        <div style="display: flex; gap: 8px;">
          <button type="button" class="btn btn-primary" id="btn-doctor-fix" onclick="executeDoctorFix()" style="background: #10b981; border-color: #059669; font-weight: 600;">🩺 Tự Động Chuyển Về Đúng Emulator</button>
          <button type="button" class="btn btn-secondary" onclick="closeDoctorModal()">Đóng</button>
        </div>
      </div>
    </div>
  </div>

  <!-- FLOATING ACTIVE UPLOAD CARD -->
  <div id="active-upload-floating">
    <div style="display: flex; justify-content: space-between; align-items: flex-start; margin-bottom: 6px;">
      <div>
        <span class="badge" style="background:#9333ea;color:#fff;font-size:10px;padding:2px 6px;">📤 SAO LƯU DRIVE</span>
        <span style="font-size: 13.5px; font-weight: 700; margin-left: 6px; color:#fff;" id="upload-float-title">Game Title</span>
      </div>
      <button class="btn btn-secondary" onclick="cancelActiveUpload()" style="padding: 2px 8px; font-size: 11px;">✕ Hủy</button>
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
        <span class="badge" style="background:#0284c7;color:#fff;font-size:10px;padding:2px 6px;">🎨 ĐANG CÀO DỮ LIỆU</span>
        <span style="font-size: 13px; font-weight: 700; margin-left: 6px; color:#fff;" id="scrape-float-title">Game Title</span>
      </div>
      <button class="btn btn-secondary" onclick="cancelAutoScrape()" style="padding: 2px 8px; font-size: 11px;">✕ Hủy</button>
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
      loadScreenScraperConfig();
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
        loadScreenScraperConfig();
      }
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
        if (tbody) tbody.innerHTML = `<tr><td colspan="5" style="text-align: center; padding: 25px; color: var(--red);">Lỗi nạp dữ liệu. Vui lòng thử lại.</td></tr>`;
        if (gridWrap) gridWrap.innerHTML = `<div style="grid-column: 1 / -1; text-align: center; padding: 40px; color: var(--red);">Lỗi nạp dữ liệu. Vui lòng thử lại.</div>`;
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
          <div style="font-size: 38px; margin-bottom: 12px;">☁️❌</div>
          <div style="font-size: 16px; font-weight: 700; color: #fef3c7; margin-bottom: 6px;">Google Drive chưa được kết nối (Đã ngắt kết nối)</div>
          <div style="font-size: 13px; color: var(--text-muted); margin-bottom: 18px;">Chỉ khi còn kết nối Google Drive thì hệ thống mới hiển thị danh mục ROM game.</div>
          <button class="btn btn-primary" onclick="switchTab('tab-storage')">🔗 Kết nối Google Drive ngay</button>
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
        if (tbody) tbody.innerHTML = `<tr><td colspan="5" style="text-align: center; padding: 30px; color: var(--text-dim);">Không tìm thấy game nào phù hợp với bộ lọc.</td></tr>`;
        if (gridWrap) gridWrap.innerHTML = `<div style="grid-column: 1 / -1; text-align: center; padding: 40px 20px; color: var(--text-dim);">Không tìm thấy game nào phù hợp với bộ lọc.</div>`;
        return;
      }

      let tableHtml = '';
      let gridHtml = '';
      for (const g of games) {
        const sysClass = 'sys-' + (g.sys_code || 'default');
        let statusBadge = '';
        let statusPillClass = '';
        let statusPillText = '';
        const detailBtn = `<button class="btn btn-secondary" onclick="openGameModal(${g.id})" title="Xem chi tiết & Cốt truyện game">ℹ️ Chi tiết</button>`;
        const scrapeBtn = `<button class="btn btn-secondary" onclick="scrapeBoxart(${g.id})" title="Cào ảnh bìa & thông tin game">🎨 Scrape</button>`;
        let tableActions = '';
        let gridActions = '';

        if (g.local_state === 1) {
          statusBadge = `<span class="status-badge status-local" title="ROM này đã có trên thẻ nhớ TrimUI">🟢 Trên thẻ SD</span>`;
          statusPillClass = 'status-local';
          statusPillText = '🟢 Trên thẻ SD';
          const backupBtn = `<button class="btn btn-primary" onclick="uploadGameToDrive(${g.id}, '${escapeHtml(g.title)}')" style="background:var(--purple);border-color:#9333ea;" title="Sao lưu ROM này từ thẻ nhớ lên Google Drive cá nhân (/RomCloud_Backup)">📤 Sao lưu</button>`;
          tableActions = `
            ${backupBtn}
            ${detailBtn}
            ${scrapeBtn}
            <button class="btn btn-danger" onclick="deleteRom(${g.id}, '${escapeHtml(g.title)}')">🗑️ Xóa</button>
          `;
          gridActions = `
            ${backupBtn}
            ${detailBtn}
            ${scrapeBtn}
            <button class="btn btn-danger" onclick="deleteRom(${g.id}, '${escapeHtml(g.title)}')">🗑️ Xóa</button>
          `;
        } else if (g.in_queue) {
          statusBadge = `<span class="status-badge status-queue" title="Đang chờ tải về máy TrimUI">⏳ Đang tải về</span>`;
          statusPillClass = 'status-queue';
          statusPillText = '⏳ Đang tải';
          tableActions = `${detailBtn} ${scrapeBtn} <button class="btn btn-secondary" onclick="removeFromQueue(${g.id})">✕ Bỏ</button>`;
          gridActions = `${detailBtn} ${scrapeBtn} <button class="btn btn-secondary" onclick="removeFromQueue(${g.id})">✕ Bỏ</button>`;
        } else {
          statusBadge = `<span class="status-badge status-cloud" title="ROM trên kho Google Drive công khai, chưa tải về thẻ nhớ">☁️ Kho Public</span>`;
          statusPillClass = 'status-cloud';
          statusPillText = '☁️ Kho Public';
          tableActions = `${detailBtn} ${scrapeBtn} <button class="btn btn-primary" onclick="downloadGame(${g.id})" title="Tải ROM này từ kho Public về thẻ nhớ TrimUI">📥 Tải về máy</button>`;
          gridActions = `${detailBtn} ${scrapeBtn} <button class="btn btn-primary" onclick="downloadGame(${g.id})" title="Tải ROM này từ kho Public về thẻ nhớ TrimUI">📥 Tải về máy</button>`;
        }

        let metaBadges = '';
        if (g.release_year) metaBadges += `<span class="badge" style="background:#334155;color:#94a3b8;font-size:10px;padding:2px 6px;margin-right:4px;">📅 ${escapeHtml(g.release_year)}</span>`;
        if (g.genre) metaBadges += `<span class="badge" style="background:#1e293b;border:1px solid #475569;color:#38bdf8;font-size:10px;padding:2px 6px;">🏷️ ${escapeHtml(g.genre)}</span>`;

        let coverHtml = '';
        if (g.has_cover) {
          coverHtml = `<img class="card-cover-img" src="/api/cover?game_id=${g.id}" alt="${escapeHtml(g.title)}" loading="lazy" onerror="this.onerror=null;this.parentElement.innerHTML='<div class=\\'card-cover-placeholder\\'><div class=\\'card-cover-icon\\'>🎮</div><span style=\\'font-size:11px;\\'>${escapeHtml(g.sys_code || 'GAME')}</span></div>';">`;
        } else {
          coverHtml = `
            <div class="card-cover-placeholder">
              <div class="card-cover-icon">🎮</div>
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
                  <span>💾 ${g.size_str || '0 B'}</span>
                  ${g.release_year ? `<span style="margin-left: 6px; color: #94a3b8;">📅 ${escapeHtml(g.release_year)}</span>` : ''}
                </div>
                ${g.genre ? `<div style="font-size: 11px; color: #38bdf8; margin-top: 3px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;">🏷️ ${escapeHtml(g.genre)}</div>` : ''}
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

        document.getElementById('modal-game-title').textContent = g.title || 'Game ROM';
        document.getElementById('modal-game-filename').textContent = g.filename || '';
        document.getElementById('modal-sys-tag').textContent = g.sys_code || 'GAME';
        document.getElementById('modal-release-year').textContent = g.release_year || 'Chưa có thông tin';
        document.getElementById('modal-genre').textContent = g.genre || 'Chưa phân loại';
        document.getElementById('modal-developer').textContent = g.developer || 'Chưa rõ';
        document.getElementById('modal-status').innerHTML = `${g.size_str || '0 B'} • ${g.local_state === 1 ? '<span style="color:var(--green);">🟢 Đã tải về máy</span>' : '<span style="color:var(--accent);">☁️ Trên Google Drive</span>'}`;
        document.getElementById('modal-description').textContent = g.description || 'Chưa có dữ liệu mô tả cho game này. Bấm nút "Cào lại Bìa & Thông tin" bên dưới để tìm nạp tự động từ Libretro và Wikipedia.';

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
      if (!query) { showToast('Vui lòng nhập tên game để tìm kiếm.'); return; }

      const cList = document.getElementById('modal-candidates-list');
      if (cList) {
        cList.style.display = 'flex';
        cList.innerHTML = '<div style="color:var(--text-dim); padding:10px; text-align:center;">⏳ Đang tìm kiếm trên ScreenScraper &amp; Libretro...</div>';
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
          if (cList) cList.innerHTML = '<div style="color:var(--text-dim); padding:10px; text-align:center;">Không tìm thấy kết quả phù hợp. Hãy thử tên ngắn gọn hơn.</div>';
          return;
        }

        let html = '';
        modalCandidatesCache.forEach((c, idx) => {
          html += `
            <div style="display:flex; gap:10px; align-items:center; background:var(--bg); border:1px solid var(--border); border-radius:6px; padding:8px 10px;">
              <div style="width:40px; height:50px; flex-shrink:0; background:#000; border-radius:4px; overflow:hidden; display:flex; align-items:center; justify-content:center;">
                ${c.cover_url ? `<img src="${c.cover_url}" style="width:100%; height:100%; object-fit:cover;" onerror="this.style.display='none';">` : '🎮'}
              </div>
              <div style="flex:1; min-width:0;">
                <div style="font-size:12px; font-weight:600; color:#fff; overflow:hidden; text-overflow:ellipsis; white-space:nowrap;">${escapeHtml(c.title)}</div>
                <div style="font-size:11px; color:var(--text-dim);">${escapeHtml(c.source || '')} ${c.release_year ? '• ' + c.release_year : ''}</div>
              </div>
              <button type="button" class="btn btn-primary" onclick="applySelectedCandidate(${idx})" style="font-size:11px; padding:4px 10px; white-space:nowrap;">Áp dụng bìa này</button>
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

      showToast('⏳ Đang tải và áp dụng ảnh bìa...');
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
        showToast(data.message || 'Đã áp dụng thông tin & ảnh bìa game!');
        await openGameModal(currentModalGameId);
        loadGames();
      } catch (e) {
        showToast('❌ Lỗi khi áp dụng bìa game.');
      }
    }

    async function scrapeCurrentModalGame() {
      if (!currentModalGameId) return;
      const btn = document.getElementById('btn-modal-scrape');
      const origText = btn ? btn.innerHTML : '🎨 Cào lại Bìa & Thông tin';
      if (btn) { btn.disabled = true; btn.innerHTML = '⏳ Đang cào dữ liệu...'; }
      showToast('⏳ Đang tìm kiếm ảnh bìa và thông tin trên ScreenScraper & Libretro...');

      try {
        const res = await fetch('/api/scrape_cover', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: `game_id=${currentModalGameId}`
        });
        const data = await res.json();
        showToast(data.message || 'Cào dữ liệu hoàn tất!');
        await openGameModal(currentModalGameId);
        loadGames();
      } catch (e) {
        showToast('Lỗi khi cào dữ liệu game.');
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
            summaryBox.innerHTML = '<b>✅ Tuyệt vời!</b> Toàn bộ ROM trên thẻ nhớ của bạn đều đang nằm đúng thư mục Emulator chuẩn của TrimUI. Không phát hiện ROM nào bị lạc chỗ.';
          }
          if (fixBtn) fixBtn.style.display = 'none';
        } else {
          if (summaryBox) {
            summaryBox.style.background = 'rgba(245,158,11,0.15)';
            summaryBox.style.border = '1px solid #f59e0b';
            summaryBox.style.color = '#fbbf24';
            summaryBox.innerHTML = `<b>⚠️ Phát hiện ${doctorAuditItems.length} ROM đang đặt sai thư mục!</b><br>Các game này có thể không chạy được hoặc bị lỗi nếu không dời về đúng Emulator. Bấm nút màu xanh bên dưới để RomCloud tự động sắp xếp lại.`;
          }
          if (fixBtn) {
            fixBtn.style.display = 'inline-block';
            fixBtn.disabled = false;
            fixBtn.innerHTML = `🩺 Tự Động Chuyển ${doctorAuditItems.length} ROM Về Đúng Emulator`;
          }

          if (itemsWrap) {
            itemsWrap.innerHTML = doctorAuditItems.map(item => `
              <div style="background: var(--bg); border: 1px solid var(--border); border-radius: 8px; padding: 10px 12px; display: flex; justify-content: space-between; align-items: center; gap: 10px;">
                <div style="min-width: 0; flex: 1;">
                  <div style="font-weight: 600; color: #fff; font-size: 13px; text-overflow: ellipsis; overflow: hidden; white-space: nowrap;">${escapeHtml(item.filename)}</div>
                  <div style="font-size: 11.5px; color: var(--text-dim); margin-top: 3px;">
                    Hiện tại: <span style="color: #f87171; background: rgba(239,68,68,0.15); padding: 1px 6px; border-radius: 4px; font-weight: 600;">/Roms/${escapeHtml(item.current_system)}</span>
                    ➔ Cần dời về: <span style="color: #34d399; background: rgba(16,185,129,0.15); padding: 1px 6px; border-radius: 4px; font-weight: 600;">/Roms/${escapeHtml(item.detected_system)} (${escapeHtml(item.detected_name)})</span>
                  </div>
                  <div style="font-size: 11px; color: #94a3b8; margin-top: 2px;">💡 Lý do: ${escapeHtml(item.reason)}</div>
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
          summaryBox.innerHTML = '❌ Lỗi khi kiểm tra thư mục ROM trên thẻ nhớ.';
        }
      }
    }

    async function executeDoctorFix() {
      const fixBtn = document.getElementById('btn-doctor-fix');
      if (fixBtn) {
        fixBtn.disabled = true;
        fixBtn.innerHTML = '⏳ Đang tự động chuyển file & Boxart...';
      }
      showToast('⏳ Bác sĩ ROM đang điều chuyển các game về đúng Emulator...');

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
          summaryBox.innerHTML = `<b>🎉 THÀNH CÔNG!</b> Đã tự động dời ${successCount} ROM về đúng thư mục Emulator trên thẻ nhớ. Toàn bộ game và Boxart đã sẵn sàng để bạn chơi ngay trên TrimUI!`;
        }

        const itemsWrap = document.getElementById('doctor-items-wrap');
        if (itemsWrap) {
          itemsWrap.innerHTML = fixedItems.map(item => `
            <div style="background: rgba(16,185,129,0.06); border: 1px solid rgba(16,185,129,0.3); border-radius: 8px; padding: 8px 12px; font-size: 12px; color: #e2e8f0; display: flex; align-items: center; justify-content: space-between;">
              <div>
                <b>${escapeHtml(item.filename)}</b>
                <div style="font-size: 11px; color: #34d399;">${escapeHtml(item.status)}</div>
              </div>
              <span style="font-size: 16px;">✅</span>
            </div>
          `).join('');
        }

        if (fixBtn) fixBtn.style.display = 'none';
        showToast(`🎉 Đã sửa xong ${successCount} ROM đặt nhầm!`);
        loadGames();
        loadStorageInfo();
      } catch (e) {
        showToast('❌ Lỗi khi thực hiện dời ROM.');
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
        statusDiv.innerHTML = `⏳ Đang tải lên và phân tích hệ máy cho <b>${escapeHtml(file.name)}</b> (${(file.size / 1024 / 1024).toFixed(2)} MB)...`;
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
            statusDiv.innerHTML = `✅ ${escapeHtml(data.message || 'Đã phân loại thành công!')}`;
          }
          showToast(`🎉 Nhận diện thành công: ${data.system_name || 'ROM'}! Đã đưa vào Emulator.`);
          setTimeout(() => {
            runDoctorAudit();
            loadGames();
            loadStorageInfo();
          }, 1000);
        } else {
          if (statusDiv) {
            statusDiv.style.color = '#f87171';
            statusDiv.innerHTML = `❌ ${escapeHtml(data.error || 'Lỗi xử lý file')}`;
          }
        }
      } catch (e) {
        if (statusDiv) {
          statusDiv.style.color = '#f87171';
          statusDiv.innerHTML = '❌ Lỗi đường truyền khi tải file lên máy TrimUI.';
        }
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
      showToast('⏳ Đang tìm kiếm ảnh bìa và thông tin trên ScreenScraper & Libretro...');
      try {
        const res = await fetch('/api/scrape_cover', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: `game_id=${gameId}`
        });
        const data = await res.json();
        if (data.success) {
          showToast(data.message || 'Cập nhật ảnh bìa & thông tin hoàn tất!');
          loadGames();
        } else {
          showToast('⚠️ ' + (data.message || 'Không tìm thấy dữ liệu.'));
        }
      } catch (e) {
        showToast('❌ Lỗi cào thông tin game.');
      }
    }

    let isBatchScraping = false;
    async function batchScrapeCurrentPage() {
      if (isBatchScraping) {
        showToast('⚠️ Đang trong quá trình cào thông tin, vui lòng đợi...');
        return;
      }
      if (!lastLoadedGames || !lastLoadedGames.games || lastLoadedGames.games.length === 0) {
        showToast('Không có game nào ở trang hiện tại để cào.');
        return;
      }
      const gamesToScrape = lastLoadedGames.games;
      if (!confirm(`Bạn có muốn tự động cào ảnh bìa và thông tin cho tất cả ${gamesToScrape.length} game trong trang này?`)) return;

      isBatchScraping = true;
      const btn = document.getElementById('btn-batch-scrape');
      const origText = btn ? btn.innerHTML : '🎨 Cào toàn bộ trang';
      if (btn) { btn.disabled = true; btn.innerHTML = '⏳ Đang cào...'; }

      let successCount = 0;
      for (let i = 0; i < gamesToScrape.length; i++) {
        const g = gamesToScrape[i];
        showToast(`⏳ Đang cào (${i + 1}/${gamesToScrape.length}): ${g.title.substring(0, 20)}...`);
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

      showToast(`✅ Đã hoàn tất cào thông tin cho ${successCount}/${gamesToScrape.length} game!`);
      if (btn) { btn.disabled = false; btn.innerHTML = origText; }
      isBatchScraping = false;
      loadGames();
    }

    // Auto-scrape all SD Card ROMs in background
    async function startAutoScrapeSd(force = false) {
      if (!confirm('Tự động cào ảnh bìa và thông tin cốt truyện cho các ROM trên thẻ nhớ (chạy ngầm)?\n\nNguồn dữ liệu: ScreenScraper.fr & Libretro Thumbnails chính thức (Không dùng Wikipedia).')) {
        return;
      }
      try {
        const res = await fetch('/api/auto_scrape_sd', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: `force=${force ? '1' : '0'}`
        });
        const data = await res.json();
        showToast(data.message || 'Đã khởi chạy tiến trình cào tự động!');
        startScrapePolling();
      } catch (e) {
        showToast('❌ Lỗi khi khởi chạy cào tự động.');
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

        if (titleEl) titleEl.textContent = data.current_game || 'ROM Game';
        if (sysEl) sysEl.textContent = `Hệ máy: ${data.current_sys || 'SYS'} (${data.scraped}/${data.total} game)`;
        if (fillEl) fillEl.style.width = (data.progress_pct || 0) + '%';
        if (countEl) countEl.textContent = `Đã cào: ${data.scraped} / ${data.total} (${data.success} thành công)`;
        if (pctEl) pctEl.textContent = `${data.progress_pct || 0}%`;
      } catch (e) {}
    }

    async function cancelAutoScrape() {
      if (!confirm('Bạn có chắc chắn muốn hủy quá trình cào tự động ROM trên thẻ?')) return;
      try {
        await fetch('/api/cancel_auto_scrape', { method: 'POST' });
        showToast('Đã gửi yêu cầu hủy cào tự động.');
        const card = document.getElementById('active-scrape-floating');
        if (card) card.style.display = 'none';
        if (scrapePollTimer) {
          clearInterval(scrapePollTimer);
          scrapePollTimer = null;
        }
      } catch (e) {}
    }

    async function loadScreenScraperConfig() {
      try {
        const res = await fetch('/api/screenscraper_config');
        const data = await res.json();
        const userInput = document.getElementById('input-ss-user');
        const devInput = document.getElementById('input-ss-devid');
        const passInput = document.getElementById('input-ss-pass');
        if (userInput && data.user) userInput.value = data.user;
        if (devInput && data.dev_id) devInput.value = data.dev_id;
        if (passInput && data.has_pass) passInput.placeholder = '•••••••• (Đã lưu mật khẩu)';
      } catch (e) {}
    }

    async function saveScreenScraperConfig(e) {
      e.preventDefault();
      const user = (document.getElementById('input-ss-user')?.value || '').trim();
      const pass = (document.getElementById('input-ss-pass')?.value || '').trim();
      const devId = (document.getElementById('input-ss-devid')?.value || '').trim();
      const devPass = (document.getElementById('input-ss-devpass')?.value || '').trim();

      const params = new URLSearchParams();
      params.append('user', user);
      if (pass) params.append('pass', pass);
      if (devId) params.append('dev_id', devId);
      if (devPass) params.append('dev_pass', devPass);

      try {
        const res = await fetch('/api/save_screenscraper_config', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: params.toString()
        });
        const data = await res.json();
        showToast(data.message || 'Đã lưu cấu hình ScreenScraper.fr');
        const st = document.getElementById('ss-test-status');
        if (st) {
          st.style.display = 'block';
          st.innerHTML = '<span style="color:var(--green);">✅ Đã lưu cấu hình. Hãy bấm "Kiểm tra kết nối" để xác thực tài khoản.</span>';
        }
      } catch (e) {
        showToast('❌ Lỗi lưu cấu hình.');
      }
    }

    async function testScreenScraperConn() {
      const user = (document.getElementById('input-ss-user')?.value || '').trim();
      const pass = (document.getElementById('input-ss-pass')?.value || '').trim();
      const devId = (document.getElementById('input-ss-devid')?.value || '').trim();
      const devPass = (document.getElementById('input-ss-devpass')?.value || '').trim();

      const st = document.getElementById('ss-test-status');
      if (st) {
        st.style.display = 'block';
        st.innerHTML = '<span style="color:var(--accent);">⏳ Đang kết nối đến API ScreenScraper.fr...</span>';
      }

      const params = new URLSearchParams();
      params.append('user', user);
      if (pass) params.append('pass', pass);
      if (devId) params.append('dev_id', devId);
      if (devPass) params.append('dev_pass', devPass);

      try {
        const res = await fetch('/api/test_screenscraper', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: params.toString()
        });
        const data = await res.json();
        if (st) {
          if (data.success) {
            st.innerHTML = `<span style="color:var(--green);">🟢 ${escapeHtml(data.message)}</span>`;
          } else {
            st.innerHTML = `<span style="color:var(--red);">🔴 ${escapeHtml(data.message)}</span>`;
          }
        }
      } catch (e) {
        if (st) st.innerHTML = '<span style="color:var(--red);">🔴 Lỗi kết nối kiểm tra.</span>';
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

    // Google Drive Personal Backup Actions
    async function uploadGameToDrive(gameId, title) {
      if (!canUploadGlobal) {
        if (confirm('⚠️ Bạn chưa cấp Token Google Drive cá nhân để sao lưu!\n\nBạn có muốn chuyển sang Tab "Cài đặt Cloud" để nhập Refresh Token hoặc Access Token theo hướng dẫn không?')) {
          switchTab('tab-storage');
        }
        return;
      }

      if (!confirm(`Sao lưu ROM "${title}" từ thẻ nhớ TrimUI lên thư mục /RomCloud_Backup trên Google Drive cá nhân của bạn?`)) {
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
          showToast(data.message || 'Đã bắt đầu sao lưu ROM lên Google Drive!');
          startUploadPolling();
        } else {
          showToast('⚠️ ' + (data.message || 'Không thể bắt đầu sao lưu.'));
        }
      } catch (e) {
        showToast('❌ Lỗi khi gửi yêu cầu sao lưu.');
      }
    }

    async function uploadAllGamesToDrive() {
      if (!canUploadGlobal) {
        if (confirm('⚠️ Bạn chưa cài đặt Token Google Drive cá nhân để cấp quyền sao lưu!\n\nBạn có muốn chuyển sang Tab "Cài đặt Cloud" để nhập Refresh Token hoặc Access Token theo hướng dẫn không?')) {
          switchTab('tab-storage');
        }
        return;
      }

      if (!confirm('Bạn có muốn sao lưu TOÀN BỘ game hiện có trên thẻ nhớ TrimUI lên thư mục /RomCloud_Backup trên Google Drive cá nhân của bạn?\n\nQuá trình sao lưu sẽ chạy ngầm lần lượt từng game.')) {
        return;
      }

      try {
        const res = await fetch('/api/upload_all', { method: 'POST' });
        const data = await res.json();
        if (data.success) {
          showToast(data.message || 'Đã bắt đầu sao lưu toàn bộ thẻ nhớ lên Google Drive!');
          startUploadPolling();
        } else {
          showToast('⚠️ ' + (data.message || 'Không thể bắt đầu sao lưu.'));
        }
      } catch (e) {
        showToast('❌ Lỗi khi gửi yêu cầu sao lưu toàn bộ.');
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
        if (!data.in_progress) {
          if (floatCard) floatCard.style.display = 'none';
          if (uploadPollTimer) {
            clearInterval(uploadPollTimer);
            uploadPollTimer = null;
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

        if (titleEl) titleEl.textContent = data.current_game || 'Game ROM';
        if (fileEl) fileEl.textContent = `${data.current_file || ''} (${data.uploaded_count}/${data.total_games} game)`;
        if (fillEl) fillEl.style.width = (data.progress_pct || 0) + '%';
        if (speedEl) speedEl.textContent = `${data.speed_kbps || 0} KB/s`;
        if (pctEl) pctEl.textContent = `${data.progress_pct || 0}%`;
        if (bytesEl) bytesEl.textContent = `${data.uploaded_mb || '0'} / ${data.total_mb || '0'} MB`;
      } catch (e) {}
    }

    async function cancelActiveUpload() {
      if (!confirm('Bạn có chắc chắn muốn hủy quá trình sao lưu hiện tại?')) return;
      try {
        await fetch('/api/cancel_upload', { method: 'POST' });
        showToast('Đã gửi yêu cầu hủy sao lưu.');
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

        const backupBadge = document.getElementById('backup-perm-badge');
        const btnClearToken = document.getElementById('btn-clear-personal-token');
        if (backupBadge) {
          if (data.can_upload) {
            backupBadge.innerHTML = `<span style="color:var(--green);">🟢 Đã kích hoạt</span> (${escapeHtml(data.user_email || 'Google Drive cá nhân')} - Sao lưu vào thư mục /RomCloud_Backup)`;
            if (btnClearToken) btnClearToken.style.display = 'inline-block';
          } else {
            backupBadge.innerHTML = `<span style="color:var(--yellow);">⚪ Chưa kích hoạt</span> (Chế độ hiện tại chỉ cho phép tải về)`;
            if (btnClearToken) btnClearToken.style.display = 'none';
          }
        }

        canUploadGlobal = !!data.can_upload;

        // Update Architecture Banner cards in Tab 1
        const archPublicUrl = document.getElementById('arch-public-url');
        if (archPublicUrl) {
          archPublicUrl.textContent = data.is_linked ? (data.drive_url || 'Đã liên kết kho ROM') : 'Chưa kết nối kho ROM';
        }
        const archLocalStatus = document.getElementById('arch-local-status');
        if (archLocalStatus) {
          archLocalStatus.innerHTML = `${data.total_local || 0} ROMs trên thẻ • <span style="color:var(--text-dim);">${data.avail_str || '--'} trống</span>`;
        }
        const archBackupBadge = document.getElementById('arch-backup-badge');
        if (archBackupBadge) {
          if (data.can_upload) {
            archBackupBadge.innerHTML = `<span style="color:var(--green);">🟢 Sẵn sàng sao lưu</span> (${escapeHtml(data.user_email || 'Drive cá nhân')})`;
          } else {
            archBackupBadge.innerHTML = `<span style="color:var(--yellow);">⚪ Chưa cấp Token sao lưu</span>`;
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

    async function savePersonalToken() {
      const input = document.getElementById('input-personal-token');
      const btn = document.getElementById('btn-save-personal-token');
      const statusBox = document.getElementById('personal-token-status');
      const val = (input ? input.value : '').trim();

      if (!val) {
        showToast('⚠️ Vui lòng dán Access Token hoặc Refresh Token vào ô.');
        if (input) input.focus();
        return;
      }

      const origText = btn ? btn.innerHTML : '💾 Kích hoạt Sao lưu';
      if (btn) {
        btn.disabled = true;
        btn.innerHTML = '⏳ Đang xác thực với Google...';
      }
      if (statusBox) statusBox.innerHTML = '<span style="color:var(--accent);">⏳ Đang kiểm tra token với Google Drive API...</span>';

      try {
        const body = 'token=' + encodeURIComponent(val);
        const res = await fetch('/api/set_personal_auth', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: body
        });
        const data = await res.json();
        if (data.success) {
          showToast(data.message || 'Kích hoạt sao lưu thành công!');
          if (statusBox) statusBox.innerHTML = `<span style="color:var(--green);">✅ ${escapeHtml(data.message || 'Đã kích hoạt sao lưu thành công!')}</span>`;
          if (input) input.value = '';
          await loadStorageInfo();
        } else {
          showToast('❌ ' + (data.error || 'Xác thực token thất bại.'));
          if (statusBox) statusBox.innerHTML = `<span style="color:var(--red);">❌ ${escapeHtml(data.error || 'Xác thực token thất bại.')}</span>`;
        }
      } catch (e) {
        showToast('❌ Lỗi kết nối đến máy chủ RomCloud.');
        if (statusBox) statusBox.innerHTML = '<span style="color:var(--red);">❌ Lỗi kết nối máy chủ RomCloud.</span>';
      } finally {
        if (btn) {
          btn.disabled = false;
          btn.innerHTML = origText;
        }
      }
    }

    async function clearPersonalToken() {
      if (!confirm('Bạn có chắc muốn hủy quyền sao lưu cá nhân? (Hệ thống sẽ trở về chế độ chỉ tải game về từ kho công khai)')) return;
      try {
        const res = await fetch('/api/set_personal_auth', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'action=clear'
        });
        const data = await res.json();
        showToast(data.message || 'Đã hủy quyền sao lưu cá nhân.');
        const statusBox = document.getElementById('personal-token-status');
        if (statusBox) statusBox.innerHTML = '';
        await loadStorageInfo();
      } catch (e) {
        showToast('Lỗi khi hủy token.');
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
            json += "\"has_cover\":" + std::string(g.coverPath.empty() ? "false" : "true") + ",";
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
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "GET" && path == "/api/game_detail") {
        std::string gameIdStr = extractQueryParam(queryString, "game_id");
        int64_t gameId = 0;
        try { gameId = std::stoll(gameIdStr); } catch(...) {}
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
            json += "\"sys_code\":\"" + escapeJson(sys.code.empty() ? g.systemCode : sys.code) + "\",";
            json += "\"sys_name\":\"" + escapeJson(sys.name) + "\",";
            json += "\"size_str\":\"" + FileSystemManager::instance().formatBytes(g.sizeBytes) + "\",";
            json += "\"local_state\":" + std::to_string(static_cast<int>(g.localState)) + ",";
            json += "\"in_queue\":" + std::string(inQueue ? "true" : "false") + ",";
            json += "\"has_cover\":" + std::string(g.coverPath.empty() ? "false" : "true") + ",";
            json += "\"release_year\":\"" + escapeJson(g.releaseYear) + "\",";
            json += "\"genre\":\"" + escapeJson(g.genre) + "\",";
            json += "\"developer\":\"" + escapeJson(g.developer) + "\",";
            json += "\"description\":\"" + escapeJson(g.description) + "\"";
            json += "}";
            std::string res = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json; charset=UTF-8\r\n"
                              "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                              "Access-Control-Allow-Origin: *\r\n"
                              "Content-Length: " + std::to_string(json.length()) + "\r\n"
                              "Connection: close\r\n\r\n" + json;
            send(clientFd, res.c_str(), res.length(), 0);
            return;
        } else {
            std::string json = "{\"error\":\"Game not found\"}";
            std::string res = "HTTP/1.1 404 Not Found\r\n"
                              "Content-Type: application/json; charset=UTF-8\r\n"
                              "Access-Control-Allow-Origin: *\r\n"
                              "Content-Length: " + std::to_string(json.length()) + "\r\n"
                              "Connection: close\r\n\r\n" + json;
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
    } else if (method == "GET" && path == "/api/cover") {
        std::string gameIdStr = extractQueryParam(queryString, "game_id");
        int64_t gameId = 0;
        try { if (!gameIdStr.empty()) gameId = std::stoll(gameIdStr); } catch(...) {}

        GameRecord game;
        bool found = (gameId > 0 && DatabaseManager::instance().getGameById(gameId, game) && !game.coverPath.empty());
        if (found) {
            std::ifstream file(game.coverPath, std::ios::binary);
            if (file.is_open()) {
                std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                file.close();
                std::string mime = "image/png";
                if (game.coverPath.rfind(".jpg") != std::string::npos || game.coverPath.rfind(".jpeg") != std::string::npos) {
                    mime = "image/jpeg";
                } else if (game.coverPath.rfind(".webp") != std::string::npos) {
                    mime = "image/webp";
                }
                std::string header = "HTTP/1.1 200 OK\r\n"
                                     "Content-Type: " + mime + "\r\n"
                                     "Cache-Control: public, max-age=86400\r\n"
                                     "Access-Control-Allow-Origin: *\r\n"
                                     "Content-Length: " + std::to_string(content.size()) + "\r\n"
                                     "Connection: close\r\n\r\n";
                send(clientFd, header.c_str(), header.length(), 0);
                send(clientFd, content.data(), content.size(), 0);
                return;
            }
        }
        std::string notFound = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(clientFd, notFound.c_str(), notFound.length(), 0);
        return;
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
    } else if (method == "POST" && (path == "/api/scrape_cover" || path == "/api/scrape_game")) {
        std::string gameIdStr = extractPostParam(postBody, "game_id");
        int64_t gameId = 0;
        try { gameId = std::stoll(gameIdStr); } catch(...) {}

        bool ok = false;
        std::string coverPath;
        std::string releaseYear;
        std::string genre;
        std::string developer;
        std::string desc;
        std::string title;
        std::string msg = "Không thể cào thông tin game";

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
                        msg = "Đã cập nhật ảnh bìa & thông tin cho \"" + (title.empty() ? g.title : title) + "\"!";
                    } else {
                        msg = "Không tìm thấy dữ liệu trên ScreenScraper.fr / Libretro.";
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
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/auto_scrape_sd") {
        bool force = (extractPostParam(postBody, "force") == "1");
        bool started = BoxartScraper::instance().startAutoScrapeSdCard(force);
        std::string msg = started ? "Đã bắt đầu tự động cào ảnh bìa và thông tin cho ROM trên thẻ SD." :
                                    "Không có ROM nào trên thẻ cần cào hoặc tiến trình đang chạy.";
        std::string json = "{\"success\":" + std::string(started ? "true" : "false") + ",\"message\":\"" + escapeJson(msg) + "\"}";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "GET" && path == "/api/auto_scrape_status") {
        auto st = BoxartScraper::instance().getAutoScrapeStatus();
        std::string json = "{";
        json += "\"is_scraping\":" + std::string(st.isScraping ? "true" : "false") + ",";
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
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/cancel_auto_scrape") {
        BoxartScraper::instance().cancelAutoScrape();
        std::string json = "{\"success\":true,\"message\":\"Đã gửi yêu cầu hủy cào tự động.\"}";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "GET" && path == "/api/screenscraper_config") {
        std::string user = DatabaseManager::instance().getSetting("screenscraper_user", "");
        std::string devId = DatabaseManager::instance().getSetting("screenscraper_devid", "");
        bool hasPass = !DatabaseManager::instance().getSetting("screenscraper_pass", "").empty();
        std::string json = "{";
        json += "\"user\":\"" + escapeJson(user) + "\",";
        json += "\"dev_id\":\"" + escapeJson(devId) + "\",";
        json += "\"has_pass\":" + std::string(hasPass ? "true" : "false");
        json += "}";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/save_screenscraper_config") {
        std::string user = extractPostParam(postBody, "user");
        std::string pass = extractPostParam(postBody, "pass");
        std::string devId = extractPostParam(postBody, "dev_id");
        std::string devPass = extractPostParam(postBody, "dev_pass");

        DatabaseManager::instance().setSetting("screenscraper_user", user);
        if (!pass.empty()) {
            DatabaseManager::instance().setSetting("screenscraper_pass", pass);
        }
        if (!devId.empty()) {
            DatabaseManager::instance().setSetting("screenscraper_devid", devId);
        }
        if (!devPass.empty()) {
            DatabaseManager::instance().setSetting("screenscraper_devpass", devPass);
        }

        std::string json = "{\"success\":true,\"message\":\"Đã lưu cấu hình ScreenScraper.fr thành công!\"}";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/test_screenscraper") {
        std::string user = extractPostParam(postBody, "user");
        std::string pass = extractPostParam(postBody, "pass");
        std::string devId = extractPostParam(postBody, "dev_id");
        std::string devPass = extractPostParam(postBody, "dev_pass");

        if (pass.empty()) {
            pass = DatabaseManager::instance().getSetting("screenscraper_pass", "");
        }

        std::string errMsg;
        bool ok = BoxartScraper::instance().testScreenScraperAuth(user, pass, devId, devPass, errMsg);
        std::string msg = ok ? "Kết nối ScreenScraper.fr thành công!" : errMsg;

        std::string json = "{\"success\":" + std::string(ok ? "true" : "false") + ",\"message\":\"" + escapeJson(msg) + "\"}";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/search_scraper") {
        std::string query = extractPostParam(postBody, "query");
        std::string sysCode = extractPostParam(postBody, "system_code");
        auto cands = BoxartScraper::instance().searchCandidates(query, sysCode);

        std::string json = "{\"success\":true,\"candidates\":[";
        for (size_t i = 0; i < cands.size(); ++i) {
            if (i > 0) json += ",";
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
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/apply_candidate") {
        std::string gameIdStr = extractPostParam(postBody, "game_id");
        int64_t gameId = 0;
        try { gameId = std::stoll(gameIdStr); } catch(...) {}

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
                           ",\"message\":\"" + (ok ? "Đã áp dụng ảnh bìa và thông tin game thành công!" : "Lỗi khi áp dụng thông tin game.") + "\"}";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/audit_roms") {
        auto list = RomOrganizer::instance().auditMisplacedRoms();
        std::string json = "{\"success\":true,\"count\":" + std::to_string(list.size()) + ",\"items\":[";
        for (size_t i = 0; i < list.size(); ++i) {
            if (i > 0) json += ",";
            json += "{";
            json += "\"filename\":\"" + escapeJson(list[i].filename) + "\",";
            json += "\"original_path\":\"" + escapeJson(list[i].originalPath) + "\",";
            json += "\"current_system\":\"" + escapeJson(list[i].currentSystemCode) + "\",";
            json += "\"detected_system\":\"" + escapeJson(list[i].detectedSystemCode) + "\",";
            json += "\"detected_name\":\"" + escapeJson(list[i].detectedSystemName) + "\",";
            json += "\"target_path\":\"" + escapeJson(list[i].targetPath) + "\",";
            json += "\"confidence\":\"" + escapeJson(list[i].confidence) + "\",";
            json += "\"reason\":\"" + escapeJson(list[i].reason) + "\"";
            json += "}";
        }
        json += "]}";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/fix_misplaced_roms") {
        auto list = RomOrganizer::instance().fixMisplacedRoms();
        // Re-index ROMs so library updates immediately
        std::string romsDir = AppConfig::instance().getRomsDir();
        std::thread([romsDir]() {
            RomIndexer::instance().scanAllSystems(romsDir, nullptr);
        }).detach();
        std::string json = "{\"success\":true,\"count\":" + std::to_string(list.size()) + ",\"items\":[";
        for (size_t i = 0; i < list.size(); ++i) {
            if (i > 0) json += ",";
            json += "{";
            json += "\"filename\":\"" + escapeJson(list[i].filename) + "\",";
            json += "\"current_system\":\"" + escapeJson(list[i].currentSystemCode) + "\",";
            json += "\"detected_system\":\"" + escapeJson(list[i].detectedSystemCode) + "\",";
            json += "\"detected_name\":\"" + escapeJson(list[i].detectedSystemName) + "\",";
            json += "\"fixed\":" + std::string(list[i].fixed ? "true" : "false") + ",";
            json += "\"status\":\"" + escapeJson(list[i].statusMessage) + "\"";
            json += "}";
        }
        json += "]}";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/organize_inbox") {
        auto list = RomOrganizer::instance().organizeDirectory(RomOrganizer::instance().getInboxDir());
        std::string json = "{\"success\":true,\"count\":" + std::to_string(list.size()) + "}";
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: application/json; charset=UTF-8\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/upload_rom_auto") {
        std::string fname = extractQueryParam(queryString, "filename");
        if (fname.empty()) fname = "uploaded_rom.bin";

        std::string inboxPath = RomOrganizer::instance().getInboxDir() + "/" + fname;
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
                } catch (...) {}

                long bytesReadSoFar = static_cast<long>(postBody.size());
                char chunk[32768];
                while (bytesReadSoFar < totalCl) {
                    long toRead = std::min<long>(sizeof(chunk), totalCl - bytesReadSoFar);
                    int n = recv(clientFd, chunk, toRead, 0);
                    if (n <= 0) break;
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
                std::string targetDir = AppConfig::instance().getRomsDir() + "/" + targetSys.romDir;
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
                if (::stat(finalDst.c_str(), &st) == 0) g.sizeBytes = st.st_size;
                DatabaseManager::instance().upsertGame(g);

                message = "Đã nhận diện: " + sysName + " (" + sysCode + ")! Game đã được lưu vào /Roms/" + targetSys.romDir + "/";
            } else {
                message = "Đã lưu ROM vào Hộp tiếp nhận _INBOX.";
            }

            std::string json = "{\"success\":true,\"filename\":\"" + escapeJson(fname) +
                               "\",\"system_code\":\"" + escapeJson(sysCode) +
                               "\",\"system_name\":\"" + escapeJson(sysName) +
                               "\",\"message\":\"" + escapeJson(message) + "\"}";
            std::string res = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json; charset=UTF-8\r\n"
                              "Access-Control-Allow-Origin: *\r\n"
                              "Content-Length: " + std::to_string(json.length()) + "\r\n"
                              "Connection: close\r\n\r\n" + json;
            send(clientFd, res.c_str(), res.length(), 0);
        } else {
            std::string json = "{\"success\":false,\"error\":\"Không thể ghi file vào thẻ nhớ.\"}";
            std::string res = "HTTP/1.1 500 Internal Server Error\r\n"
                              "Content-Type: application/json; charset=UTF-8\r\n"
                              "Access-Control-Allow-Origin: *\r\n"
                              "Content-Length: " + std::to_string(json.length()) + "\r\n"
                              "Connection: close\r\n\r\n" + json;
            send(clientFd, res.c_str(), res.length(), 0);
        }
    } else if (method == "GET" && path == "/api/storage_info") {
        auto disk = FileSystemManager::instance().getDiskSpace(AppConfig::instance().getRomsDir());
        bool isLinked = AuthManager::instance().isLinked();
        bool canUpload = AuthManager::instance().canUpload();
        bool isPublicOnly = AuthManager::instance().isPublicOnly();
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
        json += "\"can_upload\":" + std::string(canUpload ? "true" : "false") + ",";
        json += "\"is_public_only\":" + std::string(isPublicOnly ? "true" : "false") + ",";
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
    } else if (method == "POST" && path == "/api/set_personal_auth") {
        std::string action = extractPostParam(postBody, "action");
        if (action == "clear") {
            AuthManager::instance().clearPersonalTokens();
            std::string json = "{\"success\":true,\"message\":\"Đã hủy kích hoạt sao lưu Drive cá nhân thành công.\"}";
            std::string res = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json; charset=UTF-8\r\n"
                              "Access-Control-Allow-Origin: *\r\n"
                              "Content-Length: " + std::to_string(json.length()) + "\r\n"
                              "Connection: close\r\n\r\n" + json;
            send(clientFd, res.c_str(), res.length(), 0);
            return;
        }

        std::string token = extractPostParam(postBody, "token");
        std::string refreshToken = extractPostParam(postBody, "refresh_token");
        std::string email = extractPostParam(postBody, "email");
        if (token.empty() && refreshToken.empty()) {
            std::string json = "{\"success\":false,\"error\":\"Vui lòng dán Access Token hoặc Refresh Token vào ô.\"}";
            std::string res = "HTTP/1.1 400 Bad Request\r\n"
                              "Content-Type: application/json; charset=UTF-8\r\n"
                              "Access-Control-Allow-Origin: *\r\n"
                              "Content-Length: " + std::to_string(json.length()) + "\r\n"
                              "Connection: close\r\n\r\n" + json;
            send(clientFd, res.c_str(), res.length(), 0);
            return;
        }
        auto result = AuthManager::instance().setPersonalTokens(token, refreshToken, email);
        if (result.success) {
            std::string json = "{\"success\":true,\"message\":\"" + escapeJson(result.message) + "\",\"email\":\"" + escapeJson(result.userEmail) + "\"}";
            std::string res = "HTTP/1.1 200 OK\r\n"
                              "Content-Type: application/json; charset=UTF-8\r\n"
                              "Access-Control-Allow-Origin: *\r\n"
                              "Content-Length: " + std::to_string(json.length()) + "\r\n"
                              "Connection: close\r\n\r\n" + json;
            send(clientFd, res.c_str(), res.length(), 0);
        } else {
            std::string json = "{\"success\":false,\"error\":\"" + escapeJson(result.message) + "\"}";
            std::string res = "HTTP/1.1 400 Bad Request\r\n"
                              "Content-Type: application/json; charset=UTF-8\r\n"
                              "Access-Control-Allow-Origin: *\r\n"
                              "Content-Length: " + std::to_string(json.length()) + "\r\n"
                              "Connection: close\r\n\r\n" + json;
            send(clientFd, res.c_str(), res.length(), 0);
        }
    } else if (method == "POST" && path == "/api/upload_game") {
        if (!AuthManager::instance().canUpload()) {
            std::string json = "{\"success\":false,\"error\":\"Chưa kích hoạt quyền sao lưu! Vui lòng vào tab 'Đồng bộ & Thẻ nhớ' để nạp Google Token cá nhân trước.\"}";
            std::string res = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json; charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
            send(clientFd, res.c_str(), res.length(), 0);
        } else {
            std::string gameIdStr = extractPostParam(postBody, "game_id");
            int64_t gameId = 0;
            try { gameId = std::stoll(gameIdStr); } catch (...) {}
            if (gameId > 0) {
                UploadManager::instance().startUploadGames({gameId});
                std::string json = "{\"success\":true,\"message\":\"Đang bắt đầu sao lưu game lên Google Drive cá nhân...\"}";
                std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
                send(clientFd, res.c_str(), res.length(), 0);
            } else {
                std::string json = "{\"success\":false,\"error\":\"ID game không hợp lệ.\"}";
                std::string res = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json; charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
                send(clientFd, res.c_str(), res.length(), 0);
            }
        }
    } else if (method == "POST" && path == "/api/upload_all") {
        if (!AuthManager::instance().canUpload()) {
            std::string json = "{\"success\":false,\"error\":\"Chưa kích hoạt quyền sao lưu! Vui lòng vào tab 'Đồng bộ & Thẻ nhớ' để nạp Google Token cá nhân trước.\"}";
            std::string res = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json; charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
            send(clientFd, res.c_str(), res.length(), 0);
        } else {
            UploadManager::instance().startReverseSync();
            std::string json = "{\"success\":true,\"message\":\"Đang bắt đầu sao lưu toàn bộ game trên thẻ nhớ lên Google Drive cá nhân...\"}";
            std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
            send(clientFd, res.c_str(), res.length(), 0);
        }
    } else if (method == "GET" && path == "/api/upload_status") {
        auto prog = UploadManager::instance().getProgress();
        bool isUp = UploadManager::instance().isUploading();
        std::string stateStr = "IDLE";
        if (prog.state == UploadState::PREPARING) stateStr = "PREPARING";
        else if (prog.state == UploadState::UPLOADING) stateStr = "UPLOADING";
        else if (prog.state == UploadState::COMPLETED) stateStr = "COMPLETED";
        else if (prog.state == UploadState::FAILED) stateStr = "FAILED";
        else if (prog.state == UploadState::CANCELLED) stateStr = "CANCELLED";

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

        std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/cancel_upload") {
        UploadManager::instance().cancel();
        std::string json = "{\"success\":true,\"message\":\"Đã yêu cầu hủy sao lưu.\"}";
        std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
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
    } else if (method == "GET" && path == "/api/iptv/list") {
        // List IPTV playlists
        std::vector<std::string> playlists;
        std::string iptvDir = IPTVManager::instance().getIptvDir();
        if (iptvDir.empty()) {
            iptvDir = AppConfig::instance().getAppRoot() + "/iptv";
        }
        FileSystemManager::instance().createDirectoryRecursive(iptvDir);

        DIR* dir = opendir(iptvDir.c_str());
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                std::string name = entry->d_name;
                if (name.length() > 4 && (name.substr(name.length()-4) == ".m3u" || name.substr(name.length()-5) == ".m3u8")) {
                    playlists.push_back(name);
                }
            }
            closedir(dir);
        }
        std::string json = "{";
        json += "\"playlists\":[";
        for (size_t i = 0; i < playlists.size(); i++) {
            if (i > 0) json += ",";
            json += "\"" + playlists[i] + "\"";
        }
        json += "]}";
        std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(json.length()) + "\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/iptv/upload") {
        // Read full POST body according to Content-Length
        size_t clPos = req.find("Content-Length: ");
        if (clPos == std::string::npos) clPos = req.find("content-length: ");
        long totalCl = 0;
        if (clPos != std::string::npos) {
            size_t clEnd = req.find("\r\n", clPos);
            try {
                totalCl = std::stol(req.substr(clPos + 16, clEnd - (clPos + 16)));
            } catch (...) {}
        }

        std::string fullBody = postBody;
        if (totalCl > 0 && static_cast<long>(fullBody.size()) < totalCl) {
            char chunk[16384];
            while (static_cast<long>(fullBody.size()) < totalCl) {
                long toRead = std::min<long>(sizeof(chunk), totalCl - static_cast<long>(fullBody.size()));
                int n = recv(clientFd, chunk, toRead, 0);
                if (n <= 0) break;
                fullBody.append(chunk, n);
            }
        }

        // Parse multipart filename: filename="..."
        std::string filename = "uploaded.m3u";
        size_t fnPos = fullBody.find("filename=\"");
        if (fnPos != std::string::npos) {
            fnPos += 10; // strlen("filename=\"") == 10
            size_t fnEnd = fullBody.find("\"", fnPos);
            if (fnEnd != std::string::npos && fnEnd > fnPos) {
                filename = fullBody.substr(fnPos, fnEnd - fnPos);
                size_t slash = filename.find_last_of("/\\");
                if (slash != std::string::npos) filename = filename.substr(slash + 1);
            }
        }
        if (filename.empty() || filename == "." || filename == "..") {
            filename = "playlist_" + std::to_string(std::time(nullptr)) + ".m3u";
        }
        if (filename.find(".m3u") == std::string::npos && filename.find(".m3u8") == std::string::npos) {
            filename += ".m3u";
        }

        // Extract content after multipart headers (double CRLF)
        std::string fileContent;
        size_t dataStart = fullBody.find("\r\n\r\n");
        if (dataStart != std::string::npos && fullBody.find("Content-Disposition") != std::string::npos) {
            dataStart += 4;
            // End boundary
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
                Logger::info("IPTV: Playlist uploaded successfully to " + outPath + " (" + std::to_string(fileContent.size()) + " bytes)");
                IPTVManager::instance().loadPlaylists(iptvDir);
                size_t count = IPTVManager::instance().getChannels().size();
                std::string json = "{\"success\":true,\"file\":\"" + escapeJson(filename) + "\",\"channels\":" + std::to_string(count) + "}";
                std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
                send(clientFd, res.c_str(), res.length(), 0);
            } else {
                Logger::error("IPTV: Cannot write uploaded file to " + outPath);
                std::string json = "{\"success\":false,\"error\":\"Cannot write file: " + escapeJson(filename) + "\"}";
                std::string res = "HTTP/1.1 500 OK\r\nContent-Type: application/json; charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
                send(clientFd, res.c_str(), res.length(), 0);
            }
        } else {
            std::string json = "{\"success\":false,\"error\":\"No content\"}";
            std::string res = "HTTP/1.1 400 OK\r\nContent-Type: application/json; charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
            send(clientFd, res.c_str(), res.length(), 0);
        }
    } else if (method == "GET" && path.find("/api/iptv/add") == 0) {
        // Download and add playlist from URL using HttpClient (curl C-API)
        std::string url;
        size_t qPos = path.find("url=");
        if (qPos != std::string::npos) {
            url = path.substr(qPos + 4);
            size_t ampPos = url.find('&');
            if (ampPos != std::string::npos) url = url.substr(0, ampPos);
            url = urlDecode(url);
        }

        if (!url.empty()) {
            std::string iptvDir = IPTVManager::instance().getIptvDir();
            if (iptvDir.empty()) {
                iptvDir = AppConfig::instance().getAppRoot() + "/iptv";
            }
            FileSystemManager::instance().createDirectoryRecursive(iptvDir);

            std::string safeName = "playlist_url.m3u";
            size_t lastSlash = url.find_last_of("/\\");
            if (lastSlash != std::string::npos && lastSlash + 1 < url.size()) {
                std::string cand = url.substr(lastSlash + 1);
                size_t q = cand.find('?');
                if (q != std::string::npos) cand = cand.substr(0, q);
                if (cand.size() > 4 && (cand.rfind(".m3u") != std::string::npos || cand.rfind(".m3u8") != std::string::npos)) {
                    safeName = cand;
                }
            }

            std::string destPath = iptvDir + "/" + safeName;
            Logger::info("IPTV: Fetching playlist from URL: " + url + " -> " + destPath);

            HttpResponse resp = HttpClient::instance().get(url, {
                "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
                "Accept: */*"
            }, 30);

            if (resp.success && resp.statusCode >= 200 && resp.statusCode < 400 && !resp.body.empty()) {
                std::ofstream out(destPath, std::ios::binary);
                if (out.is_open()) {
                    out.write(resp.body.data(), resp.body.size());
                    out.close();
                    sync();
                    IPTVManager::instance().loadPlaylists(iptvDir);
                    size_t count = IPTVManager::instance().getChannels().size();
                    std::string json = "{\"success\":true,\"file\":\"" + escapeJson(safeName) + "\",\"channels\":" + std::to_string(count) + "}";
                    std::string res = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
                    send(clientFd, res.c_str(), res.length(), 0);
                } else {
                    std::string json = "{\"success\":false,\"error\":\"Cannot write file: " + escapeJson(destPath) + "\"}";
                    std::string res = "HTTP/1.1 500 OK\r\nContent-Type: application/json; charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
                    send(clientFd, res.c_str(), res.length(), 0);
                }
            } else {
                std::string err = resp.error.empty() ? ("HTTP " + std::to_string(resp.statusCode)) : resp.error;
                std::string json = "{\"success\":false,\"error\":\"Download failed: " + escapeJson(err) + "\"}";
                std::string res = "HTTP/1.1 500 OK\r\nContent-Type: application/json; charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
                send(clientFd, res.c_str(), res.length(), 0);
            }
        } else {
            std::string json = "{\"success\":false,\"error\":\"No URL provided\"}";
            std::string res = "HTTP/1.1 400 OK\r\nContent-Type: application/json; charset=UTF-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(json.length()) + "\r\nConnection: close\r\n\r\n" + json;
            send(clientFd, res.c_str(), res.length(), 0);
        }
    } else {
        std::string notFound = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(clientFd, notFound.c_str(), notFound.length(), 0);
    }
}

} // namespace RomCloud
