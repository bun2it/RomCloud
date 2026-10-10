// main.js — "não" trình duyệt của RC-Web, code do RomCloud tự viết.
// Electron kiosk toàn màn hình chạy trên Weston (xem launch.sh).
// Bố cục cửa sổ: header (tiêu đề trang) + thanh URL ở trên, trang web
// (thẻ <webview> trong cùng 1 cửa sổ) ở giữa, footer gợi ý phím ở
// dưới — viewport của trang luôn chừa 3 dải này.
// RomCloud (C++) đưa URL mở đầu qua biến RC_WEB_URL / file
// /tmp/rc-web-start-url, và deep-link các URL sau qua CDP :9222
// (PUT /json/new?<URL>). Phím chức năng do gptokeyb gửi (browser.gptk):
//   F2 = trang chủ   F3 = bàn phím ảo   F6 = sửa địa chỉ (bật/tắt)
//   F7/F8 = lui/tới trang (D-pad trái/phải), L1/R1 = PageUp/PageDown.
//   B thoát phiên do RomCloud lo.
//
// TRACE: mọi mốc đều in 1 dòng [WEBTRACE] Jxx ra stdout/stderr, được
// launch.sh gom vào rc-web.log. Mã bước: B=bridge (debug.log),
// L=launcher, J=file này — đối chiếu được phiên chết ở lớp nào.
'use strict';
const { app, BrowserWindow, ipcMain, session } = require('electron');
const fs = require('fs');
const path = require('path');

const HOME_URL = 'https://duckduckgo.com/';
const MOBILE_UA = 'Mozilla/5.0 (Linux; Android 13; Mobile) ' +
  'AppleWebKit/537.36 (KHTML, like Gecko) Chrome/108.0.0.0 Mobile Safari/537.36';
const ZOOM_STEPS = [0, 0.5, 1, 1.5, 2];
const HEADER_H = 58;   // header kiểu RomCloud (trong dải 106px trên)
const URLBAR_H = 48;   // thanh URL kiểu RomCloud (pill 36)
const FOOTER_H = 48;   // footer kiểu RomCloud

// ── Trace ────────────────────────────────────────────────────────────
const T0 = Date.now();
function TR(step, msg) {
  console.log('[WEBTRACE] ' + step + ' +' + (Date.now() - T0) +
              'ms ' + msg);
}
function TRW(step, msg) {
  console.error('[WEBTRACE] ' + step + ' +' + (Date.now() - T0) +
                'ms ' + msg);
}
process.on('uncaughtException', (err) => {
  TRW('J99', 'uncaughtException: ' + (err && err.stack || err));
});
process.on('unhandledRejection', (err) => {
  TRW('J99', 'unhandledRejection: ' + (err && err.stack || err));
});

// CDP cho RomCloud deep-link (launcher cũng truyền cờ này trên argv;
// appendSwitch ở đây là lớp dự phòng, chạy trước whenReady).
app.commandLine.appendSwitch('remote-debugging-port', '9222');
app.commandLine.appendSwitch('remote-debugging-address', '127.0.0.1');

// Chạy thẳng trên Wayland của weston, KHÔNG qua Xwayland: lớp cửa sổ
// X11 của weston 9 có lỗi assert (weston_wm_handle_map_request) làm
// weston chết ngay khi Electron hiện cửa sổ đầu tiên.
app.commandLine.appendSwitch('ozone-platform', 'wayland');
app.commandLine.appendSwitch('enable-features', 'UseOzonePlatform');

function startUrl() {
  for (const a of process.argv.slice(1)) {
    if (/^https?:\/\//i.test(a)) return a;
  }
  if (process.env.RC_WEB_URL && /^https?:\/\//i.test(process.env.RC_WEB_URL)) {
    return process.env.RC_WEB_URL;
  }
  try {
    const s = fs.readFileSync('/tmp/rc-web-start-url', 'utf8')
      .split('\n')[0].trim();
    if (/^https?:\/\//i.test(s)) return s;
  } catch (e) { /* chưa có file -> dùng trang chủ */ }
  return HOME_URL;
}

let page = null;       // cửa sổ vỏ (shell): header + URL + webview + footer
let pageWc = null;     // webContents của trang (webview hoặc cửa sổ CDP)
let kbOpen = false;    // bàn phím ảo (lớp phủ trong vỏ) đang mở
let urlHistory = [];   // lịch sử địa chỉ/tìm kiếm [{q,url}] (tối đa 20)
let zoomIdx = 0;
let urlEdit = false;   // đang sửa địa chỉ trên thanh URL (START bật/tắt)

function esc(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
}
function escAttr(s) {
  return esc(s).replace(/"/g, '&quot;');
}

function focusPage() {
  if (pageWc && !pageWc.isDestroyed()) pageWc.focus();
  shellJs('window.__rcFocus && window.__rcFocus()');
}

// Chế độ mobile: giả lập màn hình điện thoại (ngang 412px) phóng đầy
// khung webview -> trang web phục vụ bản mobile nhẹ CPU, chữ vẫn to rõ.
// Mốc J34/J35 trong log cho biết viewport thật của trang trước/sau.
function applyMobileMode(wc) {
  if (wc.__rcMobile) return;
  wc.__rcMobile = true;
  // Khung web cố định trên máy này: cửa sổ toàn màn 1024x768, webview
  // = phần còn lại sau header + URL + footer (đã đo đúng 1024x664).
  const vw = 1024;
  const vh = 768 - (HEADER_H + URLBAR_H + FOOTER_H);  // = 614 theo tọa độ Tai chốt
  // Tai chốt: viewport = khung web / 2 rồi phóng x2 (nhẹ CPU):
  // trang thấy 512x307, ảnh phóng lên đầy khung 1024x614.
  const mw = Math.round(vw / 2);
  const mh = Math.round(vh / 2);
  const dsf = 2;
  TR('J34', 'ép mobile (type=' + (wc.getType ? wc.getType() : '?') +
     '): khung web ' + vw + 'x' + vh + ' -> mobile ' + mw + 'x' + mh);
  // enableDeviceEmulation không ăn trên webview (đo lại vẫn 1024)
  // -> ép qua giao thức CDP: trang luôn thấy mình là điện thoại ở
  // mọi lớp tín hiệu (UA, client hints, viewport, cảm ứng).
  try {
    if (!wc.debugger.isAttached()) wc.debugger.attach('1.3');
    wc.debugger.sendCommand('Emulation.setUserAgentOverride', {
      userAgent: MOBILE_UA,
      userAgentMetadata: {
        brands: [
          { brand: 'Chromium', version: '108' },
          { brand: 'Google Chrome', version: '108' }
        ],
        fullVersionList: [
          { brand: 'Chromium', version: '108.0.5359.215' },
          { brand: 'Google Chrome', version: '108.0.5359.215' }
        ],
        platform: 'Android', platformVersion: '13',
        architecture: '', model: 'TrimUI Brick', mobile: true
      }
    }).then(() => {
      TR('J35', 'CDP: đã ép UA + client hints mobile');
    }).catch((e) => TRW('J35', 'setUserAgentOverride lỗi: ' + e));
    // scale + dontSetVisibleSize: giữ nguyên khung webview 1024x664
    // mà phóng ảnh mobile lên đầy khung; thiếu 2 tham số này thì
    // Chromium thu cả khung nhìn về 412x267 ở góc trái trên.
    wc.debugger.sendCommand('Emulation.setDeviceMetricsOverride', {
      width: mw, height: mh, deviceScaleFactor: 1, mobile: true,
      screenWidth: mw, screenHeight: mh,
      scale: dsf, dontSetVisibleSize: true
    }).then(() => {
      TR('J35', 'CDP: đã ép viewport mobile ' + mw + 'x' + mh +
         ' phóng x' + dsf.toFixed(2));
    }).catch((e) => TRW('J35', 'setDeviceMetricsOverride lỗi: ' + e));
    wc.debugger.sendCommand('Emulation.setTouchEmulationEnabled', {
      enabled: true, maxTouchPoints: 1
    }).catch(() => {});
    // Giả lập tín hiệu 'tiết kiệm' như điện thoại: trang sẽ ưu
    // tiên ảnh tĩnh thay cho thumbnail động (video/gif tự chạy)
    // và Chromium tắt bớt hiệu ứng chuyển động.
    wc.debugger.sendCommand('Emulation.setEmulatedMedia', {
      media: '',
      features: [
        { name: 'prefers-reduced-motion', value: 'reduce' },
        { name: 'prefers-reduced-data', value: 'reduce' }
      ]
    }).then(() => {
      TR('J45', 'CDP: đã bật reduced-motion + reduced-data');
    }).catch((e) => TRW('J45', 'setEmulatedMedia lỗi: ' + e));
    wc.debugger.sendCommand('Network.enable', {})
      .then(() => wc.debugger.sendCommand(
        'Network.setExtraHTTPHeaders',
        { headers: { 'Save-Data': 'on' } }))
      .then(() => TR('J45', 'CDP: đã gửi header Save-Data'))
      .catch((e) => TRW('J45', 'Save-Data lỗi: ' + e));
  } catch (e) {
    TRW('J35', 'gắn debugger mobile lỗi: ' + e);
  }
}

// Kiểm tra sau khi trang vào: viewport còn là mobile không; nếu lần
// điều hướng đầu làm rơi ép (đổi tiến trình renderer) thì ép lại.
function verifyMobile(wc) {
  setTimeout(() => {
    if (wc.isDestroyed()) return;
    wc.executeJavaScript('window.innerWidth')
      .then((w) => {
        TR('J35', 'kiểm tra viewport trang: innerWidth=' + w);
        if (w >= 1000) {
          wc.__rcMobile = false;
          TRW('J35', 'ép mobile bị mất -> ép lại');
          applyMobileMode(wc);
        }
      })
      .catch((e) => TRW('J35', 'kiểm tra viewport lỗi: ' + e));
  }, 1500);
}

// ── Vỏ trình duyệt (khung RomCloud + web ở giữa) ───────────────────
// Tai chốt tọa độ (gốc dưới-trái): web từ (0,48) cao 614 tới (0,662),
// bề ngang trọn 1024. Quy ra gốc trên-trái: header 0..58, thanh URL
// 58..106, web 106..720, footer 720..768. Header/URL/footer vẽ kiểu
// RomCloud, hiển thị CÙNG LÚC với trang web.
function buildShellHtml(startUrl) {
  return '<!doctype html><meta charset="utf-8">' +
  '<style>' +
  '*{margin:0;padding:0;box-sizing:border-box}' +
  'html,body{height:100%}' +
  'body{background:#111418;color:#e6ebf2;font-family:sans-serif;' +
  'display:flex;flex-direction:column;overflow:hidden}' +
  '#hd{height:' + HEADER_H + 'px;display:flex;align-items:center;' +
  'padding:0 24px;font-size:20px;font-weight:bold;color:#3fc1e8;' +
  'white-space:nowrap;overflow:hidden;text-overflow:ellipsis;flex:none}' +
  '#url{height:' + URLBAR_H + 'px;background:#10121a;flex:none;' +
  'padding:6px 16px}' +
  '#pill{height:36px;background:#161a24;border:1px solid #323746;' +
  'border-radius:18px;display:flex;align-items:center;padding:0 16px}' +
  '#urltxt{font-size:30px;color:#b4b4b4;white-space:nowrap;' +
  'overflow:hidden;text-overflow:ellipsis}' +
  '#ld{color:#f59e0b;font-size:20px;margin-left:10px;display:none}' +
  '#urlin{display:none;width:100%;background:transparent;border:0;' +
  'outline:0;color:#ffd76a;font-size:30px}' +
  '#wv{flex:1;min-height:0;width:100%;border:0;background:#ffffff}' +
  '#ft{height:' + FOOTER_H + 'px;display:flex;align-items:center;' +
  'padding:0 14px;font-size:15px;color:#8a94a6;background:#111418;' +
  'white-space:nowrap;overflow:hidden;flex:none}' +
  '#kb{position:absolute;left:0;right:0;bottom:0;height:300px;' +
  'background:#111418;border-top:1px solid #323746;display:none;' +
  'z-index:30;padding:8px 0}' +
  '#kbhist{display:none;gap:6px;padding:2px 8px 8px;overflow:hidden}' +
  '.chip{flex:none;background:#1c222d;border:1px solid #323746;' +
  'border-radius:16px;color:#9fb4d8;font-size:16px;' +
  'padding:8px 14px;white-space:nowrap}' +
  '.chip.sel{background:#0e5a7a;border-color:#3fc1e8;color:#fff}' +
  '.krow{display:flex;gap:6px;padding:3px 8px}' +
  '.key{flex:1;text-align:center;background:#1c222d;' +
  'border:1px solid #323746;border-radius:10px;color:#e6ebf2;' +
  'font-size:22px;padding:9px 0}' +
  '.key.sel{background:#0e5a7a;border-color:#3fc1e8;color:#fff}' +
  '#ptr{display:none;position:absolute;width:48px;height:48px;' +
  'border:3px solid #3fc1e8;border-radius:50%;z-index:40;' +
  'pointer-events:none;background:rgba(63,193,232,0.12)}' +
  '#ptr:before{content:"";position:absolute;left:50%;top:50%;' +
  'width:6px;height:6px;margin:-3px 0 0 -3px;border-radius:50%;' +
  'background:#3fc1e8}' +
  '</style>' +
  '<div id="hd">Trình duyệt web</div>' +
  '<div id="url"><div id="pill"><span id="urltxt"></span>' +
  '<span id="ld">[Đang tải...]</span>' +
  '<input id="urlin" type="text" spellcheck="false"></div></div>' +
  '<webview id="wv" src="' + escAttr(startUrl) + '"></webview>' +
  '<div id="ft">Trái: Lui &nbsp;·&nbsp; Phải: Tới &nbsp;·&nbsp; ' +
  'Lên/Xuống: Cuộn nhẹ &nbsp;·&nbsp; L1/R1: Cuộn trang &nbsp;·&nbsp; ' +
  'START: Địa chỉ &nbsp;·&nbsp; SELECT: Chuột &nbsp;·&nbsp; B: Thoát</div>' +
  '<div id="kb"><div id="kbhist"></div><div id="kbgrid"></div></div>' +
  '<div id="ptr"></div>' +
  '<scr' + 'ipt>' +
  'var _h=document.getElementById("hd"),' +
  '_t=document.getElementById("urltxt"),' +
  '_l=document.getElementById("ld"),' +
  '_i=document.getElementById("urlin"),' +
  '_p=document.getElementById("pill"),' +
  '_w=document.getElementById("wv");' +
  '_w.addEventListener("new-window",function(e){' +
  'e.preventDefault();_w.loadURL(e.url);});' +
  'window.__rcSet=function(t,u,l){' +
  '_h.textContent=t||"Trình duyệt web";_t.textContent=u||"";' +
  '_l.style.display=l?"inline":"none"};' +
  'window.__rcEdit=function(on,u){' +
  'if(on){_t.style.display="none";_l.style.display="none";' +
  '_i.style.display="block";_p.style.borderColor="#3fc1e8";' +
  '_i.value=u||"";_i.select();_i.focus();}' +
  'else{_i.style.display="none";_t.style.display="";' +
  '_p.style.borderColor="#323746";}};' +
  'window.__rcType=function(s){_i.value+=s};' +
  'window.__rcBack=function(){_i.value=_i.value.slice(0,-1)};' +
  'window.__rcUrl=function(){return _i.value};' +
  'window.__rcFocus=function(){try{_w.focus()}catch(e){}};' +
  'window.__rcFit=function(){_w.style.width=window.innerWidth+"px";' +
  '_w.style.height=(window.innerHeight-' + (HEADER_H + URLBAR_H + FOOTER_H) + ')+"px"};' +
  'window.addEventListener("resize",window.__rcFit);' +
  'window.addEventListener("load",function(){' +
  'window.__rcFit();window.__rcFocus()});window.__rcFit();' +
  'window.__rcKbRows=[' +
  '["1","2","3","4","5","6","7","8","9","0"],' +
  '["q","w","e","r","t","y","u","i","o","p"],' +
  '["a","s","d","f","g","h","j","k","l"],' +
  '["z","x","c","v","b","n","m",".","-","_"],' +
  '["/","@","XÓA","CÁCH"]];' +
  'window.__rcKb={open:false,row:1,col:0,hist:[],onHist:false,hcol:0};' +
  'window.__rcKbRender=function(){' +
  'var s=window.__rcKb,' +
  'kb=document.getElementById("kb");' +
  'kb.style.display=s.open?"block":"none";' +
  'if(!s.open)return;' +
  'var hs=document.getElementById("kbhist");hs.innerHTML="";' +
  's.hist.forEach(function(h,i){' +
  'var c=document.createElement("div");' +
  'c.className="chip"+(s.onHist&&s.hcol===i?" sel":"");' +
  'c.textContent=h;hs.appendChild(c)});' +
  'hs.style.display=s.hist.length?"flex":"none";' +
  'var g=document.getElementById("kbgrid");g.innerHTML="";' +
  'window.__rcKbRows.forEach(function(row,ri){' +
  'var r=document.createElement("div");r.className="krow";' +
  'row.forEach(function(k,ci){' +
  'var d=document.createElement("div");' +
  'd.className="key"+(!s.onHist&&s.row===ri&&s.col===ci?" sel":"");' +
  'if(k==="CÁCH")d.style.flex="3";' +
  'd.textContent=k;r.appendChild(d)});g.appendChild(r)});};' +
  'window.__rcKbSet=function(open,hist){' +
  'var s=window.__rcKb;s.open=!!open;' +
  'if(hist)s.hist=hist;' +
  'if(open){s.onHist=false;s.row=1;s.col=0;}' +
  'window.__rcKbRender();return s.open};' +
  'window.__rcKbMove=function(dir){' +
  'var s=window.__rcKb,rows=window.__rcKbRows;' +
  'if(!s.open)return;' +
  'if(dir==="Left"||dir==="Right"){' +
  'var d=dir==="Left"?-1:1;' +
  'if(s.onHist){s.hcol=(s.hcol+d+s.hist.length)%s.hist.length;}' +
  'else{var n=rows[s.row].length;s.col=(s.col+d+n)%n;}}' +
  'else if(dir==="Up"){' +
  'if(s.onHist){s.onHist=false;}' +
  'else if(s.row>0){s.row--;' +
  's.col=Math.min(s.col,rows[s.row].length-1);}' +
  'else if(s.hist.length){s.onHist=true;s.hcol=0;}}' +
  'else if(dir==="Down"){' +
  'if(s.onHist){s.onHist=false;s.row=0;}' +
  'else if(s.row<rows.length-1){s.row++;' +
  's.col=Math.min(s.col,rows[s.row].length-1);}}' +
  'window.__rcKbRender()};' +
  'window.__rcKbPress=function(){' +
  'var s=window.__rcKb;if(!s.open)return null;' +
  'if(s.onHist)return{t:"hist",i:s.hcol};' +
  'return{t:"key",k:window.__rcKbRows[s.row][s.col]}};' +
  'window.__rcPtr={on:false,x:512,y:413};' +
  'window.__rcPtrRender=function(){' +
  'var s=window.__rcPtr,p=document.getElementById("ptr");' +
  'p.style.left=(s.x-24)+"px";p.style.top=(s.y-24)+"px"};' +
  'window.__rcPtrSet=function(on){' +
  'var s=window.__rcPtr;s.on=!!on;' +
  'document.getElementById("ptr").style.display=' +
  's.on?"block":"none";' +
  'if(s.on){s.x=window.innerWidth/2;s.y=413;' +
  'window.__rcPtrRender();}' +
  'return s.on};' +
  'window.__rcPtrMove=function(dx,dy){' +
  'var s=window.__rcPtr;if(!s.on)return;' +
  's.x=Math.max(0,Math.min(window.innerWidth,s.x+dx));' +
  's.y=Math.max(0,Math.min(window.innerHeight,s.y+dy));' +
  'window.__rcPtrRender()};' +
  'window.__rcPtrPos=function(){' +
  'var s=window.__rcPtr,r=_w.getBoundingClientRect();' +
  'return{x:s.x,y:s.y,wx:s.x-r.left,wy:s.y-r.top,' +
  'inWeb:(s.x>=r.left&&s.x<r.right&&s.y>=r.top&&s.y<r.bottom)}};' +
  'document.addEventListener("mousemove",function(e){' +
  'var s=window.__rcPtr;if(!s.on)return;' +
  's.x=e.clientX;s.y=e.clientY;window.__rcPtrRender()});' +
  '</scr' + 'ipt>';
}

function shellJs(code) {
  if (!page || page.isDestroyed()) return Promise.resolve(null);
  return page.webContents.executeJavaScript(code).catch(() => null);
}

function updateShell(loading) {
  if (urlEdit) return;  // đang sửa địa chỉ: không ghi đè thanh URL
  if (!page || page.isDestroyed() || !pageWc || pageWc.isDestroyed()) {
    return;
  }
  const js = 'window.__rcSet && window.__rcSet(' +
    JSON.stringify(pageWc.getTitle() || '') + ',' +
    JSON.stringify(pageWc.getURL() || '') + ',' +
    (loading ? 'true' : 'false') + ')';
  page.webContents.executeJavaScript(js).catch(() => {});
}

// ── Lịch sử địa chỉ / tìm kiếm ─────────────────────────────────────
// Lưu ở userData (conf/profile trên thẻ SD) nên qua phiên vẫn còn.
// Hiện thành hàng chip trên đầu bàn phím ảo khi sửa địa chỉ.
function historyPath() {
  try { return path.join(app.getPath('userData'), 'rc-history.json'); }
  catch (e) { return ''; }
}
function loadHistory() {
  try {
    const arr = JSON.parse(fs.readFileSync(historyPath(), 'utf8'));
    if (Array.isArray(arr)) {
      urlHistory = arr.filter((h) => h && h.q && h.url).slice(0, 20);
    }
  } catch (e) { /* chưa có file lịch sử: bỏ qua */ }
  TR('J37', 'lịch sử địa chỉ: ' + urlHistory.length + ' mục');
}
function saveHistory() {
  try { fs.writeFileSync(historyPath(), JSON.stringify(urlHistory)); }
  catch (e) { TRW('J37', 'lưu lịch sử lỗi: ' + e); }
}
function pushHistory(q, url) {
  urlHistory = urlHistory.filter((h) => h.url !== url);
  urlHistory.unshift({ q: q, url: url });
  if (urlHistory.length > 20) urlHistory.length = 20;
  saveHistory();
}

// ── Bàn phím ảo (lớp phủ trong vỏ, ghim đáy màn hình) ─────────────
// D-pad di chuyển ô sáng, A (Enter) = bấm phím, Y (F3) bật/tắt.
// Đang sửa địa chỉ thì chữ vào ô URL; bình thường chữ vào trang.
// Đóng bàn phím giữa chừng lúc sửa địa chỉ = hủy sửa.
function setKb(open) {
  if (!open && urlEdit) {
    urlEdit = false;
    shellJs('window.__rcEdit && window.__rcEdit(false)');
    TR('J26', 'hủy sửa địa chỉ (đóng bàn phím)');
    updateShell(false);
  }
  kbOpen = !!open;
  if (kbOpen && ptrOn) setPtr(false);
  shellJs('window.__rcKbSet && window.__rcKbSet(' +
    (kbOpen ? 'true' : 'false') + ',' +
    JSON.stringify(urlHistory.map((h) => h.q)) + ')');
  writeUiMode();
  TR('J20', kbOpen ? 'mở bàn phím ảo' : 'đóng bàn phím ảo');
  if (!kbOpen) focusPage();
}
function toggleKeyboard() { setKb(!kbOpen); }
function kbType(s) {
  if (urlEdit) {
    shellJs('window.__rcType && window.__rcType(' +
      JSON.stringify(s) + ')');
  } else if (pageWc && !pageWc.isDestroyed()) {
    pageWc.insertText(s);
  }
}
function kbBack() {
  TR('J40', 'Y xóa lùi (urlEdit=' + urlEdit + ')');
  if (urlEdit) {
    shellJs('window.__rcBack && window.__rcBack()');
  } else if (pageWc && !pageWc.isDestroyed()) {
    pageWc.sendInputEvent({ type: 'keyDown', keyCode: 'Backspace' });
    pageWc.sendInputEvent({ type: 'keyUp', keyCode: 'Backspace' });
  }
}
function kbEnter() {
  if (urlEdit) { commitUrlEdit(); return; }
  if (pageWc && !pageWc.isDestroyed()) {
    pageWc.sendInputEvent({ type: 'keyDown', keyCode: 'Enter' });
    pageWc.sendInputEvent({ type: 'keyUp', keyCode: 'Enter' });
  }
  setKb(false);
}
function kbLoadHistory(i) {
  const it = urlHistory[i];
  if (!it) return;
  TR('J37', 'mở lại từ lịch sử: ' + it.url);
  urlEdit = false;
  shellJs('window.__rcEdit && window.__rcEdit(false)');
  setKb(false);
  if (pageWc && !pageWc.isDestroyed()) pageWc.loadURL(it.url);
  focusPage();
}
function kbRoute(key) {
  if (key === 'Enter') {
    shellJs('window.__rcKbPress && window.__rcKbPress()')
      .then((tok) => {
        TR('J40', 'A chọn phím ảo: ' + JSON.stringify(tok) +
          ' urlEdit=' + urlEdit);
        if (!tok) {
          TRW('J40', 'A không lấy được phím từ vỏ');
          return;
        }
        if (tok.t === 'hist') { kbLoadHistory(tok.i); return; }
        if (tok.k === 'XÓA') kbBack();
        else if (tok.k === 'XONG') kbEnter();
        else kbType(tok.k === 'CÁCH' ? ' ' : tok.k);
      });
    return;
  }
  shellJs('window.__rcKbMove && window.__rcKbMove(' +
    JSON.stringify(key) + ')');
}

// ── Con trỏ chuột (SELECT bật/tắt) ─────────────────────────────────
// Vòng tròn 48px (= x2 con trỏ thường) vẽ ngay trong vỏ. D-pad di
// chuyển, A = bấm vào trang, SELECT hoặc B = tắt. Gậy analog rê con
// trỏ hệ thống của weston thì vòng tròn bám theo vị trí chuột thật
// (sự kiện mousemove ở phía vỏ).
let ptrOn = false;
function writeUiMode() {
  // RomCloud đọc /tmp/rc-web-ui-mode khi gặp nút B: lớp phụ (bàn
  // phím/con trỏ) đang mở thì B chỉ đóng lớp đó, chưa thoát phiên
  // (B = lùi lại 1 tiến trình).
  try {
    fs.writeFileSync('/tmp/rc-web-ui-mode',
      kbOpen ? 'kb' : (ptrOn ? 'pointer' : ''));
  } catch (e) { /* không ghi được thì B thoát thẳng — chấp nhận */ }
}
function setPtr(on) {
  if (on && kbOpen) return;  // bàn phím đang mở: SELECT không tác dụng
  ptrOn = !!on;
  // Con trỏ = chuột hệ thống của weston (joystick rê), không vẽ
  // vòng riêng trong vỏ nữa (Tai chốt 2026-10-10).
  writeUiMode();
  TR('J38', ptrOn ? 'bật chế độ con trỏ (SELECT)' : 'tắt con trỏ');
}
function togglePtr() { setPtr(!ptrOn); }
// Electron có thể báo phím mũi tên kiểu 'ArrowUp' thay vì 'Up' —
// chuẩn hoá về một kiểu cho cả bàn phím lẫn con trỏ.
function ptrNormKey(key) {
  return key === 'F7' ? 'Left' : key === 'F8' ? 'Right'
    : key === 'ArrowUp' ? 'Up' : key === 'ArrowDown' ? 'Down'
    : key === 'ArrowLeft' ? 'Left' : key === 'ArrowRight' ? 'Right'
    : key;
}
function ptrRoute(key) {
  if (key !== 'Enter') return;  // D-pad: không gán ở chế độ con trỏ
  // A = bấm chuột trái tại vị trí chuột hệ thống của weston.
  // Trang tự ghi vị trí chuột cuối (tracker gắn ở dom-ready) theo
  // toạ độ trang; điều hướng qua CDP Input cùng hệ toạ độ đó nên
  // không phải đoán hệ số phóng.
  if (!pageWc || pageWc.isDestroyed()) return;
  pageWc.executeJavaScript(
    'window.__rcMouse ? [window.__rcMouse.x, window.__rcMouse.y,' +
    ' window.innerWidth, window.innerHeight] : null')
    .then((v) => {
      if (!pageWc || pageWc.isDestroyed()) return;
      const x = (v && v[0] != null) ? v[0] : (v ? v[2] / 2 : 0);
      const y = (v && v[1] != null) ? v[1] : (v ? v[3] / 2 : 0);
      TR('J42', 'A bấm chuột tại (' + Math.round(x) + ',' +
        Math.round(y) + ')' + (v ? '' : ' — chưa có vị trí chuột,' +
        ' dùng tâm trang'));
      // Bấm chuột vào trang = có ý định xem: mở khóa tải video 30s
      // cho lần phát này (tầng mạng chặn ở J46).
      allowMediaUntil = Date.now() + 30000;
      try {
        if (!pageWc.debugger.isAttached()) {
          pageWc.debugger.attach('1.3');
        }
        pageWc.debugger.sendCommand('Input.dispatchMouseEvent', {
          type: 'mousePressed', x: x, y: y, button: 'left',
          clickCount: 1 });
        pageWc.debugger.sendCommand('Input.dispatchMouseEvent', {
          type: 'mouseReleased', x: x, y: y, button: 'left',
          clickCount: 1 });
      } catch (e) {
        TRW('J42', 'bấm chuột lỗi: ' + e);
      }
    })
    .catch((e) => TRW('J42', 'đọc vị trí chuột lỗi: ' + e));
}

// ── Sửa địa chỉ bằng START ─────────────────────────────────────────
// START (F6 qua gptokeyb) lần 1: thanh URL thành ô nhập + mở bàn phím
// ảo, chữ gõ dồn vào ô địa chỉ. START lần nữa (hoặc phím XONG trên
// bàn phím ảo): tải địa chỉ vừa gõ + lưu vào lịch sử.
function normalizeUrl(s) {
  s = String(s || '').trim();
  if (!s) return '';
  if (!/^[a-z][a-z0-9+.-]*:/i.test(s)) {
    s = (s.indexOf('.') >= 0 && s.indexOf(' ') < 0)
      ? 'https://' + s
      : 'https://duckduckgo.com/?q=' + encodeURIComponent(s);
  }
  return s;
}

function enterUrlEdit() {
  urlEdit = true;
  const cur = (pageWc && !pageWc.isDestroyed()) ? pageWc.getURL() : '';
  shellJs('window.__rcEdit && window.__rcEdit(true,' +
    JSON.stringify(cur || '') + ')');
  if (!kbOpen) setKb(true);
  TR('J26', 'START: mở sửa địa chỉ + bàn phím');
}

function commitUrlEdit() {
  shellJs('window.__rcUrl ? window.__rcUrl() : ""').then((v) => {
    TR('J40', 'START tải trang: ô địa chỉ=' + JSON.stringify(v));
    const url = normalizeUrl(v);
    urlEdit = false;
    shellJs('window.__rcEdit && window.__rcEdit(false)');
    setKb(false);
    if (url && pageWc && !pageWc.isDestroyed()) {
      pushHistory(String(v || '').trim(), url);
      TR('J27', 'START: tải địa chỉ mới: ' + url);
      pageWc.loadURL(url);
    } else {
      updateShell(false);
    }
    focusPage();
  });
}

function cancelUrlEdit() {
  if (!urlEdit) return;
  urlEdit = false;
  shellJs('window.__rcEdit && window.__rcEdit(false)');
  TR('J26', 'hủy sửa địa chỉ');
  updateShell(false);
}

function toggleUrlEdit() {
  if (urlEdit) commitUrlEdit(); else enterUrlEdit();
}

// Phím chức năng gắn cho nội dung trang (webview + cửa sổ do CDP tạo).
function attachPageKeys(wc) {
  if (wc.__rcKeys) return;
  wc.__rcKeys = true;
  wc.on('before-input-event', (event, input) => {
    if (input.type !== 'keyDown') return;
    if (kbOpen) TR('J41', 'phím thô trong bàn phím: ' +
      JSON.stringify(input.key));
    {
      // Chuẩn hoá tên phím mũi tên: Electron có thể báo 'ArrowUp'
      // thay vì 'Up' (đây là lý do D-pad lên/xuống từng chết trong
      // bàn phím trong khi trái/phải qua F7/F8 vẫn chạy).
      const kk = ptrNormKey(input.key);
      const isDir = kk === 'Up' || kk === 'Down' || kk === 'Left' ||
        kk === 'Right' || kk === 'Enter';
      if (ptrOn) {
        if (isDir) { ptrRoute(kk); event.preventDefault(); return; }
        if (input.key === 'F1') {  // B: tắt con trỏ, chưa thoát web
          setPtr(false);
          event.preventDefault();
          return;
        }
      }
      if (kbOpen) {
        if (input.key === 'F2') {  // X: không gán trong bàn phím
          event.preventDefault();
          return;
        }
        if (isDir) { kbRoute(kk); event.preventDefault(); return; }
        if (input.key === 'F3') {  // Y = xóa lùi
          kbBack();
          event.preventDefault();
          return;
        }
        if (input.key === 'F1') {  // B: đóng bàn phím, về lại web
          setKb(false);
          event.preventDefault();
          return;
        }
      }
    }
    switch (input.key) {
      case 'F1':
        // gptokeyb ánh xạ B -> F1; B nay là nút thoát (RomCloud dừng
        // cả phiên) nên F1 ở đây không còn tác dụng.
        event.preventDefault();
        break;
      case 'F2':
        TR('J22', 'phím F2 (X): về trang chủ');
        wc.loadURL(HOME_URL);
        event.preventDefault();
        break;
      case 'F3':
        TR('J23', 'phím F3 (Y): bật/tắt bàn phím ảo');
        toggleKeyboard();
        event.preventDefault();
        break;
      case 'F9':
        TR('J38', 'phím F9 (SELECT): bật/tắt con trỏ');
        togglePtr();
        event.preventDefault();
        break;
      case 'F4':
        zoomIdx = (zoomIdx + 1) % ZOOM_STEPS.length;
        TR('J24', 'phím F4: phóng to mức ' + ZOOM_STEPS[zoomIdx]);
        wc.setZoomLevel(ZOOM_STEPS[zoomIdx]);
        event.preventDefault();
        break;
      case 'F5':
        TR('J25', 'phím F5: tải lại trang');
        wc.reload();
        event.preventDefault();
        break;
      case 'F6':
        TR('J26', 'phím F6 (START): bật/tắt sửa địa chỉ');
        toggleUrlEdit();
        event.preventDefault();
        break;
      case 'F7':
        TR('J28', 'phím F7 (D-pad trái): lui trang');
        if (wc.canGoBack()) wc.goBack();
        event.preventDefault();
        break;
      case 'F8':
        TR('J29', 'phím F8 (D-pad phải): tới trang');
        if (wc.canGoForward()) wc.goForward();
        event.preventDefault();
        break;
    }
  });
}

// Khi focus bàn phím nằm ở nội dung VỎ (thay vì webview), các phím chức
// năng vẫn phải chạy: xử lý tại vỏ rồi chuyển hành động vào trang.
// Nếu webview đang giữ focus thì handler này không nhận sự kiện, nên
// không lo chạy trùng.
function attachShellKeys(wc) {
  if (wc.__rcShellKeys) return;
  wc.__rcShellKeys = true;
  wc.on('before-input-event', (event, input) => {
    if (input.type !== 'keyDown') return;
    if (kbOpen) TR('J41', 'phím thô trong bàn phím (vỏ): ' +
      JSON.stringify(input.key));
    {
      const kk = ptrNormKey(input.key);
      const isDir = kk === 'Up' || kk === 'Down' || kk === 'Left' ||
        kk === 'Right' || kk === 'Enter';
      if (ptrOn) {
        if (isDir) { ptrRoute(kk); event.preventDefault(); return; }
        if (input.key === 'F1') {  // B: tắt con trỏ, chưa thoát web
          setPtr(false);
          event.preventDefault();
          return;
        }
      }
      if (kbOpen) {
        if (input.key === 'F2') {  // X: không gán trong bàn phím
          event.preventDefault();
          return;
        }
        if (isDir) { kbRoute(kk); event.preventDefault(); return; }
        if (input.key === 'F3') {  // Y = xóa lùi
          kbBack();
          event.preventDefault();
          return;
        }
        if (input.key === 'F1') {  // B: đóng bàn phím, về lại web
          setKb(false);
          event.preventDefault();
          return;
        }
      }
    }
    const guest = (pageWc && !pageWc.isDestroyed()) ? pageWc : null;
    switch (input.key) {
      case 'F1':
        event.preventDefault();
        break;
      case 'F2':
        TR('J33', 'phím F2 qua vỏ: về trang chủ');
        if (guest) guest.loadURL(HOME_URL);
        event.preventDefault();
        break;
      case 'F3':
        TR('J33', 'phím F3 qua vỏ: bật/tắt bàn phím ảo');
        toggleKeyboard();
        event.preventDefault();
        break;
      case 'F9':
        TR('J33', 'phím F9 (SELECT) qua vỏ: bật/tắt con trỏ');
        togglePtr();
        event.preventDefault();
        break;
      case 'F6':
        TR('J33', 'phím F6 (START) qua vỏ: sửa địa chỉ');
        toggleUrlEdit();
        event.preventDefault();
        break;
      case 'F7':
        TR('J33', 'phím F7 qua vỏ: lui trang');
        if (guest && guest.canGoBack()) guest.goBack();
        event.preventDefault();
        break;
      case 'F8':
        TR('J33', 'phím F8 qua vỏ: tới trang');
        if (guest && guest.canGoForward()) guest.goForward();
        event.preventDefault();
        break;
      case 'Up': case 'Down': case 'ArrowUp': case 'ArrowDown':
      case 'PageUp': case 'PageDown':
        if (!urlEdit && guest) {
          guest.sendInputEvent({ type: 'keyDown', keyCode: input.key });
          guest.sendInputEvent({ type: 'keyUp', keyCode: input.key });
          event.preventDefault();
        }
        break;
      case 'Enter':
        if (!urlEdit && guest) {
          guest.sendInputEvent({ type: 'keyDown', keyCode: 'Enter' });
          guest.sendInputEvent({ type: 'keyUp', keyCode: 'Enter' });
          event.preventDefault();
        }
        break;
    }
  });
}

// Cửa sổ phụ do CDP /json/new tạo (deep-link khi vỏ đã đóng hẳn): vẫn
// toàn màn hình như cũ, không có header/footer.
function configureWindow(win) {
  if (!win || win.__rcConfigured) return;
  win.__rcConfigured = true;
  win.setMenuBarVisibility(false);
  win.setFullScreen(true);
  TR('J10', 'cấu hình cửa sổ trang (fullscreen, id=' + win.id + ')');
  attachPageKeys(win.webContents);
}

function isShellContents(contents) {
  return contents === (page && page.webContents);
}
function isKeyboardContents(contents) {
  return contents.getURL().endsWith('keyboard.html');
}

app.on('web-contents-created', (event, contents) => {
  contents.setUserAgent(MOBILE_UA);
  if (isShellContents(contents)) return;
  // Ép mobile NGAY khi webview ra đời, trước khung hình đầu tiên:
  // trước đây chờ tới dom-ready (~13s) nên trang mở đầu kiểu desktop
  // tràn khung rồi mới nhảy sang mobile (tốn CPU + nhảy hình).
  if (contents.getType && contents.getType() === 'webview') {
    applyMobileMode(contents);
  }
  contents.on('did-start-loading', () => {
    if (!isKeyboardContents(contents)) {
      TR('J12', 'bắt đầu tải: ' + contents.getURL());
      if (contents === pageWc) updateShell(true);
    }
  });
  contents.on('did-navigate', () => {
    if (contents === pageWc) updateShell(false);
  });
  contents.on('page-title-updated', () => {
    if (contents === pageWc) updateShell(false);
  });
  contents.on('dom-ready', () => { contents.__rcLoadOk = true; });
  contents.on('did-fail-load', (e, errorCode, errorDesc, url, isMainFrame) => {
    TRW('J14', 'did-fail-load code=' + errorCode + ' "' + errorDesc +
        '" url=' + url + ' mainFrame=' + isMainFrame);
    if (!isMainFrame) return;
    if (errorCode === -3) {
      // -3 (ERR_ABORTED): chuyển hướng giữa chừng bị hủy là bình thường,
      // nhưng nếu trang không bao giờ vào được thì khung web trắng mãi.
      // Sau 4s mà chưa dom-ready thì tự tải lại 1 lần cho tự lành.
      if (!contents.__rcRetry && !isKeyboardContents(contents)) {
        contents.__rcRetry = 1;
        setTimeout(() => {
          if (!contents.__rcLoadOk && !contents.isDestroyed()) {
            TR('J14', 'tải bị hủy mà trang chưa vào -> tự tải lại');
            contents.reload();
          }
        }, 4000);
      }
      return;
    }
    if (isKeyboardContents(contents)) return;
    const html = '<!doctype html><meta charset="utf-8">' +
      '<body style="background:#0b0e13;color:#cfd6e4;font-family:sans-serif;' +
      'display:flex;align-items:center;justify-content:center;height:100vh;' +
      'margin:0;text-align:center"><div><h2>Không tải được trang</h2>' +
      '<p style="color:#8a94a6">' + esc(url) + '</p>' +
      '<p>' + esc(errorDesc) + ' (' + errorCode + ')</p>' +
      '<p>Bấm nút B để thoát, hoặc START để sửa địa chỉ.</p></div>';
    contents.loadURL('data:text/html;charset=utf-8,' +
      encodeURIComponent(html));
  });
  contents.on('render-process-gone', (e, details) => {
    TRW('J15', 'renderer CHẾT: ' + (details && details.reason) +
        ' exitCode=' + (details && details.exitCode) +
        ' — tự tải lại trang (nếu renderer bị kill vì RAM, xem ' +
        'SNAP/dmesg trong rc-web.log)');
    if (!contents.isDestroyed()) contents.reload();
  });
  contents.once('dom-ready', () => {
    if (isShellContents(contents)) {
      TR('J13', 'vỏ trình duyệt (khung RomCloud + web) đã sẵn sàng');
      updateShell(true);
      return;
    }
    if (isKeyboardContents(contents)) {
      TR('J20', 'bàn phím ảo đã sẵn sàng');
      return;
    }
    TR('J13', 'dom-ready: ' + contents.getURL());
    attachPageKeys(contents);
    // Ghi vị trí chuột hệ thống trong trang (cho nút A ở chế độ
    // con trỏ biết bấm vào đâu) — gắn lại mỗi lần trang tải xong.
    contents.executeJavaScript(
      'if(!window.__rcMouse){window.__rcMouse=null;' +
      'document.addEventListener("mousemove",function(e){' +
      'window.__rcMouse={x:e.clientX,y:e.clientY}},true)}')
      .catch(() => {});
    // Đóng băng media tự chạy trong trang: video thumbnail chỉ
    // hiện ảnh bìa (poster), không tự phát — như trên điện thoại.
    // Ai muốn xem thì bấm vào, video vẫn phát tay được.
    contents.executeJavaScript(
      '(function(){if(window.__rcFrz)return;window.__rcFrz=1;' +
      'function fz(v){try{v.pause();' +
      'v.removeAttribute("autoplay");v.preload="none"}' +
      'catch(e){}}' +
      'document.querySelectorAll("video").forEach(fz);' +
      'new MutationObserver(function(ms){for(var i=0;i<ms.length;' +
      'i++){var ad=ms[i].addedNodes;for(var j=0;j<ad.length;j++)' +
      '{var n=ad[j];if(n.tagName==="VIDEO")fz(n);' +
      'if(n.querySelectorAll)n.querySelectorAll("video")' +
      '.forEach(fz)}}}).observe(document.documentElement,' +
      '{childList:true,subtree:true})})()')
      .then(() => TR('J45', 'đã đóng băng video tự chạy trong trang'))
      .catch((e) => TRW('J45', 'đóng băng video lỗi: ' + e));
    if (contents.getType && contents.getType() === 'webview') {
      pageWc = contents;
      applyMobileMode(contents);
      verifyMobile(contents);
      focusPage();
    } else if (!pageWc || pageWc.isDestroyed()) {
      pageWc = contents;
      applyMobileMode(contents);
    }
    const win = BrowserWindow.fromWebContents(contents);
    if (win && win !== page) {
      configureWindow(win);
    }
  });
});

function createShell() {
  const url = startUrl();
  TR('J02', 'mở vỏ trình duyệt (khung RomCloud + web 1024x614), URL mở đầu: ' + url);
  page = new BrowserWindow({
    fullscreen: true, frame: false, backgroundColor: '#111418',
    webPreferences: {
      nodeIntegration: false, contextIsolation: true, webviewTag: true
    }
  });
  page.on('closed', () => {
    TR('J30', 'cửa sổ vỏ đã đóng');
    pageWc = null;
    kbOpen = false;
  });
  attachShellKeys(page.webContents);
  page.loadURL('data:text/html;charset=utf-8,' +
    encodeURIComponent(buildShellHtml(url)));
  TR('J02', 'vỏ đã tạo (id=' + page.id + '), webview bắt đầu tải');
}

// (Bàn phím ảo nay là lớp phủ ngay trong vỏ — xem __rcKb ở
// buildShellHtml và setKb/kbRoute ở trên. Các kênh ipcMain kb-* của
// cửa sổ bàn phím riêng ngày trước đã gỡ cùng cửa sổ đó.)

// ── Chặn quảng cáo/tracker cho nhẹ CPU ────────────────────────────
// Mỗi khung quảng cáo là 1 tiến trình render riêng + cả đống JS nền;
// cắt từ gốc thì trang mở nhanh hơn và máy đỡ bị ghì (load từng đo
// 3.5–5 khi mở trang tin). Chỉ chặn tên miền quảng cáo/tracker đã
// biết + vài mẫu URL rõ ràng là quảng cáo.
const AD_HOSTS = [
  'doubleclick.net', 'googlesyndication.com', 'googleadservices.com',
  'googletagservices.com', 'googletagmanager.com', 'google-analytics.com',
  'connect.facebook.net', 'ads.yahoo.com', 'adnxs.com',
  'rubiconproject.com', 'pubmatic.com', 'openx.net', 'criteo.com',
  'taboola.com', 'outbrain.com', 'amazon-adsystem.com',
  'scorecardresearch.com', 'quantserve.com', 'moatads.com',
  'adsrvr.org', 'demdex.net', 'bluekai.com', 'casalemedia.com',
  'lijit.com', 'sovrn.com', 'teads.tv', 'eclick.vn',
  'ads-twitter.com', 'analytics.tiktok.com', 'hotjar.com', 'fullstory.com'
];
let adBlocked = 0;
function isAdUrl(u) {
  let host = '';
  try { host = new URL(u).hostname; } catch (e) { return false; }
  for (const d of AD_HOSTS) {
    if (host === d || host.endsWith('.' + d)) return true;
  }
  return /adsbygoogle|\/pagead\/|\/gampad\/|prebid/.test(u);
}
// ── Chặn tải dữ liệu video ở tầng mạng ───────────────────────────
// Thumbnail dạng video là thủ phạm ngốn CPU lớn nhất trên trang tin:
// chỉ 'pause' trong trang thì dữ liệu vẫn tải về và giải mã ngầm.
// Cắt ngay từ request -> không tải, không giải mã; trang chỉ hiện
// hình bìa tĩnh. Người dùng bấm chuột vào video (chế độ con trỏ)
// thì mở khóa 30s cho lần phát đó (allowMediaUntil, đặt ở J42).
let mediaBlocked = 0;
let allowMediaUntil = 0;
function isMediaReq(details) {
  if (!details || !details.url) return false;
  if (details.resourceType === 'media') return true;
  return /\.(mp4|webm|mov|m4v|m3u8|mpd)(\?|#|$)/i.test(details.url);
}
function installAdBlock() {
  session.defaultSession.webRequest.onBeforeRequest((details, cb) => {
    if (isMediaReq(details) && Date.now() > allowMediaUntil) {
      mediaBlocked++;
      if (mediaBlocked % 20 === 1) {
        TR('J46', 'đã chặn ' + mediaBlocked +
           ' request video (chỉ hiện hình bìa)');
      }
      cb({ cancel: true });
      return;
    }
    if (isAdUrl(details.url)) {
      adBlocked++;
      if (adBlocked % 20 === 1) {
        TR('J36', 'đã chặn ' + adBlocked +
           ' request quảng cáo/tracker');
      }
      cb({ cancel: true });
    } else {
      cb({});
    }
  });
  TR('J36', 'đã bật chặn quảng cáo/tracker (' +
     AD_HOSTS.length + ' tên miền)');
}

TR('J00', 'main.js khởi động, electron ' + process.versions.electron +
          ' chrome ' + process.versions.chrome +
          ' argv=' + process.argv.slice(1).join(' '));
app.whenReady().then(() => {
  TR('J01', 'app.whenReady — tạo vỏ trình duyệt');
  installAdBlock();
  loadHistory();
  writeUiMode();  // dọn cờ lớp phụ sót lại từ phiên chết trước
  createShell();
});
app.on('window-all-closed', () => {
  TR('J31', 'mọi cửa sổ đã đóng -> app.quit() (phiên kết thúc)');
  app.quit();
});
app.on('quit', () => {
  TR('J32', 'app đã quit — tiến trình electron sắp thoát');
});
