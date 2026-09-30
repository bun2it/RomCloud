#pragma once
#include "../database/DatabaseManager.h"
#include "../iptv/IPTVManager.h"
#include "../iptv/TikTokManager.h"
#include "../localsend/LocalSendProtocol.h"
#include "CoverManager.h"
#include "UiTheme.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <atomic>

namespace RomCloud {

enum class UIState {
  MENU,
  SYSTEM_SELECT,
  GAME_LIST,
  SEARCH,
  CONFIRM_DELETE,
  CONFIRM_BATCH_DELETE,
  DISCLAIMER,
  CLOUD_LOGIN,
  SETTINGS,
  DIAGNOSTICS,
  OTA_UPDATE,
  REVERSE_SYNC,
  IPTV_PLAYLIST_SELECT, // Chon playlist truoc khi xem kenh
  IPTV_LIST,
  IPTV_SEARCH,
  YOUTUBE_SEARCH,
  YOUTUBE_RESULTS,
  TIKTOK_SEARCH,
  TIKTOK_RESULTS,
  LOCALSEND_HOME,       // Trang chính LocalSend: devices + pending + send queue
  LOCALSEND_INCOMING,   // Modal duyệt file gửi đến (A=đồng ý, B=từ chối)
  LOCALSEND_FOLDER,     // Chọn folder đích
  LOCALSEND_SEND,       // Chọn file để gửi + danh sách target devices
  LOCALSEND_GAME_PICKER, // Chọn game LOCAL từ DB để gửi (thay vì raw file picker)
  LOCALSEND_PROGRESS,   // Xem progress upload/download
  EXIT_REQUESTED
};

enum class GameFilterMode { ALL = -1, LOCAL_ONLY = 1, CLOUD_ONLY = 0 };

class UIManager {
public:
  static UIManager &instance();
  bool init(SDL_Window *window, SDL_Renderer *renderer);
  void shutdown();
  void update();
  void render();
  void initGridMenu();

  UIState getState() const { return m_currentState; }
  void setState(UIState state);

private:
  UIManager() = default;

  SDL_Window *m_window = nullptr;
  SDL_Renderer *m_renderer = nullptr;
  TTF_Font *m_fontTitle = nullptr;
  TTF_Font *m_fontLarge = nullptr;
  TTF_Font *m_fontMedium = nullptr;
  TTF_Font *m_fontSmall = nullptr;

  UIState m_currentState = UIState::MENU;
  int m_selectedMenuIndex = 0;

  // Menu items as grid icons
  struct GridMenuItem {
      std::string id;
      std::string title;
      std::string iconFile;  // PNG filename in assets/apps_icons/
      std::string subtitle;
  };
  std::vector<GridMenuItem> m_gridMenuItems;

  // IPTV Playlist Browser State (MOI)
  int m_selectedPlaylistIndex = 0;
  int m_playlistScrollOffset  = 0;

  // IPTV Channel List State
  int m_selectedIPTVChannelIndex = 0;

  // LocalSend P2P state
  // m_localSendMode: 0=SEND, 1=RECEIVE
  int m_localSendMode = 0;
  int m_localSendSelectedDevice = 0;
  int m_localSendFolderSelected = 0;
  int m_localSendIncomingFolderIdx = 0;  // 0=auto, còn lại = preset trong dialog duyệt
  std::string m_localSendPendingSessionId;
  LsUploadRequest m_localSendCurrentPrompt;
  int m_localSendProgressSel = 0;     // dong dang chon trong man PROGRESS
  int m_localSendProgressScroll = 0;  // scroll man PROGRESS
  uint32_t m_localSendProgressDoneMs = 0;  // moc tat ca xong (de auto-ve home)
  int m_localSendScrollOffset = 0;
  int m_localSendTab = 0;  // 0=devices, 1=received, 2=send queue (giữ để tương thích)
  int64_t m_localSendLastDeviceRefreshMs = 0;

  // LocalSend: ROM picker — quét thẳng /mnt/SDCARD/Roms (không qua DB)
  struct LsRomEntry {
      std::string path;        // full path file rom
      std::string name;        // basename
      std::string systemDir;   // vd "GBA", "FC"
      uint64_t sizeBytes = 0;
  };
  std::vector<LsRomEntry> m_lsRomList;
  bool m_lsRomListLoaded = false;
  int m_lsRomSelected = 0;
  int m_lsRomScrollOffset = 0;

  // LocalSend: picker tabs 0=File (Roms SD) / 1=Folder (Apps SD) — theo Selection gốc
  int m_lsPickerTab = 0;

  // Folder picker (LOCALSEND_FOLDER + LOCALSEND_INCOMING) — file explorer 2 cột
  std::string m_localSendFolderCurrentPath = "/mnt/SDCARD";
  std::vector<std::string> m_localSendFolderEntries;  // path tuyệt đối của folder con
  bool m_localSendFolderLoaded = false;
  int m_localSendFolderFocus = 0;  // 0 = danh sách folder, 1 = nút Chốt (panel phải)
  // Rename / New-folder keyboard (tai dung layout QWERTY nhu YouTube)
  bool m_lsFolderRenaming = false;
  bool m_lsFolderRenameExisting = false;
  std::string m_lsFolderRenameText;
  std::string m_lsFolderRenameOriginal;
  int m_lsFolderKbRow = 0;
  int m_lsFolderKbCol = 0;
  bool m_lsFolderKbShift = false;
  // LOCALSEND_INCOMING: 2 screen — 0=picker 2 cột, 1=xác nhận cuối
  int m_localSendIncomingMode = 0;  // 0=picker, 1=confirm
  std::string m_localSendIncomingSavePath;  // path tuyệt đối đích lưu file

  // Helper: gợi ý tên folder mới không trùng trong parent (NewFolder, NewFolder_2, ...)
  std::string suggestNewFolderName(const std::string& parentPath);
  void startLsFolderRename(bool existing);
  void commitLsFolderRename();
  void cancelLsFolderRename();
  void renderLsFolderKeyboard();
  bool handleLsFolderKeyboardInput();

  // LocalSend: Apps picker (/mnt/SDCARD/Apps/ drill-down)
  struct LsAppEntry {
      std::string path;        // full path
      std::string name;        // display name
      uint64_t sizeBytes = 0;
      bool isDirectory = false;
  };
  std::vector<LsAppEntry> m_lsAppList;
  std::vector<std::string> m_lsAppBreadcrumb;  // empty = top level (/mnt/SDCARD/Apps)
  int m_lsAppSelected = 0;
  int m_lsAppScrollOffset = 0;
  bool m_lsAppListLoaded = false;

  void renderLsRomsList(int dlgX, int dlgY, int dlgW, int dlgH);
  void renderLsAppsList(int dlgX, int dlgY, int dlgW, int dlgH);
  int m_iptvScrollOffset = 0;
  bool m_iptvShowFavoritesOnly = false;
  // Group filter bar (Loi 2 fix)
  std::string m_iptvSelectedGroup;   // "" = Tat ca
  int m_iptvGroupBarOffset = 0;      // scroll ngang cua group bar
  int m_activePlaylistIndex = -1;    // -1 = tat ca playlists

  // IPTV Search & Virtual Keyboard State
  std::string m_iptvSearchQuery;
  std::vector<IPTVChannel> m_iptvSearchResults;
  int m_iptvSearchSelectedIndex = 0;
  int m_iptvSearchScrollOffset = 0;
  int m_iptvKbRow = 0;
  int m_iptvKbCol = 0;
  bool m_iptvKbInResults = false;

  // YouTube Search State
  std::string m_ytSearchQuery;
  std::string m_ytLastSearchQuery;
  std::vector<std::string> m_ytSearchResults;       // current page results (up to 6)
  std::vector<std::string> m_ytAllCachedResults;    // cache of all fetched results for current query
  int m_ytSearchSelectedIndex = 0;
  int m_ytSearchScrollOffset = 0;
  int m_ytCurrentPage = 1;           // current page (1-based, 6 results/page)
  int m_ytKbRow = 0;
  int m_ytKbCol = 0;
  bool m_ytKbShift = false;
  bool m_ytKbInResults = false;
  std::string m_ytErrorMessage;
  std::atomic<bool> m_ytIsSearching{false};
  std::atomic<bool> m_ytSearchFinished{false};
  std::atomic<bool> m_ytIsLoadingVideo{false};
  std::atomic<bool> m_ytVideoReady{false};
  std::string m_ytPendingStreamUrl;
  std::string m_ytPendingVideoId;
  std::vector<std::string> m_ytSearchHistory;
  int m_ytSelectedTagIndex = 0;
  bool m_ytFocusInTags = false;
  void loadYouTubeHistory();
  void saveYouTubeHistory(const std::string& query);

  // TikTok Search State
  std::string m_ttSearchQuery;
  std::string m_ttLastSearchQuery;
  std::vector<std::string> m_ttSearchResults;
  int m_ttSearchSelectedIndex = 0;
  int m_ttSearchScrollOffset = 0;
  int m_ttCurrentPage = 1;
  int m_ttKbRow = 0;
  int m_ttKbCol = 0;
  bool m_ttKbShift = false;
  bool m_ttTelexMode = true;
  std::string m_ttErrorMessage;
  std::atomic<bool> m_ttIsSearching{false};
  std::atomic<bool> m_ttSearchFinished{false};
  std::atomic<bool> m_ttIsLoadingVideo{false};
  std::atomic<bool> m_ttVideoReady{false};
  std::string m_ttPendingStreamUrl;
  std::string m_ttPendingVideoId;
  std::string m_ttPendingTitle;
  std::vector<std::string> m_ttTrendingTags;
  int m_ttSelectedTagIndex = 0;
  bool m_ttFocusInTags = false;
  void initTikTokTags();

  // System Selection State
  int m_selectedSystemIndex = 0;
  std::vector<SystemRecord> m_cachedSystems;

  // Game List State
  SystemRecord m_activeSystem;
  int m_selectedGameIndex = 0;
  int m_gameScrollOffset = 0;
  GameFilterMode m_filterMode = GameFilterMode::ALL;

  // Multi-Select State
  bool m_multiSelectMode = false;
  std::vector<int64_t> m_selectedGameIds;

  // Settings State
  int m_selectedSettingsRow = 0;
  int m_settingsScrollOffset = 0;

  // Diagnostics scroll state
  int m_diagnosticsScrollOffset = 0;

  // Search state
  std::string m_searchQuery;
  std::vector<GameRecord> m_searchResults;
  int m_searchSelectedIndex = 0;
  int m_searchScrollOffset = 0;
  // On-screen keyboard
  int m_kbCursorRow = 0;
  int m_kbCursorCol = 0;
  bool m_kbInResults = false; // false=typing, true=browsing results
  std::vector<GameRecord> m_cachedGames;

  // Notification toast
  std::string m_toastMessage;
  uint32_t m_toastExpiry = 0;
  SDL_Color m_toastColor = {0, 180, 216, 255};

  void showToast(const std::string &message,
                 SDL_Color color = {0, 180, 216, 255},
                 uint32_t durationMs = 2500);

  void refreshSystems();
  void refreshGames();
  void triggerManualSync();

  // Render helpers
  void renderHeader();
  void renderFooter();
  void renderMenuState();
  void renderSystemSelectState();
  void renderGameListState();
  void renderSearchState();
  void renderConfirmDeleteDialog();
  void renderConfirmBatchDeleteDialog();
  void renderDisclaimerState();
  void renderCloudLoginState();
  void renderSyncOverlay();
  void renderDownloadOverlay();
  void renderSettingsState();
  void renderDiagnosticsState();
  void renderOTAUpdateState();
  void renderReverseSyncState();
  void renderIPTVPlaylistSelectState();
  void renderIPTVState();
  void renderIPTVSearchState();
  void renderYouTubeSearchState();
  void renderYouTubeResultsState();
  void renderTikTokSearchState();
  void renderTikTokResultsState();

  // LocalSend P2P
  void renderLocalSendHome();
  void renderLocalSendIncomingDialog();
  void renderLocalSendFolderPicker();
  void renderLocalSendSendPicker();
  void renderLocalSendGamePicker();
  void renderLsProgressRow(bool isSend, int idx, int x, int y, int w, bool sel);
  void renderLocalSendProgress();

  // YouTube integration
  bool m_ytTelexMode = true;
  std::unordered_map<std::string, SDL_Texture*> m_ytThumbnails;
  std::unordered_map<std::string, std::string> m_ytStreamUrlCache;
  std::vector<std::string> runYouTubeSearch(const std::string& query, int page = 1);
  std::string resolveYouTubeStreamUrl(const std::string& videoId);
  void preloadYouTubeStreamUrl(const std::string& videoId);
  void triggerYouTubeSearch();
  void playYouTubeVideo(const std::string& videoId);
  void startThumbnailDownloads(const std::vector<std::string>& videoIds);
  void clearThumbnailCache();
  void renderUploadOverlay();
  void renderToast();

  // TikTok integration
  std::vector<std::string> runTikTokSearch(const std::string& query, int page = 1);
  std::vector<std::string> runTikTokTrending(int page = 1);
  std::string resolveTikTokStreamUrl(const std::string& tiktokUrl);
  void triggerTikTokSearch();
  void triggerTikTokTrending();
  void playTikTokVideo(const std::string& tiktokUrl, const std::string& title);

  // Primitive drawing
  void drawText(const std::string &text, int x, int y, SDL_Color color,
                TTF_Font *font, bool centered = false);
  void drawRect(int x, int y, int w, int h, SDL_Color color,
                bool filled = true);
  void drawBorder(int x, int y, int w, int h, SDL_Color color,
                  int thickness = 2);
  void drawRoundedRect(int x, int y, int w, int h, int radius, SDL_Color color,
                       bool filled = true);
  void drawRoundedBorder(int x, int y, int w, int h, int radius,
                         SDL_Color color, int thickness = 1);
  void drawBadge(int x, int y, int w, int h, const std::string &text,
                 SDL_Color bg, SDL_Color fg);
  void drawIcon(const std::string &iconName, int x, int y, int w, int h);
  void drawPlayerIcon(const std::string &iconName, int x, int y, int w, int h);
  void drawButtonIcon(const std::string &button, int x, int y, int size);
  void drawGridIcon(const std::string &iconFile, int x, int y, int w, int h);
  // Ve icon nut + label can giua doc trong footer (fix lech text vs button)
  int textHeight(TTF_Font *font);
  int textWidth(const std::string &text, TTF_Font *font);
  // Cat chu theo pixel, khong cat giua ky tu UTF-8 (them "..." khi cat)
  std::string truncateToWidth(const std::string &text, TTF_Font *font, int maxPx);
  // Rong pill fit chu theo pixel (kep MIN/MAX)
  int pillWidth(const std::string &text, TTF_Font *font);
  // Chip/button/badge tron mem (phuong an A: pill full-round h/2)
  void drawPill(int x, int y, int w, int h, const std::string &text, bool active, TTF_Font *font = nullptr);
  // Button hanh dong (OK/Huy/Xoa...) radius BTN
  void drawButton(int x, int y, int w, int h, const std::string &label, bool focused, bool danger = false);
  // Row list chuan (game/channel/file/cai dat...)
  void drawRow(int x, int y, int w, int h, bool focused, bool dim = false);
  // Y can giua doc chuan cho 1 dong text trong box h (dung FontHeight)
  int textYCentered(int y, int h, TTF_Font *font);
  // Ve cap main/sub can giua doc trong row (fix main/sub lech tam)
  void drawRowMainSub(int x, int y, int h, const std::string &main, TTF_Font *fMain,
                      const std::string &sub, TTF_Font *fSub, int maxW = 0, int gap = 4);
  // Ve text canh phai that (right-align theo pixel, cho goc top-right/bottom-right)
  void drawTextRight(const std::string &text, int rightX, int y, SDL_Color color, TTF_Font *font);
  // Tra ve x sau khi ve xong 1 hint (de noi tiep nhieu hint). Icon va text can giua doc theo bar.
  int drawFooterHint(const std::string &button, const std::string &label, int x, int barY, int barH,
                     SDL_Color color, TTF_Font *font, int iconSize = 30, int gap = 8);
  // Ve chuoi footer dang "icon label   icon label..." can giua ngang + doc
  void drawFooterHintsCentered(const std::vector<std::pair<std::string,std::string>> &hints,
                               int barY, int barH, SDL_Color color, TTF_Font *font,
                               int iconSize = 30, int gap = 8, int hintGap = 28);
  // Ve 1 badge co 2 icon nut (vd: "[A] Kiem tra  •  [B] Quay lai")
  void drawBadgeDual(int x, int y, int w, int h,
                     const std::string &btn1, const std::string &label1,
                     const std::string &btn2, const std::string &label2,
                     SDL_Color bg, SDL_Color fg);
  // Ve 1 dong text co chua token [NUT] o bat ky vi tri nao -> thay bang icon that, can giua
  void drawInlineHintsCentered(const std::string &text, int centerX, int y,
                               SDL_Color color, TTF_Font *font, int iconSize = 26, int gap = 6);
  // ---- UiTheme helpers (Design Tokens, khong doi logic nghiep vu) ----
  void drawAppBackground();
  void drawCard(int x, int y, int w, int h);
  void drawFocusRow(int x, int y, int w, int h);
  void drawAppHeader(const std::string &title, const std::string &sub = "");
  void drawPadIcon(UiTheme::PadBtn btn, int x, int y, int size);
  void drawAppFooter(const std::vector<UiTheme::FooterHint> &hints);
  void beginModalDim();

  // Performance caches (60 FPS Smooth UI)
  std::unordered_map<std::string, SDL_Texture*> m_gridIconCache;
  std::unordered_map<std::string, SDL_Texture*> m_systemIconCache;
  std::unordered_map<std::string, SDL_Texture*> m_buttonIconCache;

  struct CachedTextTexture {
      SDL_Texture* texture = nullptr;
      int w = 0;
      int h = 0;
      uint32_t lastUsed = 0;
  };
  std::unordered_map<std::string, CachedTextTexture> m_textCache;
  void clearTextCache();

  std::atomic<bool> m_isIndexing{false};
  std::atomic<bool> m_needLibraryRefresh{false};

public:
  // Public API cho background worker threads (LocalSend, IPTV, etc.)
  void setNeedLibraryRefresh(bool v = true) { m_needLibraryRefresh.store(v); }
  bool needLibraryRefresh() const { return m_needLibraryRefresh.load(); }
};

} // namespace RomCloud
