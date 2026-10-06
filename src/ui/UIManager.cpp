#include "UIManager.h"
#include "../app/Application.h"
#include "../auth/AuthManager.h"
#include "../backup/BackupManager.h"
#include "../cast/CastManager.h"
#include "../config/AppConfig.h"
#include "../database/DatabaseManager.h"
#include "../database/RomIndexer.h"
#include "../database/Schema.h"
#include "../diagnostics/DeviceIdentity.h"
#include "../download/DownloadManager.h"
#include "../filesystem/FileSystemManager.h"
#include "../input/InputManager.h"
#include "../iptv/IPTVManager.h"
#include "../localsend/LocalSendManager.h"
#include "../logging/IssueLogger.h"
#include "../logging/Logger.h"
#include "../media/MpvPlayer.h"
#include "../network/HttpClient.h"
#include "../network/JsonHelper.h"
#include "../network/WebServer.h"
#include "../ota/UpdateManager.h"
#include "../platform/PlatformInfo.h"
#include "../sync/DriveSyncEngine.h"
#include "../sync/UploadManager.h"
#include "BoxartScraper.h"
#include "QrRenderer.h"
#include "TelexHelper.h"
#include "UiStrings.h"
#include "UiTheme.h"
#include <SDL2/SDL_image.h>
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>

namespace RomCloud {

UIManager &UIManager::instance() {
  static UIManager instance;
  return instance;
}

// P0-6: feedQuery - Sử dụng feed chuẩn từ YouTube Data API (chart=mostPopular)
const std::array<UIManager::Category, 9> UIManager::kYtCategories = {{
    {"all", "Thịnh hành", "feed:"},
    {"music", "Âm nhạc", "feed:10"},
    {"gaming", "Trò chơi", "feed:20"},
    {"sports", "Thể thao", "feed:17"},
    {"news", "Tin tức", "feed:25"},
    {"film", "Phim ảnh", "feed:1"},
    {"lifestyle", "Đời sống", "feed:26"},
    {"comedy", "Hài hước", "feed:23"},
    {"tech", "Công nghệ", "feed:28"},
}};

// FNV-1a 64-bit hash → 16 hex chars. Dùng cho avatar channel key/path
// để mỗi kênh có file + cache riêng (tránh hiện avatar kênh cũ khi đổi kênh).
static std::string ytAvatarHash(const std::string &s) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 0x100000001b3ULL;
  }
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx",
                static_cast<unsigned long long>(h));
  return std::string(buf);
}

void UIManager::initGridMenu() {
  // Hàng trên (xem/chơi gần nhau): game, TV, YouTube, cast, file.
  // Hàng dưới: còn lại.
  m_gridMenuItems = {
      {"games", "THƯ VIỆN", "GAMES.png", "Danh sách ROM"},
      {"iptv", "XEM TV", "TV.png", "Kênh TV online"},
      {"youtube", "YOUTUBE", "YOUTUBE.png", "YouTube"},
      {"cast", "GAME CAST", "CAST.png", "Cast lên TV & Laptop"},
      {"explorer", "FILE EXPLORER", "FOLDER.png", "Duyet file SD"},
      {"weather", "OFFICEGO", "OFFICEGO.png", "Thời tiết & Lịch"},
      {"localsend", "LOCALSEND", "LOCALSEND.png", "Chia se P2P trong LAN"},
      {"upload", "TẢI LÊN", "UPLOAD.png", "Upload lên Drive"},
      {"settings", "CÀI ĐẶT", "SETTINGS.png", "Cấu hình"}};
}

bool UIManager::init(SDL_Window *window, SDL_Renderer *renderer) {
  m_window = window;
  m_renderer = renderer;

  // Initialize grid menu
  initGridMenu();

  if (TTF_Init() == -1) {
    Logger::error(std::string("TTF_Init failed: ") + TTF_GetError());
    return false;
  }

  std::string fontPath = AppConfig::instance().getFontPath();
  // Ưu tiên NotoSans-Regular của app (full TV), font hệ thống fallback sau.
  const char *fallbackFonts[] = {
      "/mnt/SDCARD/Apps/RomCloud/assets/fonts/NotoSans-Regular.ttf",
      "assets/fonts/NotoSans-Regular.ttf",
      "/rom/usr/trimui/res/full.ttf",
      "/usr/trimui/res/full.ttf",
      "/usr/trimui/res/regular.ttf",
      "/mnt/SDCARD/Themes/TRIMUI YaHei/msyh.ttf",
      fontPath.c_str()};

  for (const char *path : fallbackFonts) {
    if (!m_fontTitle)
      m_fontTitle = TTF_OpenFont(path, 42);
    if (!m_fontHuge)
      m_fontHuge = TTF_OpenFont(path, 100);
    if (!m_fontLarge)
      m_fontLarge = TTF_OpenFont(path, 36);
    if (!m_fontMedium)
      m_fontMedium = TTF_OpenFont(path, 30);
    if (!m_fontSmall)
      m_fontSmall = TTF_OpenFont(path, 24);
    if (m_fontTitle && m_fontHuge && m_fontLarge && m_fontMedium &&
        m_fontSmall) {
      Logger::info(std::string("Loaded TTF font from: ") + path +
                   " (Sizes: 42, 100, 36, 30, 24)");
      break;
    }
  }

  if (!m_fontLarge || !m_fontMedium || !m_fontSmall) {
    Logger::warn("Could not load desired font point sizes.");
  }

  // Font số đồng hồ: Rajdhani Bold (SIL OFL 1.1, xem OFL-Rajdhani.txt).
  // Chỉ chứa số/dấu nên thiếu là bỏ qua, fallback về font hệ.
  {
    std::string clockPath =
        AppConfig::instance().getFontsDir() + "/Rajdhani-Bold.ttf";
    m_fontClock = TTF_OpenFont(clockPath.c_str(), 140);
    if (!m_fontClock)
      m_fontClock = TTF_OpenFont("assets/fonts/Rajdhani-Bold.ttf", 140);
    {
      std::string clockSm =
          AppConfig::instance().getFontsDir() + "/Rajdhani-Bold.ttf";
      m_fontClockSm = TTF_OpenFont(clockSm.c_str(), 60);
      if (!m_fontClockSm)
        m_fontClockSm = TTF_OpenFont("assets/fonts/Rajdhani-Bold.ttf", 60);
      std::string clockFs =
          AppConfig::instance().getFontsDir() + "/Rajdhani-Bold.ttf";
      m_fontClockFs = TTF_OpenFont(clockFs.c_str(), 260);
      if (!m_fontClockFs)
        m_fontClockFs = TTF_OpenFont("assets/fonts/Rajdhani-Bold.ttf", 260);
      std::string clockMd =
          AppConfig::instance().getFontsDir() + "/Rajdhani-Bold.ttf";
      m_fontClockMd = TTF_OpenFont(clockMd.c_str(), 200);
      if (!m_fontClockMd)
        m_fontClockMd = TTF_OpenFont("assets/fonts/Rajdhani-Bold.ttf", 200);
      // Font số flip clock (Fliqlo, ISC — xem gluqlo-ISC.txt). Chỉ có
      // 0-9AMP, cặp số tabular nên không cần bù trừ số "1" hẹp.
      std::string flipPath =
          AppConfig::instance().getFontsDir() + "/gluqlo.ttf";
      m_fontFlip = TTF_OpenFont(flipPath.c_str(), 260);
      if (!m_fontFlip)
        m_fontFlip = TTF_OpenFont("assets/fonts/gluqlo.ttf", 260);
    }
    if (!m_fontClock)
      Logger::warn("Clock font Rajdhani-Bold missing, fallback NotoSans.");
    Logger::info(std::string("Clock fonts: flip=") +
                 (m_fontFlip ? "ok" : "MISSING") +
                 " clockMd=" + (m_fontClockMd ? "ok" : "MISSING") +
                 " clock=" + (m_fontClock ? "ok" : "MISSING") +
                 " fs=" + (m_fontClockFs ? "ok" : "MISSING"));
  }

  CoverManager::instance().init(m_renderer);
  m_ui.bind(m_renderer, m_fontSmall, m_fontMedium, m_fontLarge,
            AppConfig::instance().getAssetsDir());
  refreshSystems();

  // Auto-check for OTA updates in background on launch
  UpdateManager::instance().checkForUpdatesAsync(
      [this](bool hasUpdate, const UpdateInfo &info) {
        if (hasUpdate) {
          showToast(std::string(UiStrings::TOAST_NEW_OTA_PREFIX) +
                        info.remoteVersion + "!",
                    {34, 197, 94, 255}, 6000);
        } else if (UpdateManager::instance().repairIfBroken()) {
          // Máy lên đời bằng OTA binary-only đời cũ, thiếu file runtime
          // (YouTube/script/icon) → tự vá full-zip ngay, không cần bấm gì.
          showToast("Phát hiện thiếu file, tự tải bổ sung...",
                    {245, 158, 11, 255}, 6000);
        }
      });

  // Tự vá xong → báo + tự khởi động lại để nhận binary mới.
  UpdateManager::instance().setOnRepairCompleted([this](bool ok) {
    if (ok) {
      showToast("Đã tự sửa xong! Khởi động lại...", {34, 197, 94, 255}, 5000);
      std::this_thread::sleep_for(std::chrono::seconds(3));
      Application::instance().requestRestart();
    } else {
      showToast("Tự sửa thất bại. Vào Cập nhật thử lại.", {239, 68, 68, 255},
                5000);
    }
  });

  // LocalSend: wire callback từ background thread (HTTP) sang UI state.
  // Modal overlay toàn màn hình — switch sang LOCALSEND_INCOMING bất kể
  // user đang ở state nào (Game List, IPTV, Settings...).
  LocalSendManager::instance().setOnUserPrompt([this](
                                                   const LsUploadRequest &req) {
    m_localSendCurrentPrompt = req;
    m_localSendPendingSessionId = req.sessionId;
    m_localSendIncomingMode = 0;
    m_localSendIncomingSavePath =
        req.savedPath.empty() ? "/mnt/SDCARD/Downloads" : req.savedPath;
    if (!m_localSendIncomingSavePath.empty()) {
      size_t slash = m_localSendIncomingSavePath.find_last_of('/');
      if (slash != std::string::npos &&
          m_localSendIncomingSavePath.find('.', slash) != std::string::npos) {
        m_localSendIncomingSavePath =
            m_localSendIncomingSavePath.substr(0, slash);
      }
    }
    FileSystemManager::instance().createDirectoryRecursive(
        m_localSendIncomingSavePath);
    setState(UIState::LOCALSEND_INCOMING);
  });

  // P0-2: start thumbnail decode worker thread
  startThumbWorker();

  // P1-2: CastManager callback — khi FN switch hoặc A press thay đổi stream
  // state, show toast để user biết hệ thống đã phản hồi (im lặng trước đó).
  // Callback chạy trên UI thread (gọi từ UIManager::update()).
  CastManager::instance().setFnToggleCallback(
      [this](bool running, bool viaFnSwitch) {
        if (viaFnSwitch) {
          showToast(running ? "GameCast: BẬT bởi công tắc FN"
                            : "GameCast: TẮT bởi công tắc FN",
                    running ? SDL_Color{34, 197, 94, 255}
                            : SDL_Color{148, 163, 184, 255},
                    1800);
        }
      });

  // P3-D: nếu user gạt FN = ON khi Wi-Fi mất, CastManager skip start() và gọi
  // callback này để UI hiển thị toast cảnh báo (tránh "im lặng" gây hiểu nhầm).
  CastManager::instance().setNoWifiFnCallback([this]() {
    showToast("⚠ Không thể bật Cast - Wi-Fi chưa kết nối (vào Settings)",
              {252, 165, 165, 255}, 2500);
  });

  // P3-B: khi gamecast_d bị kill ngoài (OOM, crash, ...) và tự restart thành
  // công / thất bại, báo user qua toast. Reset elapsed time nếu restart thành
  // công (m_streamStartedAt sẽ được update trong start()).
  CastManager::instance().setAutoReconnectCallback([this](bool succeeded) {
    if (succeeded) {
      showToast("↻ Cast bị ngắt - đã tự khởi động lại", {59, 130, 246, 255},
                2500);
    } else {
      showToast("✗ Cast bị ngắt - không thể tự khởi động lại (bấm A thử lại)",
                {239, 68, 68, 255}, 3500);
    }
  });

  return true;
}

void UIManager::shutdown() {
  unlink("/tmp/stay_awake");
  // P0-2: stop thumb worker thread before destroying renderer
  stopThumbWorker();
  CoverManager::instance().shutdown();
  m_ui.unbind();

  // P2-2: icon/logo/button/grid do m_ui.images() giu (unbind da clear).
  clearTextCache();
  clearThumbnailCache();

  if (m_fontTitle) {
    TTF_CloseFont(m_fontTitle);
    m_fontTitle = nullptr;
  }
  if (m_fontHuge) {
    TTF_CloseFont(m_fontHuge);
    m_fontHuge = nullptr;
  }
  if (m_fontClock) {
    TTF_CloseFont(m_fontClock);
    m_fontClock = nullptr;
  }
  if (m_fontClockSm) {
    TTF_CloseFont(m_fontClockSm);
    m_fontClockSm = nullptr;
  }
  if (m_fontClockFs) {
    TTF_CloseFont(m_fontClockFs);
    m_fontClockFs = nullptr;
  }
  if (m_fontClockMd) {
    TTF_CloseFont(m_fontClockMd);
    m_fontClockMd = nullptr;
  }
  if (m_fontFlip) {
    TTF_CloseFont(m_fontFlip);
    m_fontFlip = nullptr;
  }
  if (m_fontLarge) {
    TTF_CloseFont(m_fontLarge);
    m_fontLarge = nullptr;
  }
  if (m_fontMedium) {
    TTF_CloseFont(m_fontMedium);
    m_fontMedium = nullptr;
  }
  if (m_fontSmall) {
    TTF_CloseFont(m_fontSmall);
    m_fontSmall = nullptr;
  }

  TTF_Quit();
}

// Cụm state nặng (video/thumb/worker): rời khỏi cụm là xả để màn tiếp
// theo sẵn sàng, không giật. Trong cụm đi lại với nhau thì giữ nguyên
// (vd HOME<->RESULTS giữ thumb, chuyển kênh nhanh giữ URL cache).
static bool isHeavyState(UIState s) {
  return s == UIState::IPTV_PLAYLIST_SELECT || s == UIState::IPTV_LIST ||
         s == UIState::IPTV_SEARCH || s == UIState::YOUTUBE_HOME ||
         s == UIState::YOUTUBE_RESULTS || s == UIState::YOUTUBE_SEARCH ||
         s == UIState::GAME_CAST;
}

bool UIManager::goBack() {
  while (!m_stateHistory.empty()) {
    UIState prev = m_stateHistory.back();
    m_stateHistory.pop_back();
    if (prev == m_currentState)
      continue; // bỏ trùng, lùi tiếp
    if (prev == UIState::EXIT_REQUESTED)
      continue;
    // Qua setState để giữ nguyên side-effect từng trang, nhưng không
    // push ngược lại stack.
    m_suppressPush = true;
    setState(prev);
    m_suppressPush = false;
    Logger::error("[STATE] BACK");
    return true;
  }
  return false;
}

void UIManager::setState(UIState state) {
  // Rời launcher -> hủy arm thoát (tránh B ở trang khác rồi về bấm B thoát
  // oan).
  if (m_currentState == UIState::MENU && state != UIState::MENU)
    m_exitArmedMs = 0;
  if ((m_currentState == UIState::YOUTUBE_RESULTS ||
       m_currentState == UIState::YOUTUBE_SEARCH) &&
      (state != UIState::YOUTUBE_RESULTS && state != UIState::YOUTUBE_SEARCH)) {
    // Chi xa GPU cache (VRAM). GIU file /tmp/yt_thumbs/*: render tu
    // nap lai texture tu file trong 1-2 frame (drawYtStandardThumb) nen
    // Back tu video ve list/Home khong bi mat thumbnail. Xoa file o day
    // la ly do list tran truoc day (phai tai lai het tu mang).
    clearThumbnailCache();
  }

  Logger::error("[STATE] SET");
  // Smart cleaner: rời cụm nặng (IPTV/YouTube/GameCast) ra ngoài thì xả
  // cache chữ + thumb + pagecache video để màn tiếp theo sẵn sàng, không
  // giật. Trong cụm giữ nguyên (chuyển kênh/video nhanh).
  if (isHeavyState(m_currentState) && !isHeavyState(state))
    releaseHeavyState();
  // B = back: lưu trang hiện tại vào stack (trừ lặp liên tiếp, trừ EXIT).
  // Giới hạn 30 để không phình RAM.
  if (state != UIState::EXIT_REQUESTED && state != m_currentState) {
    if (!m_suppressPush &&
        (m_stateHistory.empty() || m_stateHistory.back() != m_currentState)) {
      m_stateHistory.push_back(m_currentState);
      if (m_stateHistory.size() > 30)
        m_stateHistory.erase(m_stateHistory.begin());
    }
  }
  if (state == UIState::EXIT_REQUESTED)
    m_stateHistory.clear();
  m_currentState = state;
  InputManager::instance().reset();
  if (state == UIState::SYSTEM_SELECT) {
    refreshSystems();
  } else if (state == UIState::YOUTUBE_SEARCH) {
    loadYouTubeHistory();
    m_ytSearchFocus = 2;
    m_ytHistoryRow = 0;
    m_ytHistoryCol = 0;
    m_ytVk.query = m_ytKeyboardQuery;
  } else if (state == UIState::YOUTUBE_HOME) {
    // P0-6: HOME state init
    Logger::error(
        "[YT] ENTER HOME (view=" + std::string(m_ytViewMode ? "grid" : "row") +
        ", cached=" + std::to_string(m_ytAllCachedResults.size()) + ")");
    loadYouTubeHistory();
    loadYouTubeViewMode();
    m_ytSearchModalOpen = false;
    m_ytSelectedCategory = 0;
    m_ytCategoryScrollOffset = 0;
    m_ytHomeContentSelected = 0;
    m_ytHomeContentScrollOffset = 0;
    m_ytHomePage = 1;
    m_ytHomeFocus = 1; // start on tag pills
    m_ytHomeLoadedThisEnter = false;
    // Auto-feed first time HOME entered (no cached results). Khi user đã
    // search 1 query rồi thì khi quay lại HOME (từ RESULTS) giữ nguyên cache
    // và query → user thấy lại content vừa search, không bị clobber.
    if (m_ytAllCachedResults.empty()) {
      const char *fq = kYtCategories[0].feedQuery;
      if (fq && fq[0] != '\0') {
        m_ytVk.query.clear();
        m_ytKeyboardQuery.clear();
        runYouTubeHomeSearch(fq);
      }
    }
    m_ytHomeLoadedThisEnter = true;
  } else if (state == UIState::CLOUD_LOGIN) {
    // Vào màn liên kết: tự lấy mã thiết bị nền (user chỉ nhập mã trên ĐT).
    m_cloudPortalView = false;
    m_cloudAutoSync = AuthManager::instance().isLinked();
    if (!AuthManager::instance().isLinked()) {
      AuthManager::instance().cancelDeviceFlow();
      std::thread([] { AuthManager::instance().startDeviceFlow(); }).detach();
    }
  } else if (state == UIState::CONFIRM_DELETE) {
    openConfirmDeleteDialog();
  } else if (state == UIState::CONFIRM_BATCH_DELETE) {
    openConfirmBatchDeleteDialog();
  }
}

void UIManager::showToast(const std::string &message, SDL_Color color,
                          uint32_t durationMs) {
  uint32_t packed = ((uint32_t)color.r << 24) | ((uint32_t)color.g << 16) |
                    ((uint32_t)color.b << 8) | (uint32_t)color.a;
  m_dialogs.toastMsg(message, packed, durationMs);
}

void UIManager::refreshSystems() {
  m_cachedSystems = DatabaseManager::instance().getSystems(true);
}

void UIManager::refreshGames() {
  int filterInt = static_cast<int>(m_filterMode);
  m_cachedGames = DatabaseManager::instance().getGamesBySystem(
      m_activeSystem.id, filterInt);
  if (m_selectedGameIndex >= static_cast<int>(m_cachedGames.size())) {
    m_selectedGameIndex =
        std::max(0, static_cast<int>(m_cachedGames.size()) - 1);
  }
}

void UIManager::triggerManualSync() {
  if (DriveSyncEngine::instance().isSyncing() || isIndexing()) {
    showToast(UiStrings::TOAST_SYNCING_DRIVE, {245, 158, 11, 255});
    return;
  }
  showToast(UiStrings::TOAST_SCANNING_SD, {0, 180, 216, 255}, 2000);

  // P0-3: dua tien trinh quet SD len ProgressDialog dung chung (thay vi chi
  // toast).
  m_dialogs.progress.open("Dang quet the SD...", false);
  auto systems = DatabaseManager::instance().getSystems(false);
  uint64_t totalSys = systems.empty() ? 1 : systems.size();
  m_indexTask.run([this, totalSys](TaskProgress &p) {
    p.total = totalSys;
    p.done = 0;
    std::string romsDir = AppConfig::instance().getRomsDir();
    auto all = DatabaseManager::instance().getSystems(false);
    uint64_t idx = 0;
    for (const auto &sys : all) {
      if (p.cancel.load())
        break;
      RomIndexer::instance().scanSystem(sys, romsDir, nullptr);
      p.done = ++idx;
    }
    BoxartScraper::instance().startAutoScrapeSdCard(false);
    m_needLibraryRefresh = true;
    if (AuthManager::instance().isLinked()) {
      DriveSyncEngine::instance().startSync();
    }
  });
}

// Đọc ~300 ký tự cuối file stderr của script python để chẩn đoán khi
// search về rỗng (trước đây 2>/dev/null nên lỗi câm hoàn toàn).
static std::string ytErrTail(const char* path) {
  std::string out;
  FILE* f = fopen(path, "rb");
  if (!f) return out;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, sz > 300 ? sz - 300 : 0, SEEK_SET);
  char buf[320];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  if (n == 0) return out;
  buf[n] = '\0';
  out = buf;
  for (char& c : out)
    if (c == '\n' || c == '\r') c = ' ';
  if (out.size() > 200) out = out.substr(out.size() - 200);
  return out;
}

void UIManager::update() {
  auto &input = InputManager::instance();

  // P0-2: Drain thumbnail decode worker surfaces every frame (prevents RAM
  // leaks)
  drainReadyThumbs();

  // Poll Hardware FN Switch for GameCast stream toggle
  CastManager::instance().update();

  // Đồng hồ: báo thức + pomodoro (poll 5s, giờ theo máy Brick)
  pollClock();

  // Giá cả (tab 3) + Thị trường crypto/chứng khoán (tab 4):
  // live 10 phút/lần khi đang ở tab tương ứng.
  pollMarket();
  pollWatch();

  // CLOUD_LOGIN: vừa link xong -> toast + tự sync, user khỏi bấm thêm.
  if (m_currentState == UIState::CLOUD_LOGIN &&
      AuthManager::instance().isLinked() && !m_cloudAutoSync) {
    m_cloudAutoSync = true;
    showToast("Đã liên kết Drive! Đang đồng bộ...", {34, 197, 94, 255}, 2500);
    DriveSyncEngine::instance().startSync();
  }

  // P1-2: Poll /api/status moi 2s khi dang o man GAME_CAST va co stream chay.
  // Fetch qua detached thread de khong block UI; atomic de render doc gia tri
  // moi frame nhanh.
  if (m_currentState == UIState::GAME_CAST &&
      CastManager::instance().isRunning()) {
    uint32_t now = SDL_GetTicks();
    if (now - m_lastCastStatsPoll >= 2000) {
      m_lastCastStatsPoll = now;
      std::thread([this]() {
        auto resp = HttpClient::instance().get(
            "http://127.0.0.1:8090/api/status", {}, 1);
        if (!resp.success || resp.body.empty())
          return;
        // JSON format (ASCII-only):
        // {"running":true,"fps":N,"viewers":N,"downscale":bool} parse don gian
        // bang find + stoi (tranh cat giua UTF-8 vi toan ASCII).
        auto parseInt = [&](const std::string &key) -> int {
          std::string pat = "\"" + key + "\":";
          size_t pos = resp.body.find(pat);
          if (pos == std::string::npos)
            return -1;
          pos += pat.size();
          return std::atoi(resp.body.c_str() + pos);
        };
        auto parseBool = [&](const std::string &key) -> bool {
          std::string pat = "\"" + key + "\":";
          size_t pos = resp.body.find(pat);
          if (pos == std::string::npos)
            return false;
          pos += pat.size();
          return (resp.body.compare(pos, 4, "true") == 0);
        };
        int fps = parseInt("fps");
        int viewers = parseInt("viewers");
        if (fps >= 0)
          m_castStats.fps.store(fps);
        if (viewers >= 0)
          m_castStats.viewers.store(viewers);
        m_castStats.downscale.store(parseBool("downscale"));
        m_castStats.valid.store(true);
      }).detach();
    }
  }

  // Check if download finished
  auto dlProg = DownloadManager::instance().getProgress();
  if (dlProg.state == DownloadState::COMPLETED) {
    std::string finishedTitle = dlProg.gameTitle;
    DownloadManager::instance().resetProgress();
    refreshSystems();
    refreshGames();
    showToast("Đã tải xong: " + finishedTitle + "!", {34, 197, 94, 255}, 3000);
    // Auto-start next item in queue
    DownloadManager::instance().processNextInQueue();
  } else if (dlProg.state == DownloadState::FAILED) {
    std::string err = dlProg.errorMessage.empty()
                          ? UiStrings::TOAST_UNKNOWN_ERROR
                          : dlProg.errorMessage;
    DownloadManager::instance().resetProgress();
    refreshSystems();
    refreshGames();
    showToast("Tải thất bại: " + err, {239, 68, 68, 255}, 3500);
    // Try next in queue even after failure
    DownloadManager::instance().processNextInQueue();
  }

  // P0-3: poll tien trinh quet SD tu BackgroundTask -> ProgressDialog dung
  // chung. Dat truoc early-return sync de dialog tu dong dong khi sync noi
  // tiep.
  if (isIndexing()) {
    const auto &tp = m_indexTask.progress();
    m_dialogs.progress.update(tp.done.load(),
                              tp.total.load() == 0 ? 1 : tp.total.load());
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%llu / %llu he may",
                  (unsigned long long)tp.done.load(),
                  (unsigned long long)tp.total.load());
    m_dialogs.progress.detail = buf;
  } else if (m_dialogs.progress.visible &&
             m_dialogs.progress.title == "Dang quet the SD...") {
    m_dialogs.progress.close();
  }
  // P2-1: Explorer task (copy/xoa) -> ProgressDialog dung chung + forward
  // toast/confirm.
  syncExplorerDialogs();

  // If sync is running, allow cancel button [B]
  if (DriveSyncEngine::instance().isSyncing()) {
    if (input.isButtonJustPressed(Button::B)) {
      DriveSyncEngine::instance().cancelSync();
      showToast(UiStrings::TOAST_SYNC_CANCELLED, {245, 158, 11, 255});
    }
    return;
  }

  // Check if local library indexing completed
  if (m_needLibraryRefresh.exchange(false)) {
    refreshSystems();
    refreshGames();
    if (!AuthManager::instance().isLinked()) {
      showToast(UiStrings::TOAST_SD_SCANNED_NO_DRIVE, {34, 197, 94, 255}, 3000);
    }
  }

  // Check if sync completed or errored
  auto syncProg = DriveSyncEngine::instance().getProgress();
  if (syncProg.status == SyncStatus::COMPLETED) {
    DriveSyncEngine::instance().init();
    refreshSystems();
    refreshGames();
    showToast("Đồng bộ hoàn tất: Đã lưu " +
                  std::to_string(syncProg.cloudGamesFound) +
                  " game vào thư viện!",
              {34, 197, 94, 255});
  } else if (syncProg.status == SyncStatus::ERROR_OCCURRED) {
    std::string err = syncProg.errorMessage.empty() ? "Lỗi đồng bộ Google Drive"
                                                    : syncProg.errorMessage;
    DriveSyncEngine::instance().init();
    showToast(err, {239, 68, 68, 255}, 4000);
  }

  // Check if YouTube search finished
  if (m_ytSearchFinished.exchange(false)) {
    m_ytIsSearching = false;
    if (!m_ytSearchResults.empty()) {
      if (m_currentState == UIState::YOUTUBE_SEARCH) {
        setState(UIState::YOUTUBE_RESULTS);
        m_ytSearchSelectedIndex = 0;
        m_ytSearchScrollOffset = 0;

        // Collect video IDs and start background thumbnail downloads
        std::vector<std::string> vids;
        for (const auto &item : m_ytSearchResults) {
          size_t p = item.find('|');
          if (p != std::string::npos) {
            vids.push_back(item.substr(0, p));
          }
        }
        startThumbnailDownloads(vids);
      }
    } else {
      if (m_currentState == UIState::YOUTUBE_SEARCH) {
        std::string msg = m_ytErrorMessage.empty() ? "Không tìm thấy video nào"
                                                   : m_ytErrorMessage;
        showToast(msg, {245, 158, 11, 255}, 3500);
      }
    }
  }

  // YouTube search results handoff (main thread safe)
  if (m_ytSearchDataReady.exchange(false)) {
    std::vector<std::string> results;
    {
      std::lock_guard<std::mutex> lk(m_ytSearchMutex);
      results = std::move(m_ytPendingSearchResults);
    }
    applyYouTubeSearchResults(std::move(results));
  }

  // Watchdog: task search chết câm (exception trong worker) mà không trả
  // cờ/search-finished -> reset để lần tìm sau chạy được, không kẹt vĩnh viễn.
  if (m_ytIsSearching && !m_ytSearchTask.isRunning() &&
      !m_ytSearchDataReady.load() && !m_ytSearchFinished.load() &&
      SDL_GetTicks() - m_ytSearchStartMs > 120000) {
    Logger::warn("[YouTube] search watchdog: stuck >120s, reset flag");
    m_ytIsSearching = false;
    showToast("Tìm kiếm bị kẹt, thử lại", {245, 158, 11, 255}, 2500);
  }

  // Check if YouTube video stream resolution finished
  if (m_ytVideoReady.exchange(false)) {
    m_ytIsLoadingVideo = false;
    if (!m_ytPendingStreamUrl.empty()) {
      std::string url = m_ytPendingStreamUrl;
      std::string vid = m_ytPendingVideoId;
      std::string vtitle = m_ytPendingVideoTitle;
      std::string vq = m_ytPendingQuality.empty() ? "720" : m_ytPendingQuality;
      m_ytPendingStreamUrl.clear();
      m_ytPendingVideoId.clear();
      m_ytPendingVideoTitle.clear();
      m_ytPendingQuality.clear();
      IPTVManager::instance().playYouTubeVideo(vid, url, vq, vtitle);
      setState(UIState::YOUTUBE_RESULTS);
    } else {
      showToast("Không thể lấy link phát video", {239, 68, 68, 255}, 3000);
    }
  }

  // Tiến trình tải video: toast lại mỗi 5s kèm số giây đã chờ
  // (resolve + mpv đệm có thể >15s, tránh cảm giác treo).
  if (m_ytIsLoadingVideo && !m_ytVideoReady.load()) {
    uint32_t nowMs = SDL_GetTicks();
    if (nowMs - m_ytLoadToastMs >= 5000) {
      m_ytLoadToastMs = nowMs;
      int secs = (int)((nowMs - m_ytLoadStartMs) / 1000);
      showToast("Đang tải video... " + std::to_string(secs) + "s",
                UiTheme::ACCENT_CYAN, 4500);
    }
  }

  static AuthState lastAuthState = AuthManager::instance().getState();
  AuthState curAuthState = AuthManager::instance().getState();
  if (curAuthState == AuthState::LINKED && lastAuthState != AuthState::LINKED) {
    showToast(UiStrings::TOAST_GOOGLE_LOGIN_SUCCESS, {34, 197, 94, 255}, 4000);
    refreshSystems();
  }
  lastAuthState = curAuthState;

  switch (m_currentState) {
  case UIState::MENU: {
    int itemCount = static_cast<int>(m_gridMenuItems.size());

    // Lưới 5x2 vừa 1 màn hình (10 icon, không carousel/pager).
    const int MCOLS = 5;
    int mcount = itemCount > 0 ? itemCount : 1;
    int rows = (mcount + MCOLS - 1) / MCOLS;
    int row = m_selectedMenuIndex / MCOLS;
    int col = m_selectedMenuIndex % MCOLS;
    if (row < 0)
      row = 0;
    if (row >= rows)
      row = rows - 1;
    if (input.isButtonJustPressed(Button::LEFT)) {
      int nc = (col + MCOLS - 1) % MCOLS;
      int ni = row * MCOLS + nc;
      if (ni >= mcount)
        ni = mcount - 1;
      m_selectedMenuIndex = ni;
    } else if (input.isButtonJustPressed(Button::RIGHT)) {
      int nc = (col + 1) % MCOLS;
      int ni = row * MCOLS + nc;
      if (ni >= mcount)
        ni = row * MCOLS; // hàng cuối thiếu ô -> về đầu hàng
      m_selectedMenuIndex = ni;
    } else if (input.isButtonJustPressed(Button::UP)) {
      int ni = ((row + rows - 1) % rows) * MCOLS + col;
      if (ni >= mcount)
        ni = mcount - 1;
      m_selectedMenuIndex = ni;
    } else if (input.isButtonJustPressed(Button::DOWN)) {
      int ni = ((row + 1) % rows) * MCOLS + col;
      if (ni >= mcount)
        ni = mcount - 1;
      m_selectedMenuIndex = ni;
    } else if (input.isButtonJustPressed(Button::START)) {
      m_settingsTab = UpdateManager::instance().isUpdateAvailable() ? 2 : 0;
      m_selectedSettingsRow = 0;
      m_settingsScrollOffset = 0;
      setState(UIState::SETTINGS);
    } else if (input.isButtonJustPressed(Button::SELECT)) {
      // INFO đã merge vào Cài đặt tab GIỚI THIỆU.
      m_settingsTab = 4;
      m_selectedSettingsRow = 0;
      m_settingsScrollOffset = 0;
      setState(UIState::SETTINGS);
    } else if (input.isButtonJustPressed(Button::A)) {
      // Handle menu selection based on id
      std::string selectedId = m_gridMenuItems[m_selectedMenuIndex].id;

      if (selectedId == "games") {
        setState(UIState::SYSTEM_SELECT);
      } else if (selectedId == "weather") {
        openWeather();
      } else if (selectedId == "iptv") {
        if (IPTVManager::instance().playlistCount() == 0) {
          IPTVManager::instance().loadPlaylists();
        }
        m_selectedPlaylistIndex = 0;
        m_playlistScrollOffset = 0;
        // Neu co nhieu hon 1 playlist -> hien man hinh chon playlist
        // Neu chi co 1 (hoac 0) -> vao thang danh sach kenh
        if (IPTVManager::instance().playlistCount() > 1) {
          setState(UIState::IPTV_PLAYLIST_SELECT);
        } else {
          m_selectedIPTVChannelIndex = 0;
          m_iptvScrollOffset = 0;
          setState(UIState::IPTV_LIST);
        }
      } else if (selectedId == "youtube") {
        VirtualKeyboard::reset(m_ytVk, true);
        m_ytVk.charset = 1;
        m_ytSearchResults.clear();
        m_ytSearchSelectedIndex = 0;
        m_ytSearchScrollOffset = 0;
        m_ytErrorMessage.clear();
        m_ytIsSearching = false;
        m_ytSearchFinished = false;
        m_ytIsLoadingVideo = false;
        m_ytVideoReady = false;
        m_ytPendingStreamUrl.clear();
        m_ytPendingQuality.clear();
        setState(UIState::YOUTUBE_HOME);
      } else if (selectedId == "localsend") {
        LocalSendManager::instance().start();
        LocalSendManager::instance().refreshDiscovery();
        m_localSendSelectedDevice = 0;
        m_localSendFolderSelected = 0;
        setState(UIState::LOCALSEND_HOME);
      } else if (selectedId == "cast") {
        CastManager::instance().init();
        setState(UIState::GAME_CAST);
      } else if (selectedId == "explorer") {
        m_expL.open("/mnt/SDCARD");
        m_expR.open("/mnt/SDCARD");
        m_expActive = 0;
        m_expScroll[0] = 0;
        m_expScroll[1] = 0;
        setState(UIState::FILE_EXPLORER);
      } else if (selectedId == "upload") {
        if (!AuthManager::instance().isLinked()) {
          showToast(UiStrings::TOAST_CONNECT_DRIVE_FIRST, {245, 158, 11, 255});
        } else if (!AuthManager::instance().canUpload()) {
          showToast(UiStrings::TOAST_CONNECT_PERSONAL_DRIVE,
                    {245, 158, 11, 255}, 4000);
        } else {
          UploadManager::instance().startReverseSync();
          setState(UIState::REVERSE_SYNC);
        }
      } else if (selectedId == "settings") {
        // P5: Settings giờ có 2 tab [CHUNG] [CẬP NHẬT]. Nếu có bản OTA mới,
        // auto-switch sang tab CẬP NHẬT để badge "NEW" trên icon Cài đặt
        // (renderMenuState) dẫn thẳng tới màn cập nhật — không phải click
        // thêm L1.
        m_settingsTab = UpdateManager::instance().isUpdateAvailable() ? 2 : 0;
        m_selectedSettingsRow = 0;
        m_settingsScrollOffset = 0;
        setState(UIState::SETTINGS);
        // Refetch để chắc badge là fresh (background check có thể cũ)
        UpdateManager::instance().checkForUpdatesAsync();
      } else if (selectedId == "info") {
        // INFO đã merge vào Cài đặt tab GIỚI THIỆU (giữ tương thích).
        m_settingsTab = 4;
        m_selectedSettingsRow = 0;
        m_settingsScrollOffset = 0;
        setState(UIState::SETTINGS);
      }
    } else if (input.isButtonJustPressed(Button::B)) {
      // B 2 lần trong 3s để thoát app (thay nút THOÁT đã dọn).
      // Lần 1 toast 3s, hết 3s không bấm nữa thì ở lại app.
      uint32_t nowMs = SDL_GetTicks();
      if (m_exitArmedMs != 0 && nowMs - m_exitArmedMs < 3000) {
        m_exitArmedMs = 0;
        setState(UIState::EXIT_REQUESTED);
      } else {
        m_exitArmedMs = nowMs;
        showToast("Bấm B thêm 1 lần nữa để thoát app", {245, 158, 11, 255},
                  3000);
      }
    }
    break;
  }

  case UIState::SYSTEM_SELECT: {
    int total = static_cast<int>(m_cachedSystems.size());
    if (total > 0) {
      if (input.isButtonJustPressed(Button::UP)) {
        m_selectedSystemIndex = (m_selectedSystemIndex - 1 + total) % total;
      } else if (input.isButtonJustPressed(Button::DOWN)) {
        m_selectedSystemIndex = (m_selectedSystemIndex + 1) % total;
      } else if (input.isButtonJustPressed(Button::L1)) {
        m_selectedSystemIndex = std::max(0, m_selectedSystemIndex - 6);
      } else if (input.isButtonJustPressed(Button::R1)) {
        m_selectedSystemIndex = std::min(total - 1, m_selectedSystemIndex + 6);
      } else if (input.isButtonJustPressed(Button::A)) {
        m_activeSystem = m_cachedSystems[m_selectedSystemIndex];
        m_selectedGameIndex = 0;
        m_gameScrollOffset = 0;
        refreshGames();
        setState(UIState::GAME_LIST);
      } else if (input.isButtonJustPressed(Button::Y)) {
        triggerManualSync();
      }
    }
    if (input.isButtonJustPressed(Button::B)) {
      setState(UIState::MENU);
    }
    break;
  }

  case UIState::GAME_LIST: {
    int total = static_cast<int>(m_cachedGames.size());
    int pageSize = 7;

    // Multi-select mode handling
    if (input.isButtonJustPressed(Button::L2)) {
      m_multiSelectMode = !m_multiSelectMode;
      if (!m_multiSelectMode) {
        m_selectedGameIds.clear();
        showToast(UiStrings::MULTI_SELECT_DISABLED, {168, 85, 247, 255}, 2000);
      } else {
        showToast(std::string(UiStrings::MULTI_SELECT_ENABLED) + ". " +
                      UiStrings::MULTI_SELECT_HINT,
                  {168, 85, 247, 255}, 3000);
      }
    }

    if (m_multiSelectMode && total > 0) {
      // Multi-select mode: different controls
      if (input.isButtonJustPressed(Button::UP) ||
          input.isButtonJustPressed(Button::DOWN) ||
          input.isButtonJustPressed(Button::L1) ||
          input.isButtonJustPressed(Button::R1)) {
        // Normal navigation while in multi-select mode
        if (input.isButtonJustPressed(Button::UP)) {
          m_selectedGameIndex = std::max(0, m_selectedGameIndex - 1);
        } else if (input.isButtonJustPressed(Button::DOWN)) {
          m_selectedGameIndex = std::min(total - 1, m_selectedGameIndex + 1);
        } else if (input.isButtonJustPressed(Button::L1)) {
          m_selectedGameIndex = std::max(0, m_selectedGameIndex - pageSize);
        } else if (input.isButtonJustPressed(Button::R1)) {
          m_selectedGameIndex =
              std::min(total - 1, m_selectedGameIndex + pageSize);
        }
        // Update scroll offset
        if (m_selectedGameIndex < m_gameScrollOffset) {
          m_gameScrollOffset = m_selectedGameIndex;
        } else if (m_selectedGameIndex >= m_gameScrollOffset + pageSize) {
          m_gameScrollOffset = m_selectedGameIndex - pageSize + 1;
        }
      } else if (input.isButtonJustPressed(Button::Y)) {
        // Toggle selection on current game
        const auto &g = m_cachedGames[m_selectedGameIndex];
        auto it =
            std::find(m_selectedGameIds.begin(), m_selectedGameIds.end(), g.id);
        if (it != m_selectedGameIds.end()) {
          m_selectedGameIds.erase(it);
          showToast("Đã bỏ chọn: " + g.title, {245, 158, 11, 255}, 1500);
        } else {
          m_selectedGameIds.push_back(g.id);
          showToast("Đã chọn: " + g.title, {34, 197, 94, 255}, 1500);
        }
      } else if (input.isButtonJustPressed(Button::X) &&
                 !m_selectedGameIds.empty()) {
        // Batch delete - show confirmation
        setState(UIState::CONFIRM_BATCH_DELETE);
      } else if (input.isButtonJustPressed(Button::R2)) {
        // Add all selected to download queue
        if (!AuthManager::instance().isLinked()) {
          showToast(UiStrings::TOAST_CONNECT_DRIVE_FIRST, {245, 158, 11, 255});
        } else {
          int addedCount = 0;
          for (int64_t gameId : m_selectedGameIds) {
            // Find the game record
            for (const auto &g : m_cachedGames) {
              if (g.id == gameId && g.localState != GameState::LOCAL &&
                  !DownloadManager::instance().isInQueue(g.id)) {
                if (DownloadManager::instance().addToQueue(g, m_activeSystem)) {
                  addedCount++;
                }
              }
            }
          }
          if (addedCount > 0) {
            showToast("Đã thêm " + std::to_string(addedCount) +
                          " game vào hàng tải!",
                      {34, 197, 94, 255}, 3000);
            if (!DownloadManager::instance().isDownloading()) {
              DownloadManager::instance().processNextInQueue();
            }
          } else {
            showToast("Không có game nào được thêm (đã tải hoặc đang chờ)",
                      {245, 158, 11, 255}, 3000);
          }
          refreshGames();
        }
      } else if (input.isButtonJustPressed(Button::L1) &&
                 !m_selectedGameIds.empty()) {
        // Start upload of selected games to cloud
        if (!AuthManager::instance().isLinked()) {
          showToast(UiStrings::TOAST_CONNECT_DRIVE_FIRST, {245, 158, 11, 255});
        } else if (!AuthManager::instance().canUpload()) {
          showToast(UiStrings::TOAST_CONNECT_PERSONAL_DRIVE,
                    {245, 158, 11, 255}, 4000);
        } else {
          // Gather games to upload (local but not on cloud)
          std::vector<int64_t> uploadIds;
          for (int64_t gameId : m_selectedGameIds) {
            for (const auto &g : m_cachedGames) {
              if (g.id == gameId && g.localState == GameState::LOCAL &&
                  g.cloudFileId.empty()) {
                uploadIds.push_back(gameId);
                break;
              }
            }
          }
          if (!uploadIds.empty()) {
            showToast("Bắt đầu sao lưu " + std::to_string(uploadIds.size()) +
                          " game lên Drive...",
                      {168, 85, 247, 255}, 2000);
            UploadManager::instance().startUploadGames(uploadIds);
            m_multiSelectMode = false;
            m_selectedGameIds.clear();
            setState(UIState::REVERSE_SYNC);
          } else {
            showToast("Không có game nào cần tải lên (đã có trên Cloud)",
                      {245, 158, 11, 255}, 3000);
          }
        }
      } else if (input.isButtonJustPressed(Button::B)) {
        // Exit multi-select mode
        m_multiSelectMode = false;
        m_selectedGameIds.clear();
        showToast(UiStrings::MULTI_SELECT_DISABLED, {168, 85, 247, 255}, 2000);
      }
    } else if (total > 0) {
      if (input.isButtonJustPressed(Button::UP)) {
        if (m_selectedGameIndex > 0) {
          m_selectedGameIndex--;
          if (m_selectedGameIndex < m_gameScrollOffset) {
            m_gameScrollOffset = m_selectedGameIndex;
          }
        } else {
          m_selectedGameIndex = total - 1;
          m_gameScrollOffset = std::max(0, total - pageSize);
        }
      } else if (input.isButtonJustPressed(Button::DOWN)) {
        if (m_selectedGameIndex < total - 1) {
          m_selectedGameIndex++;
          if (m_selectedGameIndex >= m_gameScrollOffset + pageSize) {
            m_gameScrollOffset = m_selectedGameIndex - pageSize + 1;
          }
        } else {
          m_selectedGameIndex = 0;
          m_gameScrollOffset = 0;
        }
      } else if (input.isButtonJustPressed(Button::L1)) {
        m_selectedGameIndex = std::max(0, m_selectedGameIndex - pageSize);
        m_gameScrollOffset = std::max(0, m_gameScrollOffset - pageSize);
      } else if (input.isButtonJustPressed(Button::R1)) {
        m_selectedGameIndex =
            std::min(total - 1, m_selectedGameIndex + pageSize);
        m_gameScrollOffset = std::min(std::max(0, total - pageSize),
                                      m_gameScrollOffset + pageSize);
      } else if (input.isButtonJustPressed(Button::A)) {
        const auto &g = m_cachedGames[m_selectedGameIndex];
        if (g.localState == GameState::LOCAL) {
          showToast(UiStrings::TOAST_GAME_EXISTS_DELETE, {34, 197, 94, 255},
                    3000);
        } else if (DownloadManager::instance().isInQueue(g.id)) {
          showToast("\"" + g.title + "\" đã có trong danh sách tải.",
                    {245, 158, 11, 255});
        } else {
          if (AuthManager::instance().isLinked()) {
            bool added =
                DownloadManager::instance().addToQueue(g, m_activeSystem);
            if (added) {
              showToast(std::string(UiStrings::TOAST_ADDED_TO_QUEUE) + g.title,
                        {0, 180, 216, 255}, 2500);
              // Auto-start if nothing is currently downloading
              if (!DownloadManager::instance().isDownloading()) {
                DownloadManager::instance().processNextInQueue();
              }
              refreshGames();
            }
          } else {
            showToast(UiStrings::TOAST_CONNECT_DRIVE_FIRST,
                      {245, 158, 11, 255});
          }
        }
      } else if (input.isButtonJustPressed(Button::X)) {
        const auto &g = m_cachedGames[m_selectedGameIndex];
        if (DownloadManager::instance().isDownloading() &&
            DownloadManager::instance().getProgress().gameId == g.id) {
          DownloadManager::instance().cancelDownload();
          showToast(std::string(UiStrings::TOAST_DOWNLOAD_STOPPED) + g.title,
                    {245, 158, 11, 255});
          DownloadManager::instance().processNextInQueue();
          refreshGames();
        } else if (DownloadManager::instance().isInQueue(g.id)) {
          DownloadManager::instance().removeFromQueue(g.id);
          showToast(std::string(UiStrings::TOAST_REMOVED_FROM_QUEUE) + g.title,
                    {245, 158, 11, 255});
        } else if (g.localState == GameState::LOCAL) {
          setState(UIState::CONFIRM_DELETE);
        } else {
          showToast(UiStrings::TOAST_GAME_ONLY_ON_DRIVE, {245, 158, 11, 255});
        }
      } else if (input.isButtonJustPressed(Button::Y)) {
        // Quick Alphabet Jump across large game library
        if (!m_cachedGames.empty()) {
          char curL = 'A';
          if (!m_cachedGames[m_selectedGameIndex].title.empty()) {
            curL = std::toupper(m_cachedGames[m_selectedGameIndex].title[0]);
          }
          int nextIdx = -1;
          for (size_t i = m_selectedGameIndex + 1; i < m_cachedGames.size();
               ++i) {
            char l = ' ';
            if (!m_cachedGames[i].title.empty()) {
              l = std::toupper(m_cachedGames[i].title[0]);
            }
            if (l != curL && std::isalnum(l)) {
              nextIdx = static_cast<int>(i);
              break;
            }
          }
          if (nextIdx < 0) {
            nextIdx = 0; // wrap around to top
          }
          m_selectedGameIndex = nextIdx;
          m_gameScrollOffset = std::max(0, m_selectedGameIndex - 4);
          char newL = '?';
          if (!m_cachedGames[m_selectedGameIndex].title.empty()) {
            newL = std::toupper(m_cachedGames[m_selectedGameIndex].title[0]);
          }
          showToast(std::string("Chuyển đến vần chữ: [ ") + newL + " ]",
                    {234, 179, 8, 255}, 1500);
        }
      } else if (input.isButtonJustPressed(Button::SELECT)) {
        if (m_filterMode == GameFilterMode::ALL) {
          m_filterMode = GameFilterMode::LOCAL_ONLY;
          showToast(UiStrings::FILTER_LABEL_LOCAL, {34, 197, 94, 255});
        } else if (m_filterMode == GameFilterMode::LOCAL_ONLY) {
          m_filterMode = GameFilterMode::CLOUD_ONLY;
          showToast(UiStrings::FILTER_LABEL_CLOUD, {0, 180, 216, 255});
        } else {
          m_filterMode = GameFilterMode::ALL;
          showToast(UiStrings::FILTER_LABEL_ALL, {168, 85, 247, 255});
        }
        m_selectedGameIndex = 0;
        m_gameScrollOffset = 0;
        refreshGames();
      }

      if (input.isButtonJustPressed(Button::START)) {
        // Open on-device search (unified keyboard, charset media nhu YT/IPTV)
        VirtualKeyboard::reset(m_searchVk, true);
        m_searchVk.charset = 1;
        m_searchVk.maxLen = 30;
        m_searchResults.clear();
        m_searchSelectedIndex = 0;
        m_searchScrollOffset = 0;
        setState(UIState::SEARCH);
      }

      if (input.isButtonJustPressed(Button::B)) {
        setState(UIState::SYSTEM_SELECT);
      }
    }
    break;
  }

  case UIState::CONFIRM_DELETE: {
    if (input.isButtonJustPressed(Button::A)) {
      m_dialogs.confirm.confirm();
      setState(UIState::GAME_LIST);
    } else if (input.isButtonJustPressed(Button::B) ||
               input.isButtonJustPressed(Button::X)) {
      m_dialogs.confirm.cancel();
      setState(UIState::GAME_LIST);
    }
    break;
  }

  case UIState::CONFIRM_BATCH_DELETE: {
    if (input.isButtonJustPressed(Button::A)) {
      m_dialogs.confirm.confirm();
      setState(UIState::GAME_LIST);
    } else if (input.isButtonJustPressed(Button::B) ||
               input.isButtonJustPressed(Button::X)) {
      m_dialogs.confirm.cancel();
      setState(UIState::GAME_LIST);
    }
    break;
  }

  case UIState::SEARCH: {
    if (!m_searchVk.inResults) {
      // Unified keyboard (P0-2): giong YouTube — move/pressA stride2.
      if (input.isButtonJustPressed(Button::UP)) {
        VirtualKeyboard::move(m_searchVk, -1, 0, true);
      } else if (input.isButtonJustPressed(Button::DOWN)) {
        if (m_searchVk.row == 4) {
          // Go to results if any
          if (!m_searchResults.empty()) {
            m_searchVk.inResults = true;
            m_searchSelectedIndex = 0;
            m_searchScrollOffset = 0;
          }
        } else {
          VirtualKeyboard::move(m_searchVk, 1, 0, true);
        }
      } else if (input.isButtonJustPressed(Button::LEFT)) {
        VirtualKeyboard::move(m_searchVk, 0, -1, true);
      } else if (input.isButtonJustPressed(Button::RIGHT)) {
        VirtualKeyboard::move(m_searchVk, 0, 1, true);
      } else if (input.isButtonJustPressed(Button::A)) {
        VkAction act = VirtualKeyboard::pressA(
            m_searchVk,
            [this](const char *) {
              showToast(m_searchVk.telexMode ? "Chế độ: TELEX"
                                             : "Chế độ: TIẾNG ANH (US)",
                        {0, 180, 216, 255}, 1200);
            },
            true);
        if (act == VkAction::Commit) {
          if (!m_searchResults.empty()) {
            m_searchVk.inResults = true;
            m_searchSelectedIndex = 0;
            m_searchScrollOffset = 0;
          }
        } else {
          // Auto-search as user types
          if (m_searchVk.query.length() >= 2) {
            m_searchResults = DatabaseManager::instance().searchAllGames(
                m_searchVk.query, 50);
          } else {
            m_searchResults.clear();
          }
          m_searchSelectedIndex = 0;
          m_searchScrollOffset = 0;
        }
      } else if (input.isButtonJustPressed(Button::X)) {
        // Clear query
        m_searchVk.query.clear();
        m_searchResults.clear();
        m_searchSelectedIndex = 0;
        m_searchScrollOffset = 0;
      } else if (input.isButtonJustPressed(Button::Y)) {
        VirtualKeyboard::backspace(m_searchVk);
        if (m_searchVk.query.length() >= 2) {
          m_searchResults =
              DatabaseManager::instance().searchAllGames(m_searchVk.query, 50);
        } else {
          m_searchResults.clear();
        }
        m_searchSelectedIndex = 0;
        m_searchScrollOffset = 0;
      } else if (input.isButtonJustPressed(Button::L1)) {
        m_searchVk.shift = !m_searchVk.shift;
      } else if (input.isButtonJustPressed(Button::R1)) {
        m_searchVk.telexMode = !m_searchVk.telexMode;
        showToast(m_searchVk.telexMode ? "Chế độ: TELEX"
                                       : "Chế độ: TIẾNG ANH (US)",
                  {0, 180, 216, 255}, 1200);
      } else if (input.isButtonJustPressed(Button::START)) {
        if (!m_searchResults.empty()) {
          m_searchVk.inResults = true;
          m_searchSelectedIndex = 0;
          m_searchScrollOffset = 0;
        }
      }
    } else {
      // Browse results
      int numResults = static_cast<int>(m_searchResults.size());
      int pageSize = 6;
      if (input.isButtonJustPressed(Button::UP)) {
        if (m_searchSelectedIndex > 0) {
          m_searchSelectedIndex--;
          if (m_searchSelectedIndex < m_searchScrollOffset)
            m_searchScrollOffset = m_searchSelectedIndex;
        } else {
          m_searchVk.inResults = false; // go back to keyboard
        }
      } else if (input.isButtonJustPressed(Button::DOWN)) {
        if (m_searchSelectedIndex < numResults - 1) {
          m_searchSelectedIndex++;
          if (m_searchSelectedIndex >= m_searchScrollOffset + pageSize)
            m_searchScrollOffset = m_searchSelectedIndex - pageSize + 1;
        }
      } else if (input.isButtonJustPressed(Button::A)) {
        if (m_searchSelectedIndex >= 0 && m_searchSelectedIndex < numResults) {
          const auto &g = m_searchResults[m_searchSelectedIndex];
          if (g.localState == GameState::LOCAL) {
            showToast(UiStrings::TOAST_GAME_EXISTS_DELETE, {34, 197, 94, 255},
                      2500);
          } else if (!AuthManager::instance().isLinked()) {
            showToast(UiStrings::TOAST_CONNECT_DRIVE_FIRST,
                      {245, 158, 11, 255});
          } else {
            // Find system for this game
            SystemRecord sys;
            if (DatabaseManager::instance().getSystemById(g.systemId, sys)) {
              bool added = DownloadManager::instance().addToQueue(g, sys);
              if (added) {
                showToast(std::string(UiStrings::TOAST_ADDED_TO_QUEUE) +
                              g.title,
                          {0, 180, 216, 255}, 2500);
                if (!DownloadManager::instance().isDownloading()) {
                  DownloadManager::instance().processNextInQueue();
                }
                // Refresh results state
                m_searchResults = DatabaseManager::instance().searchAllGames(
                    m_searchVk.query, 50);
              }
            }
          }
        }
      } else if (input.isButtonJustPressed(Button::X)) {
        if (m_searchSelectedIndex >= 0 && m_searchSelectedIndex < numResults) {
          const auto &g = m_searchResults[m_searchSelectedIndex];
          if (g.localState == GameState::LOCAL) {
            // Jump to delete confirm
            // Find game in cached list or use search result id
            m_selectedGameIndex = -1;
            for (int i = 0; i < static_cast<int>(m_cachedGames.size()); ++i) {
              if (m_cachedGames[i].id == g.id) {
                m_selectedGameIndex = i;
                break;
              }
            }
            if (m_selectedGameIndex < 0) {
              // Delete directly via markGameDeletedLocally
              DatabaseManager::instance().markGameDeletedLocally(g.id);
              refreshSystems();
              m_searchResults = DatabaseManager::instance().searchAllGames(
                  m_searchVk.query, 50);
              if (m_searchSelectedIndex >=
                  static_cast<int>(m_searchResults.size()))
                m_searchSelectedIndex =
                    std::max(0, static_cast<int>(m_searchResults.size()) - 1);
              showToast("Đã xóa \"" + g.title + "\" khỏi thẻ nhớ.",
                        {239, 68, 68, 255}, 3000);
            } else {
              setState(UIState::CONFIRM_DELETE);
            }
          } else {
            showToast(UiStrings::TOAST_GAME_ONLY_ON_DRIVE, {245, 158, 11, 255});
          }
        }
      }
    }

    if (input.isButtonJustPressed(Button::B)) {
      if (m_searchVk.inResults) {
        m_searchVk.inResults = false; // ve ban phim nhu IPTV
      } else if (!m_searchVk.query.empty()) {
        // Unified nhu YouTube: con chu thi xoa dan, het chu thi thoat.
        VirtualKeyboard::backspace(m_searchVk);
        if (m_searchVk.query.length() >= 2) {
          m_searchResults =
              DatabaseManager::instance().searchAllGames(m_searchVk.query, 50);
        } else {
          m_searchResults.clear();
        }
        m_searchSelectedIndex = 0;
        m_searchScrollOffset = 0;
      } else if (m_activeSystem.id > 0) {
        setState(UIState::GAME_LIST);
      } else {
        setState(UIState::MENU);
      }
    }
    break;
  }

  case UIState::SETTINGS: {
    constexpr int visibleRows = 9;

    // Tab switching: L1/R1 (rule toàn app). D-pad dành cho nội dung/modal
    // (bàn phím ảo dùng Trái/Phải di chuyển, tab không được cướp).
    // Modal Wi-Fi mở thì nhường toàn bộ phím cho modal.
    // 0=CẤU HÌNH, 1=TÙY CHỌN, 2=CẬP NHẬT, 3=WI-FI, 4=GIỚI THIỆU, 5=HỆ THỐNG.
    if (!m_wifiModalOpen) {
    if (input.isButtonJustPressed(Button::L1)) {
      m_settingsTab = (m_settingsTab + 5) % 6;
      m_selectedSettingsRow = 0;
      m_settingsScrollOffset = 0;
    } else if (input.isButtonJustPressed(Button::R1)) {
      m_settingsTab = (m_settingsTab + 1) % 6;
      m_selectedSettingsRow = 0;
      m_settingsScrollOffset = 0;
    }
    }

    if (m_settingsTab == 0) {
      // ─── Tab 0: CẤU HÌNH (9 rows: 0..8) ─────────────────────────────
      const int totalRows = 9;
      if (input.isButtonJustPressed(Button::UP)) {
        if (m_selectedSettingsRow > 0) {
          m_selectedSettingsRow--;
          if (m_selectedSettingsRow < m_settingsScrollOffset) {
            m_settingsScrollOffset = m_selectedSettingsRow;
          }
        } else {
          m_selectedSettingsRow = totalRows - 1;
          m_settingsScrollOffset = std::max(0, totalRows - visibleRows);
        }
      } else if (input.isButtonJustPressed(Button::DOWN)) {
        if (m_selectedSettingsRow < totalRows - 1) {
          m_selectedSettingsRow++;
          if (m_selectedSettingsRow >= m_settingsScrollOffset + visibleRows) {
            m_settingsScrollOffset = m_selectedSettingsRow - visibleRows + 1;
          }
        } else {
          m_selectedSettingsRow = 0;
          m_settingsScrollOffset = 0;
        }
      }

      if (input.isButtonJustPressed(Button::A)) {
        if (m_selectedSettingsRow == 0) {
          if (!AuthManager::instance().isLinked()) {
            setState(UIState::DISCLAIMER);
          }
        } else if (m_selectedSettingsRow == 2) {
          OSType current = AppConfig::instance().getOSType();
          OSType next;
          switch (current) {
          case OSType::AUTO:
            next = OSType::STOCK_PS;
            break;
          case OSType::STOCK_PS:
            next = OSType::NEXTUI;
            break;
          case OSType::NEXTUI:
            next = OSType::SPRUCE_OS;
            break;
          case OSType::SPRUCE_OS:
            next = OSType::AUTO;
            break;
          default:
            next = OSType::AUTO;
            break;
          }
          AppConfig::instance().setOSType(next);
          showToast("OS: " + AppConfig::instance().getOSName(),
                    {168, 85, 247, 255}, 2000);
        } else if (m_selectedSettingsRow == 7) {
          int newPort = WebServer::instance().cycleNextPort();
          DatabaseManager::instance().setSetting("web_portal_port",
                                                 std::to_string(newPort));
          std::string newUrl = WebServer::instance().getPortalUrl();
          showToast("✔ Đã đổi Web Portal: " + newUrl, {0, 180, 255, 255}, 3500);
        } else if (m_selectedSettingsRow == 8) {
          CastManager::instance().toggle();
          bool running = CastManager::instance().isRunning();
          std::string ip = PlatformInfo::instance().getIpAddress("wlan0");
          if (ip.empty() || ip == "Disconnected")
            ip = "192.168.1.164";
          if (running) {
            showToast("✔ Đã bật GameCast: http://" + ip + ":8090",
                      {34, 197, 94, 255}, 3000);
          } else {
            showToast("GameCast đã tắt", {239, 68, 68, 255}, 2000);
          }
        }
      } else if (input.isButtonJustPressed(Button::X)) {
        if (m_selectedSettingsRow == 0 && AuthManager::instance().isLinked()) {
          AuthManager::instance().logout();
          refreshSystems();
          refreshGames();
          showToast(UiStrings::TOAST_LOGOUT_SUCCESS, {245, 158, 11, 255});
        }
      }
    } else if (m_settingsTab == 1) {
      // ─── Tab 1: CÀI ĐẶT (6 rows: 0..5) ──────────────────────────────
      const int totalRows = 6;
      if (input.isButtonJustPressed(Button::UP)) {
        if (m_selectedSettingsRow > 0) {
          m_selectedSettingsRow--;
          if (m_selectedSettingsRow < m_settingsScrollOffset) {
            m_settingsScrollOffset = m_selectedSettingsRow;
          }
        } else {
          m_selectedSettingsRow = totalRows - 1;
          m_settingsScrollOffset = std::max(0, totalRows - visibleRows);
        }
      } else if (input.isButtonJustPressed(Button::DOWN)) {
        if (m_selectedSettingsRow < totalRows - 1) {
          m_selectedSettingsRow++;
          if (m_selectedSettingsRow >= m_settingsScrollOffset + visibleRows) {
            m_settingsScrollOffset = m_selectedSettingsRow - visibleRows + 1;
          }
        } else {
          m_selectedSettingsRow = 0;
          m_settingsScrollOffset = 0;
        }
      }

      if (input.isButtonJustPressed(Button::A)) {
        if (m_selectedSettingsRow == 0) {
          // Clean cache
          showToast("Đang dọn dẹp bộ nhớ đệm...", {250, 204, 21, 255}, 1500);
          uint64_t freed = cleanCache();
          showToast("✔ Đã giải phóng " + LsUtil::humanSize(freed) +
                        " bộ nhớ đệm!",
                    {34, 197, 94, 255}, 4000);
        } else if (m_selectedSettingsRow == 1) {
          // Export backup
          showToast(UiStrings::BACKUP_EXPORTING, {168, 85, 247, 255}, 2000);
          auto result = BackupManager::instance().exportToSdCard();
          if (result.success) {
            showToast(UiStrings::BACKUP_SUCCESS, {34, 197, 94, 255}, 4000);
          } else {
            showToast(UiStrings::BACKUP_FAILED, {239, 68, 68, 255}, 4000);
          }
        } else if (m_selectedSettingsRow == 2) {
          // Import backup
          showToast(UiStrings::BACKUP_IMPORTING, {0, 180, 216, 255}, 2000);
          auto lastBackup = BackupManager::instance().getMostRecentBackup();
          if (lastBackup.empty()) {
            showToast(UiStrings::BACKUP_NO_FILE, {245, 158, 11, 255}, 4000);
          } else {
            auto result = BackupManager::instance().importFromFile(lastBackup);
            if (result.success) {
              showToast(UiStrings::BACKUP_RESTORE_SUCCESS, {34, 197, 94, 255},
                        4000);
            } else {
              showToast(UiStrings::BACKUP_RESTORE_FAILED, {239, 68, 68, 255},
                        4000);
            }
          }
        } else if (m_selectedSettingsRow == 3) {
          // Toggle issue reporting
          bool currentEnabled = IssueLogger::instance().isEnabled();
          if (currentEnabled) {
            IssueLogger::instance().setEnabled(false);
            showToast("Đã tắt báo lỗi tự động", {239, 68, 68, 255}, 2000);
          } else {
            IssueLogger::instance().setEnabled(true);
            showToast("Đã bật báo lỗi tự động", {34, 197, 94, 255}, 2000);
          }
        } else if (m_selectedSettingsRow == 4) {
          // Gửi lỗi thủ công (chạy nền): toast "Đang gửi..." giữ suốt tiến
          // trình (60s, phủ timeout curl 30s+10s), xong mới báo kết quả.
          // Trước đây gọi blocking + toast 2s nên user không thấy tiến trình.
          if (m_manualReportSending.load()) {
            showToast("Đang gửi, vui lòng chờ...", {245, 158, 11, 255}, 2000);
          } else {
            m_manualReportSending.store(true);
            showToast("Đang gửi báo cáo lỗi...", {0, 180, 216, 255}, 60000);
            std::thread([this]() {
              bool sent = IssueLogger::instance().sendManualReport();
              m_manualReportSending.store(false);
              if (sent) {
                showToast("Đã gửi báo cáo lỗi!", {34, 197, 94, 255}, 4000);
              } else {
                showToast("Gửi thất bại. Kiểm tra mạng.", {239, 68, 68, 255},
                          4000);
              }
            }).detach();
          }
        } else if (m_selectedSettingsRow == 5) {
          // Wi-Fi diagnostics
          runWifiDiagnostics();
          showToast("Đang kiểm tra kết nối mạng Wi-Fi...", {0, 180, 255, 255},
                    2000);
        }
      }
    } else if (m_settingsTab == 2) {
      // ─── Tab 2: CẬP NHẬT: route A/B theo UpdateState ─────────────────
      auto prog = UpdateManager::instance().getProgress();
      if (prog.state == UpdateState::UPDATE_AVAILABLE) {
        if (input.isButtonJustPressed(Button::A)) {
          UpdateManager::instance().startUpdate(
              UpdateManager::instance().getLatestInfo());
        } else if (input.isButtonJustPressed(Button::Y)) {
          // P6: mở sub-page full changelog
          m_otaChangelogScrollLine = 0;
          setState(UIState::OTA_CHANGELOG);
        }
      } else if (prog.state == UpdateState::UP_TO_DATE ||
                 prog.state == UpdateState::FAILED) {
        if (input.isButtonJustPressed(Button::A)) {
          UpdateManager::instance().checkForUpdatesAsync();
        } else if (input.isButtonJustPressed(Button::Y) &&
                   prog.state == UpdateState::UP_TO_DATE &&
                   !UpdateManager::instance()
                        .getLatestInfo()
                        .changelog.empty()) {
          // Mở sub-page full changelog ngay cả khi đã là bản mới nhất
          m_otaChangelogScrollLine = 0;
          setState(UIState::OTA_CHANGELOG);
        }
      } else if (prog.state == UpdateState::DOWNLOADING ||
                 prog.state == UpdateState::DOWNLOADING_DEPS ||
                 prog.state == UpdateState::VERIFYING) {
        if (input.isButtonJustPressed(Button::B)) {
          UpdateManager::instance().cancelUpdate();
          showToast(UiStrings::TOAST_OTA_CANCELLED, {245, 158, 11, 255});
        }
      } else if (prog.state == UpdateState::COMPLETED) {
        if (input.isButtonJustPressed(Button::A)) {
          Application::instance().requestRestart();
        }
      }
    } else if (m_settingsTab == 3) {
      // ─── Tab 3: WI-FI (quét + nối mạng + portal) ──────────────────────
      handleWifiTabInput();
    } else if (m_settingsTab == 4 || m_settingsTab == 5) {
      // ─── Tab 4/5: GIỚI THIỆU / HỆ THỐNG (từ INFO) ─────────────────────
      int infoTab = (m_settingsTab == 4) ? 0 : 1;
      if (infoTab == 0 && m_lastInfoTab != 0) {
        m_aboutAutoScrollY = 0.0f;
        m_aboutLastTickMs = 0;
      }
      m_lastInfoTab = infoTab;
      if (infoTab == 1) {
        if (input.isButtonJustPressed(Button::UP)) {
          m_diagnosticsScrollOffset =
              std::max(0, m_diagnosticsScrollOffset - 1);
        } else if (input.isButtonJustPressed(Button::DOWN)) {
          m_diagnosticsScrollOffset++;
        }
      }
    }

    // ─── B (chung) về MENU + reset toàn bộ state về tab CẤU HÌNH ─────────
    // (bỏ qua khi modal nhập SSID/pass đang mở để B làm Hủy modal).
    if (input.isButtonJustPressed(Button::B) && !m_wifiModalOpen) {
      setState(UIState::MENU);
      m_selectedSettingsRow = 0;
      m_settingsScrollOffset = 0;
      m_settingsTab = 0;
    }
    break;
  }

  // P6: OTA Changelog sub-page. Back (B/Y) → quay về Settings (Cập nhật tab).
  // UP/DOWN nếu changelog quá dài (auto-detect). L1/R1 no-op.
  case UIState::OTA_CHANGELOG: {
    if (input.isButtonJustPressed(Button::B) ||
        input.isButtonJustPressed(Button::Y)) {
      setState(UIState::SETTINGS);
      m_settingsTab = 2; // về thẳng Cập nhật
    } else if (input.isButtonJustPressed(Button::UP)) {
      if (m_otaChangelogScrollLine > 0)
        m_otaChangelogScrollLine--;
    } else if (input.isButtonJustPressed(Button::DOWN)) {
      m_otaChangelogScrollLine++;
      // Cap tối đa = maxLines. Nếu vượt sẽ bị clamp trong render.
    }
    break;
  }

  case UIState::DISCLAIMER: {
    if (input.isButtonJustPressed(Button::A)) {
      setState(UIState::CLOUD_LOGIN);
    } else if (input.isButtonJustPressed(Button::B)) {
      setState(UIState::SETTINGS);
    }
    break;
  }

  case UIState::CLOUD_LOGIN: {
    if (AuthManager::instance().isLinked()) {
      if (input.isButtonJustPressed(Button::A) ||
          input.isButtonJustPressed(Button::B)) {
        refreshSystems();
        setState(UIState::SYSTEM_SELECT);
      }
    } else {
      if (input.isButtonJustPressed(Button::B)) {
        AuthManager::instance().cancelDeviceFlow();
        setState(UIState::SETTINGS);
      } else if (input.isButtonJustPressed(Button::Y)) {
        m_cloudPortalView = !m_cloudPortalView; // mã thiết bị <-> web portal
      } else if (input.isButtonJustPressed(Button::A)) {
        // Lỗi lấy mã -> thử lại nền (không đơ UI).
        if (AuthManager::instance().getState() == AuthState::ERROR_OCCURRED) {
          AuthManager::instance().cancelDeviceFlow();
          std::thread([] { AuthManager::instance().startDeviceFlow(); })
              .detach();
        }
      }
    }
    break;
  }

  case UIState::DIAGNOSTICS: {
    // Reset About auto-scroll state on first frame after entering About tab
    // (either from MENU or via L1/R1 tab switch). Keeps cell start from top.
    if (m_infoTab == 0 && m_lastInfoTab != 0) {
      m_aboutAutoScrollY = 0.0f;
      m_aboutLastTickMs = 0;
    }
    m_lastInfoTab = m_infoTab;

    if (input.isButtonJustPressed(Button::L1) ||
        input.isButtonJustPressed(Button::LEFT)) {
      m_infoTab = (m_infoTab == 0) ? 1 : 0;
    } else if (input.isButtonJustPressed(Button::R1) ||
               input.isButtonJustPressed(Button::RIGHT)) {
      m_infoTab = (m_infoTab == 0) ? 1 : 0;
    } else if (input.isButtonJustPressed(Button::UP)) {
      // About tab scrolls automatically — UP/DOWN intentionally no-op.
      // System tab keeps manual scrolling.
      if (m_infoTab != 0)
        m_diagnosticsScrollOffset = std::max(0, m_diagnosticsScrollOffset - 1);
    } else if (input.isButtonJustPressed(Button::DOWN)) {
      if (m_infoTab != 0)
        m_diagnosticsScrollOffset++;
    } else if (input.isButtonJustPressed(Button::B)) {
      setState(UIState::MENU);
      m_diagnosticsScrollOffset = 0;
      m_aboutAutoScrollY = 0.0f;
      m_aboutLastTickMs = 0;
      m_infoTab = 0;
      m_lastInfoTab = -1;
    }
    break;
  }

  case UIState::REVERSE_SYNC: {
    auto prog = UploadManager::instance().getProgress();
    if (prog.state == UploadState::IDLE ||
        prog.state == UploadState::PREPARING) {
      if (input.isButtonJustPressed(Button::B)) {
        UploadManager::instance().cancel();
        setState(UIState::MENU);
      }
    } else if (prog.state == UploadState::UPLOADING) {
      if (input.isButtonJustPressed(Button::B)) {
        UploadManager::instance().cancel();
        showToast(UiStrings::REVERSE_SYNC_CANCELLED, {245, 158, 11, 255});
        setState(UIState::MENU);
      }
    } else if (prog.state == UploadState::COMPLETED ||
               prog.state == UploadState::FAILED ||
               prog.state == UploadState::CANCELLED) {
      if (input.isButtonJustPressed(Button::A) ||
          input.isButtonJustPressed(Button::B)) {
        setState(UIState::MENU);
      }
    }
    break;
  }

  case UIState::IPTV_PLAYLIST_SELECT: {
    const auto &playlists = IPTVManager::instance().getPlaylists();
    int plCount = static_cast<int>(playlists.size());
    int visibleItems = 10;

    if (input.isButtonJustPressed(Button::UP)) {
      if (plCount > 0) {
        m_selectedPlaylistIndex = std::max(0, m_selectedPlaylistIndex - 1);
        if (m_selectedPlaylistIndex < m_playlistScrollOffset)
          m_playlistScrollOffset = m_selectedPlaylistIndex;
      }
    } else if (input.isButtonJustPressed(Button::DOWN)) {
      if (plCount > 0) {
        m_selectedPlaylistIndex =
            std::min(plCount - 1, m_selectedPlaylistIndex + 1);
        if (m_selectedPlaylistIndex >= m_playlistScrollOffset + visibleItems)
          m_playlistScrollOffset = m_selectedPlaylistIndex - visibleItems + 1;
      }
    } else if (input.isButtonJustPressed(Button::A)) {
      if (plCount > 0 && m_selectedPlaylistIndex < plCount) {
        const Playlist *pl = IPTVManager::instance().getPlaylist(
            static_cast<size_t>(m_selectedPlaylistIndex));
        if (pl) {
          showToast("Đang mở: " + pl->name + " (" +
                        std::to_string(pl->channelCount()) + " kênh)...",
                    {0, 180, 216, 255}, 1500);
        }
        m_activePlaylistIndex = m_selectedPlaylistIndex;
        m_iptvSelectedGroup.clear();
        m_iptvGroupBarOffset = 0;
        m_selectedIPTVChannelIndex = 0;
        m_iptvScrollOffset = 0;
        setState(UIState::IPTV_LIST);
      }
    } else if (input.isButtonJustPressed(Button::Y)) {
      // Manual refresh playlist co URL nguon
      if (plCount > 0 && m_selectedPlaylistIndex < plCount) {
        const Playlist *pl = IPTVManager::instance().getPlaylist(
            static_cast<size_t>(m_selectedPlaylistIndex));
        if (pl) {
          const auto &sources = IPTVManager::instance().getSources();
          bool hasUrl = false;
          for (const auto &s : sources)
            if (s.filename == pl->sourceFile && s.type == "url") {
              hasUrl = true;
              break;
            }

          if (hasUrl) {
            showToast("Đang cập nhật playlist: " + pl->name + "...",
                      {250, 204, 21, 255}, 3000);
            render();
            std::string err;
            if (IPTVManager::instance().refreshPlaylistFromUrl(pl->sourceFile,
                                                               err)) {
              std::string ts =
                  IPTVManager::instance().getLastRefreshedStr(pl->sourceFile);
              showToast("Cập nhật thành công! (" + ts + ")", {34, 197, 94, 255},
                        3000);
            } else {
              showToast("Cập nhật thất bại: " + err, {239, 68, 68, 255}, 4000);
            }
          } else {
            showToast("Playlist này không có URL nguồn (chỉ có thể refresh "
                      "playlist được thêm qua URL)",
                      {148, 163, 184, 255}, 3000);
          }
        }
      }
    } else if (input.isButtonJustPressed(Button::X)) {
      // Thay doi interval refresh: 6h -> 12h -> 24h -> 72h -> 0(tat) -> 6h
      if (plCount > 0 && m_selectedPlaylistIndex < plCount) {
        const Playlist *pl = IPTVManager::instance().getPlaylist(
            static_cast<size_t>(m_selectedPlaylistIndex));
        if (pl) {
          const auto &sources = IPTVManager::instance().getSources();
          for (const auto &s : sources) {
            if (s.filename == pl->sourceFile && s.type == "url") {
              int cur = s.refreshIntervalHours;
              int next = (cur == 0)    ? 6
                         : (cur == 6)  ? 12
                         : (cur == 12) ? 24
                         : (cur == 24) ? 72
                                       : 0;
              IPTVManager::instance().setRefreshInterval(pl->sourceFile, next);
              std::string msg = (next == 0) ? "Đã tắt tự động refresh"
                                            : "Tự động refresh mỗi " +
                                                  std::to_string(next) + " giờ";
              showToast(msg, {0, 180, 216, 255}, 2000);
              break;
            }
          }
        }
      }
    } else if (input.isButtonJustPressed(Button::B)) {
      setState(UIState::MENU);
    }
    break;
  }

  case UIState::IPTV_LIST: {
    // Nếu đang phát: xử lý playback controls thay vì list navigation
    if (IPTVManager::instance().isIPTVPlaying()) {
      if (input.isButtonJustPressed(Button::A)) {
        // Play/Pause toggle
        MpvPlayer::instance().cyclePause();
      } else if (input.isButtonJustPressed(Button::B)) {
        // Stop and return to list
        IPTVManager::instance().stop();
        m_iptvOsdVisible = false;
        m_selectedIPTVChannelIndex = 0;
        m_iptvScrollOffset = 0;
      } else if (input.isButtonJustPressed(Button::SELECT)) {
        // Toggle OSD channel selector
        m_iptvOsdVisible = !m_iptvOsdVisible;
        if (m_iptvOsdVisible) {
          // Hiện OSD với danh sách kênh
          IPTVManager::instance().showIPTVChannelOSD(
              IPTVManager::instance().getChannels(), m_selectedIPTVChannelIndex,
              m_iptvSelectedGroup, 0);
        } else {
          // Ẩn OSD
          IPTVManager::instance().hideIPTVChannelOSD();
        }
      } else if (input.isButtonJustPressed(Button::UP)) {
        // Volume up
        IPTVManager::instance().sendMpvIpcCommand(
            "{\"command\":[\"add\",\"volume\",5]}\n");
      } else if (input.isButtonJustPressed(Button::DOWN)) {
        // Volume down
        IPTVManager::instance().sendMpvIpcCommand(
            "{\"command\":[\"add\",\"volume\",-5]}\n");
      }
      break; // Đang phát, không xử lý list navigation
    }

    // ------- Build filtered channel list -------
    std::vector<IPTVChannel> allChannels;
    if (m_iptvShowFavoritesOnly) {
      allChannels = IPTVManager::instance().getFavoriteChannels();
    } else if (m_activePlaylistIndex >= 0) {
      const Playlist *pl = IPTVManager::instance().getPlaylist(
          static_cast<size_t>(m_activePlaylistIndex));
      if (pl) {
        for (const auto &item : pl->channels)
          allChannels.push_back(
              IPTVChannel::fromItem(item, pl->name, pl->sourceFile));
      }
    } else {
      allChannels = IPTVManager::instance().getChannels();
    }

    // Ẩn kênh đã xác định mất kết nối (ping == -1). Kênh chưa đo (-2)
    // vẫn hiện bình thường.
    {
      std::vector<IPTVChannel> alive;
      alive.reserve(allChannels.size());
      for (const auto &ch : allChannels)
        if (IPTVManager::instance().getCachedPing(ch.url) != -1)
          alive.push_back(ch);
      allChannels.swap(alive);
    }

    // Build group list
    std::vector<std::string> groups;
    groups.push_back("");
    {
      std::unordered_set<std::string> seen;
      for (const auto &ch : allChannels) {
        if (!ch.group.empty() && seen.find(ch.group) == seen.end()) {
          seen.insert(ch.group);
          groups.push_back(ch.group);
        }
      }
      std::sort(groups.begin() + 1, groups.end());
    }

    // Apply group filter
    std::vector<IPTVChannel> channels;
    if (m_iptvSelectedGroup.empty()) {
      channels = allChannels;
    } else {
      for (const auto &ch : allChannels)
        if (ch.group == m_iptvSelectedGroup)
          channels.push_back(ch);
    }

    int channelCount = static_cast<int>(channels.size());
    int visibleItems = 8;

    // List có thể co lại khi ping resolve xong (kênh chết bị ẩn) → clamp
    if (channelCount == 0) {
      m_selectedIPTVChannelIndex = 0;
      m_iptvScrollOffset = 0;
    } else if (m_selectedIPTVChannelIndex >= channelCount) {
      m_selectedIPTVChannelIndex = channelCount - 1;
      if (m_selectedIPTVChannelIndex < m_iptvScrollOffset)
        m_iptvScrollOffset = m_selectedIPTVChannelIndex;
    }

    // ==============================================================
    // SDL browser binh thuong (mpv blocking loop xu ly playback)
    // ==============================================================
    if (input.isButtonJustPressed(Button::UP)) {
      if (channelCount > 0) {
        if (m_selectedIPTVChannelIndex > 0) {
          m_selectedIPTVChannelIndex -= 1;
        } else {
          // Loop: tu dau -> cuoi danh sach
          m_selectedIPTVChannelIndex = channelCount - 1;
          m_iptvScrollOffset = std::max(0, channelCount - visibleItems);
        }
        if (m_selectedIPTVChannelIndex < m_iptvScrollOffset)
          m_iptvScrollOffset = m_selectedIPTVChannelIndex;
      }
    } else if (input.isButtonJustPressed(Button::DOWN)) {
      if (channelCount > 0) {
        if (m_selectedIPTVChannelIndex + 1 < channelCount) {
          m_selectedIPTVChannelIndex += 1;
        } else {
          // Loop: tu cuoi -> dau danh sach
          m_selectedIPTVChannelIndex = 0;
          m_iptvScrollOffset = 0;
        }
        if (m_selectedIPTVChannelIndex >= m_iptvScrollOffset + visibleItems)
          m_iptvScrollOffset = m_selectedIPTVChannelIndex - visibleItems + 1;
      }
    } else if (input.isButtonJustPressed(Button::LEFT)) {
      auto it = std::find(groups.begin(), groups.end(), m_iptvSelectedGroup);
      if (it != groups.end() && it != groups.begin()) {
        --it;
        m_iptvSelectedGroup = *it;
        m_selectedIPTVChannelIndex = 0;
        m_iptvScrollOffset = 0;
        centerIptvGroupBar(groups, m_iptvSelectedGroup);
      }
    } else if (input.isButtonJustPressed(Button::RIGHT)) {
      auto it = std::find(groups.begin(), groups.end(), m_iptvSelectedGroup);
      if (it != groups.end()) {
        ++it;
        if (it != groups.end()) {
          m_iptvSelectedGroup = *it;
          m_selectedIPTVChannelIndex = 0;
          m_iptvScrollOffset = 0;
          centerIptvGroupBar(groups, m_iptvSelectedGroup);
        }
      }
    } else if (input.isButtonJustPressed(Button::L1)) {
      if (channelCount > 0) {
        int step = std::max(1, visibleItems);
        int lastPageStart = std::max(0, channelCount - step);
        if (m_selectedIPTVChannelIndex > 0) {
          m_selectedIPTVChannelIndex =
              std::max(0, m_selectedIPTVChannelIndex - step);
        } else {
          // Loop: tu trang dau -> trang cuoi
          m_selectedIPTVChannelIndex = lastPageStart;
        }
        m_iptvScrollOffset =
            std::max(0, m_selectedIPTVChannelIndex - (step - 1));
        if (m_iptvScrollOffset > lastPageStart)
          m_iptvScrollOffset = lastPageStart;
        if (m_selectedIPTVChannelIndex < m_iptvScrollOffset)
          m_iptvScrollOffset = m_selectedIPTVChannelIndex;
      }
    } else if (input.isButtonJustPressed(Button::R1)) {
      if (channelCount > 0) {
        int step = std::max(1, visibleItems);
        int lastPageStart = std::max(0, channelCount - step);
        int target = m_selectedIPTVChannelIndex + step;
        if (target < channelCount) {
          m_selectedIPTVChannelIndex = target;
        } else {
          // Loop: tu trang cuoi -> trang dau
          m_selectedIPTVChannelIndex = 0;
          m_iptvScrollOffset = 0;
        }
        if (m_selectedIPTVChannelIndex >= m_iptvScrollOffset + visibleItems)
          m_iptvScrollOffset = std::min(
              lastPageStart, m_selectedIPTVChannelIndex - visibleItems + 1);
      }
    } else if (input.isButtonJustPressed(Button::A)) {
      if (channelCount > 0 && m_selectedIPTVChannelIndex >= 0 &&
          m_selectedIPTVChannelIndex < channelCount) {
        showToast("Đang kết nối: " + channels[m_selectedIPTVChannelIndex].name +
                      "...",
                  {0, 180, 216, 255}, 3000);
        render();
        if (!IPTVManager::instance().playChannel(
                channels[m_selectedIPTVChannelIndex],
                m_selectedIPTVChannelIndex, channels)) {
          showToast("Không thể phát video (Lỗi kết nối hoặc player)",
                    {239, 68, 68, 255}, 4000);
        }
      } else if (channelCount == 0) {
        // List rỗng (có thể do kênh chết bị ẩn hết) → xóa cache ping để
        // hiện lại và đo lại từ đầu.
        IPTVManager::instance().clearPingCache();
        showToast("Đang đo lại kết nối các kênh...", {0, 180, 216, 255}, 2000);
      }
    } else if (input.isButtonJustPressed(Button::B)) {
      IPTVManager::instance().stop();
      if (!m_iptvSelectedGroup.empty()) {
        m_iptvSelectedGroup.clear();
        m_iptvGroupBarOffset = 0;
        m_selectedIPTVChannelIndex = 0;
        m_iptvScrollOffset = 0;
      } else if (m_iptvShowFavoritesOnly) {
        m_iptvShowFavoritesOnly = false;
        m_selectedIPTVChannelIndex = 0;
        m_iptvScrollOffset = 0;
        showToast("Đang hiển thị tất cả kênh", {0, 180, 216, 255}, 1500);
      } else if (IPTVManager::instance().playlistCount() > 1) {
        m_selectedPlaylistIndex = std::max(0, m_activePlaylistIndex);
        setState(UIState::IPTV_PLAYLIST_SELECT);
      } else {
        setState(UIState::MENU);
      }
    } else if (input.isButtonJustPressed(Button::X)) {
      if (channelCount > 0 && m_selectedIPTVChannelIndex >= 0 &&
          m_selectedIPTVChannelIndex < channelCount) {
        std::string chanName = channels[m_selectedIPTVChannelIndex].name;
        bool wasFav = IPTVManager::instance().isFavorite(chanName);
        IPTVManager::instance().toggleFavorite(chanName);
        showToast(wasFav ? "☆ Đã xóa khỏi yêu thích: " + chanName
                         : "★ Đã thêm vào yêu thích: " + chanName,
                  wasFav ? SDL_Color{148, 163, 184, 255}
                         : SDL_Color{250, 204, 21, 255},
                  2000);
      }
    } else if (input.isButtonJustPressed(Button::Y)) {
      m_iptvShowFavoritesOnly = !m_iptvShowFavoritesOnly;
      m_iptvSelectedGroup.clear();
      m_iptvGroupBarOffset = 0;
      m_selectedIPTVChannelIndex = 0;
      m_iptvScrollOffset = 0;
      showToast(m_iptvShowFavoritesOnly ? "★ Đang lọc: Kênh Yêu Thích"
                                        : "Đang lọc: Tất cả kênh",
                {0, 180, 216, 255}, 2000);
    } else if (input.isButtonJustPressed(Button::SELECT) ||
               input.isButtonJustPressed(Button::START)) {
      VirtualKeyboard::reset(m_iptvVk, true);
      m_iptvVk.charset = 1;
      m_iptvVk.maxLen = 30;
      m_iptvSearchResults.clear();
      m_iptvSearchSelectedIndex = 0;
      m_iptvSearchScrollOffset = 0;
      setState(UIState::IPTV_SEARCH);
    }
    break;
  }

  case UIState::IPTV_SEARCH: {
    int resultCount = static_cast<int>(m_iptvSearchResults.size());
    const int pageSize = 5;

    if (!m_iptvVk.inResults) {
      if (input.isButtonJustPressed(Button::UP)) {
        if (m_iptvVk.row == 0 && resultCount > 0) {
          m_iptvVk.inResults = true;
          m_iptvSearchSelectedIndex =
              std::min(m_iptvSearchSelectedIndex, resultCount - 1);
        } else {
          VirtualKeyboard::move(m_iptvVk, -1, 0, true);
        }
      } else if (input.isButtonJustPressed(Button::DOWN)) {
        VirtualKeyboard::move(m_iptvVk, 1, 0, true);
      } else if (input.isButtonJustPressed(Button::LEFT)) {
        VirtualKeyboard::move(m_iptvVk, 0, -1, true);
      } else if (input.isButtonJustPressed(Button::RIGHT)) {
        VirtualKeyboard::move(m_iptvVk, 0, 1, true);
      } else if (input.isButtonJustPressed(Button::A)) {
        VkAction act = VirtualKeyboard::pressA(
            m_iptvVk,
            [this](const char *) {
              showToast(m_iptvVk.telexMode ? "Chế độ: TELEX"
                                           : "Chế độ: TIẾNG ANH (US)",
                        {0, 180, 255, 255}, 1200);
            },
            true);
        if (act == VkAction::Commit) {
          if (resultCount > 0) {
            const auto &selChan =
                m_iptvSearchResults[m_iptvSearchSelectedIndex];
            showToast("Đang kết nối: " + selChan.name + "...",
                      {0, 180, 255, 255}, 3000);
            render();
            if (!IPTVManager::instance().playChannel(
                    selChan, m_iptvSearchSelectedIndex, m_iptvSearchResults)) {
              showToast("Không thể phát video (Lỗi kết nối hoặc player)",
                        {239, 68, 68, 255}, 4000);
            }
          }
        } else {
          if (!m_iptvVk.query.empty()) {
            m_iptvSearchResults =
                IPTVManager::instance().search(m_iptvVk.query);
          } else {
            m_iptvSearchResults.clear();
          }
          m_iptvSearchSelectedIndex = 0;
          m_iptvSearchScrollOffset = 0;
        }
      } else if (input.isButtonJustPressed(Button::X)) {
        VirtualKeyboard::typeSpace(m_iptvVk);
        if (!m_iptvVk.query.empty()) {
          m_iptvSearchResults = IPTVManager::instance().search(m_iptvVk.query);
        } else {
          m_iptvSearchResults.clear();
        }
        m_iptvSearchSelectedIndex = 0;
        m_iptvSearchScrollOffset = 0;
      } else if (input.isButtonJustPressed(Button::Y)) {
        VirtualKeyboard::backspace(m_iptvVk);
        if (!m_iptvVk.query.empty()) {
          m_iptvSearchResults = IPTVManager::instance().search(m_iptvVk.query);
        } else {
          m_iptvSearchResults.clear();
        }
        m_iptvSearchSelectedIndex = 0;
        m_iptvSearchScrollOffset = 0;
      } else if (input.isButtonJustPressed(Button::L1)) {
        m_iptvVk.shift = !m_iptvVk.shift;
      } else if (input.isButtonJustPressed(Button::R1)) {
        m_iptvVk.telexMode = !m_iptvVk.telexMode;
        showToast(m_iptvVk.telexMode ? "Chế độ: TELEX"
                                     : "Chế độ: TIẾNG ANH (US)",
                  {0, 180, 255, 255}, 1200);
      } else if (input.isButtonJustPressed(Button::START)) {
        if (resultCount > 0) {
          const auto &selChan = m_iptvSearchResults[m_iptvSearchSelectedIndex];
          showToast("Đang kết nối: " + selChan.name + "...", {0, 180, 255, 255},
                    3000);
          render();
          if (!IPTVManager::instance().playChannel(
                  selChan, m_iptvSearchSelectedIndex, m_iptvSearchResults)) {
            showToast("Không thể phát video (Lỗi kết nối hoặc player)",
                      {239, 68, 68, 255}, 4000);
          }
        }
      } else if (input.isButtonJustPressed(Button::B)) {
        setState(UIState::IPTV_LIST);
      }
    } else {
      // In Results
      if (input.isButtonJustPressed(Button::UP)) {
        if (m_iptvSearchSelectedIndex > 0) {
          m_iptvSearchSelectedIndex--;
          if (m_iptvSearchSelectedIndex < m_iptvSearchScrollOffset) {
            m_iptvSearchScrollOffset = m_iptvSearchSelectedIndex;
          }
        }
      } else if (input.isButtonJustPressed(Button::DOWN)) {
        if (m_iptvSearchSelectedIndex < resultCount - 1) {
          m_iptvSearchSelectedIndex++;
          if (m_iptvSearchSelectedIndex >=
              m_iptvSearchScrollOffset + pageSize) {
            m_iptvSearchScrollOffset = m_iptvSearchSelectedIndex - pageSize + 1;
          }
        } else {
          m_iptvVk.inResults = false;
          m_iptvVk.row = 0;
        }
      } else if (input.isButtonJustPressed(Button::LEFT) ||
                 input.isButtonJustPressed(Button::L1)) {
        if (resultCount > 0) {
          m_iptvSearchSelectedIndex =
              std::max(0, m_iptvSearchSelectedIndex - pageSize);
          m_iptvSearchScrollOffset =
              std::max(0, m_iptvSearchScrollOffset - pageSize);
        }
      } else if (input.isButtonJustPressed(Button::RIGHT) ||
                 input.isButtonJustPressed(Button::R1)) {
        if (resultCount > 0) {
          m_iptvSearchSelectedIndex =
              std::min(resultCount - 1, m_iptvSearchSelectedIndex + pageSize);
          if (m_iptvSearchSelectedIndex >=
              m_iptvSearchScrollOffset + pageSize) {
            m_iptvSearchScrollOffset =
                std::min(std::max(0, resultCount - pageSize),
                         m_iptvSearchScrollOffset + pageSize);
          }
        }
      } else if (input.isButtonJustPressed(Button::A) ||
                 input.isButtonJustPressed(Button::START)) {
        if (resultCount > 0 && m_iptvSearchSelectedIndex >= 0 &&
            m_iptvSearchSelectedIndex < resultCount) {
          const auto &selChan = m_iptvSearchResults[m_iptvSearchSelectedIndex];
          showToast("Đang kết nối: " + selChan.name + "...", {0, 180, 255, 255},
                    3000);
          render();
          if (!IPTVManager::instance().playChannel(
                  selChan, m_iptvSearchSelectedIndex, m_iptvSearchResults)) {
            showToast("Không thể phát video (Lỗi kết nối hoặc player)",
                      {239, 68, 68, 255}, 4000);
          }
          m_iptvSearchSelectedIndex =
              IPTVManager::instance().getLastPlayingIndex();
        }
      } else if (input.isButtonJustPressed(Button::X)) {
        if (resultCount > 0 && m_iptvSearchSelectedIndex >= 0 &&
            m_iptvSearchSelectedIndex < resultCount) {
          std::string chanName =
              m_iptvSearchResults[m_iptvSearchSelectedIndex].name;
          bool wasFav = IPTVManager::instance().isFavorite(chanName);
          IPTVManager::instance().toggleFavorite(chanName);
          m_iptvSearchResults[m_iptvSearchSelectedIndex].isFavorite = !wasFav;
          if (!wasFav) {
            showToast("★ Đã thêm vào yêu thích: " + chanName,
                      {250, 204, 21, 255}, 2000);
          } else {
            showToast("☆ Đã xóa khỏi yêu thích: " + chanName,
                      {148, 163, 184, 255}, 2000);
          }
        }
      } else if (input.isButtonJustPressed(Button::Y)) {
        VirtualKeyboard::backspace(m_iptvVk);
        if (!m_iptvVk.query.empty()) {
          m_iptvSearchResults = IPTVManager::instance().search(m_iptvVk.query);
        } else {
          m_iptvSearchResults.clear();
        }
        m_iptvSearchSelectedIndex = 0;
        m_iptvSearchScrollOffset = 0;
      } else if (input.isButtonJustPressed(Button::B)) {
        m_iptvVk.inResults = false;
      }
    }
    break;
  }

  case UIState::YOUTUBE_HOME: {
    // P0-6: HOME = search input + pills + content area
    if (m_ytIsSearching)
      break;

    // Modal mở → xử lý modal input
    if (m_ytSearchModalOpen) {
      // Read input flags locally to keep modal handler pure
      bool a = input.isButtonJustPressed(Button::A);
      bool b = input.isButtonJustPressed(Button::B);
      bool s = input.isButtonJustPressed(Button::START);
      bool u = input.isButtonJustPressed(Button::UP);
      bool d = input.isButtonJustPressed(Button::DOWN);
      bool l = input.isButtonJustPressed(Button::LEFT);
      bool r = input.isButtonJustPressed(Button::RIGHT);
      bool x = input.isButtonJustPressed(Button::X);
      bool y = input.isButtonJustPressed(Button::Y);
      bool l1 = input.isButtonJustPressed(Button::L1);
      bool r1 = input.isButtonJustPressed(Button::R1);
      auto res = SearchInputModal::handleInput(m_ytHomeModalCfg, a, b, s, u, d,
                                               l, r, x, y, l1, r1);
      if (res == SearchInputModal::Result::Commit) {
        m_ytSearchModalOpen = false;
        std::string q = m_ytVk.query;
        if (!q.empty()) {
          m_ytKeyboardQuery = q;
          m_ytLastSearchQuery = q;
          saveYouTubeHistory(q);
          runYouTubeHomeSearch(q);
        }
      } else if (res == SearchInputModal::Result::Cancel) {
        m_ytSearchModalOpen = false;
      }
      break;
    }

    int totalResults = static_cast<int>(m_ytSearchResults.size());
    if (input.isButtonJustPressed(Button::L1)) {
      if (m_ytHomePage > 1) {
        m_ytHomePage--;
        int start = (m_ytHomePage - 1) * 6;
        int end = std::min<int>(start + 6,
                                static_cast<int>(m_ytAllCachedResults.size()));
        if (start < end) {
          m_ytSearchResults.assign(m_ytAllCachedResults.begin() + start,
                                   m_ytAllCachedResults.begin() + end);
          m_ytHomeContentSelected = 0;
          m_ytHomeCol = 0;
          rebuildYtItems();
        }
      }
    } else if (input.isButtonJustPressed(Button::R1)) {
      int maxPage = m_ytInChannelView ? 2 : 100;
      int targetPage = m_ytHomePage + 1;
      int start = (targetPage - 1) * 6;
      if (targetPage <= maxPage &&
          start < static_cast<int>(m_ytAllCachedResults.size())) {
        m_ytHomePage = targetPage;
        int end = std::min<int>(start + 6,
                                static_cast<int>(m_ytAllCachedResults.size()));
        m_ytSearchResults.assign(m_ytAllCachedResults.begin() + start,
                                 m_ytAllCachedResults.begin() + end);
        m_ytHomeContentSelected = 0;
        m_ytHomeCol = 0;
        rebuildYtItems();
      } else {
        showToast(m_ytInChannelView ? "Đã đến trang cuối (2/2)"
                                    : "Đã đến trang cuối",
                  {245, 158, 11, 255}, 1500);
      }
    } else if (input.isButtonJustPressed(Button::LEFT)) {
      if (m_ytHomeRow == 1) {
        // Hàng 1 (Tag): di chuyển sang tag bên trái
        if (--m_ytSelectedCategory < 0)
          m_ytSelectedCategory = static_cast<int>(kYtCategories.size()) - 1;
      } else if (m_ytHomeRow == 2) {
        if (m_ytMatchedChannel.matched && !m_ytInChannelView) {
          // Hàng 2 (Channel Card): 2 cột (0: Logo tròn, 1: Thông tin kênh)
          if (m_ytHomeCol > 0)
            m_ytHomeCol--;
        } else {
          // Hàng 2 (Thumbnail hàng 1): di chuyển trái qua các col (0..2)
          if (m_ytHomeCol > 0) {
            m_ytHomeCol--;
            m_ytHomeContentSelected = m_ytHomeCol;
          }
        }
      } else if (m_ytHomeRow == 3) {
        // Hàng 3 (Thumbnail hàng 2 / hoặc 3 video mới nhất của kênh)
        if (m_ytHomeCol > 0) {
          m_ytHomeCol--;
          m_ytHomeContentSelected =
              (m_ytMatchedChannel.matched && !m_ytInChannelView)
                  ? m_ytHomeCol
                  : (3 + m_ytHomeCol);
        }
      }
    } else if (input.isButtonJustPressed(Button::RIGHT)) {
      if (m_ytHomeRow == 1) {
        // Hàng 1 (Tag): di chuyển sang tag bên phải
        m_ytSelectedCategory =
            (m_ytSelectedCategory + 1) % kYtCategories.size();
      } else if (m_ytHomeRow == 2) {
        if (m_ytMatchedChannel.matched && !m_ytInChannelView) {
          // Hàng 2 (Channel Card): 2 cột (0: Logo, 1: Info)
          if (m_ytHomeCol < 1)
            m_ytHomeCol++;
        } else {
          int row1Count = std::min(3, totalResults);
          if (m_ytHomeCol + 1 < row1Count) {
            m_ytHomeCol++;
            m_ytHomeContentSelected = m_ytHomeCol;
          }
        }
      } else if (m_ytHomeRow == 3) {
        int rowCount = (m_ytMatchedChannel.matched && !m_ytInChannelView)
                           ? std::min(3, totalResults)
                           : std::max(0, totalResults - 3);
        if (m_ytHomeCol + 1 < rowCount) {
          m_ytHomeCol++;
          m_ytHomeContentSelected =
              (m_ytMatchedChannel.matched && !m_ytInChannelView)
                  ? m_ytHomeCol
                  : (3 + m_ytHomeCol);
        }
      }
    } else if (input.isButtonJustPressed(Button::UP)) {
      if (m_ytHomeRow == 1) {
        // Hàng 1 (Tag) -> UP lên Hàng 0 (Search)
        m_ytHomeRow = 0;
        m_ytHomeFocus = 0;
      } else if (m_ytHomeRow == 2) {
        // Hàng 2 -> UP lên Hàng 1 (Tag)
        m_ytHomeRow = 1;
        m_ytHomeFocus = 1;
      } else if (m_ytHomeRow == 3) {
        // Hàng 3 -> UP lên Hàng 2
        m_ytHomeRow = 2;
        m_ytHomeFocus = 2;
        if (m_ytMatchedChannel.matched && !m_ytInChannelView) {
          // Col 0 -> Col 0 (Logo kênh); Col 1 & 2 -> Col 1 (Info kênh)
          m_ytHomeCol = (m_ytHomeCol == 0) ? 0 : 1;
        } else {
          int row1Count = std::min(3, totalResults);
          m_ytHomeCol = std::min(m_ytHomeCol, std::max(0, row1Count - 1));
          m_ytHomeContentSelected = m_ytHomeCol;
        }
      }
    } else if (input.isButtonJustPressed(Button::DOWN)) {
      if (m_ytHomeRow == 0) {
        // Hàng 0 (Search) -> DOWN xuống Hàng 1 (Tag)
        m_ytHomeRow = 1;
        m_ytHomeFocus = 1;
      } else if (m_ytHomeRow == 1 &&
                 (totalResults > 0 ||
                  (m_ytMatchedChannel.matched && !m_ytInChannelView))) {
        // Hàng 1 (Tag) -> DOWN xuống Hàng 2
        m_ytHomeRow = 2;
        m_ytHomeFocus = 2;
        if (m_ytMatchedChannel.matched && !m_ytInChannelView) {
          m_ytHomeCol = std::min(m_ytHomeCol, 1);
        } else {
          int row1Count = std::min(3, totalResults);
          m_ytHomeCol = std::min(m_ytHomeCol, std::max(0, row1Count - 1));
          m_ytHomeContentSelected = m_ytHomeCol;
        }
      } else if (m_ytHomeRow == 2) {
        if (m_ytMatchedChannel.matched && !m_ytInChannelView &&
            totalResults > 0) {
          // Hàng 2 (Channel) -> DOWN xuống Hàng 3 (3 video mới nhất của kênh)
          m_ytHomeRow = 3;
          m_ytHomeFocus = 2;
          m_ytHomeCol = (m_ytHomeCol == 0) ? 0 : 1;
          m_ytHomeCol = std::min(m_ytHomeCol, std::max(0, totalResults - 1));
          m_ytHomeContentSelected = m_ytHomeCol;
        } else if (totalResults > 3) {
          // Hàng 2 (Thumbnail hàng 1) -> DOWN xuống Hàng 3 (Thumbnail hàng 2)
          m_ytHomeRow = 3;
          m_ytHomeFocus = 2;
          int row2Count = totalResults - 3;
          m_ytHomeCol = std::min(m_ytHomeCol, std::max(0, row2Count - 1));
          m_ytHomeContentSelected = 3 + m_ytHomeCol;
        }
      }
    } else if (input.isButtonJustPressed(Button::A)) {
      if (m_ytHomeRow == 0) {
        // Hàng 0: Search box → mở modal nhập
        openYouTubeHomeModal();
      } else if (m_ytHomeRow == 1) {
        // Hàng 1: Pill → fire feed search
        const char *fq = kYtCategories[m_ytSelectedCategory].feedQuery;
        if (fq && fq[0] != '\0') {
          m_ytKeyboardQuery.clear();
          m_ytVk.query.clear();
          m_ytLastSearchQuery = fq;
          runYouTubeHomeSearch(fq);
        } else {
          openYouTubeHomeModal();
        }
      } else if (m_ytHomeRow == 2) {
        if (m_ytMatchedChannel.matched && !m_ytInChannelView) {
          // Bấm A ở Logo kênh (Cột 0) hoặc Cột 1:
          // Chuyển sang trang load các video mới nhất (3 cột 2 hàng, 6
          // thumbnails video, chuẩn bị 12 video cho 2 trang)
          m_ytInChannelView = true;
          m_ytHomePage = 1;
          m_ytHomeRow = 2;
          m_ytHomeCol = 0;
          m_ytHomeContentSelected = 0;
          m_ytAllCachedResults = m_ytChannelAllVideos;
          int count =
              std::min<int>(6, static_cast<int>(m_ytAllCachedResults.size()));
          m_ytSearchResults.assign(m_ytAllCachedResults.begin(),
                                   m_ytAllCachedResults.begin() + count);
          rebuildYtItems();
        } else if (totalResults > 0 && m_ytHomeContentSelected >= 0 &&
                   m_ytHomeContentSelected < totalResults) {
          std::string selected = m_ytSearchResults[m_ytHomeContentSelected];
          std::string videoId = selected.substr(0, selected.find('|'));
          playYouTubeVideo(videoId);
        }
      } else if (m_ytHomeRow == 3 && totalResults > 0) {
        int selIdx = (!m_ytInChannelView && m_ytMatchedChannel.matched)
                         ? m_ytHomeCol
                         : (3 + m_ytHomeCol);
        if (selIdx >= 0 && selIdx < totalResults) {
          std::string selected = m_ytSearchResults[selIdx];
          std::string videoId = selected.substr(0, selected.find('|'));
          playYouTubeVideo(videoId);
        }
      }
    } else if (input.isButtonJustPressed(Button::X)) {
      // X = open search modal (always)
      openYouTubeHomeModal();
    } else if (input.isButtonJustPressed(Button::START)) {
      openYouTubeHomeModal();
    } else if (input.isButtonJustPressed(Button::Y)) {
      // Y = view mode toggle (alt to L1/R1 in row mode)
      toggleYouTubeViewMode();
    } else if (input.isButtonJustPressed(Button::B)) {
      if (m_ytInChannelView) {
        // Thoát khỏi trang video của kênh, quay lại Channel Card Layout
        m_ytInChannelView = false;
        m_ytHomePage = 1;
        m_ytHomeRow = 2;
        m_ytHomeCol = 0;
        int end =
            std::min<int>(3, static_cast<int>(m_ytChannelAllVideos.size()));
        m_ytSearchResults.assign(m_ytChannelAllVideos.begin(),
                                 m_ytChannelAllVideos.begin() + end);
        rebuildYtItems();
      } else if (m_ytLastSearchQuery.rfind("channel:", 0) == 0) {
        const char *fq = kYtCategories[m_ytSelectedCategory].feedQuery;
        std::string target = (fq && fq[0] != '\0') ? fq : "feed:all";
        runYouTubeHomeSearch(target);
      } else {
        setState(UIState::MENU);
      }
    } else if (input.isButtonJustPressed(Button::SELECT)) {
      setState(UIState::MENU);
    }
    break;
  }

  case UIState::YOUTUBE_SEARCH: {
    if (m_ytIsSearching) {
      break; // Ignore input while searching
    }

    std::vector<YtHistoryPill> r3Pills, r4Pills;
    getYouTubeHistoryPills(r3Pills, r4Pills);
    bool hasPills = !r3Pills.empty();
    bool hasR4 = !r4Pills.empty();

    if (m_ytSearchFocus == 1) { // History Pills Focus
      auto &curRowList = (m_ytHistoryRow == 0) ? r3Pills : r4Pills;
      int curCount = static_cast<int>(curRowList.size());

      if (input.isButtonJustPressed(Button::LEFT)) {
        if (curCount > 0) {
          m_ytHistoryCol = (m_ytHistoryCol - 1 + curCount) % curCount;
        }
      } else if (input.isButtonJustPressed(Button::RIGHT)) {
        if (curCount > 0) {
          m_ytHistoryCol = (m_ytHistoryCol + 1) % curCount;
        }
      } else if (input.isButtonJustPressed(Button::UP)) {
        if (m_ytHistoryRow == 1) {
          m_ytHistoryRow = 0;
          if (!r3Pills.empty())
            m_ytHistoryCol =
                std::min(m_ytHistoryCol, static_cast<int>(r3Pills.size()) - 1);
        }
      } else if (input.isButtonJustPressed(Button::DOWN)) {
        if (m_ytHistoryRow == 0 && hasR4) {
          m_ytHistoryRow = 1;
          m_ytHistoryCol =
              std::min(m_ytHistoryCol, static_cast<int>(r4Pills.size()) - 1);
        } else {
          // Drop into keyboard row 0
          m_ytSearchFocus = 2;
          m_ytVk.row = 0;
        }
      } else if (input.isButtonJustPressed(Button::A) ||
                 input.isButtonJustPressed(Button::START)) {
        if (curCount > 0 && m_ytHistoryCol >= 0 && m_ytHistoryCol < curCount) {
          std::string pickedQuery = curRowList[m_ytHistoryCol].text;
          m_ytVk.query = pickedQuery;
          m_ytKeyboardQuery = pickedQuery;
          m_ytLastSearchQuery = pickedQuery;
          saveYouTubeHistory(pickedQuery);
          runYouTubeHomeSearch(pickedQuery);
          setState(UIState::YOUTUBE_HOME);
        }
      } else if (input.isButtonJustPressed(Button::B)) {
        // B moves focus back to keyboard
        m_ytSearchFocus = 2;
      } else if (input.isButtonJustPressed(Button::X)) {
        VirtualKeyboard::typeSpace(m_ytVk);
      } else if (input.isButtonJustPressed(Button::Y)) {
        VirtualKeyboard::backspace(m_ytVk);
      } else if (input.isButtonJustPressed(Button::L1)) {
        m_ytVk.shift = !m_ytVk.shift;
      } else if (input.isButtonJustPressed(Button::R1)) {
        m_ytVk.telexMode = !m_ytVk.telexMode;
        showToast(m_ytVk.telexMode ? "Chế độ: TELEX" : "Chế độ: TIẾNG ANH (US)",
                  UiTheme::ACCENT_GREEN, 1200);
      } else if (input.isButtonJustPressed(Button::SELECT)) {
        setState(UIState::MENU);
      }
      break;
    }

    // m_ytSearchFocus == 2 (Virtual Keyboard Focus)
    if (input.isButtonJustPressed(Button::UP)) {
      if (m_ytVk.row == 0 && hasPills) {
        m_ytSearchFocus = 1;
        m_ytHistoryRow = hasR4 ? 1 : 0;
        auto &targetRow = (m_ytHistoryRow == 0) ? r3Pills : r4Pills;
        m_ytHistoryCol =
            std::min(m_ytHistoryCol,
                     std::max(0, static_cast<int>(targetRow.size()) - 1));
      } else {
        VirtualKeyboard::move(m_ytVk, -1, 0, true);
      }
    } else if (input.isButtonJustPressed(Button::DOWN)) {
      VirtualKeyboard::move(m_ytVk, 1, 0, true);
    } else if (input.isButtonJustPressed(Button::LEFT)) {
      VirtualKeyboard::move(m_ytVk, 0, -1, true);
    } else if (input.isButtonJustPressed(Button::RIGHT)) {
      VirtualKeyboard::move(m_ytVk, 0, 1, true);
    } else if (input.isButtonJustPressed(Button::A)) {
      VkAction act = VirtualKeyboard::pressA(
          m_ytVk,
          [this](const char *) {
            showToast(m_ytVk.telexMode ? "Chế độ: TELEX"
                                       : "Chế độ: TIẾNG ANH (US)",
                      UiTheme::ACCENT_GREEN, 1200);
          },
          true);
      if (act == VkAction::Commit) {
        if (!m_ytVk.query.empty()) {
          saveYouTubeHistory(m_ytVk.query);
          m_ytKeyboardQuery = m_ytVk.query;
          m_ytLastSearchQuery = m_ytVk.query;
          runYouTubeHomeSearch(m_ytVk.query);
        }
        setState(UIState::YOUTUBE_HOME);
      } else if (act == VkAction::Cancel) {
        m_ytKeyboardQuery = m_ytVk.query;
        setState(UIState::YOUTUBE_HOME);
      }
    } else if (input.isButtonJustPressed(Button::L1)) {
      m_ytVk.shift = !m_ytVk.shift;
    } else if (input.isButtonJustPressed(Button::R1)) {
      m_ytVk.telexMode = !m_ytVk.telexMode;
      showToast(m_ytVk.telexMode ? "Chế độ: TELEX" : "Chế độ: TIẾNG ANH (US)",
                UiTheme::ACCENT_GREEN, 1200);
    } else if (input.isButtonJustPressed(Button::X)) {
      VirtualKeyboard::typeSpace(m_ytVk);
    } else if (input.isButtonJustPressed(Button::Y)) {
      VirtualKeyboard::backspace(m_ytVk);
    } else if (input.isButtonJustPressed(Button::START)) {
      if (!m_ytVk.query.empty()) {
        saveYouTubeHistory(m_ytVk.query);
        m_ytKeyboardQuery = m_ytVk.query;
        m_ytLastSearchQuery = m_ytVk.query;
        runYouTubeHomeSearch(m_ytVk.query);
      }
      setState(UIState::YOUTUBE_HOME);
    } else if (input.isButtonJustPressed(Button::B)) {
      // B = Back to YouTube Home
      m_ytKeyboardQuery = m_ytVk.query;
      setState(UIState::YOUTUBE_HOME);
    } else if (input.isButtonJustPressed(Button::SELECT)) {
      setState(UIState::MENU);
    }
    break;
  }

  case UIState::YOUTUBE_RESULTS: {
    if (m_ytIsLoadingVideo) {
      if (input.isButtonJustPressed(Button::B)) {
        m_ytIsLoadingVideo = false;
        m_ytPendingStreamUrl.clear();
        m_ytPendingVideoId.clear();
      }
      break; // Ignore other input while resolving video stream
    }

    int resultCount = static_cast<int>(m_ytSearchResults.size());
    if (resultCount == 0) {
      setState(UIState::YOUTUBE_HOME);
      break;
    }

    const int pageSize = 6;

    // Row-view: list doc, UP/DOWN di chuyen tung dong, LEFT/RIGHT bo qua
    if (input.isButtonJustPressed(Button::UP)) {
      if (m_ytSearchSelectedIndex > 0) {
        m_ytSearchSelectedIndex--;
      }
    } else if (input.isButtonJustPressed(Button::DOWN)) {
      if (m_ytSearchSelectedIndex + 1 < resultCount) {
        m_ytSearchSelectedIndex++;
      }
    } else if (input.isButtonJustPressed(Button::X)) {
      // X = open search modal in HOME
      setState(UIState::YOUTUBE_HOME);
      openYouTubeHomeModal();
    } else if (input.isButtonJustPressed(Button::L1)) {
      if (m_ytCurrentPage > 1) {
        m_ytCurrentPage--;
        int startIdx = (m_ytCurrentPage - 1) * 6;
        int endIdx = std::min<int>(
            startIdx + 6, static_cast<int>(m_ytAllCachedResults.size()));
        if (startIdx < endIdx) {
          m_ytSearchResults.assign(m_ytAllCachedResults.begin() + startIdx,
                                   m_ytAllCachedResults.begin() + endIdx);
          m_ytSearchSelectedIndex = 0;
          m_ytSearchScrollOffset = 0;
          rebuildYtItems();
          std::vector<std::string> vids;
          for (const auto &item : m_ytSearchResults) {
            size_t p = item.find('|');
            if (p != std::string::npos)
              vids.push_back(item.substr(0, p));
          }
          startThumbnailDownloads(vids);
        }
      }
    } else if (input.isButtonJustPressed(Button::R1)) {
      int targetPage = m_ytCurrentPage + 1;
      int startIdx = (targetPage - 1) * 6;
      if (startIdx < static_cast<int>(m_ytAllCachedResults.size())) {
        // Turn page instantly from in-memory cache
        m_ytCurrentPage = targetPage;
        int endIdx = std::min<int>(
            startIdx + 6, static_cast<int>(m_ytAllCachedResults.size()));
        m_ytSearchResults.assign(m_ytAllCachedResults.begin() + startIdx,
                                 m_ytAllCachedResults.begin() + endIdx);
        m_ytSearchSelectedIndex = 0;
        m_ytSearchScrollOffset = 0;
        rebuildYtItems();
        std::vector<std::string> vids;
        for (const auto &item : m_ytSearchResults) {
          size_t p = item.find('|');
          if (p != std::string::npos)
            vids.push_back(item.substr(0, p));
        }
        startThumbnailDownloads(vids);
      } else if (!m_ytIsSearching) {
        // Fetch next page dynamically via script
        m_ytIsSearching = true;
        m_ytSearchStartMs = SDL_GetTicks();
        showToast("Đang tải trang tiếp theo...", {0, 180, 216, 255}, 1500);
        std::string query = m_ytLastSearchQuery;
        m_ytSearchTask.run([this, query, targetPage, startIdx](TaskProgress &) {
          std::string appRoot = AppConfig::instance().getAppRoot();
          if (appRoot.empty())
            appRoot = "/mnt/SDCARD/Apps/RomCloud";
          // P0-3: Pagination cũng dùng Python smart_search để consistent
          std::string escapedQuery;
          for (char c : query) {
            if (c == '"' || c == '\\' || c == '$' || c == '`')
              escapedQuery += '\\';
            escapedQuery += c;
          }
          int maxResults = std::min(targetPage * 6, 50);
          std::string cmd = "/mnt/SDCARD/System/bin/python3 \"" + appRoot +
                            "/scripts/youtube_search.py\" smart \"" +
                            escapedQuery + "\" " + std::to_string(maxResults) +
                            " 2>/tmp/yt_search_err.log";
          FILE *pipe = popen(cmd.c_str(), "r");
          std::vector<std::string> moreResults;
          if (pipe) {
            char buffer[2048];
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
              std::string line(buffer);
              while (!line.empty() &&
                     (line.back() == '\n' || line.back() == '\r'))
                line.pop_back();
              if (line.empty())
                continue;
              if (line.find("ERROR:") == 0 || line.find("WARNING:") == 0)
                continue;
              int pipeCount = std::count(line.begin(), line.end(), '|');
              if (line.compare(0, 8, "CHANNEL|") == 0) {
                if (pipeCount >= 4)
                  moreResults.push_back(line);
              } else if (pipeCount >= 4) {
                moreResults.push_back(line);
              }
            }
            pclose(pipe);
          }
          if (!moreResults.empty()) {
            m_ytAllCachedResults.insert(m_ytAllCachedResults.end(),
                                        moreResults.begin(), moreResults.end());
            m_ytCurrentPage = targetPage;
            int endIdx = std::min<int>(
                startIdx + 6, static_cast<int>(m_ytAllCachedResults.size()));
            m_ytSearchResults.assign(m_ytAllCachedResults.begin() + startIdx,
                                     m_ytAllCachedResults.begin() + endIdx);
            m_ytSearchSelectedIndex = 0;
            m_ytSearchScrollOffset = 0;
            rebuildYtItems();
            std::vector<std::string> vids;
            for (const auto &item : m_ytSearchResults) {
              size_t p = item.find('|');
              if (p != std::string::npos)
                vids.push_back(item.substr(0, p));
            }
            startThumbnailDownloads(vids);
          } else {
            std::string err = ytErrTail("/tmp/yt_search_err.log");
            if (!err.empty())
              Logger::warn("[YouTube] next page empty err=" + err);
            showToast("Đã đến trang cuối", {245, 158, 11, 255}, 2000);
          }
          m_ytIsSearching = false;
        });
      }
    } else if (input.isButtonJustPressed(Button::A)) {
      if (m_ytSearchSelectedIndex >= 0 &&
          m_ytSearchSelectedIndex < resultCount &&
          m_ytSearchSelectedIndex < static_cast<int>(m_ytItems.size())) {
        const auto &item = m_ytItems[m_ytSearchSelectedIndex];
        if (item.type == YtItem::Type::Channel && !item.channelId.empty()) {
          // P0-3: click A trên Channel row → search latest videos từ kênh
          // Dùng syntax đặc biệt "channel:<id>" để python backend biết
          m_ytVk.query = "channel:" + item.channelId;
          Logger::error("[YT] Channel selected, search latest from " +
                        item.channelId);
          triggerYouTubeSearch();
        } else {
          std::string selected = m_ytSearchResults[m_ytSearchSelectedIndex];
          std::string videoId = selected.substr(0, selected.find('|'));
          Logger::error("[YT] Play");
          playYouTubeVideo(videoId);
        }
      }
    } else if (input.isButtonJustPressed(Button::B)) {
      // B = quay lai HOME (giu query), MENU chi tu HOME
      Logger::error("[YT] B pressed in RESULTS -> go HOME");
      m_ytIsSearching = false;
      setState(UIState::YOUTUBE_HOME);
    } else if (input.isButtonJustPressed(Button::START)) {
      m_ytIsSearching = false;
      setState(UIState::YOUTUBE_HOME);
    } else if (input.isButtonJustPressed(Button::SELECT)) {
      m_ytIsSearching = false;
      setState(UIState::MENU);
    }

    m_ytSearchScrollOffset = (m_ytSearchSelectedIndex / pageSize) * pageSize;
    break;
  }

  // ===========================================================\r\n        //
  // LocalSend P2P states
  // ===========================================================
  case UIState::LOCALSEND_HOME: {
    auto devices = LocalSendManager::instance().knownDevices();
    int devCount = (int)devices.size();

    if (input.isButtonJustPressed(Button::B)) {
      LocalSendManager::instance().stop();
      setState(UIState::MENU);
      break;
    } else if (input.isButtonJustPressed(Button::Y)) {
      // Refresh: dọn list cũ + quét burst ngay (thấy máy trong ~1s)
      LocalSendManager::instance().refreshDiscovery();
      m_localSendSelectedDevice = 0;
      showToast("Đang quét thiết bị...", {100, 116, 139, 255}, 1500);
    } else if (input.isButtonJustPressed(Button::X)) {
      if (m_localSendMode == 0) {
        // Apps tab tạm ẩn: X chỉ refresh ROMs
        m_lsPickerTab = 0;
        m_lsRomListLoaded = false;
        m_lsRomSelected = 0;
        m_lsRomScrollOffset = 0;
        showToast("Đã refresh ROMs", {100, 116, 139, 255}, 1000);
      } else {
        std::string cur = LocalSendManager::instance().currentTargetFolder();
        if (cur.empty())
          cur = "/mnt/SDCARD";
        else
          cur = "/mnt/SDCARD/" + cur;
        if (!FileSystemManager::instance().directoryExists(cur))
          cur = "/mnt/SDCARD";
        m_expPicker.setDirOnly(true);
        m_expPicker.open(cur);
        setState(UIState::LOCALSEND_FOLDER);
      }
    } else if (input.isButtonJustPressed(Button::LEFT) ||
               input.isButtonJustPressed(Button::RIGHT)) {
      // L/R: chọn thiết bị trong SEND (Up/Down đã dùng để toggle Send/Receive)
      if (m_localSendMode == 0 && devCount > 0) {
        int dir = input.isButtonJustPressed(Button::RIGHT) ? 1 : -1;
        m_localSendSelectedDevice =
            (m_localSendSelectedDevice + dir + devCount) % devCount;
      }
    } else if (input.isButtonJustPressed(Button::UP) ||
               input.isButtonJustPressed(Button::DOWN)) {
      // DPad Up/Down: toggle Send/Receive
      m_localSendMode = (m_localSendMode == 0) ? 1 : 0;
    } else if (input.isButtonJustPressed(Button::A) && m_localSendMode == 0 &&
               devCount > 0) {
      // Vào SEND: rescan /Roms mỗi lần (flag false → render quét lại)
      m_lsPickerTab = 0;
      m_lsRomListLoaded = false;
      m_lsRomSelected = 0;
      m_lsRomScrollOffset = 0;
      setState(UIState::LOCALSEND_GAME_PICKER);
    }
    break;
  }

  case UIState::LOCALSEND_INCOMING: {
    if (m_localSendIncomingMode == 1) {
      handleFolderPickerInput();
    } else {
      if (input.isButtonJustPressed(Button::A)) {
        std::string saveDir = m_localSendIncomingSavePath;
        if (saveDir.empty())
          saveDir = "/mnt/SDCARD/Downloads";
        uint64_t need = m_localSendCurrentPrompt.file.size;
        if (need > 0) {
          uint64_t freeB = LsUtil::sdFreeBytes("/mnt/SDCARD");
          if (freeB < need) {
            showToast("Thẻ đầy: cần " + LsUtil::humanSize(need) + ", trống " +
                          LsUtil::humanSize(freeB),
                      {239, 68, 68, 255}, 3000);
            break;
          }
        }
        LocalSendManager::instance().approveUploadWithPath(
            m_localSendPendingSessionId, saveDir);
        showToast("Đã chấp nhận: " + m_localSendCurrentPrompt.file.fileName,
                  {34, 197, 94, 255}, 2000);
        setState(UIState::LOCALSEND_PROGRESS);
      } else if (input.isButtonJustPressed(Button::X)) {
        std::string p = m_localSendIncomingSavePath;
        if (p.empty() || !FileSystemManager::instance().directoryExists(p))
          p = "/mnt/SDCARD";
        m_expPicker.setDirOnly(true);
        m_expPicker.open(p);
        m_localSendIncomingMode = 1;
      } else if (input.isButtonJustPressed(Button::B)) {
        LocalSendManager::instance().rejectUpload(m_localSendPendingSessionId);
        showToast("Đã từ chối", {239, 68, 68, 255}, 1500);
        setState(UIState::LOCALSEND_HOME);
      }
    }
    break;
  }

  case UIState::LOCALSEND_FOLDER: {
    handleFolderPickerInput();
    break;
  }

  case UIState::FILE_EXPLORER: {
    if (!handleExplorerInput())
      setState(UIState::MENU);
    break;
  }

  case UIState::DEST_PICKER: {
    handleFolderPickerInput();
    break;
  }

  case UIState::TEXT_VIEWER: {
    handleTextViewerInput();
    break;
  }

  case UIState::CLOCK_ALARM: {
    handleAlarmRingInput();
    break;
  }

  case UIState::WEATHER: {
    handleWeatherInput();
    break;
  }

  case UIState::PORTAL: {
    handlePortalInput();
    break;
  }

  case UIState::LOCALSEND_SEND: {
    // Legacy: chuyển thẳng sang Game Picker (LOCALSEND_SEND giờ chỉ là
    // bước chọn device → mở game picker).
    m_lsRomSelected = 0;
    m_lsRomScrollOffset = 0;
    setState(UIState::LOCALSEND_GAME_PICKER);
    break;
  }

  case UIState::LOCALSEND_GAME_PICKER: {
    // Tạm ẩn Apps tab: chỉ quét /Roms, rescan mỗi lần vào.
    m_lsPickerTab = 0;
    int n = (int)m_lsRomList.size();
    if (input.isButtonJustPressed(Button::B)) {
      setState(UIState::LOCALSEND_HOME);
    } else if (input.isButtonJustPressed(Button::Y)) {
      m_lsRomListLoaded = false;
      showToast("Đã refresh", {100, 116, 139, 255}, 1000);
    } else if (input.isButtonJustPressed(Button::A) && n > 0) {
      auto devices = LocalSendManager::instance().knownDevices();
      if (devices.empty() || m_localSendSelectedDevice < 0 ||
          m_localSendSelectedDevice >= (int)devices.size()) {
        showToast("Chưa chọn thiết bị đích", {239, 68, 68, 255}, 1500);
        break;
      }
      const auto &dev = devices[m_localSendSelectedDevice];

      // ROMS: gửi với relative path chính xác để receiver tái tạo
      // đúng cấu trúc thư mục (vd Roms/GBA/sub/...).
      if (m_lsRomSelected < 0 || m_lsRomSelected >= (int)m_lsRomList.size())
        break;
      const auto &e0 = m_lsRomList[m_lsRomSelected];
      if (!FileSystemManager::instance().fileExists(e0.path)) {
        showToast("File không tồn tại", {239, 68, 68, 255}, 2000);
        break;
      }
      LsFileMeta meta;
      meta.fileName = e0.name;
      // Tính relativePath chính xác từ /mnt/SDCARD/ (giữ subfolder).
      {
        const std::string prefix = "/mnt/SDCARD/";
        if (e0.path.compare(0, prefix.size(), prefix) == 0) {
          std::string sub = e0.path.substr(prefix.size());
          auto slash = sub.find_last_of('/');
          meta.relativePath =
              (slash == std::string::npos) ? "" : sub.substr(0, slash + 1);
        } else {
          meta.relativePath = "Roms/" + e0.systemDir + "/";
        }
      }
      meta.gameTitle = e0.name;
      std::string sid =
          LocalSendManager::instance().sendFileMetaAsync(meta, e0.path, dev);
      if (sid.empty()) {
        showToast("Gửi thất bại (mạng?)", {239, 68, 68, 255}, 2000);
      } else {
        m_localSendProgressSel = 0;
        m_localSendProgressScroll = 0;
        showToast("Đang gửi: " + e0.name, {34, 197, 94, 255}, 2000);
        setState(UIState::LOCALSEND_PROGRESS);
      }
    } else if (input.isButtonJustPressed(Button::UP) && n > 0) {
      m_lsRomSelected = (m_lsRomSelected - 1 + n) % n;
    } else if (input.isButtonJustPressed(Button::DOWN) && n > 0) {
      m_lsRomSelected = (m_lsRomSelected + 1) % n;
    } else if (input.isButtonJustPressed(Button::L1) && n > 0) {
      m_lsRomSelected = (m_lsRomSelected - 10 + n) % n;
    } else if (input.isButtonJustPressed(Button::R1) && n > 0) {
      m_lsRomSelected = (m_lsRomSelected + 10) % n;
    }
    break;
  }

  case UIState::LOCALSEND_PROGRESS: {
    auto sends = LocalSendManager::instance().sendProgresses();
    auto recvs = LocalSendManager::instance().receiveProgresses();
    int total = (int)sends.size() + (int)recvs.size();
    if (input.isButtonJustPressed(Button::B) ||
        input.isButtonJustPressed(Button::A)) {
      setState(UIState::LOCALSEND_HOME);
      break;
    }
    if (input.isButtonJustPressed(Button::Y)) {
      LocalSendManager::instance().clearFinishedTasks();
      m_localSendProgressSel = 0;
      m_localSendProgressScroll = 0;
      showToast("Đã xóa các tác vụ đã xong/lỗi", {34, 197, 94, 255}, 1500);
    }
    if (input.isButtonJustPressed(Button::UP) && total > 0) {
      m_localSendProgressSel = (m_localSendProgressSel - 1 + total) % total;
    } else if (input.isButtonJustPressed(Button::DOWN) && total > 0) {
      m_localSendProgressSel = (m_localSendProgressSel + 1) % total;
    }
    // Tự động về Home sau 3s khi tất cả gửi/nhận THÀNH CÔNG (nếu có lỗi giữ
    // nguyên để user xem)
    bool allSuccess = total > 0;
    for (auto &s : sends) {
      if (s.state != LsSendProgress::DONE) {
        allSuccess = false;
        break;
      }
    }
    if (allSuccess) {
      for (auto &r : recvs) {
        if (r.state != LsUploadRequest::DONE) {
          allSuccess = false;
          break;
        }
      }
    }
    uint32_t now = SDL_GetTicks();
    if (allSuccess) {
      if (m_localSendProgressDoneMs == 0)
        m_localSendProgressDoneMs = now;
      if (now - m_localSendProgressDoneMs > 3500) {
        m_localSendProgressDoneMs = 0;
        setState(UIState::LOCALSEND_HOME);
      }
    } else {
      m_localSendProgressDoneMs = 0;
    }
    break;
  }

  case UIState::GAME_CAST: {
    if (input.isButtonJustPressed(Button::B)) {
      setState(UIState::MENU);
    } else if (input.isButtonJustPressed(Button::A)) {
      // P1-2: chặn A start khi Wi-Fi mất (IP=192.168.x.x) — không cho user
      // bật stream khi đầu bên kia không có cách kết nối.
      bool noWifi = (CastManager::instance().getIpAddress() == "192.168.x.x");
      if (noWifi) {
        showToast("⚠ Wi-Fi chưa kết nối - vào Settings để kết nối",
                  {252, 165, 165, 255}, 2200);
        break;
      }
      bool currentlyRunning = CastManager::instance().isRunning();
      if (currentlyRunning) {
        CastManager::instance().stop();
        showToast("GameCast: ĐÃ TẮT (Đã dừng stream triệt để)",
                  {239, 68, 68, 255}, 1500);
      } else {
        bool ok =
            CastManager::instance().start(CastManager::instance().isHdMode());
        if (ok) {
          showToast("GameCast: ĐÃ BẬT - Sẵn sàng stream!", {34, 197, 94, 255},
                    2000);
        } else {
          showToast("GameCast: LỖI KHỞI ĐỘNG (Kiểm tra cổng/mạng)",
                    {239, 68, 68, 255}, 2000);
        }
      }
    } else if (input.isButtonJustPressed(Button::X)) {
      // Chi X moi doi do phan giai. Y khong bind de tranh nham lan (footer chi
      // show X). Xem `renderFooter` cho hint tuong ung.
      bool curHd = CastManager::instance().isHdMode();
      CastManager::instance().setHdMode(!curHd);
      if (!curHd) {
        showToast("Độ phân giải: 1024x768 (Native HD)", {59, 130, 246, 255},
                  1500);
      } else {
        showToast("Độ phân giải: 512x384 (Tiết kiệm Wi-Fi)",
                  {245, 158, 11, 255}, 1500);
      }
    }
    break;
  }

  default:
    break;
  }
}

std::string UIManager::suggestNewFolderName(const std::string &parentPath) {
  // Đề xuất tên folder mới không trùng: NewFolder, NewFolder_2, NewFolder_3,
  // ...
  auto exists = [&](const std::string &p) {
    return FileSystemManager::instance().directoryExists(p);
  };
  std::string base = parentPath + "/NewFolder";
  if (!exists(base))
    return "NewFolder";
  for (int i = 2; i < 1000; ++i) {
    std::string cand = base + "_" + std::to_string(i);
    if (!exists(cand))
      return cand.substr(parentPath.size() + 1);
  }
  return "NewFolder";
}

void UIManager::clearTextCache() { m_ui.clearTextCache(); }

void UIManager::releaseHeavyState() {
  // Chữ render lại trong 1-2 frame; thumb worker drain + trim ở trong.
  // Không đụng player (stop path riêng đã kill/reap) và không đụng
  // stream-URL cache (giữ để vào lại phát nhanh).
  clearTextCache();
  clearThumbnailCache();
#ifndef PC_SIMULATOR_MODE
  // Thả pagecache video đã chết (mpv đã kill ở stop path). Best-effort.
  FILE* f = fopen("/proc/sys/vm/drop_caches", "w");
  if (f) {
    fputs("1\n", f);
    fclose(f);
  }
#endif
  Logger::info("Cleaned heavy state, UI ready");
}

void UIManager::drawText(const std::string &text, int x, int y, SDL_Color color,
                         TTF_Font *font, bool centered) {
  m_ui.drawText(text, x, y, color, font, centered);
}

void UIManager::drawFlipDigits(const std::string &newText,
                               const std::string &oldText, float t01, int x,
                               int y, int w, int h, TTF_Font *font,
                               SDL_Color color) {
  m_ui.drawFlipDigits(newText, oldText, t01, x, y, w, h, font, color);
}

void UIManager::drawRect(int x, int y, int w, int h, SDL_Color color,
                         bool filled) {
  m_ui.drawRect(x, y, w, h, color, filled);
}

void UIManager::drawBorder(int x, int y, int w, int h, SDL_Color color,
                           int thickness) {
  m_ui.drawBorder(x, y, w, h, color, thickness);
}

void UIManager::drawRoundedRect(int x, int y, int w, int h, int radius,
                                SDL_Color color, bool filled) {
  m_ui.drawRoundedRect(x, y, w, h, radius, color, filled);
}

void UIManager::drawRoundedBorder(int x, int y, int w, int h, int radius,
                                  SDL_Color color, int thickness) {
  m_ui.drawRoundedBorder(x, y, w, h, radius, color, thickness);
}

void UIManager::drawBadge(int x, int y, int w, int h, const std::string &text,
                          SDL_Color bg, SDL_Color fg) {
  m_ui.drawBadge(x, y, w, h, text, bg, fg);
}

void UIManager::drawDot(int cx, int cy, int r, SDL_Color color) {
  drawRoundedRect(cx - r, cy - r, r * 2, r * 2, r, color, true);
}

void UIManager::drawIcon(const std::string &iconName, int x, int y, int w,
                         int h) {
  m_ui.drawIcon(iconName, x, y, w, h);
}

void UIManager::drawPlayerIcon(const std::string &iconName, int x, int y, int w,
                               int h) {
  m_ui.drawPlayerIcon(iconName, x, y, w, h);
}

void UIManager::drawButtonIcon(const std::string &button, int x, int y,
                               int size) {
  m_ui.drawButtonIcon(button, x, y, size);
}

int UIManager::textHeight(TTF_Font *font) { return m_ui.textHeight(font); }

int UIManager::textWidth(const std::string &text, TTF_Font *font) {
  return m_ui.textWidth(text, font);
}

std::string UIManager::truncateToWidth(const std::string &text, TTF_Font *font,
                                       int maxPx) {
  return m_ui.truncateToWidth(text, font, maxPx);
}

int UIManager::pillWidth(const std::string &text, TTF_Font *font) {
  return m_ui.pillWidth(text, font);
}

int UIManager::badgeWidth(const std::string &text, int h) {
  return m_ui.badgeWidth(text, h, m_fontSmall);
}

int UIManager::buttonWidth(const std::string &label) {
  return m_ui.buttonWidth(label, m_fontSmall);
}

int UIManager::badgeDualWidth(const std::string &label1,
                              const std::string &label2, int h) {
  return m_ui.badgeDualWidth(label1, label2, h);
}

void UIManager::drawPill(int x, int y, int w, int h, const std::string &text,
                         bool active, TTF_Font *font) {
  m_ui.drawPill(x, y, w, h, text, active, font);
}

void UIManager::drawButton(int x, int y, int w, int h, const std::string &label,
                           bool focused, bool danger) {
  m_ui.drawButton(x, y, w, h, label, focused, danger);
}

void UIManager::drawRow(int x, int y, int w, int h, bool focused, bool dim) {
  m_ui.drawRow(x, y, w, h, focused, dim);
}

int UIManager::textYCentered(int y, int h, TTF_Font *font) {
  return m_ui.textYCentered(y, h, font);
}

void UIManager::drawRowMainSub(int x, int y, int h, const std::string &main,
                               TTF_Font *fMain, const std::string &sub,
                               TTF_Font *fSub, int maxW, int gap) {
  m_ui.drawRowMainSub(x, y, h, main, fMain, sub, fSub, maxW, gap);
}

void UIManager::drawTextRight(const std::string &text, int rightX, int y,
                              SDL_Color color, TTF_Font *font) {
  m_ui.drawTextRight(text, rightX, y, color, font);
}

int UIManager::drawFooterHint(const std::string &button,
                              const std::string &label, int x, int barY,
                              int barH, SDL_Color color, TTF_Font *font,
                              int iconSize, int gap) {
  return m_ui.drawFooterHint(button, label, x, barY, barH, color, font,
                             iconSize, gap);
}

void UIManager::drawFooterHintsCentered(
    const std::vector<std::pair<std::string, std::string>> &hints, int barY,
    int barH, SDL_Color color, TTF_Font *font, int iconSize, int gap,
    int hintGap) {
  m_ui.drawFooterHintsCentered(hints, barY, barH, color, font, iconSize, gap,
                               hintGap);
}

void UIManager::drawBadgeDual(int x, int y, int w, int h,
                              const std::string &btn1,
                              const std::string &label1,
                              const std::string &btn2,
                              const std::string &label2, SDL_Color bg,
                              SDL_Color fg) {
  m_ui.drawBadgeDual(x, y, w, h, btn1, label1, btn2, label2, bg, fg);
}

void UIManager::drawInlineHintsCentered(const std::string &text, int centerX,
                                        int y, SDL_Color color, TTF_Font *font,
                                        int iconSize, int gap) {
  m_ui.drawInlineHintsCentered(text, centerX, y, color, font, iconSize, gap);
}

void UIManager::drawAppBackground() { m_ui.drawAppBackground(); }
void UIManager::drawRoundedTopBar(int x, int y, int w, int h, int radius,
                                  SDL_Color color) {
  m_ui.drawRoundedTopBar(x, y, w, h, radius, color);
}
void UIManager::drawModalDialog(int x, int y, int w, int h, int radius,
                                SDL_Color bodyBg, SDL_Color titleBg,
                                int titleH) {
  m_ui.drawModalDialog(x, y, w, h, radius, bodyBg, titleBg, titleH);
}
void UIManager::drawCard(int x, int y, int w, int h) {
  m_ui.drawCard(x, y, w, h);
}
void UIManager::drawFocusRow(int x, int y, int w, int h) {
  m_ui.drawFocusRow(x, y, w, h);
}

void UIManager::drawHighlight(int x, int y, int w, int h) {
  if (!m_renderer || w <= 0 || h <= 0)
    return;
  SDL_BlendMode prev;
  SDL_GetRenderDrawBlendMode(m_renderer, &prev);
  SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
  SDL_Color c = UiTheme::FOCUS_BG_SOFT;
  SDL_SetRenderDrawColor(m_renderer, c.r, c.g, c.b, c.a);
  SDL_Rect r = {
      PlatformInfo::instance().scaleX(x), PlatformInfo::instance().scaleY(y),
      PlatformInfo::instance().scaleW(w), PlatformInfo::instance().scaleH(h)};
  SDL_RenderFillRect(m_renderer, &r);
  SDL_SetRenderDrawBlendMode(m_renderer, prev);
}
void UIManager::drawAppHeader(const std::string &title,
                              const std::string &sub) {
  m_ui.drawAppHeader(title, sub);
}
void UIManager::drawHeaderStatus() { m_ui.drawHeaderStatus(); }
void UIManager::drawPadIcon(UiTheme::PadBtn btn, int x, int y, int size) {
  m_ui.drawPadIcon(btn, x, y, size);
}
void UIManager::drawAppFooter(const std::vector<UiTheme::FooterHint> &hints) {
  m_ui.drawAppFooter(hints);
}
void UIManager::beginModalDim() { m_ui.beginModalDim(); }

void UIManager::drawGridIcon(const std::string &iconFile, int x, int y, int w,
                             int h) {
  m_ui.drawGridIcon(iconFile, x, y, w, h);
}

void UIManager::renderHeader() {
  // Header removed to maximize full-screen view for all screens
}

void UIManager::renderSearchState() {
  // ─── Left panel: keyboard + query bar (Borderless) ───
  int panelW = 430;
  int panelX = 24;
  int panelY = 74;

  // Query bar with rounded corners
  drawRoundedRect(panelX, panelY, panelW, 52, UiTheme::RADIUS_ROW,
                  {22, 32, 46, 255}, true);
  drawRoundedBorder(panelX, panelY, panelW, 52, UiTheme::RADIUS_ROW,
                    {0, 180, 216, 255}, 2);
  std::string displayQuery = m_searchVk.query.empty()
                                 ? UiStrings::SEARCH_PROMPT_INPUT
                                 : m_searchVk.query + "_";
  SDL_Color qColor = m_searchVk.query.empty() ? SDL_Color{80, 95, 115, 255}
                                              : SDL_Color{255, 255, 255, 255};
  drawText(displayQuery, panelX + 16, panelY + 14, qColor, m_fontMedium);

  // Unified keyboard (P0-2): charset media + action row, stride2 nhu YouTube.
  {
    static std::string s0, s1, s2, s3, s4;
    s0 = m_searchVk.shift ? "ABC" : "abc";
    s1 = m_searchVk.telexMode ? "TELEX" : "US";
    s2 = "Cách";
    s3 = "Xóa";
    s4 = "Tìm";
    const char *sActs[5] = {s0.c_str(), s1.c_str(), s2.c_str(), s3.c_str(),
                            s4.c_str()};
    VkState sDraw = m_searchVk;
    if (m_searchVk.inResults)
      sDraw.row = -1;
    m_ui.drawVirtualKeyboard(sDraw, panelX + 12, panelY + 68, 37, 40, 4, 6,
                             SDL_Color{0, 140, 230, 255},
                             SDL_Color{0, 180, 255, 255}, sActs, true, true, 2);
  }

  // ─── Divider line ───
  drawRect(476, 64, 1, 651, {38, 48, 64, 255}, true);

  // ─── Right panel: results (Borderless) ───
  int rPanelX = 496;
  int rPanelW = 1024 - rPanelX - 24;
  int rPanelY = panelY;
  int rPanelH = 630;

  int numResults = static_cast<int>(m_searchResults.size());
  if (m_searchVk.query.length() < 2) {
    drawText(UiStrings::SEARCH_PROMPT_MIN_CHARS, rPanelX + rPanelW / 2,
             rPanelY + 60, {80, 95, 115, 255}, m_fontSmall, true);
  } else if (numResults == 0) {
    drawText(UiStrings::SEARCH_NO_RESULTS, rPanelX + rPanelW / 2, rPanelY + 60,
             {239, 68, 68, 255}, m_fontSmall, true);
  } else {
    std::string countStr =
        std::to_string(numResults) + UiStrings::SEARCH_RESULTS_SUFFIX;
    drawText(countStr, rPanelX + rPanelW / 2, rPanelY + 8, {100, 115, 135, 255},
             m_fontSmall, true);

    int pageSize = 10;
    int itemH = 58;
    int listStartY = rPanelY + 36;

    for (int i = 0; i < pageSize && (m_searchScrollOffset + i) < numResults;
         i++) {
      int idx = m_searchScrollOffset + i;
      const auto &g = m_searchResults[idx];
      bool isSel = (m_searchVk.inResults && idx == m_searchSelectedIndex);

      int itemY = listStartY + i * itemH;
      SDL_Color rowBg =
          isSel ? SDL_Color{2, 55, 82, 255} : SDL_Color{20, 28, 42, 255};
      drawRoundedRect(rPanelX, itemY, rPanelW, itemH - 4, UiTheme::RADIUS_CARD,
                      rowBg, true);
      if (isSel) {
        drawRoundedBorder(rPanelX, itemY, rPanelW, itemH - 4,
                          UiTheme::RADIUS_CARD, {0, 180, 216, 255}, 2);
      }

      // State badge (pill)
      bool isLocal = (g.localState == GameState::LOCAL);
      SDL_Color badgeBg =
          isLocal ? SDL_Color{22, 78, 99, 255} : SDL_Color{45, 30, 72, 255};
      SDL_Color badgeFg =
          isLocal ? SDL_Color{34, 197, 94, 255} : SDL_Color{168, 85, 247, 255};
      std::string stateLabel = isLocal ? UiStrings::SEARCH_BADGE_LOCAL
                                       : UiStrings::SEARCH_BADGE_CLOUD;
      drawBadge(rPanelX + 12, itemY + 12, 0, 28, stateLabel, badgeBg, badgeFg);

      // Title (truncate theo pixel, khong cat giua UTF-8)
      std::string title = truncateToWidth(g.title, m_fontMedium, rPanelW - 130);
      drawText(title, rPanelX + 102, itemY + 8, {230, 240, 255, 255},
               m_fontMedium);

      // System label
      drawText(g.systemCode, rPanelX + 102, itemY + 34, {100, 115, 135, 255},
               m_fontSmall);
    }

    // Scroll indicator
    if (numResults > pageSize) {
      float scrollFrac =
          static_cast<float>(m_searchScrollOffset) / (numResults - pageSize);
      int scrollBarH = rPanelH - 46;
      int thumbH = std::max(24, scrollBarH / (numResults / pageSize + 1));
      int thumbY =
          rPanelY + 38 + static_cast<int>(scrollFrac * (scrollBarH - thumbH));
      drawRoundedRect(rPanelX + rPanelW - 6, rPanelY + 38, 4, scrollBarH, 2,
                      {30, 42, 58, 255}, true);
      drawRoundedRect(rPanelX + rPanelW - 6, thumbY, 4, thumbH, 2,
                      {0, 180, 216, 255}, true);
    }
  }

  // Hint: which panel is active
  std::string hint = m_searchVk.inResults ? UiStrings::SEARCH_NAV_UP_HINT
                                          : UiStrings::SEARCH_NAV_DOWN_HINT;
  drawText(hint, 512, 705, {60, 75, 95, 255}, m_fontSmall, true);
}

void UIManager::renderFooter() {
  if (m_currentState == UIState::MENU || m_currentState == UIState::SETTINGS ||
      m_currentState == UIState::DIAGNOSTICS ||
      m_currentState == UIState::OTA_CHANGELOG ||
      m_currentState == UIState::IPTV_PLAYLIST_SELECT ||
      m_currentState == UIState::IPTV_LIST ||
      m_currentState == UIState::IPTV_SEARCH ||
      m_currentState == UIState::YOUTUBE_HOME ||
      m_currentState == UIState::YOUTUBE_SEARCH ||
      m_currentState == UIState::YOUTUBE_RESULTS ||
      m_currentState == UIState::LOCALSEND_HOME ||
      m_currentState == UIState::LOCALSEND_INCOMING ||
      m_currentState == UIState::LOCALSEND_FOLDER ||
      m_currentState == UIState::LOCALSEND_SEND ||
      m_currentState == UIState::LOCALSEND_GAME_PICKER ||
      m_currentState == UIState::LOCALSEND_PROGRESS ||
      m_currentState == UIState::FILE_EXPLORER ||
      m_currentState == UIState::GAME_CAST ||
      m_currentState == UIState::PORTAL) {
    return;
  }

  // WEATHER (thời tiết/đồng hồ/camera) tự vẽ footer riêng — global vẽ đè
  // sẽ xóa hint của trang. Picker tỉnh/xã (m_wxMode != 0) không có footer
  // riêng nên giữ global.
  if (m_currentState == UIState::WEATHER && m_wxMode == 0) {
    return;
  }

  const int barY = UiTheme::FOOTER_Y, barH = UiTheme::FOOTER_H,
            iconSize = UiTheme::FOOTER_ICON, gap = UiTheme::FOOTER_GAP;
  drawRect(0, barY, UiTheme::APP_W, barH, UiTheme::FOOTER_BG, true);
  drawRect(0, barY, UiTheme::APP_W, 1, UiTheme::FOOTER_LINE, true);

  SDL_Color fg = {210, 220, 230, 255};
  SDL_Color red = {239, 68, 68, 255};
  SDL_Color cyan = {0, 180, 216, 255};
  int x = 20;

  if (m_currentState == UIState::GAME_LIST) {
    bool isLocal = false;
    if (m_selectedGameIndex >= 0 &&
        m_selectedGameIndex < static_cast<int>(m_cachedGames.size())) {
      isLocal =
          (m_cachedGames[m_selectedGameIndex].localState == GameState::LOCAL);
    }

    x = drawFooterHint("A", isLocal ? "Chơi" : "Tải", x, barY, barH, fg,
                       m_fontSmall, iconSize, gap) +
        30;
    x = drawFooterHint("B", "Lùi", x, barY, barH, fg, m_fontSmall, iconSize,
                       gap) +
        30;
    if (isLocal) {
      x = drawFooterHint("X", "Xóa ROM", x, barY, barH, red, m_fontSmall,
                         iconSize, gap) +
          30;
    }
    x = drawFooterHint("Y", "Nhảy chữ", x, barY, barH, fg, m_fontSmall,
                       iconSize, gap) +
        30;
    x = drawFooterHint("SELECT", "Lọc", x, barY, barH, fg, m_fontSmall,
                       iconSize, gap) +
        30;
    x = drawFooterHint("START", "Tìm kiếm", x, barY, barH, cyan, m_fontSmall,
                       iconSize, gap) +
        30;
  } else if (m_currentState == UIState::SEARCH) {
    if (!m_searchVk.inResults) {
      x = drawFooterHint("A", "Nhập", x, barY, barH, fg, m_fontSmall, iconSize,
                         gap) +
          30;
      x = drawFooterHint("B", "Lùi", x, barY, barH, fg, m_fontSmall, iconSize,
                         gap) +
          30;
      x = drawFooterHint("Y", "Xóa", x, barY, barH, fg, m_fontSmall, iconSize,
                         gap) +
          30;
      x = drawFooterHint("X", "Xóa hết", x, barY, barH, fg, m_fontSmall,
                         iconSize, gap) +
          30;
      x = drawFooterHint("START", "Tìm", x, barY, barH, cyan, m_fontSmall,
                         iconSize, gap) +
          30;
    } else {
      x = drawFooterHint("A", "Tải / Chơi", x, barY, barH, fg, m_fontSmall,
                         iconSize, gap) +
          30;
      x = drawFooterHint("B", "Lùi", x, barY, barH, fg, m_fontSmall, iconSize,
                         gap) +
          30;
      x = drawFooterHint("X", "Xóa", x, barY, barH, red, m_fontSmall, iconSize,
                         gap) +
          30;
      x = drawFooterHint("START", "Bàn phím", x, barY, barH, cyan, m_fontSmall,
                         iconSize, gap) +
          30;
    }
  } else if (m_currentState == UIState::CONFIRM_DELETE ||
             m_currentState == UIState::CONFIRM_BATCH_DELETE) {
    x = drawFooterHint("A", "Xóa", x, barY, barH, red, m_fontSmall, iconSize,
                       gap) +
        30;
    x = drawFooterHint("B", "Hủy", x, barY, barH, fg, m_fontSmall, iconSize,
                       gap) +
        30;
  } else if (m_currentState == UIState::DISCLAIMER) {
    x = drawFooterHint("A", "Đồng ý", x, barY, barH, cyan, m_fontSmall,
                       iconSize, gap) +
        30;
    x = drawFooterHint("B", "Từ chối", x, barY, barH, fg, m_fontSmall, iconSize,
                       gap) +
        30;
  } else if (m_currentState == UIState::CLOUD_LOGIN) {
    x = drawFooterHint("B", "Lùi", x, barY, barH, fg, m_fontSmall, iconSize,
                       gap) +
        30;
  } else if (m_currentState == UIState::REVERSE_SYNC) {
    auto prog = UploadManager::instance().getProgress();
    if (prog.state == UploadState::UPLOADING ||
        prog.state == UploadState::PREPARING) {
      x = drawFooterHint("B", "Hủy", x, barY, barH, red, m_fontSmall, iconSize,
                         gap) +
          30;
    } else {
      x = drawFooterHint("A", "Đóng", x, barY, barH, cyan, m_fontSmall,
                         iconSize, gap) +
          30;
      x = drawFooterHint("B", "Lùi", x, barY, barH, fg, m_fontSmall, iconSize,
                         gap) +
          30;
    }
  } else if (m_currentState == UIState::SYSTEM_SELECT) {
    x = drawFooterHint("A", "Vào", x, barY, barH, fg, m_fontSmall, iconSize,
                       gap) +
        30;
    x = drawFooterHint("B", "Menu", x, barY, barH, fg, m_fontSmall, iconSize,
                       gap) +
        30;
    x = drawFooterHint("Y", "Đồng bộ", x, barY, barH, fg, m_fontSmall, iconSize,
                       gap) +
        30;
    // L1 + R1 cùng là chuyển trang
    {
      int centerY = barY + barH / 2;
      drawButtonIcon("L1", x, centerY - iconSize / 2, iconSize);
      x += iconSize + 4;
      drawButtonIcon("R1", x, centerY - iconSize / 2, iconSize);
      x += iconSize + gap;
      int th = textHeight(m_fontSmall);
      drawText("Trang", x, centerY - th / 2, fg, m_fontSmall);
      x += textWidth("Trang", m_fontSmall) + 30;
    }
  } else {
    x = drawFooterHint("A", "Chọn", x, barY, barH, fg, m_fontSmall, iconSize,
                       gap) +
        30;
    x = drawFooterHint("B", "Lùi", x, barY, barH, fg, m_fontSmall, iconSize,
                       gap) +
        30;
  }

  std::string verText = "v" + std::string(APP_VERSION) + " Native";
  drawTextRight(verText, 1012, barY + (barH - textHeight(m_fontSmall)) / 2,
                UiTheme::TEXT_FAINT, m_fontSmall);
}

void UIManager::renderToast() { m_dialogs.renderToast(m_ui, m_fontSmall); }

void UIManager::renderMenuState() {
  // OTA update info
  bool hasUpdate = UpdateManager::instance().isUpdateAvailable();

  int itemCount = static_cast<int>(m_gridMenuItems.size());

  // Lưới 5x2 vừa 1 màn hình: 10 icon hiện hết, không carousel/pager.
  const int COLS = 5;
  const int gx0 = 24, gapX = 16;
  const int cellW = (1024 - gx0 * 2 - gapX * (COLS - 1)) / COLS; // 182
  const int gy0 = 140, gapY = 20;
  const int cellH = 250;

  // Render từng ô theo hàng/cột, không phụ thuộc vị trí chọn.
  for (int i = 0; i < itemCount; ++i) {
    bool isSel = (i == m_selectedMenuIndex);
    int x = gx0 + (i % COLS) * (cellW + gapX);
    int y = gy0 + (i / COLS) * (cellH + gapY);
    int w = cellW;
    int h = cellH;

    // Card background
    SDL_Color bg =
        isSel ? SDL_Color{26, 52, 88, 255} : SDL_Color{20, 26, 36, 230};
    drawRoundedRect(x, y, w, h, UiTheme::RADIUS_MODAL, bg, true);

    if (isSel) {
      // Glowing cyan border on selected card
      drawRoundedBorder(x, y, w, h, UiTheme::RADIUS_MODAL, {0, 180, 216, 255},
                        3);
    } else {
      drawRoundedBorder(x, y, w, h, UiTheme::RADIUS_MODAL, {38, 48, 64, 255},
                        1);
    }

    // Icon inside card (enlarged logo size)
    int iconSize = isSel ? 132 : 112;
    int iconX = x + (w - iconSize) / 2;
    int iconY = y + 26;
    drawGridIcon(m_gridMenuItems[i].iconFile, iconX, iconY, iconSize, iconSize);

    // Card Title (Logo + Tên chức năng duy nhất)
    int textY = y + 196;
    SDL_Color titleColor =
        isSel ? SDL_Color{255, 255, 255, 255} : SDL_Color{160, 175, 195, 255};
    drawText(m_gridMenuItems[i].title, x + w / 2, textY, titleColor,
             isSel ? m_fontMedium : m_fontSmall, true);

    // OTA badge — dán lên icon Cài đặt (đã gộp OTA thành tab 1 của Settings)
    if (hasUpdate && m_gridMenuItems[i].id == "settings") {
      // Hiển thị "v1.2.3" nếu có, fallback "NEW" nếu version trống.
      auto updInfo = UpdateManager::instance().getLatestInfo();
      std::string verText = updInfo.remoteVersion.empty()
                                ? std::string("NEW")
                                : ("v" + updInfo.remoteVersion);
      int bw = badgeWidth(verText, 24);
      int bx = x + w - 12 - bw;
      drawRoundedRect(bx, y + 10, bw, 24, UiTheme::RADIUS_ROW,
                      {239, 68, 68, 255}, true);
      drawText(verText, bx + bw / 2, textYCentered(y + 10, 24, m_fontSmall),
               {255, 255, 255, 255}, m_fontSmall, true);
    }
  }

  drawAppFooter({{UiTheme::PadBtn::A, "Mở"},
                 {UiTheme::PadBtn::DPAD, "Chuyển"},
                 {UiTheme::PadBtn::SELECT, "Thông tin"},
                 {UiTheme::PadBtn::START, "Cài đặt"},
                 {UiTheme::PadBtn::B, "Thoát"}});
}

void UIManager::renderSystemSelectState() {
  int totalLocal = 0, totalCloud = 0;
  DatabaseManager::instance().getTotalGameCounts(totalLocal, totalCloud);

  // Header 1 hàng: CHỌN HỆ MÁY - 24 Hệ máy | 2 Thẻ | ... (không 2 hàng)
  std::string counts =
      std::to_string(m_cachedSystems.size()) +
      UiStrings::SYS_SELECT_SYSTEMS_LABEL + std::to_string(totalLocal) +
      UiStrings::SYS_SELECT_GAMES_LOCAL + std::to_string(totalCloud) +
      UiStrings::SYS_SELECT_GAMES_CLOUD;
  std::string title = UiStrings::SYSTEM_SELECT_TITLE;
  drawRect(0, 0, UiTheme::APP_W, UiTheme::HEADER_H, UiTheme::FOOTER_BG, true);
  drawRect(0, UiTheme::HEADER_H - 1, UiTheme::APP_W, 1, UiTheme::FOOTER_LINE,
           true);
  drawHeaderStatus();
  int hcy = textYCentered(0, UiTheme::HEADER_H, m_fontLarge);
  drawText(title, 24, hcy, UiTheme::ACCENT_CYAN, m_fontLarge);
  int htx = 24 + textWidth(title, m_fontLarge);
  std::string rest = truncateToWidth(std::string(" - ") + counts, m_fontLarge,
                                     UiTheme::APP_W - htx - 300);
  drawText(rest, htx, hcy, UiTheme::TEXT_DIM, m_fontLarge);

  int visibleCount = 6;
  int startIdx = 0;
  if (m_selectedSystemIndex >= visibleCount) {
    startIdx = m_selectedSystemIndex - visibleCount + 1;
  }

  int rowY = 76;
  int rowH = 88;
  int rowW = 976;
  int rx = 24;

  for (int i = startIdx; i < static_cast<int>(m_cachedSystems.size()) &&
                         (i - startIdx) < visibleCount;
       ++i) {
    const auto &sys = m_cachedSystems[i];
    bool selected = (i == m_selectedSystemIndex);
    int y = rowY + (i - startIdx) * (rowH + 10);

    SDL_Color bg = selected ? UiTheme::FOCUS_BG : UiTheme::ROW_BG;
    drawRoundedRect(rx, y, rowW, rowH, UiTheme::RADIUS_ROW, bg, true);

    if (selected) {
      drawRoundedBorder(rx, y, rowW, rowH, UiTheme::RADIUS_ROW,
                        UiTheme::ACCENT_CYAN, 2);
      // Left neon accent
      drawRoundedRect(rx + 6, y + 12, 5, rowH - 24, 2, UiTheme::ACCENT_CYAN,
                      true);
    }

    // System icon
    int iconSize = 64;
    drawIcon(sys.code, rx + 16, y + (rowH - iconSize) / 2, iconSize, iconSize);

    std::string sysName = truncateToWidth(sys.name, m_fontLarge, rowW - 320);
    std::string subtext = truncateToWidth(std::string("/Roms/") + sys.romDir +
                                              "  •  " + sys.extList,
                                          m_fontSmall, rowW - 320);
    drawRowMainSub(rx + 96, y, rowH, sysName, m_fontLarge, subtext,
                   m_fontSmall);

    std::string localBadge = std::to_string(sys.localCount) + " local";
    std::string cloudBadge = std::to_string(sys.cloudCount) + " cloud";

    // Count badges (elastic, right edge pinned with 10px margin)
    int cloudW = badgeWidth(cloudBadge, 40);
    int cloudX = rx + rowW - 10 - cloudW;
    int localW = badgeWidth(localBadge, 40);
    int localX = cloudX - 10 - localW;
    drawBadge(localX, y + (rowH - 40) / 2, localW, 40, localBadge,
              UiTheme::PILL_BG, UiTheme::TEXT_MAIN);
    drawBadge(cloudX, y + (rowH - 40) / 2, cloudW, 40, cloudBadge,
              UiTheme::FOCUS_BG, UiTheme::TEXT_MAIN);
  }
}

void UIManager::renderGameListState() {
  // Header 1 hang: TEN HE MAY - N game
  std::string gTitle =
      m_activeSystem.name.empty() ? "DANH SACH GAME" : m_activeSystem.name;
  std::string gCounts =
      std::string(" - ") + std::to_string(m_cachedGames.size()) + " game";
  drawRect(0, 0, UiTheme::APP_W, UiTheme::HEADER_H, UiTheme::FOOTER_BG, true);
  drawRect(0, UiTheme::HEADER_H - 1, UiTheme::APP_W, 1, UiTheme::FOOTER_LINE,
           true);
  drawHeaderStatus();
  int ghcy = textYCentered(0, UiTheme::HEADER_H, m_fontLarge);
  drawText(gTitle, 24, ghcy, UiTheme::ACCENT_CYAN, m_fontLarge);
  int ghtx = 24 + textWidth(gTitle, m_fontLarge);
  std::string gRest =
      truncateToWidth(gCounts, m_fontLarge, UiTheme::APP_W - ghtx - 300);
  drawText(gRest, ghtx, ghcy, UiTheme::TEXT_DIM, m_fontLarge);

  // Layout A 65/35: list 666px + detail 300px (gap + divider)
  int listY = 76;
  int listH = 627;
  int listW = UiTheme::LIST_W;
  int listX = 16;

  int detailX = UiTheme::DETAIL_X;
  int detailY = 76;
  int detailW = UiTheme::DETAIL_W;
  int detailH = 627;

  // Subtle 1px vertical divider between panes
  drawRect(listX + listW + 8, 76, 1, 627, {38, 48, 64, 255}, true);

  // 1. Render Left Games List
  int totalGames = static_cast<int>(m_cachedGames.size());
  int pageSize = 6;
  int rowH = 92;
  int rowW = listW - 30;
  int rowX = listX;
  int listStartY = listY + 12;

  if (totalGames == 0) {
    drawText(UiStrings::GAME_LIST_EMPTY, listX + listW / 2, listY + 280,
             {140, 150, 165, 255}, m_fontLarge, true);
    drawInlineHintsCentered(UiStrings::GAME_FILTER_HINT, listX + listW / 2,
                            listY + 325, {0, 180, 216, 255}, m_fontSmall, 24);
  } else {
    for (int i = 0; i < pageSize && (m_gameScrollOffset + i) < totalGames;
         ++i) {
      int gameIdx = m_gameScrollOffset + i;
      const auto &game = m_cachedGames[gameIdx];
      bool selected = (gameIdx == m_selectedGameIndex);
      int y = listStartY + i * (rowH + 12);

      SDL_Color bg = selected ? UiTheme::FOCUS_BG : UiTheme::ROW_BG;
      if (selected)
        drawFocusRow(rowX, y, rowW, rowH);
      else
        drawRoundedRect(rowX, y, rowW, rowH, UiTheme::RADIUS_ROW, bg, true);

      // State Pill Badge (compact: h 20, pad 6 moi ben, sat mep trai)
      bool isThisDownloading =
          DownloadManager::instance().isDownloading() &&
          DownloadManager::instance().getProgress().gameId == game.id;
      auto dlp = DownloadManager::instance().getProgress();
      int badgeH = 20;
      int badgeY = y + (rowH - badgeH) / 2;
      int badgeX = rowX + 6;
      std::string badgeTxt;
      SDL_Color badgeBg{22, 101, 52, 255};

      if (game.localState == GameState::LOCAL) {
        badgeTxt = UiStrings::BADGE_DOWNLOADED;
        badgeBg = {22, 101, 52, 255};
      } else if (isThisDownloading) {
        char pctBuf[16];
        std::snprintf(pctBuf, sizeof(pctBuf), "%.0f%%", dlp.progressPct);
        badgeTxt = std::string(pctBuf);
        badgeBg = {2, 132, 199, 255};
      } else if (DownloadManager::instance().isInQueue(game.id)) {
        auto q = DownloadManager::instance().getQueue();
        int pos = 1;
        for (const auto &qi : q) {
          if (qi.game.id == game.id)
            break;
          pos++;
        }
        badgeTxt = "#" + std::to_string(pos);
        badgeBg = {107, 33, 168, 255};
      } else if (game.localState == GameState::CLOUD) {
        badgeTxt = "CLOUD";
        badgeBg = UiTheme::FOCUS_BG;
      } else {
        badgeTxt = UiStrings::BTN_SYNC;
        badgeBg = {217, 119, 6, 255};
      }
      // Pill compact tu ve (pad 7 moi ben) de giam ca cao + rong
      int btw = m_fontSmall ? textWidth(badgeTxt, m_fontSmall) : 60;
      int bw = btw + 14;
      if (bw < 40)
        bw = 40;
      drawRoundedRect(badgeX, badgeY, bw, badgeH, badgeH / 2, badgeBg, true);
      drawText(badgeTxt, badgeX + bw / 2,
               textYCentered(badgeY, badgeH, m_fontSmall), UiTheme::TEXT_MAIN,
               m_fontSmall, true);
      // Ten game cach pill 1 space (~12px), khong dinh nhau
      int titleX = badgeX + bw + 12;

      // Title: marquee chay ngang khi highlight + ten dai
      // titleX da cach pill 1 space, maxW do theo mep phai row
      int titleMaxW = rowX + rowW - 16 - titleX;
      if (titleMaxW < 120)
        titleMaxW = 120;
      std::string titleFull = game.title;
      std::string title;
      if (selected && m_fontLarge &&
          textWidth(titleFull, m_fontLarge) > titleMaxW) {
        // Tach UTF-8 thanh codepoint de scroll khong vo chu
        std::vector<std::string> cps;
        for (size_t k = 0; k < titleFull.size();) {
          unsigned char c = titleFull[k];
          size_t len = 1;
          if ((c & 0x80) == 0x00)
            len = 1;
          else if ((c & 0xE0) == 0xC0)
            len = 2;
          else if ((c & 0xF0) == 0xE0)
            len = 3;
          else if ((c & 0xF8) == 0xF0)
            len = 4;
          if (k + len > titleFull.size())
            len = titleFull.size() - k;
          cps.push_back(titleFull.substr(k, len));
          k += len;
        }
        static int s_marqueeSel = -999;
        static uint32_t s_marqueeT0 = 0;
        uint32_t now = SDL_GetTicks();
        if (s_marqueeSel != gameIdx) {
          s_marqueeSel = gameIdx;
          s_marqueeT0 = now;
        }
        uint32_t el = now - s_marqueeT0;
        std::string cand = titleFull;
        if (el > 1200 && !cps.empty()) {
          size_t steps = cps.size() + 3;
          size_t off = ((el - 1200) / 350) % steps;
          if (off < cps.size()) {
            std::string rot;
            for (size_t k = off; k < cps.size(); k++)
              rot += cps[k];
            rot += "   ";
            for (size_t k = 0; k < off && k < cps.size(); k++)
              rot += cps[k];
            cand = rot;
          } else {
            cand = titleFull;
          }
        }
        title = truncateToWidth(cand, m_fontLarge, titleMaxW);
      } else {
        title = truncateToWidth(titleFull, m_fontLarge, titleMaxW);
      }
      SDL_Color titleCol = isThisDownloading ? UiTheme::ACCENT_CYAN
                                             : (selected ? UiTheme::TEXT_MAIN
                                                         : UiTheme::TEXT_DIM);

      if (isThisDownloading) {
        drawText(title, titleX, textYCentered(y, 56, m_fontLarge), titleCol,
                 m_fontLarge);

        char pctBuf[16];
        std::snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", dlp.progressPct);
        std::string dlSub =
            FileSystemManager::instance().formatBytes(dlp.bytesDownloaded) +
            " / " + FileSystemManager::instance().formatBytes(dlp.totalBytes) +
            "  (" + pctBuf + ")";
        dlSub = truncateToWidth(dlSub, m_fontSmall, titleMaxW);
        drawText(dlSub, titleX, y + 44, {140, 205, 245, 255}, m_fontSmall);

        // Live in-row rounded progress bar
        int pBarX = titleX;
        int pBarY = y + 70;
        int pBarW = rowX + rowW - 16 - titleX;
        int pBarH = 6;
        drawRoundedRect(pBarX, pBarY, pBarW, pBarH, 3, {35, 45, 60, 255}, true);
        float pct = std::max(0.0, std::min(100.0, dlp.progressPct));
        drawRoundedRect(pBarX, pBarY, (int)(pBarW * (pct / 100.0)), pBarH, 3,
                        {34, 197, 94, 255}, true);
      } else {
        std::string sizeStr =
            FileSystemManager::instance().formatBytes(game.sizeBytes);
        std::string sub = truncateToWidth(game.filename + "  (" + sizeStr + ")",
                                          m_fontSmall, titleMaxW);
        // Cum main/sub can giua doc trong row 92px (fix lech tam)
        int thM = m_fontLarge ? TTF_FontHeight(m_fontLarge) : 24;
        int thS = m_fontSmall ? TTF_FontHeight(m_fontSmall) : 16;
        int blockH = thM + 4 + thS;
        int ty = y + (rowH - blockH) / 2;
        drawText(title, titleX, ty, titleCol, m_fontLarge);
        drawText(sub, titleX, ty + thM + 4, UiTheme::TEXT_SUB, m_fontSmall);
      }

      // Multi-select checkbox
      if (m_multiSelectMode) {
        auto it = std::find(m_selectedGameIds.begin(), m_selectedGameIds.end(),
                            game.id);
        bool isSelected = (it != m_selectedGameIds.end());

        // Checkbox background
        SDL_Color cbBg = isSelected ? SDL_Color{34, 197, 94, 255}
                                    : SDL_Color{50, 60, 75, 255};
        drawRoundedRect(rowX + rowW - 40, y + 35, 24, 24, 4, cbBg, true);

        // Checkmark
        if (isSelected) {
          drawText("✓", rowX + rowW - 40 + 3, y + 33, {255, 255, 255, 255},
                   m_fontMedium);
        }
      }
    }

    // Multi-select mode header badge
    if (m_multiSelectMode && !m_selectedGameIds.empty()) {
      std::string countText =
          std::to_string(m_selectedGameIds.size()) + " đã chọn";
      int badgeW = badgeWidth(countText, 36);
      int badgeX = 512 - badgeW / 2;
      int badgeH = 36;
      int badgeY = 8;
      drawBadge(badgeX, badgeY, badgeW, badgeH, countText, {107, 33, 168, 255},
                {255, 255, 255, 255});
    }

    // Low storage warning banner
    auto dlProg = DownloadManager::instance().getProgress();
    if (dlProg.storageWarning) {
      float freePct =
          (dlProg.storageTotal > 0)
              ? (float)dlProg.storageAvailable * 100.0f / dlProg.storageTotal
              : 0;
      std::string warnText = "⚠ Cảnh báo: Thẻ nhớ sắp đầy (" +
                             std::to_string((int)freePct) + "% trống)";
      int warnW = badgeWidth(warnText, 36);
      int warnH = 36;
      int warnX = (1024 - warnW) / 2;
      int warnY = 720;
      drawBadge(warnX, warnY, warnW, warnH, warnText, UiTheme::ACCENT_RED,
                UiTheme::TEXT_MAIN);
    }

    // Scrollbar (sat mep phai cot trai, khong de len row)
    if (totalGames > pageSize) {
      int barX = rowX + rowW + 8;
      int barTrackH = listH - 24;
      drawRoundedRect(barX, listY + 12, 4, barTrackH, 2, {35, 42, 54, 255},
                      true);

      float ratio = (float)pageSize / (float)totalGames;
      int thumbH = std::max(24, (int)(barTrackH * ratio));
      float scrollRatio =
          (float)m_gameScrollOffset / (float)(totalGames - pageSize);
      int thumbY = listY + 12 + (int)((barTrackH - thumbH) * scrollRatio);
      drawRoundedRect(barX, thumbY, 4, thumbH, 2, {0, 180, 216, 255}, true);
    }
  }

  // 2. Render Right Details & Cover Panel (Borderless)
  const GameRecord *selGame =
      (totalGames > 0 && m_selectedGameIndex < totalGames)
          ? &m_cachedGames[m_selectedGameIndex]
          : nullptr;

  // Cover Art Box (vừa khít panel DETAIL_W=300, không tràn viền)
  int coverBoxW = detailW - 16;
  int coverBoxH = 290;
  int coverBoxX = detailX + (detailW - coverBoxW) / 2;
  int coverBoxY = detailY + 14;

  // Rounded backdrop for cover art container
  drawRoundedRect(coverBoxX - 4, coverBoxY - 4, coverBoxW + 8, coverBoxH + 8,
                  UiTheme::RADIUS_MODAL, {16, 20, 28, 255}, true);
  drawRoundedBorder(coverBoxX - 4, coverBoxY - 4, coverBoxW + 8, coverBoxH + 8,
                    UiTheme::RADIUS_MODAL, {38, 48, 64, 255}, 1);

  CoverManager::instance().renderCoverBox(coverBoxX, coverBoxY, coverBoxW,
                                          coverBoxH, selGame, &m_activeSystem,
                                          m_fontMedium);

  // Detail Metadata (cant le trai, truncate theo pixel de khong tran vien)
  if (selGame) {
    int metaX = detailX + 16;
    int valX = detailX + 108;
    int valMaxW = detailX + detailW - 16 - valX;
    int metaY = coverBoxY + coverBoxH + 18;

    std::string title =
        truncateToWidth(selGame->title, m_fontLarge, detailW - 48);
    drawText(title, metaX, metaY, {255, 255, 255, 255}, m_fontLarge);

    metaY += 44;
    drawText(UiStrings::DETAIL_SYS_LABEL, metaX, metaY, {140, 155, 175, 255},
             m_fontSmall);
    std::string sysVal =
        truncateToWidth(m_activeSystem.name + " (" + m_activeSystem.code + ")",
                        m_fontSmall, valMaxW);
    drawText(sysVal, valX, metaY, {0, 180, 216, 255}, m_fontSmall);

    metaY += 34;
    drawText(UiStrings::DETAIL_SIZE_LABEL, metaX, metaY, {140, 155, 175, 255},
             m_fontSmall);
    std::string sizeVal = truncateToWidth(
        FileSystemManager::instance().formatBytes(selGame->sizeBytes),
        m_fontSmall, valMaxW);
    drawText(sizeVal, valX, metaY, {255, 255, 255, 255}, m_fontSmall);

    metaY += 34;
    drawText(UiStrings::DETAIL_LOCATION_LABEL, metaX, metaY,
             {140, 155, 175, 255}, m_fontSmall);
    if (selGame->localState == GameState::LOCAL) {
      std::string locVal =
          truncateToWidth(std::string(UiStrings::GAME_LOCATION_SD_PREFIX) +
                              m_activeSystem.romDir + ")",
                          m_fontSmall, valMaxW);
      drawText(locVal, valX, metaY, {34, 197, 94, 255}, m_fontSmall);
    } else {
      std::string locVal =
          truncateToWidth(UiStrings::GAME_LOCATION_DRIVE, m_fontSmall, valMaxW);
      drawText(locVal, valX, metaY, {0, 180, 216, 255}, m_fontSmall);
    }

    // Action Status Pill & Live Progress
    metaY += 42;
    if (selGame->localState == GameState::LOCAL) {
      int halfW = (detailW - 40) / 2;
      drawBadge(metaX, metaY, halfW, 46, UiStrings::BADGE_DOWNLOADED,
                UiTheme::ACCENT_GREEN, UiTheme::TEXT_MAIN);
      drawBadge(metaX + 8 + halfW, metaY, halfW, 46,
                UiStrings::BADGE_DELETE_BTN, UiTheme::ACCENT_RED,
                UiTheme::TEXT_MAIN);
    } else if (DownloadManager::instance().isDownloading() &&
               DownloadManager::instance().getProgress().gameId ==
                   selGame->id) {
      auto dlp = DownloadManager::instance().getProgress();
      char pctBuf[32];
      std::snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", dlp.progressPct);
      std::string dlInfo =
          std::string(UiStrings::GAME_DOWNLOADING_PREFIX) + std::string(pctBuf);
      drawBadge(metaX, metaY, detailW - 32, 40, dlInfo, {2, 132, 199, 255},
                {255, 255, 255, 255});

      int dBarX = metaX;
      int dBarY = metaY + 46;
      int dBarW = detailW - 32;
      int dBarH = 10;
      drawRoundedRect(dBarX, dBarY, dBarW, dBarH, 5, {35, 45, 60, 255}, true);
      float pct = std::max(0.0, std::min(100.0, dlp.progressPct));
      drawRoundedRect(dBarX, dBarY, (int)(dBarW * (pct / 100.0)), dBarH, 5,
                      {34, 197, 94, 255}, true);

      std::string dSizeStr =
          FileSystemManager::instance().formatBytes(dlp.bytesDownloaded) +
          " / " + FileSystemManager::instance().formatBytes(dlp.totalBytes);
      drawText(dSizeStr, detailX + detailW / 2, dBarY + 16,
               {200, 220, 240, 255}, m_fontSmall, true);

      drawBadge(metaX, dBarY + 38, detailW - 32, 36,
                UiStrings::BADGE_CANCEL_DL_BTN, UiTheme::ACCENT_RED,
                UiTheme::TEXT_MAIN);
      metaY += 56;
    } else if (DownloadManager::instance().isInQueue(selGame->id)) {
      drawBadge(metaX, metaY, detailW - 32, 46,
                UiStrings::BADGE_REMOVE_QUEUE_BTN, {107, 33, 168, 255},
                {255, 255, 255, 255});
    } else {
      drawBadge(metaX, metaY, detailW - 32, 46, UiStrings::BADGE_ADD_QUEUE_BTN,
                {2, 132, 199, 255}, {255, 255, 255, 255});
    }

    // Queue info panel below action pill
    int queueCount = DownloadManager::instance().queueSize();
    bool isCurrentlyDownloading = DownloadManager::instance().isDownloading();
    if (queueCount > 0 || isCurrentlyDownloading) {
      metaY += 54;
      std::string qInfo;
      if (isCurrentlyDownloading && queueCount > 0) {
        qInfo = std::string(UiStrings::GAME_QUEUE_DOWNLOADING) +
                std::to_string(queueCount) + UiStrings::GAME_QUEUE_REMAINING;
      } else if (isCurrentlyDownloading) {
        qInfo = UiStrings::QUEUE_DOWNLOADING_EMPTY;
      } else {
        qInfo = std::string(UiStrings::GAME_QUEUE_WAITING) +
                std::to_string(queueCount) + UiStrings::GAME_QUEUE_REMAINING;
      }
      drawText(qInfo, metaX, metaY, {168, 85, 247, 255}, m_fontSmall);
    }
  }

  // Active download indicator banner at bottom right if user is browsing
  // another game
  if (DownloadManager::instance().isDownloading() &&
      (!selGame ||
       DownloadManager::instance().getProgress().gameId != selGame->id)) {
    auto activeProg = DownloadManager::instance().getProgress();
    int bY = detailY + detailH - 74;
    drawRoundedRect(detailX + 24, bY, detailW - 48, 62, UiTheme::RADIUS_ROW,
                    {18, 28, 44, 255}, true);
    drawRoundedBorder(detailX + 24, bY, detailW - 48, 62, UiTheme::RADIUS_ROW,
                      {0, 180, 216, 255}, 1);

    char pBuf[16];
    std::snprintf(pBuf, sizeof(pBuf), "%.0f%%", activeProg.progressPct);
    std::string tTrunc =
        truncateToWidth(activeProg.gameTitle, m_fontSmall, 180);
    drawText("v " + tTrunc, detailX + 36, bY + 8, {0, 180, 216, 255},
             m_fontSmall);
    drawText(std::string(pBuf), detailX + detailW - 75, bY + 8,
             {34, 197, 94, 255}, m_fontSmall);

    int aBarW = detailW - 72;
    int aBarH = 6;
    drawRoundedRect(detailX + 36, bY + 38, aBarW, aBarH, 3, {30, 40, 55, 255},
                    true);
    float aPct = std::max(0.0, std::min(100.0, activeProg.progressPct));
    drawRoundedRect(detailX + 36, bY + 38, (int)(aBarW * (aPct / 100.0)), aBarH,
                    3, {34, 197, 94, 255}, true);
  }
}

void UIManager::openConfirmDeleteDialog() {
  if (m_selectedGameIndex < 0 ||
      m_selectedGameIndex >= static_cast<int>(m_cachedGames.size()))
    return;
  int64_t gameId = m_cachedGames[m_selectedGameIndex].id;
  std::string gameTitle = m_cachedGames[m_selectedGameIndex].title;
  std::string fileLine = std::string("File: ") +
                         m_cachedGames[m_selectedGameIndex].filename + " (" +
                         FileSystemManager::instance().formatBytes(
                             m_cachedGames[m_selectedGameIndex].sizeBytes) +
                         ")";
  m_dialogs.confirm.open(
      std::string(UiStrings::DIALOG_DELETE_TITLE),
      {gameTitle, fileLine, std::string(UiStrings::DIALOG_DELETE_PROMPT)},
      [this, gameId, gameTitle]() {
        DatabaseManager::instance().markGameDeletedLocally(gameId);
        refreshSystems();
        refreshGames();
        showToast("Đã xóa \"" + gameTitle + "\" khỏi thẻ nhớ.",
                  {239, 68, 68, 255}, 3000);
      },
      true);
}

void UIManager::openConfirmBatchDeleteDialog() {
  int selCount = static_cast<int>(m_selectedGameIds.size());
  if (selCount <= 0)
    return;
  uint64_t totalSize = 0;
  std::vector<std::string> lines;
  lines.push_back("Bạn muốn xóa " + std::to_string(selCount) +
                  " game khỏi thẻ nhớ?");
  for (int64_t gameId : m_selectedGameIds) {
    for (const auto &g : m_cachedGames) {
      if (g.id == gameId) {
        totalSize += g.sizeBytes;
        break;
      }
    }
  }
  lines.push_back("Tổng dung lượng: " +
                  FileSystemManager::instance().formatBytes(totalSize));
  int shown = 0;
  for (const auto &g : m_cachedGames) {
    if (shown >= 5)
      break;
    if (std::find(m_selectedGameIds.begin(), m_selectedGameIds.end(), g.id) !=
        m_selectedGameIds.end()) {
      std::string t = truncateToWidth(g.title, m_fontSmall, 320);
      lines.push_back("- " + t);
      shown++;
    }
  }
  std::vector<int64_t> ids = m_selectedGameIds;
  m_dialogs.confirm.open(
      std::string(UiStrings::MULTI_BATCH_DELETE_TITLE), std::move(lines),
      [this, ids]() {
        for (int64_t gameId : ids)
          DatabaseManager::instance().markGameDeletedLocally(gameId);
        refreshSystems();
        refreshGames();
        showToast("Đã xóa " + std::to_string(ids.size()) +
                      " game khỏi thẻ nhớ.",
                  {239, 68, 68, 255}, 4000);
        m_multiSelectMode = false;
        m_selectedGameIds.clear();
      },
      true);
}

void UIManager::renderConfirmDialogFromState() {
  m_dialogs.renderConfirm(m_ui, m_fontSmall, m_fontMedium, m_fontLarge);
}

void UIManager::renderConfirmDeleteDialog() { renderConfirmDialogFromState(); }

void UIManager::renderConfirmBatchDeleteDialog() {
  renderConfirmDialogFromState();
}

void UIManager::renderProgressDialogFromState() {
  m_dialogs.renderProgress(m_ui, m_fontSmall, m_fontLarge);
}
void UIManager::drawProgressBar(int x, int y, int w, int h, double frac,
                                SDL_Color fill, bool rounded, SDL_Color bg) {
  if (frac < 0)
    frac = 0;
  if (frac > 1)
    frac = 1;
  int radius = rounded ? UiTheme::RADIUS_ROW : 0;
  if (rounded) {
    drawRoundedRect(x, y, w, h, radius, bg, true);
  } else {
    drawRect(x, y, w, h, bg, true);
  }
  int fw = (int)(w * frac);
  if (fw <= 0)
    return;
  if (rounded) {
    drawRoundedRect(x, y, fw, h, radius, fill, true);
  } else {
    drawRect(x, y, fw, h, fill, true);
  }
}

void UIManager::renderDisclaimerState() {
  // Borderless disclaimer card - no outer border
  int cardX = 80;
  int cardY = 80;
  int cardW = 864;
  int cardH = 615;

  // Modal dialog bo tron chuan (title bar cong theo goc)
  drawModalDialog(cardX, cardY, cardW, cardH, UiTheme::RADIUS_MODAL,
                  {22, 27, 36, 255}, {50, 32, 12, 255}, 58);
  drawText(UiStrings::DISCLAIMER_TITLE, 512, cardY + 18, {245, 158, 11, 255},
           m_fontLarge, true);

  // Inner panel - subtle inset without border
  int innerX = cardX + 30;
  int innerY = cardY + 76;
  int innerW = cardW - 60;
  int innerH = 435;
  drawRoundedRect(innerX, innerY, innerW, innerH, UiTheme::RADIUS_CARD,
                  {16, 20, 28, 255}, true);

  int textX = innerX + 28;
  int textY = innerY + 22;

  drawText(UiStrings::DISCLAIMER_SUBTITLE, 512, textY, {255, 255, 255, 255},
           m_fontMedium, true);
  drawRect(innerX + 40, textY + 34, innerW - 80, 1, {60, 72, 90, 255}, true);

  textY += 52;
  drawText(UiStrings::DISCLAIMER_SEC1_TITLE, textX, textY, {0, 180, 216, 255},
           m_fontMedium);
  textY += 28;
  drawText(UiStrings::DISCLAIMER_SEC1_LINE1, textX + 15, textY,
           {200, 210, 225, 255}, m_fontSmall);
  textY += 25;
  drawText(UiStrings::DISCLAIMER_SEC1_LINE2, textX + 15, textY,
           {239, 68, 68, 255}, m_fontSmall);

  textY += 40;
  drawText(UiStrings::DISCLAIMER_SEC2_TITLE, textX, textY, {0, 180, 216, 255},
           m_fontMedium);
  textY += 28;
  drawText(UiStrings::DISCLAIMER_SEC2_LINE1, textX + 15, textY,
           {200, 210, 225, 255}, m_fontSmall);
  textY += 25;
  drawText(UiStrings::DISCLAIMER_SEC2_LINE2, textX + 15, textY,
           {200, 210, 225, 255}, m_fontSmall);

  textY += 40;
  drawText(UiStrings::DISCLAIMER_SEC3_TITLE, textX, textY, {245, 158, 11, 255},
           m_fontMedium);
  textY += 28;
  drawText(UiStrings::DISCLAIMER_SEC3_LINE1, textX + 15, textY,
           {253, 224, 71, 255}, m_fontSmall);
  textY += 25;
  drawText(UiStrings::DISCLAIMER_SEC3_LINE2, textX + 15, textY,
           {253, 224, 71, 255}, m_fontSmall);
  textY += 25;
  drawText(UiStrings::DISCLAIMER_SEC3_LINE3, textX + 15, textY,
           {253, 224, 71, 255}, m_fontSmall);

  // Action buttons at the bottom of card (elastic, pinned to card edges)
  int btnY = cardY + 530;
  int btnH = 54;
  int agreeW = badgeWidth(UiStrings::DISCLAIMER_AGREE, btnH);
  int declineW = badgeWidth(UiStrings::DISCLAIMER_DECLINE, btnH);
  int btn1X = cardX + 50;
  int btn2X = cardX + cardW - 50 - declineW;

  drawBadge(btn1X, btnY, agreeW, btnH, UiStrings::DISCLAIMER_AGREE,
            {22, 101, 52, 255}, {255, 255, 255, 255});
  drawBadge(btn2X, btnY, declineW, btnH, UiStrings::DISCLAIMER_DECLINE,
            {75, 85, 99, 255}, {255, 255, 255, 255});
}

// ─── Settings Helpers: Cache Cleaner & Wi-Fi Diagnostics ──────────────
static uint64_t getDirSize(const std::string &path) {
  uint64_t total = 0;
  DIR *d = opendir(path.c_str());
  if (!d)
    return 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != nullptr) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;
    std::string full = path + "/" + ent->d_name;
    struct stat st;
    if (stat(full.c_str(), &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        total += getDirSize(full);
      } else {
        total += st.st_size;
      }
    }
  }
  closedir(d);
  return total;
}

static uint64_t removeDirContents(const std::string &path) {
  uint64_t freed = 0;
  DIR *d = opendir(path.c_str());
  if (!d)
    return 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != nullptr) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;
    std::string full = path + "/" + ent->d_name;
    struct stat st;
    if (stat(full.c_str(), &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        freed += removeDirContents(full);
        rmdir(full.c_str());
      } else {
        freed += st.st_size;
        unlink(full.c_str());
      }
    }
  }
  closedir(d);
  return freed;
}

uint64_t UIManager::calculateCacheSizeBytes() {
  uint64_t total = 0;
  std::string cacheDir = AppConfig::instance().getCacheDir();
  total += getDirSize(cacheDir);
  total += getDirSize("/tmp/yt_thumbs");
  return total;
}

std::string UIManager::getCacheSizeFormatted() {
  uint64_t bytes = calculateCacheSizeBytes();
  if (bytes == 0)
    return "0 B (Đã tối ưu)";
  return LsUtil::humanSize(bytes);
}

uint64_t UIManager::cleanCache() {
  uint64_t freed = 0;
  std::string cacheDir = AppConfig::instance().getCacheDir();
  freed += removeDirContents(cacheDir + "/covers");
  freed += removeDirContents(cacheDir + "/metadata");
  freed += removeDirContents("/tmp/yt_thumbs");

  mkdir(cacheDir.c_str(), 0755);
  mkdir((cacheDir + "/covers").c_str(), 0755);
  mkdir((cacheDir + "/metadata").c_str(), 0755);
  mkdir("/tmp/yt_thumbs", 0755);

  m_ytThumbCache.clear();
  IPTVManager::instance().clearPingCache();

  m_cacheSizeFormatted = getCacheSizeFormatted();
  return freed;
}

void UIManager::runWifiDiagnostics() {
  if (m_wifiDiagRunning.load())
    return;
  m_wifiDiagRunning.store(true);
  m_wifiDiagStatus = "Đang kiểm tra ping...";
  m_wifiDiagColor = UiTheme::ACCENT_CYAN;

  std::thread([this]() {
    std::string ip = PlatformInfo::instance().getIpAddress("wlan0");
    if (ip.empty() || ip == "Disconnected") {
      m_wifiDiagStatus = "Không có kết nối Wi-Fi";
      m_wifiDiagColor = UiTheme::ACCENT_RED;
      m_wifiDiagRunning.store(false);
      return;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
      m_wifiDiagStatus = "Lỗi socket mạng";
      m_wifiDiagColor = UiTheme::ACCENT_RED;
      m_wifiDiagRunning.store(false);
      return;
    }

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 500000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);

    auto t0 = std::chrono::steady_clock::now();
    int res = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    auto t1 = std::chrono::steady_clock::now();
    close(sock);

    int rttMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

    if (res == 0 || (rttMs > 0 && rttMs < 1500)) {
      std::string quality;
      SDL_Color col;
      if (rttMs < 60) {
        quality = "Rất tốt";
        col = {74, 222, 128, 255};
      } else if (rttMs < 150) {
        quality = "Tốt";
        col = {0, 180, 255, 255};
      } else if (rttMs < 300) {
        quality = "Bình thường";
        col = {250, 204, 21, 255};
      } else {
        quality = "Chậm";
        col = {248, 113, 113, 255};
      }
      m_wifiDiagStatus =
          std::to_string(rttMs) + " ms (" + quality + ") • " + ip;
      m_wifiDiagColor = col;
    } else {
      m_wifiDiagStatus = "Mất kết nối Internet • " + ip;
      m_wifiDiagColor = UiTheme::ACCENT_RED;
    }
    m_wifiDiagRunning.store(false);
  }).detach();
}

void UIManager::renderSettingsState() {
  // ─── Background + Header chuẩn (giống IPTV/YouTube/Explorer/GameCast) ───
  // P4: bỏ custom sub-header strip cũ (Y=64..112 + line) để đồng bộ với
  // các app khác. drawAppHeader tự render: FOOTER_BG bar + separator line +
  // title (ACCENT_CYAN) + cum status Wi-Fi/pin/clock bên phải.
  drawAppBackground();
  drawAppHeader(UiStrings::HEADER_SETTINGS);

  // ─── Tab pills top-right (CẤU HÌNH | TÙY CHỌN | CẬP NHẬT | WI-FI | GIỚI THIỆU | HỆ THỐNG)
  // Tabs nằm trong vùng Y=72..104 (8px gap dưới header, 32px tall).
  {
    int tabH = 32;
    int tabY = 72;
    int wCfg = pillWidth(UiStrings::SETTINGS_TAB_CONFIG, m_fontSmall) + 32;
    int wSet = pillWidth(UiStrings::SETTINGS_TAB_PREFS, m_fontSmall) + 32;
    int wUpd = pillWidth(UiStrings::SETTINGS_TAB_UPDATE, m_fontSmall) + 32;
    int wWifi = pillWidth(UiStrings::SETTINGS_TAB_WIFI, m_fontSmall) + 32;
    int wAbout = pillWidth(UiStrings::INFO_TAB_ABOUT, m_fontSmall) + 32;
    int wSys = pillWidth(UiStrings::INFO_TAB_SYSTEM, m_fontSmall) + 32;
    int xSys = 1024 - 24 - wSys;
    int xAbout = xSys - 8 - wAbout;
    int xWifi = xAbout - 8 - wWifi;
    int xUpd = xWifi - 8 - wUpd;
    int xSet = xUpd - 8 - wSet;
    int xCfg = xSet - 8 - wCfg;
    drawPill(xCfg, tabY, wCfg, tabH, UiStrings::SETTINGS_TAB_CONFIG,
             m_settingsTab == 0, m_fontSmall);
    drawPill(xSet, tabY, wSet, tabH, UiStrings::SETTINGS_TAB_PREFS,
             m_settingsTab == 1, m_fontSmall);
    drawPill(xUpd, tabY, wUpd, tabH, UiStrings::SETTINGS_TAB_UPDATE,
             m_settingsTab == 2, m_fontSmall);
    drawPill(xWifi, tabY, wWifi, tabH, UiStrings::SETTINGS_TAB_WIFI,
             m_settingsTab == 3, m_fontSmall);
    drawPill(xAbout, tabY, wAbout, tabH, UiStrings::INFO_TAB_ABOUT,
             m_settingsTab == 4, m_fontSmall);
    drawPill(xSys, tabY, wSys, tabH, UiStrings::INFO_TAB_SYSTEM,
             m_settingsTab == 5, m_fontSmall);
  }

  // ─── Tab 2 (CẬP NHẬT): render OTA body (đã bỏ header/footer nội bộ) ────
  if (m_settingsTab == 2) {
    renderOTAUpdateState();
    auto prog = UpdateManager::instance().getProgress();
    // P6: footer với [Y] hint khi có update, hoặc khi đã mới nhất nhưng có
    // release notes để xem (sub-page changelog dùng chung).
    bool showYHint =
        prog.state == UpdateState::UPDATE_AVAILABLE ||
        (prog.state == UpdateState::UP_TO_DATE &&
         !UpdateManager::instance().getLatestInfo().changelog.empty());
    if (showYHint) {
      drawAppFooter({{UiTheme::PadBtn::A, "Tải"},
                     {UiTheme::PadBtn::Y, "Tính năng mới"},
                     {UiTheme::PadBtn::B, "Lùi"},
                     {UiTheme::PadBtn::L1R1, "Chuyển tab"}});
    } else {
      drawAppFooter({{UiTheme::PadBtn::A, "Đồng ý"},
                     {UiTheme::PadBtn::B, "Lùi"},
                     {UiTheme::PadBtn::L1R1, "Chuyển tab"}});
    }
    return;
  }

  // ─── Tab 3 (WI-FI): quét + nối mạng + portal ──────────────────────────
  if (m_settingsTab == 3) {
    renderWifiTab();
    return;
  }

  // ─── Tab 4/5 (GIỚI THIỆU / HỆ THỐNG, từ INFO) ─────────────────────────
  if (m_settingsTab == 4) {
    renderAboutTab(124);
    drawAppFooter({{UiTheme::PadBtn::B, "Lùi"},
                   {UiTheme::PadBtn::L1R1, "Chuyển tab"}});
    return;
  }
  if (m_settingsTab == 5) {
    renderSystemTab();
    drawAppFooter({{UiTheme::PadBtn::DPAD, "Cuộn"},
                   {UiTheme::PadBtn::B, "Lùi"},
                   {UiTheme::PadBtn::L1R1, "Chuyển tab"}});
    return;
  }

  struct SettingItem {
    std::string label;
    std::string value;
    SDL_Color valColor;
    std::string badgeText;
    SDL_Color badgeBg;
    SDL_Color badgeFg;
  };

  std::vector<SettingItem> items;

  if (m_settingsTab == 0) {
    // ════════════════════════════════════════════════════════════════════════
    // TAB 0: CẤU HÌNH (8 mục cấu hình hệ thống & cloud)
    // ════════════════════════════════════════════════════════════════════════
    std::string email = AuthManager::instance().getUserEmail();
    std::string folderId = DatabaseManager::instance().getSetting(
        "drive_folder_id", UiStrings::SETTING_NOT_CONFIGURED);
    std::string lastSync = DatabaseManager::instance().getSetting(
        "last_cloud_sync_time", UiStrings::SETTING_NEVER_SYNCED);
    std::string ip = PlatformInfo::instance().getIpAddress("wlan0");
    if (ip.empty() || ip == "Disconnected")
      ip = "192.168.1.164";

    int curWebPort = WebServer::instance().getPort();
    std::string webUrl = "http://" + ip + ":" + std::to_string(curWebPort);

    std::string castUrl = "http://" + ip + ":8090";
    bool castRunning = CastManager::instance().isRunning();

    items.reserve(9);

    // 0: Google Drive Account
    items.push_back(
        {UiStrings::SETTING_DRIVE_STATUS,
         AuthManager::instance().isLinked()
             ? (std::string(UiStrings::SETTING_CONNECTED) + " (" + email + ")")
             : std::string(UiStrings::SETTING_DISCONNECTED),
         AuthManager::instance().isLinked() ? SDL_Color{34, 197, 94, 255}
                                            : SDL_Color{239, 68, 68, 255},
         AuthManager::instance().isLinked()
             ? std::string(UiStrings::SETTING_LOGOUT_BTN)
             : std::string(UiStrings::SETTING_CONNECT_WEB_BTN),
         AuthManager::instance().isLinked() ? UiTheme::ACCENT_RED
                                            : UiTheme::FOCUS_BG,
         SDL_Color{255, 255, 255, 255}});

    // 1: Thư mục Drive
    items.push_back({UiStrings::SETTING_DRIVE_FOLDER, folderId,
                     SDL_Color{0, 180, 216, 255}, "", SDL_Color{0, 0, 0, 0},
                     SDL_Color{0, 0, 0, 0}});

    // 2: Phiên bản hệ điều hành (OS Type)
    std::string osName = AppConfig::instance().getOSName();
    items.push_back({"Phiên bản OS", osName, SDL_Color{168, 85, 247, 255},
                     "[A] Đổi", SDL_Color{88, 28, 135, 255},
                     SDL_Color{255, 255, 255, 255}});

    // 3: Thư mục ROM trên thẻ nhớ
    items.push_back({UiStrings::SETTING_ROM_SD_FOLDER,
                     AppConfig::instance().getRomsDir(),
                     SDL_Color{255, 255, 255, 255}, "", SDL_Color{0, 0, 0, 0},
                     SDL_Color{0, 0, 0, 0}});

    // 4: Đồng bộ cuối
    items.push_back({UiStrings::SETTING_LAST_SYNC, lastSync,
                     SDL_Color{255, 255, 255, 255}, "", SDL_Color{0, 0, 0, 0},
                     SDL_Color{0, 0, 0, 0}});

    // 5: Cơ sở dữ liệu SQLite
    items.push_back({UiStrings::SETTING_SQLITE_DB,
                     AppConfig::instance().getDatabasePath(),
                     SDL_Color{34, 197, 94, 255}, "", SDL_Color{0, 0, 0, 0},
                     SDL_Color{0, 0, 0, 0}});

    // 6: Chế độ quét thẻ nhớ
    items.push_back({UiStrings::SETTING_SCAN_MODE, UiStrings::SETTING_SCAN_AUTO,
                     SDL_Color{34, 197, 94, 255}, "", SDL_Color{0, 0, 0, 0},
                     SDL_Color{0, 0, 0, 0}});

    // 7: Cổng Web Portal
    items.push_back({"Cổng Web Portal", webUrl, SDL_Color{0, 180, 255, 255},
                     "[A] Đổi port", SDL_Color{14, 116, 144, 255},
                     SDL_Color{255, 255, 255, 255}});

    // 8: Cổng GameCast
    items.push_back(
        {"Cổng GameCast", castUrl,
         castRunning ? SDL_Color{34, 197, 94, 255}
                     : SDL_Color{148, 163, 184, 255},
         castRunning ? "[A] Tắt" : "[A] Bật",
         castRunning ? UiTheme::ACCENT_RED : SDL_Color{21, 94, 117, 255},
         SDL_Color{255, 255, 255, 255}});
  } else {
    // ════════════════════════════════════════════════════════════════════════
    // TAB 1: CÀI ĐẶT (5 mục tiện ích hệ thống & bảo trì)
    // ════════════════════════════════════════════════════════════════════════
    items.reserve(5);

    // 0: Dọn dẹp bộ nhớ đệm
    if (m_cacheSizeFormatted.empty())
      m_cacheSizeFormatted = getCacheSizeFormatted();
    items.push_back(
        {"Dọn dẹp bộ nhớ đệm", "Chiếm dụng: " + m_cacheSizeFormatted,
         SDL_Color{250, 204, 21, 255}, "[A] Dọn dẹp",
         SDL_Color{133, 77, 14, 255}, SDL_Color{255, 255, 255, 255}});

    // 1: Xuất sao lưu cài đặt
    items.push_back(
        {UiStrings::BACKUP_EXPORT_BTN, UiStrings::BACKUP_EXPORT_DESC,
         SDL_Color{168, 85, 247, 255}, "[A] Xuất sao lưu",
         SDL_Color{88, 28, 135, 255}, SDL_Color{255, 255, 255, 255}});

    // 2: Phục hồi cài đặt
    items.push_back({UiStrings::BACKUP_IMPORT_BTN,
                     UiStrings::BACKUP_IMPORT_DESC, SDL_Color{0, 180, 216, 255},
                     "[A] Phục hồi", SDL_Color{21, 94, 117, 255},
                     SDL_Color{255, 255, 255, 255}});

    // 3: Báo cáo lỗi tự động
    bool issueReportingEnabled = IssueLogger::instance().isEnabled();
    std::string deviceId = DeviceIdentity::instance().getDeviceId();
    items.push_back({UiStrings::DIAG_DEVICE_ID_LABEL, deviceId,
                     SDL_Color{34, 197, 94, 255},
                     issueReportingEnabled ? "[A] Tắt" : "[A] Auto",
                     issueReportingEnabled ? SDL_Color{34, 197, 94, 255}
                                           : SDL_Color{239, 68, 68, 255},
                     SDL_Color{255, 255, 255, 255}});

    // 4: Nút gửi lỗi thủ công
    items.push_back({"Báo lỗi", "Gửi lỗi", SDL_Color{239, 68, 68, 255},
                     "[A] Gửi lỗi", SDL_Color{185, 28, 28, 255},
                     SDL_Color{255, 255, 255, 255}});

    // 5: Kiểm tra mạng Wi-Fi
    items.push_back({"Kiểm tra mạng Wi-Fi", m_wifiDiagStatus, m_wifiDiagColor,
                     "[A] Kiểm tra", SDL_Color{21, 94, 117, 255},
                     SDL_Color{255, 255, 255, 255}});
  }

  int cardX = 24;
  int cardW = 976;
  int rowH = 50;
  int spacing = 6;
  int stepY = rowH + spacing;
  int visibleRows = 9;

  int maxScroll = static_cast<int>(items.size()) - visibleRows;
  if (maxScroll < 0)
    maxScroll = 0;

  // P4-P5: startY = 124 vì có tab pills ở Y=72..104 (32px tall, 8px gap dưới
  // header Y=0..64). Content list bắt đầu Y=124 (gap 20px dưới tabs), khớp
  // pattern với trang THÔNG TIN (contentTop=124). scroll clip range (70..680)
  // tự xử lý khi scroll lên/xuống.
  int startY = 124 - (m_settingsScrollOffset * stepY);

  for (size_t i = 0; i < items.size(); ++i) {
    int y = startY + static_cast<int>(i) * stepY;
    if (y < 70 || y > 680)
      continue;

    bool selected = (static_cast<int>(i) == m_selectedSettingsRow);

    SDL_Color bg;
    if (selected) {
      bg = SDL_Color{30, 58, 95, 255};
    } else if (i % 2 == 1) {
      bg = SDL_Color{22, 28, 38, 255};
    } else {
      bg = SDL_Color{16, 20, 28, 255};
    }

    drawRoundedRect(cardX, y, cardW, rowH, UiTheme::RADIUS_CARD, bg, true);

    if (selected) {
      drawRoundedBorder(cardX, y, cardW, rowH, UiTheme::RADIUS_CARD,
                        {0, 180, 216, 255}, 2);
      // Left neon accent indicator
      drawRoundedRect(cardX + 4, y + 10, 5, rowH - 20, 2, {0, 180, 216, 255},
                      true);
    }

    SDL_Color lblColor = selected ? SDL_Color{255, 255, 255, 255}
                                  : SDL_Color{170, 185, 200, 255};
    drawText(items[i].label, cardX + 24, y + 12, lblColor, m_fontMedium);

    if (!items[i].badgeText.empty()) {
      // Shortened value to leave room for badge
      std::string val = truncateToWidth(items[i].value, m_fontSmall, 460);
      drawText(val, cardX + 340, y + 15, items[i].valColor, m_fontSmall);
      int setBW = badgeWidth(items[i].badgeText, 38);
      drawBadge(cardX + cardW - 20 - setBW, y + 6, setBW, 38,
                items[i].badgeText, items[i].badgeBg, items[i].badgeFg);
    } else {
      std::string val = truncateToWidth(items[i].value, m_fontSmall, 600);
      drawText(val, cardX + 340, y + 15, items[i].valColor, m_fontSmall);
    }
  }

  // Scrollbar indicator
  if (maxScroll > 0) {
    int scrollBarX = 1006;
    int scrollBarY = 124; // P5: 80→124 khớp tab content top (Y=124 dưới tabs)
    int scrollBarH = visibleRows * stepY - spacing;
    int thumbH = scrollBarH * visibleRows / static_cast<int>(items.size());
    int thumbY = scrollBarY +
                 (m_settingsScrollOffset * (scrollBarH - thumbH) / maxScroll);

    drawRoundedRect(scrollBarX, scrollBarY, 6, scrollBarH, 3, {35, 42, 54, 255},
                    true);
    drawRoundedRect(scrollBarX, thumbY, 6, thumbH, 3, {0, 180, 216, 255}, true);
  }

  if (m_settingsTab == 0 && m_selectedSettingsRow == 0 &&
      AuthManager::instance().isLinked()) {
    drawAppFooter({{UiTheme::PadBtn::A, "Chọn"},
                   {UiTheme::PadBtn::B, "Lùi"},
                   {UiTheme::PadBtn::X, "Đăng xuất"},
                   {UiTheme::PadBtn::L1R1, "Chuyển tab"}});
  } else if (m_settingsTab == 1) {
    drawAppFooter({{UiTheme::PadBtn::A, "Thực hiện"},
                   {UiTheme::PadBtn::B, "Lùi"},
                   {UiTheme::PadBtn::L1R1, "Chuyển tab"}});
  } else {
    drawAppFooter({{UiTheme::PadBtn::A, "Chọn"},
                   {UiTheme::PadBtn::B, "Lùi"},
                   {UiTheme::PadBtn::L1R1, "Chuyển tab"}});
  }
}

void UIManager::renderCloudLoginState() {
  int cardX = 72;
  int cardY = 82;
  int cardW = 880;
  int cardH = 610;

  drawRect(cardX, cardY, cardW, cardH, {22, 27, 36, 255}, true);
  drawBorder(cardX, cardY, cardW, cardH, {0, 180, 216, 255}, 2);

  drawText(UiStrings::HEADER_WEB_CONNECT, 512, cardY + 22, {0, 180, 216, 255},
           m_fontTitle ? m_fontTitle : m_fontLarge, true);

  if (AuthManager::instance().isLinked()) {
    int boxW = 680;
    int boxH = 340;
    int boxX = (1024 - boxW) / 2;
    int boxY = (768 - boxH) / 2;

    drawModalDialog(boxX, boxY, boxW, boxH, UiTheme::RADIUS_MODAL,
                    {20, 25, 35, 255}, {22, 101, 52, 255}, 60);
    drawText(UiStrings::WEB_CONNECT_SUCCESS, boxX + boxW / 2, boxY + 16,
             {255, 255, 255, 255}, m_fontLarge, true);

    drawText(UiStrings::WEB_CONNECT_ACCOUNT_PREFIX, boxX + boxW / 2, boxY + 95,
             {150, 165, 180, 255}, m_fontMedium, true);
    drawText(AuthManager::instance().getUserEmail(), boxX + boxW / 2,
             boxY + 135, {0, 180, 216, 255}, m_fontLarge, true);
    drawText(UiStrings::WEB_CONNECT_READY, boxX + boxW / 2, boxY + 195,
             {200, 210, 220, 255}, m_fontMedium, true);

    drawBadge(boxX +
                  (boxW - badgeWidth(UiStrings::WEB_CONNECT_START_BTN, 52)) / 2,
              boxY + 250, 0, 52, UiStrings::WEB_CONNECT_START_BTN,
              {22, 101, 52, 255}, {255, 255, 255, 255});
  } else if (!m_cloudPortalView) {
    // Liên kết 1 chạm: quét QR hoặc nhập mã trên điện thoại là xong,
    // không cần dán token thủ công.
    AuthState st = AuthManager::instance().getState();
    DeviceCodeResponse info = AuthManager::instance().getDeviceCodeInfo();
    int innerX = cardX + 35;
    int innerY = cardY + 75;
    int innerW = cardW - 70;
    int innerH = 435;
    drawRect(innerX, innerY, innerW, innerH, {16, 20, 28, 255}, true);
    drawBorder(innerX, innerY, innerW, innerH, {45, 55, 72, 255}, 1);
    int dy = innerY + 18;
    drawText("B1: Quét mã QR hoặc vào trang sau trên điện thoại:", 512, dy,
             {200, 215, 230, 255}, m_fontSmall, true);
    if (st == AuthState::AWAITING_USER && !info.verificationUrl.empty()) {
      QrRenderer::renderQrCode(m_renderer, info.verificationUrl,
                               512 - 95, dy + 30, 190);
      drawText(truncateToWidth(info.verificationUrl, m_fontSmall, 700), 512,
               dy + 226, {0, 180, 216, 255}, m_fontSmall, true);
    }
    drawText("B2: Nhập mã. Google cảnh báo: bấm Nâng cao → Tiếp tục", 512,
             dy + 256, {200, 215, 230, 255}, m_fontSmall, true);
    if (st == AuthState::AWAITING_USER && !info.userCode.empty()) {
      TTF_Font* fCode = m_fontHuge ? m_fontHuge : m_fontLarge;
      drawText(info.userCode, 512, dy + 282, {255, 255, 255, 255}, fCode,
               true);
      int nd = (int)((SDL_GetTicks() / 500) % 4);
      drawText("Đang chờ xác nhận" + std::string(nd, '.'), 512, dy + 388,
               {250, 204, 21, 255}, m_fontSmall, true);
    } else if (st == AuthState::ERROR_OCCURRED) {
      drawText(truncateToWidth(AuthManager::instance().getErrorMessage(),
                               m_fontSmall, 700),
               512, dy + 300, {248, 113, 113, 255}, m_fontSmall, true);
      drawText("A: Thử lấy mã mới", 512, dy + 340, {0, 180, 216, 255},
               m_fontSmall, true);
    } else {
      int nd = (int)((SDL_GetTicks() / 500) % 4);
      drawText("Đang lấy mã liên kết" + std::string(nd, '.'), 512, dy + 310,
               {0, 180, 216, 255}, m_fontMedium, true);
    }
    drawInlineHintsCentered("A Thử lại  •  Y Web nâng cao  •  B Hủy", 512,
                            cardY + cardH - 30, {148, 163, 184, 255},
                            m_fontSmall, 24);
  } else {
    std::string ip = PlatformInfo::instance().getIpAddress("wlan0");
    if (ip.empty() || ip == "Disconnected")
      ip = "192.168.1.164";
    std::string portalUrl = WebServer::instance().getPortalUrl();
    if (portalUrl.empty())
      portalUrl = "http://" + ip + ":??? (Port ban)";

    // Inner panel
    int innerX = cardX + 35;
    int innerY = cardY + 75;
    int innerW = cardW - 70;
    int innerH = 435;
    drawRect(innerX, innerY, innerW, innerH, {16, 20, 28, 255}, true);
    drawBorder(innerX, innerY, innerW, innerH, {45, 55, 72, 255}, 1);
    // QR portal góc phải: quét bằng điện thoại là vào web, khỏi gõ IP.
    QrRenderer::renderQrCode(m_renderer, portalUrl, innerX + innerW - 136,
                             innerY + 16, 120);

    int textY = innerY + 30;

    drawText(UiStrings::WEB_CONNECT_GUIDE_TITLE, 512, textY,
             {255, 255, 255, 255}, m_fontMedium, true);
    drawRect(innerX + 50, textY + 32, innerW - 100, 1, {60, 72, 90, 255}, true);

    textY += 55;
    drawText(truncateToWidth(UiStrings::WEB_CONNECT_STEP1, m_fontMedium,
                             innerW - 136 - 80),
             innerX + 40, textY, {200, 215, 230, 255}, m_fontMedium);

    textY += 45;
    drawText(truncateToWidth(UiStrings::WEB_CONNECT_STEP2, m_fontMedium,
                             innerW - 136 - 80),
             innerX + 40, textY, {200, 215, 230, 255}, m_fontMedium);

    // Prominent glowing URL box in center
    textY += 38;
    int urlBoxW = 620;
    int urlBoxH = 68;
    int urlBoxX = innerX + (innerW - urlBoxW) / 2;
    drawRect(urlBoxX, textY, urlBoxW, urlBoxH, {10, 15, 22, 255}, true);
    drawBorder(urlBoxX, textY, urlBoxW, urlBoxH, {0, 180, 216, 255}, 2);
    drawText(portalUrl, urlBoxX + urlBoxW / 2, textY + 18, {0, 180, 216, 255},
             m_fontTitle ? m_fontTitle : m_fontLarge, true);

    textY += urlBoxH + 34;
    drawText(UiStrings::WEB_CONNECT_STEP3, innerX + 40, textY,
             {200, 215, 230, 255}, m_fontMedium);

    textY += 42;
    drawText(UiStrings::WEB_CONNECT_WAITING, 512, textY, {245, 158, 11, 255},
             m_fontMedium, true);

    textY += 28;
    drawText(UiStrings::WEB_CONNECT_AUTO_HINT, 512, textY, {140, 155, 170, 255},
             m_fontSmall, true);

    // Bottom action button (elastic, centered)
    drawBadge(512 - badgeWidth(UiStrings::WEB_CONNECT_BACK_BTN, 48) / 2,
              cardY + cardH - 68, 0, 48, UiStrings::WEB_CONNECT_BACK_BTN,
              {55, 65, 81, 255}, {255, 255, 255, 255});
    drawInlineHintsCentered("Y Về mã thiết bị  •  B Hủy", 512,
                            cardY + cardH - 30, {148, 163, 184, 255},
                            m_fontSmall, 24);
  }
}

void UIManager::renderSyncOverlay() {
  m_dialogs.renderSyncOverlay(m_ui, m_fontSmall, m_fontMedium, m_fontLarge);
}

void UIManager::renderDownloadOverlay() {
  m_dialogs.renderDownloadOverlay(m_ui, m_fontSmall, m_fontLarge);
}

void UIManager::renderSystemTab() {
  // Body tab HỆ THỐNG (dùng chung cho Diagnostics và Settings tab 5).
  auto diag = PlatformInfo::instance().getDiagnostics();

  struct DiagRow {
    std::string label;
    std::string value;
    SDL_Color valColor;
  };

  std::vector<DiagRow> rows = {
      {UiStrings::DIAG_HW_DEVICE, diag.socName, {255, 255, 255, 255}},
      {UiStrings::DIAG_CPU_ARCH,
       diag.cpuArch + std::string(UiStrings::DIAG_VAL_64BIT),
       {255, 255, 255, 255}},
      {UiStrings::DIAG_OS_KERNEL,
       diag.osName + " " + diag.kernelRelease,
       {255, 255, 255, 255}},
      {UiStrings::DIAG_RAM,
       std::string(UiStrings::DIAG_FREE_PREFIX) + diag.freeRam +
           UiStrings::DIAG_TOTAL_SEPARATOR + diag.totalRam,
       {34, 197, 94, 255}},
      {UiStrings::DIAG_DISPLAY, diag.displayResolution, {0, 180, 216, 255}},
      {UiStrings::DIAG_SDL2_GFX,
       "v" + diag.sdlVersion + std::string(UiStrings::DIAG_VAL_HW_ACCEL),
       {255, 255, 255, 255}},
      {UiStrings::DIAG_SQLITE_DB,
       "v" + diag.sqliteVersion +
           std::string(UiStrings::DIAG_VAL_SCHEMA_PREFIX) +
           std::to_string(CURRENT_SCHEMA_VERSION) + ")",
       {34, 197, 94, 255}},
      {UiStrings::DIAG_SD_STORAGE,
       std::string(UiStrings::DIAG_FREE_PREFIX) + diag.sdFreeSpace +
           UiStrings::DIAG_TOTAL_SEPARATOR + diag.sdTotalSpace,
       {34, 197, 94, 255}},
      {UiStrings::DIAG_GAMEPAD, diag.controllerName, {255, 255, 255, 255}},
      {UiStrings::DIAG_WIFI,
       diag.networkStatus +
           (diag.ipAddress != "N/A" ? " (IP: " + diag.ipAddress + ")" : ""),
       diag.ipAddress != "N/A" ? SDL_Color{34, 197, 94, 255}
                               : SDL_Color{239, 68, 68, 255}},
      {UiStrings::DIAG_SAFETY, UiStrings::DIAG_SAFETY_VAL, {34, 197, 94, 255}}};

  int rowH = 44;
  int startY = 124 - (m_diagnosticsScrollOffset * rowH);
  int cardX = 24;
  int cardW = 976;
  int visibleRows = 14; // How many rows fit on screen

  int maxScroll = static_cast<int>(rows.size()) - visibleRows;
  if (maxScroll < 0)
    maxScroll = 0;

  for (size_t i = 0; i < rows.size(); ++i) {
    int y = startY + static_cast<int>(i) * rowH;
    // Only draw if visible on screen
    if (y < 60 || y > 720)
      continue;

    if (i % 2 == 1) {
      drawRoundedRect(cardX, y - 4, cardW, rowH, UiTheme::RADIUS_CARD,
                      {22, 28, 38, 255}, true);
    }
    drawText(rows[i].label, cardX + 24, y, {150, 165, 180, 255}, m_fontSmall);
    drawText(rows[i].value, cardX + 280, y, rows[i].valColor, m_fontSmall);
  }

  // Scroll indicator (Y=124 dưới tab pills để không trùng mép phải
  // tab "HỆ THỐNG" có X=860..1000, height 715-124-8=583 vừa khít content)
  if (maxScroll > 0) {
    int scrollBarX = 990;
    int scrollBarH = 715 - 124 - 8; // = 583, cách footer 8px
    int scrollBarY = 124;
    int thumbH = scrollBarH * visibleRows / rows.size();
    int thumbY = scrollBarY + (m_diagnosticsScrollOffset *
                               (scrollBarH - thumbH) / maxScroll);

    drawRoundedRect(scrollBarX, scrollBarY, 6, scrollBarH, 4, {35, 42, 54, 255},
                    true);
    drawRoundedRect(scrollBarX, thumbY, 6, thumbH, 4, {0, 180, 216, 255}, true);
  }
}

void UIManager::renderDiagnosticsState() {
  // ─── Background + Header chuẩn (giống Settings/IPTV/YouTube/Explorer) ───
  // Bỏ custom sub-header strip cũ (Y=64..112 + line + title thủ công) để đồng
  // bộ với các app khác. drawAppHeader tự render: FOOTER_BG bar (Y=0..64) +
  // separator line + title ACCENT_CYAN + cum status Wi-Fi/pin/clock bên phải.
  drawAppBackground();
  drawAppHeader(UiStrings::HEADER_DIAG);

  // Tab pills top-right: [GIỚI THIỆU] [HỆ THỐNG]
  {
    int tabH = 32;
    int tabY = 72;
    int wAbout = pillWidth(UiStrings::INFO_TAB_ABOUT, m_fontSmall) + 32;
    int wSys = pillWidth(UiStrings::INFO_TAB_SYSTEM, m_fontSmall) + 32;
    int xSys = 1024 - 24 - wSys;
    int xAbout = xSys - 8 - wAbout;
    drawPill(xAbout, tabY, wAbout, tabH, UiStrings::INFO_TAB_ABOUT,
             m_infoTab == 0, m_fontSmall);
    drawPill(xSys, tabY, wSys, tabH, UiStrings::INFO_TAB_SYSTEM, m_infoTab == 1,
             m_fontSmall);
  }

  if (m_infoTab == 0) {
    renderAboutTab(124);
    drawAppFooter(
        {{UiTheme::PadBtn::B, "Lùi"}, {UiTheme::PadBtn::L1R1, "Chuyển tab"}});
    return;
  }

  renderSystemTab();

  drawAppFooter({{UiTheme::PadBtn::B, "Lùi"}, {UiTheme::PadBtn::DPAD, "Cuộn"}});
}

std::vector<std::string> UIManager::wrapAboutText(const std::string &text,
                                                  TTF_Font *font, int maxPx) {
  std::vector<std::string> lines;
  std::string cur;
  // Wrap theo từ, an toàn UTF-8 (không cắt giữa byte multi-byte).
  auto flush = [&]() {
    if (!cur.empty()) {
      lines.push_back(cur);
      cur.clear();
    }
  };
  size_t i = 0;
  std::string word;
  auto pushWord = [&]() {
    if (word.empty())
      return;
    std::string trial = cur.empty() ? word : cur + " " + word;
    if (textWidth(trial, font) <= maxPx) {
      cur = trial;
    } else {
      if (!cur.empty())
        lines.push_back(cur);
      // Từ đơn quá dài: cắt cứng theo byte-hợp-lệ UTF-8
      cur.clear();
      std::string part;
      for (size_t k = 0; k < word.size();) {
        unsigned char c = static_cast<unsigned char>(word[k]);
        size_t len = 1;
        if ((c & 0x80) == 0)
          len = 1;
        else if ((c & 0xE0) == 0xC0)
          len = 2;
        else if ((c & 0xF0) == 0xE0)
          len = 3;
        else if ((c & 0xF8) == 0xF0)
          len = 4;
        std::string trialPart = part + word.substr(k, len);
        if (textWidth(trialPart, font) > maxPx && !part.empty()) {
          lines.push_back(part);
          part.clear();
          trialPart = word.substr(k, len);
        }
        part = trialPart;
        k += len;
      }
      cur = part;
    }
    word.clear();
  };
  while (i <= text.size()) {
    char c = (i < text.size()) ? text[i] : ' ';
    if (c == '\n') {
      pushWord();
      flush();
    } else if (c == ' ') {
      pushWord();
    } else {
      word += c;
    }
    ++i;
  }
  pushWord();
  flush();
  if (lines.empty())
    lines.push_back("");
  return lines;
}

void UIManager::renderAboutTab(int contentTop) {
  // ════════════════════════════════════════════════════════════════════════
  // LAYOUT: 2 CỘT ĐỀU (bám theo contentTop để không đè tab pill Y=72..104)
  //   Mỗi col W=480 (1024 - 24 - 24 margin - 16 gap = 960 → /2)
  //   Col 1 (trái, X=24) — 1 CARD duy nhất, full height (contentTop..715):
  //     Top:    Version + author + date + divider
  //     Bottom: 2 SUB-COL bên trong (QR căn giữa dọc trong space còn lại):
  //             Sub-A (W=200, trái): QR 200x200
  //             Sub-B (W=232, phải): 2 dòng caption donate, left-align
  //     (changelog "Tính năng mới" đã move về OTA page trong Settings tab)
  //   Col 2 (phải, X=520) — full content height (contentTop..715):
  //     THƯ NGỎ letter với auto-scroll
  // ════════════════════════════════════════════════════════════════════════
  const int col1X = 24;
  const int col1W = 480;
  const int col2X = 24 + col1W + 16;   // = 520 (16px gap giữa 2 col)
  const int col2W = 1024 - 24 - col2X; // = 480 (đều với col1)
  const int col1Y = contentTop;        // = 124
  const int col1H = 715 - col1Y;       // = 591 (full height)
  const int col2Y = contentTop;        // = 124
  const int col2H = 715 - col2Y;       // = 591
  const int cardR = UiTheme::RADIUS_CARD;

  auto otaInfo = UpdateManager::instance().getLatestInfo();

  // ─── Col 1: Single card (version + QR donate) ───
  drawRoundedRect(col1X, col1Y, col1W, col1H, cardR, {22, 28, 38, 255}, true);
  drawRoundedBorder(col1X, col1Y, col1W, col1H, cardR, {51, 65, 85, 255}, 1);

  // ── Top section: Version + author + date + divider ──
  const int col1PadL = 16, col1PadR = 16;
  int y = col1Y + 16;
  std::string verText = std::string("RomCloud v") + APP_VERSION;
  drawText(verText, col1X + col1PadL, y, {255, 255, 255, 255}, m_fontLarge);
  y += textHeight(m_fontLarge) + 4;

  std::string subText = UiStrings::ABOUT_AUTHOR;
  if (!otaInfo.releaseDate.empty()) {
    subText += std::string("  •  ") + otaInfo.releaseDate;
  }
  drawText(subText, col1X + col1PadL, y, {0, 180, 216, 255}, m_fontSmall);
  y += textHeight(m_fontSmall) + 14;

  // Divider dưới author
  drawRect(col1X + col1PadL, y, col1W - col1PadL - col1PadR, 1,
           {51, 65, 85, 100}, true);
  y += 1 + 16;

  // (changelog "Tính năng mới" đã move về OTA page trong Settings tab)

  // ── Bottom section: QR 250x250 căn giữa ngang + 2 dòng bullets bên dưới, căn
  // trái ── Toàn bộ block (QR + gap + caption) căn giữa dọc trong space còn
  // lại.
  //   Layout:
  //     ┌─ Card W=480 ─┐
  //     │   top section│
  //     │              │
  //     │   ┌──250──┐  │  ← QR 250x250, centered horizontally (qrX=139)
  //     │   │       │  │
  //     │   │  QR   │  │
  //     │   │       │  │
  //     │   └───────┘  │
  //     │              │
  //     │  • line 1    │  ← bullet + text, left-align (captionX=40)
  //     │  • line 2    │
  //     └──────────────┘
  const int qrSize = 300; // up từ 200 → 250
  const int subGap = 14;  // gap QR → text
  const int qrY_padBottom = 16;
  int sectionTop = y;                                // sau divider
  int sectionBottom = col1Y + col1H - qrY_padBottom; // = 699
  int sectionH = sectionBottom - sectionTop;         // = 466

  const int lineGap = textHeight(m_fontSmall) + 4; // = 32
  const int bulletIndent = 20;                // indent cho wrapped continuation
  int captionX = col1X + col1PadL;            // = 40 (căn trái theo padding)
  int captionW = col1W - col1PadL - col1PadR; // = 448

  // Measure caption block height (mỗi dòng có thể wrap thành nhiều dòng con).
  // wrapAboutText wrap theo từ tại maxWidth; truncateToWidth là safety-net.
  auto measureLines = [&](const std::string &text) -> int {
    auto wrapped = wrapAboutText(text, m_fontSmall, captionW - bulletIndent);
    if (wrapped.empty())
      return 1;
    return static_cast<int>(wrapped.size());
  };
  int captionLineCount = measureLines(UiStrings::ABOUT_DONATE_LINE1) +
                         measureLines(UiStrings::ABOUT_DONATE_LINE2);
  int captionBlockH = captionLineCount * lineGap; // dynamic: 64 / 96 / ...
  int blockH = qrSize + subGap + captionBlockH;
  int blockY = sectionTop + (sectionH - blockH) / 2; // căn giữa dọc
  int qrX = col1X + (col1W - qrSize) / 2;            // = 139 (căn giữa ngang)
  int qrY = blockY;
  int captionY = qrY + qrSize + subGap;

  std::string qrPath =
      AppConfig::instance().getAssetsDir() + "/apps_icons/QR.png";
  SDL_Texture *qrTex = m_ui.getOrLoadImage("about/QR.png", qrPath);
  if (qrTex) {
    SDL_Rect dst = {qrX, qrY, qrSize, qrSize};
    SDL_RenderCopy(m_renderer, qrTex, nullptr, &dst);
    drawBorder(qrX, qrY, qrSize, qrSize, {51, 65, 85, 255}, 1);
  } else {
    // Fallback nếu chưa nạp file ảnh: render mã VietQR (Vietcombank -
    // 9929666989)
    const std::string vietQrPayload =
        "00020101021138540010A00000072701240006970436011099296669890208QRIBFTTA"
        "53037045802VN630462B9";
    QrRenderer::renderQrCode(m_renderer, vietQrPayload, qrX, qrY, qrSize,
                             {0, 0, 0, 255}, {255, 255, 255, 255});
    drawBorder(qrX, qrY, qrSize, qrSize, {51, 65, 85, 255}, 1);
  }

  // Render 2 dòng bullets, left-align, mỗi dòng 1 bullet (•).
  // Nếu 1 dòng wrap, các dòng continuation indent bằng bulletIndent để align
  // với text sau bullet (chứ không về lề trái card).
  //   first line:   "•  <text chunk 1>"      tại captionX
  //   continuation: "    <text chunk 2>"     tại captionX + bulletIndent
  auto renderBullet = [&](const std::string &text, int lineY, SDL_Color color) {
    // Wrap về captionW - bulletIndent để first line + "•  " vẫn fit captionW
    auto wrapped = wrapAboutText(text, m_fontSmall, captionW - bulletIndent);
    if (wrapped.empty())
      wrapped.push_back("");
    // First line: bullet prefix + wrapped text
    std::string first = "•  " + truncateToWidth(wrapped[0], m_fontSmall,
                                                captionW - bulletIndent);
    drawText(first, captionX, lineY, color, m_fontSmall);
    lineY += lineGap;
    // Continuation: indent bằng bulletIndent
    for (size_t i = 1; i < wrapped.size(); ++i) {
      std::string l =
          truncateToWidth(wrapped[i], m_fontSmall, captionW - bulletIndent);
      drawText(l, captionX + bulletIndent, lineY, color, m_fontSmall);
      lineY += lineGap;
    }
    return lineY;
  };
  int ly = captionY;
  // Line 1 (sáng hơn — message chính)
  ly = renderBullet(UiStrings::ABOUT_DONATE_LINE1, ly, {200, 210, 225, 255});
  // Line 2 (mờ hơn — instruction)
  ly = renderBullet(UiStrings::ABOUT_DONATE_LINE2, ly, {100, 115, 135, 255});

  // ─── Col 2: THƯ NGỎ card với auto-scroll ───
  drawRoundedRect(col2X, col2Y, col2W, col2H, cardR, {22, 28, 38, 255}, true);
  drawRoundedBorder(col2X, col2Y, col2W, col2H, cardR, {51, 65, 85, 255}, 1);

  // ─── "THƯ NGỎ" cell: auto-scroll ───
  // Tốc độ 22 px/sec ~ đọc vừa phải. Khi chạm đáy + hold ~4 dòng → loop về top.
  static constexpr float kAboutScrollPxPerSec = 22.0f;
  static constexpr int kAboutEndHoldLines = 4;

  std::vector<std::string> paras = {
      " "
      "Xin chào bạn! Mình là bun2it, tác giả của RomCloud.",
      "Mỗi khi được nghe từ bạn rằng Romcloud xem TV mượt, YouTube chạy tốt, "
      "gửi file qua LocalSend thành công, cast lên màn hình lớn mượt mà; mình "
      "đều cảm thấy hạnh phúc. Mỗi một lượt cài đặt là một động lực lớn cho "
      "người tạo ra một ứng dụng có ích cho cộng đồng Trimui Brick Pro.",
      "RomCloud hoàn toàn miễn phí. Mình viết nó vì đam mê, và vì mình cũng là "
      "người xài Brick Pro.",
      "Nếu bạn thấy app này chất lượng và muốn ủng hộ để mình có thêm động "
      "lực tiếp tục cập nhật, nghiên cứu các tính năng hữu ích, bạn có thể "
      "ủng hộ mình bằng cách quét QR bên cạnh nhé.",
      "Dù ủng hộ hay không, cảm ơn bạn đã ở đây và cùng nhau tạo nên "
      "những giá trị cho cộng đồng Brick Pro.",
      "Chúc các bạn có những trải nghiệm thật vui vẻ!",
  };

  // ─── Layout THƯ NGỎ card: FIX OVERLAP với title ───
  // Old: padT=8, titleH = textHeight(fontMedium) + 4, divider textAreaY-4,
  //      scroll start textAreaY → gap chỉ 4px, scroll lên có thể đè title.
  // New: padT=14, divider cách title 4px (titleH là textHeight),
  //      scroll start = dividerY + 12 → 12px gap rõ ràng giữa divider
  //      và dòng scroll đầu tiên. Dùng SDL_RenderSetClipRect để chặn
  //      bất kỳ dòng nào scrolling ra ngoài textArea.
  const int padL = 22, padR = 22, padT = 14, padB = 18;
  const int cellX = col2X;
  const int cellY = col2Y;
  const int cellW = col2W;
  const int cellH = col2H;
  const int innerW = cellW - padL - padR; // = 436 (cellW=480)

  std::vector<std::string> lines;
  for (const auto &p : paras) {
    auto wl = wrapAboutText(p, m_fontSmall, innerW);
    for (const auto &l : wl)
      lines.push_back(l);
    lines.push_back(""); // blank line between paragraphs
  }
  int lineH = textHeight(m_fontSmall) + 4;
  int totalH = static_cast<int>(lines.size()) * lineH;

  const int titleH = textHeight(m_fontMedium);
  const int titleY = cellY + padT;          // = 138 (cellY=124, padT=14)
  const int dividerY = titleY + titleH + 4; // = ~178 (4px gap dưới title)
  const int textAreaY = dividerY + 12;      // = ~190 (12px gap rõ ràng)
  const int textAreaBottom = cellY + cellH - padB; // = 697
  const int textAreaH = textAreaBottom - textAreaY;

  // Vẽ title
  drawText(UiStrings::ABOUT_LETTER_TITLE, cellX + padL, titleY,
           {255, 255, 255, 255}, m_fontMedium);
  // Divider 1px dưới title
  drawRect(cellX + padL, dividerY, cellW - padL - padR, 1, {51, 65, 85, 100},
           true);

  if (textAreaH >= lineH * 2) {
    // ─── Auto-scroll state ───
    uint32_t now = SDL_GetTicks();
    if (m_aboutLastTickMs == 0)
      m_aboutLastTickMs = now; // first frame sau entry → dt = 0
    float dt = (now - m_aboutLastTickMs) / 1000.0f;
    m_aboutLastTickMs = now;
    // Clamp dt để tránh giật khi app resume sau minimize.
    if (dt > 0.25f)
      dt = 0.25f;
    m_aboutAutoScrollY += dt * kAboutScrollPxPerSec;

    int maxScrollPx = totalH - textAreaH;
    if (maxScrollPx < 0)
      maxScrollPx = 0;
    int loopThreshold = maxScrollPx + kAboutEndHoldLines * lineH;
    if (m_aboutAutoScrollY >= loopThreshold) {
      // Đã cuộn hết + hold cuối → loop về đầu để user đọc lại.
      m_aboutAutoScrollY = 0.0f;
    }
    int scrollPx = static_cast<int>(m_aboutAutoScrollY);
    if (scrollPx > maxScrollPx)
      scrollPx = maxScrollPx;

    // Set clip rect để scroll text KHÔNG BAO GIỜ render ra ngoài textArea
    // (lên title/divider hoặc xuống quá bottom card).
    SDL_Rect clip = {cellX + padL, textAreaY, cellW - padL - padR, textAreaH};
    SDL_RenderSetClipRect(m_renderer, &clip);

    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
      int ly = textAreaY + i * lineH - scrollPx;
      int lyEnd = ly + lineH;
      if (lyEnd <= textAreaY || ly >= textAreaBottom)
        continue;
      if (lines[i].empty())
        continue;
      drawText(lines[i], cellX + padL, ly, {200, 210, 225, 255}, m_fontSmall);
    }

    // Reset clip rect về mặc định
    SDL_RenderSetClipRect(m_renderer, nullptr);
  }
}

void UIManager::renderReverseSyncState() {
  // ─── Borderless Full-Width Sub-Header ───
  drawRect(0, 64, 1024, 48, UiTheme::CARD_SOLID, true);
  drawRect(0, 111, 1024, 1, {38, 48, 64, 255}, true);
  drawText(UiStrings::REVERSE_SYNC_TITLE, 36, 78, {168, 85, 247, 255},
           m_fontLarge);

  auto prog = UploadManager::instance().getProgress();

  int cardX = 100;
  int cardW = 824;
  int cardY = 126;
  int cardH = 480;

  drawRoundedRect(cardX, cardY, cardW, cardH, UiTheme::RADIUS_MODAL,
                  {22, 28, 38, 255}, true);
  drawRoundedBorder(cardX, cardY, cardW, cardH, UiTheme::RADIUS_MODAL,
                    {168, 85, 247, 255}, 2);

  int contentY = cardY + 30;

  switch (prog.state) {
  case UploadState::IDLE: {
    drawText(UiStrings::REVERSE_SYNC_PREPARING, cardX + cardW / 2,
             contentY + 100, {255, 255, 255, 255}, m_fontLarge, true);
    break;
  }

  case UploadState::PREPARING: {
    drawText(UiStrings::REVERSE_SYNC_PREPARING, cardX + cardW / 2,
             contentY + 100, {0, 180, 216, 255}, m_fontLarge, true);
    break;
  }

  case UploadState::UPLOADING: {
    // Game title
    std::string titleText = UiStrings::REVERSE_SYNC_UPLOADING;
    drawText(titleText, cardX + cardW / 2, contentY + 20, {255, 255, 255, 255},
             m_fontMedium, true);

    // Current file
    std::string gameName = truncateToWidth(prog.gameTitle, m_fontLarge, 560);
    drawText(gameName, cardX + cardW / 2, contentY + 60, {0, 180, 216, 255},
             m_fontLarge, true);

    // Progress
    int barW = 600;
    int barH = 20;
    int barX = cardX + (cardW - barW) / 2;
    int barY = contentY + 120;
    float pct = std::max(0.0f, std::min(100.0f, (float)prog.progressPct));
    drawProgressBar(barX, barY, barW, barH, pct / 100.0, {168, 85, 247, 255},
                    true);

    // Stats & speed
    std::string speedStr = "";
    if (prog.speedKBps >= 1024.0) {
      char sBuf[32];
      std::snprintf(sBuf, sizeof(sBuf), "  •  %.1f MB/s",
                    prog.speedKBps / 1024.0);
      speedStr = sBuf;
    } else if (prog.speedKBps > 0.0) {
      char sBuf[32];
      std::snprintf(sBuf, sizeof(sBuf), "  •  %.0f KB/s", prog.speedKBps);
      speedStr = sBuf;
    }

    char pctBuf[16];
    std::snprintf(pctBuf, sizeof(pctBuf), " (%.1f%%)", prog.progressPct);

    std::string stats =
        UiStrings::REVERSE_SYNC_STATS + std::to_string(prog.currentIndex) +
        " / " + std::to_string(prog.totalGames) + "  •  " +
        FileSystemManager::instance().formatBytes(prog.bytesUploaded) + " / " +
        FileSystemManager::instance().formatBytes(prog.totalBytes) + pctBuf +
        speedStr;
    drawText(stats, cardX + cardW / 2, barY + 50, {200, 210, 225, 255},
             m_fontMedium, true);

    // Success/fail counts
    int statY = barY + 90;
    drawText("✓ " + std::to_string(prog.gamesUploaded) + " thành công",
             cardX + 100, statY, {34, 197, 94, 255}, m_fontMedium);
    drawText("✗ " + std::to_string(prog.gamesFailed) + " thất bại",
             cardX + cardW - 200, statY, {239, 68, 68, 255}, m_fontMedium);
    break;
  }

  case UploadState::COMPLETED: {
    drawText(UiStrings::REVERSE_SYNC_SUCCESS, cardX + cardW / 2, contentY + 80,
             {34, 197, 94, 255}, m_fontLarge, true);
    drawText(std::to_string(prog.gamesUploaded) +
                 UiStrings::REVERSE_SYNC_SUCCESS_SUF,
             cardX + cardW / 2, contentY + 130, {255, 255, 255, 255},
             m_fontMedium, true);
    if (prog.gamesFailed > 0) {
      drawText(std::to_string(prog.gamesFailed) + " game thất bại.",
               cardX + cardW / 2, contentY + 170, {239, 68, 68, 255},
               m_fontSmall, true);
    }
    break;
  }

  case UploadState::FAILED: {
    drawText("THẤT BẠI", cardX + cardW / 2, contentY + 60, {239, 68, 68, 255},
             m_fontLarge, true);
    // Multi-line error message display
    std::istringstream errStream(prog.errorMessage);
    std::string errLine;
    int errY = contentY + 110;
    while (std::getline(errStream, errLine)) {
      drawText(errLine, cardX + cardW / 2, errY, {200, 210, 225, 255},
               m_fontSmall, true);
      errY += 28;
    }
    break;
  }

  case UploadState::CANCELLED: {
    drawText("ĐÃ HỦY", cardX + cardW / 2, contentY + 80, {245, 158, 11, 255},
             m_fontLarge, true);
    drawText("Đã tải lên " + std::to_string(prog.gamesUploaded) +
                 " game trước khi hủy.",
             cardX + cardW / 2, contentY + 130, {200, 210, 225, 255},
             m_fontSmall, true);
    break;
  }
  }

  // Cancel button (elastic, centered)
  if (prog.state == UploadState::UPLOADING ||
      prog.state == UploadState::PREPARING) {
    drawBadge(cardX +
                  (cardW - badgeWidth(UiStrings::REVERSE_SYNC_CANCEL_BTN, 46)) /
                      2,
              cardY + cardH - 70, 0, 46, UiStrings::REVERSE_SYNC_CANCEL_BTN,
              {55, 65, 81, 255}, {255, 255, 255, 255});
  } else {
    int dualW = badgeDualWidth("/", "Quay lại", 46);
    drawBadgeDual(cardX + (cardW - dualW) / 2, cardY + cardH - 70, dualW, 46,
                  "A", "/", "B", "Quay lại", {55, 65, 81, 255},
                  {255, 255, 255, 255});
  }
}

void UIManager::renderUploadOverlay() {
  m_dialogs.renderUploadOverlay(m_ui, m_fontSmall);
}

void UIManager::renderOTAUpdateState() {
  // ─── P5: body-only render (không vẽ header/footer) ─────────────────────
  // Được gọi từ renderSettingsState() khi tab=1 (CẬP NHẬT). Header đã có ở
  // parent (drawAppHeader "CÀI ĐẶT"), tab pills đã có (Y=72..104), footer
  // do parent lo (drawAppFooter "Đồng ý/Lùi/Chuyển tab").
  // Nội dung bắt đầu Y=124 (20px dưới tabs) khớp pattern THÔNG TIN.
  auto prog = UpdateManager::instance().getProgress();
  auto info = UpdateManager::instance().getLatestInfo();

  int cardX = 24;
  int cardW = 976;

  std::string currentVer = std::string(UiStrings::OTA_DEV_CURRENT_VER) +
                           UpdateManager::instance().getCurrentVersion();
  drawText(currentVer, cardX + 20, 124, {210, 220, 235, 255}, m_fontMedium);

  std::string repoSource =
      std::string(UiStrings::OTA_DEV_SOURCE_PREFIX) + std::string(GITHUB_REPO);
  drawText(repoSource, cardX + 20, 154, {130, 145, 165, 255}, m_fontSmall);

  int contentBoxY = 188;
  int contentBoxH = 360;
  drawRoundedRect(cardX, contentBoxY, cardW, contentBoxH, UiTheme::RADIUS_MODAL,
                  {18, 24, 34, 255}, true);
  drawRoundedBorder(cardX, contentBoxY, cardW, contentBoxH,
                    UiTheme::RADIUS_MODAL, {38, 48, 64, 255}, 1);

  switch (prog.state) {
  case UpdateState::IDLE:
  case UpdateState::CHECKING: {
    drawText(UiStrings::OTA_CHECKING, 512, contentBoxY + 130,
             {245, 158, 11, 255}, m_fontLarge, true);
    drawText(UiStrings::OTA_WAITING, 512, contentBoxY + 175,
             {150, 165, 180, 255}, m_fontSmall, true);
    break;
  }
  case UpdateState::UP_TO_DATE: {
    int stW = badgeWidth(UiStrings::OTA_STATUS_UP_TO_DATE, 36);
    drawBadge(512 - stW / 2, contentBoxY + 22, stW, 36,
              UiStrings::OTA_STATUS_UP_TO_DATE, {22, 101, 52, 255},
              {34, 197, 94, 255});
    // Dòng version + ngày release (giữ pill, thêm context bản hiện tại)
    std::string verLine =
        std::string("v") +
        (info.remoteVersion.empty()
             ? UpdateManager::instance().getCurrentVersion()
             : info.remoteVersion) +
        (info.releaseDate.empty() ? "" : std::string(" • ") + info.releaseDate);
    drawText(verLine, 512, contentBoxY + 72, {0, 180, 216, 255}, m_fontSmall,
             true);

    // Nội dung release notes feed từ bản release (version.json/GitHub).
    // Không có thì fallback trống (không hiện text giả).
    std::string notes = info.changelog;
    if (!notes.empty()) {
      drawText(UiStrings::OTA_CHANGELOG_TITLE, cardX + 40, contentBoxY + 104,
               {255, 255, 255, 255}, m_fontSmall);
      std::vector<std::string> paragraphs;
      std::string cur;
      for (char c : notes) {
        if (c == '\n') {
          if (!cur.empty()) {
            paragraphs.push_back(cur);
            cur.clear();
          }
        } else {
          cur += c;
        }
      }
      if (!cur.empty())
        paragraphs.push_back(cur);
      if (paragraphs.empty())
        paragraphs.push_back(notes);
      std::vector<std::string> lines;
      for (size_t i = 0; i < paragraphs.size(); ++i) {
        auto w = wrapAboutText(paragraphs[i], m_fontSmall, cardW - 80);
        for (const auto &l : w)
          lines.push_back(l);
        if (i + 1 < paragraphs.size())
          lines.push_back("");
      }
      const int maxLines = 5;
      const int lineGap = textHeight(m_fontSmall) + 4;
      int noteY = contentBoxY + 130;
      bool truncated = static_cast<int>(lines.size()) > maxLines;
      int showN = truncated ? maxLines - 1 : static_cast<int>(lines.size());
      for (int i = 0; i < showN; ++i) {
        if (lines[i].empty())
          continue;
        drawText(std::string("• ") + lines[i], cardX + 40, noteY,
                 {170, 180, 195, 255}, m_fontSmall);
        noteY += lineGap;
      }
      if (truncated) {
        drawText(UiStrings::OTA_VIEW_FULL_HINT, cardX + 40, noteY,
                 {0, 180, 216, 255}, m_fontSmall);
      }
    } else {
      // Fallback trống: không có notes từ release thì không hiện gì thêm.
    }
    break;
  }
  case UpdateState::UPDATE_AVAILABLE: {
    drawBadge(512 - badgeWidth(UiStrings::OTA_STATUS_NEW_UPDATE, 36) / 2,
              contentBoxY + 30, 0, 36, UiStrings::OTA_STATUS_NEW_UPDATE,
              {180, 83, 9, 255}, {255, 255, 255, 255});
    std::string newVerTxt =
        std::string(UiStrings::OTA_DEV_NEW_VER_PREFIX) + info.remoteVersion +
        (info.releaseDate.empty() ? "" : " (" + info.releaseDate + ")");
    drawText(newVerTxt, 512, contentBoxY + 85, {0, 180, 216, 255}, m_fontLarge,
             true);

    if (!info.changelog.empty()) {
      drawText(UiStrings::OTA_CHANGELOG_TITLE, cardX + 40, contentBoxY + 130,
               {255, 255, 255, 255}, m_fontSmall);
      drawText(info.changelog, cardX + 40, contentBoxY + 160,
               {170, 180, 195, 255}, m_fontSmall);
    }

    drawBadge(512 - badgeWidth(UiStrings::OTA_BTN_INSTALL_NOW, 52) / 2,
              contentBoxY + 260, 0, 52, UiStrings::OTA_BTN_INSTALL_NOW,
              {34, 197, 94, 255}, {0, 0, 0, 255});
    break;
  }
  case UpdateState::DOWNLOADING:
  case UpdateState::DOWNLOADING_DEPS:
  case UpdateState::INSTALLING:
  case UpdateState::INSTALLING_DEPS:
  case UpdateState::VERIFYING: {
    std::string title = UiStrings::OTA_DOWNLOADING_TITLE;
    if (prog.state == UpdateState::DOWNLOADING_DEPS) {
      title = "Đang tải gói hỗ trợ phát video (mpv)...";
    } else if (prog.state == UpdateState::INSTALLING ||
               prog.state == UpdateState::INSTALLING_DEPS) {
      title = "Đang cài đặt bản cập nhật...";
    }
    drawText(title, 512, contentBoxY + 40, {0, 180, 216, 255}, m_fontLarge,
             true);

    // Detailed current step description
    std::string stepMsg = prog.currentStep.empty() ? title : prog.currentStep;
    drawText(stepMsg, 512, contentBoxY + 80, {210, 225, 240, 255}, m_fontSmall,
             true);

    int barW = 580;
    int barH = 22;
    int barX = 512 - barW / 2;
    int barY = contentBoxY + 115;
    float pct = std::max(0.0, std::min(100.0, prog.progressPct));
    drawProgressBar(barX, barY, barW, barH, pct / 100.0, {34, 197, 94, 255},
                    true);

    char pctBuf[32];
    std::snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", pct);
    std::string dlStr =
        (prog.bytesDownloaded > 0)
            ? FileSystemManager::instance().formatBytes(prog.bytesDownloaded)
            : "0 B";
    std::string totStr =
        (prog.totalBytes > 0)
            ? FileSystemManager::instance().formatBytes(prog.totalBytes)
            : "...";
    std::string progressInfo = dlStr + " / " + totStr + " (" + pctBuf + ")";

    // Format download speed
    if (prog.speedKBps >= 1024.0) {
      char sBuf[32];
      std::snprintf(sBuf, sizeof(sBuf), "  •  %.1f MB/s",
                    prog.speedKBps / 1024.0);
      progressInfo += sBuf;
    } else if (prog.speedKBps > 0.0) {
      char sBuf[32];
      std::snprintf(sBuf, sizeof(sBuf), "  •  %.0f KB/s", prog.speedKBps);
      progressInfo += sBuf;
    }

    drawText(progressInfo, 512, barY + 34, {255, 255, 255, 255}, m_fontSmall,
             true);

    if (prog.state == UpdateState::VERIFYING) {
      drawText(UiStrings::OTA_VERIFYING_FILE, 512, contentBoxY + 190,
               {245, 158, 11, 255}, m_fontSmall, true);
    }
    if (prog.state == UpdateState::DOWNLOADING ||
        prog.state == UpdateState::DOWNLOADING_DEPS) {
      drawBadge(512 - badgeWidth(UiStrings::OTA_BTN_CANCEL_DOWNLOAD, 44) / 2,
                contentBoxY + 265, 0, 44, UiStrings::OTA_BTN_CANCEL_DOWNLOAD,
                {55, 65, 81, 255}, {255, 255, 255, 255});
    } else {
      drawBadge(512 - badgeWidth("Đang xử lý, vui lòng chờ...", 44) / 2,
                contentBoxY + 265, 0, 44, "Đang xử lý, vui lòng chờ...",
                {40, 50, 65, 255}, {200, 215, 230, 255});
    }
    break;
  }
  case UpdateState::COMPLETED: {
    drawBadge(512 - badgeWidth(UiStrings::OTA_STATUS_COMPLETED, 40) / 2,
              contentBoxY + 35, 0, 40, UiStrings::OTA_STATUS_COMPLETED,
              {22, 101, 52, 255}, {34, 197, 94, 255});
    drawText(UiStrings::OTA_MSG_COMPLETED, 512, contentBoxY + 105,
             {34, 197, 94, 255}, m_fontLarge, true);
    drawText(UiStrings::OTA_MSG_RESTART_HINT, 512, contentBoxY + 150,
             {255, 255, 255, 255}, m_fontSmall, true);

    drawBadge(512 - badgeWidth(UiStrings::OTA_BTN_RESTART_NOW, 52) / 2,
              contentBoxY + 245, 0, 52, UiStrings::OTA_BTN_RESTART_NOW,
              {34, 197, 94, 255}, {0, 0, 0, 255});
    break;
  }
  case UpdateState::FAILED: {
    drawBadge(512 - badgeWidth(UiStrings::OTA_STATUS_FAILED, 40) / 2,
              contentBoxY + 35, 0, 40, UiStrings::OTA_STATUS_FAILED,
              {153, 27, 27, 255}, {248, 113, 113, 255});
    drawText(UiStrings::OTA_MSG_FAILED, 512, contentBoxY + 105,
             {239, 68, 68, 255}, m_fontLarge, true);
    std::string err = prog.errorMessage.empty() ? UiStrings::OTA_ERR_NETWORK
                                                : prog.errorMessage;
    drawText(err, 512, contentBoxY + 150, {245, 158, 11, 255}, m_fontSmall,
             true);

    int retryW = badgeDualWidth("Thử lại", "Quay lại", 48);
    drawBadgeDual(512 - retryW / 2, contentBoxY + 245, retryW, 48, "A",
                  "Thử lại", "B", "Quay lại", {35, 45, 60, 255},
                  {255, 255, 255, 255});
    break;
  }
  }

  // Footer do renderSettingsState() xử lý (đã có L1R1 "Chuyển tab" hint)
}

void UIManager::renderOTAChangelogState() {
  // ─── P6: Full changelog sub-page từ Cập nhật tab (Y → open) ────────
  // Render full-page (không modal). Header riêng "Cập nhật - Tính năng mới"
  // + sub "vX.Y.Z • date". Card chiếm full content area. Changelog được wrap
  // paragraph-aware (split bằng \n trước, wrap từng đoạn). Nếu dài hơn card →
  // scroll thủ công bằng UP/DOWN (line-based).
  drawAppBackground();
  auto info = UpdateManager::instance().getLatestInfo();
  std::string sub;
  if (!info.remoteVersion.empty())
    sub = std::string("v") + info.remoteVersion;
  if (!info.releaseDate.empty())
    sub +=
        sub.empty() ? info.releaseDate : std::string(" • ") + info.releaseDate;
  drawAppHeader(UiStrings::OTA_CHANGELOG_HEADER, sub);

  // ─── Card full content area ───
  int cardX = 24;
  int cardW = 1024 - 48;        // = 976
  int cardY = 124;              // sau header (no tabs ở sub-page)
  int cardH = 715 - cardY - 16; // = 575 (chừa 16 cho footer)
  drawRoundedRect(cardX, cardY, cardW, cardH, UiTheme::RADIUS_CARD,
                  {22, 28, 38, 255}, true);
  drawRoundedBorder(cardX, cardY, cardW, cardH, UiTheme::RADIUS_CARD,
                    {51, 65, 85, 255}, 1);

  // ─── Header section trong card: version title + date + divider ───
  const int padL = 28, padR = 28, padT = 22;
  int textW = cardW - padL - padR; // = 920
  int y = cardY + padT;
  std::string title = std::string("RomCloud v") +
                      (info.remoteVersion.empty() ? "?" : info.remoteVersion);
  drawText(title, cardX + padL, y, {255, 255, 255, 255}, m_fontLarge);
  y += textHeight(m_fontLarge) + 6;

  if (!info.releaseDate.empty()) {
    drawText(std::string("Ngày phát hành: ") + info.releaseDate, cardX + padL,
             y, {0, 180, 216, 255}, m_fontSmall);
    y += textHeight(m_fontSmall) + 6;
  }

  drawRect(cardX + padL, y, textW, 1, {51, 65, 85, 100}, true);
  y += 1 + 16;

  // ─── Body: changelog feed từ release, trống thì để trống ───
  std::string body = info.changelog;

  // Split body thành paragraphs (split bằng \n, bỏ qua dòng rỗng).
  // Mỗi paragraph được wrap bằng wrapAboutText (UTF-8 safe).
  std::vector<std::string> paragraphs;
  std::string cur;
  for (char c : body) {
    if (c == '\n') {
      if (!cur.empty()) {
        paragraphs.push_back(cur);
        cur.clear();
      }
    } else {
      cur += c;
    }
  }
  if (!cur.empty())
    paragraphs.push_back(cur);
  if (paragraphs.empty())
    paragraphs.push_back(body);

  std::vector<std::string> lines;
  const int paraGap = 10;
  for (size_t i = 0; i < paragraphs.size(); ++i) {
    auto w = wrapAboutText(paragraphs[i], m_fontSmall, textW);
    for (const auto &l : w)
      lines.push_back(l);
    if (i + 1 < paragraphs.size())
      lines.push_back(""); // dấu hiệu paragraph break
  }

  const int lineGap = textHeight(m_fontSmall) + 4;
  const int contentTopY = y;
  const int contentBottomY = cardY + cardH - 16;
  const int contentH = contentBottomY - contentTopY;
  const int visibleLines = contentH / lineGap;
  int totalLines = static_cast<int>(lines.size());

  // Clamp scroll offset (cap maxLines có thể scroll tới khi hiện dòng cuối)
  int maxScroll = std::max(0, totalLines - visibleLines);
  if (m_otaChangelogScrollLine > maxScroll)
    m_otaChangelogScrollLine = maxScroll;
  if (m_otaChangelogScrollLine < 0)
    m_otaChangelogScrollLine = 0;

  // Render visible lines (scroll-based)
  SDL_Rect clip = {cardX + padL, contentTopY, textW, contentH};
  SDL_RenderSetClipRect(m_renderer, &clip);
  int drawY = contentTopY;
  for (int i = 0; i < totalLines; ++i) {
    if (i < m_otaChangelogScrollLine)
      continue;
    if (drawY + lineGap > contentBottomY)
      break;

    if (lines[i].empty()) {
      // Paragraph break → extra gap (1 line) trước khi render dòng kế
      drawY += paraGap;
      continue;
    }
    std::string shown = truncateToWidth(lines[i], m_fontSmall, textW);
    drawText(shown, cardX + padL, drawY, {200, 210, 225, 255}, m_fontSmall);
    drawY += lineGap;
  }
  SDL_RenderSetClipRect(m_renderer, nullptr);

  // ─── Scroll indicator (góc phải-dưới card, chỉ khi content overflow) ───
  if (totalLines > visibleLines) {
    int scrollBarX = cardX + cardW - 6;
    int scrollBarY = contentTopY + 4;
    int scrollBarH = contentH - 8;
    // Track
    drawRect(scrollBarX, scrollBarY, 2, scrollBarH, {40, 50, 65, 255}, true);
    // Thumb (proportional to visibleLines/totalLines)
    int thumbH = std::max(20, scrollBarH * visibleLines / totalLines);
    int thumbTravel = scrollBarH - thumbH;
    int thumbY =
        scrollBarY + (maxScroll > 0
                          ? (thumbTravel * m_otaChangelogScrollLine / maxScroll)
                          : 0);
    drawRect(scrollBarX, thumbY, 2, thumbH, {0, 180, 216, 255}, true);
  }

  // ─── Footer ───
  drawAppFooter({{UiTheme::PadBtn::B, "Quay lại"}});
}

// ---------------------------------------------------------------------------
// renderIPTVPlaylistSelectState
// Man hinh chon playlist khi co nhieu file .m3u
// ---------------------------------------------------------------------------
void UIManager::renderIPTVPlaylistSelectState() {
  drawRect(0, 0, UiTheme::APP_W, UiTheme::HEADER_H, UiTheme::FOOTER_BG, true);
  drawRect(0, UiTheme::HEADER_H - 1, UiTheme::APP_W, 1, UiTheme::FOOTER_LINE,
           true);
  {
    std::string tvLogoPath =
        AppConfig::instance().getAssetsDir() + "/apps_icons/TV.png";
    SDL_Texture *tvLogo = m_ui.getOrLoadImage("grid/TV.png", tvLogoPath);
    int titleX = 24;
    if (tvLogo) {
      int texW = 0, texH = 0;
      SDL_QueryTexture(tvLogo, nullptr, nullptr, &texW, &texH);
      int logoH = 40, logoW = 40;
      if (texH > 0)
        logoW = (texW * logoH) / texH;
      int slx = PlatformInfo::instance().scaleX(24);
      int sly =
          PlatformInfo::instance().scaleY((UiTheme::HEADER_H - logoH) / 2);
      int slw = PlatformInfo::instance().scaleW(logoW);
      int slh = PlatformInfo::instance().scaleH(logoH);
      SDL_Rect dst = {slx, sly, slw, slh};
      SDL_RenderCopy(m_renderer, tvLogo, nullptr, &dst);
      titleX = 24 + logoW + 10;
    }
    drawText("XEM TV - CHỌN PLAYLIST", titleX,
             textYCentered(0, UiTheme::HEADER_H, m_fontLarge),
             UiTheme::ACCENT_CYAN, m_fontLarge);
    m_ui.drawHeaderStatus();
  }

  const auto &playlists = IPTVManager::instance().getPlaylists();
  int plCount = static_cast<int>(playlists.size());

  int listY = 80;
  int itemH = 72;
  int visItems = 8;
  int itemW = 992;
  int itemX = 16;

  if (playlists.empty()) {
    drawText("Chưa có playlist nào.", 512, 340, {150, 160, 175, 255},
             m_fontMedium, true);
    drawText("Tải file .m3u qua Web Server (Cổng 8888)", 512, 385,
             {100, 110, 125, 255}, m_fontSmall, true);
  } else {
    for (int i = m_playlistScrollOffset;
         i < plCount && i < m_playlistScrollOffset + visItems; i++) {
      const Playlist &pl = playlists[static_cast<size_t>(i)];
      int y = listY + (i - m_playlistScrollOffset) * (itemH + 4);
      bool selected = (i == m_selectedPlaylistIndex);

      SDL_Color bg = selected ? UiTheme::FOCUS_BG : UiTheme::ROW_BG;
      drawRoundedRect(itemX, y, itemW, itemH, UiTheme::RADIUS_CARD, bg, true);
      if (selected)
        drawRoundedBorder(itemX, y, itemW, itemH, UiTheme::RADIUS_CARD,
                          UiTheme::ACCENT_CYAN, 2);

      // Index badge
      char numBuf[8];
      snprintf(numBuf, sizeof(numBuf), "%02d", i + 1);
      drawText(numBuf, itemX + 18, textYCentered(y, itemH, m_fontMedium),
               UiTheme::ACCENT_CYAN, m_fontMedium);

      // Row 1: "[ten] - ten file.m3u" (medium)
      // Row 2: "Đã làm mới" (small, chi voi URL playlist)
      // drawRowMainSub tu center ca block trong itemH (medium + small, gap 2)
      std::string plLine = pl.name;
      if (!pl.sourceFile.empty())
        plLine += " - " + pl.sourceFile;
      std::string refreshLine;
      const auto &srcs = IPTVManager::instance().getSources();
      for (const auto &s : srcs) {
        if (s.filename == pl.sourceFile && s.type == "url") {
          refreshLine = "Đã làm mới: " + s.lastRefreshedStr();
          break;
        }
      }
      drawRowMainSub(itemX + 68, y, itemH, plLine, m_fontMedium, refreshLine,
                     m_fontSmall, itemW - 260, 2);

      // Channel count badge on right (elastic, right-anchored)
      std::string cntText = std::to_string(pl.channelCount()) + " kênh";
      int cntW = badgeWidth(cntText, 28);
      int cntX = itemX + itemW - 20 - cntW;
      drawBadge(cntX, y + (itemH - 28) / 2, cntW, 28, cntText,
                {28, 42, 62, 255}, {147, 197, 253, 255});
      drawRoundedBorder(cntX, y + (itemH - 28) / 2, cntW, 28,
                        UiTheme::RADIUS_ROW, {59, 130, 246, 120}, 1);
    }

    // Scrollbar indicator
    if (plCount > visItems) {
      int sbH = 640;
      int sbX = 1010;
      int sbY = listY;
      drawRect(sbX, sbY, 4, sbH, {35, 45, 60, 255}, true);
      int thumbH = std::max(30, sbH * visItems / plCount);
      int thumbY = sbY + (sbH - thumbH) * m_playlistScrollOffset /
                             std::max(1, plCount - visItems);
      drawRect(sbX, thumbY, 4, thumbH, {0, 180, 216, 200}, true);
    }
  }

  // Footer chuan IPTV (drawAppFooter tu ve nen)
  drawAppFooter({{UiTheme::PadBtn::Y, "Tải lại"},
                 {UiTheme::PadBtn::X, "Đổi"},
                 {UiTheme::PadBtn::B, "Menu"}});
}

void UIManager::centerIptvGroupBar(const std::vector<std::string> &groups,
                                   const std::string &selected) {
  // Giu pill selected full-visible + co gang center, dung chung cach do
  // rong voi render (truncate + pillWidth), viewport [8, 824] (reserve 200px
  // phai cho count "2886 kênh yêu thích" + 1 space trong).
  const int VIEW_L = 8;
  const int VIEW_RIGHT = 1024 - 200;
  std::vector<int> widths;
  widths.reserve(groups.size());
  int totalW = 0;
  int selIdx = 0;
  for (size_t gi = 0; gi < groups.size(); gi++) {
    std::string label = groups[gi].empty() ? "Tất cả" : groups[gi];
    label = truncateToWidth(label, m_fontSmall,
                            UiTheme::PILL_MAX_W - UiTheme::PILL_PAD_X * 2);
    int gW = pillWidth(label, m_fontSmall);
    widths.push_back(gW);
    totalW += gW + UiTheme::PILL_GAP;
    if (groups[gi] == selected)
      selIdx = static_cast<int>(gi);
  }
  if (!widths.empty())
    totalW -= UiTheme::PILL_GAP;
  int maxScroll = totalW - (VIEW_RIGHT - VIEW_L);
  if (maxScroll < 0)
    maxScroll = 0;
  int selX = VIEW_L;
  for (int gi = 0; gi < selIdx && gi < static_cast<int>(widths.size()); gi++)
    selX += widths[gi] + UiTheme::PILL_GAP;
  int selW = widths.empty() ? 0 : widths[selIdx];
  // Center pill selected trong viewport, pin 2 bien khi tran.
  int viewW = VIEW_RIGHT - VIEW_L;
  int target = selX - VIEW_L - viewW / 2 + selW / 2;
  if (target < 0)
    target = 0;
  if (target > maxScroll)
    target = maxScroll;
  m_iptvGroupBarOffset = target;
}

void UIManager::renderIPTVState() {
  // Khi mpv đang phát + OSD đang hiện: mpv/mpv OSD chiếm framebuffer
  if (IPTVManager::instance().isIPTVPlaying() && m_iptvOsdVisible)
    return; // Để OSD xử lý

  // Khi mpv đang phát + OSD đang ẩn: Vẽ footer overlay cho playback
  if (IPTVManager::instance().isIPTVPlaying() && !m_iptvOsdVisible) {
    // Vẽ footer bar chuẩn cho trạng thái playback
    drawAppFooter({{UiTheme::PadBtn::A, "Phát/Tạm dừng"},
                   {UiTheme::PadBtn::B, "Thoát"},
                   {UiTheme::PadBtn::SELECT, "Chọn kênh"},
                   {UiTheme::PadBtn::DPAD, "Âm lượng"}});
    return;
  }

  // Layout: toàn màn hình khi chưa phát (isIPTVPlaying() == false đã return ở
  // trên rồi)
  const int SCREEN_H = 768;
  const int UI_TOP = 0;
  const int UI_H = SCREEN_H;

  // Nen toan man hinh
  drawRect(0, UI_TOP, 1024, UI_H, {14, 18, 26, 255}, true);

  // ─── Row 1: Header chuẩn chung + Logo TV + status bar (Wi-Fi, Pin, Giờ) ───
  drawRect(0, 0, UiTheme::APP_W, UiTheme::HEADER_H, UiTheme::FOOTER_BG, true);
  drawRect(0, UiTheme::HEADER_H - 1, UiTheme::APP_W, 1, UiTheme::FOOTER_LINE,
           true);
  {
    std::string tvLogoPath =
        AppConfig::instance().getAssetsDir() + "/apps_icons/TV.png";
    SDL_Texture *tvLogo = m_ui.getOrLoadImage("grid/TV.png", tvLogoPath);
    int titleX = 24;
    if (tvLogo) {
      int texW = 0, texH = 0;
      SDL_QueryTexture(tvLogo, nullptr, nullptr, &texW, &texH);
      int logoH = 40, logoW = 40;
      if (texH > 0)
        logoW = (texW * logoH) / texH;
      int slx = PlatformInfo::instance().scaleX(24);
      int sly =
          PlatformInfo::instance().scaleY((UiTheme::HEADER_H - logoH) / 2);
      int slw = PlatformInfo::instance().scaleW(logoW);
      int slh = PlatformInfo::instance().scaleH(logoH);
      SDL_Rect dst = {slx, sly, slw, slh};
      SDL_RenderCopy(m_renderer, tvLogo, nullptr, &dst);
      titleX = 24 + logoW + 10;
    }

    std::string header = UiStrings::IPTV_TITLE; // "XEM TV"
    if (m_activePlaylistIndex >= 0) {
      if (const Playlist *pl = IPTVManager::instance().getPlaylist(
              static_cast<size_t>(m_activePlaylistIndex))) {
        if (!pl->name.empty()) {
          std::string pName = truncateToWidth(pl->name, m_fontLarge, 380);
          header += " - " + pName;
        }
      }
    }
    if (m_iptvShowFavoritesOnly)
      header += " - YÊU THÍCH ★";

    header =
        truncateToWidth(header, m_fontLarge, UiTheme::APP_W - titleX - 300);
    drawText(header, titleX, textYCentered(0, UiTheme::HEADER_H, m_fontLarge),
             UiTheme::ACCENT_CYAN, m_fontLarge);
    m_ui.drawHeaderStatus();
  }
  int headerBottom = 64;

  // -----------------------------------------------------------------------
  // Build filtered channel list (dong bo voi input handler)
  // -----------------------------------------------------------------------
  std::vector<IPTVChannel> allChannels;
  if (m_iptvShowFavoritesOnly) {
    allChannels = IPTVManager::instance().getFavoriteChannels();
  } else if (m_activePlaylistIndex >= 0) {
    const Playlist *pl = IPTVManager::instance().getPlaylist(
        static_cast<size_t>(m_activePlaylistIndex));
    if (pl) {
      for (const auto &item : pl->channels)
        allChannels.push_back(
            IPTVChannel::fromItem(item, pl->name, pl->sourceFile));
    }
  } else {
    allChannels = IPTVManager::instance().getChannels();
  }

  // Ẩn kênh đã xác định mất kết nối (ping == -1). Kênh chưa đo (-2)
  // vẫn hiện bình thường, đo xong mà chết mới ẩn dần khi lướt list.
  size_t preFilterCount = allChannels.size();
  {
    std::vector<IPTVChannel> alive;
    alive.reserve(allChannels.size());
    for (const auto &ch : allChannels)
      if (IPTVManager::instance().getCachedPing(ch.url) != -1)
        alive.push_back(ch);
    allChannels.swap(alive);
  }
  size_t hiddenDeadCount = preFilterCount - allChannels.size();

  // Build group list
  std::vector<std::string> groups;
  groups.push_back(""); // Tất cả
  {
    std::unordered_set<std::string> seen;
    for (const auto &ch : allChannels) {
      if (!ch.group.empty() && seen.find(ch.group) == seen.end()) {
        seen.insert(ch.group);
        groups.push_back(ch.group);
      }
    }
    std::sort(groups.begin() + 1, groups.end());
  }

  // Apply group filter
  std::vector<IPTVChannel> channels;
  if (m_iptvSelectedGroup.empty()) {
    channels = allChannels;
  } else {
    for (const auto &ch : allChannels)
      if (ch.group == m_iptvSelectedGroup)
        channels.push_back(ch);
  }
  int channelCount = static_cast<int>(channels.size());

  // List có thể co lại khi ping resolve xong (kênh chết bị ẩn) → clamp
  if (channelCount == 0) {
    m_selectedIPTVChannelIndex = 0;
    m_iptvScrollOffset = 0;
  } else if (m_selectedIPTVChannelIndex >= channelCount) {
    m_selectedIPTVChannelIndex = channelCount - 1;
    if (m_selectedIPTVChannelIndex < m_iptvScrollOffset)
      m_iptvScrollOffset = m_selectedIPTVChannelIndex;
  }

  // -----------------------------------------------------------------------
  // Group filter bar (nam ngay duoi header)
  // -----------------------------------------------------------------------
  const int GROUP_BAR_Y = headerBottom;
  const int GROUP_BAR_H = UiTheme::GROUP_BAR_H;
  drawRect(0, GROUP_BAR_Y, 1024, GROUP_BAR_H, {20, 26, 38, 255}, true);
  drawRect(0, GROUP_BAR_Y + GROUP_BAR_H - 1, 1024, 1, {40, 48, 62, 255}, true);

  {
    // Cụm đếm góc phải: ● [sống] ● [chết] kênh.
    const int DOT_R = 8;
    const int DOT_GAP = 8;
    std::string aliveText = std::to_string(channelCount);
    std::string deadText;
    if (hiddenDeadCount > 0)
      deadText = std::to_string(hiddenDeadCount);
    const std::string unitText = "kênh";
    int aliveW = textWidth(aliveText, m_fontSmall);
    int unitW = textWidth(unitText, m_fontSmall);
    int countW = DOT_R * 2 + DOT_GAP + aliveW;
    int deadW = 0;
    if (!deadText.empty())
      deadW =
          DOT_GAP * 2 + DOT_R * 2 + DOT_GAP + textWidth(deadText, m_fontSmall);
    countW += deadW + DOT_GAP + unitW;
    const int COUNT_RIGHT = 1000;
    const int COUNT_GAP = 16;
    const int VIEW_RIGHT = COUNT_RIGHT - countW - COUNT_GAP;
    int gx = 8 - m_iptvGroupBarOffset;
    const int gH = UiTheme::PILL_H;
    const int gY = GROUP_BAR_Y + (GROUP_BAR_H - gH) / 2;

    // Scroll ngang dua tren m_iptvGroupBarOffset
    for (int gi = 0; gi < static_cast<int>(groups.size()); gi++) {
      const std::string &g = groups[gi];
      std::string label = g.empty() ? "Tất cả" : g;
      label = truncateToWidth(label, m_fontSmall,
                              UiTheme::PILL_MAX_W - UiTheme::PILL_PAD_X * 2);

      int gW = pillWidth(label, m_fontSmall);

      // Skip if before viewport
      if (gx + gW < 0) {
        gx += gW + UiTheme::PILL_GAP;
        continue;
      }
      // Stop nếu pill lấn vào vùng đếm góc phải (so theo MÉP PHẢI pill)
      if (gx + gW > VIEW_RIGHT)
        break;

      bool isActiveG = (g == m_iptvSelectedGroup);

      drawPill(gx, gY, gW, gH, label, isActiveG, m_fontSmall);

      gx += gW + UiTheme::PILL_GAP;
    }

    // Cụm đếm góc phải: chấm xanh + sống, chấm đỏ + chết
    if (true) {
      int th = m_fontSmall ? TTF_FontHeight(m_fontSmall) : 16;
      int cy = GROUP_BAR_Y + GROUP_BAR_H / 2;
      int textY = GROUP_BAR_Y + (GROUP_BAR_H - th) / 2;
      int dx = COUNT_RIGHT - countW;
      drawDot(dx + DOT_R, cy, DOT_R, {34, 197, 94, 255});
      dx += DOT_R * 2 + DOT_GAP;
      drawText(aliveText, dx, textY, UiTheme::TEXT_SUB, m_fontSmall);
      dx += aliveW;
      if (!deadText.empty()) {
        dx += DOT_GAP * 2;
        drawDot(dx + DOT_R, cy, DOT_R, {248, 113, 113, 255});
        dx += DOT_R * 2 + DOT_GAP;
        drawText(deadText, dx, textY, UiTheme::TEXT_SUB, m_fontSmall);
        dx += textWidth(deadText, m_fontSmall);
      }
      dx += DOT_GAP;
      drawText(unitText, dx, textY, UiTheme::TEXT_SUB, m_fontSmall);
    }
  }

  // -----------------------------------------------------------------------
  // Channel list
  // -----------------------------------------------------------------------
  const int LIST_TOP = GROUP_BAR_Y + GROUP_BAR_H + 2;
  const int LIST_BOTTOM = UiTheme::FOOTER_Y;
  const int LIST_H = LIST_BOTTOM - LIST_TOP;
  const int itemH = 64; // 2 dong: ten kenh + "- ten file"
  const int itemGap = 4;
  const int visibleItems = std::max(3, LIST_H / (itemH + itemGap));

  if (channels.empty()) {
    int midY = LIST_TOP + LIST_H / 2;
    if (m_iptvShowFavoritesOnly) {
      drawText("Chưa có kênh yêu thích nào.", 512, midY - 16,
               {150, 160, 175, 255}, m_fontMedium, true);
      drawInlineHintsCentered(
          "Bấm [X] trên danh sách kênh để đánh dấu yêu thích", 512, midY + 14,
          {100, 110, 125, 255}, m_fontSmall, 24);
    } else if (!m_iptvSelectedGroup.empty()) {
      drawText("Không có kênh trong nhóm \"" + m_iptvSelectedGroup + "\"", 512,
               midY, {150, 160, 175, 255}, m_fontMedium, true);
    } else if (hiddenDeadCount > 0 && preFilterCount > 0) {
      drawText("Các kênh đều mất kết nối.", 512, midY - 16,
               {248, 113, 113, 255}, m_fontMedium, true);
      drawText("Kiểm tra Wi-Fi rồi bấm [A] để đo lại", 512, midY + 14,
               {100, 110, 125, 255}, m_fontSmall, true);
    } else {
      drawText(UiStrings::IPTV_NO_CHANNELS, 512, midY - 16,
               {150, 160, 175, 255}, m_fontMedium, true);
      drawText("Vui lòng tải playlist (.m3u) qua Web Server (Cổng 8888)", 512,
               midY + 14, {100, 110, 125, 255}, m_fontSmall, true);
    }
  } else {
    // Do ping nền cho cửa sổ giới hạn: trang hiện tại + 2 trang kế
    // (tối đa 24 kênh). Worker ping đơn (không đẻ thread mỗi frame) nên
    // lướt list 10k kênh cũng không crash. Kênh ngoài cửa sổ đo khi tới.
    {
      constexpr int kPingWindow = 24;
      std::vector<std::string> pingUrls;
      pingUrls.reserve(kPingWindow);
      for (int i = m_iptvScrollOffset;
           i < channelCount && i < m_iptvScrollOffset + kPingWindow; i++) {
        pingUrls.push_back(channels[i].url);
      }
      IPTVManager::instance().prefetchPings(pingUrls);
    }
    for (int i = m_iptvScrollOffset;
         i < channelCount && i < m_iptvScrollOffset + visibleItems; i++) {
      int y = LIST_TOP + (i - m_iptvScrollOffset) * (itemH + itemGap);
      bool isSelected = (i == m_selectedIPTVChannelIndex);

      SDL_Color bg = isSelected ? UiTheme::FOCUS_BG : UiTheme::ROW_BG_IPTV;
      drawRoundedRect(8, y, 1008, itemH, UiTheme::RADIUS_ROW, bg, true);
      if (isSelected)
        drawRoundedBorder(8, y, 1008, itemH, UiTheme::RADIUS_ROW,
                          UiTheme::ACCENT_CYAN, 2);

      // So kenh + ten kenh can giua doc rieng theo font (fix Small/Medium chung
      // Y)
      int numY = textYCentered(y, itemH, m_fontSmall);
      int nameY = textYCentered(y, itemH, m_fontMedium);

      // Channel number
      char numBuf[8];
      snprintf(numBuf, sizeof(numBuf), "%02d", i + 1);
      drawText(numBuf, 22, numY, UiTheme::ACCENT_CYAN, m_fontSmall);

      // Channel name (1 dong, fontMedium)
      std::string chanName =
          truncateToWidth(channels[i].name, m_fontMedium, 560);
      drawText(chanName, 58, nameY, UiTheme::TEXT_MAIN, m_fontMedium);

      // Playing indicator
      if (false &&
          IPTVManager::instance().getCurrentChannelName() == channels[i].name) {
        drawText("● DANG PHAT", 420, textYCentered(y, itemH, m_fontSmall),
                 UiTheme::ACCENT_GREEN, m_fontSmall);
      }

      // Right side: favorite icon | group tag | ping
      // Icon dat o COT RIENG ben trai group title, group title KHONG shift
      // (giu cot co dinh "nhu cu")
      int badgeCY = y + (itemH - 24) / 2;
      const int ICON_SIZE = 26;
      const int FAV_X = 590; // cot rieng, ben trai group
      const int GRP_X = 624; // group title cot co dinh (nhu cu)
      const int GRP_MAX_W = 170;
      if (channels[i].isFavorite) {
        drawIcon("favorite", FAV_X, y + (itemH - ICON_SIZE) / 2, ICON_SIZE,
                 ICON_SIZE);
      }
      if (!channels[i].group.empty() && m_iptvSelectedGroup.empty()) {
        std::string grp =
            truncateToWidth(channels[i].group, m_fontSmall, GRP_MAX_W);
        drawText(grp, GRP_X, badgeCY, UiTheme::TEXT_SUB, m_fontSmall);
      }
      // Ping badge: hien toc do ket noi kenh
      int pingMs = IPTVManager::instance().getCachedPing(channels[i].url);
      std::string pingText;
      SDL_Color pingBg, pingFg;
      if (pingMs < -1) { // -2 = chua do/dang do
        pingText = "...";
        pingBg = {40, 48, 62, 255};
        pingFg = {150, 160, 175, 255};
      } else if (pingMs < 0) {
        pingText = "Mất kết nối";
        pingBg = {62, 28, 32, 255};
        pingFg = {248, 113, 113, 255};
      } else {
        pingText = std::to_string(pingMs) + " ms";
        if (pingMs < 300) {
          pingBg = {22, 62, 42, 255};
          pingFg = {74, 222, 128, 255};
        } else if (pingMs < 1000) {
          pingBg = {62, 52, 22, 255};
          pingFg = {250, 204, 21, 255};
        } else {
          pingBg = {62, 32, 22, 255};
          pingFg = {251, 146, 60, 255};
        }
      }
      int pingW = badgeWidth(pingText, 24);
      int pingX = 990 - pingW;
      drawBadge(pingX, badgeCY, pingW, 24, pingText, pingBg, pingFg);
    }

    // Scrollbar
    if (channelCount > visibleItems) {
      int sbH = LIST_H - 4;
      int sbX = 1014;
      drawRect(sbX, LIST_TOP + 2, 4, sbH, {35, 45, 60, 255}, true);
      int thumbH = std::max(20, sbH * visibleItems / channelCount);
      int thumbY = LIST_TOP + 2 +
                   (sbH - thumbH) * m_iptvScrollOffset /
                       std::max(1, channelCount - visibleItems);
      drawRect(sbX, thumbY, 4, thumbH, {0, 180, 216, 200}, true);
    }
  }

  // -----------------------------------------------------------------------
  // Footer (ve qua drawAppFooter — tu ve nen + chong tran)
  // -----------------------------------------------------------------------
  if (m_iptvShowFavoritesOnly) {
    drawAppFooter({{UiTheme::PadBtn::A, "Xem"},
                   {UiTheme::PadBtn::B, "Lùi"},
                   {UiTheme::PadBtn::X, "Thích"},
                   {UiTheme::PadBtn::SELECT, "Tìm kênh"},
                   {UiTheme::PadBtn::DPAD, "Nhóm"}});
  } else {
    drawAppFooter({{UiTheme::PadBtn::A, "Xem"},
                   {UiTheme::PadBtn::B, "Lùi"},
                   {UiTheme::PadBtn::X, "Thích"},
                   {UiTheme::PadBtn::Y, "Lọc"},
                   {UiTheme::PadBtn::SELECT, "Tìm kênh"},
                   {UiTheme::PadBtn::DPAD, "Nhóm"}});
  }
}

void UIManager::renderIPTVSearchState() {
  drawAppBackground();

  // ─── Row 1: Header (Header chuẩn chung + logo TV + status bar) ───
  drawRect(0, 0, UiTheme::APP_W, UiTheme::HEADER_H, UiTheme::FOOTER_BG, true);
  drawRect(0, UiTheme::HEADER_H - 1, UiTheme::APP_W, 1, UiTheme::FOOTER_LINE,
           true);
  {
    std::string tvLogoPath =
        AppConfig::instance().getAssetsDir() + "/apps_icons/TV.png";
    SDL_Texture *tvLogo = m_ui.getOrLoadImage("grid/TV.png", tvLogoPath);
    int titleX = 24;
    if (tvLogo) {
      int texW = 0, texH = 0;
      SDL_QueryTexture(tvLogo, nullptr, nullptr, &texW, &texH);
      int logoH = 40, logoW = 40;
      if (texH > 0)
        logoW = (texW * logoH) / texH;
      int slx = PlatformInfo::instance().scaleX(24);
      int sly =
          PlatformInfo::instance().scaleY((UiTheme::HEADER_H - logoH) / 2);
      int slw = PlatformInfo::instance().scaleW(logoW);
      int slh = PlatformInfo::instance().scaleH(logoH);
      SDL_Rect dst = {slx, sly, slw, slh};
      SDL_RenderCopy(m_renderer, tvLogo, nullptr, &dst);
      titleX = 24 + logoW + 10;
    }
    drawText("TÌM KIẾM KÊNH IPTV", titleX,
             textYCentered(0, UiTheme::HEADER_H, m_fontLarge),
             UiTheme::ACCENT_CYAN, m_fontLarge);
  }
  drawHeaderStatus();

  int numResults = static_cast<int>(m_iptvSearchResults.size());
  int inX = 46;
  int inY = 74;
  int inW = 932;
  int inH = 58;

  // ─── Row 2: Input Field Box ───
  std::string bgInputPath = AppConfig::instance().getAssetsDir() +
                            "/stock_keyboard/bg-search-input.png";
  SDL_Texture *bgInputTex =
      m_ui.getOrLoadImage("stock_keyboard/bg-search-input.png", bgInputPath);
  if (bgInputTex) {
    int slx = PlatformInfo::instance().scaleX(inX);
    int sly = PlatformInfo::instance().scaleY(inY);
    int slw = PlatformInfo::instance().scaleW(inW);
    int slh = PlatformInfo::instance().scaleH(inH);
    SDL_Rect dst = {slx, sly, slw, slh};
    SDL_RenderCopy(m_renderer, bgInputTex, nullptr, &dst);
  } else {
    drawRoundedRect(inX, inY, inW, inH, UiTheme::RADIUS_CARD, {12, 38, 50, 230},
                    true);
    drawRoundedBorder(inX, inY, inW, inH, UiTheme::RADIUS_CARD,
                      {26, 72, 92, 255}, 1);
  }

  std::string displayQuery = m_iptvVk.query.empty()
                                 ? "Nhập tên kênh (VTV, HBO, thể thao...)"
                                 : m_iptvVk.query + " _";
  displayQuery = truncateToWidth(displayQuery, m_fontLarge, inW - 48);
  SDL_Color qColor = m_iptvVk.query.empty() ? SDL_Color{75, 115, 135, 255}
                                            : SDL_Color{255, 255, 255, 255};
  drawText(displayQuery, inX + 24, inY + (inH - textHeight(m_fontLarge)) / 2,
           qColor, m_fontLarge, false);

  // ─── Row 3 / Middle: Channel Results List (Y = 144..440) ───
  if (m_iptvVk.query.empty()) {
    drawText("Nhập chữ cái trên bàn phím để tìm kiếm kênh.", 512, 230,
             {148, 163, 184, 255}, m_fontMedium, true);
    drawText(
        "Hỗ trợ gõ tiếng Việt Telex, tìm tức thì theo tên kênh hoặc thể loại.",
        512, 270, {100, 116, 139, 255}, m_fontSmall, true);
  } else if (numResults == 0) {
    drawText("Không tìm thấy kênh phù hợp với từ khóa.", 512, 250,
             {239, 68, 68, 255}, m_fontMedium, true);
  } else {
    int pageSize = 5;
    int itemH = 64; // giong renderIPTVState() de dong 2 khong bi cat
    int itemGap = 8;
    int listStartY = 144;

    std::vector<std::string> su;
    for (int i = 0; i < pageSize && (m_iptvSearchScrollOffset + i) < numResults;
         i++) {
      int idx = m_iptvSearchScrollOffset + i;
      su.push_back(m_iptvSearchResults[idx].url);
    }
    IPTVManager::instance().prefetchPings(su);

    for (int i = 0; i < pageSize && (m_iptvSearchScrollOffset + i) < numResults;
         i++) {
      int idx = m_iptvSearchScrollOffset + i;
      const auto &chan = m_iptvSearchResults[idx];
      bool isSel = (m_iptvVk.inResults && idx == m_iptvSearchSelectedIndex);

      int itemY = listStartY + i * (itemH + itemGap);
      SDL_Color rowBg =
          isSel ? SDL_Color{24, 48, 80, 255} : SDL_Color{18, 26, 38, 240};
      drawRoundedRect(inX, itemY, inW, itemH, UiTheme::RADIUS_CARD, rowBg,
                      true);
      if (isSel) {
        drawRoundedBorder(inX, itemY, inW, itemH, UiTheme::RADIUS_CARD,
                          SDL_Color{0, 180, 255, 255}, 2);
      } else {
        drawRoundedBorder(inX, itemY, inW, itemH, UiTheme::RADIUS_CARD,
                          SDL_Color{32, 46, 64, 255}, 1);
      }

      // Channel number (top line)
      char numBuf[16];
      snprintf(numBuf, sizeof(numBuf), "%02d", idx + 1);
      drawText(numBuf, inX + 16, textYCentered(itemY, itemH, m_fontSmall),
               UiTheme::ACCENT_CYAN, m_fontSmall);

      // Channel name (1 dong, fontMedium)
      std::string chanName =
          truncateToWidth(chan.name, m_fontMedium, inW - 380);
      drawText(chanName, inX + 55, textYCentered(itemY, itemH, m_fontMedium),
               UiTheme::TEXT_MAIN, m_fontMedium);

      // Favorite icon - COT RIENG ben trai group, group KHONG shift
      const int ICON_SIZE_S = 26;
      const int FAV_X_S = inX + inW - 235; // cot rieng (truoc group)
      const int GRP_X_S = inX + inW - 200; // group cot co dinh
      const int GRP_MAX_W_S = 130;
      if (chan.isFavorite) {
        drawIcon("favorite", FAV_X_S, itemY + (itemH - ICON_SIZE_S) / 2,
                 ICON_SIZE_S, ICON_SIZE_S);
      }
      if (!chan.group.empty()) {
        std::string grp = truncateToWidth(chan.group, m_fontSmall, GRP_MAX_W_S);
        drawText(grp, GRP_X_S, itemY + (itemH - 24) / 2, UiTheme::TEXT_SUB,
                 m_fontSmall);
      }

      // Ping badge
      int pingMs2 = IPTVManager::instance().getCachedPing(chan.url);
      std::string pingT2;
      SDL_Color pingBg2, pingFg2;
      if (pingMs2 < -1) {
        pingT2 = "...";
        pingBg2 = {40, 48, 62, 255};
        pingFg2 = {150, 160, 175, 255};
      } else if (pingMs2 < 0) {
        pingT2 = "Mất kết nối";
        pingBg2 = {62, 28, 32, 255};
        pingFg2 = {248, 113, 113, 255};
      } else {
        pingT2 = std::to_string(pingMs2) + " ms";
        if (pingMs2 < 300) {
          pingBg2 = {22, 62, 42, 255};
          pingFg2 = {74, 222, 128, 255};
        } else if (pingMs2 < 1000) {
          pingBg2 = {62, 52, 22, 255};
          pingFg2 = {250, 204, 21, 255};
        } else {
          pingBg2 = {62, 32, 22, 255};
          pingFg2 = {251, 146, 60, 255};
        }
      }
      int pingW2 = badgeWidth(pingT2, 28);
      int pingX2 = inX + inW - 10 - pingW2;
      drawBadge(pingX2, itemY + (itemH - 28) / 2, pingW2, 28, pingT2, pingBg2,
                pingFg2);
    }
  }

  // Divider line above keyboard
  drawRect(46, 444, 932, 1, {20, 48, 64, 180}, true);

  // ─── Bottom: Compact Virtual Keyboard (Y = 452..706) ───
  const char *iActs[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  {
    static std::string i0, i1, i2, i3, i4;
    i0 = m_iptvVk.shift ? "ABC" : "abc";
    i1 = m_iptvVk.telexMode ? "TELEX" : "US";
    i2 = "Cách";
    i3 = "Xóa";
    i4 = "Xem";
    iActs[0] = i0.c_str();
    iActs[1] = i1.c_str();
    iActs[2] = i2.c_str();
    iActs[3] = i3.c_str();
    iActs[4] = i4.c_str();
    VkState iDraw = m_iptvVk;
    if (m_iptvVk.inResults)
      iDraw.row = -1;
    m_ui.drawVirtualKeyboard(iDraw, 46, 452, 86, 46, 8, 6,
                             SDL_Color{0, 140, 230, 255},
                             SDL_Color{0, 180, 255, 255}, iActs, true, true, 2);
  }

  // ─── Bottom Bar chuẩn: drawAppFooter ───
  if (m_iptvVk.inResults) {
    drawAppFooter({{UiTheme::PadBtn::A, "Xem"},
                   {UiTheme::PadBtn::B, "Bàn phím"},
                   {UiTheme::PadBtn::X, "Thích"},
                   {UiTheme::PadBtn::UPDOWN, "Chọn"}});
  } else {
    drawAppFooter({{UiTheme::PadBtn::A, "Nhập"},
                   {UiTheme::PadBtn::B, "Thoát"},
                   {UiTheme::PadBtn::X, "Cách"},
                   {UiTheme::PadBtn::Y, "Xóa"},
                   {UiTheme::PadBtn::R1, "Telex"},
                   {UiTheme::PadBtn::START, "Tìm kênh"}});
  }
}

void UIManager::render() {
  // Khi IPTV dang phat: khong render SDL de tranh conflict framebuffer voi mpv.
  // mpv so huu man hinh, channel list hien qua mpv OSD native (IPC show-text).
  if (IPTVManager::instance().isIPTVPlaying() &&
      m_currentState == UIState::IPTV_LIST) {
    return;
  }

  SDL_SetRenderDrawColor(m_renderer, UiTheme::BG_APP.r, UiTheme::BG_APP.g,
                         UiTheme::BG_APP.b, 255);
  SDL_RenderClear(m_renderer);

  // Fullscreen đồng hồ: chỉ số trên nền đen, bỏ mọi chrome.
  // Màn tĩnh thì nghỉ 90ms thay vì present 60fps (đỡ nóng máy, đỡ tốn pin).
  if (m_clkFullscreen && m_currentState == UIState::WEATHER && m_wxTab == 1) {
    if (clockFrameNeeded()) {
      renderClockFullscreen();
      SDL_RenderPresent(m_renderer);
    } else {
      SDL_Delay(90);
    }
    return;
  }

  renderHeader();

  switch (m_currentState) {
  case UIState::MENU:
    renderMenuState();
    break;
  case UIState::SYSTEM_SELECT:
    renderSystemSelectState();
    break;
  case UIState::GAME_LIST:
    renderGameListState();
    break;
  case UIState::SEARCH:
    renderSearchState();
    break;
  case UIState::CONFIRM_DELETE:
    renderGameListState();
    renderConfirmDeleteDialog();
    break;
  case UIState::CONFIRM_BATCH_DELETE:
    renderGameListState();
    renderConfirmBatchDeleteDialog();
    break;
  case UIState::DISCLAIMER:
    renderDisclaimerState();
    break;
  case UIState::CLOUD_LOGIN:
    renderCloudLoginState();
    break;
  case UIState::SETTINGS:
    renderSettingsState();
    break;
  case UIState::DIAGNOSTICS:
    renderDiagnosticsState();
    break;
  // P5: UIState::OTA_UPDATE không còn được set nữa (OTA là tab 1 trong
  // Settings, dispatch tới renderSettingsState ở trên). Enum giữ để tránh
  // breaking change.
  case UIState::OTA_CHANGELOG:
    renderOTAChangelogState();
    break;
  case UIState::REVERSE_SYNC:
    renderReverseSyncState();
    break;
  case UIState::IPTV_PLAYLIST_SELECT:
    renderIPTVPlaylistSelectState();
    break;
  case UIState::IPTV_LIST:
    renderIPTVState();
    break;
  case UIState::IPTV_SEARCH:
    renderIPTVSearchState();
    break;
  case UIState::YOUTUBE_SEARCH:
    renderYouTubeSearchState();
    break;
  case UIState::YOUTUBE_HOME:
    renderYouTubeHomeState();
    break;
  case UIState::YOUTUBE_RESULTS:
    renderYouTubeResultsState();
    break;
  case UIState::LOCALSEND_HOME:
    renderLocalSendHome();
    break;
  case UIState::LOCALSEND_INCOMING:
    if (m_localSendIncomingMode == 1) {
      renderLocalSendFolderPicker();
    } else {
      renderLocalSendHome();
      renderLocalSendIncomingDialog();
    }
    break;
  case UIState::LOCALSEND_FOLDER:
    renderLocalSendFolderPicker();
    break;
  case UIState::LOCALSEND_SEND:
    renderLocalSendSendPicker();
    break;
  case UIState::LOCALSEND_GAME_PICKER:
    renderLocalSendGamePicker();
    break;
  case UIState::LOCALSEND_PROGRESS:
    renderLocalSendProgress();
    break;
  case UIState::FILE_EXPLORER:
    renderFileExplorer();
    renderConfirmDialogFromState();
    break;
  case UIState::DEST_PICKER:
    renderLocalSendFolderPicker();
    renderConfirmDialogFromState();
    break;
  case UIState::TEXT_VIEWER:
    renderTextViewer();
    break;
  case UIState::CLOCK_ALARM:
    renderAlarmRing();
    break;
  case UIState::WEATHER:
    renderWeather();
    break;
  case UIState::GAME_CAST:
    renderGameCastState();
    break;
  case UIState::PORTAL:
    renderPortal();
    break;
  default:
    break;
  }

  renderFooter();
  renderToast();
  renderProgressDialogFromState();
  renderSyncOverlay();
  renderUploadOverlay();
  SDL_RenderPresent(m_renderer);
}

static std::vector<std::string> split(const std::string &s, char delim) {
  std::vector<std::string> parts;
  std::stringstream ss(s);
  std::string token;
  while (std::getline(ss, token, delim))
    parts.push_back(token);
  return parts;
}

static std::string formatDuration(const std::string &raw) {
  if (raw.empty() || raw == "NA" || raw == "None")
    return "";
  try {
    long sec = std::stol(raw);
    if (sec <= 0)
      return "";
    long h = sec / 3600;
    long m = (sec % 3600) / 60;
    long s = sec % 60;
    char buf[32];
    if (h > 0) {
      snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld", h, m, s);
    } else {
      snprintf(buf, sizeof(buf), "%ld:%02ld", m, s);
    }
    return buf;
  } catch (...) {
    return raw;
  }
}

// P0-3: Lấy ký tự UTF-8 đầu tiên (1 grapheme approx = 1 code point đầu).
// Return "" nếu input rỗng.
static std::string utf8FirstChar(const std::string &s) {
  if (s.empty())
    return "";
  unsigned char c = static_cast<unsigned char>(s[0]);
  size_t n = 1;
  if ((c & 0x80) == 0)
    n = 1;
  else if ((c & 0xE0) == 0xC0)
    n = 2;
  else if ((c & 0xF0) == 0xE0)
    n = 3;
  else if ((c & 0xF8) == 0xF0)
    n = 4;
  if (n > s.size())
    n = s.size();
  return s.substr(0, n);
}

static std::string formatViews(const std::string &raw) {
  if (raw.empty() || raw == "NA" || raw == "None")
    return "";
  try {
    long long views = std::stoll(raw);
    if (views <= 0)
      return "";
    if (views >= 1000000) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%.1fM lượt xem", views / 1000000.0);
      return buf;
    } else if (views >= 1000) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%.1fK lượt xem", views / 1000.0);
      return buf;
    }
    return std::to_string(views) + " lượt xem";
  } catch (...) {
    return raw;
  }
}

static std::mutex s_ytStreamMutex;

void UIManager::loadYouTubeHistory() {
  if (!m_ytSearchHistory.empty())
    return;
  std::string appRoot = AppConfig::instance().getAppRoot();
  if (appRoot.empty())
    appRoot = "/mnt/SDCARD/Apps/RomCloud";
  std::string histFile = appRoot + "/config/yt_history.txt";
  std::ifstream file(histFile);
  if (file.is_open()) {
    std::string line;
    while (std::getline(file, line)) {
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
      if (!line.empty()) {
        m_ytSearchHistory.push_back(line);
        if (m_ytSearchHistory.size() >= 8)
          break;
      }
    }
  }
  if (m_ytSearchHistory.empty()) {
    m_ytSearchHistory = {"Nhac tre remix", "Karaoke bolero",
                         "Tin tức 24h",    "Review phim",
                         "Hài sitcom",     "Bóng đá highlights",
                         "Phim hoạt hình", "Công nghệ mới"};
  }
}

void UIManager::saveYouTubeHistory(const std::string &query) {
  if (query.empty())
    return;
  auto it =
      std::remove(m_ytSearchHistory.begin(), m_ytSearchHistory.end(), query);
  m_ytSearchHistory.erase(it, m_ytSearchHistory.end());
  m_ytSearchHistory.insert(m_ytSearchHistory.begin(), query);
  while (m_ytSearchHistory.size() > 8) {
    m_ytSearchHistory.pop_back();
  }
  std::string appRoot = AppConfig::instance().getAppRoot();
  if (appRoot.empty())
    appRoot = "/mnt/SDCARD/Apps/RomCloud";
  std::string configDir = appRoot + "/config";
  mkdir(configDir.c_str(), 0755);
  std::string histFile = configDir + "/yt_history.txt";
  std::ofstream file(histFile);
  if (file.is_open()) {
    for (const auto &q : m_ytSearchHistory) {
      file << q << "\n";
    }
  }
}

void UIManager::getYouTubeHistoryPills(std::vector<YtHistoryPill> &row3,
                                       std::vector<YtHistoryPill> &row4) {
  row3.clear();
  row4.clear();
  int totalItems = std::min(8, static_cast<int>(m_ytSearchHistory.size()));
  if (totalItems == 0)
    return;

  const int maxRowW = 932;
  const int gap = 10;

  int wAll = 0;
  for (int i = 0; i < totalItems; i++) {
    int tw = textWidth(m_ytSearchHistory[i], m_fontSmall) + 28;
    if (tw > 280)
      tw = 280;
    if (tw < 60)
      tw = 60;
    wAll += tw + (i > 0 ? gap : 0);
  }

  if (totalItems <= 4 && wAll <= maxRowW) {
    for (int i = 0; i < totalItems; i++) {
      int tw = textWidth(m_ytSearchHistory[i], m_fontSmall) + 28;
      if (tw > 280)
        tw = 280;
      if (tw < 60)
        tw = 60;
      row3.push_back({i, m_ytSearchHistory[i], tw});
    }
  } else {
    int half = (totalItems + 1) / 2;
    int curW = 0;
    for (int i = 0; i < totalItems; i++) {
      int tw = textWidth(m_ytSearchHistory[i], m_fontSmall) + 28;
      if (tw > 280)
        tw = 280;
      if (tw < 60)
        tw = 60;
      if ((static_cast<int>(row3.size()) < half ||
           (curW + gap + tw <= maxRowW && row4.empty() && i < 4)) &&
          curW + (row3.empty() ? 0 : gap) + tw <= maxRowW) {
        if (!row3.empty())
          curW += gap;
        curW += tw;
        row3.push_back({i, m_ytSearchHistory[i], tw});
      } else {
        row4.push_back({i, m_ytSearchHistory[i], tw});
      }
    }
  }
}

std::vector<std::string> UIManager::runYouTubeSearch(const std::string &query,
                                                     int page) {
  std::vector<std::string> results;
  if (query.empty())
    return results;

  // P0-3: Channel-aware smart_search via youtube_search.py
  // - Detect nếu query match channel name (Vietnamese diacritics-insensitive)
  //   → in CHANNEL|<name>|<id>|<subs>|<vcount> + latest videos từ channel
  // - Nếu query bắt đầu "channel:<id>" → direct fetch videos từ channel đó
  // - Else → fallback video search thường

  std::string appRoot = AppConfig::instance().getAppRoot();
  if (appRoot.empty())
    appRoot = "/mnt/SDCARD/Apps/RomCloud";

  std::string escapedQuery;
  for (char c : query) {
    if (c == '"' || c == '\\' || c == '$' || c == '`')
      escapedQuery += '\\';
    escapedQuery += c;
  }

  // Compute max results: page * per_page (page starts at 1)
  int maxResults = page * 24;
  if (maxResults < 24)
    maxResults = 24;
  if (maxResults > 50)
    maxResults = 50; // Hard cap

  std::string cmd = "LD_LIBRARY_PATH=/mnt/SDCARD/System/lib:$LD_LIBRARY_PATH "
                    "SSL_CERT_FILE=/mnt/SDCARD/System/lib/python3.11/"
                    "site-packages/pip/_vendor/certifi/cacert.pem "
                    "/mnt/SDCARD/System/bin/python3 \"" +
                    appRoot + "/scripts/youtube_search.py\" smart \"" +
                    escapedQuery + "\" " + std::to_string(maxResults) +
                    " 2>/tmp/yt_search_err.log";
  Logger::info("[YouTube] Smart search (P0-3): query='" + query +
               "', maxResults=" + std::to_string(maxResults));

  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    Logger::error("[YouTube] popen failed for smart_search");
    return results;
  }

  char buffer[2048];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    std::string line(buffer);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
      line.pop_back();
    }
    if (line.empty())
      continue;
    if (line.find("ERROR:") == 0 || line.find("WARNING:") == 0)
      continue;
    // CHANNEL marker: CHANNEL|name|id|subs|vcount (5 pipes)
    // Video line: id|title|channel|duration|views (4 pipes)
    int pipeCount = std::count(line.begin(), line.end(), '|');
    if (line.compare(0, 8, "CHANNEL|") == 0) {
      // CHANNEL marker (5 pipes = name|id|subs|vcount + 1 prefix)
      if (pipeCount >= 4) {
        results.push_back(line);
      }
    } else if (pipeCount >= 4) {
      results.push_back(line);
    }
  }
  pclose(pipe);
  if (results.empty()) {
    std::string err = ytErrTail("/tmp/yt_search_err.log");
    Logger::warn("[YouTube] smart search empty, query='" + query + "'" +
                 (err.empty() ? "" : " err=" + err));
  }
  Logger::info("[YouTube] Smart search returned " +
               std::to_string(results.size()) + " lines");
  return results;
}

std::string UIManager::resolveYouTubeStreamUrl(const std::string &videoId,
                                                 const std::string &quality) {
  if (videoId.empty())
    return "";
  std::string key = videoId + "|" + quality;

  {
    std::lock_guard<std::mutex> lock(s_ytStreamMutex);
    auto it = m_ytStreamUrlCache.find(key);
    if (it != m_ytStreamUrlCache.end() && !it->second.empty()) {
      Logger::info("[YouTube] Using cached stream URL for: " + key);
      return it->second;
    }
  }

  std::string appRoot = AppConfig::instance().getAppRoot();
  if (appRoot.empty())
    appRoot = "/mnt/SDCARD/Apps/RomCloud";
  std::string scriptPath = appRoot + "/scripts/youtube_search.sh";
  std::string ytdl = appRoot + "/bin/yt-dlp";
  std::string ytdlGlibc = appRoot + "/bin/yt-dlp-glibc";

  if (access(scriptPath.c_str(), X_OK) != 0 ||
      (access(ytdl.c_str(), X_OK) != 0 &&
       access(ytdlGlibc.c_str(), X_OK) != 0)) {
    Logger::warn("[YouTube] YouTube dependencies missing, auto-repairing...");
    UpdateManager::instance().checkAndInstallDependencies();
  }

  // 360p android (~2s, progressive sẵn audio) phát nhanh; 720p DASH nét
  // hơn cho auto-upgrade / SELECT (resolve ngầm, không chặn phát).
  std::string cmd = "\"" + scriptPath + "\" url \"" + videoId + "\" " +
                    (quality.empty() ? "360" : quality) + " 2>/dev/null";

  Logger::info("[YouTube] Resolving video URL: " + cmd);
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    Logger::error("[YouTube] popen failed for url resolver");
    return "";
  }

  char buffer[4096];
  // -g co the tra 1 URL (progressive, co san audio) hoac 2 URL
  // (DASH video-only + audio). Giua ca 2 bang '|' de phat qua --audio-file.
  std::string videoStreamUrl, audioStreamUrl;
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    std::string line(buffer);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
      line.pop_back();
    }
    if (line.find("http://") == 0 || line.find("https://") == 0) {
      if (videoStreamUrl.empty())
        videoStreamUrl = line;
      else if (audioStreamUrl.empty()) {
        audioStreamUrl = line;
        break;
      }
    }
  }
  std::string streamUrl = videoStreamUrl;
  if (!audioStreamUrl.empty())
    streamUrl += "|" + audioStreamUrl;
  pclose(pipe);

  if (!streamUrl.empty()) {
    std::lock_guard<std::mutex> lock(s_ytStreamMutex);
    m_ytStreamUrlCache[key] = streamUrl;
  }
  return streamUrl;
}

std::string UIManager::cachedStreamUrl(const std::string &videoId,
                                       const std::string &quality) {
  if (videoId.empty() || quality.empty())
    return "";
  std::lock_guard<std::mutex> lock(s_ytStreamMutex);
  auto it = m_ytStreamUrlCache.find(videoId + "|" + quality);
  if (it != m_ytStreamUrlCache.end())
    return it->second;
  return "";
}

void UIManager::cacheStreamUrl(const std::string &videoId,
                               const std::string &quality,
                               const std::string &url) {
  if (videoId.empty() || quality.empty() || url.empty())
    return;
  std::lock_guard<std::mutex> lock(s_ytStreamMutex);
  m_ytStreamUrlCache[videoId + "|" + quality] = url;
}

void UIManager::preloadYouTubeStreamUrl(const std::string &videoId) {
  if (videoId.empty() || m_resolveTask.isRunning())
    return; // đang resolve dở: run() sẽ join block UI nên bỏ qua
  {
    std::lock_guard<std::mutex> lock(s_ytStreamMutex);
    if (m_ytStreamUrlCache.find(videoId + "|360") != m_ytStreamUrlCache.end())
      return;
  }
  m_resolveTask.run([this, videoId](TaskProgress &) {
    resolveYouTubeStreamUrl(videoId, "360");
  });
}

void UIManager::clearThumbnailCache() {
  m_ytThumbCache.clear();
  // P0-2: Drain & free all surfaces in ready queue to prevent RAM leaks
  std::lock_guard<std::mutex> lk(m_thumbReadyMtx);
  while (!m_thumbReady.empty()) {
    if (m_thumbReady.front().surf)
      SDL_FreeSurface(m_thumbReady.front().surf);
    m_thumbReady.pop();
  }
  std::lock_guard<std::mutex> qlk(m_thumbQueueMtx);
  m_thumbPendingDecode.clear();
  m_thumbEnqueued.clear();
#ifdef __GLIBC__
  malloc_trim(0);
#endif
}

// =====================================================================
// P0-2: Thumbnail decode worker thread
// =====================================================================
void UIManager::startThumbWorker() {
  if (m_thumbWorkerStarted.exchange(true))
    return;
  m_thumbWorkerStop = false;
  m_thumbWorker = std::thread([this]() {
    while (true) {
      std::pair<std::string, std::string> job; // vid, path
      {
        std::unique_lock<std::mutex> lk(m_thumbQueueMtx);
        m_thumbCv.wait(lk, [this]() {
          return m_thumbWorkerStop.load() || !m_thumbPendingDecode.empty();
        });
        if (m_thumbWorkerStop.load() && m_thumbPendingDecode.empty())
          return;
        job = std::move(m_thumbPendingDecode.front());
        m_thumbPendingDecode.pop_front();
      }
      // Check file validity & size (> 1KB) off-thread
      struct stat st;
      SDL_Surface *surf = nullptr;
      if (stat(job.second.c_str(), &st) == 0 && st.st_size > 1000) {
        surf = IMG_Load(job.second.c_str());
      }
      {
        std::lock_guard<std::mutex> lk(m_thumbReadyMtx);
        if (surf) {
          if (m_thumbReady.size() < 32) {
            m_thumbReady.push({job.first, surf});
          } else {
            SDL_FreeSurface(surf);
          }
        }
      }
      {
        std::lock_guard<std::mutex> lk(m_thumbQueueMtx);
        m_thumbEnqueued.erase(job.first);
      }
    }
  });
}

void UIManager::stopThumbWorker() {
  if (!m_thumbWorkerStarted.exchange(false))
    return;
  m_thumbWorkerStop = true;
  m_thumbCv.notify_all();
  if (m_thumbWorker.joinable())
    m_thumbWorker.join();
  // Drain ready queue + free surfaces
  std::lock_guard<std::mutex> lk(m_thumbReadyMtx);
  while (!m_thumbReady.empty()) {
    if (m_thumbReady.front().surf)
      SDL_FreeSurface(m_thumbReady.front().surf);
    m_thumbReady.pop();
  }
  std::lock_guard<std::mutex> qlk(m_thumbQueueMtx);
  while (!m_thumbPendingDecode.empty())
    m_thumbPendingDecode.pop_front();
  m_thumbEnqueued.clear();
}

void UIManager::enqueueThumbDecode(const std::string &vid,
                                   const std::string &path, bool priority) {
  if (vid.empty() || m_ytThumbCache.contains(vid))
    return;
  {
    std::lock_guard<std::mutex> lk(m_thumbQueueMtx);
    if (m_thumbEnqueued.count(vid))
      return;
    if (m_thumbPendingDecode.size() >= 40) {
      if (priority) {
        m_thumbEnqueued.erase(m_thumbPendingDecode.back().first);
        m_thumbPendingDecode.pop_back();
      } else {
        return;
      }
    }
    m_thumbEnqueued.insert(vid);
    if (priority) {
      m_thumbPendingDecode.push_front({vid, path});
    } else {
      m_thumbPendingDecode.push_back({vid, path});
    }
  }
  m_thumbCv.notify_one();
}

void UIManager::drainReadyThumbs() {
  // Main thread only — uploads + frees surfaces
  std::queue<ReadyThumb> batch;
  {
    std::lock_guard<std::mutex> lk(m_thumbReadyMtx);
    batch.swap(m_thumbReady);
  }
  while (!batch.empty()) {
    auto &r = batch.front();
    if (r.surf && !m_ytThumbCache.contains(r.vid)) {
      SDL_Texture *tex = SDL_CreateTextureFromSurface(m_renderer, r.surf);
      int iw = r.surf->w, ih = r.surf->h;
      SDL_FreeSurface(r.surf);
      if (tex)
        m_ytThumbCache.put(r.vid, tex, iw, ih);
    } else if (r.surf) {
      SDL_FreeSurface(r.surf);
    }
    batch.pop();
  }
}

void UIManager::startThumbnailDownloads(
    const std::vector<std::string> &videoIds) {
  if (videoIds.empty())
    return;
  // Worker còn bận lô cũ: BỎ QUA lô mới. BackgroundTask::run() sẽ join()
  // thread cũ -> block main thread hàng chục giây (8 ảnh x curl tối đa
  // 4s), UI đơ toàn tập dù kết quả đã hiện. Thumb chỉ là trang trí:
  // lô sau bù lại khi user chuyển trang/tìm mới, ô thiếu hiện placeholder.
  if (m_thumbTask.isRunning())
    return;
  m_thumbTask.run([videoIds](TaskProgress &) {
    mkdir("/tmp/yt_thumbs", 0777);
    DIR *dir = opendir("/tmp/yt_thumbs");
    if (dir) {
      std::vector<std::string> files;
      struct dirent *ent;
      while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] != '.')
          files.push_back(std::string("/tmp/yt_thumbs/") + ent->d_name);
      }
      closedir(dir);
      if (files.size() > 32) {
        for (size_t i = 24; i < files.size(); ++i) {
          unlink(files[i].c_str());
        }
      }
    }
    // Limit to at most 8 items to prevent CPU/memory spikes
    size_t limit = std::min<size_t>(videoIds.size(), 8);
    for (size_t i = 0; i < limit; ++i) {
      const auto &vid = videoIds[i];
      if (vid.empty())
        continue;
      std::string outPath = "/tmp/yt_thumbs/" + vid + ".jpg";
      struct stat st;
      if (stat(outPath.c_str(), &st) == 0 && st.st_size > 1000)
        continue;
      std::string tmpPath = outPath + ".tmp";
      std::string url = "https://i.ytimg.com/vi/" + vid + "/mqdefault.jpg";
      std::string cmd =
          "curl -4 -k -s -L --connect-timeout 2 --max-time 4 -o \"" + tmpPath +
          "\" \"" + url + "\" && mv -f \"" + tmpPath + "\" \"" + outPath +
          "\" >/dev/null 2>&1";
      system(cmd.c_str());
    }
#ifdef __GLIBC__
    malloc_trim(0);
#endif
  });
}

void UIManager::triggerYouTubeSearch() {
  if (m_ytVk.query.empty()) {
    showToast("Vui lòng nhập từ khóa tìm kiếm", {245, 158, 11, 255}, 2000);
    return;
  }
  if (m_ytIsSearching)
    return;

  m_ytCurrentPage = 1;
  m_ytLastSearchQuery = m_ytVk.query;
  m_ytAllCachedResults.clear();
  clearThumbnailCache();
  saveYouTubeHistory(m_ytVk.query);

  m_ytIsSearching = true;
  m_ytSearchStartMs = SDL_GetTicks();
  m_ytSearchFinished = false;
  m_ytErrorMessage.clear();
  std::string query = m_ytVk.query;

  m_ytSearchTask.run([this, query](TaskProgress &) {
    auto results = runYouTubeSearch(query, 1);
    {
      std::lock_guard<std::mutex> lk(m_ytSearchMutex);
      m_ytPendingSearchResults = std::move(results);
      m_ytSearchDataReady = true;
    }
  });
}

void UIManager::playYouTubeVideo(const std::string &videoId) {
  if (videoId.empty() || m_ytIsLoadingVideo)
    return;

  // Tiêu đề cho OSD top bar: tìm trong kết quả search (không đổi logic search)
  std::string videoTitle;
  for (const auto *vec : {&m_ytItems, &m_ytAllItems}) {
    for (const auto &it : *vec) {
      if (it.type == YtItem::Type::Video && it.id == videoId &&
          !it.title.empty()) {
        videoTitle = it.title;
        break;
      }
    }
    if (!videoTitle.empty())
      break;
  }
  m_ytPendingVideoTitle = videoTitle;

  // Cache: ưu tiên bản nét đã resolve (replay), rồi tới bản nhanh 360p.
  std::string hit = cachedStreamUrl(videoId, "720");
  std::string hitQ = "720";
  if (hit.empty()) { hit = cachedStreamUrl(videoId, "360"); hitQ = "360"; }
  if (!hit.empty()) {
    m_ytPendingVideoId = videoId;
    m_ytPendingStreamUrl = hit;
    m_ytPendingQuality = hitQ;
    m_ytVideoReady = true;
    return;
  }

  m_ytIsLoadingVideo = true;
  m_ytVideoReady = false;
  m_ytPendingStreamUrl.clear();
  m_ytPendingVideoId = videoId;
  m_ytPendingQuality = "720"; // mặc định 720, rớt mới xuống 360
  m_ytLoadStartMs = SDL_GetTicks();
  m_ytLoadToastMs = 0; // update() toast ngay frame tới
  showToast("Đang tải video...", UiTheme::ACCENT_CYAN, 4000);

  m_resolveTask.run([this, videoId](TaskProgress &) {
    // Mặc định 720; chỉ khi 720 không resolve được mới dùng 360.
    std::string streamUrl = resolveYouTubeStreamUrl(videoId, "720");
    std::string q = "720";
    if (streamUrl.empty()) {
      streamUrl = resolveYouTubeStreamUrl(videoId, "360");
      if (!streamUrl.empty()) q = "360";
    }
    m_ytPendingQuality = q;
    m_ytPendingStreamUrl = streamUrl;
    m_ytVideoReady = true;
  });
}

static std::string trimUtf8(const std::string &str) {
  if (str.empty())
    return "";
  size_t start = 0;
  while (start < str.size()) {
    unsigned char c = static_cast<unsigned char>(str[start]);
    if (c <= 32) {
      start++;
    } else if (c == 0xC2 && start + 1 < str.size() &&
               static_cast<unsigned char>(str[start + 1]) == 0xA0) {
      // Non-breaking space \u00A0
      start += 2;
    } else if (c == 0xE2 && start + 2 < str.size()) {
      // Check for \u200B..\u200F or \u2068..\u2069 or \uFEFF
      unsigned char b1 = static_cast<unsigned char>(str[start + 1]);
      unsigned char b2 = static_cast<unsigned char>(str[start + 2]);
      if (b1 == 0x80 && (b2 >= 0x8B && b2 <= 0x8F)) {
        start += 3;
      } else if (b1 == 0x81 && (b2 >= 0xA6 && b2 <= 0xA9)) {
        start += 3;
      } else {
        break;
      }
    } else {
      break;
    }
  }
  size_t end = str.size();
  while (end > start) {
    unsigned char c = static_cast<unsigned char>(str[end - 1]);
    if (c <= 32) {
      end--;
    } else {
      break;
    }
  }
  return str.substr(start, end - start);
}

static std::pair<std::string, std::string>
wrapUtf8TwoLines(const std::string &text, size_t maxCharsPerLine) {
  std::string cleanText = trimUtf8(text);
  auto chars = TelexHelper::splitUtf8(cleanText);
  if (chars.empty())
    return {"", ""};

  // Filter out unrenderable 4-byte emojis and control codes that cause square
  // box glyphs
  std::vector<std::string> cleanChars;
  for (const auto &c : chars) {
    if (c.empty())
      continue;
    unsigned char b0 = static_cast<unsigned char>(c[0]);
    if (b0 < 32)
      continue;
    if (b0 >= 0xF0)
      continue; // 4-byte emojis (often missing in handheld TTF)
    cleanChars.push_back(c);
  }

  if (cleanChars.size() <= maxCharsPerLine) {
    std::string l1;
    for (const auto &c : cleanChars)
      l1 += c;
    return {trimUtf8(l1), ""};
  }

  // Try to word-wrap at a space
  size_t breakIdx = maxCharsPerLine;
  for (size_t i = maxCharsPerLine; i > maxCharsPerLine / 2; --i) {
    if (cleanChars[i] == " ") {
      breakIdx = i;
      break;
    }
  }

  std::string l1;
  for (size_t i = 0; i < breakIdx; ++i)
    l1 += cleanChars[i];

  size_t start2 = (breakIdx < cleanChars.size() && cleanChars[breakIdx] == " ")
                      ? breakIdx + 1
                      : breakIdx;
  std::string l2;
  size_t count2 = cleanChars.size() - start2;
  if (count2 <= maxCharsPerLine) {
    for (size_t i = start2; i < cleanChars.size(); ++i)
      l2 += cleanChars[i];
  } else {
    size_t end2 = start2 + maxCharsPerLine - 1;
    for (size_t i = start2; i < end2 && i < cleanChars.size(); ++i)
      l2 += cleanChars[i];
    l2 += "..";
  }

  return {trimUtf8(l1), trimUtf8(l2)};
}

// ============================================================================
// CHUẨN HOÁ LAYOUT THUMBNAIL VIDEO YOUTUBE
// ----------------------------------------------------------------------------
// Layout duy nhất áp dụng cho MỌI màn có thumbnail video:
//   ┌───────────────────────────────────────┐
//   │                                       │
//   │          [THUMBNAIL]                  │   Ảnh thuần, không banner,
//   │                                       │   không pill duration
//   └───────────────────────────────────────┘
//   Dòng 1: Tên video                      (canh trái, TEXT_MAIN)
//   Dòng 2: 12:34 - 1.2M lượt xem          (canh trái, TEXT_SUB)
// Spacing gọn: lineStep = textHeight(font) + 2, infoY = thumbY + thumbH + 4.
// ============================================================================

void UIManager::drawYtStandardThumb(int tx, int ty, int tw, int th,
                                    const std::string &vid, bool selected,
                                    bool scaledThumb) {
  (void)selected; // selection border do caller ve ben ngoai (row/card)
  if (tw <= 0 || th <= 0)
    return;

  // Thumbnail thuần (fallback nền tối khi ảnh chưa decode xong)
  SDL_Texture *thumb = nullptr;
  if (!vid.empty()) {
    thumb = m_ytThumbCache.get(vid);
    if (!thumb) {
      std::string thumbPath = "/tmp/yt_thumbs/" + vid + ".jpg";
      if (access(thumbPath.c_str(), R_OK) == 0)
        enqueueThumbDecode(vid, thumbPath, selected);
    }
  }
  if (thumb) {
    SDL_Rect dst;
    if (scaledThumb) {
      // Màn Results vốn RenderCopy bằng toạ độ đã nhân tỉ lệ.
      dst.x = PlatformInfo::instance().scaleX(tx);
      dst.y = PlatformInfo::instance().scaleY(ty);
      dst.w = PlatformInfo::instance().scaleW(tw);
      dst.h = PlatformInfo::instance().scaleH(th);
    } else {
      dst.x = tx;
      dst.y = ty;
      dst.w = tw;
      dst.h = th;
    }
    SDL_RenderCopy(m_renderer, thumb, nullptr, &dst);
  } else {
    drawRoundedRect(tx, ty, tw, th, 6, SDL_Color{12, 18, 28, 255}, true);
  }
}

void UIManager::drawYtStandardMeta(int x, int y, int maxW, int lineStep,
                                   const YtItem &item) {
  if (maxW <= 0)
    return;
  // Dòng 1: tên video (TEXT_MAIN, canh trái)
  drawText(truncateToWidth(item.title, m_fontSmall, maxW), x, y,
           UiTheme::TEXT_MAIN, m_fontSmall, false);
  // Dòng 2: "duration - lượt xem" (TEXT_SUB, canh trái)
  std::string metaLine;
  if (!item.durationStr.empty())
    metaLine += item.durationStr;
  if (!item.viewsStr.empty()) {
    if (!metaLine.empty())
      metaLine += " - ";
    metaLine += item.viewsStr;
  }
  drawText(truncateToWidth(metaLine, m_fontSmall, maxW), x, y + lineStep,
           UiTheme::TEXT_SUB, m_fontSmall, false);
}

void UIManager::renderYouTubeSearchState() {
  loadYouTubeHistory();

  // ─── TrimUI Stock OS Ambient Teal Theme ───
  drawAppBackground();

  // ─── Row 1: Header (Header chuẩn chung + logo YouTube + status bar) ───
  drawRect(0, 0, UiTheme::APP_W, UiTheme::HEADER_H, UiTheme::FOOTER_BG, true);
  drawRect(0, UiTheme::HEADER_H - 1, UiTheme::APP_W, 1, UiTheme::FOOTER_LINE,
           true);
  {
    std::string ytLogoPath =
        AppConfig::instance().getAssetsDir() + "/apps_icons/YOUTUBE.png";
    SDL_Texture *ytLogo = m_ui.getOrLoadImage("grid/YOUTUBE.png", ytLogoPath);
    int titleX = 24;
    if (ytLogo) {
      int texW = 0, texH = 0;
      SDL_QueryTexture(ytLogo, nullptr, nullptr, &texW, &texH);
      int logoH = 40, logoW = 40;
      if (texH > 0)
        logoW = (texW * logoH) / texH;
      int slx = PlatformInfo::instance().scaleX(24);
      int sly =
          PlatformInfo::instance().scaleY((UiTheme::HEADER_H - logoH) / 2);
      int slw = PlatformInfo::instance().scaleW(logoW);
      int slh = PlatformInfo::instance().scaleH(logoH);
      SDL_Rect dst = {slx, sly, slw, slh};
      SDL_RenderCopy(m_renderer, ytLogo, nullptr, &dst);
      titleX = 24 + logoW + 10;
    }
    drawText("YouTube", titleX,
             textYCentered(0, UiTheme::HEADER_H, m_fontLarge),
             UiTheme::ACCENT_CYAN, m_fontLarge);
  }
  drawHeaderStatus();

  // ─── Row 2: Input Field Box (Y = 74, H = 58) ───
  int inX = 46;
  int inY = 74;
  int inW = 932;
  int inH = 58;

  std::string bgInputPath = AppConfig::instance().getAssetsDir() +
                            "/stock_keyboard/bg-search-input.png";
  SDL_Texture *bgInputTex =
      m_ui.getOrLoadImage("stock_keyboard/bg-search-input.png", bgInputPath);
  if (bgInputTex) {
    int slx = PlatformInfo::instance().scaleX(inX);
    int sly = PlatformInfo::instance().scaleY(inY);
    int slw = PlatformInfo::instance().scaleW(inW);
    int slh = PlatformInfo::instance().scaleH(inH);
    SDL_Rect dst = {slx, sly, slw, slh};
    SDL_RenderCopy(m_renderer, bgInputTex, nullptr, &dst);
  } else {
    drawRoundedRect(inX, inY, inW, inH, UiTheme::RADIUS_CARD, {12, 38, 50, 230},
                    true);
    drawRoundedBorder(inX, inY, inW, inH, UiTheme::RADIUS_CARD,
                      {26, 72, 92, 255}, 1);
  }

  std::string dispQ = m_ytVk.query.empty() ? "Nhập từ khóa tìm kiếm video..."
                                           : (m_ytVk.query + " _");
  dispQ = truncateToWidth(dispQ, m_fontLarge, inW - 48);
  SDL_Color qCol = m_ytVk.query.empty() ? SDL_Color{75, 115, 135, 255}
                                        : SDL_Color{255, 255, 255, 255};
  drawText(dispQ, inX + 24, inY + (inH - textHeight(m_fontLarge)) / 2, qCol,
           m_fontLarge, false);

  // ─── Row 3 & Row 4: Search History Pills ───
  std::vector<YtHistoryPill> r3Pills, r4Pills;
  getYouTubeHistoryPills(r3Pills, r4Pills);

  drawText("LỊCH SỬ TÌM KIẾM", inX, 150, UiTheme::TEXT_SUB, m_fontSmall, false);

  const int pillH = 36;
  const int pillGap = 10;

  auto drawPillRow = [this](const std::vector<YtHistoryPill> &pills, int y,
                            int rowIdx) {
    int curX = 46;
    for (size_t i = 0; i < pills.size(); i++) {
      const auto &p = pills[i];
      bool isFocused = (m_ytSearchFocus == 1 && m_ytHistoryRow == rowIdx &&
                        m_ytHistoryCol == static_cast<int>(i));
      std::string text = truncateToWidth(p.text, m_fontSmall, p.w - 24);
      if (isFocused) {
        drawRoundedRect(curX, y, p.w, pillH, pillH / 2,
                        SDL_Color{0, 100, 200, 80}, true);
        drawRoundedBorder(curX, y, p.w, pillH, pillH / 2,
                          SDL_Color{0, 180, 255, 255}, 2);
        drawText(text, curX + p.w / 2,
                 y + (pillH - textHeight(m_fontSmall)) / 2, UiTheme::TEXT_MAIN,
                 m_fontSmall, true);
      } else {
        drawRoundedRect(curX, y, p.w, pillH, pillH / 2, UiTheme::PILL_BG, true);
        drawRoundedBorder(curX, y, p.w, pillH, pillH / 2, UiTheme::PILL_BORDER,
                          1);
        drawText(text, curX + p.w / 2,
                 y + (pillH - textHeight(m_fontSmall)) / 2,
                 UiTheme::PILL_TEXT_DIM, m_fontSmall, true);
      }
      curX += p.w + pillGap;
    }
  };

  if (!r3Pills.empty()) {
    drawPillRow(r3Pills, 192, 0); // Row 3
  }
  if (!r4Pills.empty()) {
    drawPillRow(r4Pills, 246, 1); // Row 4
  }
  if (r3Pills.empty() && r4Pills.empty()) {
    drawText("Chưa có lịch sử tìm kiếm", inX, 192, UiTheme::TEXT_DIM,
             m_fontSmall, false);
  }

  // Divider line above keyboard
  drawRect(46, 444, 932, 1, {20, 48, 64, 180}, true);

  // Compact Virtual Keyboard (Pinned to bottom, Y = 452 to 706)
  const char *ytActs[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  {
    static std::string s0, s1, s2, s3, s4;
    s0 = m_ytVk.shift ? "ABC" : "abc";
    s1 = m_ytVk.telexMode ? "TELEX" : "US";
    s2 = "Cách";
    s3 = "Xóa";
    s4 = "Tìm";
    ytActs[0] = s0.c_str();
    ytActs[1] = s1.c_str();
    ytActs[2] = s2.c_str();
    ytActs[3] = s3.c_str();
    ytActs[4] = s4.c_str();
    VkState ytDraw = m_ytVk;
    if (m_ytSearchFocus != 2)
      ytDraw.row = -1; // Deselect keyboard when history pills have focus
    m_ui.drawVirtualKeyboard(
        ytDraw, 46, 452, 86, 46, 8, 6, SDL_Color{0, 140, 230, 255},
        SDL_Color{0, 180, 255, 255}, ytActs, true, true, 2);
  }

  // ─── Bottom Bar chuẩn: drawAppFooter tự vẽ nền (B = Thoát) ───
  if (m_ytSearchFocus == 1) {
    drawAppFooter({{UiTheme::PadBtn::A, "Tìm"},
                   {UiTheme::PadBtn::UPDOWN, "Bàn phím"},
                   {UiTheme::PadBtn::Y, "Xóa chữ"},
                   {UiTheme::PadBtn::B, "Thoát"}});
  } else {
    drawAppFooter({{UiTheme::PadBtn::A, "Nhập"},
                   {UiTheme::PadBtn::X, "Cách"},
                   {UiTheme::PadBtn::Y, "Xóa"},
                   {UiTheme::PadBtn::L1, "Hoa"},
                   {UiTheme::PadBtn::R1, "Telex"},
                   {UiTheme::PadBtn::START, "Tìm"},
                   {UiTheme::PadBtn::B, "Thoát"}});
  }

  // Searching modal overlay (style chuẩn modal)
  if (m_ytIsSearching) {
    beginModalDim();
    drawRoundedRect(272, 285, 480, 150, UiTheme::RADIUS_MODAL,
                    UiTheme::CARD_SOLID, true);
    drawRoundedBorder(272, 285, 480, 150, UiTheme::RADIUS_MODAL,
                      UiTheme::FOCUS_BG, 2);
    drawText("ĐANG TÌM KIẾM...", 512, 325, UiTheme::TEXT_MAIN, m_fontMedium,
             true);
    std::string qText =
        truncateToWidth("\"" + m_ytVk.query + "\"", m_fontSmall, 420);
    drawText(qText, 512, 370, UiTheme::TEXT_DIM, m_fontSmall, true);
  }
}

void UIManager::renderYouTubeResultsState() {
  // RAM Management: giu thumbnail dang hien, xoa phan con lai khi vuot 24 (LRU
  // 24).
  if (m_ytThumbCache.size() > 24) {
    std::unordered_set<std::string> currentVisible;
    for (const auto &item : m_ytSearchResults) {
      size_t p = item.find('|');
      if (p != std::string::npos)
        currentVisible.insert(item.substr(0, p));
    }
    m_ytThumbCache.retainOnly(currentVisible);
  }

  // P0-2: GPU-upload các surfaces đã decode xong trên worker thread
  drainReadyThumbs();

  // Header chuan chung + logo YouTube + query (khong sub TELEX/US vi
  // keyboard da co bao hieu R2). Van giu nen/status chuan.
  // NOTE: drawAppBackground() đã vẽ header + line separator; KHÔNG vẽ lại ở
  // đây.
  drawAppBackground();

  {
    std::string ytLogoPath =
        AppConfig::instance().getAssetsDir() + "/apps_icons/YOUTUBE.png";
    SDL_Texture *ytLogo = m_ui.getOrLoadImage("grid/YOUTUBE.png", ytLogoPath);
    int titleX = 24;
    if (ytLogo) {
      int texW = 0, texH = 0;
      SDL_QueryTexture(ytLogo, nullptr, nullptr, &texW, &texH);
      int logoH = 40, logoW = 40;
      if (texH > 0)
        logoW = (texW * logoH) / texH;
      int slx = PlatformInfo::instance().scaleX(24);
      int sly =
          PlatformInfo::instance().scaleY((UiTheme::HEADER_H - logoH) / 2);
      int slw = PlatformInfo::instance().scaleW(logoW);
      int slh = PlatformInfo::instance().scaleH(logoH);
      SDL_Rect dst = {slx, sly, slw, slh};
      SDL_RenderCopy(m_renderer, ytLogo, nullptr, &dst);
      titleX = 24 + logoW + 10;
    }
    std::string ytTitle = "YouTube";
    if (!m_ytVk.query.empty())
      ytTitle += ": " + m_ytVk.query;
    // Chua 300px phai cho cum status (giong drawAppHeader)
    ytTitle = truncateToWidth(ytTitle, m_fontLarge, UiTheme::APP_W - 24 - 300);
    drawText(ytTitle, titleX, textYCentered(0, UiTheme::HEADER_H, m_fontLarge),
             UiTheme::ACCENT_CYAN, m_fontLarge);
  }
  drawHeaderStatus();

  // P0-5 build tag (góc phải content, 24px) — giúp verify binary mới đang chạy
  drawText("P0.5", UiTheme::APP_W - 50, 70, UiTheme::TEXT_FAINT, m_fontSmall,
           true);

  // Row-view list doc: 5 rows/page, thumb trai 16:9 + info phai
  // 5 * 110 + 4 * 8 = 582px (content area = 651px) -> thoáng cho title 2 dong
  const int itemsPerPage = 5;
  int totalResults = static_cast<int>(m_ytSearchResults.size());
  int pageStartIndex = (m_ytSearchSelectedIndex / itemsPerPage) * itemsPerPage;

  int rowX = 24;
  int rowW = UiTheme::APP_W - 48; // 976
  int rowH = 110;
  int gapY = 8;
  int startX = rowX;
  int startY = 78;

  int thumbW = 160;
  int thumbH = 90; // 16:9
  int thumbPad = 6;

  for (int i = 0; i < itemsPerPage; i++) {
    int idx = pageStartIndex + i;
    if (idx >= totalResults)
      break;

    int cy = startY + i * (rowH + gapY);
    bool isSelected = (idx == m_ytSearchSelectedIndex);
    bool isChannelRow = (idx < static_cast<int>(m_ytItems.size()) &&
                         m_ytItems[idx].type == YtItem::Type::Channel);

    // Row background chuan theme (Channel row có accent nhẹ khác)
    SDL_Color rowBg;
    if (isChannelRow) {
      rowBg =
          isSelected ? SDL_Color{40, 30, 60, 255} : SDL_Color{24, 18, 38, 255};
    } else {
      rowBg = isSelected ? UiTheme::CARD_SOLID : UiTheme::ROW_BG_ALT;
    }
    drawRoundedRect(startX, cy, rowW, rowH, UiTheme::RADIUS_ROW, rowBg, true);

    // Selected focus: vien xanh + glow
    if (isSelected) {
      SDL_Color glow =
          isChannelRow ? SDL_Color{170, 130, 255, 255} : UiTheme::FOCUS_GLOW;
      drawRoundedBorder(startX, cy, rowW, rowH, UiTheme::RADIUS_ROW, glow, 2);
    }

    // ─── P0-3: CHANNEL row riêng (avatar tròn + tên kênh + sub/video count)
    // ───
    if (isChannelRow) {
      // Avatar placeholder (hình tròn) bên trái, không có thumb
      int avatarSize = 70;
      int avatarX = startX + 18;
      int avatarY = cy + (rowH - avatarSize) / 2;
      drawRoundedRect(avatarX, avatarY, avatarSize, avatarSize, avatarSize / 2,
                      SDL_Color{80, 60, 140, 255}, true);
      // First letter of channel name
      std::string chName = (idx < static_cast<int>(m_ytItems.size()))
                               ? m_ytItems[idx].title
                               : "";
      std::string firstChar = chName.empty() ? "?" : utf8FirstChar(chName);
      drawText(firstChar, avatarX + avatarSize / 2,
               avatarY + (avatarSize - textHeight(m_fontLarge)) / 2,
               SDL_Color{240, 230, 255, 255}, m_fontLarge, true);

      // "KÊNH" badge phía trên avatar
      drawRoundedRect(avatarX, avatarY - 4, 38, 14, 4,
                      SDL_Color{170, 130, 255, 255}, true);
      drawText("KÊNH", avatarX + 19, avatarY - 2, SDL_Color{20, 12, 32, 255},
               m_fontSmall, true);

      int infoX = avatarX + avatarSize + 18;
      int infoW = startX + rowW - infoX - 14;
      int infoY = cy + 12;

      if (idx < static_cast<int>(m_ytItems.size())) {
        const auto &item = m_ytItems[idx];
        // Channel name (fontMedium - to, prominent)
        std::string name = truncateToWidth(item.titleL1, m_fontMedium, infoW);
        drawText(name, infoX, infoY, UiTheme::TEXT_MAIN, m_fontMedium, false);

        // Meta line: "X sub • Y videos"
        std::string meta = item.subscribersStr;
        if (!item.videoCountStr.empty()) {
          if (!meta.empty())
            meta += "  •  ";
          meta += item.videoCountStr;
        }
        meta = truncateToWidth(meta, m_fontSmall, infoW);
        drawText(meta, infoX, infoY + 30, UiTheme::TEXT_SUB, m_fontSmall,
                 false);

        // Hint: "Nhấn [A] để xem video mới nhất"
        std::string hint =
            isSelected ? "[A] Xem video mới nhất từ kênh" : "Kênh YouTube";
        drawText(hint, infoX, infoY + 52,
                 isSelected ? SDL_Color{170, 130, 255, 255} : UiTheme::TEXT_DIM,
                 m_fontSmall, false);
      }
      continue; // skip video thumb render
    }

    // ─── VIDEO row: chuẩn hoá layout (banner tên video trên thumbnail,
    // dòng 1 duration - views, dòng 2 tên kênh) ───
    auto parts = split(m_ytSearchResults[idx], '|');
    std::string vid = !parts.empty() ? parts[0] : "";
    const YtItem *item =
        (idx < static_cast<int>(m_ytItems.size())) ? &m_ytItems[idx] : nullptr;

    // Thumbnail trai (16:9) — dùng chung helper, scaledThumb=true cho màn
    // này (RenderCopy theo toạ độ đã scale của PlatformInfo).
    int thumbX = startX + thumbPad;
    int thumbY = cy + (rowH - thumbH) / 2;
    drawYtStandardThumb(thumbX, thumbY, thumbW, thumbH, vid, isSelected,
                        /*scaledThumb=*/true);
    SDL_Texture *thumbTex = vid.empty() ? nullptr : m_ytThumbCache.get(vid);
    if (thumbTex) {
      drawRoundedBorder(thumbX, thumbY, thumbW, thumbH, UiTheme::RADIUS_ROW,
                        {45, 45, 45, 180}, 1);
    } else {
      // Chưa có ảnh: helper đã vẽ nền tối, thêm chữ "YouTube" cho nhận diện.
      drawRoundedRect(thumbX, thumbY, thumbW, thumbH, UiTheme::RADIUS_ROW,
                      SDL_Color{32, 32, 32, 255}, true);
      drawText("YouTube", thumbX + thumbW / 2,
               thumbY + (thumbH - textHeight(m_fontSmall)) / 2,
               SDL_Color{229, 9, 20, 255}, m_fontSmall, true);
    }

    // Info phai: chuan 2 dong canh trai, can giua deu voi thumbnail
    int infoX = thumbX + thumbW + 14;
    int infoW = startX + rowW - infoX - 12;
    if (item) {
      int lh = textHeight(m_fontSmall);
      int step = lh + 6;
      int infoY = thumbY + (thumbH - (lh * 2 + 6)) / 2;
      drawYtStandardMeta(infoX, infoY, infoW, step, *item);
    }
  }

  // Page info nam tren footer (khong de len drawAppFooter)
  // Pill bo góc cho đẹp
  char pageInfo[64];
  int maxPage = std::max(
      1, (static_cast<int>(m_ytAllCachedResults.size()) + itemsPerPage - 1) /
             itemsPerPage);
  snprintf(pageInfo, sizeof(pageInfo), "Trang %d/%d  •  %d videos",
           m_ytCurrentPage, maxPage, totalResults);
  drawRoundedRect(32, 680, 220, 26, 13, UiTheme::CARD_SOLID, true);
  drawText(pageInfo, 142, 685, UiTheme::TEXT_SUB, m_fontSmall, true);

  // Scrollbar ben phai (chi khi nhieu hon 1 page)
  int cachedTotal = static_cast<int>(m_ytAllCachedResults.size());
  if (cachedTotal > itemsPerPage) {
    int barX = UiTheme::APP_W - 14;
    int barY = startY + 2;
    int barH = itemsPerPage * (rowH + gapY) - gapY - 4;
    // track
    drawRoundedRect(barX, barY, 4, barH, 2, UiTheme::TEXT_FAINT, true);
    // thumb: ti le theo itemsPerPage/total, position theo selectedIndex
    int thumbH =
        std::max<int>(24, barH * itemsPerPage / std::max(1, cachedTotal));
    int maxThumbY = barY + barH - thumbH;
    int thumbY = barY;
    if (cachedTotal > itemsPerPage) {
      thumbY = barY + (int64_t)(maxThumbY - barY) * m_ytSearchSelectedIndex /
                          std::max(1, cachedTotal - 1);
    }
    drawRoundedRect(barX, thumbY, 4, thumbH, 2, UiTheme::ACCENT_CYAN, true);
  }

  drawAppFooter({{UiTheme::PadBtn::A, "Xem"},
                 {UiTheme::PadBtn::B, "Lùi"},
                 {UiTheme::PadBtn::START, "Tìm kiếm"},
                 {UiTheme::PadBtn::L1R1, "Trang"}});

  // Resolving stream overlay (style chuan modal)
  if (m_ytIsLoadingVideo) {
    beginModalDim();
    drawRoundedRect(272, 285, 480, 150, UiTheme::RADIUS_MODAL,
                    UiTheme::CARD_SOLID, true);
    drawRoundedBorder(272, 285, 480, 150, UiTheme::RADIUS_MODAL,
                      UiTheme::FOCUS_BG, 2);
    drawText("ĐANG TẢI VIDEO...", 512, 325, UiTheme::TEXT_MAIN, m_fontMedium,
             true);
    drawText("Đang kết nối luồng phát...", 512, 372, UiTheme::TEXT_DIM,
             m_fontSmall, true);
  }
}

// =============================================================
// LocalSend render functions
// =============================================================
void UIManager::renderLocalSendHome() {
  // ===== Background + Header chuẩn app =====
  drawAppBackground();
  std::string myIp = LocalSendManager::instance().ownIp();
  if (myIp.empty() || myIp == "0.0.0.0")
    myIp = LsUtil::getOwnIp("wlan0");
  // Single-line header: title (cyan) + IP (dim) cùng row, bỏ "P2P chia sẻ..."
  drawAppHeader("LocalSend", "");
  if (!myIp.empty()) {
    std::string ipTxt = "IP " + myIp;
    int ipX = 24 + textWidth("LocalSend", m_fontLarge) + 16;
    int ipY = textYCentered(0, UiTheme::HEADER_H, m_fontSmall);
    drawText(ipTxt, ipX, ipY, UiTheme::TEXT_DIM, m_fontSmall, false);
  }

  // ===== Layout: sidebar trái (chế độ) + content phải =====
  // Sidebar bg card-style (CARD_SOLID), thấp hơn header, cao hơn footer.
  int sbW = 284;
  int sbX = 0;
  int sbY = UiTheme::HEADER_H + 12;             // 76
  int sbH = 768 - sbY - UiTheme::FOOTER_H - 12; // ~627
  drawRoundedRect(sbX, sbY, sbW, sbH, UiTheme::RADIUS_CARD, UiTheme::CARD_SOLID,
                  true);
  drawRoundedBorder(sbX, sbY, sbW, sbH, UiTheme::RADIUS_CARD,
                    UiTheme::CARD_BORDER, 1);
  // Sub-label "CHẾ ĐỘ"
  drawText("CHẾ ĐỘ", sbW / 2, sbY + 24, UiTheme::ACCENT_CYAN, m_fontSmall,
           true);
  // m_localSendMode: 0=SEND, 1=RECEIVE — sidebar hien thi Receive tren, Send
  const char *sbLabels[2] = {"Receive", "Send"};
  for (int i = 0; i < 2; ++i) {
    int modeVal = (i == 0) ? 1 : 0; // row0=Receive(1), row1=Send(0)
    bool sel = (m_localSendMode == modeVal);
    int ry = sbY + 60 + i * 80;
    int rx = 14;
    int rw = sbW - 28;
    int rH = 60;
    if (sel)
      drawRoundedRect(rx, ry, rw, rH, 30, UiTheme::FOCUS_BG, true);
    else
      drawRoundedRect(rx, ry, rw, rH, 30, UiTheme::ROW_BG, true);
    // Icon PNG: Send -> assets/player_icons/send.png, Receive -> receive.png
    const char *iconFile = (i == 0) ? "receive" : "send";
    drawPlayerIcon(iconFile, rx + 18, ry + 14, 32, 32);
    SDL_Color lblColor = sel ? UiTheme::TEXT_MAIN : UiTheme::TEXT_DIM;
    // Can giua text theo glyph trong pill de tranh lech xuong duoi
    drawText(sbLabels[i], rx + 64, textYCentered(ry, rH, m_fontMedium),
             lblColor, m_fontMedium, false);
  }
  // Ghi chú nhỏ cuối sidebar
  drawText("LAN-only • HTTP/HTTPS", sbW / 2, sbY + sbH - 28,
           UiTheme::TEXT_FAINT, m_fontSmall, true);

  // ===== Content phải =====
  int cx = sbW + 30;
  int cw = 1024 - cx - 24;
  if (m_localSendMode == 1) {
    // ===== RECEIVE: panel trạng thái lắng nghe =====
    drawText("Receive", cx, sbY + 10, UiTheme::ACCENT_CYAN, m_fontLarge, false);
    int stY = sbY + 64;
    drawRoundedRect(cx, stY, cw, 140, UiTheme::RADIUS_MODAL,
                    UiTheme::CARD_SOLID, true);
    drawRoundedBorder(cx, stY, cw, 140, UiTheme::RADIUS_MODAL,
                      UiTheme::FOCUS_BG, 2);
    // Status dot (cyan, animated-looking — solid)
    drawRoundedRect(cx + 24, stY + 24, 12, 12, 6, UiTheme::ACCENT_CYAN, true);
    std::string myAlias = LocalSendManager::instance().alias();
    drawText(myAlias.empty() ? "TrimUI" : myAlias, cx + 50, stY + 16,
             UiTheme::TEXT_MAIN, m_fontLarge, false);
    std::string tgt = LocalSendManager::instance().currentTargetFolder();
    drawText(tgt.empty() ? "Đang lắng nghe..." : tgt, cx + 24, stY + 64,
             UiTheme::TEXT_DIM, m_fontSmall, false);
    drawText("Mở LocalSend trên máy khác và gử file tới thiết bị này.", cx + 24,
             stY + 96, UiTheme::TEXT_SUB, m_fontSmall, false);
  } else {
    // ===== SEND: Nearby devices (A vào thẳng picker ROM) =====
    drawText("Nearby devices", cx, sbY + 10, UiTheme::ACCENT_CYAN, m_fontLarge,
             false);
    int ny = sbY + 64;
    auto devices = LocalSendManager::instance().knownDevices();
    int devCount = (int)devices.size();
    if (devCount == 0) {
      drawRoundedRect(cx, ny, cw, 110, UiTheme::RADIUS_MODAL,
                      UiTheme::CARD_SOLID, true);
      drawRoundedBorder(cx, ny, cw, 110, UiTheme::RADIUS_MODAL,
                        UiTheme::CARD_BORDER, 1);
      drawText("Đang tìm thiết bị...", cx + cw / 2, ny + 28, UiTheme::TEXT_MAIN,
               m_fontMedium, true);
      drawText("Hãy mở LocalSend trên máy khác", cx + cw / 2, ny + 64,
               UiTheme::TEXT_SUB, m_fontSmall, true);
    } else {
      for (int i = 0; i < devCount; ++i) {
        int dy = ny + i * 108;
        if (dy + 96 > 640)
          break;
        const auto &d = devices[i];
        bool sel = (i == m_localSendSelectedDevice);
        drawRoundedRect(cx, dy, cw, 96, UiTheme::RADIUS_MODAL,
                        UiTheme::CARD_SOLID, true);
        if (sel) {
          drawRoundedBorder(cx, dy, cw, 96, UiTheme::RADIUS_MODAL,
                            UiTheme::FOCUS_BG, 3);
        } else {
          drawRoundedBorder(cx, dy, cw, 96, UiTheme::RADIUS_MODAL,
                            UiTheme::CARD_BORDER, 1);
        }
        // Icon thiết bị (viền cyan-blue)
        drawRoundedBorder(cx + 22, dy + 18, 34, 60, UiTheme::RADIUS_ROW,
                          UiTheme::FOCUS_GLOW, 2);
        drawRect(cx + 26, dy + 24, 26, 42, UiTheme::ROW_BG, true);
        drawRect(cx + 36, dy + 70, 6, 2, UiTheme::TEXT_DIM, true);

        std::string nm = d.alias.empty() ? d.ip : d.alias;
        // Truncate theo pixel (UTF-8 safe) — alias TV có thể chứa dấu.
        nm = truncateToWidth(nm, m_fontLarge, cw - 72 - 130);
        drawText(nm, cx + 72, dy + 18, UiTheme::TEXT_MAIN, m_fontLarge, false);

        std::string proto = (d.protocol == "https") ? "HTTPS" : "HTTP";
        SDL_Color protoBg =
            (d.protocol == "https") ? UiTheme::FOCUS_BG : UiTheme::ACCENT_BLUE;
        int protoW = badgeWidth(proto, 24);
        drawBadge(cx + 72, dy + 54, protoW, 24, proto, protoBg,
                  UiTheme::TEXT_MAIN);

        std::string sub = d.deviceModel.empty()
                              ? (d.ip + ":" + std::to_string(d.port))
                              : d.deviceModel;
        drawText(sub, cx + 72 + protoW + 8, dy + 56, UiTheme::TEXT_SUB,
                 m_fontSmall, false);
      }
    }
    // Troubleshoot tip (trong content, không đè footer bar)
    int tipY = 768 - UiTheme::FOOTER_H - 56;
    drawText("Hãy chắc chắn máy đích cùng mạng Wi-Fi.", cx + cw / 2, tipY,
             UiTheme::TEXT_FAINT, m_fontSmall, true);
  }
  // Footer hint — luôn có B để back về Menu
  if (m_localSendMode == 0) {
    drawAppFooter({{UiTheme::PadBtn::DPAD, "Chuyển tab"},
                   {UiTheme::PadBtn::A, "Chọn gửi ROM"},
                   {UiTheme::PadBtn::Y, "Tìm lại"},
                   {UiTheme::PadBtn::B, "Quay lại"}});
  } else {
    drawAppFooter({{UiTheme::PadBtn::DPAD, "Chuyển tab"},
                   {UiTheme::PadBtn::X, "Chọn thư mục lưu"},
                   {UiTheme::PadBtn::Y, "Tìm lại"},
                   {UiTheme::PadBtn::B, "Quay lại"}});
  }
}

void UIManager::renderLocalSendIncomingDialog() {
  beginModalDim();
  int dlgX = 122, dlgY = 144, dlgW = 780, dlgH = 480;
  drawRoundedRect(dlgX, dlgY, dlgW, dlgH, UiTheme::RADIUS_MODAL,
                  SDL_Color{15, 23, 42, 245}, true);

  drawText("CÓ FILE ĐẾN", dlgX + dlgW / 2, dlgY + 36, {250, 204, 21, 255},
           m_fontTitle, true);

  drawText("Từ thiết bị:", dlgX + 40, dlgY + 110, {148, 163, 184, 255},
           m_fontMedium, false);
  drawText(m_localSendCurrentPrompt.fromAlias, dlgX + 40, dlgY + 142,
           {255, 255, 255, 255}, m_fontLarge, false);
  drawText("(" + m_localSendCurrentPrompt.fromIp + ")", dlgX + dlgW - 40,
           dlgY + 142, {148, 163, 184, 255}, m_fontMedium, true);

  const auto &file = m_localSendCurrentPrompt.file;
  bool isGame = !file.gameTitle.empty() || !file.systemCode.empty();

  if (isGame) {
    int cvX = dlgX + 40, cvY = dlgY + 190, cvW = 120, cvH = 160;
    GameRecord tmpG;
    tmpG.coverPath = file.coverPath;
    tmpG.title = file.gameTitle;
    tmpG.filename = file.fileName;
    SystemRecord sys;
    sys.code = file.systemCode;
    sys.name = file.systemName;
    int texW = 0, texH = 0;
    SDL_Texture *tex =
        CoverManager::instance().getCoverTexture(tmpG, sys, texW, texH);
    if (tex) {
      SDL_Rect dst{cvX, cvY, cvW, cvH};
      SDL_RenderCopy(m_renderer, tex, nullptr, &dst);
    } else {
      drawRoundedRect(cvX, cvY, cvW, cvH, UiTheme::RADIUS_CARD,
                      SDL_Color{30, 41, 59, 255}, true);
      drawText(file.systemCode.empty() ? "?" : file.systemCode, cvX + cvW / 2,
               cvY + cvH / 2 - 12, {100, 116, 139, 255}, m_fontLarge, true);
    }

    int txtX = cvX + cvW + 30;
    std::string dispTitle =
        file.gameTitle.empty() ? file.fileName : file.gameTitle;
    drawText(dispTitle, txtX, cvY, {34, 197, 94, 255}, m_fontLarge, false);

    if (!file.systemName.empty() || !file.systemCode.empty()) {
      std::string sysStr =
          "Hệ máy: " +
          (file.systemName.empty() ? file.systemCode : file.systemName);
      if (!file.systemCode.empty())
        sysStr += "  (" + file.systemCode + ")";
      drawText(sysStr, txtX, cvY + 42, {250, 204, 21, 255}, m_fontMedium,
               false);
    }

    drawText("File: " + file.fileName, txtX, cvY + 78, {148, 163, 184, 255},
             m_fontSmall, false);

    std::string sizeStr = "Kích thước: " + LsUtil::humanSize(file.size);
    uint64_t freeB0 = LsUtil::sdFreeBytes("/mnt/SDCARD");
    sizeStr += " | Trống: " + LsUtil::humanSize(freeB0);
    drawText(sizeStr, txtX, cvY + 102,
             (file.size > 0 && freeB0 < file.size)
                 ? SDL_Color{239, 68, 68, 255}
                 : SDL_Color{203, 213, 225, 255},
             m_fontMedium, false);

    std::string saveDisplay = m_localSendIncomingSavePath.empty()
                                  ? m_localSendCurrentPrompt.savedPath
                                  : m_localSendIncomingSavePath;
    drawText("Sẽ lưu vào:", dlgX + 40, dlgY + dlgH - 60, {148, 163, 184, 255},
             m_fontSmall, false);
    drawText(saveDisplay, dlgX + 40, dlgY + dlgH - 36, {34, 197, 94, 255},
             m_fontSmall, false);
  } else {
    drawText("File:", dlgX + 40, dlgY + 200, {148, 163, 184, 255}, m_fontMedium,
             false);
    drawText(file.fileName, dlgX + 40, dlgY + 232, {34, 197, 94, 255},
             m_fontLarge, false);
    std::string sizeStr = "Kích thước: " + LsUtil::humanSize(file.size);
    uint64_t freeB1 = LsUtil::sdFreeBytes("/mnt/SDCARD");
    sizeStr += " | Trống: " + LsUtil::humanSize(freeB1);
    drawText(sizeStr, dlgX + 40, dlgY + 280,
             (file.size > 0 && freeB1 < file.size)
                 ? SDL_Color{239, 68, 68, 255}
                 : SDL_Color{203, 213, 225, 255},
             m_fontMedium, false);

    if (!file.relativePath.empty()) {
      drawText("Đường dẫn: " + file.relativePath, dlgX + 40, dlgY + 310,
               {148, 163, 184, 255}, m_fontMedium, false);
    }

    std::string saveDisplay = m_localSendIncomingSavePath.empty()
                                  ? m_localSendCurrentPrompt.savedPath
                                  : m_localSendIncomingSavePath;
    drawText("Sẽ lưu vào:", dlgX + 40, dlgY + 368, {148, 163, 184, 255},
             m_fontMedium, false);
    drawText(saveDisplay, dlgX + 40, dlgY + 398, {34, 197, 94, 255},
             m_fontMedium, false);
  }

  drawAppFooter({{UiTheme::PadBtn::A, "Nhận file"},
                 {UiTheme::PadBtn::X, "Chọn thư mục lưu"},
                 {UiTheme::PadBtn::B, "Từ chối"}});
}

void UIManager::renderLocalSendFolderPicker() {
  if (m_expPicker.creatingFolder() || m_expPicker.renaming()) {
    renderExplorerKeyboardFor(m_expPicker);
    return;
  }

  drawAppBackground();

  drawAppHeader(m_expPickerTitle.c_str());

  uint64_t freeB = LsUtil::sdFreeBytes("/mnt/SDCARD");
  drawText("Trống: " + LsUtil::humanSize(freeB), 996, 22, {34, 197, 94, 255},
           m_fontSmall, false);

  // 1 single pane: width 976, x = 24, y = 72, h = 636
  const int px = 24, paneY = 72, paneW = 976, paneH = 636;
  drawRect(px, paneY, paneW, paneH, {15, 23, 42, 255}, true);
  drawBorder(px, paneY, paneW, paneH, {59, 130, 246, 255}, 1);

  // Path bar on top of the pane
  std::string path = m_expPicker.currentPath();
  const std::string root = "/mnt/SDCARD";
  std::string sp = path;
  if (sp.compare(0, root.size(), root) == 0)
    sp = sp.substr(root.size());
  if (sp.empty())
    sp = "/";
  if ((int)sp.size() > 80)
    sp = ".." + sp.substr(sp.size() - 78);
  drawText("Thư mục: " + sp, px + 16, paneY + 12, {255, 255, 255, 255},
           m_fontMedium, false);
  drawRect(px, paneY + 44, paneW, 1, {30, 41, 59, 255}, true);

  const auto &entries = m_expPicker.entries();
  int total = (int)entries.size();
  int rowH = 54;
  int listTop = paneY + 48;
  int visibleRows = (paneH - 52) / rowH;
  if (visibleRows < 1)
    visibleRows = 1;

  m_expPickerScroll = FileListView::calcScroll(m_expPicker.selected(),
                                               visibleRows, m_expPickerScroll);

  for (int i = 0; i < visibleRows && m_expPickerScroll + i < total; ++i) {
    int idx = m_expPickerScroll + i;
    int y = listTop + i * rowH;
    bool sel = (idx == m_expPicker.selected());
    if (sel) {
      drawRect(px + 8, y, paneW - 16, rowH - 6, UiTheme::FOCUS_BG, true);
      drawRect(px + 8, y, 4, rowH - 6, UiTheme::FOCUS_GLOW, true);
    }
    const auto &e = entries[(size_t)idx];
    drawGridIcon("FOLDER.png", px + 16, y + 8, 34, 34);
    drawText(e.name, px + 60, y + 12,
             sel ? SDL_Color{255, 255, 255, 255}
                 : SDL_Color{203, 213, 225, 255},
             m_fontMedium, false);
  }

  if (total == 0) {
    drawText("(Thư mục trống)", px + paneW / 2, listTop + 60,
             {100, 116, 139, 255}, m_fontMedium, true);
  }

  // Footer chuan IPTV (drawAppFooter tu ve nen)
  drawAppFooter({{UiTheme::PadBtn::A, "Vào"},
                 {UiTheme::PadBtn::START, "Xác nhận mục lưu"},
                 {UiTheme::PadBtn::B, "Lùi"},
                 {UiTheme::PadBtn::Y, "Thư mục mới"}});
}

void UIManager::renderLocalSendSendPicker() {
  drawText("GỬI FILE ĐẾN THIẾT BỊ", 512, 50, {255, 255, 255, 255}, m_fontTitle,
           true);
  drawText("Tính năng đang phát triển — CLI script sẵn:", 512, 102,
           {148, 163, 184, 255}, m_fontMedium, true);

  int dlgX = 80, dlgY = 150, dlgW = 864, dlgH = 480;
  drawRoundedRect(dlgX, dlgY, dlgW, dlgH, UiTheme::RADIUS_MODAL,
                  SDL_Color{15, 23, 42, 220}, true);

  auto devices = LocalSendManager::instance().knownDevices();
  if (devices.empty()) {
    drawText("Chưa tìm thấy thiết bị nào", dlgX + dlgW / 2, dlgY + 100,
             {148, 163, 184, 255}, m_fontLarge, true);
  } else {
    for (size_t i = 0; i < devices.size(); ++i) {
      int y = dlgY + 60 + (int)i * 60;
      drawText(devices[i].alias + "  (" + devices[i].ip + ")", dlgX + 40, y,
               {203, 213, 225, 255}, m_fontMedium, false);
    }
  }

  drawText("Hiện tại gửi file bằng CLI:", dlgX + 40, dlgY + 330,
           {250, 204, 21, 255}, m_fontMedium, false);
  drawText("  bash /mnt/SDCARD/Apps/RomCloud/scripts/localsend_send.sh \\",
           dlgX + 40, dlgY + 370, {148, 163, 184, 255}, m_fontSmall, false);
  drawText("    <alias_or_ip> <file_path>", dlgX + 40, dlgY + 402,
           {148, 163, 184, 255}, m_fontSmall, false);

  drawAppFooter({{UiTheme::PadBtn::B, "Lùi"}});
}

void UIManager::renderLocalSendGamePicker() {
  // SEND chỉ quét /Roms (Apps tab tạm ẩn). Rescan mỗi lần vào
  // (m_lsRomListLoaded=false khi chuyển state) + quét đệ quy để giữ
  // nguyên cấu trúc subfolder, gửi kèm relative path chính xác.
  m_lsPickerTab = 0;
  if (!m_lsRomListLoaded) {
    m_lsRomListLoaded = true;
    m_lsRomList.clear();
    // Ép quét tươi thẻ SD (giống nút ĐỒNG BỘ) rồi mới đọc DB → số lượng khớp.
    try {
      if (!isIndexing()) {
        RomIndexer::instance().scanAllSystems(
            AppConfig::instance().getRomsDir(), nullptr);
        refreshSystems();
      }
    } catch (...) {
    }
    // DÙNG CHUNG NGUỒN VỚI ĐỒNG BỘ/THƯ VIỆN: đọc DB (RomIndexer đã quét
    // /mnt/SDCARD/Roms/<SYSTEM> và lưu localPath). Như vậy số lượng ROM
    // ở màn SEND luôn khớp với số lượng đồng bộ thấy.
    // Nếu DB chưa có (chưa scan lần nào) thì fallback quét filesystem đệ quy.
    auto systems = DatabaseManager::instance().getSystems(false);
    for (const auto &sys : systems) {
      auto games = DatabaseManager::instance().getGamesBySystem(
          sys.id, static_cast<int>(GameState::LOCAL));
      for (const auto &g : games) {
        if (g.localPath.empty())
          continue;
        if (!FileSystemManager::instance().fileExists(g.localPath))
          continue;
        LsRomEntry e;
        e.path = g.localPath;
        e.name = g.filename.empty() ? g.title : g.filename;
        e.systemDir = sys.romDir.empty() ? sys.code : sys.romDir;
        e.sizeBytes = g.sizeBytes;
        m_lsRomList.push_back(e);
      }
    }
    if (m_lsRomList.empty()) {
      // Fallback: quét ĐỆ QUY /Roms để giữ nguyên cấu trúc subfolder.
      std::string romsRoot = AppConfig::instance().getRomsDir();
      std::vector<std::string> stack;
      stack.push_back(romsRoot);
      auto topNameOf = [&](const std::string &p) -> std::string {
        std::string rel = (p.compare(0, romsRoot.size(), romsRoot) == 0)
                              ? p.substr(romsRoot.size())
                              : p;
        while (!rel.empty() && rel.front() == '/')
          rel.erase(rel.begin());
        auto sl = rel.find('/');
        std::string top = (sl == std::string::npos) ? rel : rel.substr(0, sl);
        return top.empty() ? std::string("Roms") : top;
      };
      while (!stack.empty()) {
        std::string cur = stack.back();
        stack.pop_back();
        auto entries = FileSystemManager::instance().listDirectory(cur, false);
        for (const auto &f : entries) {
          if (!f.name.empty() && f.name[0] == '.')
            continue;
          if (f.isDirectory) {
            stack.push_back(f.path);
            continue;
          }
          LsRomEntry e;
          e.path = f.path;
          e.name = f.name;
          e.systemDir = topNameOf(cur);
          e.sizeBytes = f.sizeBytes;
          m_lsRomList.push_back(e);
        }
      }
    }
    std::sort(m_lsRomList.begin(), m_lsRomList.end(),
              [](const LsRomEntry &a, const LsRomEntry &b) {
                if (a.systemDir != b.systemDir)
                  return a.systemDir < b.systemDir;
                return a.name < b.name;
              });
    Logger::info("LocalSend ROMS SD scan: " +
                 std::to_string(m_lsRomList.size()));
    m_lsRomSelected = 0;
    m_lsRomScrollOffset = 0;
  }

  // (Apps picker đã tạm ẩn — không quét /Apps ở màn SEND.)

  drawAppBackground();

  drawAppHeader("CHỌN ROM ĐỂ GỬI");

  auto devices = LocalSendManager::instance().knownDevices();
  std::string sub = "Gửi tới: ";
  if (!devices.empty() && m_localSendSelectedDevice >= 0 &&
      m_localSendSelectedDevice < (int)devices.size()) {
    sub += devices[m_localSendSelectedDevice].alias;
  } else {
    sub += "(chưa chọn thiết bị)";
  }
  drawTextRight(sub, 1000, 22, {0, 180, 216, 255},
                m_fontMedium ? m_fontMedium : m_fontSmall);

  const int px = 24, listY = 74, paneW = 976, paneH = 630;
  int n = (int)m_lsRomList.size();
  if (n == 0) {
    drawText("Chưa có game LOCAL nào trên thẻ nhớ.", px + paneW / 2,
             listY + 220, {148, 163, 184, 255}, m_fontLarge, true);
    drawText("Bấm Y để làm mới sau khi quét ROM", px + paneW / 2, listY + 265,
             {100, 116, 139, 255}, m_fontSmall, true);
  } else {
    renderLsRomsList(px, listY, paneW, paneH);
  }

  // Footer chuan IPTV (drawAppFooter tu ve nen)
  drawAppFooter({{UiTheme::PadBtn::A, "Gửi file"},
                 {UiTheme::PadBtn::B, "Quay lại"},
                 {UiTheme::PadBtn::Y, "Tải lại"}});
}

void UIManager::renderLsProgressRow(bool isSend, int idx, int x, int y, int w,
                                    bool sel) {
  m_dialogs.renderLsRow(m_ui, m_fontSmall, m_fontMedium, isSend, idx, x, y, w,
                        sel);
}

void UIManager::renderLocalSendProgress() {
  m_dialogs.renderLocalSendProgress(
      m_ui, m_fontSmall, m_fontMedium, m_fontLarge, m_fontTitle,
      m_localSendProgressSel, m_localSendProgressScroll);
}

// Helper: render ROMS list với track thực tế Y để tránh overlap khi có system
// separator.
void UIManager::renderLsRomsList(int dlgX, int dlgY, int dlgW, int dlgH) {
  int n = (int)m_lsRomList.size();
  if (n == 0)
    return;
  const int headerH = 26;
  const int itemH = 68;
  const int padding = 4;
  const int pageSize = 6;
  int availH = dlgH - 2 * padding;
  auto drawnRowsFrom = [&](int startIdx) -> int {
    int rows = 0;
    std::string firstSys;
    bool first = true;
    for (int i = startIdx; i < n; ++i) {
      const auto &e = m_lsRomList[i];
      int need = itemH;
      if (first || e.systemDir != firstSys)
        need += headerH;
      if (rows * headerH + need > availH)
        break;
      rows = (rows * headerH + need + headerH - 1) / headerH;
      firstSys = e.systemDir;
      first = false;
      if ((int)(i - startIdx + 1) >= pageSize + 2)
        break;
    }
    return rows;
  };
  if (m_lsRomSelected < m_lsRomScrollOffset)
    m_lsRomScrollOffset = m_lsRomSelected;
  while (m_lsRomScrollOffset < n) {
    int drawn = drawnRowsFrom(m_lsRomScrollOffset);
    if (drawn == 0) {
      m_lsRomScrollOffset = std::min(n - 1, m_lsRomScrollOffset + 1);
      continue;
    }
    if (m_lsRomSelected < m_lsRomScrollOffset + drawn)
      break;
    m_lsRomScrollOffset++;
  }
  if (m_lsRomScrollOffset >= n)
    m_lsRomScrollOffset = std::max(0, n - 1);
  int y = dlgY + padding;
  std::string curSys;
  bool firstRow = true;
  int startIdx = m_lsRomScrollOffset;
  for (int i = startIdx; i < n; ++i) {
    const auto &e = m_lsRomList[i];
    int need = itemH;
    if (firstRow || e.systemDir != curSys)
      need += headerH;
    if (y + need > dlgY + dlgH - 8)
      break;
    if (firstRow || e.systemDir != curSys) {
      curSys = e.systemDir;
      drawText(curSys.empty() ? "ROMS" : curSys, dlgX + 8, y + 2,
               {0, 180, 216, 255}, m_fontSmall);
      y += headerH;
    }
    firstRow = false;
    bool sel = (i == m_lsRomSelected);
    int itemX = dlgX;
    int itemW = dlgW;
    if (sel) {
      drawFocusRow(itemX, y, itemW, itemH);
    } else {
      drawRoundedRect(itemX, y, itemW, itemH, UiTheme::RADIUS_ROW,
                      UiTheme::ROW_BG, true);
      drawRoundedBorder(itemX, y, itemW, itemH, UiTheme::RADIUS_ROW,
                        UiTheme::CARD_BORDER, 1);
    }

    std::string badgeText = e.systemDir.empty() ? "ROM" : e.systemDir;
    if (badgeText.size() > 8)
      badgeText = badgeText.substr(0, 8);
    int lsBW = badgeWidth(badgeText, 32);
    drawBadge(itemX + 14, y + (itemH - 32) / 2, lsBW, 32, badgeText,
              sel ? UiTheme::FOCUS_BG : UiTheme::CARD_SOLID,
              UiTheme::TEXT_MAIN);

    int tx = itemX + 14 + lsBW + 12;
    SDL_Color titleC =
        sel ? SDL_Color{255, 255, 255, 255} : SDL_Color{226, 232, 240, 255};
    SDL_Color metaC =
        sel ? SDL_Color{200, 225, 245, 255} : SDL_Color{148, 163, 184, 255};

    std::string sizeStr =
        FileSystemManager::instance().formatBytes(e.sizeBytes);
    drawTextRight(sizeStr, itemX + itemW - 18, y + 24, metaC, m_fontSmall);

    int maxTitleW =
        (itemX + itemW - 18 - textWidth(sizeStr, m_fontSmall) - 12) - tx;
    if (maxTitleW < 200)
      maxTitleW = 200;
    std::string title = truncateToWidth(
        e.name, m_fontMedium ? m_fontMedium : m_fontSmall, maxTitleW);
    drawText(title, tx, y + 12, titleC,
             m_fontMedium ? m_fontMedium : m_fontSmall);

    std::string meta = e.systemDir + "  •  " + e.path;
    std::string truncMeta = truncateToWidth(meta, m_fontSmall, maxTitleW);
    drawText(truncMeta, tx, y + 38, metaC, m_fontSmall);

    y += itemH + 6;
  }
  int totalPages = (n + pageSize - 1) / pageSize;
  int curPage = (m_lsRomSelected / pageSize) + 1;
  char pgbuf[64];
  snprintf(pgbuf, sizeof(pgbuf), "%d / %d", curPage, totalPages);
  drawText(pgbuf, dlgX + dlgW - 90, dlgY + dlgH - 26, {100, 116, 139, 255},
           m_fontSmall);
}

void UIManager::renderLsAppsList(int dlgX, int dlgY, int dlgW, int dlgH) {
  int n = (int)m_lsAppList.size();
  if (n == 0)
    return;
  const int itemH = 50;
  const int padding = 4;
  int visibleCount = std::max(1, (dlgH - padding * 2) / itemH);

  if (m_lsAppSelected < m_lsAppScrollOffset)
    m_lsAppScrollOffset = m_lsAppSelected;
  if (m_lsAppSelected >= m_lsAppScrollOffset + visibleCount)
    m_lsAppScrollOffset = m_lsAppSelected - visibleCount + 1;
  if (m_lsAppScrollOffset < 0)
    m_lsAppScrollOffset = 0;
  if (m_lsAppScrollOffset > std::max(0, n - 1))
    m_lsAppScrollOffset = std::max(0, n - 1);

  int renderY = dlgY + padding;
  for (int i = m_lsAppScrollOffset;
       i < n && i < m_lsAppScrollOffset + visibleCount; ++i) {
    const auto &e = m_lsAppList[i];
    if (renderY + itemH > dlgY + dlgH - padding)
      break;

    bool sel = (i == m_lsAppSelected);
    if (sel) {
      drawRoundedRect(dlgX + 8, renderY, dlgW - 16, itemH - 4,
                      UiTheme::RADIUS_ROW, SDL_Color{59, 130, 246, 90}, true);
    }

    std::string icon = e.isDirectory ? "[DIR]" : "[FILE]";
    SDL_Color iconColor = e.isDirectory ? SDL_Color{250, 204, 21, 255}
                                        : SDL_Color{148, 163, 184, 255};
    drawText(icon, dlgX + 20, renderY + 14, iconColor, m_fontSmall, false);

    SDL_Color nameColor =
        sel ? SDL_Color{255, 255, 255, 255} : SDL_Color{226, 232, 240, 255};
    drawText(e.name, dlgX + 90, renderY + 4, nameColor, m_fontMedium, false);

    std::string sub = e.isDirectory ? std::string("Bấm A để mở")
                                    : LsUtil::humanSize(e.sizeBytes);
    drawText(sub, dlgX + 90, renderY + 28, {148, 163, 184, 255}, m_fontSmall,
             false);

    renderY += itemH;
  }

  drawText(std::to_string(m_lsAppSelected + 1) + "/" + std::to_string(n),
           dlgX + dlgW - 20, dlgY - 22, {148, 163, 184, 255}, m_fontSmall,
           true);
}

void UIManager::renderGameCastState() {
  // ─── Background + Header chuẩn (giống IPTV/YouTube/Explorer) ───
  // P4: bỏ custom sub-header strip cũ (Y=64..112) để đồng bộ với các app
  // khác. Cụm status bên phải (Wi-Fi/pin/clock) tự động render bởi
  // drawAppHeader() → khớp IPTV, YouTube, Explorer, Settings.
  drawAppBackground();
  drawAppHeader("GAME CAST");

  std::string ip = CastManager::instance().getIpAddress();
  std::string castUrl = CastManager::instance().getCastUrl();
  // P1-2: Phát hiện mất Wi-Fi — placeholder "192.168.x.x" do
  // CastManager::getIpAddress() trả về khi không lấy được IP thật. Lúc này
  // đổi badge sang cảnh báo đỏ + disable QR/URL/nút A để user không nhầm.
  bool noWifi = (ip == "192.168.x.x");
  bool running = CastManager::instance().isRunning();
  bool isHd = CastManager::instance().isHdMode();

  // ─── Layout 2 rows × 1 column (P4: chuẩn hóa theo app_conventions) ───
  // Row 1: trạng thái stream + độ phân giải + IP badge góc phải
  // Row 2: nguyên cụm kết nối TV cũ (QR + URL + stats + steps)
  const int rowX = 24;
  const int rowW = 1024 - 48; // 976 — full content width
  const int rowGap = 16;
  const int row1Y = 80;
  const int row1H = 140;
  const int row2Y = row1Y + row1H + rowGap; // 236
  const int row2H = 706 - row2Y;            // 470 (còn 9px đệm cuối)

  // ─── Row 1: Status + Resolution (1 cell, 2 dòng chính) ───────────
  drawRoundedRect(rowX, row1Y, rowW, row1H, UiTheme::RADIUS_MODAL,
                  {18, 24, 34, 255}, true);
  drawRoundedBorder(rowX, row1Y, rowW, row1H, UiTheme::RADIUS_MODAL,
                    {38, 48, 64, 255}, 1);

  // IP badge (top-right của Row 1)
  if (noWifi) {
    std::string warnTxt = "⚠ CHƯA CÓ Wi-Fi";
    int warnW = badgeWidth(warnTxt, 32);
    drawBadge(rowX + rowW - 24 - warnW, row1Y + 16, warnW, 32, warnTxt,
              {127, 29, 29, 255}, {252, 165, 165, 255});
  } else {
    int ipW = badgeWidth("IP: " + ip, 32);
    drawBadge(rowX + rowW - 24 - ipW, row1Y + 16, ipW, 32, "IP: " + ip,
              {30, 41, 59, 255}, {148, 163, 184, 255});
  }

  // Dòng 1: Trạng thái Stream
  std::string statusText;
  SDL_Color statusColor;
  if (noWifi) {
    statusText = "Trạng thái:  ⚠ MẤT Wi-Fi";
    statusColor = {252, 165, 165, 255};
  } else if (running) {
    statusText = "Trạng thái:  ● ĐANG PHÁT";
    statusColor = {34, 197, 94, 255};
  } else {
    statusText = "Trạng thái:  ○ SẴN SÀNG";
    statusColor = {160, 170, 185, 255};
  }
  drawText(statusText, rowX + 24, row1Y + 36, statusColor, m_fontLarge);

  // Dòng 2: Độ phân giải
  std::string resText = isHd ? "Độ phân giải:  1024×768 (Native HD)"
                             : "Độ phân giải:  512×384 (Smooth 60 FPS)";
  drawText(resText, rowX + 24, row1Y + 92, {200, 210, 225, 255}, m_fontMedium);

  // Row 2 dimensions used downstream
  int rightW = rowW; // alias for compatibility
  int rightX = rowX;
  int cardY = row2Y;

  // ─── Row 2: Kết nối TV & Laptop (1 cell, nguyên cụm bên phải cũ) ──
  // P4: thay Card 1 (left status) + Card 2 (right QR) cũ bằng 1 cell full
  // width chứa toàn bộ nội dung kết nối TV. Bỏ "Tính năng & Điểm nổi
  // bật" + 5 bullets + btnCard (chứa button hints duplicate với footer).
  drawRoundedRect(rowX, row2Y, rowW, row2H, UiTheme::RADIUS_MODAL,
                  {18, 24, 34, 255}, true);
  drawRoundedBorder(rowX, row2Y, rowW, row2H, UiTheme::RADIUS_MODAL,
                    {38, 48, 64, 255}, 1);

  drawText("KẾT NỐI TỪ TV & LAPTOP", rightX + rightW / 2, cardY + 22,
           {255, 255, 255, 255}, m_fontLarge, true);
  drawText("Quét QR hoặc Nhập URL:", rightX + rightW / 2, cardY + 58,
           {148, 163, 184, 255}, m_fontSmall, true);

  int qrSize = 180;
  int qrX = rightX + (rightW - qrSize) / 2;
  int qrY = cardY + 95;
  if (noWifi) {
    // P1-2: Thay QR bằng ô cảnh báo lớn để user hiểu vì sao không có IP.
    drawRoundedRect(qrX, qrY, qrSize, qrSize, UiTheme::RADIUS_CARD,
                    {38, 26, 26, 255}, true);
    drawRoundedBorder(qrX, qrY, qrSize, qrSize, UiTheme::RADIUS_CARD,
                      {127, 29, 29, 255}, 1);
    drawText("⚠", qrX + qrSize / 2, qrY + 38, {252, 165, 165, 255},
             m_fontMedium, true);
    drawText("Chưa có Wi-Fi", qrX + qrSize / 2, qrY + 78, {252, 165, 165, 255},
             m_fontLarge, true);
    drawText("Brick chưa kết nối", qrX + qrSize / 2, qrY + 110,
             {200, 160, 160, 255}, m_fontSmall, true);
    drawText("mạng Wi-Fi", qrX + qrSize / 2, qrY + 132, {200, 160, 160, 255},
             m_fontSmall, true);
  } else {
    QrRenderer::renderQrCode(m_renderer, castUrl, qrX, qrY, qrSize,
                             {0, 0, 0, 255}, {255, 255, 255, 255});
  }

  int urlY = qrY + qrSize + 18; // P4: tighter gap
  int urlW = rightW - 48;
  int urlX = rowX + 24;
  drawRoundedRect(urlX, urlY, urlW, 46, UiTheme::RADIUS_ROW, {30, 41, 59, 255},
                  true);
  drawRoundedBorder(
      urlX, urlY, urlW, 46, UiTheme::RADIUS_ROW,
      noWifi ? SDL_Color{127, 29, 29, 255} : SDL_Color{0, 180, 216, 255}, 1);
  std::string dispUrl = noWifi
                            ? std::string("IP sẽ hiện sau khi kết nối Wi-Fi")
                            : truncateToWidth(castUrl, m_fontMedium, urlW - 24);
  drawText(dispUrl, rightX + rightW / 2, urlY + 11,
           noWifi ? SDL_Color{100, 116, 139, 255} : SDL_Color{0, 230, 255, 255},
           m_fontMedium, true);

  // P1-2: realtime stats tu /api/status — hien thi khi stream dang chay va
  // da fetch thanh cong it nhat 1 lan (valid=true).
  bool showStats = running && !noWifi && m_castStats.valid.load();
  int statsY = urlY + 52; // text baseline (URL box cao 46, padding 6)
  int stepY = urlY + (showStats ? 78 : 58);
  if (showStats) {
    int fps = m_castStats.fps.load();
    int viewers = m_castStats.viewers.load();
    // P3-C: stream elapsed time (HH:MM:SS neu >=1h, MM:SS neu <1h).
    uint32_t elapsedMs = CastManager::instance().getStreamElapsedMs();
    uint32_t elapsedSec = elapsedMs / 1000;
    uint32_t h = elapsedSec / 3600;
    uint32_t m = (elapsedSec % 3600) / 60;
    uint32_t s = elapsedSec % 60;
    char statsBuf[96];
    if (h > 0) {
      std::snprintf(statsBuf, sizeof(statsBuf),
                    "FPS: %d  •  Viewers: %d  •  %02u:%02u:%02u", fps, viewers,
                    h, m, s);
    } else {
      std::snprintf(statsBuf, sizeof(statsBuf),
                    "FPS: %d  •  Viewers: %d  •  %02u:%02u", fps, viewers, m,
                    s);
    }
    SDL_Color fpsColor = (fps >= 55) ? SDL_Color{34, 197, 94, 255}
                         // xanh: full FPS
                         : (fps >= 30) ? SDL_Color{245, 158, 11, 255} // cam
                                       : SDL_Color{239, 68, 68, 255}; // do
    drawText(statsBuf, rightX + rightW / 2, statsY, fpsColor, m_fontSmall,
             true);
  }

  if (noWifi) {
    // P1-2: Wi-Fi mất — đổi steps thành hướng dẫn connect.
    drawText("1. Vào Settings → Wi-Fi để kết nối mạng", urlX, stepY,
             {252, 165, 165, 255}, m_fontSmall);
    drawText("2. Quay lại đây - IP & QR tự động cập nhật", urlX, stepY + 26,
             {160, 175, 195, 255}, m_fontSmall);
    drawText("3. Đảm bảo TV cùng Wi-Fi với Brick", urlX, stepY + 52,
             {160, 175, 195, 255}, m_fontSmall);
    drawText("4. Mở trình duyệt TV → Nhập URL → Stream", urlX, stepY + 78,
             {160, 175, 195, 255}, m_fontSmall);
  } else {
    drawText("1. Kết nối TV / Laptop cùng Wi-Fi với máy Brick", urlX, stepY,
             {160, 175, 195, 255}, m_fontSmall);
    drawText("2. Mở trình duyệt Web", urlX, stepY + 26, {160, 175, 195, 255},
             m_fontSmall);
    drawText("3. Nhập địa chỉ trên để Stream", urlX, stepY + 52,
             {160, 175, 195, 255}, m_fontSmall);
    drawText("4. Cắm Gamepad hoặc dùng bàn phím để chơi", urlX, stepY + 78,
             {160, 175, 195, 255}, m_fontSmall);
  }

  // ─── Footer chuẩn (drawAppFooter tự vẽ BG + line)
  // P4: footer A dùng nhãn "Bật/Tắt Cast" cố định — state hiện ở Row 1
  // (Trạng thái Stream). Đồng nhất 3 hint cho mọi state (noWifi/running/
  // idle) → layout footer ổn định, không bị thay đổi khi state đổi.
  using PB = UiTheme::PadBtn;
  std::vector<UiTheme::FooterHint> hints;
  hints.push_back({PB::A, "Bật/Tắt"});
  hints.push_back({PB::B, "Lùi"});
  hints.push_back({PB::X, "Chất lượng"});
  drawAppFooter(hints);
}

// P0-1 YT perf: pre-compute truncation/format strings 1 lần khi nhận results,
// thay vì split('|') + truncate mỗi frame. Đặt cuối file vì dùng các static
// helpers (split, formatDuration, formatViews, trimUtf8, wrapUtf8TwoLines) đã
// defined phía trên.
UIManager::YtItem UIManager::buildYtItemFromPipe(const std::string &raw) {
  YtItem it;

  // P0-3: detect CHANNEL| prefix from smart search backend
  // Format: "CHANNEL|<name>|<channelId>" (P0-6: trimmed — subs/vcount dropped
  // from wire payload to optimize ms of search output. C++ uses defaults if
  // older 5-field format arrives for backward compat.)
  if (raw.compare(0, 8, "CHANNEL|") == 0) {
    it.type = YtItem::Type::Channel;
    auto parts = split(raw.substr(8), '|');
    it.title = !parts.empty() ? parts[0] : "";
    it.channelId = parts.size() >= 2 ? parts[1] : "";
    // id = channelId (for selecting → navigate to channel's videos)
    it.id = it.channelId;

    // Channel row: single line title (channel name) — no subs/vcount meta
    it.titleL1 = truncateToWidth(it.title, m_fontMedium, 800);
    it.titleL2 = "";
    it.hasTitleL2 = false;
    it.channel = ""; // not needed for channel row

    // subs/vcount not in output anymore — keep empty for render fallback
    it.subscribersStr = "";
    it.videoCountStr = "";
    return it;
  }

  // Default: VIDEO format "id|title|duration|channel|views"
  auto parts = split(raw, '|');
  it.type = YtItem::Type::Video;
  it.id = !parts.empty() ? parts[0] : "";
  it.title = parts.size() >= 2 ? parts[1] : "";
  std::string dur = parts.size() >= 3 ? parts[2] : "";
  std::string chan = parts.size() >= 4 ? trimUtf8(parts[3]) : "";
  std::string views = parts.size() >= 5 ? parts[4] : "";

  auto lines = wrapUtf8TwoLines(it.title, 48);
  it.titleL1 = lines.first;
  it.titleL2 = lines.second;
  it.hasTitleL2 = !lines.second.empty();

  it.channel = truncateToWidth(chan, m_fontSmall, 320);
  it.viewsStr = formatViews(views);
  it.durationStr = formatDuration(dur);
  return it;
}

void UIManager::rebuildYtItems() {
  m_ytItems.clear();
  m_ytItems.reserve(m_ytSearchResults.size());
  for (const auto &raw : m_ytSearchResults) {
    m_ytItems.push_back(buildYtItemFromPipe(raw));
  }
  m_ytAllItems.clear();
  m_ytAllItems.reserve(m_ytAllCachedResults.size());
  for (const auto &raw : m_ytAllCachedResults) {
    m_ytAllItems.push_back(buildYtItemFromPipe(raw));
  }
}

// ============================================================================
// P0-6: YOUTUBE_HOME helpers — load/save/toggle view mode
// ============================================================================

void UIManager::loadYouTubeViewMode() {
  std::string v = DatabaseManager::instance().getSetting("yt_view_mode", "row");
  m_ytViewMode = (v == "grid");
}

void UIManager::saveYouTubeViewMode() {
  DatabaseManager::instance().setSetting("yt_view_mode",
                                         m_ytViewMode ? "grid" : "row");
}

void UIManager::toggleYouTubeViewMode() {
  m_ytViewMode = !m_ytViewMode;
  saveYouTubeViewMode();
  showToast(m_ytViewMode ? "Chế độ: Lưới 3×2" : "Chế độ: Danh sách dọc",
            UiTheme::ACCENT_GREEN, 1200);
}

std::string UIManager::currentCategoryFeedQuery() const {
  if (m_ytSelectedCategory < 0 ||
      m_ytSelectedCategory >= static_cast<int>(kYtCategories.size())) {
    return "";
  }
  const char *fq = kYtCategories[m_ytSelectedCategory].feedQuery;
  return (fq && fq[0] != '\0') ? std::string(fq) : std::string();
}

void UIManager::openYouTubeHomeModal() {
  m_ytVk.query = m_ytKeyboardQuery;
  m_ytSearchFocus = 2;
  m_ytHistoryRow = 0;
  m_ytHistoryCol = 0;
  m_ytSearchModalOpen = false;
  setState(UIState::YOUTUBE_SEARCH);
}

void UIManager::closeYouTubeHomeModal() {
  m_ytSearchModalOpen = false;
  if (m_currentState == UIState::YOUTUBE_SEARCH) {
    setState(UIState::YOUTUBE_HOME);
  }
}

void UIManager::renderYouTubeHomeModalOverlay() {
  if (!m_ytSearchModalOpen)
    return;
  SearchInputModal::render(m_ytHomeModalCfg);
  drawAppFooter({{UiTheme::PadBtn::A, "Chọn"},
                 {UiTheme::PadBtn::Y, "Xóa"},
                 {UiTheme::PadBtn::L1, "Hoa"},
                 {UiTheme::PadBtn::R1, "Telex"},
                 {UiTheme::PadBtn::START, "Tìm"},
                 {UiTheme::PadBtn::B, "Đóng"}});
}

void UIManager::renderYouTubeHomeState() {
  // ─── Background + logo ─────────────────────────────────────────────
  drawAppBackground();
  {
    std::string ytLogoPath =
        AppConfig::instance().getAssetsDir() + "/apps_icons/YOUTUBE.png";
    SDL_Texture *ytLogo = m_ui.getOrLoadImage("grid/YOUTUBE.png", ytLogoPath);
    int titleX = 24;
    if (ytLogo) {
      int texW = 0, texH = 0;
      SDL_QueryTexture(ytLogo, nullptr, nullptr, &texW, &texH);
      int logoH = 40, logoW = 40;
      if (texH > 0)
        logoW = (texW * logoH) / texH;
      int slx = PlatformInfo::instance().scaleX(24);
      int sly =
          PlatformInfo::instance().scaleY((UiTheme::HEADER_H - logoH) / 2);
      int slw = PlatformInfo::instance().scaleW(logoW);
      int slh = PlatformInfo::instance().scaleH(logoH);
      SDL_Rect dst = {slx, sly, slw, slh};
      SDL_RenderCopy(m_renderer, ytLogo, nullptr, &dst);
      titleX = 24 + logoW + 10;
    }
    std::string ytTitle = "YouTube";
    if (!m_ytKeyboardQuery.empty()) {
      ytTitle += ": " + m_ytKeyboardQuery;
    } else if (m_ytSelectedCategory >= 0 &&
               m_ytSelectedCategory < static_cast<int>(kYtCategories.size())) {
      ytTitle += " - " + std::string(kYtCategories[m_ytSelectedCategory].label);
    }
    ytTitle = truncateToWidth(ytTitle, m_fontLarge, UiTheme::APP_W - 24 - 300);
    drawText(ytTitle, titleX, textYCentered(0, UiTheme::HEADER_H, m_fontLarge),
             UiTheme::ACCENT_CYAN, m_fontLarge);
  }
  drawHeaderStatus();
  drawText("P0.6", UiTheme::APP_W - 50, 70, UiTheme::TEXT_FAINT, m_fontSmall,
           true);

  // ─── Zone 0: Search input field ─────────────────────────────────
  int inX = 36;
  int inY = 74;
  int inW = 952;
  int inH = 52;
  bool isSearchFocus = (m_ytHomeFocus == 0);
  SDL_Color inBg =
      isSearchFocus ? SDL_Color{28, 44, 62, 245} : SDL_Color{16, 24, 34, 200};
  drawRoundedRect(inX, inY, inW, inH, UiTheme::RADIUS_CARD, inBg, true);

  // Search input content:
  // - KHÔNG hiển thị nội dung input nếu đang không trong trạng thái highlight
  // (m_ytHomeFocus != 0)
  // - Chỉ hiển thị với các input được gõ từ keyboard (m_ytKeyboardQuery)
  // - Bỏ text Lưới 3x2 đi
  std::string dispQ;
  SDL_Color qCol;
  if (isSearchFocus) {
    if (!m_ytKeyboardQuery.empty()) {
      dispQ = m_ytKeyboardQuery + " _";
      qCol = UiTheme::TEXT_MAIN;
    } else {
      dispQ = "Nhấn A để nhập từ khóa...";
      qCol = SDL_Color{75, 115, 135, 255};
    }
  } else {
    // Không trong trạng thái highlight: không hiển thị nội dung input
    dispQ = "Tìm kiếm video...";
    qCol = SDL_Color{50, 75, 95, 180};
  }
  dispQ = truncateToWidth(dispQ, m_fontLarge, inW - 40);
  drawText(dispQ, inX + 20, inY + (inH - textHeight(m_fontLarge)) / 2, qCol,
           m_fontLarge, false);

  // ─── Zone 1: Pills / Tags area ──────────────────────────────────
  const int tagBoxX = 36;
  const int tagBoxY = 136;
  const int tagBoxW = 952;

  const int pillY = tagBoxY + 7;
  const int pillH = 34;
  const int pillGap = 10;
  const int pillAreaX = tagBoxX + 10;
  const int pillAreaW = tagBoxW - 20;

  std::vector<int> pillWidths;
  int totalPillsW = 0;
  for (size_t i = 0; i < kYtCategories.size(); i++) {
    int tw = textWidth(kYtCategories[i].label, m_fontSmall) + 28;
    pillWidths.push_back(tw);
    totalPillsW += tw;
    if (i + 1 < kYtCategories.size())
      totalPillsW += pillGap;
  }

  int baseX;
  if (totalPillsW <= pillAreaW) {
    baseX = pillAreaX + (pillAreaW - totalPillsW) / 2;
  } else {
    int selOffset = 0;
    for (int i = 0; i < m_ytSelectedCategory; i++)
      selOffset += pillWidths[i] + pillGap;
    int selW = pillWidths[m_ytSelectedCategory];
    if (selOffset < m_ytCategoryScrollOffset) {
      m_ytCategoryScrollOffset = selOffset;
    } else if (selOffset + selW > m_ytCategoryScrollOffset + pillAreaW) {
      m_ytCategoryScrollOffset = selOffset + selW - pillAreaW;
    }
    if (m_ytCategoryScrollOffset < 0)
      m_ytCategoryScrollOffset = 0;
    int maxOffset = std::max(0, totalPillsW - pillAreaW);
    if (m_ytCategoryScrollOffset > maxOffset)
      m_ytCategoryScrollOffset = maxOffset;
    baseX = pillAreaX - m_ytCategoryScrollOffset;
  }

  int curX = baseX;
  for (size_t i = 0; i < kYtCategories.size(); i++) {
    int w = pillWidths[i];
    bool isSel = (static_cast<int>(i) == m_ytSelectedCategory);
    if (isSel) {
      drawRoundedRect(curX, pillY, w, pillH, pillH / 2,
                      SDL_Color{255, 255, 255, 255}, true);
      drawText(kYtCategories[i].label, curX + w / 2,
               pillY + (pillH - textHeight(m_fontSmall)) / 2,
               SDL_Color{20, 20, 20, 255}, m_fontSmall, true);
    } else {
      drawRoundedRect(curX, pillY, w, pillH, pillH / 2,
                      SDL_Color{28, 38, 50, 230}, true);
      drawRoundedBorder(curX, pillY, w, pillH, pillH / 2,
                        SDL_Color{46, 64, 80, 255}, 1);
      drawText(kYtCategories[i].label, curX + w / 2,
               pillY + (pillH - textHeight(m_fontSmall)) / 2, UiTheme::TEXT_SUB,
               m_fontSmall, true);
    }
    curX += w + pillGap;
  }

  // Dot indicator
  int dotY = pillY + pillH + 5;
  int totalDots = static_cast<int>(kYtCategories.size());
  int dotGap = 8;
  int dotW = (totalDots - 1) * dotGap + 4;
  int dotStartX = (UiTheme::APP_W - dotW) / 2;
  for (int i = 0; i < totalDots; i++) {
    bool isActive = (i == m_ytSelectedCategory);
    drawRect(dotStartX + i * dotGap, dotY, 4, 4,
             isActive ? UiTheme::ACCENT_GREEN : SDL_Color{60, 80, 100, 200},
             true);
  }

  // ─── Zone 2: Content (results / thumbnails) area ────────────────
  const int contentTop = 204;
  const int contentH = 711 - contentTop;
  int totalResults = static_cast<int>(m_ytSearchResults.size());

  if (m_ytIsSearching && totalResults == 0) {
    drawText("Đang tải...", UiTheme::APP_W / 2, contentTop + contentH / 2,
             UiTheme::TEXT_SUB, m_fontMedium, true);
  } else if (totalResults == 0) {
    drawText("Chọn danh mục để bắt đầu xem.", UiTheme::APP_W / 2,
             contentTop + 40, UiTheme::TEXT_SUB, m_fontMedium, true);
    drawText("D-pad L/R: chuyển danh mục • A/X: tìm kiếm", UiTheme::APP_W / 2,
             contentTop + 80, UiTheme::TEXT_FAINT, m_fontSmall, true);
  } else {
    if (m_ytMatchedChannel.matched && !m_ytInChannelView)
      renderYouTubeHomeChannelLayout(contentTop, contentH);
    else
      renderYouTubeHomeContentGrid(contentTop, contentH);
  }

  // Footer (4 hàng: 0=search, 1=tag, 2=hàng 3, 3=hàng 4)
  if (m_ytHomeRow == 0) {
    drawAppFooter({{UiTheme::PadBtn::A, "Tìm kiếm"},
                   {UiTheme::PadBtn::B, "Thoát"},
                   {UiTheme::PadBtn::UPDOWN, "Đổi hàng"},
                   {UiTheme::PadBtn::START, "Tìm nhanh"}});
  } else if (m_ytHomeRow == 1) {
    drawAppFooter({{UiTheme::PadBtn::A, "Danh mục"},
                   {UiTheme::PadBtn::B, "Thoát"},
                   {UiTheme::PadBtn::DPAD, "Chọn mục"},
                   {UiTheme::PadBtn::START, "Tìm kiếm"}});
  } else if (m_ytHomeRow == 2) {
    if (m_ytMatchedChannel.matched && !m_ytInChannelView) {
      drawAppFooter({{UiTheme::PadBtn::A, "Xem kênh"},
                     {UiTheme::PadBtn::B, "Thoát"},
                     {UiTheme::PadBtn::Y, "Kiểu xem"},
                     {UiTheme::PadBtn::START, "Tìm kiếm"}});
    } else {
      drawAppFooter(
          {{UiTheme::PadBtn::A, "Phát"},
           {UiTheme::PadBtn::B, m_ytInChannelView ? "Quay lại" : "Thoát"},
           {UiTheme::PadBtn::Y, "Kiểu xem"},
           {UiTheme::PadBtn::START, "Tìm kiếm"},
           {UiTheme::PadBtn::L1R1, "Trang"}});
    }
  } else {
    drawAppFooter(
        {{UiTheme::PadBtn::A, "Phát"},
         {UiTheme::PadBtn::B, m_ytInChannelView ? "Quay lại" : "Thoát"},
         {UiTheme::PadBtn::Y, "Kiểu xem"},
         {UiTheme::PadBtn::START, "Tìm kiếm"},
         {UiTheme::PadBtn::L1R1, "Trang"}});
  }

  // Modal overlay renders above base + footer. Khi modal mở, beginModalDim()
  // sẽ dim cả base; footer bên dưới bị dim → modal vẽ footer của nó lên cùng.
  if (m_ytSearchModalOpen) {
    renderYouTubeHomeModalOverlay();
  }
}

// (Footer ở trên đã được dim khi modal mở → modal vẽ footer riêng)

void UIManager::renderYouTubeHomeContentRow(int contentTop, int /*contentH*/) {
  // 5 rows/page, list vertical, thumb trái 16:9 + info phải
  const int itemsPerPage = 5;
  int totalResults = static_cast<int>(m_ytSearchResults.size());
  int pageStartIndex = (m_ytHomeContentSelected / itemsPerPage) * itemsPerPage;

  int rowX = 24;
  int rowW = UiTheme::APP_W - 48;
  int rowH = 92;
  int gapY = 8;
  int startX = rowX;
  int startY = contentTop + 8;

  int thumbW = 140;
  int thumbH = 78;

  for (int i = 0; i < itemsPerPage; i++) {
    int idx = pageStartIndex + i;
    if (idx >= totalResults)
      break;
    int cy = startY + i * (rowH + gapY);
    bool isSelected = (idx == m_ytHomeContentSelected);
    bool isChannelRow = (idx < static_cast<int>(m_ytItems.size()) &&
                         m_ytItems[idx].type == YtItem::Type::Channel);

    SDL_Color rowBg;
    if (isChannelRow)
      rowBg =
          isSelected ? SDL_Color{40, 30, 60, 255} : SDL_Color{24, 18, 38, 255};
    else
      rowBg = isSelected ? UiTheme::CARD_SOLID : SDL_Color{22, 28, 38, 220};
    drawRoundedRect(startX, cy, rowW, rowH, UiTheme::RADIUS_ROW, rowBg, true);
    if (isSelected) {
      drawRoundedBorder(startX, cy, rowW, rowH, UiTheme::RADIUS_ROW,
                        UiTheme::FOCUS_GLOW, 2);
    }

    int thumbX = startX + 10;
    int thumbY = cy + (rowH - thumbH) / 2;
    if (isChannelRow) {
      std::string chName = (idx < static_cast<int>(m_ytItems.size()))
                               ? m_ytItems[idx].title
                               : "";
      std::string firstChar = chName.empty() ? "?" : utf8FirstChar(chName);
      drawRoundedRect(thumbX, thumbY, thumbW, thumbH, 6,
                      SDL_Color{12, 18, 28, 255}, true);
      drawText(firstChar, thumbX + thumbW / 2, thumbY + thumbH / 2 - 8,
               UiTheme::ACCENT_CYAN, m_fontLarge, true);
    } else {
      std::string raw = m_ytSearchResults[idx];
      size_t p = raw.find('|');
      std::string vid = (p != std::string::npos) ? raw.substr(0, p) : "";
      drawYtStandardThumb(thumbX, thumbY, thumbW, thumbH, vid, isSelected,
                          /*scaledThumb=*/false);
    }

    int infoX = thumbX + thumbW + 14;
    int infoW = startX + rowW - infoX - 12;
    if (idx < static_cast<int>(m_ytItems.size())) {
      const auto &item = m_ytItems[idx];
      if (!isChannelRow) {
        // Chuan: 2 dong info (ten video + duration - views), can giua deu voi
        // thumb
        int lh = textHeight(m_fontSmall);
        int step = lh + 4;
        int infoY = thumbY + (thumbH - (lh * 2 + 4)) / 2;
        drawYtStandardMeta(infoX, infoY, infoW, step, item);
      } else {
        drawText("Kênh • A để xem video mới nhất", infoX, cy + (rowH - 18) / 2,
                 UiTheme::TEXT_SUB, m_fontSmall, false);
      }
    }
  }
}

void UIManager::renderYouTubeHomeContentGrid(int contentTop, int contentH) {
  // 3 cols × 2 rows = 6 cards
  const int cols = 3;
  const int rows = 2;
  const int itemsPerPage = cols * rows;
  int totalResults = static_cast<int>(m_ytSearchResults.size());
  int pageStartIndex = (m_ytHomeContentSelected / itemsPerPage) * itemsPerPage;

  int gridX = 24;
  int gridW = UiTheme::APP_W - 48;
  int gridGap = 12;
  int cardW = (gridW - gridGap * (cols - 1)) / cols;
  int cardH = (contentH - 16 - gridGap * (rows - 1)) / rows;

  for (int i = 0; i < itemsPerPage; i++) {
    int idx = pageStartIndex + i;
    if (idx >= totalResults)
      break;
    int col = i % cols;
    int row = i / cols;
    int cx = gridX + col * (cardW + gridGap);
    int cy = contentTop + 8 + row * (cardH + gridGap);
    bool isSel = (m_ytHomeRow >= 2 && idx == m_ytHomeContentSelected);
    bool isChannelRow = (idx < static_cast<int>(m_ytItems.size()) &&
                         m_ytItems[idx].type == YtItem::Type::Channel);

    SDL_Color cardBg =
        isSel ? SDL_Color{38, 50, 70, 255} : SDL_Color{20, 26, 36, 220};
    drawRoundedRect(cx, cy, cardW, cardH, 10, cardBg, true);

    int thumbW = cardW - 12;
    int thumbH = thumbW * 9 / 16;
    int thumbX = cx + 6;
    int thumbY = cy + 6;
    if (isChannelRow) {
      std::string chName = (idx < static_cast<int>(m_ytItems.size()))
                               ? m_ytItems[idx].title
                               : "";
      std::string firstChar = chName.empty() ? "?" : utf8FirstChar(chName);
      drawRoundedRect(thumbX, thumbY, thumbW, thumbH, 6,
                      SDL_Color{12, 18, 28, 255}, true);
      drawText(firstChar, thumbX + thumbW / 2, thumbY + thumbH / 2 - 8,
               UiTheme::ACCENT_CYAN, m_fontLarge, true);
    } else {
      std::string raw = m_ytSearchResults[idx];
      size_t p = raw.find('|');
      std::string vid = (p != std::string::npos) ? raw.substr(0, p) : "";
      drawYtStandardThumb(thumbX, thumbY, thumbW, thumbH, vid, isSel,
                          /*scaledThumb=*/false);
    }

    // Info area: chuẩn 2 dòng canh trái, spacing gọn (4/2).
    int infoX = cx + 8;
    int infoW = cardW - 16;
    int infoY = thumbY + thumbH + 4;
    if (idx < static_cast<int>(m_ytItems.size())) {
      const auto &item = m_ytItems[idx];
      if (!isChannelRow) {
        drawYtStandardMeta(infoX, infoY, infoW, textHeight(m_fontSmall) + 2,
                           item);
      } else {
        drawText(truncateToWidth(item.title, m_fontSmall, infoW), infoX, infoY,
                 UiTheme::ACCENT_GREEN, m_fontSmall, false);
        drawText("Kênh • A để xem video mới nhất", infoX, infoY + 18,
                 UiTheme::TEXT_SUB, m_fontSmall, false);
      }
    }
  }
}

void UIManager::renderYouTubeHomeChannelLayout(int contentTop, int contentH) {
  (void)contentH;
  const int cols = 3;
  const int rows = 2;
  int gridX = 24;
  int gridW = UiTheme::APP_W - 48; // 976
  int gridGap = 12;
  int cardW = (gridW - gridGap * (cols - 1)) / cols;         // 317px
  int cardH = (contentH - 16 - gridGap * (rows - 1)) / rows; // 239px

  // HÀNG 3: Channel Card (2 Cột: Cột 0 rộng 1 col = 317px, Cột 1 rộng 2 cols =
  // 646px)
  int col1W = cardW;
  int col2W = cardW * 2 + gridGap;
  int col1X = gridX;
  int col2X = gridX + cardW + gridGap;
  int channelCardH = cardH;
  int channelCardY = contentTop + 8;

  // Cột 0 (Bên trái): Chỉ có vòng tròn Logo kênh căn giữa
  bool isCol0Sel = (m_ytHomeRow == 2 && m_ytHomeCol == 0);
  SDL_Color c1Bg =
      isCol0Sel ? SDL_Color{36, 48, 68, 255} : SDL_Color{20, 26, 36, 220};
  drawRoundedRect(col1X, channelCardY, col1W, channelCardH, 10, c1Bg, true);

  // Avatar kênh — path & cache key PER-CHANNEL (hash avatarUrl).
  // Trước đây dùng "/tmp/yt_thumbs/ch_avatar.jpg" + key "__ch_avatar__" cố
  // định → khi search kênh B, cache vẫn trả texture kênh A (vì key trùng).
  // Per-channel hash tự động tách key khi URL đổi.
  std::string chHash = m_ytMatchedChannel.avatarUrl.empty()
                           ? std::string("empty")
                           : ytAvatarHash(m_ytMatchedChannel.avatarUrl);
  std::string chAvatarKey = "__ch_avatar__" + chHash;
  std::string chAvatarPath = "/tmp/yt_thumbs/ch_" + chHash + ".jpg";
  SDL_Texture *chAvatarTex = nullptr;
  struct stat chSt;
  // Size > 500: chống IMG_Load file partial khi curl đang ghi dở.
  if (stat(chAvatarPath.c_str(), &chSt) == 0 && chSt.st_size > 500) {
    chAvatarTex = m_ytThumbCache.get(chAvatarKey);
    if (!chAvatarTex) {
      SDL_Surface *surf = IMG_Load(chAvatarPath.c_str());
      if (surf) {
        chAvatarTex = SDL_CreateTextureFromSurface(m_renderer, surf);
        SDL_FreeSurface(surf);
        if (chAvatarTex) {
          m_ytThumbCache.put(chAvatarKey, chAvatarTex);
        }
      }
    }
  }

  // Avatar kênh gốc (vuông, không vòng tròn), x2 = 208px
  int avSize = 208;
  int avX = col1X + (col1W - avSize) / 2;
  int avY = channelCardY + (channelCardH - avSize) / 2;
  if (chAvatarTex) {
    SDL_Rect avatarDst = {avX, avY, avSize, avSize};
    SDL_RenderCopy(m_renderer, chAvatarTex, nullptr, &avatarDst);
  } else {
    SDL_Color letterBg =
        isCol0Sel ? SDL_Color{0, 160, 216, 255} : SDL_Color{28, 42, 60, 255};
    drawRoundedRect(avX, avY, avSize, avSize, 10, letterBg, true);
    std::string firstChar = m_ytMatchedChannel.name.empty()
                                ? "K"
                                : utf8FirstChar(m_ytMatchedChannel.name);
    SDL_Color textCol =
        isCol0Sel ? SDL_Color{255, 255, 255, 255} : UiTheme::ACCENT_CYAN;
    drawText(firstChar, avX + avSize / 2, avY + avSize / 2 - 14, textCol,
             m_fontLarge, true);
  }

  if (isCol0Sel) {
    drawRoundedBorder(avX, avY, avSize, avSize, 10, UiTheme::FOCUS_GLOW, 3);
  }

  // Cột 1 (Bên phải): Tên kênh + Lượng subscribers + Vcount
  bool isCol1Sel = (m_ytHomeRow == 2 && m_ytHomeCol == 1);
  SDL_Color c2Bg =
      isCol1Sel ? SDL_Color{36, 48, 68, 255} : SDL_Color{20, 26, 36, 220};
  drawRoundedRect(col2X, channelCardY, col2W, channelCardH, 10, c2Bg, true);

  int infoX = col2X + 36;
  int infoY = channelCardY + (channelCardH - 86) / 2;
  // Dòng 1: Tên kênh
  std::string chName =
      truncateToWidth(m_ytMatchedChannel.name, m_fontLarge, col2W - 72);
  drawText(chName, infoX, infoY, UiTheme::TEXT_MAIN, m_fontLarge, false);

  // Dòng 2: Lượng subscribers
  std::string subs = m_ytMatchedChannel.subscribers.empty()
                         ? "Kênh YouTube"
                         : m_ytMatchedChannel.subscribers;
  subs = truncateToWidth(subs, m_fontMedium, col2W - 72);
  drawText(subs, infoX, infoY + 36, UiTheme::ACCENT_CYAN, m_fontMedium, false);

  // Dòng 3: Tổng số video (vcount)
  if (!m_ytMatchedChannel.videoCount.empty()) {
    std::string vc =
        truncateToWidth(m_ytMatchedChannel.videoCount, m_fontSmall, col2W - 72);
    drawText(vc, infoX, infoY + 66, UiTheme::TEXT_SUB, m_fontSmall, false);
  }

  // HÀNG 4: 3 Video mới nhất của Kênh (chuẩn vị trí Y = 463px, H = 239px khớp
  // 100% hàng dưới Grid)
  int vidY = contentTop + 8 + (cardH + gridGap);
  int vidH = cardH;
  int maxVidCols = std::min(3, static_cast<int>(m_ytSearchResults.size()));

  for (int col = 0; col < 3; col++) {
    int cx = gridX + col * (cardW + gridGap);
    if (col >= maxVidCols) {
      drawRoundedRect(cx, vidY, cardW, vidH, 10, SDL_Color{16, 20, 28, 140},
                      true);
      continue;
    }

    bool isVidSel = (m_ytHomeRow == 3 && m_ytHomeCol == col);
    SDL_Color vBg =
        isVidSel ? SDL_Color{38, 50, 70, 255} : SDL_Color{20, 26, 36, 220};
    drawRoundedRect(cx, vidY, cardW, vidH, 10, vBg, true);

    int thumbW = cardW - 12;
    int thumbH = thumbW * 9 / 16;
    int thumbX = cx + 6;
    int thumbY = vidY + 6;

    std::string raw = m_ytSearchResults[col];
    size_t p = raw.find('|');
    std::string vid = (p != std::string::npos) ? raw.substr(0, p) : "";
    drawYtStandardThumb(thumbX, thumbY, thumbW, thumbH, vid, isVidSel,
                        /*scaledThumb=*/false);

    // Info: chuẩn 2 dòng canh trái, spacing gọn (4/2).
    int vInfoX = cx + 8;
    int vInfoW = cardW - 16;
    int vInfoY = thumbY + thumbH + 4;
    if (col < static_cast<int>(m_ytItems.size())) {
      drawYtStandardMeta(vInfoX, vInfoY, vInfoW, textHeight(m_fontSmall) + 2,
                         m_ytItems[col]);
    }
  }
}

void UIManager::runYouTubeHomeSearch(const std::string &query) {
  if (query.empty())
    return;
  if (m_ytIsSearching)
    return;
  m_ytIsSearching = true;
  m_ytSearchStartMs = SDL_GetTicks();
  m_ytCurrentPage = 1;
  m_ytHomePage = 1;
  m_ytLastSearchQuery = query;
  m_ytAllCachedResults.clear();
  clearThumbnailCache();
  showToast("Đang tìm...", UiTheme::ACCENT_CYAN, 1500);

  m_ytSearchTask.run([this, query](TaskProgress &) {
    std::string appRoot = AppConfig::instance().getAppRoot();
    if (appRoot.empty())
      appRoot = "/mnt/SDCARD/Apps/RomCloud";
    std::string escaped;
    for (char c : query) {
      if (c == '"' || c == '\\' || c == '$' || c == '`')
        escaped += '\\';
      escaped += c;
    }
    std::string cmd = "LD_LIBRARY_PATH=/mnt/SDCARD/System/lib:$LD_LIBRARY_PATH "
                      "SSL_CERT_FILE=/mnt/SDCARD/System/lib/python3.11/"
                      "site-packages/pip/_vendor/certifi/cacert.pem "
                      "/mnt/SDCARD/System/bin/python3 \"" +
                      appRoot + "/scripts/youtube_search.py\" smart \"" +
                      escaped + "\" 24 2>/tmp/yt_search_err.log";
    FILE *fp = popen(cmd.c_str(), "r");
    std::vector<std::string> resultsRaw;
    if (fp) {
      char buf[2048];
      while (fgets(buf, sizeof(buf), fp) != nullptr) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
          line.pop_back();
        if (line.empty())
          continue;
        if (line.find("ERROR:") == 0 || line.find("WARNING:") == 0)
          continue;
        if (line.find("WARN:") == 0)
          continue;
        resultsRaw.push_back(line);
      }
      pclose(fp);
    }
    if (resultsRaw.empty()) {
      std::string err = ytErrTail("/tmp/yt_search_err.log");
      Logger::warn("[YouTube] home search empty, query='" + query + "'" +
                   (err.empty() ? "" : " err=" + err));
    }
    {
      std::lock_guard<std::mutex> lk(m_ytSearchMutex);
      m_ytPendingSearchResults = std::move(resultsRaw);
      m_ytSearchDataReady = true;
    }
  });
}

void UIManager::applyYouTubeSearchResults(std::vector<std::string> results) {
  m_ytAllCachedResults = std::move(results);
  m_ytMatchedChannel = YtMatchedChannel{};
  m_ytInChannelView = false;
  if (!m_ytAllCachedResults.empty() &&
      m_ytAllCachedResults[0].rfind("CHANNEL|", 0) == 0) {
    std::stringstream ss(m_ytAllCachedResults[0]);
    std::string tag, name, subs, vcount, avatarUrl;
    std::getline(ss, tag, '|');
    std::getline(ss, name, '|');
    std::getline(ss, subs, '|');
    std::getline(ss, vcount, '|');
    std::getline(ss, avatarUrl, '|');
    m_ytMatchedChannel.matched = true;
    m_ytMatchedChannel.name = name;
    m_ytMatchedChannel.subscribers = subs;
    m_ytMatchedChannel.videoCount = vcount;
    m_ytMatchedChannel.avatarUrl = avatarUrl;
    m_ytAllCachedResults.erase(m_ytAllCachedResults.begin());
    if (m_ytAllCachedResults.size() > 24)
      m_ytAllCachedResults.resize(24);
    m_ytChannelAllVideos = m_ytAllCachedResults;

    if (!avatarUrl.empty()) {
      std::string chHash = ytAvatarHash(avatarUrl);
      std::string avatarPath = "/tmp/yt_thumbs/ch_" + chHash + ".jpg";
      system("mkdir -p /tmp/yt_thumbs 2>/dev/null");
      std::string curlCmd = "curl -4 -k -sL -m 5 \"" + avatarUrl + "\" -o \"" +
                            avatarPath + "\" &";
      system(curlCmd.c_str());
    }
  } else {
    if (m_ytAllCachedResults.size() > 24)
      m_ytAllCachedResults.resize(24);
    m_ytChannelAllVideos.clear();
  }

  // Trang channel (query 'channel:<id>'): list full 6 video/trang ngay tu dau.
  // Search thuong co matched channel: giu 3 video (3 o HÀNG 3 la video moi
  // nhat cua kenh, layout card mac dinh).
  bool isChannelQuery = m_ytLastSearchQuery.rfind("channel:", 0) == 0;
  int end =
      (m_ytMatchedChannel.matched && !isChannelQuery)
          ? std::min<int>(3, static_cast<int>(m_ytAllCachedResults.size()))
          : std::min<int>(6, static_cast<int>(m_ytAllCachedResults.size()));
  m_ytSearchResults.assign(m_ytAllCachedResults.begin(),
                           m_ytAllCachedResults.begin() + end);
  m_ytSearchSelectedIndex = 0;
  m_ytHomeContentSelected = 0;
  m_ytHomeCol = 0;
  m_ytSearchScrollOffset = 0;
  m_ytHomeContentScrollOffset = 0;
  rebuildYtItems();
  std::vector<std::string> vids;
  for (const auto &item : m_ytSearchResults) {
    size_t p = item.find('|');
    if (p != std::string::npos)
      vids.push_back(item.substr(0, p));
  }
  startThumbnailDownloads(vids);
  m_ytIsSearching = false;
  m_ytSearchFinished = true;
  // HOME về rỗng: báo rõ thay vì để trắng (feed/API lỗi câm trước đây).
  if (m_ytAllCachedResults.empty() &&
      m_currentState == UIState::YOUTUBE_HOME) {
    showToast("Không tải được nội dung. Kiểm tra mạng.", {245, 158, 11, 255},
              3500);
    Logger::warn("[YouTube] HOME empty results, query='" +
                 m_ytLastSearchQuery + "'");
  }
  // Tải trước URL video đầu lúc rảnh: bấm A là phát ngay.
  if (!vids.empty()) preloadYouTubeStreamUrl(vids[0]);
#ifdef __GLIBC__
  malloc_trim(0);
#endif
}

} // namespace RomCloud
