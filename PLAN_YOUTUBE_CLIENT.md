# Plan: YouTube Client for TrimUI Brick Pro (Integrated into RomCloud)

## Context

**Tại sao:** Người dùng muốn xem YouTube trên TrimUI Brick Pro thay vì phải dùng điện thoại/không cần rời thiết bị.

**Thay đổi gì:** Thêm tính năng YouTube Client vào RomCloud — một menu item mới trong carousel, màn hình search với on-screen keyboard, kết quả search gọi `mpv` (đã có sẵn) qua `yt-dlp`.

**Output:** User chọn video → `yt-dlp` lấy stream URL (max 720p) → `mpv` phát video trên framebuffer.

---

## Architecture Overview

```
Carousel (MENU state)
    └─ [New] YouTube item
           └─ YouTube Search Screen
                  ├─ On-Screen QWERTY Keyboard (D-pad navigate, A type, B back)
                  ├─ Search Results List (D-pad scroll, A select)
                  └─ Playing State → mpv (fork+exec, same as IPTV)

External components (bundled):
    bin/python3         Static Python3 ARM binary (~8-12 MB)
    bin/yt-dlp         Static yt-dlp ARM binary (~2 MB)
```

---

## 1. Bundle: Python3 + yt-dlp

**File:** `dist/staging/Apps/RomCloud/bin/python3` — static Python 3 ARM64 binary
**File:** `dist/staging/Apps/RomCloud/bin/yt-dlp` — static yt-dlp ARM64 binary

**mpv:** Đã có sẵn trong các bản RomCloud trước (IPTV feature).
- Full package: `mpv` + libs đã nằm trong zip → OK
- Lite installer: `mpv` được fetch qua `mpv_bundle.zip` OTA → cần kiểm tra `mpv_bundle.zip` vẫn được tạo đúng trong `package.sh`

**Python3 + yt-dlp:** Tải pre-built static binaries:
- Python: build static ARM64 hoặc tìm release phù hợp TrimUI
- yt-dlp: `https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp` (Linux ARM64)

**OTA cho Python + yt-dlp:** Thêm vào `mpv_bundle.zip` (hoặc tạo `tools_bundle.zip` riêng) để:
- Fresh install (user chưa từng cài RomCloud): tự động nhận Python + yt-dlp qua OTA sau khi cài Lite
- Existing install: OTA bundle update cũng cập nhật Python + yt-dlp

**Fallback:** Nếu device đã có `python3` hoặc `yt-dlp` ở system path, dùng system binary trước.

---

## 2. New UI State: `YOUTUBE_SEARCH` + `YOUTUBE_RESULTS`

**File to modify:** `src/ui/UIManager.h`, `src/ui/UIManager.cpp`

### 2a. Add `UIState` enum values
```cpp
enum class UIState {
    // ... existing states ...
    YOUTUBE_SEARCH,    // QWERTY keyboard input
    YOUTUBE_RESULTS,   // Search result list
    YOUTUBE_PLAYING,   // (no separate state, mpv runs fullscreen)
};
```

### 2b. On-Screen QWERTY Keyboard

Reuse existing IPTV search keyboard pattern. Layout (3 rows):
```
Row 1: Q W E R T Y U I O P
Row 2:  A S D F G H J K L
Row 3: ↵  Z X C V B N M  ⌫
```

**Input mapping:**
- `D-pad LEFT/RIGHT` → move cursor in current row
- `D-pad UP/DOWN` → switch row
- `A` → confirm/insert character
- `B` → back/cancel (back to YouTube menu or exit)
- `X` → space
- `Y` → clear input
- `START` → execute search

**Search bar** at top: shows typed text, max ~40 chars, real-time display.

### 2c. Search Results List

Fetch from `scripts/youtube_search.py <query>` via `popen()`.

**Display per result:**
```
[thumbnail] Title (up to 2 lines)
            Channel • Duration • Views
```

**Layout:**
- Left 25% width: thumbnail (if available)
- Right 75%: title + metadata
- D-pad UP/DOWN: scroll list (with auto-repeat after 320ms)
- A: play selected video
- B: back to keyboard
- L1/R1: page up/down

**Auto-retry:** If search fails (network), show toast "Network error. Retry?" and auto-retry once after 3s.

---

## 3. Backend: Search Script

**File:** `scripts/youtube_search.py`

### 3a. Script Interface

```bash
# Search
python3 youtube_search.py search "query" [max_results]

# Get stream URL for mpv (max 720p)
python3 youtube_search.py url "<video_id>" [--format "best[height<=720]"]

# Output format (search results):
# One line per result, pipe-separated:
# video_id|title|channel|duration|views
# UTF-8, no encoding issues.
```

### 3b. Implementation

```python
import subprocess, json, sys, re

def search(query, max_results=20):
    cmd = [
        "yt-dlp",
        "--flat-playlist",          # Don't recurse
        "--print", "%(id)s|%(title)s|%(uploader)s|%(duration)s|%(view_count)s",
        f"ytsearch{max_results}:{query}"
    ]
    # Fallback: if flat-playlist not available, use --dump-json + parse
```

**Error handling:**
- `yt-dlp` not found → print `ERROR:NOMODULE` to stderr, exit 1
- Network timeout (30s) → print `ERROR:TIMEOUT` to stderr, exit 1
- No results → print empty, exit 0
- Rate limited → print `ERROR:RATELIMIT` → UI shows "API rate limit, try again later"

### 3c. Stream URL fetch

```python
def get_url(video_id, format_spec="best[height<=720]"):
    cmd = ["yt-dlp", "-g", "-f", format_spec, f"https://youtube.com/watch?v={video_id}"]
    # Returns first line = direct URL
    # If format unavailable, try progressive (has audio+video)
    # If all fail, fall back to worst available
```

---

## 4. Playback: Extend IPTVManager Pattern

**File:** `src/iptv/IPTVManager.cpp` — reuse `playChannel()` pattern, or create new `YouTubePlayer` class.

### 4a. Fork + exec mpv

```cpp
// Similar to IPTVManager::playChannel()
std::vector<std::string> args = {
    playerPath,
    streamUrl,                  // from youtube_search.py url command
    "--fullscreen",
    "--hwdec=auto",
    "--vd-lavc-threads=4",
    "--framedrop=vo",
    "--demuxer-max-bytes=16M",
    "--demuxer-readahead-secs=8",
    "--audio-buffer=0.5"
};

// mpv.conf for YouTube: disable cache warning, hide console
// Write a separate config: config/youtube_mpv.conf
```

### 4b. Player path priority (add Python/yt-dlp paths)

```cpp
std::vector<std::string> playerCandidates = {
    // ... existing mpv paths ...
};
```

**yt-dlp path:**
```cpp
std::string ytDlpPath = findBinary("bin/yt-dlp", {
    appRoot + "/bin/yt-dlp",
    sdRoot + "/System/bin/yt-dlp",
    "/usr/bin/yt-dlp"
});
```

**Python path:**
```cpp
std::string pythonPath = findBinary("bin/python3", {
    appRoot + "/bin/python3",
    sdRoot + "/System/bin/python3",
    "/usr/bin/python3"
});
```

---

## 5. Menu: Add YouTube to Carousel

**File:** `src/ui/UIManager.cpp` — `renderMenuState()`

Add `MenuItem::YOUTUBE` to `MenuItem` enum. Render as icon card with YouTube icon (new PNG asset).

**Menu icon:** `assets/apps_icons/YOUTUBE.png` (existing asset)

Carousel positions: 0=Drive, 1=Local, 2=Settings, 3=IPTV, **4=YouTube**, 5=Diagnostics

When `A` pressed on YouTube item → transition to `YOUTUBE_SEARCH`.

---

## 6. Files to Create / Modify

### New files
| File | Purpose |
|---|---|
| `scripts/youtube_search.py` | Python search + URL fetch script |
| `config/youtube_mpv.conf` | mpv config for YouTube playback |

**Menu icon:** Sử dụng icon có sẵn tại `assets/apps_icons/YOUTUBE.png` (không cần tạo mới).
**App launcher icon:** `assets/apps_icons/YOUTUBE.png` cũng dùng làm icon trong TrimUI launcher grid nếu tạo standalone menu entry.

### Modified files
| File | Change |
|---|---|
| `src/ui/UIManager.h` | Add `YOUTUBE_SEARCH`, `YOUTUBE_RESULTS` to `UIState`; add `MenuItem::YOUTUBE` |
| `src/ui/UIManager.cpp` | Add YouTube carousel item using `assets/apps_icons/YOUTUBE.png`; implement `renderYouTubeSearchState()`, `renderYouTubeResultsState()`; implement `handleYouTubeSearchInput()`, `handleYouTubeResultsInput()` |
| `src/iptv/IPTVManager.cpp` | Add `playYouTubeUrl()` method (fork+exec mpv with YouTube URL) |
| `src/iptv/IPTVManager.h` | Add `playYouTubeUrl(const std::string& url)` declaration |
| `package.sh` | Bundle `bin/python3`, `bin/yt-dlp` into release zips |

### Bundled binaries (add to repo)
| File | Source |
|---|---|
| `bin/python3` | Static ARM64 Python 3 (from release) |
| `bin/yt-dlp` | ARM64 release from github.com/yt-dlp/yt-dlp |

---

## 7. Implementation Order (Step-by-Step)

### Phase 1: Bundle dependencies
1. Download `yt-dlp` ARM64 binary → save to `bin/yt-dlp`
2. Download static `python3` ARM64 binary → save to `bin/python3`
3. Test: `file bin/python3`, `file bin/yt-dlp` confirm ARM64
4. Test: `./bin/yt-dlp --version` confirm executable
5. Verify `mpv` still in `package.sh` bundles (already included for IPTV, no change needed)

### Phase 2: Python search script
5. Write `scripts/youtube_search.py`
6. Test on dev machine: `python3 scripts/youtube_search.py search "minecraft music"`
7. Test URL fetch: `python3 scripts/youtube_search.py url <video_id>`

### Phase 3: UI integration
8. Add `YOUTUBE_SEARCH`, `YOUTUBE_RESULTS` to `UIState` in `UIManager.h`
9. Add YouTube menu item to carousel in `UIManager.cpp`
10. Implement `renderYouTubeSearchState()` with QWERTY keyboard
11. Implement `renderYouTubeResultsState()` with scrollable list
12. Implement input handlers for both states

### Phase 4: Playback integration
13. Add `playYouTubeUrl()` to `IPTVManager.cpp`
14. Wire: A on result → `playYouTubeUrl(streamUrl)`
15. Create `config/youtube_mpv.conf`

### Phase 5: Packaging
16. Update `package.sh` to bundle `bin/python3`, `bin/yt-dlp` (mpv already in bundle, no change needed)
17. Build: `./build.sh && ./package.sh`
18. Deploy to device via SSH
19. E2E test: search → select → watch → back

---

## 8. Verification

1. **Dev machine test** (before device):
   - `python3 scripts/youtube_search.py search "lofi beats"` → returns 20 results
   - `python3 scripts/youtube_search.py url <id>` → returns stream URL
   - `./build.sh` compiles without errors

2. **Device test** (over SSH):
   - `ls /mnt/SDCARD/Apps/RomCloud/bin/` → `python3`, `yt-dlp` present
   - `/mnt/SDCARD/Apps/RomCloud/bin/yt-dlp --version` → version string
   - Launch RomCloud → YouTube icon appears in carousel
   - Press A on YouTube → keyboard screen
   - Type "gameplay" → press START → results load
   - Press A on result → mpv plays video
   - Press B/Menu on mpv → returns to results

3. **Error cases to verify:**
   - No network → toast "Network error"
   - Rate limited → toast "Rate limited, try later"
   - yt-dlp missing → toast "yt-dlp not found"
   - Video unavailable → toast "Video unavailable"

---

## 9. Quality Constraints

- **No new threads blocking main loop** — search runs via `popen()` with timeout
- **Timeout on popen:** 30 seconds max, kill child if exceeds
- **Memory:** results capped at 50 items; thumbnails cached to disk, not in SDL texture cache
- **No network on main UI thread** — already handled by `popen()` async model
- **Consistent button layout** with existing IPTV flow
