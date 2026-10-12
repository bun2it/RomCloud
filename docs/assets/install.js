// Cài 1 chạm qua WebUSB ADB (webadb.js classic).
// Luồng: kết nối máy -> tải zip release mới nhất -> đẩy vào thẻ ->
// bung ra /mnt/SDCARD/Apps -> chmod -> sync. User chỉ bấm 2 nút.
// Hỗ trợ 2 app qua ?app=romcloud|brickdrop (mặc định romcloud).
//
// Quy tắc gesture của WebUSB (quan trọng):
// - requestDevice() (hộp chọn máy) CHỈ được gọi trong gesture của click.
//   Gọi lại ở lần thử sau (sau await/sleep) là chết chắc với lỗi
//   "Must be handling a user gesture".
// - Vì vậy: chọn máy ĐÚNG 1 lần, giữ lại USBDevice; các lần thử sau chỉ
//   open/claim/handshake lại trên cùng handle (không cần gesture mới).
// Tem version hiện ở footer để nhìn là biết đang chạy bản mới hay cache cũ.
// Mỗi lần sửa file này thì đổi chuỗi dưới (ngày + commit ngắn).
var INSTALL_JS_VER = "2026-10-12/usb-fix4";
var APP_INSTALL = {
  romcloud: {
    label: "RomCloud",
    repo: "bun2it/RomCloud",
    zipName: "RomCloud-install.zip",
    chmod: ["Apps/RomCloud/launch.sh", "Apps/RomCloud/bin/RomCloud", "Apps/RomCloud/bin/gamecast_d"],
    verify: "Apps/RomCloud/bin/RomCloud",
    fallbackWin: "installers/cai-dat-windows.bat",
    fallbackMac: "installers/cai-dat-mac.zip"
  },
  brickdrop: {
    label: "BrickDrop",
    repo: "bun2it/Brickdrop",
    zipName: "BrickDrop-install.zip",
    chmod: ["Apps/BrickDrop/launch.sh", "Apps/BrickDrop/bin/brickdrop"],
    verify: "Apps/BrickDrop/bin/brickdrop",
    fallbackWin: "installers/cai-dat-brickdrop-windows.bat",
    fallbackMac: "installers/cai-dat-brickdrop-mac.command"
  }
};
var APP_KEY = ((location.search || "").match(/[?&]app=([a-z]+)/) || [])[1] || "romcloud";
if (!APP_INSTALL[APP_KEY]) APP_KEY = "romcloud";
var CFG = APP_INSTALL[APP_KEY];

var adb = null;
var webusb = null;

function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

// Dọn handle USB cũ của chính trang (bấm Kết nối nhiều lần để lại
// kết nối dở treo cổng). Không dọn được adb ngoài trình duyệt.
async function closeStale() {
  try { if (webusb && webusb.close) await webusb.close(); } catch (e) {}
  webusb = null;
  adb = null;
  try {
    if (navigator.usb && navigator.usb.getDevices) {
      var devs = await navigator.usb.getDevices();
      for (var i = 0; i < devs.length; i++) {
        try { if (devs[i].opened) await devs[i].close(); } catch (e) {}
      }
    }
  } catch (e) {}
}

var logEl = document.getElementById("log");
var btnConn = document.getElementById("btnConn");
var devInfo = document.getElementById("devInfo");

function log(msg, cls) {
  var d = document.createElement("div");
  if (cls) d.className = cls;
  d.textContent = msg;
  logEl.appendChild(d);
  logEl.scrollTop = logEl.scrollHeight;
}

async function shell(cmd) {
  var stream = await adb.shell(cmd);
  var out = await stream.receive();
  return (out || "").toString();
}

// Thanh tiến trình tổng (0-100%): kết nối 5% -> tải zip 30% -> đẩy 70%
// -> bung 78% -> RC-Web (nếu có) 96% -> xong 100%.
function setProgress(pct, text) {
  var w = document.getElementById("progWrap");
  var f = document.getElementById("progFill");
  var t = document.getElementById("progText");
  if (w) w.style.display = "block";
  if (f) f.style.width = Math.max(0, Math.min(100, pct)) + "%";
  if (t && text !== undefined) t.textContent = text;
}

function resetProgress() {
  setProgress(0, "Bắt đầu...");
}

// Tải file có báo % (đọc stream). Không đo được dung lượng thì tải
// thường (vẫn xong, chỉ không có % chi tiết).
async function fetchBlobProgress(url, onp) {
  var resp = await fetch(url);
  if (!resp.ok) throw new Error("HTTP " + resp.status);
  var total = +(resp.headers.get("Content-Length") || 0);
  if (!resp.body || !resp.body.getReader || !total) {
    var b0 = await resp.blob();
    if (onp) onp(b0.size, total);
    return b0;
  }
  var reader = resp.body.getReader();
  var chunks = [], got = 0;
  for (;;) {
    var r = await reader.read();
    if (r.done) break;
    chunks.push(r.value);
    got += r.value.length;
    if (onp) onp(got, total);
  }
  return new Blob(chunks);
}

// Cập nhật chữ theo app đang cài.
(function () {
  document.title = "Cài đặt 1 chạm — " + CFG.label;
  var b = document.getElementById("brandName");
  if (b) b.innerHTML = CFG.label.toUpperCase() + "<small>CÀI ĐẶT 1 CHẠM</small>";
  var s2 = document.getElementById("step2Title");
  if (s2) s2.textContent = "Cài " + CFG.label + " mới nhất:";
  var s2d = document.getElementById("step2Desc");
  if (s2d) s2d.textContent = "web tự tải bản mới nhất từ GitHub rồi đẩy vào thẻ nhớ, bung ra, cấp quyền, xong báo DONE.";
  var fw = document.getElementById("dlWin");
  if (fw) fw.href = CFG.fallbackWin;
  var fm = document.getElementById("dlMac");
  if (fm) fm.href = CFG.fallbackMac;
  var home = document.getElementById("homeLink");
  if (home) home.href = "./";
  var jsv = document.getElementById("jsVer");
  if (jsv) jsv.textContent = "bản web " + INSTALL_JS_VER;
})();

// Thử mở + claim + handshake adb trên device ĐÃ CHỌN (không gọi
// requestDevice nên không cần gesture mới). Trả true khi xong.
async function tryConnect(device, attempt) {
  try {
    if (attempt > 1) {
      // KHÔNG gọi device.reset(): trên gadget sunxi của Brick, reset làm
      // máy rớt khỏi USB luôn (lần sau open báo disconnected).
      log("Thử lại claim/kết nối lần " + attempt + "/3 (giữ nguyên máy đã chọn)...");
    }
    try { if (device.opened) await device.close(); } catch (e) {}
    await device.open();
    webusb = new Adb.WebUSB.Transport(device);
    // Cho gadget USB ổn định sau open/claim (Brick hay trả
    // "state change in progress" nếu handshake ngay). KHÔNG handshake
    // lại giữa chừng: gọi select/claim lần 2 trên thiết bị đang dở dang
    // làm sunxi rớt USB luôn — thất bại thì để vòng ngoài mở sạch lại.
    await sleep(800);
    log("Đang kết nối adb (duyệt Cho phép trên máy nếu hỏi)...");
    adb = await webusb.connectAdb("host::", function () {
      log("Hãy bấm Cho phép / OK trên màn hình máy Brick.");
    });
    var model = "?";
    try { model = (await shell("getprop ro.product.model")).trim() || "?"; } catch (e) {}
    devInfo.innerHTML = "Đã kết nối: <b>" + model.replace(/</g, "&lt;") + "</b>";
    log("Đã kết nối máy: " + model, "ok");
    return true;
  } catch (e) {
    var msg = (e && e.message ? e.message : String(e));
    if (/user gesture/i.test(msg)) {
      // Không bao giờ tới đây với handle cũ, nhưng chặn để vòng ngoài
      // dừng ngay thay vì loop vô ích.
      log("Trình duyệt hết gesture — bấm nút Kết nối lại.", "err");
      throw e;
    }
    if (/disconnect/i.test(msg)) {
      // Máy rớt khỏi USB giữa chừng (dây/hub chập chờn, Brick reset cổng):
      // thử nữa vô ích, báo đúng bệnh để user kiểm tra phần cứng.
      log("Lần " + attempt + ": máy vừa RỚT khỏi USB — kiểm tra cáp (đổi cáp data xịn), cắm thẳng không qua hub, rồi bấm Kết nối lại.", "err");
      webusb = null;
      adb = null;
      return "gone";
    }
    if (/claimInterface/i.test(msg)) {
      log("Lần " + attempt + " chưa được (cổng đang bị giữ — đã thử gỡ rồi claim lại): " + msg, "err");
    } else {
      log("Lần " + attempt + " chưa được: " + msg, "err");
    }
    webusb = null;
    adb = null;
    try { if (device.opened) await device.close(); } catch (e2) {}
    return false;
  }
}

btnConn.onclick = async function () {
  btnConn.disabled = true;
  if (!("usb" in navigator)) {
    log("Trình duyệt này không hỗ trợ WebUSB. Hãy dùng Chrome/Edge.", "err");
    btnConn.disabled = false;
    return;
  }
  if (typeof Adb === "undefined") {
    log("Chưa tải được thư viện webadb (mất mạng?). Tải lại trang.", "err");
    btnConn.disabled = false;
    return;
  }
  // Lần 1: mở hộp chọn thiết bị — BẮT BUỘC nằm trong gesture của click này.
  await closeStale();
  var picked = null;
  try {
    log("Đang mở chọn thiết bị...");
    var t0 = await Adb.open("WebUSB");
    picked = t0.device;
  } catch (e) {
    var m0 = (e && e.message ? e.message : String(e));
    if (/No device selected|not found/i.test(m0)) {
      log("Bạn chưa chọn thiết bị nào — bấm Kết nối rồi chọn máy Brick.", "err");
    } else if (/user gesture/i.test(m0)) {
      log("Trình duyệt chặn hộp chọn (hết gesture) — bấm nút Kết nối lại.", "err");
    } else {
      log("Không mở được hộp chọn thiết bị: " + m0, "err");
    }
    btnConn.disabled = false;
    return;
  }
  // Thử claim + handshake trên CÙNG handle đã chọn (không cần gesture thêm).
  // tryConnect trả: true = xong, false = lỗi thử lại được, "gone" = máy
  // rớt USB (thử nữa vô ích).
  var ok = false;
  var gone = false;
  try {
    for (var attempt = 1; attempt <= 3 && !ok && !gone; attempt++) {
      var r = await tryConnect(picked, attempt);
      if (r === true) {
        ok = true;
      } else if (r === "gone") {
        gone = true;
      } else if (attempt < 3) {
        await sleep(1500);
      }
    }
  } catch (e) {
    ok = false; // gesture: tryConnect đã log, dừng vòng thử
  }
  if (gone) {
    log("Dừng ở đây: máy không còn trên USB (kiểu cũ). NHƯNG nếu Brick đang mở app USB Storage thì đây là BÌNH THƯỜNG: đổi mode USB làm handle cũ chết — cứ dùng luôn đường chép file (.bat/.command), không cần WebUSB. Muốn dùng WebUSB thì thoát app USB Storage trên Brick rồi bấm Kết nối (chọn lại máy). Ngược lại (không mở gì): đổi cáp data khác, cắm thẳng vào Mac (bỏ hub), rồi bấm Kết nối.", "err");
    await closeStale();
    btnConn.disabled = false;
    return;
  }
  if (!ok) {
    log("Đã tự thử 3 lần không được. Cổng USB đang bị phần mềm khác giữ (adb/driver trên laptop) — Terminal chạy: adb kill-server, rút cáp cắm lại rồi bấm nút Cài đặt lại.", "err");
    await closeStale();
    btnConn.disabled = false;
    return;
  }
  // 1 nút làm hết: kết nối xong là cài luôn, không chờ bấm thêm.
  await doInstall();
  btnConn.disabled = false;
};

// Cài đặt sau khi đã có adb (gọi 1 lần từ nút duy nhất). Trả true/false.
async function doInstall() {
  if (!adb) return false;
  try {
    resetProgress();
    // 1. Release mới nhất
    log("Đang hỏi bản mới nhất...");
    var rel = await (await fetch("https://api.github.com/repos/" + CFG.repo + "/releases/latest")).json();
    // Chỉ lấy gói app, loại gói RC-Web để không tải nhầm (giữ chung cho cả BrickDrop).
    var asset = (rel.assets || []).filter(function (a) { return /\.zip$/.test(a.name) && !/^rc-web-/i.test(a.name); })[0];
    if (!asset) throw new Error("Không thấy file zip trong release " + rel.tag_name);
    log("Bản mới nhất: " + rel.tag_name + " (" + asset.name + ")", "ok");
    setProgress(5, "Đã kết nối — tải " + asset.name);

    // 2. Tải zip (có %)
    log("Đang tải zip (chờ chút)...");
    var blob = await fetchBlobProgress(asset.browser_download_url, function (got, total) {
      var p = total > 0 ? (5 + got * 25 / total) : 15;
      setProgress(p, "Đang tải: " + (got / 1048576).toFixed(1) + "MB" +
        (total > 0 ? " / " + (total / 1048576).toFixed(1) + "MB" : ""));
    });
    log("Đã tải: " + (blob.size / 1048576).toFixed(1) + "MB", "ok");

    // 3. Đẩy lên thẻ nhớ (có %)
    var remoteZip = "/mnt/SDCARD/" + CFG.zipName;
    log("Đang đẩy lên thẻ nhớ...");
    var sync = await adb.sync();
    await sync.push(blob, remoteZip, 420, function (sent, total) {
      var p = 30 + (total > 0 ? sent * 40 / total : 20);
      devInfo.innerHTML = "Đang đẩy: <b>" + Math.floor(sent * 100 / total) + "%</b>";
      setProgress(p, "Đang đẩy lên thẻ: " + Math.floor(sent * 100 / total) + "%");
    });
    await sync.quit();
    log("Đã đẩy xong.", "ok");

    // 4. Bung + quyền + dọn
    log("Đang bung ra Apps/...");
    setProgress(72, "Đang bung ra Apps/...");
    var unzip = await shell("cd /mnt/SDCARD && unzip -o -q " + CFG.zipName + " && echo UNZIP_OK");
    if (unzip.indexOf("UNZIP_OK") < 0) throw new Error("Bung zip lỗi, còn thiếu unzip trên máy?");
    setProgress(75, "Đang cấp quyền...");
    await shell("chmod +x " + CFG.chmod.map(function (p) { return "/mnt/SDCARD/" + p; }).join(" ") + "; sync; echo DONE");
    setProgress(78, "Đang kiểm tra...");
    var check = await shell("ls /mnt/SDCARD/" + CFG.verify + " && rm -f " + remoteZip + " && echo INSTALLED");
    if (check.indexOf("INSTALLED") < 0) throw new Error("Kiểm tra sau cài thất bại.");

    // 4b. RC-Web (chỉ RomCloud): gói trình duyệt riêng, app mới chạy web
    // được. Release nào không có asset rc-web-*.zip thì bỏ qua.
    if (APP_KEY === "romcloud") {
      var rcAsset = (rel.assets || []).filter(function (a) { return /^rc-web-.*\.zip$/.test(a.name); })[0];
      if (!rcAsset) {
        log("Release này chưa có gói RC-Web, bỏ qua (web sẽ báo chưa cài).");
      } else {
        log("Thấy gói RC-Web (" + rcAsset.name + "), đang tải...");
        var rcBlob = await fetchBlobProgress(rcAsset.browser_download_url, function (got, total) {
          var p = total > 0 ? (78 + got * 6 / total) : 81;
          setProgress(p, "Đang tải RC-Web: " + (got / 1048576).toFixed(1) + "MB");
        });
        log("Đã tải RC-Web: " + (rcBlob.size / 1048576).toFixed(1) + "MB", "ok");
        log("Đang đẩy RC-Web lên thẻ nhớ (lâu hơn)...");
        var sync2 = await adb.sync();
        await sync2.push(rcBlob, "/mnt/SDCARD/rcweb.zip", 420, function (sent, total) {
          var p = 84 + (total > 0 ? sent * 12 / total : 6);
          devInfo.innerHTML = "Đang đẩy RC-Web: <b>" + Math.floor(sent * 100 / total) + "%</b>";
          setProgress(p, "Đang đẩy RC-Web: " + Math.floor(sent * 100 / total) + "%");
        });
        await sync2.quit();
        log("Đang bung RC-Web...");
        setProgress(96, "Đang bung RC-Web...");
        var rcUnzip = await shell("mkdir -p /mnt/SDCARD/Apps/RomCloud/rc-web && cd /mnt/SDCARD/Apps/RomCloud/rc-web && unzip -o -q /mnt/SDCARD/rcweb.zip && echo RCWEB_OK");
        if (rcUnzip.indexOf("RCWEB_OK") < 0) throw new Error("Bung gói RC-Web lỗi.");
        await shell("chmod +x /mnt/SDCARD/Apps/RomCloud/rc-web/launch.sh /mnt/SDCARD/Apps/RomCloud/rc-web/electron/electron /mnt/SDCARD/Apps/RomCloud/rc-web/bin/gptokeyb2 /mnt/SDCARD/Apps/RomCloud/rc-web/weston/bin/weston /mnt/SDCARD/Apps/RomCloud/rc-web/weston/bin/seatd /mnt/SDCARD/Apps/RomCloud/rc-web/weston/bin/xkbcomp 2>/dev/null; sync; echo DONE");
        var rcCheck = await shell("ls /mnt/SDCARD/Apps/RomCloud/rc-web/electron/electron && rm -f /mnt/SDCARD/rcweb.zip && echo RCWEB_INSTALLED");
        if (rcCheck.indexOf("RCWEB_INSTALLED") < 0) throw new Error("Kiểm tra RC-Web sau cài thất bại.");
        log("RC-Web xong.", "ok");
      }
    }
    log("XONG! Rút cáp, mở " + CFG.label + " trên máy và dùng.", "ok");
    devInfo.innerHTML = "<b>DONE ✔</b>";
    setProgress(100, "Hoàn tất 100%");
    return true;
  } catch (e) {
    log("LỖI: " + (e && e.message ? e.message : e), "err");
    return false;
  }
}
