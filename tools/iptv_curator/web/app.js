// ==========================================================================
// RomCloud IPTV Curator - Frontend Reactive Controller
// ==========================================================================

let allChannels = [];
let probePollingTimer = null;

document.addEventListener("DOMContentLoaded", () => {
  initDropZone();
  loadStats();
  loadGroups();
  loadChannels();
  loadRules();
});

// Toast notification
function showToast(message, isError = false) {
  const toast = document.getElementById("toastBox");
  toast.textContent = message;
  toast.style.borderColor = isError ? "var(--accent-red)" : "var(--accent-cyan)";
  toast.classList.add("show");
  setTimeout(() => toast.classList.remove("show"), 3500);
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
      document.getElementById("statTotalStreams").textContent = data.db.total_streams || 0;
      document.getElementById("statDbAlive").textContent = data.db.alive_streams || 0;
      document.getElementById("statDbStandby").textContent = data.db.standby_streams || 0;
      document.getElementById("statDbDead").textContent = data.db.dead_streams || 0;
    }
  } catch (err) {
    console.error("Lỗi tải stats:", err);
  }
}

// 2. API: Load Groups Dropdown
async function loadGroups() {
  try {
    const res = await fetch("/api/groups");
    const data = await res.json();
    const select = document.getElementById("groupSelect");
    select.innerHTML = "";

    data.groups.forEach(grp => {
      const opt = document.createElement("option");
      opt.value = grp;
      opt.textContent = grp;
      select.appendChild(opt);
    });
  } catch (err) {
    console.error("Lỗi tải groups:", err);
  }
}

// 3. API: Load Channels
async function loadChannels() {
  const grp = document.getElementById("groupSelect").value || "";
  const q = document.getElementById("searchInput").value || "";
  const tbody = document.getElementById("channelTableBody");

  tbody.innerHTML = `<tr><td colspan="6" class="text-center py-8 text-dim">Đang tải dữ liệu...</td></tr>`;

  try {
    const url = `/api/channels?group=${encodeURIComponent(grp)}&q=${encodeURIComponent(q)}`;
    const res = await fetch(url);
    const data = await res.json();
    allChannels = data.channels || [];

    renderChannelTable(allChannels);
    loadStats();
  } catch (err) {
    tbody.innerHTML = `<tr><td colspan="6" class="text-center py-8 text-red">Lỗi tải danh sách kênh: ${err}</td></tr>`;
  }
}

function filterChannels() {
  const grp = document.getElementById("groupSelect").value;
  const q = document.getElementById("searchInput").value.toLowerCase().trim();

  const filtered = allChannels.filter(ch => {
    const matchGrp = (!grp || grp === "Tất cả" || ch.group === grp);
    const matchQ = (!q || ch.name.toLowerCase().includes(q) || ch.group.toLowerCase().includes(q));
    return matchGrp && matchQ;
  });

  renderChannelTable(filtered);
}

function renderChannelTable(channels) {
  const tbody = document.getElementById("channelTableBody");
  if (!channels || channels.length === 0) {
    tbody.innerHTML = `<tr><td colspan="6" class="text-center py-8 text-dim">Không có kênh nào phù hợp. Hãy thêm nguồn M3U.</td></tr>`;
    return;
  }

  tbody.innerHTML = channels.map((ch, idx) => {
    const logoHtml = ch.logo
      ? `<img src="${ch.logo}" class="ch-logo" alt="${ch.name}" onerror="this.src='data:image/svg+xml;utf8,<svg xmlns=\'http://www.w3.org/2000/svg\' width=\'36\' height=\'36\' fill=\'%2364748b\'><rect width=\'36\' height=\'36\' rx=\'4\' fill=\'%231c2433\'/><text x=\'50%\' y=\'55%\' dominant-baseline=\'middle\' text-anchor=\'middle\' font-size=\'14\' fill=\'%2394a3b8\'>TV</text></svg>'">`
      : `<div class="ch-logo text-center" style="display:flex;align-items:center;justify-content:center;font-size:12px;color:#94a3b8;">TV</div>`;

    const best = ch.best_stream;
    let statusBadge = `<span class="badge badge-amber">Chưa kiểm tra</span>`;
    let pingText = "";

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

    return `
      <tr>
        <td>${logoHtml}</td>
        <td>
          <div class="ch-name">${ch.name}</div>
        </td>
        <td><span class="badge badge-blue">${ch.group}</span></td>
        <td>${countBadge}</td>
        <td>${statusBadge}</td>
        <td>
          <button class="btn btn-secondary text-sm" onclick="openStreamModal(${idx})">
            🔍 Xem ${ch.stream_count} Nguồn
          </button>
        </td>
      </tr>
    `;
  }).join("");
}

// 4. Modal Chi Tiết Streams
function openStreamModal(channelIdx) {
  const ch = allChannels[channelIdx];
  if (!ch) return;

  document.getElementById("modalChannelTitle").textContent = `${ch.name} (${ch.group})`;
  const body = document.getElementById("modalChannelBody");

  body.innerHTML = ch.streams.map((s, idx) => {
    let badge = `<span class="badge badge-amber">CHƯA PING</span>`;
    if (s.last_status === "ALIVE") {
      badge = `<span class="badge badge-green">🟢 ALIVE (${s.latency_ms}ms)</span>`;
    } else if (s.last_status === "STANDBY") {
      badge = `<span class="badge badge-amber">🟡 STANDBY (Uy tín: ${s.score}đ)</span>`;
    } else if (s.last_status === "DEAD") {
      badge = `<span class="badge badge-red">🔴 DEAD</span>`;
    }

    const isPrimary = (idx === 0);

    return `
      <div class="stream-item">
        <div class="stream-item-header">
          <div>
            <b>Nguồn ${idx + 1}</b> ${isPrimary ? '<span class="badge badge-green text-sm">Ưu tiên số 1</span>' : '<span class="badge badge-blue text-sm">Dự phòng</span>'}
          </div>
          <div>${badge}</div>
        </div>
        <div class="stream-url">${s.url}</div>
        ${s.source ? `<div class="text-sm text-dim mt-4">Nguồn file: ${s.source}</div>` : ""}
      </div>
    `;
  }).join("");

  document.getElementById("streamModal").style.display = "flex";
}

function closeStreamModal() {
  document.getElementById("streamModal").style.display = "none";
}

// 5. Drag & Drop File Upload
function initDropZone() {
  const dropZone = document.getElementById("dropZone");
  const fileInput = document.getElementById("fileInput");

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
        body: jsonSafe({ filename: file.name, content: content })
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
  loadGroups();
  switchTab("tabChannels");
}

// 6. Thêm URL từ xa
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
      body: jsonSafe({ url: url })
    });
    const data = await res.json();
    if (data.success) {
      showToast(`Đã thêm thành công ${data.added_channels} kênh từ URL!`);
      input.value = "";
      loadStats();
      loadGroups();
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
      loadGroups();
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
      loadGroups();
      loadChannels();
    }
  } catch (err) {
    showToast(`Lỗi: ${err}`, true);
  }
}

// 7. Health Check / Ping probe
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

    // Polling progress
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
    }
  } catch (err) {
    console.error("Lỗi polling progress:", err);
  }
}

// 8. Rules Editor
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
      body: jsonSafe({ channel_aliases: chAliases, group_mappings: grpMappings })
    });
    const data = await res.json();
    if (data.success) {
      showToast("Đã lưu quy tắc và cập nhật lại danh sách kênh!");
      loadGroups();
      loadChannels();
    } else {
      showToast("Lỗi khi lưu quy tắc", true);
    }
  } catch (err) {
    showToast(`Lỗi cú pháp JSON: ${err.message}`, true);
  }
}

// 9. Export & OTA Publish
async function exportPlaylist() {
  const includeBackup = document.getElementById("chkIncludeBackup").checked;
  showToast("Đang tạo file live.m3u và iptv_manifest.json...");

  try {
    const res = await fetch("/api/export", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: jsonSafe({ include_backup: includeBackup })
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

function jsonSafe(obj) {
  return JSON.stringify(obj);
}
