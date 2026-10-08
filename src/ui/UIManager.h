#pragma once
#include "../calendar/CalManager.h"
#include "../common/BackgroundTask.h"
#include "../database/DatabaseManager.h"
#include "../fileexplorer/FileExplorer.h"
#include "../iptv/IPTVManager.h"
#include "../localsend/LocalSendProtocol.h"
#include "../weather/WeatherManager.h"
#include "CoverManager.h"
#include "DialogManager.h"
#include "FileListView.h"
#include "ImageCache.h"
#include "SearchInputModal.h"
#include "UiRenderer.h"
#include "UiTheme.h"
#include "VirtualKeyboard.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace RomCloud {

enum class UIState {
  MENU,
  SYSTEM_SELECT,
  GAME_LIST,
  SEARCH,
  CONFIRM_DELETE,
  CONFIRM_BATCH_DELETE,
  CONFIRM_UPLOAD,
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
  YOUTUBE_HOME,   // P0-6: Home — search input + pills + content area
  YOUTUBE_SEARCH, // Legacy YOUTUBE_SEARCH state (still rendered for
                  // back-compat)
  YOUTUBE_RESULTS,
  LOCALSEND_HOME,     // Trang chính LocalSend: devices + pending + send queue
  LOCALSEND_INCOMING, // Modal duyệt file gửi đến (A=đồng ý, B=từ chối)
  LOCALSEND_FOLDER,   // Chọn folder đích
  LOCALSEND_SEND,     // Chọn file để gửi + danh sách target devices
  LOCALSEND_GAME_PICKER, // Chọn game LOCAL từ DB để gửi (thay vì raw file
                         // picker)
  LOCALSEND_PROGRESS, // Xem progress upload/download
  FILE_EXPLORER, // Explorer 2-pane: m_expL/m_expR + clipboard chung m_expClip
  DEST_PICKER,   // Chọn thư mục đích cho Sao chép / Chuyển đi / Bung tới...
  TEXT_VIEWER,   // Xem nội dung file text từ File Explorer
  WEATHER,       // Thời tiết & Lịch (+ tab Đồng hồ, Camera)
  CLOCK_ALARM,   // Modal báo thức kêu (briefing)
  GAME_CAST,     // GameCast TV & Laptop streaming
  BROWSER,       // Browser main state (URL bar + content)
  BROWSER_INPUT, // Browser URL input (virtual keyboard)
  EXIT_REQUESTED
};

enum class LibraryTab { DRIVE = 0, SDCARD = 1 };
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
  // B = lùi về trang trước: stack lưu lịch sử setState (trừ EXIT).
  // goBack() false = hết lịch sử (caller tự quyết, VD home thì thoát app).
  bool goBack();

private:
  UIManager() = default;

  SDL_Window *m_window = nullptr;
  SDL_Renderer *m_renderer = nullptr;
  UiRenderer m_ui;
  TTF_Font *m_fontTitle = nullptr;
  TTF_Font *m_fontHuge = nullptr;    // giờ khổ lớn (card Đồng hồ)
  TTF_Font *m_fontClock = nullptr;   // số đồng hồ: Rajdhani Bold (OFL)
  TTF_Font *m_fontClockSm = nullptr; // giờ thế giới (Rajdhani 60)
  TTF_Font *m_fontClockFs = nullptr; // fullscreen (Rajdhani 260)
  TTF_Font *m_fontClockMd = nullptr; // thẻ lật fullscreen (Rajdhani 200)
  TTF_Font *m_fontFlip = nullptr;    // số flip clock (Fliqlo, ISC)
  TTF_Font *m_fontLarge = nullptr;
  TTF_Font *m_fontMedium = nullptr;
  TTF_Font *m_fontSmall = nullptr;
  std::string m_fontPathUsed;  // P9: resolved TTF file for CSS font-size

  UIState m_currentState = UIState::MENU;
  std::vector<UIState> m_stateHistory; // stack cho B = back
  bool m_suppressPush = false;         // goBack() set để không push ngược
  int m_selectedMenuIndex = 0;
  uint32_t m_exitArmedMs =
      0; // B lần 1 ở launcher: arm, B lần 2 trong 3s = thoát

  // Menu items as grid icons
  struct GridMenuItem {
    std::string id;
    std::string title;
    std::string iconFile; // PNG filename in assets/apps_icons/
    std::string subtitle;
  };
  std::vector<GridMenuItem> m_gridMenuItems;

  // IPTV Playlist Browser State (MOI)
  int m_selectedPlaylistIndex = 0;
  int m_playlistScrollOffset = 0;

  // IPTV Channel List State
  int m_selectedIPTVChannelIndex = 0;

  // LocalSend P2P state
  // m_localSendMode: 0=SEND, 1=RECEIVE
  int m_localSendMode = 0;
  int m_localSendSelectedDevice = 0;
  int m_localSendFolderSelected = 0;
  int m_localSendIncomingFolderIdx =
      0; // 0=auto, còn lại = preset trong dialog duyệt
  std::string m_localSendPendingSessionId;
  LsUploadRequest m_localSendCurrentPrompt;
  int m_localSendProgressSel = 0;         // dong dang chon trong man PROGRESS
  int m_localSendProgressScroll = 0;      // scroll man PROGRESS
  uint32_t m_localSendProgressDoneMs = 0; // moc tat ca xong (de auto-ve home)
  int m_localSendScrollOffset = 0;
  int m_localSendTab =
      0; // 0=devices, 1=received, 2=send queue (giữ để tương thích)
  int64_t m_localSendLastDeviceRefreshMs = 0;

  // LocalSend: ROM picker — quét thẳng /mnt/SDCARD/Roms (không qua DB)
  struct LsRomEntry {
    std::string path;      // full path file rom
    std::string name;      // basename
    std::string systemDir; // vd "GBA", "FC"
    uint64_t sizeBytes = 0;
  };
  std::vector<LsRomEntry> m_lsRomList;
  bool m_lsRomListLoaded = false;
  int m_lsRomSelected = 0;
  int m_lsRomScrollOffset = 0;

  // LocalSend: picker tabs 0=File (Roms SD) / 1=Folder (Apps SD) — theo
  // Selection gốc
  int m_lsPickerTab = 0;

  // Folder picker (LOCALSEND_FOLDER + LOCALSEND_INCOMING) — file explorer 2 cột
  std::string m_localSendFolderCurrentPath = "/mnt/SDCARD";
  std::vector<std::string>
      m_localSendFolderEntries; // path tuyệt đối của folder con
  bool m_localSendFolderLoaded = false;
  int m_localSendFolderFocus =
      0; // 0 = danh sách folder, 1 = nút Chốt (panel phải)
  // Rename / New-folder keyboard (dung chung VirtualKeyboard/VkState)
  bool m_lsFolderRenaming = false;
  bool m_lsFolderRenameExisting = false;
  std::string m_lsFolderRenameOriginal;
  VkState m_lsFolderVk;
  // LOCALSEND_INCOMING: 2 screen — 0=picker 2 cột, 1=xác nhận cuối
  int m_localSendIncomingMode = 0;         // 0=picker, 1=confirm
  std::string m_localSendIncomingSavePath; // path tuyệt đối đích lưu file

  // Explorer 2-side: 2 FileExplorer doc lap + clipboard chung.
  FileExplorer m_expL, m_expR;
  int m_expActive = 0; // 0=L, 1=R
  int m_expScroll[2] = {0, 0};
  ExplorerClipboard m_expClip;
  bool m_expDeleteArmed = false;
  FileExplorer &expA() { return m_expActive ? m_expR : m_expL; }
  FileExplorer &expB() { return m_expActive ? m_expL : m_expR; }
  const FileExplorer &expA() const { return m_expActive ? m_expR : m_expL; }

  // 1-pane FileExplorer dùng chọn thư mục nhận file (LOCALSEND_FOLDER &
  // LOCALSEND_INCOMING)
  FileExplorer m_expPicker;
  int m_expPickerScroll = 0;
  // Tiêu đề picker (mặc định nhận file LocalSend; đổi khi bung tới thư mục)
  std::string m_expPickerTitle = "CHỌN THƯ MỤC NHẬN FILE";

  // Popup menu file explorer (A trên file): Xem / Sao chép / Chuyển đi /
  // Xóa / Bung tại đây / Bung tới thư mục... (Xem chỉ file text, 2 mục
  // bung chỉ file nén).
  bool m_expMenuOpen = false;
  int m_expMenuSel = 0;
  std::vector<std::string> m_expMenuItems;
  std::string m_expMenuFile;
  // Cache lễ quan trọng (tính 1 lần/ngày, không quét 400 ngày mỗi frame)
  int m_holDayKey = -1;
  std::string m_holText;
  std::string m_holDate; // dd/MM của lễ đang cache (vẽ trước tên)
  // Text viewer: file đang xem + dòng đã wrap + scroll
  std::string m_textViewPath;
  std::vector<std::string> m_textViewLines;
  int m_textViewScroll = 0;
  // Cache texture ngày âm scale 75% (rebuild khi đổi tháng)
  int m_lunarCacheY = 0, m_lunarCacheM = 0;
  std::vector<SDL_Texture *> m_lunarTex;

  // Đồng hồ (tab 1 trang Thời tiết): world clock + báo thức + pomodoro.
  // Mọi giờ tính từ đồng hồ máy Brick (localtime), không NTP.
  int m_clkSel = 0; // chọn báo thức trong list
  int m_clkFocus =
      0;             // focus khối: 0 = giờ thế giới, 1 = báo thức, 2 = pomodoro
  int m_citySel = 0; // con trỏ thành phố trong khối giờ thế giới
  bool m_clkFullscreen = false; // toàn màn hình: chỉ đồng hồ, B quay về
  int m_clkFsStyle =
      0; // style fullscreen: 0=thẻ lật (flip), 1=phẳng, 2=qlocktwo
  // Trạng thái lật từng thẻ HH MM SS (theo mẫu FlipCard tham khảo).
  struct FsDigit {
    std::string cur;   // số đang hiện (static)
    std::string prev;  // số cũ (khi đang lật)
    bool ani = false;  // đang lật hay không
    float prog = 0.0f; // tiến trình 0..1
  };
  FsDigit m_fsD[3];
  uint32_t m_fsLastMs = 0;      // tick frame trước (tính dt)
  uint32_t m_clkFsMsgUntil = 0; // hiện gợi ý/tên kiểu tới mốc này (ms)
  std::string m_clkFsMsg;
  void clkBlockRect(int idx, int &x, int &y, int &w, int &h);
  void clkFocusMove(int dx, int dy);
  // Popup báo thức: card 2 cột (giờ 24h HH:MM | 3 chế độ).
  // ◀▶ đi qua HH → MM → chế độ (vòng lại), ▲▼ đổi giá trị tại chỗ.
  // A lưu (=OK), B hủy (=Cancel).
  bool m_apOpen = false;
  int m_apPart = 0; // 0 = khối giờ, 1 = khối chế độ
  int m_apCol = 0;
  int m_apH = 7, m_apM = 0, m_apMode = 0; // giờ 24h: 0-23
  int m_apEditIdx = -1;                   // -1 = thêm mới
  // Picker đổi thành phố giờ thế giới tại ô m_citySel: A mở/chốt, B hủy.
  bool m_cityPickOpen = false;
  // Liên kết Drive 1 chạm: portal web nâng cao / tự sync sau khi link.
  bool m_cloudPortalView = false; // true = xem hướng dẫn web portal cũ
  bool m_cloudAutoSync = false;   // đã tự sync sau khi link trong lần vào này
  int m_cityPickSel = 0;
  int m_cityPickScroll = 0;
  bool m_pomoRun = false;                 // pomodoro đang chạy
  bool m_pomoWork = true;                 // true = 25 làm, false = 5 nghỉ
  uint32_t m_pomoEndMs = 0;               // SDL ticks lúc hết phase
  uint32_t m_pomoLeftMs = 25 * 60 * 1000; // còn lại khi pause
  // Modal báo thức kêu
  bool m_alarmRing = false;
  std::string m_alarmTitle;
  std::vector<std::string> m_alarmLines;
  int m_alarmSnoozeMin = -1;   // hoãn tới phút trong ngày, -1 = không
  int m_alarmSnoozeDay = -1;   // yyyymmdd của lần hoãn
  int m_alarmFiredKey = -1;    // yyyymmdd*1440+phút đã kêu (chống lặp)
  std::set<int64_t> m_evFired; // id sự kiện lịch đã nhắc (chống lặp)
  int m_evFiredDayKey = -1;    // reset set trên khi sang ngày
  uint32_t m_lastClockPoll = 0;
  void renderClock(int contentTop, int contentH);
  void renderQlockTwoStyle(struct tm *lt); // Style 3: QlockTwo Vietnamese
  bool handleClockInput();
  void fireAlarm(const std::string &title,
                 const std::vector<std::string> &lines, int mode);
  void pollClock();
  void renderAlarmRing();
  bool handleAlarmRingInput();
  void renderAlarmPopup();
  bool handleAlarmPopupInput();
  void openAlarmPopup(int editIdx);
  void openCityPicker();
  void renderCityPicker();
  bool handleCityPickerInput();
  void renderClockFullscreen();
  // Tiết kiệm pin fullscreen: chỉ cần vẽ lại khi nội dung đổi (giây/phút
  // mới, đang animation lật, toast còn hiện). Còn lại UIManager::render()
  // sẽ SDL_Delay thay vì present 60fps.
  bool clockFrameNeeded();
  int m_fsLastKey = -1; // giây/phút đã vẽ gần nhất (tùy style)
  bool m_fsToastOn = false; // toast đang hiện ở frame đã vẽ (để xóa dứt điểm)
  void drawFlipCardBg(int x, int y, int w, int h);
  void cycleClkFsStyle(int dir); // L1/R1 đổi style fullscreen, lưu setting

  // Focus điều hướng không gian (DPAD): 0=now, 1=forecast, 2=lịch,
  // 3=clock, 4=notes. Trong lịch: con trỏ ngày (rollover đổi tháng).
  int m_wxFocus = 2;
  int m_calSelY = 0, m_calSelM = 0, m_calSelD = 0;
  // Ghi chú lịch (tab Thời tiết): con trỏ chọn việc + cache dòng đang hiện.
  int m_noteSel = 0;
  std::vector<CalEvent> m_noteVis;
  bool m_notesDay = false; // true = ghi chú lọc theo ngày con trỏ
  void wxFocusMove(int dx, int dy);
  void wxBlockRect(int idx, int &x, int &y, int &w, int &h);
  // Tab trang: 0 = Thời tiết & Lịch, 1 = Đồng hồ, 2 = Camera,
  // 3 = Giá cả, 4 = Thị trường (crypto + chứng khoán VN).
  int m_wxTab = 0;
  int m_wxMode = 0;
  int m_wxProvSel = 0, m_wxWardSel = 0, m_wxCandSel = 0;
  int m_wxCalY = 2026, m_wxCalM = 1;
  int m_wxTaskKind = 0;
  bool m_wxFetching = false;
  uint32_t m_wxEnterMs = 0; // lúc vào trang: sync DELAY sau 1.5s để vào mượt
  bool m_wxAutoSync = false;
  BackgroundTask m_wxTask;
  std::vector<WxGeoCand> m_wxCands;
  std::mutex m_wxCandsMutex;
  void openWeather();
  void renderWeather();
  void renderWxPicker(int contentTop, int contentH);
  bool handleWeatherInput();
  void drawWxIcon(const std::string &kind, int cx, int cy, int s);
  // Camera giao thông (tab 1 trang thời tiết)
  size_t m_camSel = 0;
  int m_camScroll = 0;
  bool m_camView = false; // false=list, true=xem ảnh
  bool m_camPaused = false;
  bool m_camFetching = false;
  uint32_t m_camLastRefresh = 0;
  int64_t m_camDecodedMtime = 0;
  SDL_Texture *m_camTex = nullptr;
  BackgroundTask m_camTask;
  // Tìm camera theo tên đường (SELECT mở modal, START xóa lọc)
  VkState m_camVk;
  std::vector<std::string> m_camHist;
  SearchInputModal::Config m_camModalCfg;
  bool m_camModalOpen = false;
  bool m_camModalInit = false;
  std::vector<size_t> m_camFilter; // rỗng = không lọc text
  bool m_camFavOnly = false;       // Y: chỉ hiện kênh yêu thích (giống IPTV)
  std::vector<size_t> m_camVisIdx; // cache hiển thị = text ∩ fav
  bool m_camVisDirty = true;
  size_t camVisCount() const;
  size_t camVisToReal(size_t pos) const;
  void rebuildCamVis();
  void renderTraffic();
  bool handleTrafficInput();
  void restartCamView();
  void applyCamFilter(const std::string &query);
  // Báo ngập: sub-view của tab Camera (m_wxTab == 2), D-pad Trái/Phải
  // chuyển Camera <-> Báo ngập. Không thêm tab mới (Office Go hết chỗ).
  int m_camSub = 0; // 0 = camera, 1 = báo ngập
  bool m_camSubArmed = true; // chống dội: nhả D-pad Trái/Phải mới cho chuyển tiếp
  size_t m_floodSel = 0;
  int m_floodScroll = 0;
  VkState m_floodVk;
  std::vector<std::string> m_floodHist;
  SearchInputModal::Config m_floodModalCfg;
  bool m_floodModalOpen = false;
  bool m_floodModalInit = false;
  std::vector<size_t> m_floodFilter; // rỗng = không lọc text
  std::vector<size_t> m_floodVisIdx; // cache hiển thị sau lọc
  bool m_floodVisDirty = true;
  size_t floodVisCount() const;
  size_t floodVisToReal(size_t pos) const;
  void rebuildFloodVis();
  void renderFlood();
  void drawFloodHeader(size_t vtotal);
  bool handleFloodInput();
  void applyFloodFilter(const std::string &query);
  // Live ngập: fetch nền khi vào màn, TTL 2h (delay chấp nhận được).
  BackgroundTask m_floodTask;
  bool m_floodFetching = false;
  void pollFloodLive();
  // Thị trường (tab 4): crypto + chứng khoán VN, tối đa 4 card 2x2.
  // Mỗi card 1 mã: giá + % đổi + biểu đồ sparkline. Y nhập mã (modal),
  // X xóa card, A tải lại.
  BackgroundTask m_watchTask;
  bool m_watchFetching = false;
  uint32_t m_watchLastMs = 0;
  int m_watchSel = 0; // card đang chọn trong lưới 2 cột
  // Nhập mã theo dõi (Y mở modal, START chốt như các modal tìm kiếm).
  VkState m_watchVk;
  std::vector<std::string> m_watchHist;
  SearchInputModal::Config m_watchModalCfg;
  bool m_watchModalOpen = false;
  bool m_watchModalInit = false;
  void renderWatch(int contentTop, int contentH);
  bool handleWatchInput();
  void openWatchModal();
  void startWatchFetch();
  void pollWatch();
  void drawSparkline(const std::vector<float> &pts, int x, int y, int w, int h,
                     SDL_Color color);
  // Giá cả (tab 3): xăng/dầu/vàng/USD live, 1 card 2x2.
  BackgroundTask m_mkTask;
  bool m_mkFetching = false;
  uint32_t m_mkLastMs = 0;
  void renderMarket(int contentTop, int contentH);
  bool handleMarketInput();
  void startMarketFetch();
  void pollMarket();
  // Tab Wi-Fi (settings tab 3): quét + nối mạng + portal captive.
  size_t m_wifiSel = 0;
  int m_wifiScroll = 0;
  bool m_wifiBusy = false;
  bool m_wifiNeedScan = true; // vào tab là quét ngay lần đầu
  bool m_wifiHidden = false;
  std::string m_wifiStatus;
  std::string m_wifiCurSsid; // mạng đang nối (đánh dấu trong list)
  uint32_t m_wifiStatusMs = 0;
  BackgroundTask m_wifiTask;
  VkState m_wifiVk;
  std::vector<std::string> m_wifiHist;
  SearchInputModal::Config m_wifiModalCfg;
  bool m_wifiModalOpen = false;
  bool m_wifiModalInit = false;
  int m_wifiModalMode = 0; // 0=none, 1=SSID ẩn, 2=pass
  std::string m_wifiPendingSsid;
  std::string m_wifiModalTitle; // giữ chuỗi cho Config (const char*)
  std::string m_wifiModalPh;
  void renderWifiTab();
  bool handleWifiTabInput();
  void startWifiScan();
  // Browser NetSurf (chạy riêng process, mở từ launcher/Wi-Fi).
  // Khung UI đấu nối sau khi nsfb build xong.
  void startWifiConnect(const std::string& ssid, const std::string& psk,
                        bool hidden, bool useSaved);
  void startWifiPortal();
  void openWifiModal(int mode, const std::string& title,
                     const std::string& placeholder);
  // Picker đích dùng chung (DEST_PICKER): 0=không, 1=Sao chép, 2=Chuyển đi,
  // 3=Bung tới thư mục. Mọi thao tác đều qua picker + confirm mới chạy.
  int m_pickerPendingOp = 0;
  std::string m_pickerPendingSrc;
  bool m_pickerPendingIsDir = false;

  void renderFileExplorer();
  void renderExplorerKeyboard();
  void renderExplorerKeyboardFor(FileExplorer &ex);
  bool handleExplorerInput();
  bool handleExplorerKeyboard();
  bool handleExplorerKeyboardFor(FileExplorer &ex);
  bool handleFolderPickerInput();
  bool handleExplorerBrowser();
  void renderBrowserState();
  void handleBrowserInput();
  void renderBrowserInputState();
  void handleBrowserInputKeyboard();
  // P8: seed + open the URL editor (telex OFF — URLs are ASCII).
  void openBrowserUrlEditor();
  // Cancel a page <input> edit: restore the original value, end the
  // session (B key while the field-edit VK overlay is up).
  void cancelBrowserFieldEdit();
  // P15.2: find-in-page editor (telex ON for Vietnamese queries).
  void openBrowserFindEditor();
  // P15.2: run the find and return to the page.
  void commitBrowserFind();
  // P8c: browser feature settings overlay.
  void toggleBrowserSetting(int idx);
  void renderBrowserSettings();
  // P8b: TLD suffix pills for the URL keyboard (browser only).
  static constexpr int kBrowserTldCount = 3;
  const char* browserTldLabel(int idx) const;
  // Browser URL input
  VkState m_browserVk;
  // P15.2: find-in-page reuses the URL keyboard; commit searches, START
  // jumps between matches while results are active.
  bool m_browserFindMode = false;
  // P8: URL-bar focus (YouTube-style). UP from the first page widget lands
  // on the URL bar; A opens the URL keyboard; DOWN returns to the page.
  bool m_browserUrlFocused = false;
  // Browser feature settings overlay (SELECT in browser content mode).
  bool m_browserSettingsOpen = false;
  int m_browserSettingsSel = 0;
  // P13: fullscreen media playback launched from the page (mpv owns screen).
  bool m_browserMediaPlaying = false;
  void pollBrowserMedia();
  // Browser URL history & quick sample site pills (YouTube-style)
  struct BrowserHistoryItem {
    std::string label;
    std::string url;
  };
  std::vector<BrowserHistoryItem> m_browserHistory;
  int m_browserHistorySel = 0;
  bool m_browserHistoryMode = false;
  void initBrowserHistory();
  void addBrowserHistory(const std::string& url);

  // P8b: TLD suffix pills above the URL keyboard (like history pills).
  // UP from keyboard row 0 enters pill mode; A appends the suffix.
  bool m_browserTldMode = false;
  int m_browserTldSel = 0;
  // Phase 1 — audit P1-2c: true khi user đang gõ vào một <input> trong page
  // (HtmlRenderer.m_editingText = true). UIManager sync giá trị giữa
  // m_browserVk.query (VirtualKeyboard Telex transform) và
  // HtmlRenderer::setFocusedInputValue().
  bool m_browserFieldEditing = false;
  // Value gốc của field khi mới vào edit mode — dùng để restore khi user
  // bấm "Hủy" (Cancel). Khi "Xong" (Commit) thì giữ value hiện tại.
  std::string m_browserFieldOriginal;
  // Default landing page: vnexpress.net (user's daily read; also exercises
    // libcurl + DNS + CSS + images on every launch).
    std::string m_browserUrl = "https://vnexpress.net";
  void syncExplorerDialogs();
  // Action dùng chung cho phím tắt (Y/X/MENU) và popup menu (A trên file)
  void expCopyCurrent();
  void expMoveCurrent();
  void expDeleteCurrent();
  void openDestPicker(int op, const std::string &title);
  void openExpMenu();
  void runExpMenuAction(int idx);
  bool handleExpMenuInput();
  void renderExpMenu();
  // Text viewer
  static bool isTextFile(const std::string &path);
  void openTextViewer(const std::string &path);
  void renderTextViewer();
  bool handleTextViewerInput();
  std::string suggestNewFolderName(const std::string &parentPath);

  // LocalSend: Apps picker (/mnt/SDCARD/Apps/ drill-down)
  struct LsAppEntry {
    std::string path; // full path
    std::string name; // display name
    uint64_t sizeBytes = 0;
    bool isDirectory = false;
  };
  std::vector<LsAppEntry> m_lsAppList;
  std::vector<std::string>
      m_lsAppBreadcrumb; // empty = top level (/mnt/SDCARD/Apps)
  int m_lsAppSelected = 0;
  int m_lsAppScrollOffset = 0;
  bool m_lsAppListLoaded = false;

  void renderLsRomsList(int dlgX, int dlgY, int dlgW, int dlgH);
  void renderLsAppsList(int dlgX, int dlgY, int dlgW, int dlgH);
  int m_iptvScrollOffset = 0;
  bool m_iptvShowFavoritesOnly = false;
  // Group filter bar (Loi 2 fix)
  std::string m_iptvSelectedGroup; // "" = Tat ca
  int m_iptvGroupBarOffset = 0;    // scroll ngang cua group bar
  void centerIptvGroupBar(const std::vector<std::string> &groups,
                          const std::string &selected);
  int m_activePlaylistIndex = -1; // -1 = tat ca playlists

  // IPTV OSD visibility (true = OSD đang hiện, false = footer hiện)
  bool m_iptvOsdVisible = false;

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
    std::string titleL1;     // pre-truncated line 1 (fontSmall, ~700px)
    std::string titleL2;     // pre-truncated line 2
    std::string channel;     // pre-truncated channel (~320px)
    std::string viewsStr;    // pre-formatted "1.2M views"
    std::string durationStr; // pre-formatted "12:34"
    bool hasTitleL2 = false;

    // Channel-specific fields (only used when type == Channel)
    std::string channelId;      // @handle hoặc UCxxxx (cho re-search latest)
    std::string subscribersStr; // pre-formatted "1.2M subscribers"
    std::string videoCountStr;  // pre-formatted "523 videos"
  };
  std::vector<std::string>
      m_ytSearchResults; // raw pipe-delimited (backing store)
  std::vector<std::string>
      m_ytAllCachedResults;         // raw pipe-delimited (backing store)
  std::vector<YtItem> m_ytItems;    // pre-computed view of m_ytSearchResults
  std::vector<YtItem> m_ytAllItems; // pre-computed view of m_ytAllCachedResults
  int m_ytSearchSelectedIndex = 0;
  int m_ytSearchScrollOffset = 0;
  int m_ytCurrentPage = 1; // current page (1-based, 5 results/page)
  std::string m_ytErrorMessage;
  std::atomic<bool> m_ytIsSearching{false};
  uint32_t m_ytSearchStartMs = 0; // watchdog: search quá 120s không về = kẹt
  std::atomic<bool> m_ytSearchFinished{false};
  std::atomic<bool> m_ytIsLoadingVideo{false};
  uint32_t m_ytLoadStartMs = 0; // mốc bấm play (hiện tiến trình tải)
  uint32_t m_ytLoadToastMs = 0; // mốc toast tiến trình gần nhất
  std::atomic<bool> m_ytVideoReady{false};
  std::string m_ytPendingStreamUrl;
  std::string m_ytPendingVideoId;
  std::string m_ytPendingVideoTitle; // tiêu đề cho OSD top bar (UI-only)
  std::string m_ytPendingQuality; // "720" mặc định, "360" nếu 720 rớt mới dùng
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
  int m_ytCategoryScrollOffset = 0; // horizontal scroll offset (px)

  // P0-6: YouTube HOME state (search input + pills + content)
  // m_ytSearchModalOpen: when true, render SearchInputModal over HOME.
  // m_ytViewMode: false = row list (5 rows/page), true = grid 3x2 (6
  // cards/page). m_ytHasSearched: true after first category auto-feed search
  // has results.
  bool m_ytSearchModalOpen = false;
  bool m_ytViewMode = false; // false = row, true = grid
  bool m_ytHomeHasResults = false;
  int m_ytHomeContentSelected = 0;
  int m_ytHomeContentScrollOffset = 0;
  int m_ytHomePage = 1;
  int m_ytHomeFocus = 1; // 0=search box, 1=pills, 2=content
  int m_ytHomeRow = 1; // 0=search, 1=tag, 2=thumbnail row 1, 3=thumbnail row 2
  int m_ytHomeCol = 0; // 0..2 for thumbnail rows
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
  void renderYouTubeHomeContentRow(int contentTop, int /*contentH*/);
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
  void saveYouTubeHistory(const std::string &query);
  struct YtHistoryPill {
    int index = 0;
    std::string text;
    int w = 0;
  };
  void getYouTubeHistoryPills(std::vector<YtHistoryPill> &row3,
                              std::vector<YtHistoryPill> &row4);
  YtItem buildYtItemFromPipe(const std::string &raw);
  void rebuildYtItems();

  // Library Tab State (0: DRIVE, 1: SDCARD)
  LibraryTab m_libraryTab = LibraryTab::DRIVE;

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
  VkState m_searchVk; // query/row/col/inResults unified (SEARCH, layout cu giu
                      // nguyen)
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
  void openConfirmUploadDialog();
  void renderConfirmDialogFromState();
  void renderConfirmDeleteDialog();
  void renderConfirmBatchDeleteDialog();
  void drawProgressBar(int x, int y, int w, int h, double frac, SDL_Color fill,
                       bool rounded = false, SDL_Color bg = {35, 42, 54, 255});
  void renderProgressDialogFromState();
  void renderDisclaimerState();
  void renderCloudLoginState();
  void renderSyncOverlay();
  void renderDownloadOverlay();
  void renderSettingsState();
  void renderDiagnosticsState();
  void renderSystemTab(); // body tab HỆ THỐNG (dùng chung Diagnostics/Settings)
  void renderAboutTab(int contentTop);
  std::vector<std::string> wrapAboutText(const std::string &text,
                                         TTF_Font *font, int maxPx);
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
  std::unordered_map<std::string, std::string> m_ytStreamUrlCache; // key vid|quality
  std::vector<std::string> runYouTubeSearch(const std::string &query,
                                            int page = 1);
  void preloadYouTubeStreamUrl(const std::string &videoId);
  void triggerYouTubeSearch();
  void playYouTubeVideo(const std::string &videoId);
  void startThumbnailDownloads(const std::vector<std::string> &videoIds);
  void clearThumbnailCache();
  // Xả cache khi rời cụm nặng (IPTV/YouTube/GameCast). Xem releaseHeavyState.
  void releaseHeavyState();
  void applyYouTubeSearchResults(std::vector<std::string> results);
  void renderUploadOverlay();
  void renderToast();

  // Primitive drawing
  void drawText(const std::string &text, int x, int y, SDL_Color color,
                TTF_Font *font, bool centered = false);
  void drawFlipDigits(const std::string &newText, const std::string &oldText,
                      float t01, int x, int y, int w, int h, TTF_Font *font,
                      SDL_Color color);
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
  void drawModalDialog(int x, int y, int w, int h, int radius, SDL_Color bodyBg,
                       SDL_Color titleBg, int titleH);
  void drawBadge(int x, int y, int w, int h, const std::string &text,
                 SDL_Color bg, SDL_Color fg);
  // Chấm tròn đặc (vẽ bằng rounded-rect bán kính = r)
  void drawDot(int cx, int cy, int r, SDL_Color color);
  void drawIcon(const std::string &iconName, int x, int y, int w, int h);
  void drawPlayerIcon(const std::string &iconName, int x, int y, int w, int h);
  void drawButtonIcon(const std::string &button, int x, int y, int size);
  void drawGridIcon(const std::string &iconFile, int x, int y, int w, int h);
  // Ve icon nut + label can giua doc trong footer (fix lech text vs button)
  int textHeight(TTF_Font *font);
  int textWidth(const std::string &text, TTF_Font *font);
  // Cat chu theo pixel, khong cat giua ky tu UTF-8 (them "..." khi cat)
  std::string truncateToWidth(const std::string &text, TTF_Font *font,
                              int maxPx);
  // Rong pill fit chu theo pixel (kep MIN/MAX)
  int pillWidth(const std::string &text, TTF_Font *font);
  int badgeWidth(const std::string &text, int h);
  int buttonWidth(const std::string &label);
  int badgeDualWidth(const std::string &label1, const std::string &label2,
                     int h);
  // Chip/button/badge tron mem (phuong an A: pill full-round h/2)
  void drawPill(int x, int y, int w, int h, const std::string &text,
                bool active, TTF_Font *font = nullptr);
  // Button hanh dong (OK/Huy/Xoa...) radius BTN
  void drawButton(int x, int y, int w, int h, const std::string &label,
                  bool focused, bool danger = false);
  // Row list chuan (game/channel/file/cai dat...)
  void drawRow(int x, int y, int w, int h, bool focused, bool dim = false);
  // Y can giua doc chuan cho 1 dong text trong box h (dung FontHeight)
  int textYCentered(int y, int h, TTF_Font *font);
  // Ve cap main/sub can giua doc trong row (fix main/sub lech tam)
  void drawRowMainSub(int x, int y, int h, const std::string &main,
                      TTF_Font *fMain, const std::string &sub, TTF_Font *fSub,
                      int maxW = 0, int gap = 4);
  // Ve text canh phai that (right-align theo pixel, cho goc
  // top-right/bottom-right)
  void drawTextRight(const std::string &text, int rightX, int y,
                     SDL_Color color, TTF_Font *font);
  // Tra ve x sau khi ve xong 1 hint (de noi tiep nhieu hint). Icon va text can
  // giua doc theo bar.
  int drawFooterHint(const std::string &button, const std::string &label, int x,
                     int barY, int barH, SDL_Color color, TTF_Font *font,
                     int iconSize = 30, int gap = 8);
  // Ve chuoi footer dang "icon label   icon label..." can giua ngang + doc
  void drawFooterHintsCentered(
      const std::vector<std::pair<std::string, std::string>> &hints, int barY,
      int barH, SDL_Color color, TTF_Font *font, int iconSize = 30, int gap = 8,
      int hintGap = 28);
  // Ve 1 badge co 2 icon nut (vd: "[A] Kiem tra  •  [B] Quay lai")
  void drawBadgeDual(int x, int y, int w, int h, const std::string &btn1,
                     const std::string &label1, const std::string &btn2,
                     const std::string &label2, SDL_Color bg, SDL_Color fg);
  // Ve 1 dong text co chua token [NUT] o bat ky vi tri nao -> thay bang icon
  // that, can giua
  void drawInlineHintsCentered(const std::string &text, int centerX, int y,
                               SDL_Color color, TTF_Font *font,
                               int iconSize = 26, int gap = 6);
  // ---- UiTheme helpers (Design Tokens, khong doi logic nghiep vu) ----
  void drawAppBackground();
  void drawCard(int x, int y, int w, int h);
  void drawFocusRow(int x, int y, int w, int h);
  // Ô highlight chọn: FOCUS_BG_SOFT (trong suốt 25%) + blend cục bộ.
  // Truyền đúng rect đã căn giữa dọc theo row/khối (rule highlight).
  void drawHighlight(int x, int y, int w, int h);
  void drawAppHeader(const std::string &title, const std::string &sub = "");
  void drawHeaderStatus();
  void drawPadIcon(UiTheme::PadBtn btn, int x, int y, int size);
  void drawAppFooter(const std::vector<UiTheme::FooterHint> &hints);
  void beginModalDim();

  // Performance caches (60 FPS Smooth UI) — P2-2: dung ImageCache LRU chung.
  // Icon/logo/button/grid do m_ui.images() giu; UIManager chi giu thumb YT
  // (gioi han 24).
  ImageCache m_ytThumbCache{24};

  // P0-2 YT perf: decode thumbnail off-thread.
  // - Worker thread: IMG_Load(path) -> SDL_Surface (CPU only, no GPU)
  // - Main thread: pop surface, SDL_CreateTextureFromSurface (GPU upload)
  // SDL_Surface ownership: created on worker, consumed/freed by main thread.
  std::thread m_thumbWorker;
  std::mutex m_thumbQueueMtx;
  std::condition_variable m_thumbCv;
  std::deque<std::pair<std::string, std::string>>
      m_thumbPendingDecode; // (vid, path), priority via push_front
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

  // Gửi báo cáo lỗi thủ công chạy nền: true = đang gửi (chặn bấm lặp),
  // toast "Đang gửi..." giữ suốt tiến trình, xong mới báo kết quả.
  std::atomic<bool> m_manualReportSending{false};

  // P1-2: realtime stream telemetry from GET http://127.0.0.1:8090/api/status.
  // Polled moi 2s khi UIState=GAME_CAST, render moi frame. atomic de tranh
  // race voi HTTP fetch thread.
  struct CastStats {
    std::atomic<int> fps{0};
    std::atomic<int> viewers{0};
    std::atomic<bool> downscale{false};
    std::atomic<bool> valid{
        false}; // true sau khi fetch thanh cong it nhat 1 lan
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
  // Cache URL YouTube theo chất lượng (dùng chung UIManager <-> IPTVManager
  // cho auto-upgrade 720 + SELECT chuyển tức thì).
  std::string cachedStreamUrl(const std::string &videoId,
                              const std::string &quality);
  void cacheStreamUrl(const std::string &videoId, const std::string &quality,
                      const std::string &url);
  std::string resolveYouTubeStreamUrl(const std::string &videoId,
                                      const std::string &quality = "720");
};

} // namespace RomCloud
