#include "Application.h"
#include "../config/AppConfig.h"
#include "../logging/Logger.h"
#include "../filesystem/FileSystemManager.h"
#include "../database/DatabaseManager.h"
#include "../database/RomIndexer.h"
#include "../network/HttpClient.h"
#include "../auth/AuthManager.h"
#include "../sync/DriveSyncEngine.h"
#include "../download/DownloadManager.h"
#include "../ota/UpdateManager.h"
#include "../input/InputManager.h"
#include "../ui/UIManager.h"
#include "../platform/PlatformInfo.h"
#include "../network/WebServer.h"

#include <csignal>
#include <unistd.h>

namespace RomCloud {

static void signalHandler(int signum) {
    Logger::info("Caught signal " + std::to_string(signum) + ", requesting clean shutdown...");
    Application::instance().requestExit();
}

Application& Application::instance() {
    static Application instance;
    return instance;
}

void Application::requestExit() {
    m_running = false;
}

bool Application::initSDL() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) < 0) {
        Logger::error(std::string("SDL_Init failed: ") + SDL_GetError());
        return false;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");

    m_window = SDL_CreateWindow(
        "RomCloud",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1024,
        768,
        SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP
    );

    if (!m_window) {
        Logger::error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        return false;
    }

    m_renderer = SDL_CreateRenderer(
        m_window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );

    if (!m_renderer) {
        Logger::warn(std::string("Hardware accelerated renderer failed, falling back to software: ") + SDL_GetError());
        m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_SOFTWARE);
        if (!m_renderer) {
            Logger::error(std::string("SDL_CreateRenderer software fallback also failed: ") + SDL_GetError());
            return false;
        }
    }

    SDL_ShowCursor(SDL_DISABLE);
    int w = 0, h = 0;
    SDL_GetWindowSize(m_window, &w, &h);
    Logger::info("Display window created successfully: " + std::to_string(w) + "x" + std::to_string(h));
    return true;
}

bool Application::init(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    if (argc > 1 && argv[1] != nullptr) {
        AppConfig::instance().setAppRoot(std::string(argv[1]));
    }

    if (!FileSystemManager::instance().initializeAppDirectories()) {
        return false;
    }

    Logger::instance().init(AppConfig::instance().getLogFilePath());
    Logger::info("==========================================");
    Logger::info("=== RomCloud Phase 6 (On-Demand DL) ======");
    Logger::info("==========================================");
    Logger::info("App Root: " + AppConfig::instance().getAppRoot());

    // Initialize Network, OAuth, Sync & Download
    HttpClient::instance().init();
    WebServer::instance().start(8080);

    // Initialize Database
    if (!DatabaseManager::instance().init(AppConfig::instance().getDatabasePath())) {
        Logger::error("Failed to initialize SQLite database");
        return false;
    }

    AuthManager::instance().init();
    DriveSyncEngine::instance().init();
    DownloadManager::instance().init();

    // Initialize SDL & Display
    if (!initSDL()) {
        return false;
    }

    if (!InputManager::instance().init()) {
        Logger::error("InputManager initialization failed");
        return false;
    }

    // Manual sync mode: do not scan or sync automatically on startup
    int localCount = 0, cloudCount = 0;
    DatabaseManager::instance().getTotalGameCounts(localCount, cloudCount);
    Logger::info("Database loaded: " + std::to_string(localCount) + " local, " + std::to_string(cloudCount) + " cloud games (Manual sync mode).");

    // Initialize OTA Update Manager and check GitHub in background
    UpdateManager::instance().init();
    UpdateManager::instance().checkForUpdatesAsync();

    if (!UIManager::instance().init(m_window, m_renderer)) {
        Logger::error("UIManager initialization failed");
        return false;
    }

    m_running = true;
    Logger::info("RomCloud initialization complete. Entering main loop.");
    return true;
}

void Application::run() {
    const int TARGET_FPS = 60;
    const int FRAME_DELAY = 1000 / TARGET_FPS;

    while (m_running) {
        uint32_t frameStart = SDL_GetTicks();

        InputManager::instance().update();

        if (InputManager::instance().isButtonJustPressed(Button::MENU)) {
            Logger::info("Menu button pressed, exiting cleanly...");
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
            SDL_Delay(FRAME_DELAY - frameTime);
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
