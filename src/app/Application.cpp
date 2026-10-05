#include "Application.h"
#include "../auth/AuthManager.h"
#include "../calendar/CalManager.h"
#include "../clock/ClockStore.h"
#include "../config/AppConfig.h"
#include "../database/DatabaseManager.h"
#include "../database/RomIndexer.h"
#include "../diagnostics/DeviceIdentity.h"
#include "../download/DownloadManager.h"
#include "../filesystem/FileSystemManager.h"
#include "../input/InputManager.h"
#include "../localsend/LocalSendManager.h"
#include "../logging/Logger.h"
#include "../logging/IssueLogger.h"
#include "../network/HttpClient.h"
#include "../network/WebServer.h"
#include "../ota/UpdateManager.h"
#include "../platform/PlatformInfo.h"
#include "../sync/DriveSyncEngine.h"
#include "../ui/UIManager.h"


#include <chrono>
#include <csignal>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>


#ifdef _WIN32
#define sync() (void)0
#endif

#ifdef PC_SIMULATOR_MODE
#include <iostream>
#endif

namespace RomCloud {

static void signalHandler(int signum) {
  std::string sigName;
  switch (signum) {
    case SIGSEGV: sigName = "SIGSEGV (Segmentation Fault)"; break;
    case SIGABRT: sigName = "SIGABRT (Abort)"; break;
    case SIGFPE:  sigName = "SIGFPE (Floating Point Exception)"; break;
    case SIGILL:  sigName = "SIGILL (Illegal Instruction)"; break;
    case SIGBUS:  sigName = "SIGBUS (Bus Error)"; break;
    default:      sigName = "Signal " + std::to_string(signum); break;
  }

  Logger::error("CRASH: " + sigName + " caught, attempting to report...");

  // Send crash report if enabled
  IssueLogger::instance().logCrash(sigName, "");

  Logger::info("Crash report sent, requesting shutdown...");
  Application::instance().requestExit();
}

Application &Application::instance() {
  static Application instance;
  return instance;
}

void Application::requestExit() { m_running = false; }

bool Application::initSDL() {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK |
               SDL_INIT_EVENTS | SDL_INIT_AUDIO) < 0) {
    Logger::error(std::string("SDL_Init failed: ") + SDL_GetError());
    return false;
  }

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");

#ifdef PC_SIMULATOR_MODE
  // PC Simulator: Windowed mode with title bar
  m_window =
      SDL_CreateWindow("RomCloud Simulator [1024x768]", SDL_WINDOWPOS_CENTERED,
                       SDL_WINDOWPOS_CENTERED, 1024, 768,
                       SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  std::cout << "=== PC SIMULATOR MODE ===" << std::endl;
  std::cout << "Controls: Arrow keys=D-pad, Enter=A, ESC=B, X=X, Y=Y, Q=L1, "
               "E=R1, 1=L2, 2=R2, TAB=SELECT, F1=START"
            << std::endl;
  std::cout << "========================" << std::endl;
#else
  // TrimUI: Fullscreen mode
  m_window = SDL_CreateWindow("RomCloud", SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED, 1024, 768,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
#endif

  if (!m_window) {
    Logger::error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    return false;
  }

  m_renderer = SDL_CreateRenderer(
      m_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

  if (!m_renderer) {
    Logger::warn(std::string("Hardware accelerated renderer failed, falling "
                             "back to software: ") +
                 SDL_GetError());
    m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_SOFTWARE);
    if (!m_renderer) {
      Logger::error(
          std::string("SDL_CreateRenderer software fallback also failed: ") +
          SDL_GetError());
      return false;
    }
  }

  // Lock logical rendering size to 1024x768 so SDL automatically scales UI
  // preserving aspect ratio
  SDL_RenderSetLogicalSize(m_renderer, 1024, 768);

#ifdef PC_SIMULATOR_MODE
  SDL_ShowCursor(SDL_ENABLE);
#else
  SDL_ShowCursor(SDL_DISABLE);
#endif

  int w = 0, h = 0;
  SDL_GetRendererOutputSize(m_renderer, &w, &h);
  if (w <= 0 || h <= 0) {
    SDL_GetWindowSize(m_window, &w, &h);
  }
  if (w > 0 && h > 0) {
    PlatformInfo::instance().setDisplayMetrics(w, h);
  }
  Logger::info("Display window created successfully: " + std::to_string(w) +
               "x" + std::to_string(h));
  return true;
}

bool Application::init(int argc, char *argv[]) {
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  if (argc > 1 && argv[1] != nullptr) {
    AppConfig::instance().setAppRoot(std::string(argv[1]));
  }

  if (!FileSystemManager::instance().initializeAppDirectories()) {
    return false;
  }

  // Logger::init() - just opens file + stores version/commit. Caller will dump header.
  Logger::instance().init(AppConfig::instance().getDebugLogPath(),
                          APP_VERSION, GIT_COMMIT_HASH);

  // Initialize Device Identity (for diagnostics and issue reporting)
  std::string deviceId = DeviceIdentity::instance().getDeviceId();
  std::string idSource = DeviceIdentity::instance().getIdSource();
  Logger::instance().header("Device ID: " + deviceId + " (source: " + idSource + ")");

  // Now safe to dump session header (PlatformInfo may now log via Logger::info).
  Logger::instance().logSessionHeader();
  Logger::info(LogCategory::DIAG,
              "Screen: " + PlatformInfo::instance().getDiagnostics().displayResolution +
              " | App Root: " + AppConfig::instance().getAppRoot());

  // Ensure official app icon is synchronized to all launcher icons
  std::string appRoot = AppConfig::instance().getAppRoot();
  std::string officialCandidate1 = appRoot + "/assets/apps_icons/app_main.png";
  std::string officialCandidate2 = appRoot + "/icon.png";
  std::string officialCandidate3 = "/mnt/SDCARD/Apps/RomCloud/icon.png";

  // Find the best valid high-res icon source (> 100KB)
  std::string bestSource = "";
  struct stat stBest;
  stBest.st_size = 0;

  const std::string candidates[] = {officialCandidate1, officialCandidate2,
                                    officialCandidate3};
  for (const auto &cand : candidates) {
    struct stat st;
    if (stat(cand.c_str(), &st) == 0 && st.st_size > 100000) {
      bestSource = cand;
      stBest = st;
      break;
    }
  }

  if (!bestSource.empty()) {
    std::vector<std::string> iconDestinations = {
        appRoot + "/icon.png",
        appRoot + "/iconsel.png",
        appRoot + "/icontop.png",
        appRoot + "/assets/apps_icons/APP.png",
        appRoot + "/assets/icon.png",
        "/mnt/SDCARD/Apps/RomCloud/icon.png",
        "/mnt/SDCARD/Apps/RomCloud/iconsel.png",
        "/mnt/SDCARD/Apps/RomCloud/icontop.png",
        "/mnt/SDCARD/App/RomCloud/icon.png"};
    for (const auto &dest : iconDestinations) {
      struct stat stDest;
      bool needCopy = false;
      if (stat(dest.c_str(), &stDest) != 0 ||
          stDest.st_size != stBest.st_size) {
        if (dest.find(appRoot) == 0 || stat(dest.c_str(), &stDest) == 0) {
          needCopy = true;
        }
      }
      if (needCopy && dest != bestSource) {
        std::ifstream src(bestSource, std::ios::binary);
        std::ofstream dst(dest, std::ios::binary | std::ios::trunc);
        if (src && dst) {
          dst << src.rdbuf();
          Logger::info("Synchronized official app icon to: " + dest + " (" +
                       std::to_string(stBest.st_size) + " bytes)");
        }
      }
    }
    sync();
  }

  // Ensure config.json uses only icontop to prevent dual stacked icons in
  // TrimUI
  std::string configPath = appRoot + "/config.json";
  std::ifstream cfgIn(configPath);
  if (cfgIn.is_open()) {
    std::string content((std::istreambuf_iterator<char>(cfgIn)),
                        std::istreambuf_iterator<char>());
    cfgIn.close();
    bool changed = false;
    size_t p = 0;
    while ((p = content.find("\"icon\": \"icon.png\"")) != std::string::npos) {
      content.replace(p, 18, "\"icon\": \"\"");
      changed = true;
    }
    while ((p = content.find("\"icon\":\"icon.png\"")) != std::string::npos) {
      content.replace(p, 17, "\"icon\":\"\"");
      changed = true;
    }
    while ((p = content.find("\"iconsel\": \"icon.png\"")) !=
           std::string::npos) {
      content.replace(p, 21, "\"iconsel\": \"\"");
      changed = true;
    }
    while ((p = content.find("\"iconsel\":\"icon.png\"")) !=
           std::string::npos) {
      content.replace(p, 20, "\"iconsel\":\"\"");
      changed = true;
    }
    if (changed) {
      std::ofstream cfgOut(configPath, std::ios::trunc);
      if (cfgOut.is_open()) {
        cfgOut << content;
        cfgOut.close();
        sync();
        Logger::info("Sanitized config.json: removed background icon, kept "
                     "icontop foreground logo");
      }
    }
  }

  // Initialize Network, OAuth, Sync & Download
  HttpClient::instance().init();

  // Initialize Database
  if (!DatabaseManager::instance().init(
          AppConfig::instance().getDatabasePath())) {
    Logger::error("Failed to initialize SQLite database");
    return false;
  }
  CalManager::instance().ensureTables();

  // Portal: load configured port from database or default to 8888
  int savedPort = 8888;
  std::string pStr = DatabaseManager::instance().getSetting("web_portal_port", "8888");
  try { savedPort = std::stoi(pStr); } catch (...) { savedPort = 8888; }
  if (!WebServer::instance().start(savedPort)) {
    Logger::error("WebServer: portal unavailable (all safe ports busy).");
  } else {
    // Keep DB in sync with active port
    DatabaseManager::instance().setSetting("web_portal_port",
                                           std::to_string(WebServer::instance().getPort()));
  }

  AuthManager::instance().init();
  DriveSyncEngine::instance().init();
  DownloadManager::instance().init();

  // Initialize SDL & Display
  if (!initSDL()) {
    return false;
  }

  // Brick: driver ALSA mặc định DAC volume = 0 -> câm toàn bộ (báo thức,
  // mpv/IPTV/YouTube). Mở lại khi boot; mỗi lần beep cũng tự mở lại.
  ensureAlarmVolume();

  if (!InputManager::instance().init()) {
    Logger::error("InputManager initialization failed");
    return false;
  }

  // Manual sync mode: do not scan or sync automatically on startup
  int localCount = 0, cloudCount = 0;
  DatabaseManager::instance().getTotalGameCounts(localCount, cloudCount);
  Logger::info("Database loaded: " + std::to_string(localCount) + " local, " +
               std::to_string(cloudCount) + " cloud games (Manual sync mode).");

  // Initialize OTA Update Manager and check GitHub in background
  UpdateManager::instance().init();
  UpdateManager::instance().checkForUpdatesAsync();

  if (!UIManager::instance().init(m_window, m_renderer)) {
    Logger::error("UIManager initialization failed");
    return false;
  }

  // Auto-start LocalSendManager ở background (TCP listener + multicast
  // announce). QUAN TRỌNG: phải chạy NGAY khi app launch, không chờ user vào
  // LocalSend screen. Nếu service không listen, các peer (laptop / TrimUI khác)
  // sẽ thấy TrimUI qua multicast discovery nhưng POST prepare-upload trả về
  // "Connection refused".
  if (!LocalSendManager::instance().start()) {
    Logger::warn(
        "LocalSendManager failed to start (continuing without LocalSend)");
  }

  m_running = true;
  Logger::info("RomCloud initialization complete. Entering main loop.");
  return true;
}

void Application::run() {
  const int TARGET_FPS = 60;
  const int FRAME_DELAY = 1000 / TARGET_FPS;
  int frameCount = 0;
  uint32_t lastPerfLog = SDL_GetTicks();

  while (m_running) {
    uint32_t frameStart = SDL_GetTicks();

    InputManager::instance().update();

    // MENU thoat app toan cuc — nhung bo qua khi dang o FILE_EXPLORER
    // (MENU giua trong Explorer dung de xoa 2-step, SELECT trai de thoat Explorer).
    // Check state TRUOC roi moi doc phim: tranh double-consume latch MENU cua Explorer.
    // Brick: BACK vat ly = MENU -> chi thoat khi o MENU chinh; cac man khac de back
    // bang B (khong thoat app).
    if (UIManager::instance().getState() == UIState::MENU &&
        InputManager::instance().isButtonJustPressed(Button::MENU)) {
      Logger::error("Menu button pressed at MAIN MENU, exiting cleanly...");
      m_running = false;
      break;
    }

    UIManager::instance().update();

    if (UIManager::instance().getState() == UIState::EXIT_REQUESTED) {
      Logger::info("User requested exit from UI menu.");
      m_running = false;
      break;
    }

    UIManager::instance().render();

    uint32_t frameTime = SDL_GetTicks() - frameStart;
    if (frameTime < FRAME_DELAY) {
      uint32_t remaining = FRAME_DELAY - frameTime;
      if (remaining > 2) {
        SDL_Delay(remaining - 1);
      }
    }

    // Log performance stats every 5 seconds
    frameCount++;
    uint32_t now = SDL_GetTicks();
    if (now - lastPerfLog >= 5000) {
      float fps = (frameCount * 1000.0f) / (now - lastPerfLog);

      uint64_t memTotal = 0, memFree = 0, memAvailable = 0;
      float cpuPct = 0;

#ifndef _WIN32
      // Get system memory info
      FILE *memFile = fopen("/proc/meminfo", "r");
      if (memFile) {
        char line[256];
        while (fgets(line, sizeof(line), memFile)) {
          if (sscanf(line, "MemTotal: %lu kB", &memTotal) == 1)
            continue;
          if (sscanf(line, "MemFree: %lu kB", &memFree) == 1)
            continue;
          if (sscanf(line, "MemAvailable: %lu kB", &memAvailable) == 1)
            continue;
        }
        fclose(memFile);
      }

      // Get CPU usage from /proc/stat
      static uint64_t lastIdle = 0, lastTotal = 0;
      uint64_t cpuIdle = 0, cpuTotal = 0;
      FILE *cpuFile = fopen("/proc/stat", "r");
      if (cpuFile) {
        char line[128];
        if (fgets(line, sizeof(line), cpuFile)) {
          uint64_t u, n, s, i, w, irq, softirq;
          if (sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu", &u, &n, &s, &i,
                     &w, &irq, &softirq) == 7) {
            cpuTotal = u + n + s + i + w + irq + softirq;
            cpuIdle = i + w;
          }
        }
        fclose(cpuFile);
      }

      if (cpuTotal > lastTotal) {
        cpuPct = 100.0f *
                 (1.0f - (float)(cpuIdle - lastIdle) / (cpuTotal - lastTotal));
        lastIdle = cpuIdle;
        lastTotal = cpuTotal;
      }
#endif

      // Calculate memory usage
      uint64_t memUsed =
          (memTotal > memAvailable) ? (memTotal - memAvailable) : 0;
      float memUsedMB = memUsed / 1024.0f;
      float memTotalMB = memTotal / 1024.0f;
      float memPct = (memTotal > 0) ? (memUsed * 100.0f / memTotal) : 0;

      Logger::info("[PERF] FPS: " + std::to_string((int)fps) +
                   " | CPU: " + std::to_string((int)cpuPct) + "%" +
                   " | RAM: " + std::to_string((int)memUsedMB) + "/" +
                   std::to_string((int)memTotalMB) + " MB (" +
                   std::to_string((int)memPct) + "%)");

      frameCount = 0;
      lastPerfLog = now;
    }
  }
}

void Application::shutdown() {
  Logger::info("Beginning clean shutdown sequence...");
  WebServer::instance().stop();
  DownloadManager::instance().shutdown();
  DriveSyncEngine::instance().shutdown();
  AuthManager::instance().shutdown();
  UIManager::instance().shutdown();
  InputManager::instance().shutdown();

  if (m_renderer) {
    SDL_DestroyRenderer(m_renderer);
    m_renderer = nullptr;
  }
  if (m_window) {
    SDL_DestroyWindow(m_window);
    m_window = nullptr;
  }

  DatabaseManager::instance().close();
  HttpClient::instance().shutdown();
  SDL_Quit();

  Logger::info("RomCloud shut down cleanly. Goodbye!");
  Logger::instance().flush();
}

} // namespace RomCloud
