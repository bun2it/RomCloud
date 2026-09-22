#include "UIManager.h"
#include "../input/InputManager.h"
#include "../platform/PlatformInfo.h"
#include "../config/AppConfig.h"
#include "../filesystem/FileSystemManager.h"
#include "../logging/Logger.h"
#include "../database/Schema.h"
#include "../auth/AuthManager.h"
#include "../sync/DriveSyncEngine.h"
#include "../download/DownloadManager.h"
#include "../database/RomIndexer.h"
#include "../ota/UpdateManager.h"
#include "../app/Application.h"
#include "UiStrings.h"
#include <algorithm>
#include <cstring>

namespace RomCloud {

UIManager& UIManager::instance() {
    static UIManager instance;
    return instance;
}

bool UIManager::init(SDL_Window* window, SDL_Renderer* renderer) {
    m_window = window;
    m_renderer = renderer;

    if (TTF_Init() == -1) {
        Logger::error(std::string("TTF_Init failed: ") + TTF_GetError());
        return false;
    }

    std::string fontPath = AppConfig::instance().getFontPath();
    const char* fallbackFonts[] = {
        fontPath.c_str(),
        "/mnt/SDCARD/Apps/RomCloud/assets/fonts/font.ttf",
        "/usr/trimui/res/full.ttf",
        "assets/fonts/font.ttf",
        "/usr/trimui/res/regular.ttf",
        "/mnt/SDCARD/Themes/TRIMUI YaHei/msyh.ttf"
    };

    for (const char* path : fallbackFonts) {
        if (!m_fontTitle) m_fontTitle = TTF_OpenFont(path, 38);
        if (!m_fontLarge) m_fontLarge = TTF_OpenFont(path, 32);
        if (!m_fontMedium) m_fontMedium = TTF_OpenFont(path, 25);
        if (!m_fontSmall) m_fontSmall = TTF_OpenFont(path, 20);
        if (m_fontTitle && m_fontLarge && m_fontMedium && m_fontSmall) {
            Logger::info(std::string("Loaded TTF font from: ") + path + " (Sizes: 38, 32, 25, 20)");
            break;
        }
    }

    if (!m_fontLarge || !m_fontMedium || !m_fontSmall) {
        Logger::warn("Could not load desired font point sizes.");
    }

    CoverManager::instance().init(m_renderer);
    refreshSystems();

    // Auto-check for OTA updates in background on launch
    UpdateManager::instance().checkForUpdatesAsync([this](bool hasUpdate, const UpdateInfo& info) {
        if (hasUpdate) {
            showToast(std::string(UiStrings::TOAST_NEW_OTA_PREFIX) + info.remoteVersion + "!", {34, 197, 94, 255}, 6000);
        }
    });

    return true;
}

void UIManager::shutdown() {
    CoverManager::instance().shutdown();

    if (m_fontTitle) { TTF_CloseFont(m_fontTitle); m_fontTitle = nullptr; }
    if (m_fontLarge) { TTF_CloseFont(m_fontLarge); m_fontLarge = nullptr; }
    if (m_fontMedium) { TTF_CloseFont(m_fontMedium); m_fontMedium = nullptr; }
    if (m_fontSmall) { TTF_CloseFont(m_fontSmall); m_fontSmall = nullptr; }

    TTF_Quit();
}

void UIManager::setState(UIState state) {
    m_currentState = state;
    if (state == UIState::SYSTEM_SELECT) {
        refreshSystems();
    }
}

void UIManager::showToast(const std::string& message, SDL_Color color, uint32_t durationMs) {
    m_toastMessage = message;
    m_toastColor = color;
    m_toastExpiry = SDL_GetTicks() + durationMs;
}

void UIManager::refreshSystems() {
    m_cachedSystems = DatabaseManager::instance().getSystems(true);
}

void UIManager::refreshGames() {
    int filterInt = static_cast<int>(m_filterMode);
    m_cachedGames = DatabaseManager::instance().getGamesBySystem(m_activeSystem.id, filterInt);
    if (m_selectedGameIndex >= static_cast<int>(m_cachedGames.size())) {
        m_selectedGameIndex = std::max(0, static_cast<int>(m_cachedGames.size()) - 1);
    }
}

void UIManager::triggerManualSync() {
    if (DriveSyncEngine::instance().isSyncing()) {
        showToast(UiStrings::TOAST_SYNCING_DRIVE, {245, 158, 11, 255});
        return;
    }
    showToast(UiStrings::TOAST_SCANNING_SD, {0, 180, 216, 255}, 1500);
    RomIndexer::instance().scanAllSystems(AppConfig::instance().getRomsDir());
    refreshSystems();
    refreshGames();

    if (AuthManager::instance().isLinked()) {
        DriveSyncEngine::instance().startSync();
    } else {
        showToast(UiStrings::TOAST_SD_SCANNED_NO_DRIVE, {34, 197, 94, 255}, 3000);
    }
}

void UIManager::update() {
    auto& input = InputManager::instance();

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
        std::string err = dlProg.errorMessage.empty() ? UiStrings::TOAST_UNKNOWN_ERROR : dlProg.errorMessage;
        DownloadManager::instance().resetProgress();
        refreshSystems();
        refreshGames();
        showToast("Tải thất bại: " + err, {239, 68, 68, 255}, 3500);
        // Try next in queue even after failure
        DownloadManager::instance().processNextInQueue();
    }

    // If sync is running, allow cancel button [B]
    if (DriveSyncEngine::instance().isSyncing()) {
        if (input.isButtonJustPressed(Button::B)) {
            DriveSyncEngine::instance().cancelSync();
            showToast(UiStrings::TOAST_SYNC_CANCELLED, {245, 158, 11, 255});
        }
        return;
    }

    // Check if sync completed
    auto syncProg = DriveSyncEngine::instance().getProgress();
    if (syncProg.status == SyncStatus::COMPLETED) {
        DriveSyncEngine::instance().init();
        refreshSystems();
        refreshGames();
        showToast("Đồng bộ hoàn tất: Đã lưu " + std::to_string(syncProg.cloudGamesFound) + " game vào thư viện!", {34, 197, 94, 255});
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
            if (input.isButtonJustPressed(Button::UP)) {
                m_selectedMenuIndex = (m_selectedMenuIndex - 1 + static_cast<int>(m_menuItems.size())) % static_cast<int>(m_menuItems.size());
            } else if (input.isButtonJustPressed(Button::DOWN)) {
                m_selectedMenuIndex = (m_selectedMenuIndex + 1) % static_cast<int>(m_menuItems.size());
            } else if (input.isButtonJustPressed(Button::A)) {
                if (m_selectedMenuIndex == 0) {
                    setState(UIState::SYSTEM_SELECT);
                } else if (m_selectedMenuIndex == 1) {
                    triggerManualSync();
                } else if (m_selectedMenuIndex == 2) {
                    setState(UIState::OTA_UPDATE);
                    UpdateManager::instance().checkForUpdatesAsync();
                } else if (m_selectedMenuIndex == 3) {
                    setState(UIState::SETTINGS);
                } else if (m_selectedMenuIndex == 4) {
                    setState(UIState::DIAGNOSTICS);
                } else if (m_selectedMenuIndex == 5) {
                    setState(UIState::EXIT_REQUESTED);
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

            if (total > 0) {
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
                    m_selectedGameIndex = std::min(total - 1, m_selectedGameIndex + pageSize);
                    m_gameScrollOffset = std::min(std::max(0, total - pageSize), m_gameScrollOffset + pageSize);
                } else if (input.isButtonJustPressed(Button::A)) {
                    const auto& g = m_cachedGames[m_selectedGameIndex];
                    if (g.localState == GameState::LOCAL) {
                        showToast(UiStrings::TOAST_GAME_EXISTS_DELETE, {34, 197, 94, 255}, 3000);
                    } else if (DownloadManager::instance().isInQueue(g.id)) {
                        showToast("\"" + g.title + "\" đã có trong danh sách tải.", {245, 158, 11, 255});
                    } else {
                        if (AuthManager::instance().isLinked()) {
                            bool added = DownloadManager::instance().addToQueue(g, m_activeSystem);
                            if (added) {
                                showToast(std::string(UiStrings::TOAST_ADDED_TO_QUEUE) + g.title, {0, 180, 216, 255}, 2500);
                                // Auto-start if nothing is currently downloading
                                if (!DownloadManager::instance().isDownloading()) {
                                    DownloadManager::instance().processNextInQueue();
                                }
                                refreshGames();
                            }
                        } else {
                            showToast(UiStrings::TOAST_CONNECT_DRIVE_FIRST, {245, 158, 11, 255});
                        }
                    }
                } else if (input.isButtonJustPressed(Button::X)) {
                    const auto& g = m_cachedGames[m_selectedGameIndex];
                    if (DownloadManager::instance().isDownloading() &&
                        DownloadManager::instance().getProgress().gameId == g.id) {
                        DownloadManager::instance().cancelDownload();
                        showToast(std::string(UiStrings::TOAST_DOWNLOAD_STOPPED) + g.title, {245, 158, 11, 255});
                        DownloadManager::instance().processNextInQueue();
                        refreshGames();
                    } else if (DownloadManager::instance().isInQueue(g.id)) {
                        DownloadManager::instance().removeFromQueue(g.id);
                        showToast(std::string(UiStrings::TOAST_REMOVED_FROM_QUEUE) + g.title, {245, 158, 11, 255});
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
                        for (size_t i = m_selectedGameIndex + 1; i < m_cachedGames.size(); ++i) {
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
                        showToast(std::string("Chuyển đến vần chữ: [ ") + newL + " ]", {234, 179, 8, 255}, 1500);
                    }
                }
            }

            if (input.isButtonJustPressed(Button::SELECT)) {
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
                // Open on-device search
                m_searchQuery.clear();
                m_searchResults.clear();
                m_searchSelectedIndex = 0;
                m_searchScrollOffset = 0;
                m_kbCursorRow = 0;
                m_kbCursorCol = 0;
                m_kbInResults = false;
                setState(UIState::SEARCH);
            }

            if (input.isButtonJustPressed(Button::B)) {
                setState(UIState::SYSTEM_SELECT);
            }
            break;
        }

        case UIState::CONFIRM_DELETE: {
            if (input.isButtonJustPressed(Button::A)) {
                if (m_selectedGameIndex >= 0 && m_selectedGameIndex < static_cast<int>(m_cachedGames.size())) {
                    auto& g = m_cachedGames[m_selectedGameIndex];
                    DatabaseManager::instance().markGameDeletedLocally(g.id);
                    refreshSystems();
                    refreshGames();
                    showToast("Đã xóa \"" + g.title + "\" khỏi thẻ nhớ.", {239, 68, 68, 255}, 3000);
                }
                setState(UIState::GAME_LIST);
            } else if (input.isButtonJustPressed(Button::B) || input.isButtonJustPressed(Button::X)) {
                setState(UIState::GAME_LIST);
            }
            break;
        }

        case UIState::SEARCH: {
            // On-screen keyboard layout (rows x cols)
            static const char* kbRows[] = {
                "ABCDEFGHIJ",
                "KLMNOPQRST",
                "UVWXYZ0123",
                "456789 <OK"
            };
            static const int kbRowCount = 4;

            auto getKbChar = [&](int row, int col) -> char {
                if (row < 0 || row >= kbRowCount) return 0;
                const char* r = kbRows[row];
                if (col < 0 || col >= (int)strlen(r)) return 0;
                return r[col];
            };
            auto kbRowLen = [&](int row) -> int {
                if (row < 0 || row >= kbRowCount) return 0;
                return static_cast<int>(strlen(kbRows[row]));
            };

            if (!m_kbInResults) {
                // Navigate keyboard
                if (input.isButtonJustPressed(Button::UP)) {
                    if (m_kbCursorRow > 0) {
                        m_kbCursorRow--;
                        if (m_kbCursorCol >= kbRowLen(m_kbCursorRow))
                            m_kbCursorCol = kbRowLen(m_kbCursorRow) - 1;
                    }
                } else if (input.isButtonJustPressed(Button::DOWN)) {
                    if (m_kbCursorRow < kbRowCount - 1) {
                        m_kbCursorRow++;
                        if (m_kbCursorCol >= kbRowLen(m_kbCursorRow))
                            m_kbCursorCol = kbRowLen(m_kbCursorRow) - 1;
                    } else {
                        // Go to results if any
                        if (!m_searchResults.empty()) {
                            m_kbInResults = true;
                            m_searchSelectedIndex = 0;
                            m_searchScrollOffset = 0;
                        }
                    }
                } else if (input.isButtonJustPressed(Button::LEFT)) {
                    if (m_kbCursorCol > 0) m_kbCursorCol--;
                    else m_kbCursorCol = kbRowLen(m_kbCursorRow) - 1;
                } else if (input.isButtonJustPressed(Button::RIGHT)) {
                    if (m_kbCursorCol < kbRowLen(m_kbCursorRow) - 1) m_kbCursorCol++;
                    else m_kbCursorCol = 0;
                } else if (input.isButtonJustPressed(Button::A)) {
                    char ch = getKbChar(m_kbCursorRow, m_kbCursorCol);
                    if (ch == '<') { // Backspace
                        if (!m_searchQuery.empty()) m_searchQuery.pop_back();
                    } else if (ch == 'O') { // OK (special - last row last col is "OK")
                        // OK: check if cursor is at 'K' (which follows 'O')
                        m_kbInResults = true;
                        m_searchSelectedIndex = 0;
                        m_searchScrollOffset = 0;
                    } else if (ch == ' ') {
                        if (m_searchQuery.size() < 30) m_searchQuery += ' ';
                    } else if (ch != 0) {
                        if (m_searchQuery.size() < 30) m_searchQuery += ch;
                    }
                    // Auto-search as user types
                    if (m_searchQuery.length() >= 2) {
                        m_searchResults = DatabaseManager::instance().searchAllGames(m_searchQuery, 50);
                    } else {
                        m_searchResults.clear();
                    }
                    m_searchSelectedIndex = 0;
                    m_searchScrollOffset = 0;
                } else if (input.isButtonJustPressed(Button::X)) {
                    // Clear query
                    m_searchQuery.clear();
                    m_searchResults.clear();
                    m_searchSelectedIndex = 0;
                    m_searchScrollOffset = 0;
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
                        m_kbInResults = false; // go back to keyboard
                    }
                } else if (input.isButtonJustPressed(Button::DOWN)) {
                    if (m_searchSelectedIndex < numResults - 1) {
                        m_searchSelectedIndex++;
                        if (m_searchSelectedIndex >= m_searchScrollOffset + pageSize)
                            m_searchScrollOffset = m_searchSelectedIndex - pageSize + 1;
                    }
                } else if (input.isButtonJustPressed(Button::A)) {
                    if (m_searchSelectedIndex >= 0 && m_searchSelectedIndex < numResults) {
                        const auto& g = m_searchResults[m_searchSelectedIndex];
                        if (g.localState == GameState::LOCAL) {
                            showToast(UiStrings::TOAST_GAME_EXISTS_DELETE, {34, 197, 94, 255}, 2500);
                        } else if (!AuthManager::instance().isLinked()) {
                            showToast(UiStrings::TOAST_CONNECT_DRIVE_FIRST, {245, 158, 11, 255});
                        } else {
                            // Find system for this game
                            SystemRecord sys;
                            if (DatabaseManager::instance().getSystemById(g.systemId, sys)) {
                                bool added = DownloadManager::instance().addToQueue(g, sys);
                                if (added) {
                                    showToast(std::string(UiStrings::TOAST_ADDED_TO_QUEUE) + g.title, {0, 180, 216, 255}, 2500);
                                    if (!DownloadManager::instance().isDownloading()) {
                                        DownloadManager::instance().processNextInQueue();
                                    }
                                    // Refresh results state
                                    m_searchResults = DatabaseManager::instance().searchAllGames(m_searchQuery, 50);
                                }
                            }
                        }
                    }
                } else if (input.isButtonJustPressed(Button::X)) {
                    if (m_searchSelectedIndex >= 0 && m_searchSelectedIndex < numResults) {
                        const auto& g = m_searchResults[m_searchSelectedIndex];
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
                                m_searchResults = DatabaseManager::instance().searchAllGames(m_searchQuery, 50);
                                if (m_searchSelectedIndex >= static_cast<int>(m_searchResults.size()))
                                    m_searchSelectedIndex = std::max(0, static_cast<int>(m_searchResults.size()) - 1);
                                showToast("Đã xóa \"" + g.title + "\" khỏi thẻ nhớ.", {239, 68, 68, 255}, 3000);
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
                // Return to game list or menu
                if (m_activeSystem.id > 0) {
                    setState(UIState::GAME_LIST);
                } else {
                    setState(UIState::MENU);
                }
            }
            break;
        }

        case UIState::SETTINGS: {
            if (input.isButtonJustPressed(Button::A)) {
                if (!AuthManager::instance().isLinked()) {
                    setState(UIState::DISCLAIMER);
                }
            } else if (input.isButtonJustPressed(Button::X)) {
                if (AuthManager::instance().isLinked()) {
                    AuthManager::instance().logout();
                    refreshSystems();
                    refreshGames();
                    showToast(UiStrings::TOAST_LOGOUT_SUCCESS, {245, 158, 11, 255});
                }
            } else if (input.isButtonJustPressed(Button::B)) {
                setState(UIState::MENU);
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
                if (input.isButtonJustPressed(Button::A) || input.isButtonJustPressed(Button::B)) {
                    refreshSystems();
                    setState(UIState::SYSTEM_SELECT);
                }
            } else {
                if (input.isButtonJustPressed(Button::B)) {
                    AuthManager::instance().cancelDeviceFlow();
                    setState(UIState::SETTINGS);
                }
            }
            break;
        }

        case UIState::DIAGNOSTICS: {
            if (input.isButtonJustPressed(Button::B)) {
                setState(UIState::MENU);
            }
            break;
        }

        case UIState::OTA_UPDATE: {
            auto prog = UpdateManager::instance().getProgress();
            if (prog.state == UpdateState::UPDATE_AVAILABLE) {
                if (input.isButtonJustPressed(Button::A)) {
                    UpdateManager::instance().startUpdate(UpdateManager::instance().getLatestInfo());
                } else if (input.isButtonJustPressed(Button::B)) {
                    setState(UIState::MENU);
                }
            } else if (prog.state == UpdateState::UP_TO_DATE || prog.state == UpdateState::FAILED) {
                if (input.isButtonJustPressed(Button::A)) {
                    UpdateManager::instance().checkForUpdatesAsync();
                } else if (input.isButtonJustPressed(Button::B)) {
                    setState(UIState::MENU);
                }
            } else if (prog.state == UpdateState::DOWNLOADING || prog.state == UpdateState::VERIFYING) {
                if (input.isButtonJustPressed(Button::B)) {
                    UpdateManager::instance().cancelUpdate();
                    showToast(UiStrings::TOAST_OTA_CANCELLED, {245, 158, 11, 255});
                }
            } else if (prog.state == UpdateState::COMPLETED) {
                if (input.isButtonJustPressed(Button::A)) {
                    Application::instance().requestRestart();
                }
            } else {
                if (input.isButtonJustPressed(Button::B)) {
                    setState(UIState::MENU);
                }
            }
            break;
        }

        default: break;
    }
}

void UIManager::drawText(const std::string& text, int x, int y, SDL_Color color, TTF_Font* font, bool centered) {
    if (!font || text.empty()) return;
    SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!surface) return;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(m_renderer, surface);
    if (!texture) {
        SDL_FreeSurface(surface);
        return;
    }

    int drawX = centered ? (x - surface->w / 2) : x;
    int drawY = y;
    SDL_Rect dstRect = {drawX, drawY, surface->w, surface->h};
    SDL_RenderCopy(m_renderer, texture, nullptr, &dstRect);

    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);
}

void UIManager::drawRect(int x, int y, int w, int h, SDL_Color color, bool filled) {
    SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
    SDL_Rect rect = {x, y, w, h};
    if (filled) {
        SDL_RenderFillRect(m_renderer, &rect);
    } else {
        SDL_RenderDrawRect(m_renderer, &rect);
    }
}

void UIManager::drawBorder(int x, int y, int w, int h, SDL_Color color, int thickness) {
    SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
    for (int i = 0; i < thickness; ++i) {
        SDL_Rect rect = {x + i, y + i, w - 2 * i, h - 2 * i};
        SDL_RenderDrawRect(m_renderer, &rect);
    }
}

void UIManager::drawBadge(int x, int y, int w, int h, const std::string& text, SDL_Color bg, SDL_Color fg) {
    drawRect(x, y, w, h, bg, true);
    drawText(text, x + w / 2, y + (h - 16) / 2, fg, m_fontSmall, true);
}

void UIManager::renderHeader() {
    drawRect(0, 0, 1024, 64, {18, 22, 30, 255}, true);
    drawRect(0, 63, 1024, 1, {40, 48, 62, 255}, true);

    drawText(UiStrings::APP_TITLE, 30, 14, {0, 180, 216, 255}, m_fontTitle ? m_fontTitle : m_fontLarge);
    drawText(UiStrings::APP_SUBTITLE, 240, 22, {130, 145, 165, 255}, m_fontMedium);

    if (m_currentState == UIState::GAME_LIST) {
        std::string sysTitle = m_activeSystem.code + " - " + m_activeSystem.name;
        drawText(sysTitle, 512, 18, {255, 255, 255, 255}, m_fontLarge, true);

        std::string filterLabel = UiStrings::FILTER_TAG_ALL;
        SDL_Color filterBg = {107, 33, 168, 255};
        if (m_filterMode == GameFilterMode::LOCAL_ONLY) {
            filterLabel = UiStrings::FILTER_TAG_LOCAL;
            filterBg = {22, 101, 52, 255};
        } else if (m_filterMode == GameFilterMode::CLOUD_ONLY) {
            filterLabel = UiStrings::FILTER_TAG_CLOUD;
            filterBg = {30, 58, 138, 255};
        }
        drawBadge(790, 14, 205, 38, filterLabel, filterBg, {255, 255, 255, 255});
    } else {
        auto diag = PlatformInfo::instance().getDiagnostics();
        std::string statusText = "Wi-Fi: " + (diag.ipAddress != "N/A" ? "ONLINE (" + diag.ipAddress + ")" : "OFFLINE");
        SDL_Color statusColor = (diag.ipAddress != "N/A") ? SDL_Color{34, 197, 94, 255} : SDL_Color{239, 68, 68, 255};
        drawText(statusText, 994 - (int)statusText.length() * 10, 20, statusColor, m_fontMedium);
    }

    if (DownloadManager::instance().isDownloading()) {
        auto dlp = DownloadManager::instance().getProgress();
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f%%", dlp.progressPct);
        std::string dlBadge = "⬇ " + std::string(buf);
        int bx = (m_currentState == UIState::GAME_LIST || m_currentState == UIState::SEARCH) ? 700 : 540;
        drawBadge(bx, 14, 80, 38, dlBadge, {2, 132, 199, 255}, {255, 255, 255, 255});
    }
}

void UIManager::renderSearchState() {
    // ─── On-screen keyboard layout ─────────────────────────────────────────
    static const char* kbRows[] = {
        "ABCDEFGHIJ",
        "KLMNOPQRST",
        "UVWXYZ0123",
        "456789 <OK"
    };
    static const int kbRowCount = 4;

    // ─── Left panel: keyboard + query bar ─────────────────────────────────
    int panelW = 420;
    int panelX = 18;
    int panelY = 72;

    // Query bar
    drawRect(panelX, panelY, panelW, 50, {22, 32, 46, 255}, true);
    drawBorder(panelX, panelY, panelW, 50, {0, 180, 216, 255}, 2);
    std::string displayQuery = m_searchQuery.empty() ? "Nhap chu de tim..." : m_searchQuery + "_";
    SDL_Color qColor = m_searchQuery.empty() ? SDL_Color{80, 95, 115, 255} : SDL_Color{255, 255, 255, 255};
    drawText(displayQuery, panelX + 12, panelY + 12, qColor, m_fontMedium);

    // Keyboard
    int kbStartY = panelY + 62;
    int cellW = 38;
    int cellH = 40;
    int kbPadX = 12;

    for (int row = 0; row < kbRowCount; row++) {
        const char* rowStr = kbRows[row];
        int len = static_cast<int>(strlen(rowStr));
        for (int col = 0; col < len; col++) {
            int cx = panelX + kbPadX + col * (cellW + 2);
            int cy = kbStartY + row * (cellH + 4);
            bool isSel = (!m_kbInResults && m_kbCursorRow == row && m_kbCursorCol == col);

            char ch = rowStr[col];
            std::string label;
            if (ch == '<') label = "DEL";
            else if (ch == 'O' && col < len - 1 && rowStr[col + 1] == 'K') {
                label = "OK";
                // Skip 'K' next iteration handled by drawing "OK" on O cell
            } else if (ch == 'K' && col > 0 && rowStr[col - 1] == 'O') {
                continue; // Skip K, already drawn as part of OK
            } else if (ch == ' ') {
                label = "SPC";
            } else {
                label = std::string(1, ch);
            }

            // Special wider cells
            int thisW = cellW;
            if (label == "DEL" || label == "OK" || label == "SPC") thisW = cellW * 2 + 2;

            SDL_Color bg = isSel ? SDL_Color{0, 180, 216, 255} : SDL_Color{28, 38, 55, 255};
            SDL_Color fg = isSel ? SDL_Color{0, 0, 0, 255} : SDL_Color{220, 230, 240, 255};
            drawRect(cx, cy, thisW, cellH, bg, true);
            drawBorder(cx, cy, thisW, cellH, isSel ? SDL_Color{255, 255, 255, 255} : SDL_Color{40, 55, 75, 255}, 1);
            drawText(label, cx + thisW / 2, cy + cellH / 2 - 10, fg, m_fontSmall, true);
        }
    }

    // ─── Right panel: results ─────────────────────────────────────────────
    int rPanelX = panelX + panelW + 16;
    int rPanelW = 1024 - rPanelX - 12;
    int rPanelY = panelY;
    int rPanelH = 625;

    drawRect(rPanelX, rPanelY, rPanelW, rPanelH, {16, 22, 33, 255}, true);
    drawBorder(rPanelX, rPanelY, rPanelW, rPanelH,
               m_kbInResults ? SDL_Color{0, 180, 216, 255} : SDL_Color{40, 52, 68, 255}, 2);

    int numResults = static_cast<int>(m_searchResults.size());
    if (m_searchQuery.length() < 2) {
        drawText(UiStrings::SEARCH_PROMPT_MIN_CHARS, rPanelX + rPanelW / 2, rPanelY + 30, {80, 95, 115, 255}, m_fontSmall, true);
    } else if (numResults == 0) {
        drawText(UiStrings::SEARCH_NO_RESULTS, rPanelX + rPanelW / 2, rPanelY + 30, {239, 68, 68, 255}, m_fontSmall, true);
    } else {
        std::string countStr = std::to_string(numResults) + " kết quả";
        drawText(countStr, rPanelX + rPanelW / 2, rPanelY + 12, {100, 115, 135, 255}, m_fontSmall, true);

        int pageSize = 10;
        int itemH = 57;
        int listStartY = rPanelY + 36;

        for (int i = 0; i < pageSize && (m_searchScrollOffset + i) < numResults; i++) {
            int idx = m_searchScrollOffset + i;
            const auto& g = m_searchResults[idx];
            bool isSel = (m_kbInResults && idx == m_searchSelectedIndex);

            int itemY = listStartY + i * itemH;
            SDL_Color rowBg = isSel ? SDL_Color{2, 55, 82, 255} : SDL_Color{20, 28, 42, 255};
            drawRect(rPanelX + 4, itemY, rPanelW - 8, itemH - 2, rowBg, true);
            if (isSel) {
                drawBorder(rPanelX + 4, itemY, rPanelW - 8, itemH - 2, {0, 180, 216, 255}, 2);
            }

            // State badge
            bool isLocal = (g.localState == GameState::LOCAL);
            SDL_Color badgeBg = isLocal ? SDL_Color{22, 78, 99, 255} : SDL_Color{45, 30, 72, 255};
            SDL_Color badgeFg = isLocal ? SDL_Color{34, 197, 94, 255} : SDL_Color{168, 85, 247, 255};
            std::string stateLabel = isLocal ? "LOCAL" : "CLOUD";
            drawBadge(rPanelX + 8, itemY + 8, 62, 24, stateLabel, badgeBg, badgeFg);

            // Title
            std::string title = g.title;
            if (title.length() > 32) title = title.substr(0, 31) + "…";
            drawText(title, rPanelX + 80, itemY + 8, {230, 240, 255, 255}, m_fontMedium);

            // System label
            drawText(g.systemCode, rPanelX + 80, itemY + 34, {100, 115, 135, 255}, m_fontSmall);
        }

        // Scroll indicator
        if (numResults > pageSize) {
            float scrollFrac = static_cast<float>(m_searchScrollOffset) / (numResults - pageSize);
            int scrollBarH = rPanelH - 46;
            int thumbH = std::max(24, scrollBarH / (numResults / pageSize + 1));
            int thumbY = rPanelY + 38 + static_cast<int>(scrollFrac * (scrollBarH - thumbH));
            drawRect(rPanelX + rPanelW - 8, rPanelY + 38, 4, scrollBarH, {30, 42, 58, 255}, true);
            drawRect(rPanelX + rPanelW - 8, thumbY, 4, thumbH, {0, 180, 216, 255}, true);
        }
    }

    // Hint: which panel is active
    std::string hint = m_kbInResults ? "◀ Lên để quay lại bàn phím" : "▼ Xuống để xem kết quả";
    drawText(hint, 512, 705, {60, 75, 95, 255}, m_fontSmall, true);
}

void UIManager::renderFooter() {
    drawRect(0, 715, 1024, 53, {18, 22, 30, 255}, true);
    drawRect(0, 715, 1024, 1, {40, 48, 62, 255}, true);

    if (m_currentState == UIState::GAME_LIST) {
        bool isLocal = false;
        if (m_selectedGameIndex >= 0 && m_selectedGameIndex < static_cast<int>(m_cachedGames.size())) {
            isLocal = (m_cachedGames[m_selectedGameIndex].localState == GameState::LOCAL);
        }

        if (isLocal) {
            drawText("X", 30, 726, {239, 68, 68, 255}, m_fontMedium);
            drawText(UiStrings::BTN_DELETE_ROM_SD, 52, 730, {239, 68, 68, 255}, m_fontSmall);
        } else {
            drawText("A", 30, 726, {34, 197, 94, 255}, m_fontMedium);
            drawText(UiStrings::FOOTER_ADD_QUEUE, 52, 730, {210, 220, 230, 255}, m_fontSmall);
        }

        drawText("B", 185, 726, {239, 68, 68, 255}, m_fontMedium);
        drawText(UiStrings::FOOTER_BACK, 207, 730, {210, 220, 230, 255}, m_fontSmall);

        if (!isLocal) {
            drawText("X", 295, 726, {245, 158, 11, 255}, m_fontMedium);
            drawText(UiStrings::FOOTER_DELETE, 317, 730, {210, 220, 230, 255}, m_fontSmall);
        }

        drawText("Y", 440, 726, {234, 179, 8, 255}, m_fontMedium);
        drawText(UiStrings::BTN_JUMP_ALPHA, 462, 730, {210, 220, 230, 255}, m_fontSmall);

        drawText("START", 593, 726, {0, 180, 216, 255}, m_fontMedium);
        drawText(UiStrings::BTN_SEARCH, 648, 730, {0, 180, 216, 255}, m_fontSmall);

        drawText("SELECT", 728, 726, {168, 85, 247, 255}, m_fontMedium);
        drawText(UiStrings::FOOTER_FILTER, 800, 730, {210, 220, 230, 255}, m_fontSmall);
    } else if (m_currentState == UIState::SEARCH) {
        if (!m_kbInResults) {
            drawText("A", 30, 726, {34, 197, 94, 255}, m_fontMedium);
            drawText(UiStrings::BTN_SELECT_CHAR, 52, 730, {210, 220, 230, 255}, m_fontSmall);

            drawText("X", 170, 726, {245, 158, 11, 255}, m_fontMedium);
            drawText(UiStrings::BTN_CLEAR_ALL, 192, 730, {210, 220, 230, 255}, m_fontSmall);
        } else {
            drawText("A", 30, 726, {34, 197, 94, 255}, m_fontMedium);
            drawText(UiStrings::BTN_DOWNLOAD, 52, 730, {210, 220, 230, 255}, m_fontSmall);

            drawText("X", 130, 726, {239, 68, 68, 255}, m_fontMedium);
            drawText(UiStrings::BTN_DELETE_ROM, 152, 730, {239, 68, 68, 255}, m_fontSmall);
        }

        drawText("B", 280, 726, {239, 68, 68, 255}, m_fontMedium);
        drawText(UiStrings::BTN_BACK, 302, 730, {210, 220, 230, 255}, m_fontSmall);
    } else if (m_currentState == UIState::CONFIRM_DELETE) {
        drawText("A", 30, 726, {239, 68, 68, 255}, m_fontMedium);
        drawText(UiStrings::BTN_CONFIRM_DELETE_SD, 52, 730, {239, 68, 68, 255}, m_fontSmall);

        drawText("B / X", 240, 726, {150, 160, 175, 255}, m_fontMedium);
        drawText(UiStrings::BTN_CANCEL_ACTION, 295, 730, {210, 220, 230, 255}, m_fontSmall);
    } else if (m_currentState == UIState::SYSTEM_SELECT) {
        drawText("A", 30, 726, {34, 197, 94, 255}, m_fontMedium);
        drawText(UiStrings::FOOTER_ENTER_SYSTEM, 55, 730, {210, 220, 230, 255}, m_fontSmall);

        drawText("B", 195, 726, {239, 68, 68, 255}, m_fontMedium);
        drawText(UiStrings::FOOTER_MAIN_MENU, 220, 730, {210, 220, 230, 255}, m_fontSmall);

        drawText("Y", 360, 726, {234, 179, 8, 255}, m_fontMedium);
        drawText(UiStrings::FOOTER_SYNC_DRIVE, 385, 730, {210, 220, 230, 255}, m_fontSmall);

        drawText("L1/R1", 645, 726, {0, 180, 216, 255}, m_fontMedium);
        drawText(UiStrings::FOOTER_CHANGE_PAGE, 710, 730, {210, 220, 230, 255}, m_fontSmall);
    } else if (m_currentState == UIState::OTA_UPDATE) {
        drawText("A", 30, 726, {34, 197, 94, 255}, m_fontMedium);
        drawText(UiStrings::FOOTER_CONFIRM, 55, 730, {210, 220, 230, 255}, m_fontSmall);

        drawText("B", 170, 726, {239, 68, 68, 255}, m_fontMedium);
        drawText(UiStrings::FOOTER_CANCEL, 195, 730, {210, 220, 230, 255}, m_fontSmall);
    } else {
        drawText("A", 30, 726, {34, 197, 94, 255}, m_fontMedium);
        drawText(UiStrings::FOOTER_SELECT, 55, 730, {210, 220, 230, 255}, m_fontSmall);

        drawText("B", 140, 726, {239, 68, 68, 255}, m_fontMedium);
        drawText(UiStrings::FOOTER_BACK, 165, 730, {210, 220, 230, 255}, m_fontSmall);
    }

    std::string verText = "v" + std::string(APP_VERSION) + " Native";
    drawText(verText, 880, 730, {110, 120, 135, 255}, m_fontSmall);
}

void UIManager::renderToast() {
    uint32_t now = SDL_GetTicks();
    if (now < m_toastExpiry && !m_toastMessage.empty()) {
        int toastW = 600;
        int toastH = 48;
        int toastX = (1024 - toastW) / 2;
        int toastY = 650;

        drawRect(toastX, toastY, toastW, toastH, {20, 24, 32, 240}, true);
        drawBorder(toastX, toastY, toastW, toastH, m_toastColor, 2);
        drawText(m_toastMessage, 512, toastY + 14, {255, 255, 255, 255}, m_fontSmall, true);
    }
}

void UIManager::renderMenuState() {
    int startY = 125;
    int itemHeight = 68;
    int itemWidth = 580;
    int spacing = 16;
    int startX = (1024 - itemWidth) / 2;

    std::string otaMenuText = UiStrings::MENU_OTA;
    if (UpdateManager::instance().isUpdateAvailable()) {
        otaMenuText = std::string(UiStrings::MENU_OTA_NEW_BADGE) + " (v" + UpdateManager::instance().getLatestInfo().remoteVersion + ")";
    }

    std::vector<std::string> currentMenu = {
        UiStrings::MENU_PLAY,
        UiStrings::MENU_SYNC,
        otaMenuText,
        UiStrings::MENU_SETTINGS,
        UiStrings::MENU_DIAG,
        UiStrings::MENU_EXIT
    };

    for (size_t i = 0; i < currentMenu.size(); ++i) {
        int y = startY + static_cast<int>(i) * (itemHeight + spacing);
        bool selected = (static_cast<int>(i) == m_selectedMenuIndex);

        SDL_Color bg = selected ? SDL_Color{30, 58, 95, 255} : SDL_Color{25, 30, 38, 255};
        drawRect(startX, y, itemWidth, itemHeight, bg, true);

        if (selected) {
            drawBorder(startX, y, itemWidth, itemHeight, {0, 180, 216, 255}, 3);
            drawRect(startX + 6, y + 6, 8, itemHeight - 12, {0, 180, 216, 255}, true);
        }

        SDL_Color textColor = selected ? SDL_Color{255, 255, 255, 255} : SDL_Color{170, 180, 195, 255};
        drawText(currentMenu[i], startX + 45, y + 18, textColor, m_fontLarge);
    }
}

void UIManager::renderSystemSelectState() {
    int cardX = 30;
    int cardY = 75;
    int cardW = 964;
    int cardH = 630;

    drawRect(cardX, cardY, cardW, cardH, {20, 25, 32, 255}, true);
    drawBorder(cardX, cardY, cardW, cardH, {40, 50, 65, 255}, 2);

    int totalLocal = 0, totalCloud = 0;
    DatabaseManager::instance().getTotalGameCounts(totalLocal, totalCloud);

    std::string summary = std::string(UiStrings::SYSTEM_SELECT_TITLE) + " (" + std::to_string(m_cachedSystems.size()) + UiStrings::SYS_SELECT_SYSTEMS_LABEL +
                          std::to_string(totalLocal) + UiStrings::SYS_SELECT_GAMES_LOCAL +
                          std::to_string(totalCloud) + UiStrings::SYS_SELECT_GAMES_CLOUD;
    drawText(summary, cardX + 30, cardY + 16, {0, 180, 216, 255}, m_fontLarge);

    int visibleCount = 6;
    int startIdx = 0;
    if (m_selectedSystemIndex >= visibleCount) {
        startIdx = m_selectedSystemIndex - visibleCount + 1;
    }

    int rowY = cardY + 58;
    int rowH = 84;
    int rowW = cardW - 60;

    for (int i = startIdx; i < static_cast<int>(m_cachedSystems.size()) && (i - startIdx) < visibleCount; ++i) {
        const auto& sys = m_cachedSystems[i];
        bool selected = (i == m_selectedSystemIndex);
        int y = rowY + (i - startIdx) * (rowH + 10);

        SDL_Color bg = selected ? SDL_Color{30, 58, 95, 255} : SDL_Color{28, 34, 44, 255};
        drawRect(cardX + 30, y, rowW, rowH, bg, true);

        if (selected) {
            drawBorder(cardX + 30, y, rowW, rowH, {0, 180, 216, 255}, 2);
            drawRect(cardX + 34, y + 4, 6, rowH - 8, {0, 180, 216, 255}, true);
        }

        // System Logo Badge Box
        drawRect(cardX + 45, y + 14, 110, 56, {18, 22, 30, 255}, true);
        drawBorder(cardX + 45, y + 14, 110, 56, {0, 180, 216, 255}, 1);
        drawText(sys.code, cardX + 100, y + 26, {0, 180, 216, 255}, m_fontLarge, true);

        drawText(sys.name, cardX + 175, y + 16, {255, 255, 255, 255}, m_fontLarge);

        std::string localBadge = "THẺ NHỚ: " + std::to_string(sys.localCount);
        std::string cloudBadge = "CLOUD: " + std::to_string(sys.cloudCount);

        drawBadge(cardX + 650, y + 22, 125, 38, localBadge, {22, 101, 52, 255}, {255, 255, 255, 255});
        drawBadge(cardX + 785, y + 22, 125, 38, cloudBadge, {30, 58, 138, 255}, {255, 255, 255, 255});

        std::string subtext = "Thư mục: /Roms/" + sys.romDir + "  |  Định dạng: " + sys.extList;
        drawText(subtext, cardX + 175, y + 50, {140, 155, 175, 255}, m_fontSmall);
    }
}

void UIManager::renderGameListState() {
    int listX = 30;
    int listY = 75;
    int listW = 540;
    int listH = 630;

    int detailX = 590;
    int detailY = 75;
    int detailW = 404;
    int detailH = 630;

    // 1. Render Left Games List
    drawRect(listX, listY, listW, listH, {20, 25, 32, 255}, true);
    drawBorder(listX, listY, listW, listH, {40, 50, 65, 255}, 2);

    int totalGames = static_cast<int>(m_cachedGames.size());
    int pageSize = 6;
    int rowH = 88;
    int rowW = listW - 30;

    if (totalGames == 0) {
        drawText(UiStrings::GAME_LIST_EMPTY, listX + listW / 2, listY + 280, {140, 150, 165, 255}, m_fontLarge, true);
        drawText(UiStrings::GAME_FILTER_HINT, listX + listW / 2, listY + 325, {0, 180, 216, 255}, m_fontSmall, true);
    } else {
        for (int i = 0; i < pageSize && (m_gameScrollOffset + i) < totalGames; ++i) {
            int gameIdx = m_gameScrollOffset + i;
            const auto& game = m_cachedGames[gameIdx];
            bool selected = (gameIdx == m_selectedGameIndex);
            int y = listY + 15 + i * (rowH + 12);

            SDL_Color bg = selected ? SDL_Color{30, 58, 95, 255} : SDL_Color{28, 34, 44, 255};
            drawRect(listX + 15, y, rowW, rowH, bg, true);

            if (selected) {
                drawBorder(listX + 15, y, rowW, rowH, {0, 180, 216, 255}, 2);
                drawRect(listX + 18, y + 4, 6, rowH - 8, {0, 180, 216, 255}, true);
            }

            // State Pill Badge
            bool isThisDownloading = DownloadManager::instance().isDownloading() &&
                                     DownloadManager::instance().getProgress().gameId == game.id;
            auto dlp = DownloadManager::instance().getProgress();

            if (game.localState == GameState::LOCAL) {
                drawBadge(listX + 25, y + 26, 82, 36, UiStrings::BADGE_DOWNLOADED, {22, 101, 52, 255}, {255, 255, 255, 255});
            } else if (isThisDownloading) {
                char pctBuf[16];
                std::snprintf(pctBuf, sizeof(pctBuf), "%.0f%%", dlp.progressPct);
                drawBadge(listX + 25, y + 26, 82, 36, std::string(pctBuf), {2, 132, 199, 255}, {255, 255, 255, 255});
            } else if (DownloadManager::instance().isInQueue(game.id)) {
                // In queue — find position
                auto q = DownloadManager::instance().getQueue();
                int pos = 1;
                for (const auto& qi : q) {
                    if (qi.game.id == game.id) break;
                    pos++;
                }
                drawBadge(listX + 25, y + 26, 82, 36, "#" + std::to_string(pos), {107, 33, 168, 255}, {255, 255, 255, 255});
            } else if (game.localState == GameState::CLOUD) {
                drawBadge(listX + 25, y + 26, 82, 36, "CLOUD", {30, 58, 138, 255}, {255, 255, 255, 255});
            } else {
                drawBadge(listX + 25, y + 26, 82, 36, UiStrings::BTN_SYNC, {217, 119, 6, 255}, {255, 255, 255, 255});
            }

            // Title truncated if too long
            std::string title = game.title;
            if (title.length() > 26) {
                title = title.substr(0, 23) + "...";
            }
            SDL_Color titleCol = isThisDownloading ? SDL_Color{0, 180, 216, 255} : (selected ? SDL_Color{255, 255, 255, 255} : SDL_Color{210, 220, 230, 255});

            if (isThisDownloading) {
                drawText(title, listX + 120, y + 12, titleCol, m_fontLarge);

                char pctBuf[16];
                std::snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", dlp.progressPct);
                std::string dlSub = FileSystemManager::instance().formatBytes(dlp.bytesDownloaded) + " / " +
                                    FileSystemManager::instance().formatBytes(dlp.totalBytes) + "  (" + pctBuf + ")";
                drawText(dlSub, listX + 120, y + 42, {140, 205, 245, 255}, m_fontSmall);

                // Live in-row progress bar
                int pBarX = listX + 120;
                int pBarY = y + 68;
                int pBarW = rowW - 135;
                int pBarH = 6;
                drawRect(pBarX, pBarY, pBarW, pBarH, {35, 45, 60, 255}, true);
                float pct = std::max(0.0, std::min(100.0, dlp.progressPct));
                drawRect(pBarX, pBarY, (int)(pBarW * (pct / 100.0)), pBarH, {34, 197, 94, 255}, true);
            } else {
                drawText(title, listX + 120, y + 16, titleCol, m_fontLarge);

                std::string sizeStr = FileSystemManager::instance().formatBytes(game.sizeBytes);
                std::string sub = game.filename + "  (" + sizeStr + ")";
                if (sub.length() > 34) {
                    sub = sub.substr(0, 31) + "... (" + sizeStr + ")";
                }
                drawText(sub, listX + 120, y + 50, {140, 155, 175, 255}, m_fontSmall);
            }
        }

        // Scrollbar
        if (totalGames > pageSize) {
            int barX = listX + listW - 10;
            int barTrackH = listH - 30;
            drawRect(barX, listY + 15, 4, barTrackH, {35, 42, 54, 255}, true);

            float ratio = (float)pageSize / (float)totalGames;
            int thumbH = std::max(24, (int)(barTrackH * ratio));
            float scrollRatio = (float)m_gameScrollOffset / (float)(totalGames - pageSize);
            int thumbY = listY + 15 + (int)((barTrackH - thumbH) * scrollRatio);
            drawRect(barX, thumbY, 4, thumbH, {0, 180, 216, 255}, true);
        }
    }

    // 2. Render Right Details & Cover Panel
    drawRect(detailX, detailY, detailW, detailH, {20, 25, 32, 255}, true);
    drawBorder(detailX, detailY, detailW, detailH, {40, 50, 65, 255}, 2);

    const GameRecord* selGame = (totalGames > 0 && m_selectedGameIndex < totalGames) ? &m_cachedGames[m_selectedGameIndex] : nullptr;

    // Cover Art Box
    int coverBoxW = 340;
    int coverBoxH = 280;
    int coverBoxX = detailX + (detailW - coverBoxW) / 2;
    int coverBoxY = detailY + 20;

    CoverManager::instance().renderCoverBox(coverBoxX, coverBoxY, coverBoxW, coverBoxH, selGame, &m_activeSystem, m_fontMedium);

    // Detail Metadata
    if (selGame) {
        int metaY = coverBoxY + coverBoxH + 18;

        std::string title = selGame->title;
        if (title.length() > 24) title = title.substr(0, 21) + "...";
        drawText(title, detailX + 25, metaY, {255, 255, 255, 255}, m_fontLarge);

        metaY += 38;
        drawText(UiStrings::DETAIL_SYS_LABEL, detailX + 25, metaY, {140, 155, 175, 255}, m_fontSmall);
        drawText(m_activeSystem.name + " (" + m_activeSystem.code + ")", detailX + 130, metaY, {0, 180, 216, 255}, m_fontSmall);

        metaY += 30;
        drawText(UiStrings::DETAIL_SIZE_LABEL, detailX + 25, metaY, {140, 155, 175, 255}, m_fontSmall);
        drawText(FileSystemManager::instance().formatBytes(selGame->sizeBytes), detailX + 130, metaY, {255, 255, 255, 255}, m_fontSmall);

        metaY += 30;
        drawText(UiStrings::DETAIL_LOCATION_LABEL, detailX + 25, metaY, {140, 155, 175, 255}, m_fontSmall);
        if (selGame->localState == GameState::LOCAL) {
            drawText("Thẻ nhớ (/Roms/" + m_activeSystem.romDir + ")", detailX + 130, metaY, {34, 197, 94, 255}, m_fontSmall);
        } else {
            drawText("Google Drive Cloud", detailX + 130, metaY, {0, 180, 216, 255}, m_fontSmall);
        }

        // Action Status Pill & Live Progress
        metaY += 46;
        if (selGame->localState == GameState::LOCAL) {
            int halfW = (detailW - 58) / 2;
            drawBadge(detailX + 25, metaY, halfW, 46, UiStrings::BADGE_DOWNLOADED, {22, 101, 52, 255}, {255, 255, 255, 255});
            drawBadge(detailX + 33 + halfW, metaY, halfW, 46, UiStrings::BADGE_DELETE_BTN, {185, 28, 28, 255}, {255, 255, 255, 255});
        } else if (DownloadManager::instance().isDownloading() &&
                   DownloadManager::instance().getProgress().gameId == selGame->id) {
            auto dlp = DownloadManager::instance().getProgress();
            char pctBuf[32];
            std::snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", dlp.progressPct);
            std::string dlInfo = "ĐANG TẢI... " + std::string(pctBuf);
            drawBadge(detailX + 25, metaY, detailW - 50, 40, dlInfo, {2, 132, 199, 255}, {255, 255, 255, 255});

            int dBarX = detailX + 25;
            int dBarY = metaY + 46;
            int dBarW = detailW - 50;
            int dBarH = 10;
            drawRect(dBarX, dBarY, dBarW, dBarH, {35, 45, 60, 255}, true);
            float pct = std::max(0.0, std::min(100.0, dlp.progressPct));
            drawRect(dBarX, dBarY, (int)(dBarW * (pct / 100.0)), dBarH, {34, 197, 94, 255}, true);

            std::string dSizeStr = FileSystemManager::instance().formatBytes(dlp.bytesDownloaded) +
                                   " / " + FileSystemManager::instance().formatBytes(dlp.totalBytes);
            drawText(dSizeStr, detailX + detailW / 2, dBarY + 16, {200, 220, 240, 255}, m_fontSmall, true);

            drawBadge(detailX + 25, dBarY + 38, detailW - 50, 36, UiStrings::BADGE_CANCEL_DL_BTN, {185, 28, 28, 255}, {255, 255, 255, 255});
            metaY += 56;
        } else if (DownloadManager::instance().isInQueue(selGame->id)) {
            drawBadge(detailX + 25, metaY, detailW - 50, 46, UiStrings::BADGE_REMOVE_QUEUE_BTN, {107, 33, 168, 255}, {255, 255, 255, 255});
        } else {
            drawBadge(detailX + 25, metaY, detailW - 50, 46, UiStrings::BADGE_ADD_QUEUE_BTN, {2, 132, 199, 255}, {255, 255, 255, 255});
        }

        // Queue info panel below action pill
        int queueCount = DownloadManager::instance().queueSize();
        bool isCurrentlyDownloading = DownloadManager::instance().isDownloading();
        if (queueCount > 0 || isCurrentlyDownloading) {
            metaY += 54;
            std::string qInfo;
            if (isCurrentlyDownloading && queueCount > 0) {
                qInfo = "Đang tải 1 game, còn " + std::to_string(queueCount) + " game chờ.";
            } else if (isCurrentlyDownloading) {
                qInfo = UiStrings::QUEUE_DOWNLOADING_EMPTY;
            } else {
                qInfo = "Hàng tải: " + std::to_string(queueCount) + " game chờ.";
            }
            drawText(qInfo, detailX + 25, metaY, {168, 85, 247, 255}, m_fontSmall);
        }
    }

    // Active download indicator banner at bottom right if user is browsing another game
    if (DownloadManager::instance().isDownloading() &&
        (!selGame || DownloadManager::instance().getProgress().gameId != selGame->id)) {
        auto activeProg = DownloadManager::instance().getProgress();
        int bY = detailY + detailH - 74;
        drawRect(detailX + 15, bY, detailW - 30, 60, {18, 28, 44, 255}, true);
        drawBorder(detailX + 15, bY, detailW - 30, 60, {0, 180, 216, 255}, 1);

        char pBuf[16];
        std::snprintf(pBuf, sizeof(pBuf), "%.0f%%", activeProg.progressPct);
        std::string tTrunc = activeProg.gameTitle;
        if (tTrunc.length() > 18) tTrunc = tTrunc.substr(0, 15) + "...";
        drawText("⬇ " + tTrunc, detailX + 25, bY + 8, {0, 180, 216, 255}, m_fontSmall);
        drawText(std::string(pBuf), detailX + detailW - 65, bY + 8, {34, 197, 94, 255}, m_fontSmall);

        int aBarW = detailW - 50;
        int aBarH = 6;
        drawRect(detailX + 25, bY + 36, aBarW, aBarH, {30, 40, 55, 255}, true);
        float aPct = std::max(0.0, std::min(100.0, activeProg.progressPct));
        drawRect(detailX + 25, bY + 36, (int)(aBarW * (aPct / 100.0)), aBarH, {34, 197, 94, 255}, true);
    }
}

void UIManager::renderConfirmDeleteDialog() {
    // Dim background overlay
    drawRect(0, 0, 1024, 768, {0, 0, 0, 190}, true);

    int dlgW = 640;
    int dlgH = 340;
    int dlgX = (1024 - dlgW) / 2;
    int dlgY = (768 - dlgH) / 2;

    drawRect(dlgX, dlgY, dlgW, dlgH, {24, 28, 38, 255}, true);
    drawBorder(dlgX, dlgY, dlgW, dlgH, {239, 68, 68, 255}, 3);

    // Title Banner
    drawRect(dlgX, dlgY, dlgW, 55, {185, 28, 28, 255}, true);
    drawText(UiStrings::DIALOG_DELETE_TITLE, dlgX + dlgW / 2, dlgY + 16, {255, 255, 255, 255}, m_fontLarge, true);

    if (m_selectedGameIndex >= 0 && m_selectedGameIndex < static_cast<int>(m_cachedGames.size())) {
        const auto& game = m_cachedGames[m_selectedGameIndex];

        drawText(game.title, dlgX + dlgW / 2, dlgY + 85, {255, 255, 255, 255}, m_fontMedium, true);
        std::string sizeStr = "Tập tin: " + game.filename + " (" + FileSystemManager::instance().formatBytes(game.sizeBytes) + ")";
        drawText(sizeStr, dlgX + dlgW / 2, dlgY + 120, {0, 180, 216, 255}, m_fontSmall, true);

        drawText(UiStrings::DIALOG_DELETE_PROMPT, dlgX + dlgW / 2, dlgY + 165, {220, 225, 235, 255}, m_fontSmall, true);
        drawText(UiStrings::DIALOG_DELETE_SAFE_HINT, dlgX + dlgW / 2, dlgY + 195, {34, 197, 94, 255}, m_fontSmall, true);
    }

    // Action buttons
    int btnW = 220;
    int btnH = 50;
    drawBadge(dlgX + 60, dlgY + 250, btnW, btnH, UiStrings::BTN_CONFIRM_DELETE, {185, 28, 28, 255}, {255, 255, 255, 255});
    drawBadge(dlgX + dlgW - 60 - btnW, dlgY + 250, btnW, btnH, UiStrings::BTN_CANCEL_DELETE, {55, 65, 81, 255}, {255, 255, 255, 255});
}

void UIManager::renderDisclaimerState() {
    int cardX = 80;
    int cardY = 80;
    int cardW = 864;
    int cardH = 615;

    drawRect(cardX, cardY, cardW, cardH, {22, 27, 36, 255}, true);
    drawBorder(cardX, cardY, cardW, cardH, {245, 158, 11, 255}, 3);

    // Amber warning header
    drawRect(cardX, cardY, cardW, 58, {50, 32, 12, 255}, true);
    drawText(UiStrings::DISCLAIMER_TITLE, 512, cardY + 18, {245, 158, 11, 255}, m_fontLarge, true);

    // Inner panel
    int innerX = cardX + 30;
    int innerY = cardY + 76;
    int innerW = cardW - 60;
    int innerH = 435;
    drawRect(innerX, innerY, innerW, innerH, {16, 20, 28, 255}, true);
    drawBorder(innerX, innerY, innerW, innerH, {45, 55, 72, 255}, 1);

    int textX = innerX + 28;
    int textY = innerY + 22;

    drawText(UiStrings::DISCLAIMER_SUBTITLE, 512, textY, {255, 255, 255, 255}, m_fontMedium, true);
    drawRect(innerX + 40, textY + 34, innerW - 80, 1, {60, 72, 90, 255}, true);

    textY += 52;
    drawText(UiStrings::DISCLAIMER_SEC1_TITLE, textX, textY, {0, 180, 216, 255}, m_fontMedium);
    textY += 28;
    drawText(UiStrings::DISCLAIMER_SEC1_LINE1, textX + 15, textY, {200, 210, 225, 255}, m_fontSmall);
    textY += 25;
    drawText(UiStrings::DISCLAIMER_SEC1_LINE2, textX + 15, textY, {239, 68, 68, 255}, m_fontSmall);

    textY += 40;
    drawText(UiStrings::DISCLAIMER_SEC2_TITLE, textX, textY, {0, 180, 216, 255}, m_fontMedium);
    textY += 28;
    drawText(UiStrings::DISCLAIMER_SEC2_LINE1, textX + 15, textY, {200, 210, 225, 255}, m_fontSmall);
    textY += 25;
    drawText(UiStrings::DISCLAIMER_SEC2_LINE2, textX + 15, textY, {200, 210, 225, 255}, m_fontSmall);

    textY += 40;
    drawText(UiStrings::DISCLAIMER_SEC3_TITLE, textX, textY, {245, 158, 11, 255}, m_fontMedium);
    textY += 28;
    drawText(UiStrings::DISCLAIMER_SEC3_LINE1, textX + 15, textY, {253, 224, 71, 255}, m_fontSmall);
    textY += 25;
    drawText(UiStrings::DISCLAIMER_SEC3_LINE2, textX + 15, textY, {253, 224, 71, 255}, m_fontSmall);
    textY += 25;
    drawText(UiStrings::DISCLAIMER_SEC3_LINE3, textX + 15, textY, {253, 224, 71, 255}, m_fontSmall);

    // Action buttons at the bottom of card
    int btnY = cardY + 530;
    int btnH = 54;
    int btnW = 360;
    int btn1X = cardX + 50;
    int btn2X = cardX + cardW - 50 - btnW;

    drawBadge(btn1X, btnY, btnW, btnH, UiStrings::DISCLAIMER_AGREE, {22, 101, 52, 255}, {255, 255, 255, 255});
    drawBadge(btn2X, btnY, btnW, btnH, UiStrings::DISCLAIMER_DECLINE, {75, 85, 99, 255}, {255, 255, 255, 255});
}

void UIManager::renderSettingsState() {
    int cardX = 90;
    int cardY = 80;
    int cardW = 844;
    int cardH = 610;

    drawRect(cardX, cardY, cardW, cardH, {25, 30, 38, 255}, true);
    drawBorder(cardX, cardY, cardW, cardH, {0, 180, 216, 255}, 2);

    drawText(UiStrings::HEADER_SETTINGS, 512, cardY + 22, {0, 180, 216, 255}, m_fontTitle ? m_fontTitle : m_fontLarge, true);

    int rowY = cardY + 80;
    int stepY = 52;

    // Google Drive Account Section
    drawText(UiStrings::SETTING_DRIVE_STATUS, cardX + 40, rowY, {160, 175, 190, 255}, m_fontMedium);
    if (AuthManager::instance().isLinked()) {
        std::string email = AuthManager::instance().getUserEmail();
        drawText(UiStrings::SETTING_CONNECTED, cardX + 320, rowY, {34, 197, 94, 255}, m_fontMedium);
        drawBadge(cardX + 610, rowY - 6, 190, 38, UiStrings::SETTING_LOGOUT_BTN, {185, 28, 28, 255}, {255, 255, 255, 255});
    } else {
        drawText(UiStrings::SETTING_DISCONNECTED, cardX + 320, rowY, {239, 68, 68, 255}, m_fontMedium);
        drawBadge(cardX + 510, rowY - 6, 290, 38, UiStrings::SETTING_CONNECT_WEB_BTN, {30, 58, 138, 255}, {255, 255, 255, 255});
    }

    rowY += stepY;
    drawText(UiStrings::SETTING_DRIVE_FOLDER, cardX + 40, rowY, {160, 175, 190, 255}, m_fontMedium);
    std::string folderId = DatabaseManager::instance().getSetting("drive_folder_id", UiStrings::SETTING_NOT_CONFIGURED);
    drawText(folderId, cardX + 320, rowY, {0, 180, 216, 255}, m_fontSmall);

    rowY += stepY;
    drawText(UiStrings::SETTING_ROM_SD_FOLDER, cardX + 40, rowY, {160, 175, 190, 255}, m_fontMedium);
    drawText(AppConfig::instance().getRomsDir(), cardX + 320, rowY, {255, 255, 255, 255}, m_fontSmall);

    rowY += stepY;
    drawText(UiStrings::SETTING_LAST_SYNC, cardX + 40, rowY, {160, 175, 190, 255}, m_fontMedium);
    std::string lastSync = DatabaseManager::instance().getSetting("last_cloud_sync_time", UiStrings::SETTING_NEVER_SYNCED);
    drawText(lastSync, cardX + 320, rowY, {255, 255, 255, 255}, m_fontSmall);

    rowY += stepY;
    drawText(UiStrings::SETTING_SQLITE_DB, cardX + 40, rowY, {160, 175, 190, 255}, m_fontMedium);
    drawText(AppConfig::instance().getDatabasePath(), cardX + 320, rowY, {34, 197, 94, 255}, m_fontSmall);

    rowY += stepY;
    drawText(UiStrings::SETTING_SCAN_MODE, cardX + 40, rowY, {160, 175, 190, 255}, m_fontMedium);
    drawText(UiStrings::SETTING_SCAN_AUTO, cardX + 320, rowY, {34, 197, 94, 255}, m_fontSmall);

    rowY += stepY;
    drawText(UiStrings::SETTING_WEB_PORTAL, cardX + 40, rowY, {160, 175, 190, 255}, m_fontMedium);
    std::string ip = PlatformInfo::instance().getIpAddress("wlan0");
    drawText("http://" + (ip.empty() ? "192.168.1.164" : ip) + ":8080", cardX + 320, rowY, {0, 180, 216, 255}, m_fontMedium);

    rowY += stepY;
    drawText(UiStrings::SETTING_COVER_CACHE, cardX + 40, rowY, {160, 175, 190, 255}, m_fontMedium);
    drawText(UiStrings::SETTING_COVER_CACHE_VAL, cardX + 320, rowY, {34, 197, 94, 255}, m_fontSmall);

    drawText(UiStrings::BTN_BACK_MAIN_MENU_HINT, 512, cardY + 560, {150, 165, 180, 255}, m_fontMedium, true);
}

void UIManager::renderCloudLoginState() {
    int cardX = 72;
    int cardY = 82;
    int cardW = 880;
    int cardH = 610;

    drawRect(cardX, cardY, cardW, cardH, {22, 27, 36, 255}, true);
    drawBorder(cardX, cardY, cardW, cardH, {0, 180, 216, 255}, 2);

    drawText(UiStrings::HEADER_WEB_CONNECT, 512, cardY + 22, {0, 180, 216, 255}, m_fontTitle ? m_fontTitle : m_fontLarge, true);

    if (AuthManager::instance().isLinked()) {
        int boxW = 680;
        int boxH = 340;
        int boxX = (1024 - boxW) / 2;
        int boxY = (768 - boxH) / 2;

        drawRect(boxX, boxY, boxW, boxH, {20, 25, 35, 255}, true);
        drawBorder(boxX, boxY, boxW, boxH, {34, 197, 94, 255}, 3);

        drawRect(boxX, boxY, boxW, 60, {22, 101, 52, 255}, true);
        drawText(UiStrings::WEB_CONNECT_SUCCESS, boxX + boxW / 2, boxY + 16, {255, 255, 255, 255}, m_fontLarge, true);

        drawText(UiStrings::WEB_CONNECT_ACCOUNT_PREFIX, boxX + boxW / 2, boxY + 95, {150, 165, 180, 255}, m_fontMedium, true);
        drawText(AuthManager::instance().getUserEmail(), boxX + boxW / 2, boxY + 135, {0, 180, 216, 255}, m_fontLarge, true);
        drawText(UiStrings::WEB_CONNECT_READY, boxX + boxW / 2, boxY + 195, {200, 210, 220, 255}, m_fontMedium, true);

        drawBadge(boxX + (boxW - 280) / 2, boxY + 250, 280, 52, UiStrings::WEB_CONNECT_START_BTN, {22, 101, 52, 255}, {255, 255, 255, 255});
    } else {
        std::string ip = PlatformInfo::instance().getIpAddress("wlan0");
        if (ip.empty()) ip = "192.168.1.164";
        std::string portalUrl = "http://" + ip + ":8080";

        // Inner panel
        int innerX = cardX + 35;
        int innerY = cardY + 75;
        int innerW = cardW - 70;
        int innerH = 435;
        drawRect(innerX, innerY, innerW, innerH, {16, 20, 28, 255}, true);
        drawBorder(innerX, innerY, innerW, innerH, {45, 55, 72, 255}, 1);

        int textY = innerY + 30;

        drawText(UiStrings::WEB_CONNECT_GUIDE_TITLE, 512, textY, {255, 255, 255, 255}, m_fontMedium, true);
        drawRect(innerX + 50, textY + 32, innerW - 100, 1, {60, 72, 90, 255}, true);

        textY += 55;
        drawText(UiStrings::WEB_CONNECT_STEP1, innerX + 40, textY, {200, 215, 230, 255}, m_fontMedium);

        textY += 45;
        drawText(UiStrings::WEB_CONNECT_STEP2, innerX + 40, textY, {200, 215, 230, 255}, m_fontMedium);

        // Prominent glowing URL box in center
        textY += 38;
        int urlBoxW = 620;
        int urlBoxH = 68;
        int urlBoxX = innerX + (innerW - urlBoxW) / 2;
        drawRect(urlBoxX, textY, urlBoxW, urlBoxH, {10, 15, 22, 255}, true);
        drawBorder(urlBoxX, textY, urlBoxW, urlBoxH, {0, 180, 216, 255}, 2);
        drawText(portalUrl, urlBoxX + urlBoxW / 2, textY + 18, {0, 180, 216, 255}, m_fontTitle ? m_fontTitle : m_fontLarge, true);

        textY += urlBoxH + 34;
        drawText(UiStrings::WEB_CONNECT_STEP3, innerX + 40, textY, {200, 215, 230, 255}, m_fontMedium);

        textY += 42;
        drawText(UiStrings::WEB_CONNECT_WAITING, 512, textY, {245, 158, 11, 255}, m_fontMedium, true);

        textY += 28;
        drawText(UiStrings::WEB_CONNECT_AUTO_HINT, 512, textY, {140, 155, 170, 255}, m_fontSmall, true);

        // Bottom action button
        drawBadge(512 - 130, cardY + cardH - 68, 260, 48, UiStrings::WEB_CONNECT_BACK_BTN, {55, 65, 81, 255}, {255, 255, 255, 255});
    }
}

void UIManager::renderSyncOverlay() {
    if (!DriveSyncEngine::instance().isSyncing()) return;

    // Semi-transparent dim background
    drawRect(0, 0, 1024, 768, {0, 0, 0, 210}, true);

    int boxW = 700;
    int boxH = 380;
    int boxX = (1024 - boxW) / 2;
    int boxY = (768 - boxH) / 2;

    drawRect(boxX, boxY, boxW, boxH, {22, 27, 36, 255}, true);
    drawBorder(boxX, boxY, boxW, boxH, {0, 180, 216, 255}, 3);

    // Title banner
    drawRect(boxX, boxY, boxW, 55, {18, 55, 95, 255}, true);
    drawText(UiStrings::HEADER_SYNC, boxX + boxW / 2, boxY + 16, {0, 180, 216, 255}, m_fontLarge, true);

    auto prog = DriveSyncEngine::instance().getProgress();

    int contentY = boxY + 85;
    if (prog.status == SyncStatus::CONNECTING) {
        drawText(UiStrings::SYNC_API_CONNECTING, boxX + boxW / 2, contentY, {255, 255, 255, 255}, m_fontMedium, true);
        drawText(UiStrings::SYNC_AUTH_TOKEN, boxX + boxW / 2, contentY + 35, {150, 165, 180, 255}, m_fontSmall, true);
    } else if (prog.status == SyncStatus::DISCOVERING_FOLDERS) {
        drawText(UiStrings::SYNC_SCANNING_GAMES, boxX + boxW / 2, contentY, {255, 255, 255, 255}, m_fontMedium, true);
        drawText(UiStrings::SYNC_SEARCH_FOLDERS, boxX + boxW / 2, contentY + 35, {150, 165, 180, 255}, m_fontSmall, true);
    } else if (prog.status == SyncStatus::SYNCING_FILES) {
        std::string platText = "Đang quét hệ máy: " + prog.currentPlatform;
        drawText(platText, boxX + boxW / 2, contentY, {255, 255, 255, 255}, m_fontMedium, true);

        std::string progressStr = "Hệ máy " + std::to_string(prog.currentSystemIndex) + " / " + std::to_string(prog.totalSystems);
        drawText(progressStr, boxX + boxW / 2, contentY + 35, {0, 180, 216, 255}, m_fontSmall, true);

        // Progress bar
        int barW = 540;
        int barH = 16;
        int barX = boxX + (boxW - barW) / 2;
        int barY = contentY + 70;
        drawRect(barX, barY, barW, barH, {35, 42, 54, 255}, true);
        if (prog.totalSystems > 0) {
            float pct = (float)prog.currentSystemIndex / (float)prog.totalSystems;
            drawRect(barX, barY, (int)(barW * pct), barH, {0, 180, 216, 255}, true);
        }

        int statY = contentY + 110;
        std::string stats = "Đã lưu vào kho: " + std::to_string(prog.cloudGamesFound) +
                            "  (Mới: " + std::to_string(prog.newGamesIndexed) +
                            ", Cập nhật: " + std::to_string(prog.updatedGames) + ")";
        drawText(stats, boxX + boxW / 2, statY, {34, 197, 94, 255}, m_fontSmall, true);
    }

    drawBadge(boxX + (boxW - 220) / 2, boxY + 295, 220, 50, UiStrings::SYNC_CANCEL_BTN, {55, 65, 81, 255}, {255, 255, 255, 255});
}

void UIManager::renderDownloadOverlay() {
    if (!DownloadManager::instance().isDownloading()) return;

    // Semi-transparent dim background
    drawRect(0, 0, 1024, 768, {0, 0, 0, 210}, true);

    int boxW = 720;
    int boxH = 390;
    int boxX = (1024 - boxW) / 2;
    int boxY = (768 - boxH) / 2;

    drawRect(boxX, boxY, boxW, boxH, {22, 27, 36, 255}, true);
    drawBorder(boxX, boxY, boxW, boxH, {0, 180, 216, 255}, 3);

    // Title banner
    drawRect(boxX, boxY, boxW, 55, {18, 55, 95, 255}, true);
    drawText(UiStrings::HEADER_DOWNLOAD, boxX + boxW / 2, boxY + 16, {0, 180, 216, 255}, m_fontLarge, true);

    auto prog = DownloadManager::instance().getProgress();

    int contentY = boxY + 80;
    std::string title = prog.gameTitle;
    if (title.length() > 32) title = title.substr(0, 29) + "...";
    drawText(title, boxX + boxW / 2, contentY, {255, 255, 255, 255}, m_fontLarge, true);

    std::string sysSub = "Hệ máy: " + prog.systemCode + "  |  Tập tin: " + prog.filename;
    drawText(sysSub, boxX + boxW / 2, contentY + 38, {0, 180, 216, 255}, m_fontSmall, true);

    // State text
    std::string statusMsg = UiStrings::DL_FROM_DRIVE;
    if (prog.state == DownloadState::INITIALIZING) {
        statusMsg = UiStrings::DL_CONNECTING;
    } else if (prog.state == DownloadState::VERIFYING) {
        statusMsg = UiStrings::DL_CHECKING_FILE;
    }
    drawText(statusMsg, boxX + boxW / 2, contentY + 75, {245, 158, 11, 255}, m_fontSmall, true);

    // Progress bar
    int barW = 560;
    int barH = 18;
    int barX = boxX + (boxW - barW) / 2;
    int barY = contentY + 105;
    drawRect(barX, barY, barW, barH, {35, 42, 54, 255}, true);
    float pct = std::max(0.0, std::min(100.0, prog.progressPct));
    drawRect(barX, barY, (int)(barW * (pct / 100.0)), barH, {34, 197, 94, 255}, true);

    // Size details
    std::string downloadedStr = FileSystemManager::instance().formatBytes(prog.bytesDownloaded);
    std::string totalStr = FileSystemManager::instance().formatBytes(prog.totalBytes);
    char pctBuf[32];
    std::snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", pct);

    std::string progressInfo = downloadedStr + " / " + totalStr + "  (" + pctBuf + ")";
    drawText(progressInfo, boxX + boxW / 2, barY + 28, {255, 255, 255, 255}, m_fontSmall, true);

    // Queue remaining count
    int remaining = DownloadManager::instance().queueSize();
    if (remaining > 0) {
        std::string qStr = "Còn " + std::to_string(remaining) + " game trong hàng chờ.";
        drawText(qStr, boxX + boxW / 2, barY + 56, {168, 85, 247, 255}, m_fontSmall, true);
    }

    drawBadge(boxX + (boxW - 220) / 2, boxY + 320, 220, 50, UiStrings::DL_CANCEL_BTN, {55, 65, 81, 255}, {255, 255, 255, 255});
}

void UIManager::renderDiagnosticsState() {
    int cardX = 72;
    int cardY = 85;
    int cardW = 880;
    int cardH = 605;

    drawRect(cardX, cardY, cardW, cardH, {22, 27, 34, 255}, true);
    drawBorder(cardX, cardY, cardW, cardH, {0, 180, 216, 255}, 2);

    drawText(UiStrings::HEADER_DIAG, 512, cardY + 22, {0, 180, 216, 255}, m_fontLarge, true);

    auto diag = PlatformInfo::instance().getDiagnostics();

    struct DiagRow {
        std::string label;
        std::string value;
        SDL_Color valColor;
    };

    std::vector<DiagRow> rows = {
        {UiStrings::DIAG_HW_DEVICE, diag.socName, {255, 255, 255, 255}},
        {UiStrings::DIAG_CPU_ARCH, diag.cpuArch + " (64-bit Little Endian)", {255, 255, 255, 255}},
        {UiStrings::DIAG_OS_KERNEL, diag.osName + " " + diag.kernelRelease, {255, 255, 255, 255}},
        {UiStrings::DIAG_RAM, "Còn trống " + diag.freeRam + " / Tổng " + diag.totalRam, {34, 197, 94, 255}},
        {UiStrings::DIAG_DISPLAY, diag.displayResolution, {0, 180, 216, 255}},
        {UiStrings::DIAG_SDL2_GFX, "v" + diag.sdlVersion + " (Tăng tốc phần cứng)", {255, 255, 255, 255}},
        {UiStrings::DIAG_SQLITE_DB, "v" + diag.sqliteVersion + " (Phiên bản cấu trúc v" + std::to_string(CURRENT_SCHEMA_VERSION) + ")", {34, 197, 94, 255}},
        {UiStrings::DIAG_SD_STORAGE, "Còn trống " + diag.sdFreeSpace + " / Tổng " + diag.sdTotalSpace, {34, 197, 94, 255}},
        {UiStrings::DIAG_GAMEPAD, diag.controllerName, {255, 255, 255, 255}},
        {UiStrings::DIAG_WIFI, diag.networkStatus + " (IP: " + diag.ipAddress + ")", diag.ipAddress != "N/A" ? SDL_Color{34, 197, 94, 255} : SDL_Color{239, 68, 68, 255}},
        {UiStrings::DIAG_SAFETY, UiStrings::DIAG_SAFETY_VAL, {34, 197, 94, 255}}
    };

    int startY = cardY + 70;
    int rowH = 40;

    for (size_t i = 0; i < rows.size(); ++i) {
        int y = startY + static_cast<int>(i) * rowH;
        if (i % 2 == 1) {
            drawRect(cardX + 20, y - 4, cardW - 40, rowH, {28, 34, 44, 255}, true);
        }
        drawText(rows[i].label, cardX + 35, y, {150, 165, 180, 255}, m_fontSmall);
        drawText(rows[i].value, cardX + 270, y, rows[i].valColor, m_fontSmall);
    }

    drawText(UiStrings::BTN_BACK_MAIN_MENU_HINT, 512, cardY + 560, {130, 140, 155, 255}, m_fontSmall, true);
}

void UIManager::renderOTAUpdateState() {
    int cardX = 72;
    int cardY = 85;
    int cardW = 880;
    int cardH = 605;

    drawRect(cardX, cardY, cardW, cardH, {22, 27, 34, 255}, true);
    drawBorder(cardX, cardY, cardW, cardH, {0, 180, 216, 255}, 2);

    drawText(UiStrings::HEADER_OTA, 512, cardY + 22, {0, 180, 216, 255}, m_fontLarge, true);

    auto prog = UpdateManager::instance().getProgress();
    auto info = UpdateManager::instance().getLatestInfo();

    std::string currentVer = "Phiên bản hiện tại trên máy: v" + UpdateManager::instance().getCurrentVersion();
    drawText(currentVer, cardX + 45, cardY + 70, {210, 220, 235, 255}, m_fontMedium);

    std::string repoSource = "Nguồn phát hành: GitHub @" + std::string(GITHUB_REPO);
    drawText(repoSource, cardX + 45, cardY + 105, {130, 145, 165, 255}, m_fontSmall);

    int contentBoxY = cardY + 145;
    int contentBoxH = 345;
    drawRect(cardX + 40, contentBoxY, cardW - 80, contentBoxH, {16, 20, 26, 255}, true);
    drawBorder(cardX + 40, contentBoxY, cardW - 80, contentBoxH, {40, 50, 68, 255}, 1);

    switch (prog.state) {
        case UpdateState::IDLE:
        case UpdateState::CHECKING: {
            drawText(UiStrings::OTA_CHECKING, 512, contentBoxY + 130, {245, 158, 11, 255}, m_fontLarge, true);
            drawText(UiStrings::OTA_WAITING, 512, contentBoxY + 175, {150, 165, 180, 255}, m_fontSmall, true);
            break;
        }
        case UpdateState::UP_TO_DATE: {
            drawBadge(420, contentBoxY + 50, 184, 40, UiStrings::OTA_STATUS_UP_TO_DATE, {22, 101, 52, 255}, {34, 197, 94, 255});
            drawText(UiStrings::OTA_MSG_UP_TO_DATE, 512, contentBoxY + 120, {34, 197, 94, 255}, m_fontLarge, true);
            drawText(UiStrings::OTA_NO_NEW_UPDATE, 512, contentBoxY + 165, {170, 180, 195, 255}, m_fontSmall, true);

            drawBadge(362, contentBoxY + 235, 300, 48, UiStrings::OTA_BTNS_CHECK_BACK, {35, 45, 60, 255}, {255, 255, 255, 255});
            break;
        }
        case UpdateState::UPDATE_AVAILABLE: {
            drawBadge(392, contentBoxY + 25, 240, 36, UiStrings::OTA_STATUS_NEW_UPDATE, {180, 83, 9, 255}, {255, 255, 255, 255});
            std::string newVerTxt = "Phiên bản mới: v" + info.remoteVersion + (info.releaseDate.empty() ? "" : " (" + info.releaseDate + ")");
            drawText(newVerTxt, 512, contentBoxY + 80, {0, 180, 216, 255}, m_fontLarge, true);

            if (!info.changelog.empty()) {
                drawText(UiStrings::OTA_CHANGELOG_TITLE, cardX + 70, contentBoxY + 125, {255, 255, 255, 255}, m_fontSmall);
                drawText(info.changelog, cardX + 70, contentBoxY + 155, {170, 180, 195, 255}, m_fontSmall);
            }

            drawBadge(337, contentBoxY + 245, 350, 52, UiStrings::OTA_BTN_INSTALL_NOW, {34, 197, 94, 255}, {0, 0, 0, 255});
            break;
        }
        case UpdateState::DOWNLOADING:
        case UpdateState::VERIFYING: {
            drawText(UiStrings::OTA_DOWNLOADING_TITLE, 512, contentBoxY + 50, {0, 180, 216, 255}, m_fontLarge, true);

            int barW = 560;
            int barH = 22;
            int barX = 512 - barW / 2;
            int barY = contentBoxY + 115;
            drawRect(barX, barY, barW, barH, {35, 42, 54, 255}, true);
            float pct = std::max(0.0, std::min(100.0, prog.progressPct));
            drawRect(barX, barY, (int)(barW * (pct / 100.0)), barH, {34, 197, 94, 255}, true);

            char pctBuf[32];
            std::snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", pct);
            std::string dlStr = FileSystemManager::instance().formatBytes(prog.bytesDownloaded);
            std::string totStr = FileSystemManager::instance().formatBytes(prog.totalBytes);
            std::string progressInfo = dlStr + " / " + totStr + " (" + pctBuf + ")";
            drawText(progressInfo, 512, barY + 34, {255, 255, 255, 255}, m_fontSmall, true);

            if (prog.state == UpdateState::VERIFYING) {
                drawText(UiStrings::OTA_VERIFYING_FILE, 512, contentBoxY + 190, {245, 158, 11, 255}, m_fontSmall, true);
            }
            drawBadge(422, contentBoxY + 255, 180, 44, UiStrings::OTA_BTN_CANCEL_DOWNLOAD, {55, 65, 81, 255}, {255, 255, 255, 255});
            break;
        }
        case UpdateState::COMPLETED: {
            drawBadge(412, contentBoxY + 35, 200, 40, UiStrings::OTA_STATUS_COMPLETED, {22, 101, 52, 255}, {34, 197, 94, 255});
            drawText(UiStrings::OTA_MSG_COMPLETED, 512, contentBoxY + 105, {34, 197, 94, 255}, m_fontLarge, true);
            drawText(UiStrings::OTA_MSG_RESTART_HINT, 512, contentBoxY + 150, {255, 255, 255, 255}, m_fontSmall, true);

            drawBadge(327, contentBoxY + 235, 370, 52, UiStrings::OTA_BTN_RESTART_NOW, {34, 197, 94, 255}, {0, 0, 0, 255});
            break;
        }
        case UpdateState::FAILED: {
            drawBadge(422, contentBoxY + 35, 180, 40, UiStrings::OTA_STATUS_FAILED, {153, 27, 27, 255}, {248, 113, 113, 255});
            drawText(UiStrings::OTA_MSG_FAILED, 512, contentBoxY + 105, {239, 68, 68, 255}, m_fontLarge, true);
            std::string err = prog.errorMessage.empty() ? UiStrings::OTA_ERR_NETWORK : prog.errorMessage;
            drawText(err, 512, contentBoxY + 150, {245, 158, 11, 255}, m_fontSmall, true);

            drawBadge(362, contentBoxY + 235, 300, 48, UiStrings::OTA_BTNS_RETRY_BACK, {35, 45, 60, 255}, {255, 255, 255, 255});
            break;
        }
    }

    drawText(UiStrings::BTN_BACK_MAIN_MENU_HINT, 512, cardY + 560, {130, 140, 155, 255}, m_fontSmall, true);
}

void UIManager::render() {
    SDL_SetRenderDrawColor(m_renderer, 13, 17, 23, 255);
    SDL_RenderClear(m_renderer);

    renderHeader();

    switch (m_currentState) {
        case UIState::MENU:             renderMenuState(); break;
        case UIState::SYSTEM_SELECT:    renderSystemSelectState(); break;
        case UIState::GAME_LIST:        renderGameListState(); break;
        case UIState::SEARCH:           renderSearchState(); break;
        case UIState::CONFIRM_DELETE:   renderGameListState(); renderConfirmDeleteDialog(); break;
        case UIState::DISCLAIMER:       renderDisclaimerState(); break;
        case UIState::CLOUD_LOGIN:      renderCloudLoginState(); break;
        case UIState::SETTINGS:         renderSettingsState(); break;
        case UIState::DIAGNOSTICS:      renderDiagnosticsState(); break;
        case UIState::OTA_UPDATE:       renderOTAUpdateState(); break;
        default: break;
    }

    renderFooter();
    renderToast();
    renderSyncOverlay();
    SDL_RenderPresent(m_renderer);
}

} // namespace RomCloud
