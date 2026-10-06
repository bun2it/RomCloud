// RomCloud 1-chạm installer qua WebUSB ADB (webadb.js classic).
// Luồng: kết nối máy -> tải zip release mới nhất -> đẩy vào thẻ ->
// bung ra /mnt/SDCARD/Apps -> chmod -> sync. User chỉ bấm 2 nút.
var adb = null;

var logEl = document.getElementById("log");
var btnConn = document.getElementById("btnConn");
var btnInstall = document.getElementById("btnInstall");
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

btnConn.onclick = async function () {
  try {
    if (!("usb" in navigator)) {
      log("Trình duyệt này không hỗ trợ WebUSB. Hãy dùng Chrome/Edge.", "err");
      return;
    }
    if (typeof Adb === "undefined") {
      log("Chưa tải được thư viện webadb (mất mạng?). Tải lại trang.", "err");
      return;
    }
    log("Đang mở chọn thiết bị...");
    var webusb = await Adb.open("WebUSB");
    log("Đang kết nối adb (duyệt trên máy nếu hỏi)...");
    adb = await webusb.connectAdb("host::", function () {
      log("Hãy bấm Cho phép / OK trên màn hình máy Brick.");
    });
    var model = "?";
    try { model = (await shell("getprop ro.product.model")).trim() || "?"; } catch (e) {}
    devInfo.innerHTML = "Đã kết nối: <b>" + model.replace(/</g, "&lt;") + "</b>";
    log("Đã kết nối máy: " + model, "ok");
    btnInstall.disabled = false;
    btnConn.disabled = true;
  } catch (e) {
    log("Kết nối thất bại: " + (e && e.message ? e.message : e), "err");
  }
};

btnInstall.onclick = async function () {
  if (!adb) return;
  btnInstall.disabled = true;
  try {
    // 1. Release mới nhất
    log("Đang hỏi bản mới nhất...");
    var rel = await (await fetch("https://api.github.com/repos/bun2it/RomCloud/releases/latest")).json();
    var asset = (rel.assets || []).filter(function (a) { return /\.zip$/.test(a.name); })[0];
    if (!asset) throw new Error("Không thấy file zip trong release " + rel.tag_name);
    log("Bản mới nhất: " + rel.tag_name + " (" + asset.name + ")", "ok");

    // 2. Tải zip
    log("Đang tải zip (~54MB, chờ chút)...");
    var blob = await (await fetch(asset.browser_download_url)).blob();
    log("Đã tải: " + (blob.size / 1048576).toFixed(1) + "MB", "ok");

    // 3. Đẩy lên thẻ nhớ
    var remoteZip = "/mnt/SDCARD/RomCloud-install.zip";
    log("Đang đẩy lên thẻ nhớ...");
    var sync = await adb.sync();
    await sync.push(blob, remoteZip, 420, function (sent, total) {
      devInfo.innerHTML = "Đang đẩy: <b>" + Math.floor(sent * 100 / total) + "%</b>";
    });
    await sync.quit();
    log("Đã đẩy xong.", "ok");

    // 4. Bung + quyền + dọn
    log("Đang bung ra Apps/...");
    var unzip = await shell("cd /mnt/SDCARD && unzip -o -q RomCloud-install.zip && echo UNZIP_OK");
    if (unzip.indexOf("UNZIP_OK") < 0) throw new Error("Bung zip lỗi, còn thiếu unzip trên máy?");
    await shell("chmod +x /mnt/SDCARD/Apps/RomCloud/launch.sh /mnt/SDCARD/Apps/RomCloud/bin/RomCloud /mnt/SDCARD/Apps/RomCloud/bin/gamecast_d; sync; echo DONE");
    var check = await shell("ls /mnt/SDCARD/Apps/RomCloud/bin/RomCloud && rm -f /mnt/SDCARD/RomCloud-install.zip && echo INSTALLED");
    if (check.indexOf("INSTALLED") < 0) throw new Error("Kiểm tra sau cài thất bại.");
    log("XONG! Rút cáp, mở RomCloud trên máy và dùng.", "ok");
    devInfo.innerHTML = "<b>DONE ✔</b>";
  } catch (e) {
    log("LỖI: " + (e && e.message ? e.message : e), "err");
    btnInstall.disabled = false;
  }
};
