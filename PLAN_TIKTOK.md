# Plan: TikTok Module cho RomCloud

## Nguyên tắc

**Cái gì đang chạy tốt thì không sửa.** Cụ thể:

- `IPTVManager::playYouTubeVideo()` — **reuse nguyên vẹn**, chỉ cần truyền direct URL từ TikTok
- `TelexHelper` — standalone utility, **reuse nguyên vẹn**
- mpv, yt-dlp binary, socket IPC — **reuse nguyên vẹn**

**Phải tạo mới:**

- `scripts/tiktok_search.sh` — vì `youtube_search.sh` cứng YouTube URL
- TikTok search state + render — vì YouTube state không reuse được
- TikTok web tab — vì chưa có endpoint

---

## 1. `scripts/tiktok_search.sh` — MỚI

```bash
./tiktok_search.sh search "<query>" [page] [per_page]
./tiktok_search.sh trending [page] [per_page]
./tiktok_search.sh url "<tiktok_url_or_id>"
```

- **search**: `yt-dlp --flat-playlist "ytsearchN:site:tiktok.com <query>"` → output `id|title|author|description` (pipe-separated)
- **trending**: tương tự search, query = "trending"
- **url**: `yt-dlp -g -f best[height<=720] --no-watermark` → trả về direct .mp4 URL

yt-dlp binary: ưu tiên `bin/yt-dlp-glibc` (ARM), fallback `bin/yt-dlp`, fallback system.

Output format:
```
<video_id>|<title (max 80)>|<uploader>|<description (max 120)>
```

---

## 2. `src/iptv/TikTokManager.h` — MỚI

```cpp
namespace RomCloud {
class TikTokManager {
public:
    static TikTokManager& instance();
    std::vector<std::string> search(const std::string& query, int page = 1);
    std::vector<std::string> trending(int page = 1);
    std::string resolveStreamUrl(const std::string& tiktokUrl);
private:
    TikTokManager() = default;
    std::string runScript(const std::string& sub, const std::string& arg,
                          int page = 1, int perPage = 6);
    std::unordered_map<std::string, std::string> m_streamCache; // videoId → streamUrl
};
}
```

## 3. `src/iptv/TikTokManager.cpp` — MỚI

- `search()`: popen `tiktok_search.sh search`, parse pipe-separated lines
- `trending()`: popen `tiktok_search.sh trending`
- `resolveStreamUrl()`: popen `tiktok_search.sh url`, cache kết quả trong `m_streamCache`
- **Không có `play()`** — playback gọi thẳng `IPTVManager::playYouTubeVideo()` nguyên vẹn

---

## 4. `src/ui/UIManager.h` — SỬA

**Thêm enum:**
```cpp
YOUTUBE_RESULTS,
TIKTOK_SEARCH,    // NEW
TIKTOK_RESULTS,   // NEW
EXIT_REQUESTED
```

**Thêm member variables:**
```cpp
// TikTok Search
std::string m_ttSearchQuery;
std::string m_ttLastSearchQuery;
std::vector<std::string> m_ttSearchResults;
int m_ttSearchSelectedIndex = 0;
int m_ttSearchScrollOffset = 0;
int m_ttCurrentPage = 1;
int m_ttKbRow = 0, m_ttKbCol = 0;
bool m_ttKbShift = false;
bool m_ttTelexMode = true;
std::string m_ttErrorMessage;
std::atomic<bool> m_ttIsSearching{false};
std::atomic<bool> m_ttSearchFinished{false};
std::atomic<bool> m_ttVideoReady{false};
std::string m_ttPendingStreamUrl;
std::string m_ttPendingVideoId;
std::string m_ttPendingTitle;
```

**Thêm method declarations:**
```cpp
std::vector<std::string> runTikTokSearch(const std::string& query, int page = 1);
std::vector<std::string> runTikTokTrending(int page = 1);
std::string resolveTikTokStreamUrl(const std::string& tiktokUrl);
void triggerTikTokSearch();
void playTikTokVideo(const std::string& tiktokUrl, const std::string& title);
void renderTikTokSearchState();
void renderTikTokResultsState();
```

---

## 5. `src/ui/UIManager.cpp` — SỬA NHIỀU VỊ TRÍ

**5a. `initGridMenu()`** — thêm TikTok item:
```cpp
{"tiktok", "TIKTOK", "TIKTOK.png", "TikTok"},
```

**5b. `update()` — menu dispatch** — sau `else if (selectedId == "youtube")`:
```cpp
} else if (selectedId == "tiktok") {
    m_ttSearchQuery.clear();
    m_ttSearchResults.clear();
    m_ttSearchSelectedIndex = 0;
    m_ttSearchScrollOffset = 0;
    m_ttKbRow = m_ttKbCol = 0;
    m_ttKbShift = false;
    m_ttTelexMode = true;
    m_ttErrorMessage.clear();
    m_ttIsSearching = m_ttSearchFinished = false;
    m_ttVideoReady = false;
    m_ttPendingStreamUrl.clear();
    setState(UIState::TIKTOK_SEARCH);
}
```

**5c. `update()` — search finished check** (song song với YouTube):
```cpp
if (m_ttSearchFinished.exchange(false)) {
    m_ttIsSearching = false;
    if (!m_ttSearchResults.empty()) {
        if (m_currentState == UIState::TIKTOK_SEARCH)
            setState(UIState::TIKTOK_RESULTS);
        m_ttSearchSelectedIndex = 0;
    } else {
        showToast(m_ttErrorMessage.empty() ? "Khong co ket qua" : m_ttErrorMessage, ...);
    }
}
```

**5d. `update()` — video ready check:**
```cpp
if (m_ttVideoReady.exchange(false)) {
    if (!m_ttPendingStreamUrl.empty()) {
        std::string url = m_ttPendingStreamUrl;
        std::string vid = m_ttPendingVideoId;
        std::string title = m_ttPendingTitle;
        m_ttPendingStreamUrl.clear();
        // REUSE playYouTubeVideo() nguyen van — chi truyen direct URL vao
        IPTVManager::instance().playYouTubeVideo(vid, url, "360");
        setState(UIState::TIKTOK_RESULTS);
    } else {
        showToast("Khong the lay link phat video", ...);
    }
}
```

**5e. `update()` — state switch** — thêm sau YOUTUBE_RESULTS:
```cpp
case UIState::TIKTOK_SEARCH: {
    // Trending: query rong + press A/START → runTrending()
    // Keyboard: reuse TelexHelper::processTelex() — cung ham nhu YouTube
    // Press A → triggerTikTokSearch()
    break;
}
case UIState::TIKTOK_RESULTS: {
    // Grid 3×2, khong thumbnail
    // Card: title + author + desc excerpt
    // [A] → playTikTokVideo(selectedVideoId)
    // [X] → ve TIKTOK_SEARCH
    // [B] → MENU
    break;
}
```

**5f. `render()` dispatch** — thêm:
```cpp
case UIState::TIKTOK_SEARCH:   renderTikTokSearchState();   break;
case UIState::TIKTOK_RESULTS:  renderTikTokResultsState();  break;
```

**5g. Implement các hàm logic:**
- `runTikTokSearch()` — popen `tiktok_search.sh search`
- `runTikTokTrending()` — popen `tiktok_search.sh trending`
- `resolveTikTokStreamUrl()` — popen `tiktok_search.sh url`
- `triggerTikTokSearch()` — thread, set `m_ttSearchFinished=true`
- `playTikTokVideo()` — check cache → thread resolve → set `m_ttVideoReady=true`

**5h. `renderTikTokSearchState()`:**
- Background near-black (#0A0A0A)
- Header bar TikTok icon + "TikTok"
- Input field + 4-row keyboard (TelexHelper::processTelex reuse)
- Query rong → hint "Nhan START de xem Thi Vien"

**5i. `renderTikTokResultsState()`:**
- Grid 3×2, khong thumbnail
- Card: title (2 lines) + author + description excerpt
- Selected card: border highlight

---

## 6. `src/network/WebServer.cpp` — SỬA

**6a. Tab button:**
```html
<button class="tab-btn" onclick="switchTab('tiktok')">TikTok</button>
```

**6b. Tab content panel:**
```html
<div id="tab-tiktok" class="tab-content" style="display:none">
  <input type="text" id="tt-query" placeholder="Tim kiem hoac hashtag..." />
  <button onclick="ttSearch()">Tim</button>
  <button onclick="ttTrending()">Thi vien</button>
  <div id="tt-results"></div>
</div>
```

**6c. JavaScript functions:**
- `ttSearch()` — fetch `/api/tiktok/search?q=<query>`
- `ttTrending()` — fetch `/api/tiktok/trending`
- `renderTtResults(items)` — render card grid
- `ttPlay(id)` — fetch `/api/tiktok/url?url=<id>`

**6d. HTTP handlers:**
```
GET /api/tiktok/search?q=<query>&page=<n>  → tiktok_search.sh search
GET /api/tiktok/trending?page=<n>          → tiktok_search.sh trending
GET /api/tiktok/url?url=<tiktok_url>       → tiktok_search.sh url
```

---

## Thứ tự triển khai

```
Phase 1: scripts/tiktok_search.sh     ← standalone, testable ngay tren dev machine
Phase 2: TikTokManager.h/cpp         ← khong phu thuoc UI
Phase 3: UIManager.h                 ← enum + declarations
Phase 4: UIManager.cpp               ← menu item + update hooks + state + render
Phase 5: WebServer.cpp               ← TikTok web tab
```

---

## Test Plan

**Script-level** (dev machine):
```bash
./scripts/tiktok_search.sh search "viral dance"
./scripts/tiktok_search.sh trending
./scripts/tiktok_search.sh url "https://vm.tiktok.com/..."
```

**E2E on device:**
1. RomCloud → icon TikTok trong carousel
2. A trên TikTok → keyboard screen
3. START (query rong) → Trending feed
4. Go `#funny` → search by hashtag
5. A trên result → mpv phat (dung chung `playYouTubeVideo()`)
6. B → quay ve results
7. Web portal port 8080 → TikTok tab

---

## Các file can sua/tao

| File | Action |
|------|--------|
| `assets/apps_icons/TIKTOK.png` | Da co san |
| `scripts/tiktok_search.sh` | Tao moi |
| `src/iptv/TikTokManager.h` | Tao moi |
| `src/iptv/TikTokManager.cpp` | Tao moi |
| `src/ui/UIManager.h` | Sua — enum + member vars + method declarations |
| `src/ui/UIManager.cpp` | Sua — menu item + update hooks + state switch + render dispatch + logic + render |
| `src/network/WebServer.cpp` | Sua — TikTok web tab + API handlers |
