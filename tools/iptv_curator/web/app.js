// ==========================================================================
// RomCloud IPTV Curator - Frontend Reactive Controller
// ==========================================================================

let allChannels = [];
let filteredChannels = [];
let currentPage = 1;
let pageSize = 100;
let probePollingTimer = null;

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
  navigator.clipboard.writeText(text).then(() => {
    showToast("Đã sao chép link stream vào bộ nhớ tạm!");
  }).catch(() => {
    // Fallback
    const ta = document.createElement("textarea");
    ta.value = text;
    document.body.appendChild(ta);
    ta.select();
    document.execCommand("copy");
    document.body.removeChild(ta);
    showToast("Đã sao chép link stream!");
  });
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

  // Đếm nhanh số lượng theo các trạng thái trên toàn bộ kho
  let cntAll = allChannels.length;
  let cntAlive = 0;
  let cntDead = 0;
  let cntMulti = 0;

  allChannels.forEach(ch => {
    const st = ch.best_stream ? ch.best_stream.last_status : "UNKNOWN";
    if (st === "ALIVE") cntAlive++;
    if (st === "DEAD") cntDead++;
    if (ch.stream_count > 1) cntMulti++;
  });

  const elAll = document.getElementById("countAll");
  const elAlive = document.getElementById("countAlive");
  const elDead = document.getElementById("countDead");
  const elMulti = document.getElementById("countMulti");
  if (elAll) elAll.textContent = cntAll;
  if (elAlive) elAlive.textContent = cntAlive;
  if (elDead) elDead.textContent = cntDead;
  if (elMulti) elMulti.textContent = cntMulti;

  // Lọc dữ liệu
  filteredChannels = allChannels.filter(ch => {
    // 1. Nhóm
    if (grp && grp !== "Tất cả" && ch.group !== grp) return false;

    // 2. Trạng thái
    const bestSt = ch.best_stream ? ch.best_stream.last_status : "UNKNOWN";
    if (statusFilter === "ALIVE" && bestSt !== "ALIVE") return false;
    if (statusFilter === "DEAD" && bestSt !== "DEAD") return false;
    if (statusFilter === "STANDBY" && (bestSt === "ALIVE" || bestSt === "DEAD")) return false;
    if (statusFilter === "MULTI" && ch.stream_count <= 1) return false;
    if (statusFilter === "SINGLE" && ch.stream_count !== 1) return false;

    // 3. Từ khóa tìm kiếm (tên, nhóm hoặc URL của bất kỳ stream nào)
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
      const pA = (a.best_stream && a.best_stream.last_status === "ALIVE") ? a.best_stream.latency_ms : 99999;
      const pB = (b.best_stream && b.best_stream.last_status === "ALIVE") ? b.best_stream.latency_ms : 99999;
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

  // Đặt lại trang về 1 khi đổi bộ lọc
  currentPage = 1;
  renderCurrentPage();
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
      if (best.last_status === "ALIVE") {
        statusBadge = `<span class="badge badge-green">🟢 ${best.latency_ms} ms</span>`;
      } else if (best.last_status === "STANDBY") {
        statusBadge = `<span class="badge badge-amber">🟡 Chờ (${best.score}đ)</span>`;
      } else if (best.last_status === "DEAD") {
        statusBadge = `<span class="badge badge-red">🔴 Lỗi</span>`;
      }
    }

    const countBadge = ch.stream_count > 1
      ? `<span class="badge badge-purple">${ch.stream_count} luồng</span>`
      : `<span class="badge badge-blue">1 luồng</span>`;

    const safeNameAttr = encodeURIComponent(ch.name);

    return `
      <tr>
        <td>${logoHtml}</td>
        <td>
          <div class="ch-name">${escapeHtml(ch.name)}</div>
        </td>
        <td><span class="badge badge-blue">${escapeHtml(ch.group)}</span></td>
        <td>${countBadge}</td>
        <td>${statusBadge}</td>
        <td style="text-align: right;">
          <button class="btn btn-secondary text-sm" onclick="openStreamModal('${safeNameAttr}')">
            🔍 Xem ${ch.stream_count} Nguồn
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
    let badge = `<span class="badge badge-amber" id="badge-stream-${idx}">CHƯA PING</span>`;
    if (s.last_status === "ALIVE") {
      badge = `<span class="badge badge-green" id="badge-stream-${idx}">🟢 ALIVE (${s.latency_ms}ms)</span>`;
    } else if (s.last_status === "STANDBY") {
      badge = `<span class="badge badge-amber" id="badge-stream-${idx}">🟡 STANDBY (${s.score}đ)</span>`;
    } else if (s.last_status === "DEAD") {
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
          <button class="btn-action" onclick="copyToClipboard('${escapeHtml(s.url)}')">
            📋 Sao chép link
          </button>
          <button class="btn-action btn-action-probe" id="btn-probe-${idx}" onclick="testSingleStream('${safeChannelName}', '${safeGroupName}', '${safeUrl}', ${idx})">
            ⚡ Test Ping ngay
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

// 7. Test Ping Đơn Lẻ Ngay Trong Modal
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
      // Cập nhật lại đối tượng trong allChannels
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
      // Cập nhật lại bảng ngoài
      renderCurrentPage();
    } else {
      showToast(data.error || "Lỗi kiểm tra", true);
    }
  } catch (err) {
    showToast(`Lỗi mạng: ${err}`, true);
  } finally {
    if (btn) {
      btn.disabled = false;
      btn.textContent = "⚡ Test Ping ngay";
    }
  }
}

// 8. Đặt Làm Nguồn Ưu Tiên Số 1
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
      // Đổi thứ tự trong allChannels
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

// 9. Drag & Drop File Upload
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

// 10. Thêm URL từ xa
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

// 11. Health Check / Ping probe
async function startHealthCheck() {
  const btn = document.getElementById("btnStartProbe");
  btn.disabled = true;
  btn.textContent = "⏳ Đang quét...";

  document.getElementById("progressBox").style.display = "block";
  document.getElementById("progressFill").style.width = "0%";
  document.getElementById("progressText").textContent = "Bắt đầu quét đa luồng...";

  try {
    const res = await fetch("/api/health_check", { method: "POST" });
    const data = await res.json();
    if (!data.success) {
      showToast(data.error, true);
      btn.disabled = false;
      btn.textContent = "▶️ Bắt Đầu Quét Toàn Bộ Luồng";
      return;
    }

    if (probePollingTimer) clearInterval(probePollingTimer);
    probePollingTimer = setInterval(pollHealthProgress, 1000);
  } catch (err) {
    showToast(`Lỗi: ${err}`, true);
    btn.disabled = false;
    btn.textContent = "▶️ Bắt Đầu Quét Toàn Bộ Luồng";
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
      document.getElementById("btnStartProbe").disabled = false;
      document.getElementById("btnStartProbe").textContent = "▶️ Bắt Đầu Quét Toàn Bộ Luồng";
      showToast(`Đã hoàn tất kiểm tra: ${data.alive_count} link sống, ${data.dead_count} link lỗi.`);
      loadStats();
      loadChannels();
    }
  } catch (err) {
    console.error("Lỗi polling progress:", err);
  }
}

// 12. Rules Editor
async function loadRules() {
  try {
    const res = await fetch("/api/rules");
    const data = await res.json();
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
      body: JSON.stringify({ channel_aliases: chAliases, group_mappings: grpMappings })
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

// 13. Export & OTA Publish
async function exportPlaylist() {
  const includeBackup = document.getElementById("chkIncludeBackup").checked;
  showToast("Đang tạo file live.m3u và iptv_manifest.json...");

  try {
    const res = await fetch("/api/export", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ include_backup: includeBackup })
    });
    const data = await res.json();
    if (data.success) {
      const m = data.result.manifest;
      document.getElementById("exportSummaryText").textContent =
        `Phiên bản: ${m.version} • ${m.channel_count} kênh chính (${m.total_streams} tổng luồng) • SHA256: ${m.sha256.substring(0, 16)}...`;
      document.getElementById("exportResultBox").style.display = "block";
      showToast("Xuất bản thành công!");
      switchTab("tabExport");
    } else {
      showToast("Lỗi xuất bản playlist", true);
    }
  } catch (err) {
    showToast(`Lỗi: ${err}`, true);
  }
}

async function publishToRomCloud() {
  if (!confirm("Đẩy file live.m3u và manifest trực tiếp vào thư mục RomCloud iptv/ để máy handheld dùng ngay?")) return;
  showToast("Đang xuất bản vào thư mục RomCloud/iptv/...");

  try {
    const res = await fetch("/api/publish_ota", { method: "POST" });
    const data = await res.json();
    if (data.success) {
      showToast("Đã lưu thành công vào thư mục RomCloud/iptv/!");
    } else {
      showToast("Lỗi khi lưu vào RomCloud", true);
    }
  } catch (err) {
    showToast(`Lỗi: ${err}`, true);
  }
}
