// preload.js — cầu nối tối thiểu cho cửa sổ bàn phím ảo của RC-Web.
// Chỉ lộ đúng 4 thao tác gõ phím; trang web thường không có preload.
'use strict';
const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('kb', {
  text: (t) => ipcRenderer.send('kb-text', t),
  backspace: () => ipcRenderer.send('kb-backspace'),
  enter: () => ipcRenderer.send('kb-enter'),
  close: () => ipcRenderer.send('kb-close')
});
