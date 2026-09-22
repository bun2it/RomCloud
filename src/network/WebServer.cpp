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
#include "HttpClient.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <sstream>
#include <cstring>
#include <vector>
#include <algorithm>

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
    // Direct folder ID fallback if no slashes
    if (url.find('/') == std::string::npos && url.length() >= 20) {
        return url;
    }
    return "";
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
        Logger::error("WebServer: Failed to bind to port " + std::to_string(m_port));
        close(m_serverFd);
        m_serverFd = -1;
        return false;
    }

    if (listen(m_serverFd, 5) < 0) {
        Logger::error("WebServer: Failed to listen on socket.");
        close(m_serverFd);
        m_serverFd = -1;
        return false;
    }

    m_running = true;
    m_thread = std::thread(&WebServer::serverLoop, this);
    Logger::info("WebServer: Started on port " + std::to_string(m_port));
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
    Logger::info("WebServer: Stopped cleanly.");
}

void WebServer::serverLoop() {
    while (m_running) {
        fd_set readFds;
        FD_ZERO(&readFds);
        FD_SET(m_serverFd, &readFds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000; // 200ms timeout so thread terminates promptly on exit

        int ret = select(m_serverFd + 1, &readFds, nullptr, nullptr, &tv);
        if (ret > 0 && FD_ISSET(m_serverFd, &readFds)) {
            struct sockaddr_in clientAddr;
            socklen_t clientLen = sizeof(clientAddr);
            int clientFd = accept(m_serverFd, (struct sockaddr*)&clientAddr, &clientLen);
            if (clientFd >= 0) {
                // Set receive/send timeouts to 1 second to prevent hanging on idle connections
                struct timeval timeout;
                timeout.tv_sec = 1;
                timeout.tv_usec = 0;
                setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
                setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

                handleClient(clientFd);
                shutdown(clientFd, SHUT_RDWR);
                close(clientFd);
            }
        }
    }
}

static std::string extractPostParam(const std::string& postBody, const std::string& paramName) {
    std::string key = paramName + "=";
    size_t keyPos = postBody.find(key);
    if (keyPos == std::string::npos) return "";
    std::string rawVal = postBody.substr(keyPos + key.length());
    size_t ampersand = rawVal.find('&');
    if (ampersand != std::string::npos) rawVal = rawVal.substr(0, ampersand);
    return urlDecode(rawVal);
}

static std::string extractQueryParam(const std::string& queryStr, const std::string& paramName) {
    std::string key = paramName + "=";
    size_t keyPos = queryStr.find(key);
    if (keyPos == std::string::npos) return "";
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

std::string WebServer::buildHtmlResponse() {
    std::string ip = PlatformInfo::instance().getIpAddress("wlan0");
    if (ip.empty()) ip = "192.168.1.164";

    auto& db = DatabaseManager::instance();
    std::string savedClientId = db.getSetting("auth_client_id", "");
    if (savedClientId.empty()) savedClientId = "966407933571-pc0c8c9gcfh4eiofcresgj6je79524s3.apps.googleusercontent.com";
    std::string savedClientSecret = db.getSetting("auth_client_secret", "");
    if (savedClientSecret.empty()) savedClientSecret = AuthManager::getDefaultClientSecret();
    std::string savedDriveUrl = db.getSetting("drive_folder_url", "https://drive.google.com/drive/folders/1j4Bfo5YS65zSGSOWHWRrXjovTfX6syWD");
    std::string savedApiKey = db.getSetting("google_api_key", "");

    bool isLinked = AuthManager::instance().isLinked();
    std::string currentEmail = isLinked ? AuthManager::instance().getUserEmail() : "Chưa đăng nhập";

    int totalLocal = 0, totalCloud = 0;
    db.getTotalGameCounts(totalLocal, totalCloud);

    std::string html = R"HTML(<!DOCTYPE html>
<html lang="vi">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>RomCloud TrimUI Portal</title>
  <style>
    :root {
      --bg: #0f172a;
      --card-bg: #1e293b;
      --primary: #0284c7;
      --primary-hover: #0369a1;
      --text: #f8fafc;
      --text-muted: #94a3b8;
      --border: #334155;
      --green: #22c55e;
      --yellow: #f59e0b;
    }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      background: var(--bg);
      color: var(--text);
      padding: 16px;
      margin: 0;
      line-height: 1.5;
    }
    .container { max-width: 560px; margin: 0 auto; }
    .header { text-align: center; margin-bottom: 20px; }
    .logo { font-size: 26px; font-weight: 800; color: #38bdf8; letter-spacing: -0.5px; }
    .status-card {
      background: #111827;
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 14px 18px;
      margin-bottom: 18px;
      font-size: 13px;
      display: flex;
      justify-content: space-between;
      align-items: center;
    }
    .status-pill {
      display: inline-block;
      padding: 5px 12px;
      border-radius: 20px;
      font-weight: 600;
      font-size: 12px;
    }
    .pill-green { background: #14532d; color: #4ade80; border: 1px solid #22c55e; }
    .pill-gray { background: #374151; color: #d1d5db; }
    .card {
      background: var(--card-bg);
      border-radius: 16px;
      padding: 24px;
      margin-bottom: 20px;
      box-shadow: 0 10px 25px -5px rgba(0,0,0,0.4);
      border: 1px solid var(--border);
    }
    h2 { font-size: 18px; color: #f1f5f9; margin-top: 0; margin-bottom: 10px; }
    p { font-size: 14px; color: var(--text-muted); margin-top: 0; margin-bottom: 14px; }
    label { font-size: 13px; color: var(--text-muted); display: block; margin-bottom: 6px; font-weight: 500; }
    input[type=text] {
      width: 100%;
      box-sizing: border-box;
      padding: 13px 14px;
      border-radius: 10px;
      border: 1px solid #475569;
      background: #0f172a;
      color: #fff;
      font-size: 14px;
      margin-bottom: 16px;
      outline: none;
    }
    input[type=text]:focus { border-color: #38bdf8; }
    button.btn-main {
      width: 100%;
      padding: 15px;
      border: none;
      border-radius: 10px;
      font-weight: 700;
      font-size: 15px;
      cursor: pointer;
      background: #0284c7;
      color: white;
      transition: background 0.2s;
    }
    button.btn-main:active { background: #0369a1; }
    .guide-box {
      background: #0f172a;
      border-left: 4px solid #38bdf8;
      border-radius: 8px;
      padding: 14px 16px;
      font-size: 13px;
      color: #cbd5e1;
      margin-top: 18px;
      line-height: 1.6;
    }
    .guide-box b { color: #38bdf8; }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <div class="logo">RomCloud</div>
      <div style="font-size: 13px; color: #94a3b8; margin-top: 4px;">Cổng kết nối Google Drive & Quản lý ROM &bull; <b>)HTML" + ip + R"HTML(:8080</b></div>
    </div>

    <!-- STATUS CARD -->
    <div class="status-card">
      <div>
        <div style="font-weight: 600;">Trạng thái kết nối Drive:</div>
        <div style="color: #94a3b8; font-size: 12px; margin-top: 2px;">)HTML" + (isLinked ? currentEmail : "Chưa kết nối thư mục") + R"HTML(</div>
      </div>
      <div style="display: flex; gap: 8px; align-items: center;">
        )HTML" + (isLinked ? R"HTML(<span class="status-pill pill-green">ĐÃ KẾT NỐI</span>
        <form method="POST" action="/unlink" style="margin: 0;" onsubmit="return confirm('Bạn có chắc muốn hủy liên kết Google Drive?');">
          <button type="submit" style="background: #ef4444; color: #fff; border: none; border-radius: 6px; padding: 6px 12px; font-size: 12px; font-weight: 600; cursor: pointer;">HỦY KẾT NỐI</button>
        </form>)HTML" : "<span class=\"status-pill pill-gray\">CHƯA KẾT NỐI</span>") + R"HTML(
      </div>
    </div>

    <!-- MAIN CARD: PUBLIC GOOGLE DRIVE LINK -->
    <div class="card">
      <h2>Liên kết thư mục Google Drive chia sẻ</h2>
      <p>Dành cho thư mục Google Drive được chia sẻ ở chế độ công khai (Bất kỳ ai có liên kết đều xem được).</p>

      <div style="background: rgba(245, 158, 11, 0.12); border: 1px solid #f59e0b; border-radius: 8px; padding: 14px 18px; margin: 16px 0; font-size: 13px; line-height: 1.6; color: #fbbf24;">
        <b>⚠️ TUYÊN BỐ MIỄN TRỪ TRÁCH NHIỆM BẢN QUYỀN (DISCLAIMER):</b><br>
        • RomCloud là phần mềm mã nguồn mở độc lập, KHÔNG chứa sẵn hoặc phân phối bất kỳ file ROM hay dữ liệu có bản quyền nào.<br>
        • Người dùng hoàn toàn tự chịu trách nhiệm về nội dung và quyền sử dụng các tệp tin trong Google Drive của mình.
      </div>

      <form method="POST" action="/connect">
        <label>Liên kết thư mục Google Drive:</label>
        <input type="text" name="drive_url" value=")HTML" + savedDriveUrl + R"HTML(" placeholder="https://drive.google.com/drive/folders/..." required>

        <!-- API Key hidden - pre-configured by admin -->
        <input type="hidden" name="api_key" value=")HTML" + savedApiKey + R"HTML(">

        <button type="submit" class="btn-main">👉 KẾT NỐI &amp; ĐỒNG BỘ VÀO MÁY TRIMUI</button>
      </form>

      <div class="guide-box">
        <b>💡 Hướng dẫn kết nối nhanh:</b><br>
        1. Mở Google Drive trên điện thoại hoặc máy tính.<br>
        2. Chọn thư mục chứa ROM &rarr; Bấm <b>Chia sẻ</b> &rarr; Đặt quyền truy cập là <i>"Bất kỳ ai có đường liên kết đều có thể xem"</i>.<br>
        3. Dán liên kết vào ô bên trên và bấm <b>KẾT NỐI &amp; ĐỒNG BỘ</b>. Máy TrimUI sẽ tự động quét danh sách game!
      </div>
    </div>

    <!-- SEARCH & ROM MANAGEMENT CARD -->
    <div class="card" style="border: 1px solid #38bdf8;">
      <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 10px;">
        <h2 style="margin: 0; font-size: 17px; color: #38bdf8;">🔍 Tìm kiếm & Tải ROM Drive</h2>
        <span class="status-pill pill-gray">)HTML" + std::to_string(totalCloud) + R"HTML( Cloud &bull; )HTML" + std::to_string(totalLocal) + R"HTML( Trên thẻ</span>
      </div>
      <p style="font-size: 13px; color: #94a3b8; margin-bottom: 12px;">
        Tìm kiếm tên game trên Google Drive. Bấm <b>Tải về</b> để nạp vào thẻ nhớ TrimUI, hoặc bấm <b>Xóa</b> để giải phóng bộ nhớ.
      </p>

      <input type="text" id="web-search-input" placeholder="🔍 Nhập tên game (VD: Mario, Pokemon, Contra, Sonic, Yu-Gi-Oh...)" oninput="onSearchInput(this.value)" style="margin-bottom: 8px;">

      <div id="search-msg" style="display: none; font-size: 12px; padding: 8px 12px; border-radius: 6px; margin-bottom: 10px;"></div>

      <div id="search-results-box" style="max-height: 400px; overflow-y: auto; display: flex; flex-direction: column; gap: 8px;">
        <div style="text-align: center; color: #64748b; font-size: 13px; padding: 24px 0;">
          💡 Gõ từ khóa vào ô trên để tìm game trong kho...
        </div>
      </div>
    </div>

    <!-- OTA UPDATE CARD -->
    <div class="card" style="border: 1px solid #0284c7;">
      <div style="display: flex; justify-content: space-between; align-items: center;">
        <h2 style="margin: 0; font-size: 16px; color: #38bdf8;">🔄 Cập nhật phần mềm (OTA Update)</h2>
        <span id="ota-badge" class="status-pill pill-gray">v)HTML" + UpdateManager::instance().getCurrentVersion() + R"HTML(</span>
      </div>
      <p style="font-size: 13px; color: #94a3b8; margin: 8px 0 12px 0;">
        Kiểm tra và cập nhật phiên bản RomCloud mới nhất từ GitHub trực tiếp vào máy TrimUI.
      </p>

      <div id="ota-info-box" style="display: none; background: #0f172a; border-radius: 8px; padding: 12px; margin-bottom: 12px; font-size: 13px;">
        <div id="ota-ver-title" style="font-weight: 700; color: #22c55e;"></div>
        <div id="ota-changelog" style="color: #cbd5e1; margin-top: 4px;"></div>
      </div>

      <div id="ota-progress-box" style="display: none; margin-bottom: 12px;">
        <div style="background: #334155; border-radius: 6px; height: 12px; overflow: hidden;">
          <div id="ota-bar" style="background: #22c55e; width: 0%; height: 100%; transition: width 0.3s;"></div>
        </div>
        <div id="ota-prog-text" style="font-size: 12px; color: #94a3b8; margin-top: 4px; text-align: center;"></div>
      </div>

      <div style="display: flex; gap: 10px;">
        <button type="button" id="btn-ota-check" onclick="checkOtaUpdate()" class="btn-main" style="flex: 1; padding: 11px; font-size: 14px; background: #334155;">🔍 Kiểm tra bản mới</button>
        <button type="button" id="btn-ota-install" onclick="startOtaInstall()" class="btn-main" style="display: none; flex: 1; padding: 11px; font-size: 14px; background: #16a34a;">⬇️ Cập nhật ngay vào máy</button>
      </div>
    </div>
  </div>

  <script>
    let searchDebounceTimer = null;

    function onSearchInput(val) {
      clearTimeout(searchDebounceTimer);
      const q = val.trim();
      const box = document.getElementById('search-results-box');
      if (q.length < 2) {
        box.innerHTML = '<div style="text-align: center; color: #64748b; font-size: 13px; padding: 20px 0;">💡 Gõ ít nhất 2 ký tự để tìm kiếm trong kho game...</div>';
        return;
      }
      searchDebounceTimer = setTimeout(async () => {
        box.innerHTML = '<div style="text-align: center; color: #38bdf8; font-size: 13px; padding: 15px 0;">⏳ Đang tìm kiếm...</div>';
        try {
          const res = await fetch('/api/search?q=' + encodeURIComponent(q));
          const games = await res.json();
          if (!games || games.length === 0) {
            box.innerHTML = '<div style="text-align: center; color: #94a3b8; font-size: 13px; padding: 20px 0;">Không tìm thấy game nào khớp với "' + q + '".</div>';
            return;
          }
          let html = '';
          games.forEach(g => {
            const isLocal = g.local_state === 1;
            const safeTitle = g.title.replace(/'/g, "\\'");
            html += `
              <div style="background: #0f172a; border: 1px solid #334155; border-radius: 8px; padding: 10px 12px; display: flex; align-items: center; justify-content: space-between; gap: 8px;">
                <div style="overflow: hidden; flex: 1;">
                  <div style="display: flex; align-items: center; gap: 6px; margin-bottom: 2px;">
                    <span style="background: #1e293b; color: #38bdf8; font-size: 11px; font-weight: 700; padding: 2px 6px; border-radius: 4px; border: 1px solid #475569;">${g.sys_code}</span>
                    <span style="font-weight: 600; font-size: 13px; color: #f8fafc; white-space: nowrap; overflow: hidden; text-overflow: ellipsis;">${g.title}</span>
                  </div>
                  <div style="font-size: 11px; color: #64748b;">${g.filename} &bull; ${g.size_str}</div>
                </div>
                <div style="display: flex; gap: 6px; align-items: center; flex-shrink: 0;">
                  ${isLocal ? `
                    <span style="font-size: 11px; color: #4ade80; font-weight: 600; padding: 4px 8px; background: #14532d; border-radius: 6px;">✓ ĐÃ CÓ</span>
                    <button onclick="deleteRom(${g.id}, '${safeTitle}')" style="background: #ef4444; color: #fff; border: none; border-radius: 6px; padding: 6px 10px; font-size: 11px; font-weight: 600; cursor: pointer;">🗑️ Xóa</button>
                  ` : `
                    <button onclick="downloadRom(${g.id}, '${safeTitle}')" style="background: #0284c7; color: #fff; border: none; border-radius: 6px; padding: 6px 12px; font-size: 11px; font-weight: 600; cursor: pointer;">⬇️ Tải về</button>
                  `}
                  <button onclick="scrapeBoxart(${g.id}, '${safeTitle}')" title="Cào ảnh bìa từ Libretro" style="background: #334155; color: #38bdf8; border: none; border-radius: 6px; padding: 6px 8px; font-size: 11px; cursor: pointer;">🎨</button>
                </div>
              </div>
            `;
          });
          box.innerHTML = html;
        } catch(err) {
          box.innerHTML = '<div style="color: #ef4444; font-size: 12px; text-align: center;">Lỗi khi tìm kiếm. Vui lòng thử lại.</div>';
        }
      }, 250);
    }

    function showSearchToast(msg, isSuccess) {
      const m = document.getElementById('search-msg');
      m.style.display = 'block';
      m.style.background = isSuccess ? '#14532d' : '#7f1d1d';
      m.style.color = isSuccess ? '#4ade80' : '#fca5a5';
      m.style.border = isSuccess ? '1px solid #22c55e' : '1px solid #ef4444';
      m.innerText = msg;
      setTimeout(() => { m.style.display = 'none'; }, 4000);
    }

    async function downloadRom(id, title) {
      try {
        const res = await fetch('/api/download_game', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'game_id=' + id
        });
        const data = await res.json();
        showSearchToast(data.message || ('Đã thêm "' + title + '" vào hàng tải.'), data.success);
        const q = document.getElementById('web-search-input').value;
        if (q.length >= 2) onSearchInput(q);
      } catch(e) {
        showSearchToast('Lỗi khi thêm vào hàng tải', false);
      }
    }

    async function deleteRom(id, title) {
      if (!confirm('Bạn có chắc muốn xóa ROM "' + title + '" khỏi thẻ nhớ không?')) return;
      try {
        const res = await fetch('/api/delete_rom', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'game_id=' + id
        });
        const data = await res.json();
        showSearchToast(data.message || ('Đã xóa "' + title + '" khỏi thẻ.'), data.success);
        const q = document.getElementById('web-search-input').value;
        if (q.length >= 2) onSearchInput(q);
      } catch(e) {
        showSearchToast('Lỗi khi xóa ROM', false);
      }
    }

    async function scrapeBoxart(id, title) {
      showSearchToast('⏳ Đang cào ảnh bìa cho "' + title + '" từ Libretro...', true);
      try {
        const res = await fetch('/api/scrape_cover', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'game_id=' + id
        });
        const data = await res.json();
        showSearchToast(data.message, data.success);
      } catch(e) {
        showSearchToast('Lỗi khi cào ảnh bìa', false);
      }
    }

    let otaTimer = null;

    async function checkOtaUpdate() {
      const btnCheck = document.getElementById('btn-ota-check');
      btnCheck.disabled = true;
      btnCheck.innerText = 'Đang kiểm tra...';
      try {
        const resp = await fetch('/ota_check');
        const data = await resp.json();
        const infoBox = document.getElementById('ota-info-box');
        const titleEl = document.getElementById('ota-ver-title');
        const logEl = document.getElementById('ota-changelog');
        const btnInstall = document.getElementById('btn-ota-install');

        infoBox.style.display = 'block';
        if (data.has_update) {
          titleEl.innerText = '🎉 Có bản cập nhật mới: v' + data.remote_version + (data.release_date ? (' (' + data.release_date + ')') : '');
          titleEl.style.color = '#22c55e';
          logEl.innerText = data.changelog || 'Bản vá và nâng cấp hiệu năng.';
          btnInstall.style.display = 'inline-block';
          document.getElementById('ota-badge').className = 'status-pill pill-green';
          document.getElementById('ota-badge').innerText = 'CÓ BẢN MỚI v' + data.remote_version;
        } else {
          titleEl.innerText = '✅ Máy đang ở phiên bản mới nhất (v' + data.current_version + ')';
          titleEl.style.color = '#38bdf8';
          logEl.innerText = 'Không có bản cập nhật nào mới hơn trên GitHub repository.';
          btnInstall.style.display = 'none';
        }
      } catch (e) {
        alert('Lỗi kiểm tra OTA: ' + e);
      } finally {
        btnCheck.disabled = false;
        btnCheck.innerText = '🔍 Kiểm tra bản mới';
      }
    }

    async function startOtaInstall() {
      if (!confirm('Bạn có chắc muốn tải về và cài đặt bản cập nhật ngay bây giờ?')) return;
      document.getElementById('btn-ota-install').disabled = true;
      document.getElementById('btn-ota-check').disabled = true;
      document.getElementById('ota-progress-box').style.display = 'block';

      try {
        await fetch('/ota_start', { method: 'POST' });
        if (otaTimer) clearInterval(otaTimer);
        otaTimer = setInterval(pollOtaStatus, 1000);
      } catch (e) {
        alert('Không thể bắt đầu cập nhật: ' + e);
      }
    }

    async function pollOtaStatus() {
      try {
        const resp = await fetch('/ota_status');
        const data = await resp.json();
        const bar = document.getElementById('ota-bar');
        const text = document.getElementById('ota-prog-text');

        const pct = (data.progress_pct || 0);
        bar.style.width = pct + '%';
        text.innerText = 'Đang tải bản cập nhật: ' + pct.toFixed(1) + '%';

        if (data.state === 'COMPLETED') {
          clearInterval(otaTimer);
          bar.style.width = '100%';
          text.innerHTML = '<span style="color:#22c55e; font-weight:700;">🎉 ĐÃ CẬP NHẬT THÀNH CÔNG! Đang khởi động lại ứng dụng...</span>';
          await fetch('/ota_restart', { method: 'POST' });
          setTimeout(() => { location.reload(); }, 5000);
        } else if (data.state === 'FAILED') {
          clearInterval(otaTimer);
          text.innerHTML = '<span style="color:#ef4444;">❌ Thất bại: ' + (data.error || 'Lỗi không xác định') + '</span>';
          document.getElementById('btn-ota-install').disabled = false;
          document.getElementById('btn-ota-check').disabled = false;
        }
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
    body { font-family: -apple-system, sans-serif; background: #0f172a; color: #f8fafc; padding: 40px 20px; text-align: center; }
    .card { background: #1e293b; border-radius: 16px; padding: 30px 20px; max-width: 440px; margin: 40px auto; border: 1px solid #16a34a; box-shadow: 0 10px 25px rgba(0,0,0,0.5); }
    .icon { font-size: 54px; margin-bottom: 15px; color: #22c55e; }
    h1 { font-size: 22px; color: #22c55e; margin: 0 0 15px 0; }
    p { font-size: 15px; color: #cbd5e1; line-height: 1.6; margin-bottom: 25px; }
    a { display: inline-block; padding: 12px 24px; background: #334155; color: #38bdf8; text-decoration: none; border-radius: 8px; font-weight: 600; font-size: 14px; }
  </style>
</head>
<body>
  <div class="card">
    <div class="icon">&#10004;</div>
    <h1>ĐÃ KẾT NỐI THÀNH CÔNG!</h1>
    <p>)" + message + R"(<br><br><b>Hãy nhìn vào màn hình máy TrimUI</b>, quá trình đồng bộ kho game đang diễn ra tự động!</p>
    <a href="/">&larr; Quay lại trang kết nối</a>
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

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        std::string body = buildHtmlResponse();
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/html; charset=UTF-8\r\n"
                          "Content-Length: " + std::to_string(body.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + body;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "GET" && path == "/api/search") {
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
                          "Access-Control-Allow-Origin: *\r\n"
                          "Content-Length: " + std::to_string(json.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + json;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/api/download_game") {
        size_t bodyPos = req.find("\r\n\r\n");
        std::string postBody = (bodyPos != std::string::npos) ? req.substr(bodyPos + 4) : "";
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
    } else if (method == "POST" && path == "/api/delete_rom") {
        size_t bodyPos = req.find("\r\n\r\n");
        std::string postBody = (bodyPos != std::string::npos) ? req.substr(bodyPos + 4) : "";
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
        size_t bodyPos = req.find("\r\n\r\n");
        std::string postBody = (bodyPos != std::string::npos) ? req.substr(bodyPos + 4) : "";
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
    } else if (method == "POST" && path == "/unlink") {
        AuthManager::instance().logout();
        std::string body = buildSuccessResponse("Đã hủy liên kết Google Drive thành công!");
        std::string res = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/html; charset=UTF-8\r\n"
                          "Content-Length: " + std::to_string(body.length()) + "\r\n"
                          "Connection: close\r\n\r\n" + body;
        send(clientFd, res.c_str(), res.length(), 0);
    } else if (method == "POST" && path == "/connect") {
        size_t bodyPos = req.find("\r\n\r\n");
        std::string postBody = (bodyPos != std::string::npos) ? req.substr(bodyPos + 4) : "";

        std::string driveUrl = extractPostParam(postBody, "drive_url");
        std::string apiKey = extractPostParam(postBody, "api_key");

        if (!apiKey.empty()) {
            DatabaseManager::instance().setSetting("google_api_key", apiKey);
            Logger::info("Saved google_api_key into settings.");
        }

        std::string folderId = extractFolderId(driveUrl);
        if (folderId.empty()) folderId = driveUrl;

        Logger::info("User linked Google Drive folder via Web Portal: " + driveUrl + " (Extracted Folder ID: " + folderId + ")");

        // Save URL / folder to database and link public account
        AuthManager::instance().linkPublicFolder(folderId, driveUrl);

        // Immediately start public Google Drive library sync
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
                           "\"changelog\":\"" + info.changelog + "\"}";
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
                           "\"error\":\"" + prog.errorMessage + "\"}";
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
