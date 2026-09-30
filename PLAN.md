# IPTV OSD & Playback Controls — Plan

## Context

**Vấn đề hiện tại:** `startIPTVPlayback()` fork mpv rồi return ngay → `m_mpvPid` chưa được set đúng khi UIManager check `isIPTVPlaying()`. OSD hoàn toàn không hoạt động.

**Nguyên nhân gốc:** Kiến trúc non-blocking với SDL + mpv song song không khả thi trên TrimUI (cả hai cùng ghi `/dev/fb0` → conflict).

**Giải pháp:** Dùng lại kiến trúc blocking đã hoạt động tốt bên **YouTube** và **TikTok**:
- `playChannel()` fork mpv fullscreen rồi **chặn trong while loop** (SDL không render khi mpv chạy)
- Tất cả playback controls + channel list OSD xử lý trong loop đó qua IPC

---

## 1. Viết lại `playChannel` thành blocking (copy từ YouTube)

**File:** `src/iptv/IPTVManager.cpp`

**Cấu trúc mới:**
```
playChannel(channel, initialIndex, customList)
  → stop()                                      // kill existing mpv
  → m_iptvChannelList = customList.empty() ? m_channels : customList
  → m_iptvSelectedIndex = initialIndex
  → m_iptvCurrentIndex = initialIndex
  → fork mpv (--input-ipc-server=/tmp/mpv_iptv.sock --osd-level=3 --fullscreen)
  → wait 2s for socket
  → BLOCKING LOOP:
      - poll mpv exit every 35ms
      - InputManager.update()
      - UP/DOWN: chọn kênh trong danh sách, hiện OSD channel list
      - LEFT/RIGHT: nhảy ±5 kênh, hiện OSD
      - A: chuyển kênh (loadfile IPC), hiện OSD
      - B: stop(), return
      - SELECT: toggle channel list OSD
      - L1/R1: ±8 kênh
  → khi mpv exit → cleanup, reset state
```

**IPC commands dùng trong loop:**
- `{"command":["loadfile","<url>","replace"]}` — chuyển kênh
- `{"command":["cycle","pause"]}` — pause/resume
- `{"command":["seek",N,"relative"]}` — tua
- `{"command":["add","volume",N]}` — âm lượng
- `{"command":["show-text","<text>",duration]}` — OSD text (cần `--osd-level=3`)

**mpv args cho IPTV (copy từ YouTube nhưng thêm `--osd-level=3`):**
```
--input-ipc-server=/tmp/mpv_iptv.sock
--fullscreen
--keepaspect=yes
--hwdec=auto
--vd-lavc-threads=4
--vd-lavc-fast
--vd-lavc-skiploopfilter=nonref
--vd-lavc-framedrop=nonref
--sws-scaler=fast-bilinear --scale=bilinear --dscale=bilinear
--framedrop=vo
--demuxer-max-bytes=16M --demuxer-readahead-secs=5
--audio-buffer=0.5
--terminal=no
--osd-level=3          ← khác YouTube (dùng 1), cần 3 để show-text hoạt động
--osd-font-size=48
--osd-align-x=center
--osd-align-y=center
--osd-color=#FFFFFF
--osd-border-color=#10141E
--osd-border-size=3
--osd-duration=2000
--osd-font=<appRoot>/assets/fonts/font.ttf
--input-conf=<appRoot>/config/input.conf
```

---

## 2. Channel List OSD (horizontal layout)

**Hàm:** `showIPTVChannelOSD()` (đã tồn tại, cần sửa format)

**Layout OSD horizontal:**
- **Top line:** `[GROUP]  ───────────────────────  N kênh`
- **Middle:** 7-9 kênh hiển thị ngang: `<<  03 VTV1  04 VTV2  ▷05 VTV3◁  06 VTV4  07 VTV5  >>`
- **Bottom:** `[L1] -8  [↑↓] Chọn  [A] Phát  [B] Thoát  [R1] +8  [SELECT] Ẩn`

**Kỹ thuật:** Dùng `show-text` với ASS override codes:
- `{\\an8}` — bottom-center alignment
- `{\\fs36}` — font size 36
- `{\\c&HFFD700&}` — gold color cho selected
- `\n` — new line
- `\h` — hard space

**Cập nhật mỗi khi:**
- User bấm UP/DOWN/LEFT/RIGHT/L1/R1
- User bấm A chuyển kênh
- Channel list tự ẩn sau 4 giây không tương tác

---

## 3. Playback Controls OSD

Copy y chang từ YouTube (đã có sẵn):

| Nút | Action | OSD Text |
|-----|--------|----------|
| A | `cycle pause` | ▶ ĐANG PHÁT / ❚❚ TẠM DỪNG |
| LEFT | `seek -10 relative` | ◀◀  -10s |
| RIGHT | `seek +10 relative` | ▶▶  +10s |
| L1 | `seek -60 relative` | ◀◀  -60s |
| R1 | `seek +60 relative` | ▶▶  +60s |
| UP | `add volume 5` | ▲  Âm lượng +5% |
| DOWN | `add volume -5` | ▼  Âm lượng -5% |
| X | `cycle-values video-aspect-override` | Tỉ lệ màn hình |
| Y | `cycle sub` | Phụ đề (CC) |
| START | `cycle-values speed 1.0 1.25 1.5 0.75` | Tốc độ phát |
| SELECT | toggle channel list OSD | (không có text OSD riêng) |

**Note:** YouTube dùng `--osd-level=1` nhưng IPTV cần `--osd-level=3` để `show-text` hiện trên video. Cần test xem `--osd-level=3` có ảnh hưởng seek bar không (có thể thêm `--osd-bar=no`).

---

## 4. Memory Management

**Thêm vào `IPTVManager.h`:**
```cpp
std::unordered_map<std::string, std::string> m_streamUrlCache; // URL đã resolve
size_t m_streamCacheMax = 50;
std::mutex m_streamCacheMutex;
```

**Channel list:** `m_iptvChannelList` là `std::vector<IPTVChannel>` — value type, không cấp phát động. Không cần cleanup đặc biệt.

**Stream URL cache:**
```cpp
// Trước khi switch kênh:
std::string resolved = resolveCappedHlsUrl(channel.url, 720);
// Cache
{ std::lock_guard<std::mutex> lock(m_streamCacheMutex);
  m_streamUrlCache[channel.url] = resolved;
  if (m_streamUrlCache.size() > m_streamCacheMax) {
    // Xóa oldest entry
    m_streamUrlCache.erase(m_streamUrlCache.begin());
  }
}
switchIPTVChannel(resolved); // dùng cached URL
```

**Khi `stop()`:** Reset `m_iptvChannelList.clear()`, không cần giải phóng gì thêm.

---

## 5. Files to Modify

### `src/iptv/IPTVManager.cpp`
- **Xóa** `startIPTVPlayback()`, `switchIPTVChannel()`, `showIPTVChannelOSD()`, `stopIPTVPlayback()` (cũ)
- **Viết lại** `playChannel()` — blocking loop từ đầu (copy cấu trúc từ `playYouTubeVideo()`)
- **Giữ nguyên** `showOverlayIcon()` — dùng cho playback icons
- **Cập nhật** `showIPTVChannelOSD()` — horizontal channel list format

### `src/iptv/IPTVManager.h`
- **Thêm** `m_streamUrlCache`, `m_streamCacheMutex`, `m_streamCacheMax`
- **Thêm** `std::string getCachedOrResolve(const std::string& url)` — resolve + cache
- **Thêm** `bool m_iptvShowChannelList = false` — toggle OSD visibility
- **Xóa** các methods cũ: `startIPTVPlayback`, `switchIPTVChannel`, `stopIPTVPlayback`, `isIPTVPlaying`
- **Xóa** unused fields: `m_lastChannelListRefresh`, `m_channelListVisible`, `m_iptvGroupBarOffset`

### `src/ui/UIManager.cpp`
- **Revert** IPTV_LIST input handler — bỏ code `if (ip)` OSD, quay về flow cũ (gọi `playChannel` → blocking)
- **Bỏ** `showToast` debug
- **Bỏ** file trace logging (`/tmp/iptv_start.log`)

### `config/input.conf` (không đổi — giữ nguyên mpv key bindings)

---

## 6. Implementation Status

- [x] `playChannel()` viết lại thành blocking loop (fork + poll)
- [x] Playback controls: A (pause), LEFT/RIGHT (seek ±10s), L1/R1 (seek ±60s), UP/DOWN (volume), X (aspect), Y (sub), START (speed)
- [x] Channel list OSD: SELECT toggle, UP/DOWN navigate, L1/R1 page jump
- [x] `showIPTVChannelOSD()` horizontal format với ASS styling
- [x] UIManager revert — bỏ non-blocking OSD handler
- [x] Stream URL cache (50-entry LRU) với mutex protection
- [ ] Test thực tế trên máy

---

## 7. Verification

1. Chạy IPTV → chọn kênh → xem video phát
2. Bấm **UP/DOWN** → thấy OSD horizontal channel list hiện
3. Bấm **A** → chuyển kênh mà không restart
4. Bấm **LEFT/RIGHT** → tua ±10s, thấy OSD
5. Bấm **A** khi đang phát → pause/resume, thấy OSD
6. Bấm **B** → dừng, quay về danh sách kênh SDL
7. Test chuyển kênh liên tục nhanh → không lag
