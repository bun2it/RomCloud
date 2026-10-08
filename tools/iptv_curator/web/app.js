// ==========================================================================
// RomCloud IPTV Curator - Frontend Reactive Controller with Built-in HLS Player
// ==========================================================================

let allChannels = [];
let filteredChannels = [];
let currentPage = 1;
let pageSize = 100;
let probePollingTimer = null;

// Video Player State
let hlsInstance = null;
let currentPlayingChannel = "";
let currentPlayingOriginalUrl = "";
let isProxyActive = false;

// Ping Latency Thresholds Settings
let pingThresholds = {
  good_ms: 500,
  fair_ms: 1500
};

function getStreamQualityTier(stream) {
  if (!stream) return "UNKNOWN";
  if (stream.last_status === "DEAD") return "DEAD";
  if (stream.last_status === "STANDBY" || stream.last_status === "UNKNOWN" || !stream.latency_ms || stream.latency_ms === 0) {
    return "STANDBY";
  }
  if (stream.latency_ms <= pingThresholds.good_ms) return "GOOD";
  if (stream.latency_ms <= pingThresholds.fair_ms) return "FAIR";
  return "POOR";
}

function applyPingThresholdLabels() {
  document.querySelectorAll(".val-good-ms").forEach(el => el.textContent = pingThresholds.good_ms);
  document.querySelectorAll(".val-fair-ms").forEach(el => el.textContent = pingThresholds.fair_ms);
  const lblPoor = document.getElementById("lblPoorMinMs");
  if (lblPoor) lblPoor.textContent = `> ${pingThresholds.fair_ms}`;
  const inpG = document.getElementById("settingGoodMs");
  const inpF = document.getElementById("settingFairMs");
  if (inpG) inpG.value = pingThresholds.good_ms;
  if (inpF) inpF.value = pingThresholds.fair_ms;
}

function onPingSettingsInputChange() {
  const inpG = document.getElementById("settingGoodMs");
  const inpF = document.getElementById("settingFairMs");
  if (inpG && inpF) {
    let g = parseInt(inpG.value, 10) || 500;
    let f = parseInt(inpF.value, 10) || 1500;
    if (g >= f) f = g + 100;
    pingThresholds.good_ms = g;
    pingThresholds.fair_ms = f;
    applyPingThresholdLabels();
    onFilterChange();
    updatePublishPreview();
  }
}

async function savePingSettings() {
  onPingSettingsInputChange();
  try {
    const res = await fetch("/api/rules", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ ping_thresholds: pingThresholds })
    });
    const data = await res.json();
    if (data.success) {
      showToast(`Đã lưu ngưỡng Ping: Tốt ≤ ${pingThresholds.good_ms}ms, TB ≤ ${pingThresholds.fair_ms}ms`);
      loadChannels();
    } else {
      showToast("Lỗi khi lưu ngưỡng ping", true);
    }
  } catch (e) {
    showToast(`Lỗi: ${e}`, true);
  }
}

document.addEventListener("DOMContentLoaded", () => {
  initDropZone();
  loadStats();
  loadChannels();
  loadRules();
});

// Toast notification
function showToast(message, isError = false) {
  const toast = document.getElementById("toastBox");
  if (!toast) return;
  toast.textContent = message;
  toast.style.borderColor = isError ? "var(--accent-red)" : "var(--accent-cyan)";
  toast.classList.add("show");
  setTimeout(() => toast.classList.remove("show"), 3500);
}

// Copy to clipboard
function copyToClipboard(text) {
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(text).then(() => {
      showToast("Đã sao chép link stream!");
    }).catch(() => fallbackCopy(text));
  } else {
    fallbackCopy(text);
  }
}

function fallbackCopy(text) {
  const ta = document.createElement("textarea");
  ta.value = text;
  document.body.appendChild(ta);
  ta.select();
  document.execCommand("copy");
  document.body.removeChild(ta);
  showToast("Đã sao chép link stream!");
}

// HTML escape helper
function escapeHtml(str) {
  if (!str) return "";
  return str.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&#039;");
}

// Tab navigation
function switchTab(tabId) {
  document.querySelectorAll(".tab-btn").forEach(btn => {
    btn.classList.toggle("active", btn.dataset.tab === tabId);
  });
  document.querySelectorAll(".tab-pane").forEach(pane => {
    pane.classList.toggle("active", pane.id === tabId);
  });

  if (tabId === "tabChannels") {
    loadChannels();
  } else if (tabId === "tabGrid") {
    initGridIfEmpty();
  } else if (tabId === "tabProbe") {
    loadStats();
  } else if (tabId === "tabRules") {
    loadRules();
  }
}

// 1. API: Load Stats
async function loadStats() {
  try {
    const res = await fetch("/api/stats");
    const data = await res.json();

    const distinct = data.cluster ? data.cluster.distinct_channels : 0;
    document.getElementById("statChannelsText").textContent = `${distinct} kênh chuẩn`;
    document.getElementById("tabCountBadge").textContent = distinct;

    if (data.db) {
      const elTotal = document.getElementById("statTotalStreams");
      const elAlive = document.getElementById("statDbAlive");
      const elStandby = document.getElementById("statDbStandby");
      const elDead = document.getElementById("statDbDead");
      if (elTotal) elTotal.textContent = data.db.total_streams || 0;
      if (elAlive) elAlive.textContent = data.db.alive_streams || 0;
      if (elStandby) elStandby.textContent = data.db.standby_streams || 0;
      if (elDead) elDead.textContent = data.db.dead_streams || 0;
    }
  } catch (err) {
    console.error("Lỗi tải stats:", err);
  }
}

// 2. Load Groups with Counts
function updateGroupsDropdown(channels) {
  const select = document.getElementById("groupSelect");
  if (!select) return;
  const currentVal = select.value;

  const groupCounts = {};
  channels.forEach(ch => {
    const grp = ch.group || "Khác";
    groupCounts[grp] = (groupCounts[grp] || 0) + 1;
  });

  const sortedGroups = Object.keys(groupCounts).sort();
  select.innerHTML = `<option value="Tất cả">Tất cả nhóm (${channels.length})</option>`;

  sortedGroups.forEach(grp => {
    const opt = document.createElement("option");
    opt.value = grp;
    opt.textContent = `${grp} (${groupCounts[grp]})`;
    if (grp === currentVal) opt.selected = true;
    select.appendChild(opt);
  });
}

// 3. API: Load Channels
async function loadChannels() {
  const tbody = document.getElementById("channelTableBody");
  tbody.innerHTML = `<tr><td colspan="6" class="text-center py-8 text-dim">Đang tải toàn bộ dữ liệu kênh...</td></tr>`;

  try {
    const res = await fetch("/api/channels");
    const data = await res.json();
    allChannels = data.channels || [];

    updateGroupsDropdown(allChannels);
    onFilterChange();
    loadStats();
  } catch (err) {
    tbody.innerHTML = `<tr><td colspan="6" class="text-center py-8 text-red">Lỗi tải danh sách kênh: ${err}</td></tr>`;
  }
}

// 4. Reactive Filter & Sort Pipeline
function onFilterChange() {
  const q = (document.getElementById("searchInput")?.value || "").toLowerCase().trim();
  const grp = document.getElementById("groupSelect")?.value || "Tất cả";
  const statusFilter = document.getElementById("statusSelect")?.value || "ALL";
  const sortBy = document.getElementById("sortSelect")?.value || "DEFAULT";

  // Cập nhật trạng thái nút Quick Chips
  document.querySelectorAll(".chip").forEach(chip => {
    chip.classList.toggle("active", chip.dataset.status === statusFilter);
  });

  // Đếm nhanh số lượng theo các phân hạng chất lượng
  let cntAll = allChannels.length;
  let cntGood = 0;
  let cntFair = 0;
  let cntPoor = 0;
  let cntStandby = 0;
  let cntDead = 0;

  allChannels.forEach(ch => {
    const tier = getStreamQualityTier(ch.best_stream);
    if (tier === "GOOD") cntGood++;
    else if (tier === "FAIR") cntFair++;
    else if (tier === "POOR") cntPoor++;
    else if (tier === "STANDBY") cntStandby++;
    else if (tier === "DEAD") cntDead++;
  });

  const elAll = document.getElementById("countAll");
  const elGood = document.getElementById("countGood");
  const elFair = document.getElementById("countFair");
  const elPoor = document.getElementById("countPoor");
  const elStandby = document.getElementById("countStandby");
  const elDead = document.getElementById("countDead");
  if (elAll) elAll.textContent = cntAll;
  if (elGood) elGood.textContent = cntGood;
  if (elFair) elFair.textContent = cntFair;
  if (elPoor) elPoor.textContent = cntPoor;
  if (elStandby) elStandby.textContent = cntStandby;
  if (elDead) elDead.textContent = cntDead;

  // Lọc dữ liệu
  filteredChannels = allChannels.filter(ch => {
    // 1. Nhóm
    if (grp && grp !== "Tất cả" && ch.group !== grp) return false;

    // 2. Trạng thái & Phân hạng chất lượng Ping
    const tier = getStreamQualityTier(ch.best_stream);
    if (statusFilter === "GOOD" && tier !== "GOOD") return false;
    if (statusFilter === "FAIR" && tier !== "FAIR") return false;
    if (statusFilter === "POOR" && tier !== "POOR") return false;
    if (statusFilter === "STANDBY" && tier !== "STANDBY") return false;
    if (statusFilter === "DEAD" && tier !== "DEAD") return false;
    if (statusFilter === "ALIVE" && (tier !== "GOOD" && tier !== "FAIR" && tier !== "POOR")) return false;
    if (statusFilter === "MULTI" && ch.stream_count <= 1) return false;
    if (statusFilter === "SINGLE" && ch.stream_count !== 1) return false;

    // 3. Từ khóa tìm kiếm
    if (q) {
      const matchName = ch.name.toLowerCase().includes(q);
      const matchGroup = ch.group.toLowerCase().includes(q);
      const matchUrl = ch.streams && ch.streams.some(s => s.url.toLowerCase().includes(q));
      if (!matchName && !matchGroup && !matchUrl) return false;
    }

    return true;
  });

  // Sắp xếp
  if (sortBy === "PING_ASC") {
    filteredChannels.sort((a, b) => {
      const pA = (a.best_stream && a.best_stream.latency_ms > 0) ? a.best_stream.latency_ms : 99999;
      const pB = (b.best_stream && b.best_stream.latency_ms > 0) ? b.best_stream.latency_ms : 99999;
      return pA - pB;
    });
  } else if (sortBy === "SOURCES_DESC") {
    filteredChannels.sort((a, b) => b.stream_count - a.stream_count);
  } else if (sortBy === "SCORE_DESC") {
    filteredChannels.sort((a, b) => {
      const sA = a.best_stream ? a.best_stream.score : 0;
      const sB = b.best_stream ? b.best_stream.score : 0;
      return sB - sA;
    });
  } else if (sortBy === "NAME_ASC") {
    filteredChannels.sort((a, b) => a.name.localeCompare(b.name));
  } else {
    // DEFAULT: Theo Nhóm rồi Tên
    filteredChannels.sort((a, b) => a.group.localeCompare(b.group) || a.name.localeCompare(b.name));
  }

  currentPage = 1;
  renderCurrentPage();
  updatePublishPreview();
}

function setQuickStatus(status) {
  const select = document.getElementById("statusSelect");
  if (select) select.value = status;
  onFilterChange();
}

// 5. Phân trang & Render Bảng
function changePageSize() {
  const sel = document.getElementById("pageSizeSelect");
  if (sel) pageSize = parseInt(sel.value, 10) || 100;
  currentPage = 1;
  renderCurrentPage();
}

function goToPage(p) {
  currentPage = p;
  renderCurrentPage();
}

function prevPage() {
  if (currentPage > 1) {
    currentPage--;
    renderCurrentPage();
  }
}

function nextPage() {
  const totalPages = Math.max(1, Math.ceil(filteredChannels.length / pageSize));
  if (currentPage < totalPages) {
    currentPage++;
    renderCurrentPage();
  }
}

function goToLastPage() {
  const totalPages = Math.max(1, Math.ceil(filteredChannels.length / pageSize));
  currentPage = totalPages;
  renderCurrentPage();
}

function renderCurrentPage() {
  const tbody = document.getElementById("channelTableBody");
  const totalCount = filteredChannels.length;
  const totalPages = Math.max(1, Math.ceil(totalCount / pageSize));

  if (currentPage > totalPages) currentPage = totalPages;
  if (currentPage < 1) currentPage = 1;

  const startIdx = (currentPage - 1) * pageSize;
  const endIdx = Math.min(startIdx + pageSize, totalCount);
  const pageChannels = filteredChannels.slice(startIdx, endIdx);

  // Cập nhật thông tin phân trang
  const filterSummary = document.getElementById("filterSummaryText");
  if (filterSummary) {
    filterSummary.textContent = `Tìm thấy ${totalCount.toLocaleString()} kênh (trên tổng số ${allChannels.length.toLocaleString()})`;
  }

  const pageInfo = document.getElementById("paginationInfo");
  if (pageInfo) {
    if (totalCount === 0) {
      pageInfo.textContent = "Không có kênh nào";
    } else {
      pageInfo.textContent = `Hiển thị ${startIdx + 1} - ${endIdx} / ${totalCount.toLocaleString()} kênh`;
    }
  }

  const pageInd = document.getElementById("pageIndicator");
  if (pageInd) {
    pageInd.textContent = `Trang ${currentPage} / ${totalPages}`;
  }

  const btnFirst = document.getElementById("btnPageFirst");
  const btnPrev = document.getElementById("btnPagePrev");
  const btnNext = document.getElementById("btnPageNext");
  const btnLast = document.getElementById("btnPageLast");
  if (btnFirst) btnFirst.disabled = (currentPage <= 1);
  if (btnPrev) btnPrev.disabled = (currentPage <= 1);
  if (btnNext) btnNext.disabled = (currentPage >= totalPages);
  if (btnLast) btnLast.disabled = (currentPage >= totalPages);

  if (pageChannels.length === 0) {
    tbody.innerHTML = `<tr><td colspan="6" class="text-center py-8 text-dim">Không có kênh nào phù hợp với bộ lọc hiện tại.</td></tr>`;
    return;
  }

  tbody.innerHTML = pageChannels.map((ch) => {
    const logoHtml = ch.logo
      ? `<img src="${ch.logo}" class="ch-logo" alt="${escapeHtml(ch.name)}" onerror="this.src='data:image/svg+xml;utf8,<svg xmlns=\'http://www.w3.org/2000/svg\' width=\'36\' height=\'36\' fill=\'%2364748b\'><rect width=\'36\' height=\'36\' rx=\'4\' fill=\'%231c2433\'/><text x=\'50%\' y=\'55%\' dominant-baseline=\'middle\' text-anchor=\'middle\' font-size=\'14\' fill=\'%2394a3b8\'>TV</text></svg>'">`
      : `<div class="ch-logo text-center" style="display:flex;align-items:center;justify-content:center;font-size:12px;color:#94a3b8;">TV</div>`;

    const best = ch.best_stream;
    let statusBadge = `<span class="badge badge-amber">Chưa kiểm tra</span>`;

    if (best) {
      const tier = getStreamQualityTier(best);
      const ms = best.latency_ms;
      if (tier === "GOOD") {
        statusBadge = `<span class="badge badge-green">🟢 ${ms} ms (Tốt)</span>`;
      } else if (tier === "FAIR") {
        statusBadge = `<span class="badge badge-yellow">🟡 ${ms} ms (T.Bình)</span>`;
      } else if (tier === "POOR") {
        statusBadge = `<span class="badge badge-orange">🟠 ${ms} ms (Yếu)</span>`;
      } else if (tier === "STANDBY") {
        statusBadge = (ms > 0)
          ? `<span class="badge badge-amber">🟡 Chờ (${ms} ms)</span>`
          : `<span class="badge badge-amber">⚪ Chưa ping</span>`;
      } else if (tier === "DEAD") {
        statusBadge = `<span class="badge badge-red">🔴 Lỗi</span>`;
      }
    }

    const countBadge = ch.stream_count > 1
      ? `<span class="badge badge-purple">${ch.stream_count} luồng</span>`
      : `<span class="badge badge-blue">1 luồng</span>`;

    const safeNameAttr = encodeURIComponent(ch.name);
    const needVlc = best && isVlcRequired(best.url);
    const playBtnHtml = needVlc
      ? `<button class="btn btn-action-vlc text-sm" onclick="openInVlc('${encodeURIComponent(best.url)}')" title="Luồng UDP/FLV đặc thù - Kích hoạt VLC">🎬 VLC</button>`
      : `<button class="btn btn-action-play text-sm" onclick="playChannel('${safeNameAttr}')" title="Xem phát trực tiếp trên Web">▶ Xem</button>`;

    return `
      <tr>
        <td>${logoHtml}</td>
        <td>
          <div class="ch-name">${escapeHtml(ch.name)}</div>
        </td>
        <td><span class="badge badge-blue">${escapeHtml(ch.group)}</span></td>
        <td>${countBadge}</td>
        <td>${statusBadge}</td>
        <td style="text-align: right; white-space: nowrap;">
          ${playBtnHtml}
          <button class="btn btn-secondary text-sm" onclick="openStreamModal('${safeNameAttr}')" title="Xem chi tiết các nguồn">
            🔍 Nguồn (${ch.stream_count})
          </button>
        </td>
      </tr>
    `;
  }).join("");
}

// 6. Modal Chi Tiết Streams (Tra cứu chuẩn xác theo Tên Kênh)
function openStreamModal(encodedChannelName) {
  const channelName = decodeURIComponent(encodedChannelName);
  const ch = allChannels.find(c => c.name === channelName);
  if (!ch) {
    showToast(`Không tìm thấy kênh: ${channelName}`, true);
    return;
  }

  document.getElementById("modalChannelTitle").textContent = `${ch.name} (${ch.group}) — Tổng ${ch.streams.length} nguồn`;
  renderModalStreamItems(ch);
  document.getElementById("streamModal").style.display = "flex";
}

function renderModalStreamItems(ch) {
  const body = document.getElementById("modalChannelBody");
  const safeChannelName = encodeURIComponent(ch.name);
  const safeGroupName = encodeURIComponent(ch.group);

  body.innerHTML = ch.streams.map((s, idx) => {
    const tier = getStreamQualityTier(s);
    let badge = `<span class="badge badge-amber" id="badge-stream-${idx}">CHƯA PING</span>`;
    if (tier === "GOOD") {
      badge = `<span class="badge badge-green" id="badge-stream-${idx}">🟢 ${s.latency_ms} ms (Tốt)</span>`;
    } else if (tier === "FAIR") {
      badge = `<span class="badge badge-yellow" id="badge-stream-${idx}">🟡 ${s.latency_ms} ms (Trung bình)</span>`;
    } else if (tier === "POOR") {
      badge = `<span class="badge badge-orange" id="badge-stream-${idx}">🟠 ${s.latency_ms} ms (Yếu)</span>`;
    } else if (tier === "STANDBY") {
      badge = (s.latency_ms > 0)
        ? `<span class="badge badge-amber" id="badge-stream-${idx}">🟡 Chờ (${s.latency_ms} ms • ${s.score}đ)</span>`
        : `<span class="badge badge-amber" id="badge-stream-${idx}">🟡 Chờ (Chưa ping • ${s.score}đ)</span>`;
    } else if (tier === "DEAD") {
      badge = `<span class="badge badge-red" id="badge-stream-${idx}">🔴 DEAD</span>`;
    }

    const isPrimary = (idx === 0);
    const safeUrl = encodeURIComponent(s.url);

    return `
      <div class="stream-item" id="stream-item-${idx}">
        <div class="stream-item-header">
          <div>
            <b>Nguồn ${idx + 1}</b> ${isPrimary ? '<span class="badge badge-green text-sm">Ưu tiên số 1</span>' : '<span class="badge badge-blue text-sm">Dự phòng #' + (idx + 1) + '</span>'}
          </div>
          <div>${badge}</div>
        </div>
        <div class="stream-url">${escapeHtml(s.url)}</div>
        ${s.source ? `<div class="text-sm text-dim mt-4">Nguồn file gốc: <code>${escapeHtml(s.source)}</code></div>` : ""}
        <div class="stream-item-actions">
          <button class="btn-action btn-action-play" onclick="playSingleStream('${safeChannelName}', '${safeUrl}', ${idx})">
            ▶ Phát luồng này
          </button>
          <button class="btn-action" onclick="openInVlc('${escapeHtml(s.url)}')">
            🎬 Mở VLC
          </button>
          <button class="btn-action" onclick="copyToClipboard('${escapeHtml(s.url)}')">
            📋 Sao chép
          </button>
          <button class="btn-action btn-action-probe" id="btn-probe-${idx}" onclick="testSingleStream('${safeChannelName}', '${safeGroupName}', '${safeUrl}', ${idx})">
            ⚡ Test Ping
          </button>
          ${!isPrimary ? `
            <button class="btn-action btn-action-primary" onclick="setPrimaryStream('${safeChannelName}', '${safeUrl}')">
              ⭐ Đặt làm Nguồn #1
            </button>
          ` : ''}
        </div>
      </div>
    `;
  }).join("");
}

function closeStreamModal() {
  document.getElementById("streamModal").style.display = "none";
}

// 7. Video Player Controller (HLS.js)
function isVlcRequired(url) {
  if (!url) return false;
  const u = url.toLowerCase().trim();
  if (u.startsWith("udp://") || u.startsWith("rtp://") || u.startsWith("rtmp://") || u.startsWith("rtsp://")) {
    return true;
  }
  if (u.includes(".flv") || u.endsWith(".ts") || u.includes(".mkv")) {
    return true;
  }
  return false;
}

function showVlcFallbackPrompt(reason) {
  const vlcFallback = document.getElementById("playerVlcFallback");
  const vlcReason = document.getElementById("playerVlcReason");
  if (vlcReason && reason) vlcReason.textContent = reason;
  if (vlcFallback) vlcFallback.style.display = "flex";
}

function playChannel(encodedChannelName) {
  const channelName = decodeURIComponent(encodedChannelName);
  const ch = allChannels.find(c => c.name === channelName);
  if (!ch || !ch.streams || ch.streams.length === 0) {
    showToast("Kênh không có luồng phát", true);
    return;
  }
  const streamUrl = ch.best_stream ? ch.best_stream.url : ch.streams[0].url;
  if (isVlcRequired(streamUrl)) {
    showToast("📡 Luồng UDP/FLV truyền hình — Đang kích hoạt VLC...", false);
    openInVlc(streamUrl);
    return;
  }
  openVideoPlayer(ch.name, streamUrl, ch.group);
}

function playSingleStream(encodedChannelName, encodedUrl, streamIdx) {
  const channelName = decodeURIComponent(encodedChannelName);
  const streamUrl = decodeURIComponent(encodedUrl);
  if (isVlcRequired(streamUrl)) {
    showToast("📡 Luồng UDP/FLV truyền hình — Đang kích hoạt VLC...", false);
    openInVlc(streamUrl);
    return;
  }
  openVideoPlayer(`${channelName} • Nguồn #${streamIdx + 1}`, streamUrl);
}

function openVideoPlayer(channelTitle, streamUrl, group = "") {
  currentPlayingChannel = channelTitle;
  currentPlayingOriginalUrl = streamUrl;
  isProxyActive = false;

  const modal = document.getElementById("playerModal");
  const titleEl = document.getElementById("playerChannelTitle");
  const urlEl = document.getElementById("playerCurrentUrl");
  const btnToggleProxy = document.getElementById("btnToggleProxy");

  if (titleEl) titleEl.textContent = group ? `${channelTitle} (${group})` : channelTitle;
  if (urlEl) urlEl.textContent = streamUrl;
  if (btnToggleProxy) {
    btnToggleProxy.textContent = "🔄 Đổi sang Proxy CORS";
    btnToggleProxy.classList.remove("btn-action-primary");
  }

  modal.style.display = "flex";
  startPlayback(streamUrl, false);
}

function startPlayback(streamUrl, useProxy = false) {
  const video = document.getElementById("videoPlayer");
  const loadingOverlay = document.getElementById("playerLoadingOverlay");
  const loadingText = document.getElementById("playerLoadingText");
  const statusBadge = document.getElementById("playerStatusBadge");
  const streamInfo = document.getElementById("playerStreamInfo");
  const vlcFallback = document.getElementById("playerVlcFallback");

  if (vlcFallback) vlcFallback.style.display = "none";
  if (loadingOverlay) loadingOverlay.style.display = "flex";
  if (loadingText) loadingText.textContent = useProxy ? "Đang kết nối qua CORS Proxy..." : "Đang kết nối luồng phát...";
  if (statusBadge) {
    statusBadge.className = "badge badge-amber";
    statusBadge.textContent = "⏳ Đang kết nối...";
  }

  if (hlsInstance) {
    hlsInstance.destroy();
    hlsInstance = null;
  }
  video.pause();
  video.removeAttribute("src");
  video.load();

  if (isVlcRequired(streamUrl)) {
    if (loadingOverlay) loadingOverlay.style.display = "none";
    if (statusBadge) {
      statusBadge.className = "badge badge-purple";
      statusBadge.textContent = "📡 Cần VLC";
    }
    showVlcFallbackPrompt("Luồng truyền hình này sử dụng giao thức UDP / Multicast hoặc định dạng FLV/TS mà trình duyệt không hỗ trợ. VLC sẽ giúp bạn phát mượt mà.");
    openInVlc(streamUrl);
    return;
  }

  const finalUrl = useProxy ? `/api/proxy_stream?url=${encodeURIComponent(streamUrl)}` : streamUrl;

  if (window.Hls && Hls.isSupported()) {
    hlsInstance = new Hls({
      enableWorker: true,
      lowLatencyMode: true,
      maxBufferLength: 15,
      maxMaxBufferLength: 30
    });

    hlsInstance.loadSource(finalUrl);
    hlsInstance.attachMedia(video);

    hlsInstance.on(Hls.Events.MANIFEST_PARSED, (event, data) => {
      if (loadingOverlay) loadingOverlay.style.display = "none";
      if (statusBadge) {
        statusBadge.className = "badge badge-green";
        statusBadge.textContent = useProxy ? "🟢 Live (Proxy)" : "🟢 HLS Live";
      }
      if (streamInfo && data.levels && data.levels.length > 0) {
        const lvl = data.levels[0];
        streamInfo.textContent = `${lvl.width || 0}x${lvl.height || 0} (${Math.round((lvl.bitrate || 0)/1000)} kbps)`;
      }
      video.play().catch(e => console.log("Autoplay:", e));
    });

    hlsInstance.on(Hls.Events.ERROR, (event, data) => {
      console.warn("Hls error:", data);
      if (data.fatal) {
        if (!useProxy) {
          // Tự động chuyển qua proxy nếu stream bị chặn CORS
          showToast("Luồng bị hạn chế mạng, đang tự động chuyển sang Proxy CORS...", false);
          togglePlayerProxy();
        } else {
          if (loadingOverlay) loadingOverlay.style.display = "none";
          if (statusBadge) {
            statusBadge.className = "badge badge-red";
            statusBadge.textContent = "🔴 Cần VLC";
          }
          showVlcFallbackPrompt("Trình duyệt không thể giải mã luồng video này (Codec MPEG-2, âm thanh AC-3 hoặc DRM). Bấm nút bên dưới để mở ngay bằng VLC!");
        }
      }
    });
  } else if (video.canPlayType('application/vnd.apple.mpegurl')) {
    // Safari Native HLS
    video.src = finalUrl;
    video.addEventListener('loadedmetadata', () => {
      if (loadingOverlay) loadingOverlay.style.display = "none";
      if (statusBadge) {
        statusBadge.className = "badge badge-green";
        statusBadge.textContent = "🟢 Live (Safari)";
      }
      video.play();
    }, { once: true });
  } else {
    video.src = finalUrl;
    video.play();
  }
}

function togglePlayerProxy() {
  isProxyActive = !isProxyActive;
  const btnToggleProxy = document.getElementById("btnToggleProxy");
  if (btnToggleProxy) {
    btnToggleProxy.textContent = isProxyActive ? "✅ Đang dùng Proxy CORS" : "🔄 Đổi sang Proxy CORS";
    btnToggleProxy.classList.toggle("btn-action-primary", isProxyActive);
  }
  startPlayback(currentPlayingOriginalUrl, isProxyActive);
}

function closePlayerModal() {
  const modal = document.getElementById("playerModal");
  const video = document.getElementById("videoPlayer");
  const vlcFallback = document.getElementById("playerVlcFallback");
  if (vlcFallback) vlcFallback.style.display = "none";
  if (video) {
    video.pause();
    video.removeAttribute("src");
    video.load();
  }
  if (hlsInstance) {
    hlsInstance.destroy();
    hlsInstance = null;
  }
  modal.style.display = "none";
}

function copyPlayerUrl() {
  if (currentPlayingOriginalUrl) {
    copyToClipboard(currentPlayingOriginalUrl);
  }
}

async function openInVlc(customUrl) {
  const urlToPlay = customUrl || currentPlayingOriginalUrl;
  if (!urlToPlay) {
    showToast("Không tìm thấy link stream", true);
    return;
  }
  showToast("⏳ Đang kích hoạt ứng dụng VLC trên máy tính...");
  try {
    const res = await fetch("/api/open_vlc", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ url: urlToPlay })
    });
    const data = await res.json();
    if (data.success) {
      showToast("🚀 Đã mở ứng dụng VLC thành công!");
    } else {
      // Fallback sao chép link nếu không gọi được lệnh hệ thống
      copyToClipboard(urlToPlay);
      showToast("Lỗi mở VLC: " + data.error + " (Đã copy link)", true);
    }
  } catch (err) {
    copyToClipboard(urlToPlay);
    showToast("Lỗi mạng: " + err + " (Đã copy link)", true);
  }
}

// 8. Test Ping Đơn Lẻ Ngay Trong Modal
async function testSingleStream(encodedChannelName, encodedGroupName, encodedUrl, streamIdx) {
  const chName = decodeURIComponent(encodedChannelName);
  const grpName = decodeURIComponent(encodedGroupName);
  const url = decodeURIComponent(encodedUrl);
  const btn = document.getElementById(`btn-probe-${streamIdx}`);
  const badgeEl = document.getElementById(`badge-stream-${streamIdx}`);

  if (btn) {
    btn.disabled = true;
    btn.textContent = "⏳ Đang ping...";
  }

  try {
    const res = await fetch("/api/stream/probe", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ url: url, channel_name: chName, group_name: grpName })
    });
    const data = await res.json();
    if (data.success && data.probe) {
      const p = data.probe;
      const ch = allChannels.find(c => c.name === chName);
      if (ch && ch.streams[streamIdx]) {
        ch.streams[streamIdx].last_status = p.is_alive ? "ALIVE" : "DEAD";
        ch.streams[streamIdx].latency_ms = p.latency_ms;
        ch.streams[streamIdx].score = p.is_alive ? Math.min(100, (ch.streams[streamIdx].score || 50) + 10) : 0;
      }

      if (badgeEl) {
        if (p.is_alive) {
          badgeEl.className = "badge badge-green";
          badgeEl.textContent = `🟢 ALIVE (${p.latency_ms}ms)`;
          showToast(`Link sống! Độ trễ: ${p.latency_ms} ms`);
        } else {
          badgeEl.className = "badge badge-red";
          badgeEl.textContent = `🔴 DEAD (${p.error || "Timeout"})`;
          showToast(`Link không phản hồi (${p.error || "Timeout"})`, true);
        }
      }
      renderCurrentPage();
    } else {
      showToast(data.error || "Lỗi kiểm tra", true);
    }
  } catch (err) {
    showToast(`Lỗi mạng: ${err}`, true);
  } finally {
    if (btn) {
      btn.disabled = false;
      btn.textContent = "⚡ Test Ping";
    }
  }
}

// 9. Đặt Làm Nguồn Ưu Tiên Số 1
async function setPrimaryStream(encodedChannelName, encodedUrl) {
  const chName = decodeURIComponent(encodedChannelName);
  const url = decodeURIComponent(encodedUrl);

  try {
    const res = await fetch("/api/channels/set_primary", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ channel_name: chName, url: url })
    });
    const data = await res.json();
    if (data.success) {
      showToast(`Đã đưa link lên làm nguồn ưu tiên số 1 của kênh ${chName}!`);
      const ch = allChannels.find(c => c.name === chName);
      if (ch) {
        const idx = ch.streams.findIndex(s => s.url === url);
        if (idx > 0) {
          const item = ch.streams.splice(idx, 1)[0];
          ch.streams.unshift(item);
          ch.best_stream = item;
        }
        renderModalStreamItems(ch);
      }
      renderCurrentPage();
    } else {
      showToast(data.error || "Không thể đặt nguồn", true);
    }
  } catch (err) {
    showToast(`Lỗi: ${err}`, true);
  }
}

// 10. Drag & Drop File Upload
function initDropZone() {
  const dropZone = document.getElementById("dropZone");
  const fileInput = document.getElementById("fileInput");
  if (!dropZone || !fileInput) return;

  dropZone.addEventListener("click", () => fileInput.click());

  dropZone.addEventListener("dragover", (e) => {
    e.preventDefault();
    dropZone.classList.add("dragover");
  });

  dropZone.addEventListener("dragleave", () => {
    dropZone.classList.remove("dragover");
  });

  dropZone.addEventListener("drop", (e) => {
    e.preventDefault();
    dropZone.classList.remove("dragover");
    if (e.dataTransfer.files.length > 0) {
      handleFilesUpload(e.dataTransfer.files);
    }
  });

  fileInput.addEventListener("change", (e) => {
    if (e.target.files.length > 0) {
      handleFilesUpload(e.target.files);
    }
  });
}

async function handleFilesUpload(files) {
  let totalUploaded = 0;
  for (const file of files) {
    showToast(`Đang nạp file: ${file.name}...`);
    const content = await file.text();
    try {
      const res = await fetch("/api/sources/upload_text", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ filename: file.name, content: content })
      });
      const data = await res.json();
      if (data.success) {
        totalUploaded += data.added_channels;
      }
    } catch (err) {
      showToast(`Lỗi đọc file ${file.name}: ${err}`, true);
    }
  }

  showToast(`Đã nạp thành công ${totalUploaded} kênh từ các file!`);
  loadStats();
  loadChannels();
  switchTab("tabChannels");
}

// 11. Thêm URL từ xa
async function addSourceUrl() {
  const input = document.getElementById("m3uUrlInput");
  const url = input.value.trim();
  if (!url) {
    showToast("Vui lòng nhập đường link M3U", true);
    return;
  }

  showToast("Đang tải dữ liệu M3U từ xa...");
  try {
    const res = await fetch("/api/sources/add_url", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ url: url })
    });
    const data = await res.json();
    if (data.success) {
      showToast(`Đã thêm thành công ${data.added_channels} kênh từ URL!`);
      input.value = "";
      loadStats();
      loadChannels();
      switchTab("tabChannels");
    } else {
      showToast(`Lỗi: ${data.error}`, true);
    }
  } catch (err) {
    showToast(`Lỗi mạng: ${err}`, true);
  }
}

async function loadDefaultRepoSource() {
  showToast("Đang nạp file iptv/default.m3u có sẵn trong repo...");
  try {
    const res = await fetch("/api/sources/load_default", { method: "POST" });
    const data = await res.json();
    if (data.success) {
      showToast(`Đã nạp thành công ${data.loaded_channels} kênh từ iptv/default.m3u!`);
      loadStats();
      loadChannels();
      switchTab("tabChannels");
    } else {
      showToast(`Lỗi: ${data.error}`, true);
    }
  } catch (err) {
    showToast(`Lỗi: ${err}`, true);
  }
}

async function clearAllSources() {
  if (!confirm("Bạn có chắc chắn muốn xóa toàn bộ nguồn kênh đã nạp trong bộ nhớ đệm?")) return;
  try {
    const res = await fetch("/api/sources/clear", { method: "POST" });
    const data = await res.json();
    if (data.success) {
      showToast("Đã xóa làm mới danh sách nguồn.");
      loadStats();
      loadChannels();
    }
  } catch (err) {
    showToast(`Lỗi: ${err}`, true);
  }
}

// 12. Health Check / Ping probe (Browser-Grade Deep Probe)
async function startHealthCheck(targetMode = "ALL") {
  const btnAll = document.getElementById("btnStartProbe");
  const btnStandby = document.getElementById("btnProbeStandby");
  if (btnAll) btnAll.disabled = true;
  if (btnStandby) btnStandby.disabled = true;

  if (targetMode === "STANDBY") {
    if (btnStandby) btnStandby.textContent = "⏳ Đang quét kênh Chờ...";
    showToast("Đang kiểm tra lại các kênh STANDBY theo chuẩn Browser-Grade...");
  } else {
    if (btnAll) btnAll.textContent = "⏳ Đang quét toàn bộ...";
    showToast("Đang quét toàn bộ luồng đa luồng song song...");
  }

  document.getElementById("progressBox").style.display = "block";
  document.getElementById("progressFill").style.width = "0%";
  document.getElementById("progressText").textContent = `Bắt đầu quét Browser-Grade (${targetMode === 'STANDBY' ? 'Chỉ kênh Chờ' : 'Toàn bộ'})...`;

  try {
    const res = await fetch("/api/health_check", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ target_mode: targetMode })
    });
    const data = await res.json();
    if (!data.success) {
      showToast(data.error, true);
      if (btnAll) {
        btnAll.disabled = false;
        btnAll.textContent = "▶️ Quét Sâu Chuẩn Trình Duyệt (Tất Cả)";
      }
      if (btnStandby) {
        btnStandby.disabled = false;
        btnStandby.textContent = "⚡ Chỉ Quét Lại Kênh Đang CHỜ";
      }
      return;
    }

    if (probePollingTimer) clearInterval(probePollingTimer);
    probePollingTimer = setInterval(pollHealthProgress, 1000);
  } catch (err) {
    showToast(`Lỗi: ${err}`, true);
    if (btnAll) {
      btnAll.disabled = false;
      btnAll.textContent = "▶️ Quét Sâu Chuẩn Trình Duyệt (Tất Cả)";
    }
    if (btnStandby) {
      btnStandby.disabled = false;
      btnStandby.textContent = "⚡ Chỉ Quét Lại Kênh Đang CHỜ";
    }
  }
}

async function pollHealthProgress() {
  try {
    const res = await fetch("/api/health_progress");
    const data = await res.json();

    document.getElementById("progressFill").style.width = `${data.pct}%`;
    document.getElementById("progressPctText").textContent = `${data.pct}%`;
    document.getElementById("statAliveCount").textContent = `🟢 ${data.alive_count} Sống`;
    document.getElementById("statDeadCount").textContent = `🔴 ${data.dead_count} Lỗi`;
    document.getElementById("statTotalChecked").textContent = `Đã kiểm tra: ${data.completed} / ${data.total}`;
    document.getElementById("progressText").textContent = data.is_running ? "Đang đo latency & xác thực video..." : "Hoàn tất quét!";

    if (!data.is_running && data.total > 0 && data.completed >= data.total) {
      clearInterval(probePollingTimer);
      probePollingTimer = null;
      const btnAll = document.getElementById("btnStartProbe");
      const btnStandby = document.getElementById("btnProbeStandby");
      if (btnAll) {
        btnAll.disabled = false;
        btnAll.textContent = "▶️ Quét Sâu Chuẩn Trình Duyệt (Tất Cả)";
      }
      if (btnStandby) {
        btnStandby.disabled = false;
        btnStandby.textContent = "⚡ Chỉ Quét Lại Kênh Đang CHỜ";
      }
      showToast(`Đã hoàn tất kiểm tra: ${data.alive_count} link sống, ${data.dead_count} link lỗi.`);
      loadStats();
      loadChannels();
    }
  } catch (err) {
    console.error("Lỗi polling progress:", err);
  }
}

// 13. Rules Editor & Latency Thresholds
async function loadRules() {
  try {
    const res = await fetch("/api/rules");
    const data = await res.json();
    if (data.ping_thresholds) {
      pingThresholds = data.ping_thresholds;
      applyPingThresholdLabels();
    }
    document.getElementById("rulesChannelJson").value = JSON.stringify(data.channel_aliases || {}, null, 2);
    document.getElementById("rulesGroupJson").value = JSON.stringify(data.group_mappings || {}, null, 2);
  } catch (err) {
    console.error("Lỗi tải rules:", err);
  }
}

async function saveRules() {
  try {
    const chAliases = JSON.parse(document.getElementById("rulesChannelJson").value);
    const grpMappings = JSON.parse(document.getElementById("rulesGroupJson").value);

    const res = await fetch("/api/rules", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        channel_aliases: chAliases,
        group_mappings: grpMappings,
        ping_thresholds: pingThresholds
      })
    });
    const data = await res.json();
    if (data.success) {
      showToast("Đã lưu quy tắc và cập nhật lại danh sách kênh!");
      loadChannels();
    } else {
      showToast("Lỗi khi lưu quy tắc", true);
    }
  } catch (err) {
    showToast(`Lỗi cú pháp JSON: ${err.message}`, true);
  }
}

// 14. Export & OTA Publish with Quality Filter Criteria
function getPublishFilters() {
  return {
    allow_good: document.getElementById("chkPubGood") ? document.getElementById("chkPubGood").checked : true,
    allow_fair: document.getElementById("chkPubFair") ? document.getElementById("chkPubFair").checked : true,
    allow_poor: document.getElementById("chkPubPoor") ? document.getElementById("chkPubPoor").checked : false,
    allow_standby: document.getElementById("chkPubStandby") ? document.getElementById("chkPubStandby").checked : true,
    allow_dead: document.getElementById("chkPubDead") ? document.getElementById("chkPubDead").checked : false,
    good_threshold_ms: pingThresholds.good_ms,
    fair_threshold_ms: pingThresholds.fair_ms
  };
}

function updatePublishPreview() {
  if (!allChannels || allChannels.length === 0) return;
  const filters = getPublishFilters();
  let countGood = 0;
  let countFair = 0;
  let countPoor = 0;
  let countStandby = 0;
  let countDead = 0;
  let countWillExport = 0;

  allChannels.forEach(ch => {
    const tier = getStreamQualityTier(ch.best_stream);
    if (tier === "GOOD") countGood++;
    else if (tier === "FAIR") countFair++;
    else if (tier === "POOR") countPoor++;
    else if (tier === "STANDBY") countStandby++;
    else if (tier === "DEAD") countDead++;

    let willExport = false;
    if (tier === "GOOD" && filters.allow_good) willExport = true;
    else if (tier === "FAIR" && filters.allow_fair) willExport = true;
    else if (tier === "POOR" && filters.allow_poor) willExport = true;
    else if (tier === "STANDBY" && filters.allow_standby) willExport = true;
    else if (tier === "DEAD" && filters.allow_dead) willExport = true;

    if (willExport) countWillExport++;
  });

  const elG = document.getElementById("countPubGood");
  const elF = document.getElementById("countPubFair");
  const elP = document.getElementById("countPubPoor");
  const elS = document.getElementById("countPubStandby");
  const elD = document.getElementById("countPubDead");
  if (elG) elG.textContent = `${countGood} kênh`;
  if (elF) elF.textContent = `${countFair} kênh`;
  if (elP) elP.textContent = `${countPoor} kênh`;
  if (elS) elS.textContent = `${countStandby} kênh`;
  if (elD) elD.textContent = `${countDead} kênh`;

  const badge = document.getElementById("previewPublishCountBadge");
  if (badge) {
    const pct = allChannels.length > 0 ? ((countWillExport / allChannels.length) * 100).toFixed(1) : 0;
    badge.textContent = `Dự kiến xuất bản: ${countWillExport} / ${allChannels.length} kênh (${pct}%)`;
  }
}

async function exportPlaylist() {
  const includeBackup = document.getElementById("chkIncludeBackup") ? document.getElementById("chkIncludeBackup").checked : true;
  const pubFilters = getPublishFilters();
  showToast("Đang tạo file live.m3u và iptv_manifest.json theo bộ lọc tiêu chí...");

  try {
    const res = await fetch("/api/export", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        include_backup: includeBackup,
        publish_filters: pubFilters
      })
    });
    const data = await res.json();
    if (data.success) {
      const m = data.result.manifest;
      document.getElementById("exportSummaryText").textContent =
        `Phiên bản: ${m.version} • ${m.channel_count} kênh chính (${m.total_streams} tổng luồng) • SHA256: ${m.sha256.substring(0, 16)}...`;
      document.getElementById("exportResultBox").style.display = "block";
      showToast(`Xuất bản thành công: ${m.channel_count} kênh!`);
      switchTab("tabExport");
    } else {
      showToast("Lỗi xuất bản playlist", true);
    }
  } catch (err) {
    showToast(`Lỗi: ${err}`, true);
  }
}

async function publishToRomCloud() {
  const filters = getPublishFilters();
  if (!confirm("Đẩy file live.m3u và manifest đã qua chọn lọc theo tiêu chí chất lượng trực tiếp vào thư mục RomCloud iptv/ để máy handheld dùng ngay?")) return;
  showToast("Đang xuất bản vào thư mục RomCloud/iptv/...");

  try {
    const res = await fetch("/api/publish_ota", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        include_backup: true,
        publish_filters: filters
      })
    });
    const data = await res.json();
    if (data.success) {
      const m = data.result.manifest;
      showToast(`Đã xuất bản thành công ${m.channel_count} kênh vào RomCloud/iptv/!`);
    } else {
      showToast("Lỗi khi lưu vào RomCloud", true);
    }
  } catch (err) {
    showToast(`Lỗi: ${err}`, true);
  }
}

// ==========================================================================
// 15. MultiView Grid Wall Controller (2x2, 4x2, 4x4 Viewports)
// ==========================================================================
let currentGridLayout = "2x2";
let gridSlots = [];
let cachedChannelOptionsHtml = "";

function initGridIfEmpty() {
  if (gridSlots.length === 0) {
    setGridLayout("2x2");
    // Nếu chưa có kênh nào, tự động nạp Top Kênh Sống (hoặc VTV)
    setTimeout(() => {
      const allEmpty = gridSlots.every(s => !s.channelName);
      if (allEmpty && allChannels.length > 0) {
        quickFillGrid("vtv");
      }
    }, 150);
  }
}

function getGridTargetCount(layout) {
  if (layout === "2x2") return 4;
  if (layout === "4x2") return 8;
  if (layout === "4x4") return 16;
  return 4;
}

function setGridLayout(layout) {
  currentGridLayout = layout;

  // Cập nhật button active
  ["2x2", "4x2", "4x4"].forEach(l => {
    const btn = document.getElementById(`btnLayout${l}`);
    if (btn) btn.classList.toggle("active-layout", l === layout);
  });

  const container = document.getElementById("gridviewWall");
  if (container) {
    container.className = `gridview-container layout-${layout}`;
  }

  const targetCount = getGridTargetCount(layout);
  const prevCount = gridSlots.length;

  // Nếu giảm số slot: hủy các luồng HLS thừa
  if (prevCount > targetCount) {
    for (let i = targetCount; i < prevCount; i++) {
      if (gridSlots[i] && gridSlots[i].hls) {
        gridSlots[i].hls.destroy();
      }
    }
    gridSlots = gridSlots.slice(0, targetCount);
  } else if (prevCount < targetCount) {
    // Nếu tăng số slot: tạo slot mới
    for (let i = prevCount; i < targetCount; i++) {
      gridSlots.push({
        channelName: "",
        streamUrl: "",
        hls: null,
        isMuted: true,
        status: "EMPTY"
      });
    }
  }

  renderGridSlots();
}

function buildCachedChannelOptions() {
  if (!allChannels || allChannels.length === 0) {
    return '<option value="">-- Trống --</option>';
  }

  // Nhóm theo Category
  const groups = {};
  allChannels.forEach(ch => {
    const g = ch.group || "Khác";
    if (!groups[g]) groups[g] = [];
    groups[g].push(ch);
  });

  let html = '<option value="">-- Chọn kênh hiển thị --</option>';
  Object.keys(groups).sort().forEach(grp => {
    html += `<optgroup label="${escapeHtml(grp)}">`;
    groups[grp].forEach(ch => {
      const statusIcon = (ch.best_stream && ch.best_stream.last_status === "ALIVE") ? "🟢" : "⚪";
      html += `<option value="${escapeHtml(ch.name)}">${statusIcon} ${escapeHtml(ch.name)}</option>`;
    });
    html += `</optgroup>`;
  });

  cachedChannelOptionsHtml = html;
  return html;
}

function renderGridSlots() {
  const container = document.getElementById("gridviewWall");
  if (!container) return;

  const optionsHtml = cachedChannelOptionsHtml || buildCachedChannelOptions();

  container.innerHTML = gridSlots.map((slot, idx) => {
    const slotNum = idx + 1;
    const isAudioActive = !slot.isMuted;
    const hasChannel = Boolean(slot.channelName);

    return `
      <div class="grid-slot ${isAudioActive ? 'audio-active' : ''}" id="gridSlot-${idx}">
        <div class="slot-header">
          <div class="slot-title-area">
            <span class="slot-num-badge">#${slotNum}</span>
            <select class="slot-channel-select" id="slotSelect-${idx}" onchange="onSlotSelectChange(${idx}, this.value)">
              ${optionsHtml}
            </select>
          </div>
          <div class="slot-actions">
            <button class="slot-btn ${isAudioActive ? 'active-audio' : ''}" id="btnAudioSlot-${idx}" title="Bật/Tắt âm thanh" onclick="toggleSlotAudio(${idx})">
              ${isAudioActive ? '🔊' : '🔇'}
            </button>
            <button class="slot-btn" title="Tải lại luồng" onclick="reloadSlot(${idx})">
              🔄
            </button>
            <button class="slot-btn" title="Mở VLC" onclick="openSlotInVlc(${idx})">
              🎬
            </button>
            <button class="slot-btn" title="Xóa ô" onclick="clearSlot(${idx})">
              ✕
            </button>
          </div>
        </div>
        <div class="slot-video-wrapper" onclick="onSlotVideoClick(${idx})" ondblclick="toggleSlotFullscreen(${idx})">
          <video class="slot-video" id="slotVideo-${idx}" playsinline muted></video>
          <div class="slot-status-overlay" id="slotStatus-${idx}" style="display: ${hasChannel ? 'flex' : 'none'};">
            <span id="slotStatusText-${idx}">${slot.status === 'ALIVE' ? '🟢 Live' : '⏳ Đang tải...'}</span>
          </div>
          <div class="slot-empty-placeholder" id="slotPlaceholder-${idx}" style="display: ${hasChannel ? 'none' : 'flex'};" onclick="focusSlotSelect(${idx})">
            <span style="font-size: 24px;">📺</span>
            <span>Bấm để chọn kênh #${slotNum}</span>
          </div>
          <div class="slot-vlc-overlay" id="slotVlcOverlay-${idx}" style="display: none;">
            <div style="font-size: 24px;">🎬</div>
            <div class="text-sm font-semibold">Cần mở bằng VLC</div>
            <button class="btn btn-primary btn-sm mt-4" onclick="openSlotInVlc(${idx})">Mở VLC</button>
          </div>
        </div>
      </div>
    `;
  }).join("");

  // Đồng bộ giá trị dropdown và khởi động video lại cho các slot đang có kênh
  gridSlots.forEach((slot, idx) => {
    const sel = document.getElementById(`slotSelect-${idx}`);
    if (sel && slot.channelName) {
      sel.value = slot.channelName;
      startSlotPlayback(idx, slot.channelName);
    }
  });
}

function focusSlotSelect(idx) {
  const sel = document.getElementById(`slotSelect-${idx}`);
  if (sel) sel.focus();
}

function onSlotSelectChange(idx, channelName) {
  if (!channelName) {
    clearSlot(idx);
    return;
  }
  loadSlotChannel(idx, channelName);
}

function onSlotVideoClick(idx) {
  toggleSlotAudio(idx);
}

function toggleSlotFullscreen(idx) {
  const slotEl = document.getElementById(`gridSlot-${idx}`);
  if (!slotEl) return;
  if (!document.fullscreenElement) {
    if (slotEl.requestFullscreen) slotEl.requestFullscreen();
    else if (slotEl.webkitRequestFullscreen) slotEl.webkitRequestFullscreen();
  } else {
    if (document.exitFullscreen) document.exitFullscreen();
  }
}

function loadSlotChannel(idx, channelName) {
  const ch = allChannels.find(c => c.name === channelName);
  if (!ch || !ch.streams || ch.streams.length === 0) {
    showToast(`Kênh ${channelName} không có luồng phát`, true);
    return;
  }

  const streamUrl = ch.best_stream ? ch.best_stream.url : ch.streams[0].url;
  gridSlots[idx].channelName = ch.name;
  gridSlots[idx].streamUrl = streamUrl;
  gridSlots[idx].status = "LOADING";

  const placeholder = document.getElementById(`slotPlaceholder-${idx}`);
  if (placeholder) placeholder.style.display = "none";

  const statusOverlay = document.getElementById(`slotStatus-${idx}`);
  const statusText = document.getElementById(`slotStatusText-${idx}`);
  if (statusOverlay) statusOverlay.style.display = "flex";
  if (statusText) statusText.textContent = "⏳ Đang kết nối...";

  startSlotPlayback(idx, ch.name);
}

function startSlotPlayback(idx, channelName, forceProxy = false) {
  const slot = gridSlots[idx];
  if (!slot || !slot.streamUrl) return;

  const video = document.getElementById(`slotVideo-${idx}`);
  const vlcOverlay = document.getElementById(`slotVlcOverlay-${idx}`);
  const statusText = document.getElementById(`slotStatusText-${idx}`);
  if (!video) return;

  // Hủy instance HLS cũ
  if (slot.hls) {
    slot.hls.destroy();
    slot.hls = null;
  }

  video.pause();
  video.removeAttribute("src");
  video.load();
  video.muted = slot.isMuted;

  if (vlcOverlay) vlcOverlay.style.display = "none";

  // Kiểm tra nếu cần VLC
  if (isVlcRequired(slot.streamUrl)) {
    if (vlcOverlay) vlcOverlay.style.display = "flex";
    if (statusText) statusText.textContent = "📡 Cần VLC";
    slot.status = "VLC";
    return;
  }

  const finalUrl = forceProxy ? `/api/proxy_stream?url=${encodeURIComponent(slot.streamUrl)}` : slot.streamUrl;

  if (window.Hls && Hls.isSupported()) {
    const hls = new Hls({
      enableWorker: true,
      lowLatencyMode: true,
      maxBufferLength: 6,
      maxMaxBufferLength: 12,
      maxBufferSize: 30 * 1000 * 1000
    });

    slot.hls = hls;
    hls.loadSource(finalUrl);
    hls.attachMedia(video);

    hls.on(Hls.Events.MANIFEST_PARSED, () => {
      slot.status = "ALIVE";
      if (statusText) statusText.textContent = forceProxy ? "🟢 Live (Proxy)" : "🟢 Live";
      video.play().catch(e => console.log(`Slot ${idx} autoplay:`, e));
    });

    hls.on(Hls.Events.ERROR, (event, data) => {
      if (data.fatal) {
        if (!forceProxy) {
          // Thử lại qua proxy CORS
          if (slot.hls) {
            slot.hls.destroy();
            slot.hls = null;
          }
          startSlotPlayback(idx, channelName, true);
        } else {
          slot.status = "DEAD";
          if (statusText) statusText.textContent = "🔴 Lỗi luồng";
          if (vlcOverlay) vlcOverlay.style.display = "flex";
        }
      }
    });
  } else if (video.canPlayType("application/vnd.apple.mpegurl")) {
    video.src = finalUrl;
    video.addEventListener("loadedmetadata", () => {
      slot.status = "ALIVE";
      if (statusText) statusText.textContent = "🟢 Live";
      video.play().catch(e => console.log(`Slot ${idx} play:`, e));
    });
  }
}

function toggleSlotAudio(idx) {
  const slot = gridSlots[idx];
  if (!slot) return;

  const willBeMuted = !slot.isMuted;

  if (!willBeMuted) {
    // Solo audio: Tắt tiếng TẤT CẢ các slot khác trước
    gridSlots.forEach((s, i) => {
      s.isMuted = true;
      const v = document.getElementById(`slotVideo-${i}`);
      if (v) v.muted = true;
      const slotEl = document.getElementById(`gridSlot-${i}`);
      if (slotEl) slotEl.classList.remove("audio-active");
      const btn = document.getElementById(`btnAudioSlot-${i}`);
      if (btn) {
        btn.classList.remove("active-audio");
        btn.textContent = "🔇";
      }
    });

    // Bật tiếng ô này
    slot.isMuted = false;
    const currentVideo = document.getElementById(`slotVideo-${idx}`);
    if (currentVideo) currentVideo.muted = false;
    const currentSlotEl = document.getElementById(`gridSlot-${idx}`);
    if (currentSlotEl) currentSlotEl.classList.add("audio-active");
    const currentBtn = document.getElementById(`btnAudioSlot-${idx}`);
    if (currentBtn) {
      currentBtn.classList.add("active-audio");
      currentBtn.textContent = "🔊";
    }
    showToast(`🔊 Mở âm thanh ô #${idx + 1}: ${slot.channelName || 'Không tên'}`);
  } else {
    // Tắt tiếng ô này
    slot.isMuted = true;
    const currentVideo = document.getElementById(`slotVideo-${idx}`);
    if (currentVideo) currentVideo.muted = true;
    const currentSlotEl = document.getElementById(`gridSlot-${idx}`);
    if (currentSlotEl) currentSlotEl.classList.remove("audio-active");
    const currentBtn = document.getElementById(`btnAudioSlot-${idx}`);
    if (currentBtn) {
      currentBtn.classList.remove("active-audio");
      currentBtn.textContent = "🔇";
    }
  }
}

function reloadSlot(idx) {
  const slot = gridSlots[idx];
  if (!slot || !slot.channelName) return;
  showToast(`Đang tải lại ô #${idx + 1}...`);
  startSlotPlayback(idx, slot.channelName, false);
}

function clearSlot(idx) {
  const slot = gridSlots[idx];
  if (!slot) return;

  if (slot.hls) {
    slot.hls.destroy();
    slot.hls = null;
  }

  const video = document.getElementById(`slotVideo-${idx}`);
  if (video) {
    video.pause();
    video.removeAttribute("src");
    video.load();
  }

  slot.channelName = "";
  slot.streamUrl = "";
  slot.status = "EMPTY";
  slot.isMuted = true;

  const sel = document.getElementById(`slotSelect-${idx}`);
  if (sel) sel.value = "";

  const slotEl = document.getElementById(`gridSlot-${idx}`);
  if (slotEl) slotEl.classList.remove("audio-active");

  const btnAudio = document.getElementById(`btnAudioSlot-${idx}`);
  if (btnAudio) {
    btnAudio.classList.remove("active-audio");
    btnAudio.textContent = "🔇";
  }

  const placeholder = document.getElementById(`slotPlaceholder-${idx}`);
  if (placeholder) placeholder.style.display = "flex";

  const statusOverlay = document.getElementById(`slotStatus-${idx}`);
  if (statusOverlay) statusOverlay.style.display = "none";

  const vlcOverlay = document.getElementById(`slotVlcOverlay-${idx}`);
  if (vlcOverlay) vlcOverlay.style.display = "none";
}

function openSlotInVlc(idx) {
  const slot = gridSlots[idx];
  if (!slot || !slot.streamUrl) {
    showToast("Ô này chưa có luồng phát", true);
    return;
  }
  openInVlc(slot.streamUrl);
}

function quickFillGrid(preset) {
  if (!allChannels || allChannels.length === 0) {
    showToast("Danh sách kênh chưa tải xong, vui lòng thử lại sau giây lát", true);
    return;
  }

  const targetCount = gridSlots.length;
  let candidates = [];

  if (preset === "alive") {
    candidates = allChannels
      .filter(c => c.best_stream && c.best_stream.last_status === "ALIVE")
      .sort((a, b) => (a.best_stream.latency_ms || 9999) - (b.best_stream.latency_ms || 9999));
  } else if (preset === "vtv") {
    const isMajor = name => {
      const n = name.toUpperCase();
      return n.startsWith("VTV") || n.startsWith("HTV") || n.startsWith("VTC") || n.startsWith("THVL");
    };
    candidates = allChannels
      .filter(c => isMajor(c.name))
      .sort((a, b) => {
        const aAlive = a.best_stream && a.best_stream.last_status === "ALIVE" ? 1 : 0;
        const bAlive = b.best_stream && b.best_stream.last_status === "ALIVE" ? 1 : 0;
        return bAlive - aAlive || a.name.localeCompare(b.name);
      });
  } else if (preset === "sports") {
    const isSports = c => {
      const n = (c.name + " " + (c.group || "")).toLowerCase();
      return n.includes("thể thao") || n.includes("sport") || n.includes("bóng đá") || n.includes("k+");
    };
    candidates = allChannels.filter(c => isSports(c));
  }

  if (candidates.length === 0) {
    candidates = allChannels.slice(0, targetCount);
  }

  showToast(`⚡ Đang nạp ${Math.min(targetCount, candidates.length)} kênh vào MultiView...`);

  for (let i = 0; i < targetCount; i++) {
    if (i < candidates.length) {
      const ch = candidates[i];
      const sel = document.getElementById(`slotSelect-${i}`);
      if (sel) sel.value = ch.name;
      loadSlotChannel(i, ch.name);
    } else {
      clearSlot(i);
    }
  }
}

function toggleMuteAll() {
  gridSlots.forEach((s, idx) => {
    s.isMuted = true;
    const v = document.getElementById(`slotVideo-${idx}`);
    if (v) v.muted = true;
    const slotEl = document.getElementById(`gridSlot-${idx}`);
    if (slotEl) slotEl.classList.remove("audio-active");
    const btn = document.getElementById(`btnAudioSlot-${idx}`);
    if (btn) {
      btn.classList.remove("active-audio");
      btn.textContent = "🔇";
    }
  });
  showToast("🔇 Đã tắt tiếng tất cả các ô");
}

function reloadAllGridSlots() {
  showToast("🔄 Đang tải lại toàn bộ các ô...");
  gridSlots.forEach((s, idx) => {
    if (s.channelName) {
      startSlotPlayback(idx, s.channelName, false);
    }
  });
}

function clearAllGridSlots() {
  if (!confirm("Bạn có chắc chắn muốn xóa toàn bộ các ô trong MultiView?")) return;
  gridSlots.forEach((_, idx) => clearSlot(idx));
  showToast("🗑️ Đã làm sạch toàn bộ MultiView");
}

function toggleGridFullscreen() {
  const container = document.getElementById("gridviewWall");
  if (!container) return;

  if (!document.fullscreenElement) {
    if (container.requestFullscreen) {
      container.requestFullscreen();
    } else if (container.webkitRequestFullscreen) {
      container.webkitRequestFullscreen();
    }
  } else {
    if (document.exitFullscreen) {
      document.exitFullscreen();
    }
  }
}
