#pragma once
#include "../database/DatabaseManager.h"
#include "../iptv/IPTVManager.h"
#include "../localsend/LocalSendProtocol.h"
#include "CoverManager.h"
#include "UiRenderer.h"
#include "ImageCache.h"
#include "UiTheme.h"
#include "DialogManager.h"
#include "../common/BackgroundTask.h"
#include "VirtualKeyboard.h"
#include "SearchInputModal.h"
#include "FileListView.h"
#include "../fileexplorer/FileExplorer.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <queue>
#include <deque>
#include <array>

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
  // P6: Full changelog sub-page từ Cập nhật tab → Y → xem "Tính năng mới".
  OTA_CHANGELOG,
  REVERSE_SYNC,
  IPTV_PLAYLIST_SELECT, // Chon playlist truoc khi xem kenh
  IPTV_LIST,
  IPTV_SEARCH,
  YOUTUBE_HOME,         // P0-6: Home — search input + pills + content area
  YOUTUBE_SEARCH,       // Legacy YOUTUBE_SEARCH state (still rendered for back-compat)
  YOUTUBE_RESULTS,
  LOCALSEND_HOME,       // Trang chính LocalSend: devices + pending + send queue
  LOCALSEND_INCOMING,   // Modal duyệt file gửi đến (A=đồng ý, B=từ chối)
  LOCALSEND_FOLDER,     // Chọn folder đích
  LOCALSEND_SEND,       // Chọn file để gửi + danh sách target devices
  LOCALSEND_GAME_PICKER, // Chọn game LOCAL từ DB để gửi (thay vì raw file picker)
  LOCALSEND_PROGRESS,   // Xem progress upload/download
  FILE_EXPLORER,      // Explorer 2-pane: m_expL/m_expR + clipboard chung m_expClip
  GAME_CAST,          // GameCast TV & Laptop streaming
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
  UiRenderer m_ui;
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
  // Rename / New-folder keyboard (dung chung VirtualKeyboard/VkState)
  bool m_lsFolderRenaming = false;
  bool m_lsFolderRenameExisting = false;
  std::string m_lsFolderRenameOriginal;
  VkState m_lsFolderVk;
  // LOCALSEND_INCOMING: 2 screen — 0=picker 2 cột, 1=xác nhận cuối
  int m_localSendIncomingMode = 0;  // 0=picker, 1=confirm
  std::string m_localSendIncomingSavePath;  // path tuyệt đối đích lưu file

  // Explorer 2-side: 2 FileExplorer doc lap + clipboard chung.
  FileExplorer m_expL, m_expR;
  int m_expActive = 0; // 0=L, 1=R
  int m_expScroll[2] = {0, 0};
  ExplorerClipboard m_expClip;
  bool m_expDeleteArmed = false;
  FileExplorer& expA() { return m_expActive ? m_expR : m_expL; }
  FileExplorer& expB() { return m_expActive ? m_expL : m_expR; }
  const FileExplorer& expA() const { return m_expActive ? m_expR : m_expL; }

  // 1-pane FileExplorer dùng chọn thư mục nhận file (LOCALSEND_FOLDER & LOCALSEND_INCOMING)
  FileExplorer m_expPicker;
  int m_expPickerScroll = 0;

  void renderFileExplorer();
  void renderExplorerKeyboard();
  void renderExplorerKeyboardFor(FileExplorer& ex);
  bool handleExplorerInput();
  bool handleExplorerKeyboard();
  bool handleExplorerKeyboardFor(FileExplorer& ex);
  bool handleFolderPickerInput();
  bool handleExplorerBrowser();
  void syncExplorerDialogs();
  std::string suggestNewFolderName(const std::string& parentPath);

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
  void centerIptvGroupBar(const std::vector<std::string> &groups,
                           const std::string &selected);
  int m_activePlaylistIndex = -1;    // -1 = tat ca playlists

  // IPTV Search & Virtual Keyboard State
  std::vector<IPTVChannel> m_iptvSearchResults;
  int m_iptvSearchSelectedIndex = 0;
  int m_iptvSearchScrollOffset = 0;
  VkState m_iptvVk; // query/row/col/shift/telex/inResults unified

  // YouTube Search State (keyboard unified on VkState)
  VkState m_ytVk;
  std::string m_ytLastSearchQuery;
  std::string m_ytKeyboardQuery; // Only text typed from keyboard

  // YouTube result item (pre-computed fields for fast render)
  struct YtItem {
    enum class Type { Video, Channel };
    Type type = Type::Video;
    std::string id;
    std::string title;       // raw title (for search/re-search)
    std::string titleL1;    // pre-truncated line 1 (fontSmall, ~700px)
    std::string titleL2;    // pre-truncated line 2
    std::string channel;     // pre-truncated channel (~320px)
    std::string viewsStr;   // pre-formatted "1.2M views"
    std::string durationStr;// pre-formatted "12:34"
    bool hasTitleL2 = false;

    // Channel-specific fields (only used when type == Channel)
    std::string channelId;       // @handle hoặc UCxxxx (cho re-search latest)
    std::string subscribersStr;  // pre-formatted "1.2M subscribers"
    std::string videoCountStr;   // pre-formatted "523 videos"
  };
  std::vector<std::string> m_ytSearchResults;       // raw pipe-delimited (backing store)
  std::vector<std::string> m_ytAllCachedResults;    // raw pipe-delimited (backing store)
  std::vector<YtItem> m_ytItems;                    // pre-computed view of m_ytSearchResults
  std::vector<YtItem> m_ytAllItems;                 // pre-computed view of m_ytAllCachedResults
  int m_ytSearchSelectedIndex = 0;
  int m_ytSearchScrollOffset = 0;
  int m_ytCurrentPage = 1;           // current page (1-based, 5 results/page)
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
  int m_ytSearchFocus = 2; // 1 = History pills, 2 = Virtual keyboard
  int m_ytHistoryRow = 0;  // 0 = Row 3, 1 = Row 4
  int m_ytHistoryCol = 0;  // index within that row

  // P0-3: YouTube category filter (9 categories, horizontal scroll)
  // P0-6: feedQuery thay thế suffix — query đầy đủ sẽ được smart_search
  // khi user chọn pill (không cần input). Empty feedQuery = dùng user input
  // (pill chỉ đóng vai trò category filter trên top of user query).
  struct Category {
    const char *id;        // "all", "music", ...
    const char *label;     // "Tất cả", "Âm nhạc", ...
    const char *feedQuery; // auto-feed query khi pick pill (e.g. "nhạc việt")
  };
  static const std::array<Category, 9> kYtCategories;
  int m_ytSelectedCategory = 0;
  int m_ytCategoryScrollOffset = 0;  // horizontal scroll offset (px)

  // P0-6: YouTube HOME state (search input + pills + content)
  // m_ytSearchModalOpen: when true, render SearchInputModal over HOME.
  // m_ytViewMode: false = row list (5 rows/page), true = grid 3x2 (6 cards/page).
  // m_ytHasSearched: true after first category auto-feed search has results.
  bool m_ytSearchModalOpen = false;
  bool m_ytViewMode = false; // false = row, true = grid
  bool m_ytHomeHasResults = false;
  int m_ytHomeContentSelected = 0;
  int m_ytHomeContentScrollOffset = 0;
  int m_ytHomePage = 1;
  int m_ytHomeFocus = 1;          // 0=search box, 1=pills, 2=content
  int m_ytHomeRow = 1;            // 0=search, 1=tag, 2=thumbnail row 1, 3=thumbnail row 2
  int m_ytHomeCol = 0;            // 0..2 for thumbnail rows
  bool m_ytHomeLoadedThisEnter = false;
  struct YtMatchedChannel {
    bool matched = false;
    std::string name;
    std::string subscribers;
    std::string videoCount;
    std::string avatarUrl;
  };
  YtMatchedChannel m_ytMatchedChannel;
  bool m_ytInChannelView = false;
  std::vector<std::string> m_ytChannelAllVideos;
  SearchInputModal::Config m_ytHomeModalCfg;
  void openYouTubeHomeModal();
  void closeYouTubeHomeModal();
  void runYouTubeHomeSearch(const std::string &query);
  void toggleYouTubeViewMode();
  void renderYouTubeHomeState();
  void renderYouTubeHomeContentRow(int contentTop, int contentH);
  void renderYouTubeHomeContentGrid(int contentTop, int contentH);
  void renderYouTubeHomeChannelLayout(int contentTop, int contentH);
  // ── Chuẩn hoá layout video (áp dụng cho MỌI màn có thumbnail) ──────
  // 1) drawYtStandardThumb: chỉ vẽ ẢNH thumbnail thuần (không banner, không
  //    pill duration). Fallback nền tối khi ảnh chưa decode xong.
  //    scaledThumb=true chỉ dùng cho màn Results (RenderCopy bằng toạ độ
  //    đã scale của PlatformInfo, khác 3 màn HOME).
  void drawYtStandardThumb(int tx, int ty, int tw, int th,
                           const std::string &vid, bool selected,
                           bool scaledThumb);
  // 2) drawYtStandardMeta: dòng 1 = tên video (TEXT_MAIN), dòng 2 =
  //    "duration - lượt xem" (TEXT_SUB), cả hai canh trái. lineStep do
  //    caller tính để vừa khối info.
  void drawYtStandardMeta(int x, int y, int maxW, int lineStep,
                          const YtItem &item);
  std::string currentCategoryFeedQuery() const;
  void loadYouTubeViewMode();
  void saveYouTubeViewMode();
  void renderYouTubeHomeModalOverlay();

  void loadYouTubeHistory();
  void saveYouTubeHistory(const std::string& query);
  struct YtHistoryPill {
    int index = 0;
    std::string text;
    int w = 0;
  };
  void getYouTubeHistoryPills(std::vector<YtHistoryPill> &row3,
                              std::vector<YtHistoryPill> &row4);
  YtItem buildYtItemFromPipe(const std::string &raw);
  void rebuildYtItems();


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
  // Settings tabs: 0=CHUNG (default - danh sách cài đặt), 1=CẬP NHẬT (OTA).
  // Auto-switch sang tab 1 khi vào Settings với isUpdateAvailable()==true
  // để badge "NEW" trên icon Cài đặt (menu chính) dẫn thẳng tới OTA.
  int m_settingsTab = 0;

  // Diagnostics scroll state
  int m_diagnosticsScrollOffset = 0;
  // Info tabs: 0=GIOI THIEU (default), 1=HE THONG
  int m_infoTab = 0;
  int m_lastInfoTab = -1; // detect tab transition for About auto-scroll reset
  // About tab auto-scroll for "THƯ NGỎ" letter cell.
  // m_aboutAutoScrollY: pixel offset (float for smooth sub-line motion).
  // m_aboutLastTickMs: SDL_GetTicks() snapshot from previous renderAboutTab
  // call, used to compute dt. Reset together on tab entry / exit.
  float m_aboutAutoScrollY = 0.0f;
  uint32_t m_aboutLastTickMs = 0;

  // P6: OTA Changelog sub-page scroll (manual, line-based). Reset on entry.
  int m_otaChangelogScrollLine = 0;

  // Search state
  std::vector<GameRecord> m_searchResults;
  int m_searchSelectedIndex = 0;
  int m_searchScrollOffset = 0;
  VkState m_searchVk; // query/row/col/inResults unified (SEARCH, layout cu giu nguyen)
  std::vector<GameRecord> m_cachedGames;

  // Notification toast (DialogManager/ToastState la nguon duy nhat)
  DialogManager m_dialogs;

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
  void openConfirmDeleteDialog();
  void openConfirmBatchDeleteDialog();
  void renderConfirmDialogFromState();
  void renderConfirmDeleteDialog();
  void renderConfirmBatchDeleteDialog();
  void drawProgressBar(int x, int y, int w, int h, double frac, SDL_Color fill, bool rounded = false, SDL_Color bg = {35, 42, 54, 255});
  void renderProgressDialogFromState();
  void renderDisclaimerState();
  void renderCloudLoginState();
  void renderSyncOverlay();
  void renderDownloadOverlay();
  void renderSettingsState();
  void renderDiagnosticsState();
  void renderAboutTab(int contentTop);
  std::vector<std::string> wrapAboutText(const std::string &text, TTF_Font *font, int maxPx);
  void renderOTAUpdateState();
  void renderOTAChangelogState();
  void renderReverseSyncState();
  void renderIPTVPlaylistSelectState();
  void renderIPTVState();
  void renderIPTVSearchState();
  void renderYouTubeSearchState();
  void renderYouTubeResultsState();
  void renderGameCastState();
  bool m_castNativeRes = false;

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
  std::unordered_map<std::string, std::string> m_ytStreamUrlCache;
  std::vector<std::string> runYouTubeSearch(const std::string& query, int page = 1);
  std::string resolveYouTubeStreamUrl(const std::string& videoId);
  void preloadYouTubeStreamUrl(const std::string& videoId);
  void triggerYouTubeSearch();
  void playYouTubeVideo(const std::string& videoId);
  void startThumbnailDownloads(const std::vector<std::string>& videoIds);
  void clearThumbnailCache();
  void applyYouTubeSearchResults(std::vector<std::string> results);
  void renderUploadOverlay();
  void renderToast();


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
  void drawRoundedTopBar(int x, int y, int w, int h, int radius,
                         SDL_Color color);
  void drawModalDialog(int x, int y, int w, int h, int radius,
                       SDL_Color bodyBg, SDL_Color titleBg, int titleH);
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
  int badgeWidth(const std::string &text, int h);
  int buttonWidth(const std::string &label);
  int badgeDualWidth(const std::string &label1, const std::string &label2, int h);
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
  void drawHeaderStatus();
  void drawPadIcon(UiTheme::PadBtn btn, int x, int y, int size);
  void drawAppFooter(const std::vector<UiTheme::FooterHint> &hints);
  void beginModalDim();

  // Performance caches (60 FPS Smooth UI) — P2-2: dung ImageCache LRU chung.
  // Icon/logo/button/grid do m_ui.images() giu; UIManager chi giu thumb YT (gioi han 24).
  ImageCache m_ytThumbCache{24};

  // P0-2 YT perf: decode thumbnail off-thread.
  // - Worker thread: IMG_Load(path) -> SDL_Surface (CPU only, no GPU)
  // - Main thread: pop surface, SDL_CreateTextureFromSurface (GPU upload)
  // SDL_Surface ownership: created on worker, consumed/freed by main thread.
  std::thread m_thumbWorker;
  std::mutex m_thumbQueueMtx;
  std::condition_variable m_thumbCv;
  std::deque<std::pair<std::string, std::string>> m_thumbPendingDecode; // (vid, path), priority via push_front
  std::atomic<bool> m_thumbWorkerStop{false};
  std::atomic<bool> m_thumbWorkerStarted{false};
  // Surfaces ready for GPU upload (worker -> main)
  struct ReadyThumb {
    std::string vid;
    SDL_Surface *surf;
  };
  std::mutex m_thumbReadyMtx;
  std::queue<ReadyThumb> m_thumbReady;
  // Vids already in queue (avoid duplicate enqueue)
  std::unordered_set<std::string> m_thumbEnqueued;

  void clearTextCache();

  // P0-2 thumb worker API
  void startThumbWorker();
  void stopThumbWorker();
  void enqueueThumbDecode(const std::string &vid, const std::string &path,
                          bool priority = false);
  void drainReadyThumbs(); // call on main thread each frame

  // P1-1: scan/index SD chay tren BackgroundTask (thay std::thread().detach()).
  BackgroundTask m_indexTask;
  // Media tasks: search YT/TT rieng (tranh wait-block UI khi chuyen man hinh),
  // resolve (stream URL) + thumb (tai thumbnail) dung chung.
  BackgroundTask m_ytSearchTask;
  std::mutex m_ytSearchMutex;
  std::vector<std::string> m_ytPendingSearchResults;
  std::atomic<bool> m_ytSearchDataReady{false};
  BackgroundTask m_resolveTask;
  BackgroundTask m_thumbTask;
  std::atomic<bool> m_needLibraryRefresh{false};
  bool isIndexing() const { return m_indexTask.isRunning(); }

  // P1-2: realtime stream telemetry from GET http://127.0.0.1:8090/api/status.
  // Polled moi 2s khi UIState=GAME_CAST, render moi frame. atomic de tranh
  // race voi HTTP fetch thread.
  struct CastStats {
    std::atomic<int> fps{0};
    std::atomic<int> viewers{0};
    std::atomic<bool> downscale{false};
    std::atomic<bool> valid{false}; // true sau khi fetch thanh cong it nhat 1 lan
  } m_castStats;
  uint32_t m_lastCastStatsPoll = 0;

  // Settings Tab: Cache cleaner & Wi-Fi diagnostics
  uint64_t calculateCacheSizeBytes();
  std::string getCacheSizeFormatted();
  uint64_t cleanCache();
  void runWifiDiagnostics();

  std::string m_cacheSizeFormatted;
  std::string m_wifiDiagStatus = "Chưa kiểm tra";
  SDL_Color m_wifiDiagColor = {148, 163, 184, 255};
  std::atomic<bool> m_wifiDiagRunning{false};

public:
  // Public API cho background worker threads (LocalSend, IPTV, etc.)
  void setNeedLibraryRefresh(bool v = true) { m_needLibraryRefresh.store(v); }
  bool needLibraryRefresh() const { return m_needLibraryRefresh.load(); }
};

} // namespace RomCloud
