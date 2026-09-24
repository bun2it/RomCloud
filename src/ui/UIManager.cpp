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
#include "../backup/BackupManager.h"
#include "../sync/UploadManager.h"
#include "../iptv/IPTVManager.h"
#include "BoxartScraper.h"
#include "UiStrings.h"
#include "TelexHelper.h"
#include "../network/JsonHelper.h"
#include "../network/HttpClient.h"
#include <SDL2/SDL_image.h>
#include <mutex>
#include <unordered_set>
#include <algorithm>
#include <sstream>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>

namespace RomCloud {

UIManager& UIManager::instance() {
    static UIManager instance;
    return instance;
}

void UIManager::initGridMenu() {
    m_gridMenuItems = {
        {"games", "THƯ VIỆN GAME", "GAMES.png", "Danh sách ROM"},
        {"iptv", "XEM TV", "TV.png", "Kênh TV online"},
        {"youtube", "YOUTUBE", "YOUTUBE.png", "YouTube"},
        {"sync", "ĐỒNG BỘ", "SYNC.png", "Đồng bộ Google Drive"},
        {"upload", "TẢI LÊN", "UPLOAD.png", "Upload lên Drive"},
        {"ota", "CẬP NHẬT", "OTA.png", "Cập nhật OTA"},
        {"settings", "CÀI ĐẶT", "SETTINGS.png", "Cấu hình"},
        {"info", "THÔNG TIN", "INFO.png", "Thông tin hệ thống"},
        {"exit", "THOÁT", "EXIT.png", "Thoát ứng dụng"}
    };
}

bool UIManager::init(SDL_Window* window, SDL_Renderer* renderer) {
    m_window = window;
    m_renderer = renderer;

    // Initialize grid menu
    initGridMenu();

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

    // Clean up cached textures
    for (auto& pair : m_gridIconCache) {
        if (pair.second) SDL_DestroyTexture(pair.second);
    }
    m_gridIconCache.clear();

    for (auto& pair : m_systemIconCache) {
        if (pair.second) SDL_DestroyTexture(pair.second);
    }
    m_systemIconCache.clear();

    clearTextCache();
    clearThumbnailCache();

    if (m_fontTitle) { TTF_CloseFont(m_fontTitle); m_fontTitle = nullptr; }
    if (m_fontLarge) { TTF_CloseFont(m_fontLarge); m_fontLarge = nullptr; }
    if (m_fontMedium) { TTF_CloseFont(m_fontMedium); m_fontMedium = nullptr; }
    if (m_fontSmall) { TTF_CloseFont(m_fontSmall); m_fontSmall = nullptr; }

    TTF_Quit();
}

void UIManager::setState(UIState state) {
    if ((m_currentState == UIState::YOUTUBE_RESULTS || m_currentState == UIState::YOUTUBE_SEARCH) &&
        (state != UIState::YOUTUBE_RESULTS && state != UIState::YOUTUBE_SEARCH)) {
        clearThumbnailCache();
        system("rm -rf /tmp/yt_thumbs/* 2>/dev/null &");
    }

    m_currentState = state;
    InputManager::instance().reset();
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
    if (DriveSyncEngine::instance().isSyncing() || m_isIndexing) {
        showToast(UiStrings::TOAST_SYNCING_DRIVE, {245, 158, 11, 255});
        return;
    }
    showToast(UiStrings::TOAST_SCANNING_SD, {0, 180, 216, 255}, 2000);

    m_isIndexing = true;
    std::thread([this]() {
        RomIndexer::instance().scanAllSystems(AppConfig::instance().getRomsDir());
        BoxartScraper::instance().startAutoScrapeSdCard(false);
        m_needLibraryRefresh = true;
        m_isIndexing = false;

        if (AuthManager::instance().isLinked()) {
            DriveSyncEngine::instance().startSync();
        }
    }).detach();
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
        showToast("Đồng bộ hoàn tất: Đã lưu " + std::to_string(syncProg.cloudGamesFound) + " game vào thư viện!", {34, 197, 94, 255});
    } else if (syncProg.status == SyncStatus::ERROR_OCCURRED) {
        std::string err = syncProg.errorMessage.empty() ? "Lỗi đồng bộ Google Drive" : syncProg.errorMessage;
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
                for (const auto& item : m_ytSearchResults) {
                    size_t p = item.find('|');
                    if (p != std::string::npos) {
                        vids.push_back(item.substr(0, p));
                    }
                }
                startThumbnailDownloads(vids);
            }
        } else {
            if (m_currentState == UIState::YOUTUBE_SEARCH) {
                std::string msg = m_ytErrorMessage.empty() ? "Không tìm thấy video nào" : m_ytErrorMessage;
                showToast(msg, {245, 158, 11, 255}, 3500);
            }
        }
    }

    // Check if YouTube video stream resolution finished
    if (m_ytVideoReady.exchange(false)) {
        m_ytIsLoadingVideo = false;
        if (!m_ytPendingStreamUrl.empty()) {
            std::string url = m_ytPendingStreamUrl;
            std::string vid = m_ytPendingVideoId;
            m_ytPendingStreamUrl.clear();
            m_ytPendingVideoId.clear();
            IPTVManager::instance().playYouTubeVideo(vid, url, "360");
            setState(UIState::YOUTUBE_RESULTS);
        } else {
            showToast("Không thể lấy link phát video", {239, 68, 68, 255}, 3000);
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

            if (input.isButtonJustPressed(Button::LEFT) || input.isButtonJustPressed(Button::UP)) {
                if (m_selectedMenuIndex > 0) {
                    m_selectedMenuIndex--;
                } else {
                    m_selectedMenuIndex = itemCount - 1;
                }
            } else if (input.isButtonJustPressed(Button::RIGHT) || input.isButtonJustPressed(Button::DOWN)) {
                if (m_selectedMenuIndex < itemCount - 1) {
                    m_selectedMenuIndex++;
                } else {
                    m_selectedMenuIndex = 0;
                }
            } else if (input.isButtonJustPressed(Button::START)) {
                setState(UIState::SETTINGS);
            } else if (input.isButtonJustPressed(Button::SELECT)) {
                triggerManualSync();
            } else if (input.isButtonJustPressed(Button::A)) {
                // Handle menu selection based on id
                std::string selectedId = m_gridMenuItems[m_selectedMenuIndex].id;

                if (selectedId == "games") {
                    setState(UIState::SYSTEM_SELECT);
                } else if (selectedId == "iptv") {
                    if (IPTVManager::instance().getChannels().empty()) {
                        IPTVManager::instance().loadPlaylists();
                    }
                    m_selectedIPTVChannelIndex = 0;
                    m_iptvScrollOffset = 0;
                    setState(UIState::IPTV_LIST);
                } else if (selectedId == "youtube") {
                    m_ytSearchQuery.clear();
                    m_ytSearchResults.clear();
                    m_ytSearchSelectedIndex = 0;
                    m_ytSearchScrollOffset = 0;
                    m_ytKbRow = 0;
                    m_ytKbCol = 0;
                    m_ytKbInResults = false;
                    m_ytErrorMessage.clear();
                    m_ytIsSearching = false;
                    m_ytSearchFinished = false;
                    m_ytIsLoadingVideo = false;
                    m_ytVideoReady = false;
                    m_ytPendingStreamUrl.clear();
                    setState(UIState::YOUTUBE_SEARCH);
                } else if (selectedId == "sync") {
                    triggerManualSync();
                } else if (selectedId == "upload") {
                    if (!AuthManager::instance().isLinked()) {
                        showToast(UiStrings::TOAST_CONNECT_DRIVE_FIRST, {245, 158, 11, 255});
                    } else if (!AuthManager::instance().canUpload()) {
                        showToast(UiStrings::TOAST_CONNECT_PERSONAL_DRIVE, {245, 158, 11, 255}, 4000);
                    } else {
                        UploadManager::instance().startReverseSync();
                        setState(UIState::REVERSE_SYNC);
                    }
                } else if (selectedId == "ota") {
                    setState(UIState::OTA_UPDATE);
                    UpdateManager::instance().checkForUpdatesAsync();
                } else if (selectedId == "settings") {
                    setState(UIState::SETTINGS);
                } else if (selectedId == "info") {
                    setState(UIState::DIAGNOSTICS);
                } else if (selectedId == "exit") {
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

            // Multi-select mode handling
            if (input.isButtonJustPressed(Button::L2)) {
                m_multiSelectMode = !m_multiSelectMode;
                if (!m_multiSelectMode) {
                    m_selectedGameIds.clear();
                    showToast(UiStrings::MULTI_SELECT_DISABLED, {168, 85, 247, 255}, 2000);
                } else {
                    showToast(std::string(UiStrings::MULTI_SELECT_ENABLED) + ". " + UiStrings::MULTI_SELECT_HINT, {168, 85, 247, 255}, 3000);
                }
            }

            if (m_multiSelectMode && total > 0) {
                // Multi-select mode: different controls
                if (input.isButtonJustPressed(Button::UP) || input.isButtonJustPressed(Button::DOWN) ||
                    input.isButtonJustPressed(Button::L1) || input.isButtonJustPressed(Button::R1)) {
                    // Normal navigation while in multi-select mode
                    if (input.isButtonJustPressed(Button::UP)) {
                        m_selectedGameIndex = std::max(0, m_selectedGameIndex - 1);
                    } else if (input.isButtonJustPressed(Button::DOWN)) {
                        m_selectedGameIndex = std::min(total - 1, m_selectedGameIndex + 1);
                    } else if (input.isButtonJustPressed(Button::L1)) {
                        m_selectedGameIndex = std::max(0, m_selectedGameIndex - pageSize);
                    } else if (input.isButtonJustPressed(Button::R1)) {
                        m_selectedGameIndex = std::min(total - 1, m_selectedGameIndex + pageSize);
                    }
                    // Update scroll offset
                    if (m_selectedGameIndex < m_gameScrollOffset) {
                        m_gameScrollOffset = m_selectedGameIndex;
                    } else if (m_selectedGameIndex >= m_gameScrollOffset + pageSize) {
                        m_gameScrollOffset = m_selectedGameIndex - pageSize + 1;
                    }
                } else if (input.isButtonJustPressed(Button::Y)) {
                    // Toggle selection on current game
                    const auto& g = m_cachedGames[m_selectedGameIndex];
                    auto it = std::find(m_selectedGameIds.begin(), m_selectedGameIds.end(), g.id);
                    if (it != m_selectedGameIds.end()) {
                        m_selectedGameIds.erase(it);
                        showToast("Đã bỏ chọn: " + g.title, {245, 158, 11, 255}, 1500);
                    } else {
                        m_selectedGameIds.push_back(g.id);
                        showToast("Đã chọn: " + g.title, {34, 197, 94, 255}, 1500);
                    }
                } else if (input.isButtonJustPressed(Button::X) && !m_selectedGameIds.empty()) {
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
                            for (const auto& g : m_cachedGames) {
                                if (g.id == gameId && g.localState != GameState::LOCAL &&
                                    !DownloadManager::instance().isInQueue(g.id)) {
                                    if (DownloadManager::instance().addToQueue(g, m_activeSystem)) {
                                        addedCount++;
                                    }
                                }
                            }
                        }
                        if (addedCount > 0) {
                            showToast("Đã thêm " + std::to_string(addedCount) + " game vào hàng tải!", {34, 197, 94, 255}, 3000);
                            if (!DownloadManager::instance().isDownloading()) {
                                DownloadManager::instance().processNextInQueue();
                            }
                        } else {
                            showToast("Không có game nào được thêm (đã tải hoặc đang chờ)", {245, 158, 11, 255}, 3000);
                        }
                        refreshGames();
                    }
                } else if (input.isButtonJustPressed(Button::L1) && !m_selectedGameIds.empty()) {
                    // Start upload of selected games to cloud
                    if (!AuthManager::instance().isLinked()) {
                        showToast(UiStrings::TOAST_CONNECT_DRIVE_FIRST, {245, 158, 11, 255});
                    } else if (!AuthManager::instance().canUpload()) {
                        showToast(UiStrings::TOAST_CONNECT_PERSONAL_DRIVE, {245, 158, 11, 255}, 4000);
                    } else {
                        // Gather games to upload (local but not on cloud)
                        std::vector<int64_t> uploadIds;
                        for (int64_t gameId : m_selectedGameIds) {
                            for (const auto& g : m_cachedGames) {
                                if (g.id == gameId && g.localState == GameState::LOCAL && g.cloudFileId.empty()) {
                                    uploadIds.push_back(gameId);
                                    break;
                                }
                            }
                        }
                        if (!uploadIds.empty()) {
                            showToast("Bắt đầu sao lưu " + std::to_string(uploadIds.size()) + " game lên Drive...", {168, 85, 247, 255}, 2000);
                            UploadManager::instance().startUploadGames(uploadIds);
                            m_multiSelectMode = false;
                            m_selectedGameIds.clear();
                            setState(UIState::REVERSE_SYNC);
                        } else {
                            showToast("Không có game nào cần tải lên (đã có trên Cloud)", {245, 158, 11, 255}, 3000);
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

        case UIState::CONFIRM_BATCH_DELETE: {
            if (input.isButtonJustPressed(Button::A)) {
                // Confirm batch delete
                int deletedCount = 0;
                for (int64_t gameId : m_selectedGameIds) {
                    DatabaseManager::instance().markGameDeletedLocally(gameId);
                    deletedCount++;
                }
                refreshSystems();
                refreshGames();
                showToast("Đã xóa " + std::to_string(deletedCount) + " game khỏi thẻ nhớ.", {239, 68, 68, 255}, 4000);
                m_multiSelectMode = false;
                m_selectedGameIds.clear();
                setState(UIState::GAME_LIST);
            } else if (input.isButtonJustPressed(Button::B) || input.isButtonJustPressed(Button::X)) {
                setState(UIState::GAME_LIST);
            }
            break;
        }

        case UIState::SEARCH: {
            static const char* kbRows[] = {
                "1234567890",
                "QWERTYUIOP",
                "ASDFGHJKL-",
                "ZXCVBNM<_*"  // '<' = DEL, '_' = SPACE, '*' = OK
            };
            static const int kbRowCount = 4;
            static const int kbColCount = 10;

            if (!m_kbInResults) {
                // Navigate keyboard
                if (input.isButtonJustPressed(Button::UP)) {
                    if (m_kbCursorRow > 0) {
                        m_kbCursorRow--;
                    }
                } else if (input.isButtonJustPressed(Button::DOWN)) {
                    if (m_kbCursorRow < kbRowCount - 1) {
                        m_kbCursorRow++;
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
                    else m_kbCursorCol = kbColCount - 1;
                } else if (input.isButtonJustPressed(Button::RIGHT)) {
                    if (m_kbCursorCol < kbColCount - 1) m_kbCursorCol++;
                    else m_kbCursorCol = 0;
                } else if (input.isButtonJustPressed(Button::A)) {
                    char ch = kbRows[m_kbCursorRow][m_kbCursorCol];
                    if (ch == '<') { // Backspace
                        if (!m_searchQuery.empty()) m_searchQuery.pop_back();
                    } else if (ch == '*') { // OK
                        if (!m_searchResults.empty()) {
                            m_kbInResults = true;
                            m_searchSelectedIndex = 0;
                            m_searchScrollOffset = 0;
                        }
                    } else if (ch == '_') {
                        if (m_searchQuery.size() < 30) m_searchQuery += ' ';
                    } else {
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
            constexpr int totalSettingsRows = 11;
            constexpr int visibleRows = 9;

            if (input.isButtonJustPressed(Button::UP)) {
                if (m_selectedSettingsRow > 0) {
                    m_selectedSettingsRow--;
                    if (m_selectedSettingsRow < m_settingsScrollOffset) {
                        m_settingsScrollOffset = m_selectedSettingsRow;
                    }
                } else {
                    m_selectedSettingsRow = totalSettingsRows - 1;
                    m_settingsScrollOffset = std::max(0, totalSettingsRows - visibleRows);
                }
            } else if (input.isButtonJustPressed(Button::DOWN)) {
                if (m_selectedSettingsRow < totalSettingsRows - 1) {
                    m_selectedSettingsRow++;
                    if (m_selectedSettingsRow >= m_settingsScrollOffset + visibleRows) {
                        m_settingsScrollOffset = m_selectedSettingsRow - visibleRows + 1;
                    }
                } else {
                    m_selectedSettingsRow = 0;
                    m_settingsScrollOffset = 0;
                }
            } else if (input.isButtonJustPressed(Button::L1)) {
                m_selectedSettingsRow = std::max(0, m_selectedSettingsRow - 4);
                m_settingsScrollOffset = std::max(0, m_settingsScrollOffset - 4);
            } else if (input.isButtonJustPressed(Button::R1)) {
                m_selectedSettingsRow = std::min(totalSettingsRows - 1, m_selectedSettingsRow + 4);
                if (m_selectedSettingsRow >= m_settingsScrollOffset + visibleRows) {
                    m_settingsScrollOffset = std::min(std::max(0, totalSettingsRows - visibleRows), m_selectedSettingsRow - visibleRows + 1);
                }
            }

            if (input.isButtonJustPressed(Button::A)) {
                if (m_selectedSettingsRow == 0) {
                    // Google Drive account row
                    if (!AuthManager::instance().isLinked()) {
                        setState(UIState::DISCLAIMER);
                    }
                } else if (m_selectedSettingsRow == 2) {
                    // OS Type selection - cycle through options
                    OSType current = AppConfig::instance().getOSType();
                    OSType next;
                    switch (current) {
                        case OSType::AUTO:      next = OSType::STOCK_PS; break;
                        case OSType::STOCK_PS:  next = OSType::NEXTUI; break;
                        case OSType::NEXTUI:   next = OSType::SPRUCE_OS; break;
                        case OSType::SPRUCE_OS: next = OSType::AUTO; break;
                        default:               next = OSType::AUTO; break;
                    }
                    AppConfig::instance().setOSType(next);
                    showToast("OS: " + AppConfig::instance().getOSName(), {168, 85, 247, 255}, 2000);
                } else if (m_selectedSettingsRow == 9) {
                    // Export backup
                    showToast(UiStrings::BACKUP_EXPORTING, {168, 85, 247, 255}, 2000);
                    auto result = BackupManager::instance().exportToSdCard();
                    if (result.success) {
                        showToast(UiStrings::BACKUP_SUCCESS, {34, 197, 94, 255}, 4000);
                    } else {
                        showToast(UiStrings::BACKUP_FAILED, {239, 68, 68, 255}, 4000);
                    }
                } else if (m_selectedSettingsRow == 10) {
                    // Import backup
                    showToast(UiStrings::BACKUP_IMPORTING, {0, 180, 216, 255}, 2000);
                    auto lastBackup = BackupManager::instance().getMostRecentBackup();
                    if (lastBackup.empty()) {
                        showToast(UiStrings::BACKUP_NO_FILE, {245, 158, 11, 255}, 4000);
                    } else {
                        auto result = BackupManager::instance().importFromFile(lastBackup);
                        if (result.success) {
                            showToast(UiStrings::BACKUP_RESTORE_SUCCESS, {34, 197, 94, 255}, 4000);
                        } else {
                            showToast(UiStrings::BACKUP_RESTORE_FAILED, {239, 68, 68, 255}, 4000);
                        }
                    }
                }
            } else if (input.isButtonJustPressed(Button::X)) {
                if (m_selectedSettingsRow == 0 && AuthManager::instance().isLinked()) {
                    AuthManager::instance().logout();
                    refreshSystems();
                    refreshGames();
                    showToast(UiStrings::TOAST_LOGOUT_SUCCESS, {245, 158, 11, 255});
                }
            } else if (input.isButtonJustPressed(Button::B)) {
                setState(UIState::MENU);
                m_selectedSettingsRow = 0;
                m_settingsScrollOffset = 0;
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
            // Scroll navigation
            if (input.isButtonJustPressed(Button::UP)) {
                m_diagnosticsScrollOffset = std::max(0, m_diagnosticsScrollOffset - 1);
            } else if (input.isButtonJustPressed(Button::DOWN)) {
                m_diagnosticsScrollOffset++;
            } else if (input.isButtonJustPressed(Button::L1)) {
                m_diagnosticsScrollOffset = std::max(0, m_diagnosticsScrollOffset - 5);
            } else if (input.isButtonJustPressed(Button::R1)) {
                m_diagnosticsScrollOffset += 5;
            } else if (input.isButtonJustPressed(Button::B)) {
                setState(UIState::MENU);
                m_diagnosticsScrollOffset = 0;
            }
            break;
        }

        case UIState::REVERSE_SYNC: {
            auto prog = UploadManager::instance().getProgress();
            if (prog.state == UploadState::IDLE || prog.state == UploadState::PREPARING) {
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
            } else if (prog.state == UploadState::COMPLETED || prog.state == UploadState::FAILED || prog.state == UploadState::CANCELLED) {
                if (input.isButtonJustPressed(Button::A) || input.isButtonJustPressed(Button::B)) {
                    setState(UIState::MENU);
                }
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
            } else if (prog.state == UpdateState::DOWNLOADING || prog.state == UpdateState::DOWNLOADING_DEPS || prog.state == UpdateState::VERIFYING) {
                if (input.isButtonJustPressed(Button::B)) {
                    UpdateManager::instance().cancelUpdate();
                    showToast(UiStrings::TOAST_OTA_CANCELLED, {245, 158, 11, 255});
                }
            } else if (prog.state == UpdateState::INSTALLING || prog.state == UpdateState::INSTALLING_DEPS) {
                // Prevent cancelling while extracting or installing to avoid corruption
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

        case UIState::IPTV_LIST: {
            std::vector<IPTVChannel> channels;
            if (m_iptvShowFavoritesOnly) {
                channels = IPTVManager::instance().getFavoriteChannels();
            } else {
                channels = IPTVManager::instance().getChannels();
            }
            int channelCount = static_cast<int>(channels.size());
            int visibleItems = 10;

            if (input.isButtonJustPressed(Button::UP)) {
                if (channelCount > 0) {
                    m_selectedIPTVChannelIndex = std::max(0, m_selectedIPTVChannelIndex - 1);
                    if (m_selectedIPTVChannelIndex < m_iptvScrollOffset) {
                        m_iptvScrollOffset = m_selectedIPTVChannelIndex;
                    }
                }
            } else if (input.isButtonJustPressed(Button::DOWN)) {
                if (channelCount > 0) {
                    m_selectedIPTVChannelIndex = std::min(channelCount - 1, m_selectedIPTVChannelIndex + 1);
                    if (m_selectedIPTVChannelIndex >= m_iptvScrollOffset + visibleItems) {
                        m_iptvScrollOffset = m_selectedIPTVChannelIndex - visibleItems + 1;
                    }
                }
            } else if (input.isButtonJustPressed(Button::L1)) {
                if (channelCount > 0) {
                    m_selectedIPTVChannelIndex = std::max(0, m_selectedIPTVChannelIndex - visibleItems);
                    m_iptvScrollOffset = std::max(0, m_iptvScrollOffset - visibleItems);
                    if (m_selectedIPTVChannelIndex < m_iptvScrollOffset) {
                        m_iptvScrollOffset = m_selectedIPTVChannelIndex;
                    }
                }
            } else if (input.isButtonJustPressed(Button::R1)) {
                if (channelCount > 0) {
                    m_selectedIPTVChannelIndex = std::min(channelCount - 1, m_selectedIPTVChannelIndex + visibleItems);
                    if (m_selectedIPTVChannelIndex >= m_iptvScrollOffset + visibleItems) {
                        m_iptvScrollOffset = std::min(std::max(0, channelCount - visibleItems), m_iptvScrollOffset + visibleItems);
                    }
                }
            } else if (input.isButtonJustPressed(Button::A)) {
                if (channelCount > 0 && m_selectedIPTVChannelIndex >= 0 && m_selectedIPTVChannelIndex < channelCount) {
                    showToast("Đang kết nối: " + channels[m_selectedIPTVChannelIndex].name + "...", {0, 180, 216, 255}, 3000);
                    render();
                    if (!IPTVManager::instance().playChannel(channels[m_selectedIPTVChannelIndex])) {
                        showToast("Không thể phát video (Lỗi kết nối hoặc player)", {239, 68, 68, 255}, 4000);
                    }
                }
            } else if (input.isButtonJustPressed(Button::B)) {
                if (m_iptvShowFavoritesOnly) {
                    m_iptvShowFavoritesOnly = false;
                    m_selectedIPTVChannelIndex = 0;
                    m_iptvScrollOffset = 0;
                    showToast("Đang hiển thị tất cả kênh", {0, 180, 216, 255}, 1500);
                } else {
                    IPTVManager::instance().stop();
                    setState(UIState::MENU);
                }
            } else if (input.isButtonJustPressed(Button::X)) {
                // Toggle favorite on selected channel
                if (channelCount > 0 && m_selectedIPTVChannelIndex >= 0 && m_selectedIPTVChannelIndex < channelCount) {
                    std::string chanName = channels[m_selectedIPTVChannelIndex].name;
                    bool wasFav = IPTVManager::instance().isFavorite(chanName);
                    IPTVManager::instance().toggleFavorite(chanName);
                    if (!wasFav) {
                        showToast("★ Đã thêm vào yêu thích: " + chanName, {250, 204, 21, 255}, 2000);
                    } else {
                        showToast("☆ Đã xóa khỏi yêu thích: " + chanName, {148, 163, 184, 255}, 2000);
                        if (m_iptvShowFavoritesOnly) {
                            channels = IPTVManager::instance().getFavoriteChannels();
                            channelCount = static_cast<int>(channels.size());
                            if (m_selectedIPTVChannelIndex >= channelCount) {
                                m_selectedIPTVChannelIndex = std::max(0, channelCount - 1);
                            }
                            if (m_selectedIPTVChannelIndex < m_iptvScrollOffset) {
                                m_iptvScrollOffset = m_selectedIPTVChannelIndex;
                            }
                        }
                    }
                }
            } else if (input.isButtonJustPressed(Button::Y)) {
                // Toggle Favorites filter mode
                m_iptvShowFavoritesOnly = !m_iptvShowFavoritesOnly;
                m_selectedIPTVChannelIndex = 0;
                m_iptvScrollOffset = 0;
                showToast(m_iptvShowFavoritesOnly ? "★ Đang lọc: Kênh Yêu Thích" : "Đang lọc: Tất cả kênh", {0, 180, 216, 255}, 2000);
            } else if (input.isButtonJustPressed(Button::SELECT) || input.isButtonJustPressed(Button::START)) {
                // Open IPTV QWERTY Search
                m_iptvSearchQuery.clear();
                m_iptvSearchResults.clear();
                m_iptvKbRow = 0;
                m_iptvKbCol = 0;
                m_iptvKbInResults = false;
                m_iptvSearchSelectedIndex = 0;
                m_iptvSearchScrollOffset = 0;
                setState(UIState::IPTV_SEARCH);
            }
            break;
        }

        case UIState::IPTV_SEARCH: {
            static const char* qwertyRows[] = {
                "1234567890",
                "QWERTYUIOP",
                "ASDFGHJKL-",
                "ZXCVBNM<_*"  // '<' = DEL, '_' = SPACE, '*' = OK
            };
            static const int kbRowCount = 4;
            static const int kbColCount = 10;

            if (!m_iptvKbInResults) {
                if (input.isButtonJustPressed(Button::UP)) {
                    if (m_iptvKbRow > 0) {
                        m_iptvKbRow--;
                    }
                } else if (input.isButtonJustPressed(Button::DOWN)) {
                    if (m_iptvKbRow < kbRowCount - 1) {
                        m_iptvKbRow++;
                    } else if (!m_iptvSearchResults.empty()) {
                        m_iptvKbInResults = true;
                        m_iptvSearchSelectedIndex = 0;
                        m_iptvSearchScrollOffset = 0;
                    }
                } else if (input.isButtonJustPressed(Button::LEFT)) {
                    if (m_iptvKbCol > 0) {
                        m_iptvKbCol--;
                    } else {
                        m_iptvKbCol = kbColCount - 1;
                    }
                } else if (input.isButtonJustPressed(Button::RIGHT)) {
                    if (m_iptvKbCol < kbColCount - 1) {
                        m_iptvKbCol++;
                    } else {
                        m_iptvKbCol = 0;
                    }
                } else if (input.isButtonJustPressed(Button::A)) {
                    char ch = qwertyRows[m_iptvKbRow][m_iptvKbCol];
                    if (ch == '<') {
                        if (!m_iptvSearchQuery.empty()) {
                            m_iptvSearchQuery.pop_back();
                        }
                    } else if (ch == '_') {
                        if (m_iptvSearchQuery.length() < 30) {
                            m_iptvSearchQuery += ' ';
                        }
                    } else if (ch == '*') {
                        if (!m_iptvSearchResults.empty()) {
                            m_iptvKbInResults = true;
                            m_iptvSearchSelectedIndex = 0;
                            m_iptvSearchScrollOffset = 0;
                        }
                    } else {
                        if (m_iptvSearchQuery.length() < 30) {
                            m_iptvSearchQuery += ch;
                        }
                    }
                    if (!m_iptvSearchQuery.empty()) {
                        m_iptvSearchResults = IPTVManager::instance().search(m_iptvSearchQuery);
                    } else {
                        m_iptvSearchResults.clear();
                    }
                    m_iptvSearchSelectedIndex = 0;
                    m_iptvSearchScrollOffset = 0;
                } else if (input.isButtonJustPressed(Button::X)) {
                    m_iptvSearchQuery.clear();
                    m_iptvSearchResults.clear();
                    m_iptvSearchSelectedIndex = 0;
                    m_iptvSearchScrollOffset = 0;
                } else if (input.isButtonJustPressed(Button::START)) {
                    if (!m_iptvSearchResults.empty()) {
                        m_iptvKbInResults = true;
                        m_iptvSearchSelectedIndex = 0;
                        m_iptvSearchScrollOffset = 0;
                    }
                } else if (input.isButtonJustPressed(Button::B)) {
                    setState(UIState::IPTV_LIST);
                }
            } else {
                int resultCount = static_cast<int>(m_iptvSearchResults.size());
                int visibleItems = 10;

                if (input.isButtonJustPressed(Button::UP)) {
                    if (m_iptvSearchSelectedIndex > 0) {
                        m_iptvSearchSelectedIndex--;
                        if (m_iptvSearchSelectedIndex < m_iptvSearchScrollOffset) {
                            m_iptvSearchScrollOffset = m_iptvSearchSelectedIndex;
                        }
                    } else {
                        m_iptvKbInResults = false;
                    }
                } else if (input.isButtonJustPressed(Button::DOWN)) {
                    if (m_iptvSearchSelectedIndex < resultCount - 1) {
                        m_iptvSearchSelectedIndex++;
                        if (m_iptvSearchSelectedIndex >= m_iptvSearchScrollOffset + visibleItems) {
                            m_iptvSearchScrollOffset = m_iptvSearchSelectedIndex - visibleItems + 1;
                        }
                    }
                } else if (input.isButtonJustPressed(Button::L1)) {
                    if (resultCount > 0) {
                        m_iptvSearchSelectedIndex = std::max(0, m_iptvSearchSelectedIndex - visibleItems);
                        m_iptvSearchScrollOffset = std::max(0, m_iptvSearchScrollOffset - visibleItems);
                        if (m_iptvSearchSelectedIndex < m_iptvSearchScrollOffset) {
                            m_iptvSearchScrollOffset = m_iptvSearchSelectedIndex;
                        }
                    }
                } else if (input.isButtonJustPressed(Button::R1)) {
                    if (resultCount > 0) {
                        m_iptvSearchSelectedIndex = std::min(resultCount - 1, m_iptvSearchSelectedIndex + visibleItems);
                        if (m_iptvSearchSelectedIndex >= m_iptvSearchScrollOffset + visibleItems) {
                            m_iptvSearchScrollOffset = std::min(std::max(0, resultCount - visibleItems), m_iptvSearchScrollOffset + visibleItems);
                        }
                    }
                } else if (input.isButtonJustPressed(Button::LEFT)) {
                    m_iptvKbInResults = false;
                } else if (input.isButtonJustPressed(Button::A)) {
                    if (resultCount > 0 && m_iptvSearchSelectedIndex >= 0 && m_iptvSearchSelectedIndex < resultCount) {
                        const auto& selChan = m_iptvSearchResults[m_iptvSearchSelectedIndex];
                        showToast("Đang kết nối: " + selChan.name + "...", {0, 180, 216, 255}, 3000);
                        render();
                        if (!IPTVManager::instance().playChannel(selChan)) {
                            showToast("Không thể phát video (Lỗi kết nối hoặc player)", {239, 68, 68, 255}, 4000);
                        }
                    }
                } else if (input.isButtonJustPressed(Button::X)) {
                    if (resultCount > 0 && m_iptvSearchSelectedIndex >= 0 && m_iptvSearchSelectedIndex < resultCount) {
                        std::string chanName = m_iptvSearchResults[m_iptvSearchSelectedIndex].name;
                        bool wasFav = IPTVManager::instance().isFavorite(chanName);
                        IPTVManager::instance().toggleFavorite(chanName);
                        m_iptvSearchResults[m_iptvSearchSelectedIndex].isFavorite = !wasFav;
                        if (!wasFav) {
                            showToast("★ Đã thêm vào yêu thích: " + chanName, {250, 204, 21, 255}, 2000);
                        } else {
                            showToast("☆ Đã xóa khỏi yêu thích: " + chanName, {148, 163, 184, 255}, 2000);
                        }
                    }
                } else if (input.isButtonJustPressed(Button::B)) {
                    m_iptvKbInResults = false;
                }
            }
            break;
        }

        case UIState::YOUTUBE_SEARCH: {
            if (m_ytIsSearching) {
                break; // Ignore input while searching
            }

            static const char* lowerRows[] = {
                "1234567890",
                "qwertyuiop",
                "asdfghjkl-",
                "zxcvbnm()/"
            };
            static const char* upperRows[] = {
                "1234567890",
                "QWERTYUIOP",
                "ASDFGHJKL-",
                "ZXCVBNM()/"
            };

            if (input.isButtonJustPressed(Button::UP)) {
                if (m_ytKbRow > 0) m_ytKbRow--;
            } else if (input.isButtonJustPressed(Button::DOWN)) {
                if (m_ytKbRow < 4) m_ytKbRow++;
            } else if (input.isButtonJustPressed(Button::LEFT)) {
                if (m_ytKbRow == 4) {
                    int actionIdx = m_ytKbCol / 2;
                    if (actionIdx > 0) actionIdx--;
                    else actionIdx = 4;
                    m_ytKbCol = actionIdx * 2;
                } else {
                    if (m_ytKbCol > 0) m_ytKbCol--;
                    else m_ytKbCol = 9;
                }
            } else if (input.isButtonJustPressed(Button::RIGHT)) {
                if (m_ytKbRow == 4) {
                    int actionIdx = m_ytKbCol / 2;
                    if (actionIdx < 4) actionIdx++;
                    else actionIdx = 0;
                    m_ytKbCol = actionIdx * 2;
                } else {
                    if (m_ytKbCol < 9) m_ytKbCol++;
                    else m_ytKbCol = 0;
                }
            } else if (input.isButtonJustPressed(Button::A)) {
                if (m_ytKbRow < 4) {
                    char ch = m_ytKbShift ? upperRows[m_ytKbRow][m_ytKbCol] : lowerRows[m_ytKbRow][m_ytKbCol];
                    if (m_ytSearchQuery.length() < 60) {
                        if (m_ytTelexMode) {
                            m_ytSearchQuery = TelexHelper::processTelex(m_ytSearchQuery, ch);
                        } else {
                            m_ytSearchQuery += ch;
                        }
                    }
                } else {
                    int actionIdx = m_ytKbCol / 2;
                    if (actionIdx == 0) {
                        m_ytKbShift = !m_ytKbShift;
                    } else if (actionIdx == 1) {
                        m_ytTelexMode = !m_ytTelexMode;
                        showToast(m_ytTelexMode ? "Chế độ: TELEX" : "Chế độ: TIẾNG ANH (US)", {0, 200, 83, 255}, 1200);
                    } else if (actionIdx == 2) {
                        if (m_ytSearchQuery.length() < 60) m_ytSearchQuery += ' ';
                    } else if (actionIdx == 3) {
                        TelexHelper::popUtf8(m_ytSearchQuery);
                    } else if (actionIdx == 4) {
                        if (!m_ytSearchQuery.empty()) {
                            triggerYouTubeSearch();
                        } else if (!m_ytSearchResults.empty()) {
                            setState(UIState::YOUTUBE_RESULTS);
                        }
                    }
                }
            } else if (input.isButtonJustPressed(Button::L1)) {
                m_ytKbShift = !m_ytKbShift;
            } else if (input.isButtonJustPressed(Button::R1)) {
                m_ytTelexMode = !m_ytTelexMode;
                showToast(m_ytTelexMode ? "Chế độ: TELEX" : "Chế độ: TIẾNG ANH (US)", {0, 200, 83, 255}, 1200);
            } else if (input.isButtonJustPressed(Button::X)) {
                if (m_ytSearchQuery.length() < 60) m_ytSearchQuery += ' ';
            } else if (input.isButtonJustPressed(Button::Y)) {
                TelexHelper::popUtf8(m_ytSearchQuery);
            } else if (input.isButtonJustPressed(Button::START)) {
                if (!m_ytSearchQuery.empty()) {
                    triggerYouTubeSearch();
                } else if (!m_ytSearchResults.empty()) {
                    setState(UIState::YOUTUBE_RESULTS);
                }
            } else if (input.isButtonJustPressed(Button::B)) {
                if (!m_ytSearchQuery.empty()) {
                    TelexHelper::popUtf8(m_ytSearchQuery);
                } else if (!m_ytSearchResults.empty()) {
                    setState(UIState::YOUTUBE_RESULTS);
                } else {
                    setState(UIState::MENU);
                }
            } else if (input.isButtonJustPressed(Button::SELECT)) {
                setState(UIState::MENU);
            }
            break;
        }

        case UIState::YOUTUBE_RESULTS: {
            if (m_ytIsLoadingVideo) {
                break; // Ignore input while resolving video stream
            }

            int resultCount = static_cast<int>(m_ytSearchResults.size());
            if (resultCount == 0) {
                setState(UIState::YOUTUBE_SEARCH);
                break;
            }

            const int gridCols = 3;
            const int pageSize = 6;

            if (input.isButtonJustPressed(Button::LEFT)) {
                if (m_ytSearchSelectedIndex % gridCols > 0) {
                    m_ytSearchSelectedIndex--;
                }
            } else if (input.isButtonJustPressed(Button::RIGHT)) {
                if (m_ytSearchSelectedIndex % gridCols < gridCols - 1 &&
                    m_ytSearchSelectedIndex + 1 < resultCount) {
                    m_ytSearchSelectedIndex++;
                }
            } else if (input.isButtonJustPressed(Button::UP)) {
                if (m_ytSearchSelectedIndex >= gridCols) {
                    m_ytSearchSelectedIndex -= gridCols;
                }
            } else if (input.isButtonJustPressed(Button::DOWN)) {
                if (m_ytSearchSelectedIndex + gridCols < resultCount) {
                    m_ytSearchSelectedIndex += gridCols;
                } else if (m_ytSearchSelectedIndex + 1 < resultCount && (m_ytSearchSelectedIndex % gridCols == 0)) {
                    m_ytSearchSelectedIndex = resultCount - 1;
                }
            } else if (input.isButtonJustPressed(Button::X)) {
                setState(UIState::YOUTUBE_SEARCH);
            } else if (input.isButtonJustPressed(Button::L1)) {
                if (m_ytCurrentPage > 1) {
                    m_ytCurrentPage--;
                    int startIdx = (m_ytCurrentPage - 1) * 6;
                    int endIdx = std::min<int>(startIdx + 6, static_cast<int>(m_ytAllCachedResults.size()));
                    if (startIdx < endIdx) {
                        m_ytSearchResults.assign(m_ytAllCachedResults.begin() + startIdx, m_ytAllCachedResults.begin() + endIdx);
                        m_ytSearchSelectedIndex = 0;
                        m_ytSearchScrollOffset = 0;
                        std::vector<std::string> vids;
                        for (const auto& item : m_ytSearchResults) {
                            size_t p = item.find('|');
                            if (p != std::string::npos) vids.push_back(item.substr(0, p));
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
                    int endIdx = std::min<int>(startIdx + 6, static_cast<int>(m_ytAllCachedResults.size()));
                    m_ytSearchResults.assign(m_ytAllCachedResults.begin() + startIdx, m_ytAllCachedResults.begin() + endIdx);
                    m_ytSearchSelectedIndex = 0;
                    m_ytSearchScrollOffset = 0;
                    std::vector<std::string> vids;
                    for (const auto& item : m_ytSearchResults) {
                        size_t p = item.find('|');
                        if (p != std::string::npos) vids.push_back(item.substr(0, p));
                    }
                    startThumbnailDownloads(vids);
                } else if (!m_ytIsSearching) {
                    // Fetch next page dynamically via script
                    m_ytIsSearching = true;
                    showToast("Đang tải trang tiếp theo...", {0, 180, 216, 255}, 1500);
                    std::string query = m_ytLastSearchQuery;
                    std::thread([this, query, targetPage, startIdx]() {
                        std::string appRoot = AppConfig::instance().getAppRoot();
                        if (appRoot.empty()) appRoot = "/mnt/SDCARD/Apps/RomCloud";
                        std::string scriptPath = appRoot + "/scripts/youtube_search.sh";
                        std::string escapedQuery;
                        for (char c : query) {
                            if (c == '"' || c == '\\') escapedQuery += '\\';
                            escapedQuery += c;
                        }
                        std::string cmd = "\"" + scriptPath + "\" search \"" + escapedQuery + "\" " + std::to_string(targetPage) + " 6 2>/dev/null";
                        FILE* pipe = popen(cmd.c_str(), "r");
                        std::vector<std::string> moreResults;
                        if (pipe) {
                            char buffer[2048];
                            while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                                std::string line(buffer);
                                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                                if (line.find("ERROR:") == 0 || line.find("WARNING:") == 0) continue;
                                if (std::count(line.begin(), line.end(), '|') >= 4) {
                                    moreResults.push_back(line);
                                }
                            }
                            pclose(pipe);
                        }
                        if (!moreResults.empty()) {
                            m_ytAllCachedResults.insert(m_ytAllCachedResults.end(), moreResults.begin(), moreResults.end());
                            m_ytCurrentPage = targetPage;
                            int endIdx = std::min<int>(startIdx + 6, static_cast<int>(m_ytAllCachedResults.size()));
                            m_ytSearchResults.assign(m_ytAllCachedResults.begin() + startIdx, m_ytAllCachedResults.begin() + endIdx);
                            m_ytSearchSelectedIndex = 0;
                            m_ytSearchScrollOffset = 0;
                            std::vector<std::string> vids;
                            for (const auto& item : m_ytSearchResults) {
                                size_t p = item.find('|');
                                if (p != std::string::npos) vids.push_back(item.substr(0, p));
                            }
                            startThumbnailDownloads(vids);
                        } else {
                            showToast("Đã đến trang cuối", {245, 158, 11, 255}, 2000);
                        }
                        m_ytIsSearching = false;
                    }).detach();
                }
            } else if (input.isButtonJustPressed(Button::A)) {
                if (m_ytSearchSelectedIndex >= 0 && m_ytSearchSelectedIndex < resultCount) {
                    std::string selected = m_ytSearchResults[m_ytSearchSelectedIndex];
                    std::string videoId = selected.substr(0, selected.find('|'));
                    playYouTubeVideo(videoId);
                }
            } else if (input.isButtonJustPressed(Button::B) || input.isButtonJustPressed(Button::START)) {
                m_ytIsSearching = false;
                setState(UIState::YOUTUBE_SEARCH);
            } else if (input.isButtonJustPressed(Button::SELECT)) {
                m_ytIsSearching = false;
                setState(UIState::MENU);
            }

            m_ytSearchScrollOffset = (m_ytSearchSelectedIndex / pageSize) * pageSize;
            break;
        }

        default: break;
    }
}

void UIManager::clearTextCache() {
    for (auto& pair : m_textCache) {
        if (pair.second.texture) {
            SDL_DestroyTexture(pair.second.texture);
        }
    }
    m_textCache.clear();
}

void UIManager::drawText(const std::string& text, int x, int y, SDL_Color color, TTF_Font* font, bool centered) {
    if (!font || text.empty()) return;

    // Cache key combining font pointer, color (packed 32-bit), and text content
    char keyBuf[128];
    uint32_t colorInt = (color.r << 24) | (color.g << 16) | (color.b << 8) | color.a;
    std::snprintf(keyBuf, sizeof(keyBuf), "%p_%08x_", (void*)font, colorInt);
    std::string key = std::string(keyBuf) + text;

    SDL_Texture* texture = nullptr;
    int texW = 0, texH = 0;

    auto it = m_textCache.find(key);
    if (it != m_textCache.end()) {
        texture = it->second.texture;
        texW = it->second.w;
        texH = it->second.h;
        it->second.lastUsed = SDL_GetTicks();
    } else {
        SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
        if (!surface) return;
        texture = SDL_CreateTextureFromSurface(m_renderer, surface);
        texW = surface->w;
        texH = surface->h;
        SDL_FreeSurface(surface);
        if (!texture) return;

        // Keep cache bounded to max 256 items (~2MB RAM)
        if (m_textCache.size() >= 256) {
            auto oldest = m_textCache.begin();
            for (auto iter = m_textCache.begin(); iter != m_textCache.end(); ++iter) {
                if (iter->second.lastUsed < oldest->second.lastUsed) {
                    oldest = iter;
                }
            }
            if (oldest->second.texture) {
                SDL_DestroyTexture(oldest->second.texture);
            }
            m_textCache.erase(oldest);
        }

        m_textCache[key] = {texture, texW, texH, SDL_GetTicks()};
    }

    int sx = PlatformInfo::instance().scaleX(x);
    int sy = PlatformInfo::instance().scaleY(y);
    int drawX = centered ? (sx - texW / 2) : sx;
    int drawY = sy;
    SDL_Rect dstRect = {drawX, drawY, texW, texH};
    SDL_RenderCopy(m_renderer, texture, nullptr, &dstRect);
}

void UIManager::drawRect(int x, int y, int w, int h, SDL_Color color, bool filled) {
    // Scale coordinates and dimensions
    int sx = PlatformInfo::instance().scaleX(x);
    int sy = PlatformInfo::instance().scaleY(y);
    int sw = PlatformInfo::instance().scaleW(w);
    int sh = PlatformInfo::instance().scaleH(h);

    SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
    SDL_Rect rect = {sx, sy, sw, sh};
    if (filled) {
        SDL_RenderFillRect(m_renderer, &rect);
    } else {
        SDL_RenderDrawRect(m_renderer, &rect);
    }
}

void UIManager::drawBorder(int x, int y, int w, int h, SDL_Color color, int thickness) {
    // Scale coordinates, dimensions, and thickness
    int sx = PlatformInfo::instance().scaleX(x);
    int sy = PlatformInfo::instance().scaleY(y);
    int sw = PlatformInfo::instance().scaleW(w);
    int sh = PlatformInfo::instance().scaleH(h);
    int st = PlatformInfo::instance().scaleW(thickness);

    SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
    for (int i = 0; i < st; ++i) {
        SDL_Rect rect = {sx + i, sy + i, sw - 2 * i, sh - 2 * i};
        SDL_RenderDrawRect(m_renderer, &rect);
    }
}

void UIManager::drawRoundedRect(int x, int y, int w, int h, int radius, SDL_Color color, bool filled) {
    // Scale coordinates, dimensions, and radius
    int sx = PlatformInfo::instance().scaleX(x);
    int sy = PlatformInfo::instance().scaleY(y);
    int sw = PlatformInfo::instance().scaleW(w);
    int sh = PlatformInfo::instance().scaleH(h);
    int sr = PlatformInfo::instance().scaleW(radius);

    if (sw <= 0 || sh <= 0) return;
    int maxR = std::min(sw, sh) / 2;
    if (sr > maxR) sr = maxR;
    if (sr <= 0) {
        drawRect(sx, sy, sw, sh, color, filled);
        return;
    }

    if (filled) {
        SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
        SDL_Rect centerRect = {sx, sy + sr, sw, sh - 2 * sr};
        if (centerRect.h > 0) {
            SDL_RenderFillRect(m_renderer, &centerRect);
        }

        for (int dy = 0; dy < sr; ++dy) {
            int ry = sr - 1 - dy;
            int dx = static_cast<int>(std::sqrt(sr * sr - ry * ry));
            int lineW = sw - 2 * (sr - dx);
            int lineX = sx + sr - dx;

            if (lineW > 0) {
                SDL_Rect topSlice = {lineX, sy + dy, lineW, 1};
                SDL_RenderFillRect(m_renderer, &topSlice);
                SDL_Rect btmSlice = {lineX, sy + sh - 1 - dy, lineW, 1};
                SDL_RenderFillRect(m_renderer, &btmSlice);
            }
        }
    } else {
        drawRoundedBorder(sx, sy, sw, sh, sr, color, 1);
    }
}

void UIManager::drawRoundedBorder(int x, int y, int w, int h, int radius, SDL_Color color, int thickness) {
    // Scale coordinates, dimensions, radius, and thickness
    int sx = PlatformInfo::instance().scaleX(x);
    int sy = PlatformInfo::instance().scaleY(y);
    int sw = PlatformInfo::instance().scaleW(w);
    int sh = PlatformInfo::instance().scaleH(h);
    int sr = PlatformInfo::instance().scaleW(radius);
    int st = PlatformInfo::instance().scaleW(thickness);

    if (sw <= 0 || sh <= 0) return;
    int maxR = std::min(sw, sh) / 2;
    if (sr > maxR) sr = maxR;
    if (sr <= 0) {
        drawBorder(sx, sy, sw, sh, color, st);
        return;
    }

    SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);

    // Straight bars
    SDL_Rect topBar = {sx + sr, sy, sw - 2 * sr, st};
    SDL_Rect btmBar = {sx + sr, sy + sh - st, sw - 2 * sr, st};
    SDL_RenderFillRect(m_renderer, &topBar);
    SDL_RenderFillRect(m_renderer, &btmBar);

    SDL_Rect leftBar = {sx, sy + sr, st, sh - 2 * sr};
    SDL_Rect rightBar = {sx + sw - st, sy + sr, st, sh - 2 * sr};
    SDL_RenderFillRect(m_renderer, &leftBar);
    SDL_RenderFillRect(m_renderer, &rightBar);

    // Corner arcs using 1-px high fill rects
    for (int dy = 0; dy < sr; ++dy) {
        int ry = sr - 1 - dy;
        int outerDx = static_cast<int>(std::sqrt(sr * sr - ry * ry));
        int innerR = std::max(0, sr - st);
        int innerDx = (ry < innerR) ? static_cast<int>(std::sqrt(innerR * innerR - ry * ry)) : 0;
        int segW = std::max(st, outerDx - innerDx);

        SDL_Rect tl = {sx + sr - outerDx, sy + dy, segW, 1};
        SDL_RenderFillRect(m_renderer, &tl);

        SDL_Rect tr = {sx + sw - sr + outerDx - segW, sy + dy, segW, 1};
        SDL_RenderFillRect(m_renderer, &tr);

        SDL_Rect bl = {sx + sr - outerDx, sy + sh - 1 - dy, segW, 1};
        SDL_RenderFillRect(m_renderer, &bl);

        SDL_Rect br = {sx + sw - sr + outerDx - segW, sy + sh - 1 - dy, segW, 1};
        SDL_RenderFillRect(m_renderer, &br);
    }
}

void UIManager::drawBadge(int x, int y, int w, int h, const std::string& text, SDL_Color bg, SDL_Color fg) {
    int rad = std::min(8, h / 2);
    drawRoundedRect(x, y, w, h, rad, bg, true);
    drawText(text, x + w / 2, y + (h - 16) / 2, fg, m_fontSmall, true);
}

void UIManager::drawIcon(const std::string& iconName, int x, int y, int w, int h) {
    SDL_Texture* texture = nullptr;
    auto it = m_systemIconCache.find(iconName);
    if (it != m_systemIconCache.end()) {
        texture = it->second;
    } else {
        std::string iconsDir = AppConfig::instance().getAssetsDir() + "/icons";
        std::string iconPath = iconsDir + "/" + iconName + ".png";
        SDL_Surface* surface = IMG_Load(iconPath.c_str());
        if (surface) {
            texture = SDL_CreateTextureFromSurface(m_renderer, surface);
            SDL_FreeSurface(surface);
            m_systemIconCache[iconName] = texture;
        }
    }

    if (!texture) {
        // Fallback: draw a colored rectangle if icon not found
        drawRect(x, y, w, h, {60, 70, 85, 255}, true);
        return;
    }

    int sx = PlatformInfo::instance().scaleX(x);
    int sy = PlatformInfo::instance().scaleY(y);
    int sw = PlatformInfo::instance().scaleW(w);
    int sh = PlatformInfo::instance().scaleH(h);

    int texW = 0, texH = 0;
    SDL_QueryTexture(texture, nullptr, nullptr, &texW, &texH);
    int drawW = sw;
    int drawH = sh;
    if (texW > 0 && texH > 0) {
        float aspect = static_cast<float>(texW) / static_cast<float>(texH);
        if (aspect >= 1.0f) {
            drawW = std::min(sw, static_cast<int>(sh * aspect));
            drawH = static_cast<int>(drawW / aspect);
        } else {
            drawH = std::min(sh, static_cast<int>(sw / aspect));
            drawW = static_cast<int>(drawH * aspect);
        }
    }

    int dstX = sx + (sw - drawW) / 2;
    int dstY = sy + (sh - drawH) / 2;
    SDL_Rect dst = {dstX, dstY, drawW, drawH};
    SDL_RenderCopy(m_renderer, texture, nullptr, &dst);
}

// Draw gamepad button icon (A, B, X, Y, L1, R1, etc.)
void UIManager::drawButtonIcon(const std::string& button, int x, int y, int size) {
    SDL_Color bgColor = {60, 60, 60, 255};
    SDL_Color textColor = {255, 255, 255, 255};
    std::string label = button;

    // Set colors based on button type (Xbox-style)
    if (button == "A") {
        bgColor = {16, 150, 60, 255};    // Green
        label = "A";
    } else if (button == "B") {
        bgColor = {200, 30, 30, 255};   // Red
        label = "B";
    } else if (button == "X") {
        bgColor = {30, 90, 200, 255};  // Blue
        label = "X";
    } else if (button == "Y") {
        bgColor = {200, 180, 20, 255};  // Yellow
        label = "Y";
    } else if (button == "L1") {
        bgColor = {100, 100, 100, 255};
        label = "L1";
    } else if (button == "R1") {
        bgColor = {100, 100, 100, 255};
        label = "R1";
    } else if (button == "L2") {
        bgColor = {80, 80, 80, 255};
        label = "L2";
    } else if (button == "R2") {
        bgColor = {80, 80, 80, 255};
        label = "R2";
    } else if (button == "SELECT" || button == "BACK") {
        bgColor = {80, 80, 80, 255};
        label = "SEL";
    } else if (button == "START" || button == "MENU") {
        bgColor = {80, 80, 80, 255};
        label = "STA";
    } else if (button == "HOME") {
        bgColor = {100, 100, 100, 255};
        label = "HOME";
    }

    // Draw rounded rectangle for button
    int rad = size / 4;
    drawRoundedRect(x, y, size, size, rad, bgColor, true);

    // Draw button label (letter)
    int fontSize = size * 3 / 5;
    TTF_Font* font = m_fontSmall;
    if (fontSize > 20) font = m_fontMedium;

    int textW = label.length() * fontSize * 2 / 3;
    int textX = x + (size - textW) / 2;
    int textY = y + (size - fontSize) / 2;
    drawText(label, textX, textY, textColor, font);
}

void UIManager::drawGridIcon(const std::string &iconFile, int x, int y, int w, int h) {
    SDL_Texture* texture = nullptr;
    auto it = m_gridIconCache.find(iconFile);
    if (it != m_gridIconCache.end()) {
        texture = it->second;
    } else {
        std::string iconsDir = AppConfig::instance().getAssetsDir() + "/apps_icons";
        std::string iconPath = iconsDir + "/" + iconFile;
        SDL_Surface* surface = IMG_Load(iconPath.c_str());
        if (surface) {
            texture = SDL_CreateTextureFromSurface(m_renderer, surface);
            SDL_FreeSurface(surface);
            m_gridIconCache[iconFile] = texture;
        }
    }

    if (!texture) {
        drawRoundedRect(x + 10, y + 10, w - 20, h - 20, 8, {60, 70, 85, 255}, true);
        return;
    }

    // Scale destination coordinates and bounds
    int sx = PlatformInfo::instance().scaleX(x);
    int sy = PlatformInfo::instance().scaleY(y);
    int sw = PlatformInfo::instance().scaleW(w);
    int sh = PlatformInfo::instance().scaleH(h);

    // Preserve exact 1:1 square aspect ratio of icons, centered in the slot
    int texW = 0, texH = 0;
    SDL_QueryTexture(texture, nullptr, nullptr, &texW, &texH);

    int maxW = sw;
    int maxH = sh;
    int drawW = maxW;
    int drawH = maxH;

    if (texW > 0 && texH > 0) {
        float aspect = static_cast<float>(texW) / static_cast<float>(texH);
        if (aspect >= 1.0f) {
            drawW = std::min(maxW, static_cast<int>(maxH * aspect));
            drawH = static_cast<int>(drawW / aspect);
        } else {
            drawH = std::min(maxH, static_cast<int>(maxW / aspect));
            drawW = static_cast<int>(drawH * aspect);
        }
    } else {
        int side = std::min(maxW, maxH);
        drawW = side;
        drawH = side;
    }

    int dstX = sx + (sw - drawW) / 2;
    int dstY = sy + (sh - drawH) / 2;

    SDL_Rect dst = {dstX, dstY, drawW, drawH};
    SDL_RenderCopy(m_renderer, texture, nullptr, &dst);
}

void UIManager::renderHeader() {
    // Header removed to maximize full-screen view for all screens
}

void UIManager::renderSearchState() {
    static const char* kbRows[] = {
        "1234567890",
        "QWERTYUIOP",
        "ASDFGHJKL-",
        "ZXCVBNM<_*"  // '<' = DEL, '_' = SPACE, '*' = OK
    };
    static const int kbRowCount = 4;
    static const int kbColCount = 10;

    // ─── Left panel: keyboard + query bar (Borderless) ───
    int panelW = 430;
    int panelX = 24;
    int panelY = 74;

    // Query bar with rounded corners
    drawRoundedRect(panelX, panelY, panelW, 52, 10, {22, 32, 46, 255}, true);
    drawRoundedBorder(panelX, panelY, panelW, 52, 10, {0, 180, 216, 255}, 2);
    std::string displayQuery = m_searchQuery.empty() ? UiStrings::SEARCH_PROMPT_INPUT : m_searchQuery + "_";
    SDL_Color qColor = m_searchQuery.empty() ? SDL_Color{80, 95, 115, 255} : SDL_Color{255, 255, 255, 255};
    drawText(displayQuery, panelX + 16, panelY + 14, qColor, m_fontMedium);

    // Keyboard with rounded keycaps
    int kbStartY = panelY + 68;
    int cellW = 37;
    int cellH = 46;
    int gap = 4;
    int kbPadX = 12;

    for (int row = 0; row < kbRowCount; row++) {
        for (int col = 0; col < kbColCount; col++) {
            int cx = panelX + kbPadX + col * (cellW + gap);
            int cy = kbStartY + row * (cellH + 8);
            bool isSel = (!m_kbInResults && m_kbCursorRow == row && m_kbCursorCol == col);

            char ch = kbRows[row][col];
            std::string label;
            if (ch == '<') label = "DEL";
            else if (ch == '_') label = "SPC";
            else if (ch == '*') label = "OK";
            else label = std::string(1, ch);

            SDL_Color bg = isSel ? SDL_Color{0, 180, 216, 255} : SDL_Color{28, 38, 55, 255};
            SDL_Color fg = isSel ? SDL_Color{0, 0, 0, 255} : SDL_Color{220, 230, 240, 255};
            drawRoundedRect(cx, cy, cellW, cellH, 6, bg, true);
            drawRoundedBorder(cx, cy, cellW, cellH, 6, isSel ? SDL_Color{255, 255, 255, 255} : SDL_Color{45, 60, 80, 255}, 1);
            drawText(label, cx + cellW / 2, cy + cellH / 2 - 10, fg, m_fontSmall, true);
        }
    }

    // ─── Divider line ───
    drawRect(476, 64, 1, 651, {38, 48, 64, 255}, true);

    // ─── Right panel: results (Borderless) ───
    int rPanelX = 496;
    int rPanelW = 1024 - rPanelX - 24;
    int rPanelY = panelY;
    int rPanelH = 630;

    int numResults = static_cast<int>(m_searchResults.size());
    if (m_searchQuery.length() < 2) {
        drawText(UiStrings::SEARCH_PROMPT_MIN_CHARS, rPanelX + rPanelW / 2, rPanelY + 60, {80, 95, 115, 255}, m_fontSmall, true);
    } else if (numResults == 0) {
        drawText(UiStrings::SEARCH_NO_RESULTS, rPanelX + rPanelW / 2, rPanelY + 60, {239, 68, 68, 255}, m_fontSmall, true);
    } else {
        std::string countStr = std::to_string(numResults) + UiStrings::SEARCH_RESULTS_SUFFIX;
        drawText(countStr, rPanelX + rPanelW / 2, rPanelY + 8, {100, 115, 135, 255}, m_fontSmall, true);

        int pageSize = 10;
        int itemH = 58;
        int listStartY = rPanelY + 36;

        for (int i = 0; i < pageSize && (m_searchScrollOffset + i) < numResults; i++) {
            int idx = m_searchScrollOffset + i;
            const auto& g = m_searchResults[idx];
            bool isSel = (m_kbInResults && idx == m_searchSelectedIndex);

            int itemY = listStartY + i * itemH;
            SDL_Color rowBg = isSel ? SDL_Color{2, 55, 82, 255} : SDL_Color{20, 28, 42, 255};
            drawRoundedRect(rPanelX, itemY, rPanelW, itemH - 4, 8, rowBg, true);
            if (isSel) {
                drawRoundedBorder(rPanelX, itemY, rPanelW, itemH - 4, 8, {0, 180, 216, 255}, 2);
            }

            // State badge (pill)
            bool isLocal = (g.localState == GameState::LOCAL);
            SDL_Color badgeBg = isLocal ? SDL_Color{22, 78, 99, 255} : SDL_Color{45, 30, 72, 255};
            SDL_Color badgeFg = isLocal ? SDL_Color{34, 197, 94, 255} : SDL_Color{168, 85, 247, 255};
            std::string stateLabel = isLocal ? UiStrings::SEARCH_BADGE_LOCAL : UiStrings::SEARCH_BADGE_CLOUD;
            drawBadge(rPanelX + 12, itemY + 12, 78, 28, stateLabel, badgeBg, badgeFg);

            // Title
            std::string title = g.title;
            if (title.length() > 34) title = title.substr(0, 33) + "...";
            drawText(title, rPanelX + 102, itemY + 8, {230, 240, 255, 255}, m_fontMedium);

            // System label
            drawText(g.systemCode, rPanelX + 102, itemY + 34, {100, 115, 135, 255}, m_fontSmall);
        }

        // Scroll indicator
        if (numResults > pageSize) {
            float scrollFrac = static_cast<float>(m_searchScrollOffset) / (numResults - pageSize);
            int scrollBarH = rPanelH - 46;
            int thumbH = std::max(24, scrollBarH / (numResults / pageSize + 1));
            int thumbY = rPanelY + 38 + static_cast<int>(scrollFrac * (scrollBarH - thumbH));
            drawRoundedRect(rPanelX + rPanelW - 6, rPanelY + 38, 4, scrollBarH, 2, {30, 42, 58, 255}, true);
            drawRoundedRect(rPanelX + rPanelW - 6, thumbY, 4, thumbH, 2, {0, 180, 216, 255}, true);
        }
    }

    // Hint: which panel is active
    std::string hint = m_kbInResults ? UiStrings::SEARCH_NAV_UP_HINT : UiStrings::SEARCH_NAV_DOWN_HINT;
    drawText(hint, 512, 705, {60, 75, 95, 255}, m_fontSmall, true);
}

void UIManager::renderFooter() {
    if (m_currentState == UIState::IPTV_LIST || m_currentState == UIState::IPTV_SEARCH ||
        m_currentState == UIState::YOUTUBE_SEARCH || m_currentState == UIState::YOUTUBE_RESULTS) {
        return;
    }

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
        int toastW = 620;
        int toastH = 48;
        int toastX = (1024 - toastW) / 2;
        int toastY = 645;

        drawRoundedRect(toastX, toastY, toastW, toastH, 24, {20, 24, 32, 245}, true);
        drawRoundedBorder(toastX, toastY, toastW, toastH, 24, m_toastColor, 2);
        drawText(m_toastMessage, 512, toastY + 14, {255, 255, 255, 255}, m_fontSmall, true);
    }
}

void UIManager::renderMenuState() {
    // OTA update info
    bool hasUpdate = UpdateManager::instance().isUpdateAvailable();

    int itemCount = static_cast<int>(m_gridMenuItems.size());

    // Single Horizontal Row (1 hàng ngang) Carousel - Centered vertically without header
    int selW = 260;
    int selH = 320;
    int selX = 512 - selW / 2; // 382
    int selY = 155;

    int normW = 200;
    int normH = 260;
    int normY = 185;
    int gap = 24;

    // Render items in a single horizontal row centered around m_selectedMenuIndex
    for (int i = 0; i < itemCount; ++i) {
        int offset = i - m_selectedMenuIndex;
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        bool isSel = (offset == 0);

        if (isSel) {
            x = selX;
            y = selY;
            w = selW;
            h = selH;
        } else if (offset > 0) {
            x = selX + selW + gap + (offset - 1) * (normW + gap);
            y = normY;
            w = normW;
            h = normH;
        } else { // offset < 0
            x = selX - gap - normW + (offset + 1) * (normW + gap);
            y = normY;
            w = normW;
            h = normH;
        }

        // Clip items outside visible screen
        if (x + w < -50 || x > 1024 + 50) {
            continue;
        }

        // Card background
        SDL_Color bg = isSel ? SDL_Color{26, 52, 88, 255} : SDL_Color{20, 26, 36, 230};
        drawRoundedRect(x, y, w, h, 16, bg, true);

        if (isSel) {
            // Glowing cyan border on selected card
            drawRoundedBorder(x, y, w, h, 16, {0, 180, 216, 255}, 3);
        } else {
            drawRoundedBorder(x, y, w, h, 16, {38, 48, 64, 255}, 1);
        }

        // Icon inside card (enlarged logo size)
        int iconSize = isSel ? 160 : 120;
        int iconX = x + (w - iconSize) / 2;
        int iconY = y + (isSel ? 30 : 25);
        drawGridIcon(m_gridMenuItems[i].iconFile, iconX, iconY, iconSize, iconSize);

        // Card Title (Logo + Tên chức năng duy nhất)
        int textY = y + (isSel ? 235 : 195);
        SDL_Color titleColor = isSel ? SDL_Color{255, 255, 255, 255} : SDL_Color{160, 175, 195, 255};
        drawText(m_gridMenuItems[i].title, x + w / 2, textY, titleColor, isSel ? m_fontMedium : m_fontSmall, true);

        // OTA badge
        if (hasUpdate && m_gridMenuItems[i].id == "ota") {
            drawRoundedRect(x + w - 54, y + 10, 44, 22, 6, {239, 68, 68, 255}, true);
            drawText("NEW", x + w - 32, y + 13, {255, 255, 255, 255}, m_fontSmall, true);
        }
    }

    // Left and Right navigation chevrons
    if (m_selectedMenuIndex > 0) {
        drawText("<", 36, 335, {0, 180, 216, 200}, m_fontLarge, true);
    }
    if (m_selectedMenuIndex < itemCount - 1) {
        drawText(">", 988, 335, {0, 180, 216, 200}, m_fontLarge, true);
    }

    // Dot pager centered at Y=560
    int dotW = 8;
    int selDotW = 28;
    int dotGap = 8;
    int totalDotWidth = selDotW + (itemCount - 1) * (dotW + dotGap);
    int dotX = (1024 - totalDotWidth) / 2;
    int dotY = 560;

    for (int i = 0; i < itemCount; ++i) {
        bool isSel = (i == m_selectedMenuIndex);
        int w = isSel ? selDotW : dotW;
        drawRoundedRect(dotX, dotY, w, 8, 4, isSel ? SDL_Color{0, 180, 216, 255} : SDL_Color{45, 56, 75, 255}, true);
        dotX += w + dotGap;
    }

    // Footer hint
    drawRect(0, 715, 1024, 53, {18, 22, 30, 255}, true);
    drawRect(0, 715, 1024, 1, {40, 48, 62, 255}, true);
    drawText("[A] Chọn    [D-Pad] Chuyển mục    [START] Cài đặt    [SELECT] Đồng bộ", 512, 730, {150, 165, 185, 255}, m_fontSmall, true);
}

void UIManager::renderSystemSelectState() {
    // ─── Borderless Full-Width Sub-Header ───
    drawRect(0, 64, 1024, 48, {16, 20, 28, 255}, true);
    drawRect(0, 111, 1024, 1, {38, 48, 64, 255}, true);

    int totalLocal = 0, totalCloud = 0;
    DatabaseManager::instance().getTotalGameCounts(totalLocal, totalCloud);

    std::string summary = std::string(UiStrings::SYSTEM_SELECT_TITLE) + " (" + std::to_string(m_cachedSystems.size()) + UiStrings::SYS_SELECT_SYSTEMS_LABEL +
                          std::to_string(totalLocal) + UiStrings::SYS_SELECT_GAMES_LOCAL +
                          std::to_string(totalCloud) + UiStrings::SYS_SELECT_GAMES_CLOUD;
    drawText(summary, 24, 78, {0, 180, 216, 255}, m_fontLarge);

    int visibleCount = 6;
    int startIdx = 0;
    if (m_selectedSystemIndex >= visibleCount) {
        startIdx = m_selectedSystemIndex - visibleCount + 1;
    }

    int rowY = 122;
    int rowH = 88;
    int rowW = 976;
    int rx = 24;

    for (int i = startIdx; i < static_cast<int>(m_cachedSystems.size()) && (i - startIdx) < visibleCount; ++i) {
        const auto& sys = m_cachedSystems[i];
        bool selected = (i == m_selectedSystemIndex);
        int y = rowY + (i - startIdx) * (rowH + 10);

        SDL_Color bg = selected ? SDL_Color{30, 58, 95, 255} : SDL_Color{22, 28, 38, 255};
        drawRoundedRect(rx, y, rowW, rowH, 10, bg, true);

        if (selected) {
            drawRoundedBorder(rx, y, rowW, rowH, 10, {0, 180, 216, 255}, 2);
            // Left neon accent
            drawRoundedRect(rx + 6, y + 12, 5, rowH - 24, 2, {0, 180, 216, 255}, true);
        }

        // System icon
        int iconSize = 64;
        drawIcon(sys.code, rx + 16, y + (rowH - iconSize) / 2, iconSize, iconSize);

        drawText(sys.name, rx + 96, y + 18, {255, 255, 255, 255}, m_fontLarge);

        std::string localBadge = std::to_string(sys.localCount) + " local";
        std::string cloudBadge = std::to_string(sys.cloudCount) + " cloud";

        drawBadge(rx + rowW - 200, y + 24, 90, 40, localBadge, {22, 101, 52, 255}, {255, 255, 255, 255});
        drawBadge(rx + rowW - 100, y + 24, 90, 40, cloudBadge, {30, 58, 138, 255}, {255, 255, 255, 255});

        std::string subtext = std::string("/Roms/") + sys.romDir + "  •  " + sys.extList;
        drawText(subtext, rx + 110, y + 54, {140, 155, 175, 255}, m_fontSmall);
    }
}

void UIManager::renderGameListState() {
    int listX = 0;
    int listY = 64;
    int listW = 590;
    int listH = 651;

    int detailX = 592;
    int detailY = 64;
    int detailW = 432;
    int detailH = 651;

    // Subtle 1px vertical divider between panes
    drawRect(591, 64, 1, 651, {38, 48, 64, 255}, true);

    // 1. Render Left Games List
    int totalGames = static_cast<int>(m_cachedGames.size());
    int pageSize = 6;
    int rowH = 92;
    int rowW = 560;
    int rowX = 16;
    int listStartY = listY + 12;

    if (totalGames == 0) {
        drawText(UiStrings::GAME_LIST_EMPTY, listX + listW / 2, listY + 280, {140, 150, 165, 255}, m_fontLarge, true);
        drawText(UiStrings::GAME_FILTER_HINT, listX + listW / 2, listY + 325, {0, 180, 216, 255}, m_fontSmall, true);
    } else {
        for (int i = 0; i < pageSize && (m_gameScrollOffset + i) < totalGames; ++i) {
            int gameIdx = m_gameScrollOffset + i;
            const auto& game = m_cachedGames[gameIdx];
            bool selected = (gameIdx == m_selectedGameIndex);
            int y = listStartY + i * (rowH + 12);

            SDL_Color bg = selected ? SDL_Color{30, 58, 95, 255} : SDL_Color{22, 28, 38, 255};
            drawRoundedRect(rowX, y, rowW, rowH, 10, bg, true);

            if (selected) {
                drawRoundedBorder(rowX, y, rowW, rowH, 10, {0, 180, 216, 255}, 2);
                // Left neon accent
                drawRoundedRect(rowX + 5, y + 12, 5, rowH - 24, 2, {0, 180, 216, 255}, true);
            }

            // State Pill Badge
            bool isThisDownloading = DownloadManager::instance().isDownloading() &&
                                     DownloadManager::instance().getProgress().gameId == game.id;
            auto dlp = DownloadManager::instance().getProgress();

            if (game.localState == GameState::LOCAL) {
                drawBadge(rowX + 18, y + 27, 86, 38, UiStrings::BADGE_DOWNLOADED, {22, 101, 52, 255}, {255, 255, 255, 255});
            } else if (isThisDownloading) {
                char pctBuf[16];
                std::snprintf(pctBuf, sizeof(pctBuf), "%.0f%%", dlp.progressPct);
                drawBadge(rowX + 18, y + 27, 86, 38, std::string(pctBuf), {2, 132, 199, 255}, {255, 255, 255, 255});
            } else if (DownloadManager::instance().isInQueue(game.id)) {
                auto q = DownloadManager::instance().getQueue();
                int pos = 1;
                for (const auto& qi : q) {
                    if (qi.game.id == game.id) break;
                    pos++;
                }
                drawBadge(rowX + 18, y + 27, 86, 38, "#" + std::to_string(pos), {107, 33, 168, 255}, {255, 255, 255, 255});
            } else if (game.localState == GameState::CLOUD) {
                drawBadge(rowX + 18, y + 27, 86, 38, "CLOUD", {30, 58, 138, 255}, {255, 255, 255, 255});
            } else {
                drawBadge(rowX + 18, y + 27, 86, 38, UiStrings::BTN_SYNC, {217, 119, 6, 255}, {255, 255, 255, 255});
            }

            // Title allowed up to 34 chars
            std::string title = game.title;
            if (title.length() > 34) {
                title = title.substr(0, 31) + "...";
            }
            SDL_Color titleCol = isThisDownloading ? SDL_Color{0, 180, 216, 255} : (selected ? SDL_Color{255, 255, 255, 255} : SDL_Color{210, 220, 230, 255});

            if (isThisDownloading) {
                drawText(title, rowX + 118, y + 14, titleCol, m_fontLarge);

                char pctBuf[16];
                std::snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", dlp.progressPct);
                std::string dlSub = FileSystemManager::instance().formatBytes(dlp.bytesDownloaded) + " / " +
                                    FileSystemManager::instance().formatBytes(dlp.totalBytes) + "  (" + pctBuf + ")";
                drawText(dlSub, rowX + 118, y + 44, {140, 205, 245, 255}, m_fontSmall);

                // Live in-row rounded progress bar
                int pBarX = rowX + 118;
                int pBarY = y + 70;
                int pBarW = rowW - 135;
                int pBarH = 6;
                drawRoundedRect(pBarX, pBarY, pBarW, pBarH, 3, {35, 45, 60, 255}, true);
                float pct = std::max(0.0, std::min(100.0, dlp.progressPct));
                drawRoundedRect(pBarX, pBarY, (int)(pBarW * (pct / 100.0)), pBarH, 3, {34, 197, 94, 255}, true);
            } else {
                drawText(title, rowX + 118, y + 18, titleCol, m_fontLarge);

                std::string sizeStr = FileSystemManager::instance().formatBytes(game.sizeBytes);
                std::string sub = game.filename + "  (" + sizeStr + ")";
                if (sub.length() > 38) {
                    sub = sub.substr(0, 35) + "... (" + sizeStr + ")";
                }
                drawText(sub, rowX + 118, y + 52, {140, 155, 175, 255}, m_fontSmall);
            }

            // Multi-select checkbox
            if (m_multiSelectMode) {
                auto it = std::find(m_selectedGameIds.begin(), m_selectedGameIds.end(), game.id);
                bool isSelected = (it != m_selectedGameIds.end());

                // Checkbox background
                SDL_Color cbBg = isSelected ? SDL_Color{34, 197, 94, 255} : SDL_Color{50, 60, 75, 255};
                drawRoundedRect(rowX + rowW - 40, y + 35, 24, 24, 4, cbBg, true);

                // Checkmark
                if (isSelected) {
                    drawText("✓", rowX + rowW - 40 + 3, y + 33, {255, 255, 255, 255}, m_fontMedium);
                }
            }
        }

        // Multi-select mode header badge
        if (m_multiSelectMode && !m_selectedGameIds.empty()) {
            int badgeW = 180;
            int badgeH = 36;
            int badgeX = 512 - badgeW / 2;
            int badgeY = 8;
            std::string countText = std::to_string(m_selectedGameIds.size()) + " đã chọn";
            drawBadge(badgeX, badgeY, badgeW, badgeH, countText, {107, 33, 168, 255}, {255, 255, 255, 255});
        }

        // Low storage warning banner
        auto dlProg = DownloadManager::instance().getProgress();
        if (dlProg.storageWarning) {
            float freePct = (dlProg.storageTotal > 0) ?
                (float)dlProg.storageAvailable * 100.0f / dlProg.storageTotal : 0;
            int warnW = 400;
            int warnH = 36;
            int warnX = (1024 - warnW) / 2;
            int warnY = 720;
            std::string warnText = "⚠ Cảnh báo: Thẻ nhớ sắp đầy (" + std::to_string((int)freePct) + "% trống)";
            drawBadge(warnX, warnY, warnW, warnH, warnText, {185, 28, 28, 255}, {255, 255, 255, 255});
        }

        // Scrollbar
        if (totalGames > pageSize) {
            int barX = 582;
            int barTrackH = listH - 24;
            drawRoundedRect(barX, listY + 12, 4, barTrackH, 2, {35, 42, 54, 255}, true);

            float ratio = (float)pageSize / (float)totalGames;
            int thumbH = std::max(24, (int)(barTrackH * ratio));
            float scrollRatio = (float)m_gameScrollOffset / (float)(totalGames - pageSize);
            int thumbY = listY + 12 + (int)((barTrackH - thumbH) * scrollRatio);
            drawRoundedRect(barX, thumbY, 4, thumbH, 2, {0, 180, 216, 255}, true);
        }
    }

    // 2. Render Right Details & Cover Panel (Borderless)
    const GameRecord* selGame = (totalGames > 0 && m_selectedGameIndex < totalGames) ? &m_cachedGames[m_selectedGameIndex] : nullptr;

    // Cover Art Box
    int coverBoxW = 360;
    int coverBoxH = 290;
    int coverBoxX = detailX + (detailW - coverBoxW) / 2;
    int coverBoxY = detailY + 14;

    // Rounded backdrop for cover art container
    drawRoundedRect(coverBoxX - 4, coverBoxY - 4, coverBoxW + 8, coverBoxH + 8, 12, {16, 20, 28, 255}, true);
    drawRoundedBorder(coverBoxX - 4, coverBoxY - 4, coverBoxW + 8, coverBoxH + 8, 12, {38, 48, 64, 255}, 1);

    CoverManager::instance().renderCoverBox(coverBoxX, coverBoxY, coverBoxW, coverBoxH, selGame, &m_activeSystem, m_fontMedium);

    // Detail Metadata
    if (selGame) {
        int metaY = coverBoxY + coverBoxH + 18;

        std::string title = selGame->title;
        if (title.length() > 26) title = title.substr(0, 23) + "...";
        drawText(title, detailX + 32, metaY, {255, 255, 255, 255}, m_fontLarge);

        metaY += 36;
        drawText(UiStrings::DETAIL_SYS_LABEL, detailX + 32, metaY, {140, 155, 175, 255}, m_fontSmall);
        drawText(m_activeSystem.name + " (" + m_activeSystem.code + ")", detailX + 135, metaY, {0, 180, 216, 255}, m_fontSmall);

        metaY += 28;
        drawText(UiStrings::DETAIL_SIZE_LABEL, detailX + 32, metaY, {140, 155, 175, 255}, m_fontSmall);
        drawText(FileSystemManager::instance().formatBytes(selGame->sizeBytes), detailX + 135, metaY, {255, 255, 255, 255}, m_fontSmall);

        metaY += 28;
        drawText(UiStrings::DETAIL_LOCATION_LABEL, detailX + 32, metaY, {140, 155, 175, 255}, m_fontSmall);
        if (selGame->localState == GameState::LOCAL) {
            drawText(std::string(UiStrings::GAME_LOCATION_SD_PREFIX) + m_activeSystem.romDir + ")", detailX + 135, metaY, {34, 197, 94, 255}, m_fontSmall);
        } else {
            drawText(UiStrings::GAME_LOCATION_DRIVE, detailX + 135, metaY, {0, 180, 216, 255}, m_fontSmall);
        }

        // Action Status Pill & Live Progress
        metaY += 42;
        if (selGame->localState == GameState::LOCAL) {
            int halfW = (detailW - 72) / 2;
            drawBadge(detailX + 32, metaY, halfW, 46, UiStrings::BADGE_DOWNLOADED, {22, 101, 52, 255}, {255, 255, 255, 255});
            drawBadge(detailX + 40 + halfW, metaY, halfW, 46, UiStrings::BADGE_DELETE_BTN, {185, 28, 28, 255}, {255, 255, 255, 255});
        } else if (DownloadManager::instance().isDownloading() &&
                   DownloadManager::instance().getProgress().gameId == selGame->id) {
            auto dlp = DownloadManager::instance().getProgress();
            char pctBuf[32];
            std::snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", dlp.progressPct);
            std::string dlInfo = std::string(UiStrings::GAME_DOWNLOADING_PREFIX) + std::string(pctBuf);
            drawBadge(detailX + 32, metaY, detailW - 64, 40, dlInfo, {2, 132, 199, 255}, {255, 255, 255, 255});

            int dBarX = detailX + 32;
            int dBarY = metaY + 46;
            int dBarW = detailW - 64;
            int dBarH = 10;
            drawRoundedRect(dBarX, dBarY, dBarW, dBarH, 5, {35, 45, 60, 255}, true);
            float pct = std::max(0.0, std::min(100.0, dlp.progressPct));
            drawRoundedRect(dBarX, dBarY, (int)(dBarW * (pct / 100.0)), dBarH, 5, {34, 197, 94, 255}, true);

            std::string dSizeStr = FileSystemManager::instance().formatBytes(dlp.bytesDownloaded) +
                                   " / " + FileSystemManager::instance().formatBytes(dlp.totalBytes);
            drawText(dSizeStr, detailX + detailW / 2, dBarY + 16, {200, 220, 240, 255}, m_fontSmall, true);

            drawBadge(detailX + 32, dBarY + 38, detailW - 64, 36, UiStrings::BADGE_CANCEL_DL_BTN, {185, 28, 28, 255}, {255, 255, 255, 255});
            metaY += 56;
        } else if (DownloadManager::instance().isInQueue(selGame->id)) {
            drawBadge(detailX + 32, metaY, detailW - 64, 46, UiStrings::BADGE_REMOVE_QUEUE_BTN, {107, 33, 168, 255}, {255, 255, 255, 255});
        } else {
            drawBadge(detailX + 32, metaY, detailW - 64, 46, UiStrings::BADGE_ADD_QUEUE_BTN, {2, 132, 199, 255}, {255, 255, 255, 255});
        }

        // Queue info panel below action pill
        int queueCount = DownloadManager::instance().queueSize();
        bool isCurrentlyDownloading = DownloadManager::instance().isDownloading();
        if (queueCount > 0 || isCurrentlyDownloading) {
            metaY += 54;
            std::string qInfo;
            if (isCurrentlyDownloading && queueCount > 0) {
                qInfo = std::string(UiStrings::GAME_QUEUE_DOWNLOADING) + std::to_string(queueCount) + UiStrings::GAME_QUEUE_REMAINING;
            } else if (isCurrentlyDownloading) {
                qInfo = UiStrings::QUEUE_DOWNLOADING_EMPTY;
            } else {
                qInfo = std::string(UiStrings::GAME_QUEUE_WAITING) + std::to_string(queueCount) + UiStrings::GAME_QUEUE_REMAINING;
            }
            drawText(qInfo, detailX + 32, metaY, {168, 85, 247, 255}, m_fontSmall);
        }
    }

    // Active download indicator banner at bottom right if user is browsing another game
    if (DownloadManager::instance().isDownloading() &&
        (!selGame || DownloadManager::instance().getProgress().gameId != selGame->id)) {
        auto activeProg = DownloadManager::instance().getProgress();
        int bY = detailY + detailH - 74;
        drawRoundedRect(detailX + 24, bY, detailW - 48, 62, 10, {18, 28, 44, 255}, true);
        drawRoundedBorder(detailX + 24, bY, detailW - 48, 62, 10, {0, 180, 216, 255}, 1);

        char pBuf[16];
        std::snprintf(pBuf, sizeof(pBuf), "%.0f%%", activeProg.progressPct);
        std::string tTrunc = activeProg.gameTitle;
        if (tTrunc.length() > 18) tTrunc = tTrunc.substr(0, 15) + "...";
        drawText("v " + tTrunc, detailX + 36, bY + 8, {0, 180, 216, 255}, m_fontSmall);
        drawText(std::string(pBuf), detailX + detailW - 75, bY + 8, {34, 197, 94, 255}, m_fontSmall);

        int aBarW = detailW - 72;
        int aBarH = 6;
        drawRoundedRect(detailX + 36, bY + 38, aBarW, aBarH, 3, {30, 40, 55, 255}, true);
        float aPct = std::max(0.0, std::min(100.0, activeProg.progressPct));
        drawRoundedRect(detailX + 36, bY + 38, (int)(aBarW * (aPct / 100.0)), aBarH, 3, {34, 197, 94, 255}, true);
    }
}

void UIManager::renderConfirmDeleteDialog() {
    // Dim background overlay
    drawRect(0, 0, 1024, 768, {0, 0, 0, 190}, true);

    int dlgW = 640;
    int dlgH = 340;
    int dlgX = (1024 - dlgW) / 2;
    int dlgY = (768 - dlgH) / 2;

    // Borderless dialog - just rounded background
    drawRoundedRect(dlgX, dlgY, dlgW, dlgH, 16, {24, 28, 38, 255}, true);

    // Simple title with background color bar (no border)
    drawRect(dlgX, dlgY, dlgW, 48, {185, 28, 28, 255}, true);
    drawText(UiStrings::DIALOG_DELETE_TITLE, dlgX + dlgW / 2, dlgY + 14, {255, 255, 255, 255}, m_fontLarge, true);

    if (m_selectedGameIndex >= 0 && m_selectedGameIndex < static_cast<int>(m_cachedGames.size())) {
        const auto& game = m_cachedGames[m_selectedGameIndex];

        drawText(game.title, dlgX + dlgW / 2, dlgY + 85, {255, 255, 255, 255}, m_fontMedium, true);
        std::string sizeStr = std::string(UiStrings::DIALOG_DELETE_FILE_PREFIX) + game.filename + " (" + FileSystemManager::instance().formatBytes(game.sizeBytes) + ")";
        drawText(sizeStr, dlgX + dlgW / 2, dlgY + 120, {0, 180, 216, 255}, m_fontSmall, true);

        drawText(UiStrings::DIALOG_DELETE_PROMPT, dlgX + dlgW / 2, dlgY + 165, {220, 225, 235, 255}, m_fontSmall, true);
        drawText(UiStrings::DIALOG_DELETE_SAFE_HINT, dlgX + dlgW / 2, dlgY + 195, {34, 197, 94, 255}, m_fontSmall, true);
    }

    // Action buttons with button icons
    int btnW = 220;
    int btnH = 50;
    int btnY = dlgY + 250;
    // A button (confirm - red background already in drawBadge)
    drawBadge(dlgX + 60, btnY, btnW, btnH, UiStrings::BTN_CONFIRM_DELETE, {185, 28, 28, 255}, {255, 255, 255, 255});
    // B button (cancel)
    drawBadge(dlgX + dlgW - 60 - btnW, btnY, btnW, btnH, UiStrings::BTN_CANCEL_DELETE, {55, 65, 81, 255}, {255, 255, 255, 255});
}

void UIManager::renderConfirmBatchDeleteDialog() {
    // Dim background overlay
    drawRect(0, 0, 1024, 768, {0, 0, 0, 190}, true);

    int dlgW = 680;
    int dlgH = 400;
    int dlgX = (1024 - dlgW) / 2;
    int dlgY = (768 - dlgH) / 2;

    // Borderless dialog - just rounded background
    drawRoundedRect(dlgX, dlgY, dlgW, dlgH, 16, {24, 28, 38, 255}, true);

    // Simple title bar (no border)
    drawRect(dlgX, dlgY, dlgW, 48, {185, 28, 28, 255}, true);
    drawText(UiStrings::MULTI_BATCH_DELETE_TITLE, dlgX + dlgW / 2, dlgY + 14, {255, 255, 255, 255}, m_fontLarge, true);

    // Count of selected games
    int selCount = static_cast<int>(m_selectedGameIds.size());

    // Calculate total size
    uint64_t totalSize = 0;
    for (int64_t gameId : m_selectedGameIds) {
        for (const auto& g : m_cachedGames) {
            if (g.id == gameId) {
                totalSize += g.sizeBytes;
                break;
            }
        }
    }

    drawText("Bạn muốn xóa " + std::to_string(selCount) + " game khỏi thẻ nhớ?", dlgX + dlgW / 2, dlgY + 80, {220, 225, 235, 255}, m_fontMedium, true);
    drawText("Tổng dung lượng: " + FileSystemManager::instance().formatBytes(totalSize), dlgX + dlgW / 2, dlgY + 115, {0, 180, 216, 255}, m_fontSmall, true);

    // Show selected game names (up to 5)
    int nameY = dlgY + 155;
    int shownCount = 0;
    for (const auto& g : m_cachedGames) {
        if (shownCount >= 5) break;
        auto it = std::find(m_selectedGameIds.begin(), m_selectedGameIds.end(), g.id);
        if (it != m_selectedGameIds.end()) {
            std::string title = g.title;
            if (title.length() > 40) title = title.substr(0, 37) + "...";
            drawText("- " + title, dlgX + 60, nameY, {200, 210, 225, 255}, m_fontSmall);
            nameY += 28;
            shownCount++;
        }
    }

    if (selCount > 5) {
        drawText("... và " + std::to_string(selCount - 5) + " game khác", dlgX + 60, nameY, {140, 155, 170, 255}, m_fontSmall);
    }

    drawText(UiStrings::MULTI_BATCH_DELETE_SAFE, dlgX + dlgW / 2, dlgY + dlgH - 100, {34, 197, 94, 255}, m_fontSmall, true);

    // Action buttons
    int btnW = 240;
    int btnH = 50;
    drawBadge(dlgX + 60, dlgY + dlgH - 70, btnW, btnH, UiStrings::MULTI_BATCH_DELETE_CONFIRM, {185, 28, 28, 255}, {255, 255, 255, 255});
    drawBadge(dlgX + dlgW - 60 - btnW, dlgY + dlgH - 70, btnW, btnH, UiStrings::BTN_CANCEL_DELETE, {55, 65, 81, 255}, {255, 255, 255, 255});
}

void UIManager::renderDisclaimerState() {
    // Borderless disclaimer card - no outer border
    int cardX = 80;
    int cardY = 80;
    int cardW = 864;
    int cardH = 615;

    // Main card background only
    drawRoundedRect(cardX, cardY, cardW, cardH, 16, {22, 27, 36, 255}, true);

    // Amber warning header bar
    drawRoundedRect(cardX, cardY, cardW, 58, 16, {50, 32, 12, 255}, true);
    drawText(UiStrings::DISCLAIMER_TITLE, 512, cardY + 18, {245, 158, 11, 255}, m_fontLarge, true);

    // Inner panel - subtle inset without border
    int innerX = cardX + 30;
    int innerY = cardY + 76;
    int innerW = cardW - 60;
    int innerH = 435;
    drawRoundedRect(innerX, innerY, innerW, innerH, 8, {16, 20, 28, 255}, true);

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
    // ─── Borderless Full-Width Sub-Header ───
    drawRect(0, 64, 1024, 48, {16, 20, 28, 255}, true);
    drawRect(0, 111, 1024, 1, {38, 48, 64, 255}, true);
    drawText(UiStrings::HEADER_SETTINGS, 36, 78, {0, 180, 216, 255}, m_fontLarge);

    std::string email = AuthManager::instance().getUserEmail();
    std::string folderId = DatabaseManager::instance().getSetting("drive_folder_id", UiStrings::SETTING_NOT_CONFIGURED);
    std::string lastSync = DatabaseManager::instance().getSetting("last_cloud_sync_time", UiStrings::SETTING_NEVER_SYNCED);
    std::string ip = PlatformInfo::instance().getIpAddress("wlan0");
    std::string webUrl = "http://" + (ip.empty() ? "192.168.1.164" : ip) + ":8080";

    struct SettingItem {
        std::string label;
        std::string value;
        SDL_Color valColor;
        std::string badgeText;
        SDL_Color badgeBg;
        SDL_Color badgeFg;
    };

    std::vector<SettingItem> items;
    items.reserve(10);

    // 0: Google Drive Account
    items.push_back({
        UiStrings::SETTING_DRIVE_STATUS,
        AuthManager::instance().isLinked() ? (std::string(UiStrings::SETTING_CONNECTED) + " (" + email + ")") : std::string(UiStrings::SETTING_DISCONNECTED),
        AuthManager::instance().isLinked() ? SDL_Color{34, 197, 94, 255} : SDL_Color{239, 68, 68, 255},
        AuthManager::instance().isLinked() ? std::string(UiStrings::SETTING_LOGOUT_BTN) : std::string(UiStrings::SETTING_CONNECT_WEB_BTN),
        AuthManager::instance().isLinked() ? SDL_Color{185, 28, 28, 255} : SDL_Color{30, 58, 138, 255},
        SDL_Color{255, 255, 255, 255}
    });

    // 1: Thư mục Drive
    items.push_back({UiStrings::SETTING_DRIVE_FOLDER, folderId, SDL_Color{0, 180, 216, 255}, "", SDL_Color{0, 0, 0, 0}, SDL_Color{0, 0, 0, 0}});

    // 2: Phiên bản hệ điều hành (OS Type)
    std::string osName = AppConfig::instance().getOSName();
    items.push_back({"Phiên bản OS", osName, SDL_Color{168, 85, 247, 255}, "[A] Đổi", SDL_Color{88, 28, 135, 255}, SDL_Color{255, 255, 255, 255}});

    // 3: Thư mục ROM trên thẻ nhớ
    items.push_back({UiStrings::SETTING_ROM_SD_FOLDER, AppConfig::instance().getRomsDir(), SDL_Color{255, 255, 255, 255}, "", SDL_Color{0, 0, 0, 0}, SDL_Color{0, 0, 0, 0}});

    // 4: Đồng bộ cuối
    items.push_back({UiStrings::SETTING_LAST_SYNC, lastSync, SDL_Color{255, 255, 255, 255}, "", SDL_Color{0, 0, 0, 0}, SDL_Color{0, 0, 0, 0}});

    // 5: Cơ sở dữ liệu SQLite
    items.push_back({UiStrings::SETTING_SQLITE_DB, AppConfig::instance().getDatabasePath(), SDL_Color{34, 197, 94, 255}, "", SDL_Color{0, 0, 0, 0}, SDL_Color{0, 0, 0, 0}});

    // 6: Chế độ quét thẻ nhớ
    items.push_back({UiStrings::SETTING_SCAN_MODE, UiStrings::SETTING_SCAN_AUTO, SDL_Color{34, 197, 94, 255}, "", SDL_Color{0, 0, 0, 0}, SDL_Color{0, 0, 0, 0}});

    // 7: Web Portal
    items.push_back({UiStrings::SETTING_WEB_PORTAL, webUrl, SDL_Color{0, 180, 216, 255}, "", SDL_Color{0, 0, 0, 0}, SDL_Color{0, 0, 0, 0}});

    // 8: Bộ nhớ đệm ảnh bìa
    items.push_back({UiStrings::SETTING_COVER_CACHE, UiStrings::SETTING_COVER_CACHE_VAL, SDL_Color{34, 197, 94, 255}, "", SDL_Color{0, 0, 0, 0}, SDL_Color{0, 0, 0, 0}});

    // 9: Xuất sao lưu cài đặt
    items.push_back({UiStrings::BACKUP_EXPORT_BTN, UiStrings::BACKUP_EXPORT_DESC, SDL_Color{168, 85, 247, 255}, "[A] Xuất sao lưu", SDL_Color{88, 28, 135, 255}, SDL_Color{255, 255, 255, 255}});

    // 10: Phục hồi cài đặt
    items.push_back({UiStrings::BACKUP_IMPORT_BTN, UiStrings::BACKUP_IMPORT_DESC, SDL_Color{0, 180, 216, 255}, "[A] Phục hồi", SDL_Color{21, 94, 117, 255}, SDL_Color{255, 255, 255, 255}});

    int cardX = 24;
    int cardW = 976;
    int rowH = 50;
    int spacing = 6;
    int stepY = rowH + spacing;
    int visibleRows = 9;

    int maxScroll = static_cast<int>(items.size()) - visibleRows;
    if (maxScroll < 0) maxScroll = 0;

    int startY = 122 - (m_settingsScrollOffset * stepY);

    for (size_t i = 0; i < items.size(); ++i) {
        int y = startY + static_cast<int>(i) * stepY;
        if (y < 70 || y > 680) continue;

        bool selected = (static_cast<int>(i) == m_selectedSettingsRow);

        SDL_Color bg;
        if (selected) {
            bg = SDL_Color{30, 58, 95, 255};
        } else if (i % 2 == 1) {
            bg = SDL_Color{22, 28, 38, 255};
        } else {
            bg = SDL_Color{16, 20, 28, 255};
        }

        drawRoundedRect(cardX, y, cardW, rowH, 8, bg, true);

        if (selected) {
            drawRoundedBorder(cardX, y, cardW, rowH, 8, {0, 180, 216, 255}, 2);
            // Left neon accent indicator
            drawRoundedRect(cardX + 4, y + 10, 5, rowH - 20, 2, {0, 180, 216, 255}, true);
        }

        SDL_Color lblColor = selected ? SDL_Color{255, 255, 255, 255} : SDL_Color{170, 185, 200, 255};
        drawText(items[i].label, cardX + 24, y + 12, lblColor, m_fontMedium);

        if (!items[i].badgeText.empty()) {
            // Shortened value to leave room for badge
            std::string val = items[i].value;
            if (val.length() > 38) val = val.substr(0, 35) + "...";
            drawText(val, cardX + 340, y + 15, items[i].valColor, m_fontSmall);
            drawBadge(cardX + cardW - 190, y + 6, 170, 38, items[i].badgeText, items[i].badgeBg, items[i].badgeFg);
        } else {
            std::string val = items[i].value;
            if (val.length() > 55) val = val.substr(0, 52) + "...";
            drawText(val, cardX + 340, y + 15, items[i].valColor, m_fontSmall);
        }
    }

    // Scrollbar indicator
    if (maxScroll > 0) {
        int scrollBarX = 1006;
        int scrollBarY = 122;
        int scrollBarH = visibleRows * stepY - spacing;
        int thumbH = scrollBarH * visibleRows / static_cast<int>(items.size());
        int thumbY = scrollBarY + (m_settingsScrollOffset * (scrollBarH - thumbH) / maxScroll);

        drawRoundedRect(scrollBarX, scrollBarY, 6, scrollBarH, 3, {35, 42, 54, 255}, true);
        drawRoundedRect(scrollBarX, thumbY, 6, thumbH, 3, {0, 180, 216, 255}, true);
    }

    drawText(UiStrings::BTN_BACK_MAIN_MENU_HINT, 512, 730, {130, 140, 155, 255}, m_fontSmall, true);
    drawText("▲▼ Di chuyển   [A] Chọn / Thực hiện", 740, 730, {100, 110, 125, 255}, m_fontSmall);
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
    // ─── Borderless Full-Width Sub-Header ───
    drawRect(0, 64, 1024, 48, {16, 20, 28, 255}, true);
    drawRect(0, 111, 1024, 1, {38, 48, 64, 255}, true);
    drawText(UiStrings::HEADER_DIAG, 36, 78, {0, 180, 216, 255}, m_fontLarge);

    auto diag = PlatformInfo::instance().getDiagnostics();

    struct DiagRow {
        std::string label;
        std::string value;
        SDL_Color valColor;
    };

    std::vector<DiagRow> rows = {
        {UiStrings::DIAG_HW_DEVICE, diag.socName, {255, 255, 255, 255}},
        {UiStrings::DIAG_CPU_ARCH, diag.cpuArch + std::string(UiStrings::DIAG_VAL_64BIT), {255, 255, 255, 255}},
        {UiStrings::DIAG_OS_KERNEL, diag.osName + " " + diag.kernelRelease, {255, 255, 255, 255}},
        {UiStrings::DIAG_RAM, std::string(UiStrings::DIAG_FREE_PREFIX) + diag.freeRam + UiStrings::DIAG_TOTAL_SEPARATOR + diag.totalRam, {34, 197, 94, 255}},
        {UiStrings::DIAG_DISPLAY, diag.displayResolution, {0, 180, 216, 255}},
        {UiStrings::DIAG_SDL2_GFX, "v" + diag.sdlVersion + std::string(UiStrings::DIAG_VAL_HW_ACCEL), {255, 255, 255, 255}},
        {UiStrings::DIAG_SQLITE_DB, "v" + diag.sqliteVersion + std::string(UiStrings::DIAG_VAL_SCHEMA_PREFIX) + std::to_string(CURRENT_SCHEMA_VERSION) + ")", {34, 197, 94, 255}},
        {UiStrings::DIAG_SD_STORAGE, std::string(UiStrings::DIAG_FREE_PREFIX) + diag.sdFreeSpace + UiStrings::DIAG_TOTAL_SEPARATOR + diag.sdTotalSpace, {34, 197, 94, 255}},
        {UiStrings::DIAG_GAMEPAD, diag.controllerName, {255, 255, 255, 255}},
        {UiStrings::DIAG_WIFI, diag.networkStatus + (diag.ipAddress != "N/A" ? " (IP: " + diag.ipAddress + ")" : ""), diag.ipAddress != "N/A" ? SDL_Color{34, 197, 94, 255} : SDL_Color{239, 68, 68, 255}},
        {UiStrings::DIAG_SAFETY, UiStrings::DIAG_SAFETY_VAL, {34, 197, 94, 255}}
    };

    int rowH = 44;
    int startY = 124 - (m_diagnosticsScrollOffset * rowH);
    int cardX = 24;
    int cardW = 976;
    int visibleRows = 14;  // How many rows fit on screen

    int maxScroll = static_cast<int>(rows.size()) - visibleRows;
    if (maxScroll < 0) maxScroll = 0;

    for (size_t i = 0; i < rows.size(); ++i) {
        int y = startY + static_cast<int>(i) * rowH;
        // Only draw if visible on screen
        if (y < 60 || y > 720) continue;

        if (i % 2 == 1) {
            drawRoundedRect(cardX, y - 4, cardW, rowH, 8, {22, 28, 38, 255}, true);
        }
        drawText(rows[i].label, cardX + 24, y, {150, 165, 180, 255}, m_fontSmall);
        drawText(rows[i].value, cardX + 280, y, rows[i].valColor, m_fontSmall);
    }

    // Scroll indicator
    if (maxScroll > 0) {
        int scrollBarX = 990;
        int scrollBarH = 600;
        int scrollBarY = 64;
        int thumbH = scrollBarH * visibleRows / rows.size();
        int thumbY = scrollBarY + (m_diagnosticsScrollOffset * (scrollBarH - thumbH) / maxScroll);

        drawRoundedRect(scrollBarX, scrollBarY, 8, scrollBarH, 4, {35, 42, 54, 255}, true);
        drawRoundedRect(scrollBarX, thumbY, 8, thumbH, 4, {0, 180, 216, 255}, true);
    }

    drawText(UiStrings::BTN_BACK_MAIN_MENU_HINT, 512, 730, {130, 140, 155, 255}, m_fontSmall, true);
    drawText("▲▼ Cuộn lên/xuống", 880, 730, {100, 110, 125, 255}, m_fontSmall);
}

void UIManager::renderReverseSyncState() {
    // ─── Borderless Full-Width Sub-Header ───
    drawRect(0, 64, 1024, 48, {16, 20, 28, 255}, true);
    drawRect(0, 111, 1024, 1, {38, 48, 64, 255}, true);
    drawText(UiStrings::REVERSE_SYNC_TITLE, 36, 78, {168, 85, 247, 255}, m_fontLarge);

    auto prog = UploadManager::instance().getProgress();

    int cardX = 100;
    int cardW = 824;
    int cardY = 126;
    int cardH = 480;

    drawRoundedRect(cardX, cardY, cardW, cardH, 16, {22, 28, 38, 255}, true);
    drawRoundedBorder(cardX, cardY, cardW, cardH, 16, {168, 85, 247, 255}, 2);

    int contentY = cardY + 30;

    switch (prog.state) {
        case UploadState::IDLE: {
            drawText(UiStrings::REVERSE_SYNC_PREPARING, cardX + cardW / 2, contentY + 100, {255, 255, 255, 255}, m_fontLarge, true);
            break;
        }

        case UploadState::PREPARING: {
            drawText(UiStrings::REVERSE_SYNC_PREPARING, cardX + cardW / 2, contentY + 100, {0, 180, 216, 255}, m_fontLarge, true);
            break;
        }

        case UploadState::UPLOADING: {
            // Game title
            std::string titleText = UiStrings::REVERSE_SYNC_UPLOADING;
            drawText(titleText, cardX + cardW / 2, contentY + 20, {255, 255, 255, 255}, m_fontMedium, true);

            // Current file
            std::string gameName = prog.gameTitle;
            if (gameName.length() > 50) gameName = gameName.substr(0, 47) + "...";
            drawText(gameName, cardX + cardW / 2, contentY + 60, {0, 180, 216, 255}, m_fontLarge, true);

            // Progress
            int barW = 600;
            int barH = 20;
            int barX = cardX + (cardW - barW) / 2;
            int barY = contentY + 120;
            drawRoundedRect(barX, barY, barW, barH, 10, {35, 45, 60, 255}, true);
            float pct = std::max(0.0f, std::min(100.0f, (float)prog.progressPct));
            drawRoundedRect(barX, barY, (int)(barW * (pct / 100.0)), barH, 10, {168, 85, 247, 255}, true);

            // Stats & speed
            std::string speedStr = "";
            if (prog.speedKBps >= 1024.0) {
                char sBuf[32];
                std::snprintf(sBuf, sizeof(sBuf), "  •  %.1f MB/s", prog.speedKBps / 1024.0);
                speedStr = sBuf;
            } else if (prog.speedKBps > 0.0) {
                char sBuf[32];
                std::snprintf(sBuf, sizeof(sBuf), "  •  %.0f KB/s", prog.speedKBps);
                speedStr = sBuf;
            }

            char pctBuf[16];
            std::snprintf(pctBuf, sizeof(pctBuf), " (%.1f%%)", prog.progressPct);

            std::string stats = UiStrings::REVERSE_SYNC_STATS +
                               std::to_string(prog.currentIndex) + " / " + std::to_string(prog.totalGames) +
                               "  •  " + FileSystemManager::instance().formatBytes(prog.bytesUploaded) +
                               " / " + FileSystemManager::instance().formatBytes(prog.totalBytes) +
                               pctBuf + speedStr;
            drawText(stats, cardX + cardW / 2, barY + 50, {200, 210, 225, 255}, m_fontMedium, true);

            // Success/fail counts
            int statY = barY + 90;
            drawText("✓ " + std::to_string(prog.gamesUploaded) + " thành công", cardX + 100, statY, {34, 197, 94, 255}, m_fontMedium);
            drawText("✗ " + std::to_string(prog.gamesFailed) + " thất bại", cardX + cardW - 200, statY, {239, 68, 68, 255}, m_fontMedium);
            break;
        }

        case UploadState::COMPLETED: {
            drawText(UiStrings::REVERSE_SYNC_SUCCESS, cardX + cardW / 2, contentY + 80, {34, 197, 94, 255}, m_fontLarge, true);
            drawText(std::to_string(prog.gamesUploaded) + UiStrings::REVERSE_SYNC_SUCCESS_SUF, cardX + cardW / 2, contentY + 130, {255, 255, 255, 255}, m_fontMedium, true);
            if (prog.gamesFailed > 0) {
                drawText(std::to_string(prog.gamesFailed) + " game thất bại.", cardX + cardW / 2, contentY + 170, {239, 68, 68, 255}, m_fontSmall, true);
            }
            break;
        }

        case UploadState::FAILED: {
            drawText("THẤT BẠI", cardX + cardW / 2, contentY + 60, {239, 68, 68, 255}, m_fontLarge, true);
            // Multi-line error message display
            std::istringstream errStream(prog.errorMessage);
            std::string errLine;
            int errY = contentY + 110;
            while (std::getline(errStream, errLine)) {
                drawText(errLine, cardX + cardW / 2, errY, {200, 210, 225, 255}, m_fontSmall, true);
                errY += 28;
            }
            break;
        }

        case UploadState::CANCELLED: {
            drawText("ĐÃ HỦY", cardX + cardW / 2, contentY + 80, {245, 158, 11, 255}, m_fontLarge, true);
            drawText("Đã tải lên " + std::to_string(prog.gamesUploaded) + " game trước khi hủy.", cardX + cardW / 2, contentY + 130, {200, 210, 225, 255}, m_fontSmall, true);
            break;
        }
    }

    // Cancel button
    if (prog.state == UploadState::UPLOADING || prog.state == UploadState::PREPARING) {
        drawBadge(cardX + (cardW - 180) / 2, cardY + cardH - 70, 180, 46, UiStrings::REVERSE_SYNC_CANCEL_BTN, {55, 65, 81, 255}, {255, 255, 255, 255});
    } else {
        drawBadge(cardX + (cardW - 180) / 2, cardY + cardH - 70, 180, 46, "[A] / [B] Quay lại", {55, 65, 81, 255}, {255, 255, 255, 255});
    }
}

void UIManager::renderUploadOverlay() {
    // If reverse sync is active, show persistent overlay
    if (!UploadManager::instance().isUploading()) return;

    auto prog = UploadManager::instance().getProgress();
    if (prog.state != UploadState::UPLOADING) return;

    // Small persistent banner at bottom
    int bannerW = 500;
    int bannerH = 40;
    int bannerX = (1024 - bannerW) / 2;
    int bannerY = 720;

    drawRoundedRect(bannerX, bannerY, bannerW, bannerH, 8, {30, 20, 45, 255}, true);
    drawRoundedBorder(bannerX, bannerY, bannerW, bannerH, 8, {168, 85, 247, 255}, 1);

    char pctBuf[16];
    std::snprintf(pctBuf, sizeof(pctBuf), "%.0f%%", prog.progressPct);
    std::string text = std::string(UiStrings::REVERSE_SYNC_UPLOADING) + prog.gameTitle + " " + pctBuf;
    if (text.length() > 55) text = text.substr(0, 52) + "...";
    drawText(text, bannerX + bannerW / 2, bannerY + 10, {200, 210, 225, 255}, m_fontSmall, true);
}

void UIManager::renderOTAUpdateState() {
    // ─── Borderless Full-Width Sub-Header ───
    drawRect(0, 64, 1024, 48, {16, 20, 28, 255}, true);
    drawRect(0, 111, 1024, 1, {38, 48, 64, 255}, true);
    drawText(UiStrings::HEADER_OTA, 36, 78, {0, 180, 216, 255}, m_fontLarge);

    auto prog = UpdateManager::instance().getProgress();
    auto info = UpdateManager::instance().getLatestInfo();

    int cardX = 24;
    int cardW = 976;

    std::string currentVer = std::string(UiStrings::OTA_DEV_CURRENT_VER) + UpdateManager::instance().getCurrentVersion();
    drawText(currentVer, cardX + 20, 126, {210, 220, 235, 255}, m_fontMedium);

    std::string repoSource = std::string(UiStrings::OTA_DEV_SOURCE_PREFIX) + std::string(GITHUB_REPO);
    drawText(repoSource, cardX + 20, 156, {130, 145, 165, 255}, m_fontSmall);

    int contentBoxY = 190;
    int contentBoxH = 360;
    drawRoundedRect(cardX, contentBoxY, cardW, contentBoxH, 14, {18, 24, 34, 255}, true);
    drawRoundedBorder(cardX, contentBoxY, cardW, contentBoxH, 14, {38, 48, 64, 255}, 1);

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
            drawBadge(392, contentBoxY + 30, 240, 36, UiStrings::OTA_STATUS_NEW_UPDATE, {180, 83, 9, 255}, {255, 255, 255, 255});
            std::string newVerTxt = std::string(UiStrings::OTA_DEV_NEW_VER_PREFIX) + info.remoteVersion + (info.releaseDate.empty() ? "" : " (" + info.releaseDate + ")");
            drawText(newVerTxt, 512, contentBoxY + 85, {0, 180, 216, 255}, m_fontLarge, true);

            if (!info.changelog.empty()) {
                drawText(UiStrings::OTA_CHANGELOG_TITLE, cardX + 40, contentBoxY + 130, {255, 255, 255, 255}, m_fontSmall);
                drawText(info.changelog, cardX + 40, contentBoxY + 160, {170, 180, 195, 255}, m_fontSmall);
            }

            drawBadge(337, contentBoxY + 260, 350, 52, UiStrings::OTA_BTN_INSTALL_NOW, {34, 197, 94, 255}, {0, 0, 0, 255});
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
            } else if (prog.state == UpdateState::INSTALLING || prog.state == UpdateState::INSTALLING_DEPS) {
                title = "Đang cài đặt bản cập nhật...";
            }
            drawText(title, 512, contentBoxY + 40, {0, 180, 216, 255}, m_fontLarge, true);

            // Detailed current step description
            std::string stepMsg = prog.currentStep.empty() ? title : prog.currentStep;
            drawText(stepMsg, 512, contentBoxY + 80, {210, 225, 240, 255}, m_fontSmall, true);

            int barW = 580;
            int barH = 22;
            int barX = 512 - barW / 2;
            int barY = contentBoxY + 115;
            drawRoundedRect(barX, barY, barW, barH, 10, {35, 42, 54, 255}, true);
            float pct = std::max(0.0, std::min(100.0, prog.progressPct));
            drawRoundedRect(barX, barY, (int)(barW * (pct / 100.0)), barH, 10, {34, 197, 94, 255}, true);

            char pctBuf[32];
            std::snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", pct);
            std::string dlStr = (prog.bytesDownloaded > 0) ? FileSystemManager::instance().formatBytes(prog.bytesDownloaded) : "0 B";
            std::string totStr = (prog.totalBytes > 0) ? FileSystemManager::instance().formatBytes(prog.totalBytes) : "...";
            std::string progressInfo = dlStr + " / " + totStr + " (" + pctBuf + ")";

            // Format download speed
            if (prog.speedKBps >= 1024.0) {
                char sBuf[32];
                std::snprintf(sBuf, sizeof(sBuf), "  •  %.1f MB/s", prog.speedKBps / 1024.0);
                progressInfo += sBuf;
            } else if (prog.speedKBps > 0.0) {
                char sBuf[32];
                std::snprintf(sBuf, sizeof(sBuf), "  •  %.0f KB/s", prog.speedKBps);
                progressInfo += sBuf;
            }

            drawText(progressInfo, 512, barY + 34, {255, 255, 255, 255}, m_fontSmall, true);

            if (prog.state == UpdateState::VERIFYING) {
                drawText(UiStrings::OTA_VERIFYING_FILE, 512, contentBoxY + 190, {245, 158, 11, 255}, m_fontSmall, true);
            }
            if (prog.state == UpdateState::DOWNLOADING || prog.state == UpdateState::DOWNLOADING_DEPS) {
                drawBadge(422, contentBoxY + 265, 180, 44, UiStrings::OTA_BTN_CANCEL_DOWNLOAD, {55, 65, 81, 255}, {255, 255, 255, 255});
            } else {
                drawBadge(372, contentBoxY + 265, 280, 44, "Đang xử lý, vui lòng chờ...", {40, 50, 65, 255}, {200, 215, 230, 255});
            }
            break;
        }
        case UpdateState::COMPLETED: {
            drawBadge(412, contentBoxY + 35, 200, 40, UiStrings::OTA_STATUS_COMPLETED, {22, 101, 52, 255}, {34, 197, 94, 255});
            drawText(UiStrings::OTA_MSG_COMPLETED, 512, contentBoxY + 105, {34, 197, 94, 255}, m_fontLarge, true);
            drawText(UiStrings::OTA_MSG_RESTART_HINT, 512, contentBoxY + 150, {255, 255, 255, 255}, m_fontSmall, true);

            drawBadge(327, contentBoxY + 245, 370, 52, UiStrings::OTA_BTN_RESTART_NOW, {34, 197, 94, 255}, {0, 0, 0, 255});
            break;
        }
        case UpdateState::FAILED: {
            drawBadge(422, contentBoxY + 35, 180, 40, UiStrings::OTA_STATUS_FAILED, {153, 27, 27, 255}, {248, 113, 113, 255});
            drawText(UiStrings::OTA_MSG_FAILED, 512, contentBoxY + 105, {239, 68, 68, 255}, m_fontLarge, true);
            std::string err = prog.errorMessage.empty() ? UiStrings::OTA_ERR_NETWORK : prog.errorMessage;
            drawText(err, 512, contentBoxY + 150, {245, 158, 11, 255}, m_fontSmall, true);

            drawBadge(362, contentBoxY + 245, 300, 48, UiStrings::OTA_BTNS_RETRY_BACK, {35, 45, 60, 255}, {255, 255, 255, 255});
            break;
        }
    }

    drawText(UiStrings::BTN_BACK_MAIN_MENU_HINT, 512, 650, {130, 140, 155, 255}, m_fontSmall, true);
}

void UIManager::renderIPTVState() {
    // Header
    drawRect(0, 0, 1024, 64, {18, 22, 30, 255}, true);
    drawRect(0, 63, 1024, 1, {40, 48, 62, 255}, true);

    std::string title = m_iptvShowFavoritesOnly ? "XEM TV - KÊNH YÊU THÍCH ★" : UiStrings::IPTV_TITLE;
    drawText(title, 24, 18, {0, 180, 216, 255}, m_fontLarge);

    std::vector<IPTVChannel> channels;
    if (m_iptvShowFavoritesOnly) {
        channels = IPTVManager::instance().getFavoriteChannels();
    } else {
        channels = IPTVManager::instance().getChannels();
    }
    int channelCount = static_cast<int>(channels.size());

    // Channel count
    std::string countText = std::to_string(channelCount) + (m_iptvShowFavoritesOnly ? " kênh yêu thích" : " kênh");
    drawText(countText, 1024 - 24, 18, {130, 145, 165, 255}, m_fontSmall, true);

    // Channel list (full height, 10 items per page)
    int listY = 74;
    int itemH = 56;
    int visibleItems = 10;

    if (channels.empty()) {
        if (m_iptvShowFavoritesOnly) {
            drawText("Chưa có kênh yêu thích nào.", 512, 340, {150, 160, 175, 255}, m_fontMedium, true);
            drawText("Bấm [X] trên danh sách kênh để đánh dấu yêu thích ★", 512, 385, {100, 110, 125, 255}, m_fontSmall, true);
        } else {
            drawText(UiStrings::IPTV_NO_CHANNELS, 512, 340, {150, 160, 175, 255}, m_fontMedium, true);
            drawText("Vui lòng tải playlist (.m3u) qua Web Server (Cổng 8080)", 512, 385, {100, 110, 125, 255}, m_fontSmall, true);
        }
    } else {
        for (int i = m_iptvScrollOffset; i < channelCount && i < m_iptvScrollOffset + visibleItems; i++) {
            int y = listY + (i - m_iptvScrollOffset) * (itemH + 6);
            bool isSelected = (i == m_selectedIPTVChannelIndex);

            SDL_Color bg = isSelected ? SDL_Color{30, 58, 95, 255} : SDL_Color{22, 28, 38, 255};
            drawRoundedRect(16, y, 992, itemH, 8, bg, true);

            if (isSelected) {
                drawRoundedBorder(16, y, 992, itemH, 8, {0, 180, 216, 255}, 2);
            }

            // Channel number
            char numBuf[16];
            snprintf(numBuf, sizeof(numBuf), "%02d", i + 1);
            drawText(numBuf, 35, y + 16, {0, 180, 216, 255}, m_fontMedium);

            // Channel name
            std::string chanName = channels[i].name;
            if (chanName.length() > 28) {
                chanName = chanName.substr(0, 26) + "..";
            }
            drawText(chanName, 85, y + 15, {255, 255, 255, 255}, m_fontMedium);

            // Playing indicator
            if (IPTVManager::instance().isPlaying() && IPTVManager::instance().getCurrentChannelName() == channels[i].name) {
                drawText("● ĐANG PHÁT", 475, y + 18, {34, 197, 94, 255}, m_fontSmall);
            }

            // Favorite star badge
            if (channels[i].isFavorite) {
                drawBadge(605, y + 15, 34, 26, "★", {202, 138, 4, 255}, {255, 255, 255, 255});
            }

            // Channel group tag
            if (!channels[i].group.empty()) {
                std::string grp = channels[i].group;
                if (grp.length() > 14) grp = grp.substr(0, 12) + "..";
                drawText(grp, 650, y + 18, {130, 140, 155, 255}, m_fontSmall);
            }

            // Source name column on the far right
            std::string srcText = channels[i].source.empty() ? "Mặc định" : channels[i].source;
            if (srcText.length() > 18) srcText = srcText.substr(0, 16) + "..";
            drawBadge(815, y + 14, 180, 28, srcText, {28, 42, 62, 255}, {147, 197, 253, 255});
            drawRoundedBorder(815, y + 14, 180, 28, 6, {59, 130, 246, 120}, 1);
        }
    }

    // Footer
    drawRect(0, 715, 1024, 53, {18, 22, 30, 255}, true);
    drawRect(0, 715, 1024, 1, {40, 48, 62, 255}, true);

    std::string footerText = m_iptvShowFavoritesOnly
        ? "[A] Phát trực tiếp   [B] Tất cả kênh   [X] Bỏ thích ★   [L1/R1] Sang trang   [SELECT] Tìm kiếm"
        : "[A] Phát trực tiếp   [B] Menu   [X] Thích ★   [Y] Lọc ★   [L1/R1] Sang trang   [SELECT] Tìm kiếm";
    drawText(footerText, 512, 730, {210, 220, 230, 255}, m_fontSmall, true);
}

void UIManager::renderIPTVSearchState() {
    static const char* qwertyRows[] = {
        "1234567890",
        "QWERTYUIOP",
        "ASDFGHJKL-",
        "ZXCVBNM<_*"  // '<' = DEL, '_' = SPACE, '*' = OK
    };
    static const int kbRowCount = 4;
    static const int kbColCount = 10;

    // Header
    drawRect(0, 0, 1024, 64, {18, 22, 30, 255}, true);
    drawRect(0, 63, 1024, 1, {40, 48, 62, 255}, true);
    drawText("TÌM KIẾM KÊNH IPTV", 24, 18, {0, 180, 216, 255}, m_fontLarge);

    int numResults = static_cast<int>(m_iptvSearchResults.size());
    std::string countText = std::to_string(numResults) + " kết quả";
    drawText(countText, 1024 - 24, 18, {130, 145, 165, 255}, m_fontSmall, true);

    // Left panel: Search Box + QWERTY Keyboard
    int panelW = 430;
    int panelX = 24;
    int panelY = 74;

    // Search Query Box
    drawRoundedRect(panelX, panelY, panelW, 52, 10, {22, 32, 46, 255}, true);
    drawRoundedBorder(panelX, panelY, panelW, 52, 10, {0, 180, 216, 255}, 2);
    std::string displayQuery = m_iptvSearchQuery.empty() ? "Nhập tên kênh (VTV, HBO...)" : m_iptvSearchQuery + "_";
    SDL_Color qColor = m_iptvSearchQuery.empty() ? SDL_Color{80, 95, 115, 255} : SDL_Color{255, 255, 255, 255};
    drawText(displayQuery, panelX + 16, panelY + 14, qColor, m_fontMedium);

    // QWERTY Virtual Keyboard
    int kbStartY = panelY + 68;
    int cellW = 37;
    int cellH = 46;
    int gap = 4;
    int kbPadX = 12;

    for (int row = 0; row < kbRowCount; row++) {
        for (int col = 0; col < kbColCount; col++) {
            int cx = panelX + kbPadX + col * (cellW + gap);
            int cy = kbStartY + row * (cellH + 8);
            bool isSel = (!m_iptvKbInResults && m_iptvKbRow == row && m_iptvKbCol == col);

            char ch = qwertyRows[row][col];
            std::string label;
            if (ch == '<') label = "DEL";
            else if (ch == '_') label = "SPC";
            else if (ch == '*') label = "OK";
            else label = std::string(1, ch);

            SDL_Color bg = isSel ? SDL_Color{0, 180, 216, 255} : SDL_Color{28, 38, 55, 255};
            SDL_Color fg = isSel ? SDL_Color{0, 0, 0, 255} : SDL_Color{220, 230, 240, 255};
            drawRoundedRect(cx, cy, cellW, cellH, 6, bg, true);
            drawRoundedBorder(cx, cy, cellW, cellH, 6, isSel ? SDL_Color{255, 255, 255, 255} : SDL_Color{45, 60, 80, 255}, 1);
            drawText(label, cx + cellW / 2, cy + cellH / 2 - 10, fg, m_fontSmall, true);
        }
    }

    // Divider line
    drawRect(476, 64, 1, 651, {38, 48, 64, 255}, true);

    // Right panel: Results List
    int rPanelX = 496;
    int rPanelW = 1024 - rPanelX - 24;
    int rPanelY = panelY;

    if (m_iptvSearchQuery.empty()) {
        drawText("Nhập chữ cái trên bàn phím để tìm kênh.", rPanelX + rPanelW / 2, rPanelY + 60, {100, 115, 135, 255}, m_fontSmall, true);
        drawText("Hỗ trợ tìm theo tên kênh hoặc thể loại.", rPanelX + rPanelW / 2, rPanelY + 100, {80, 95, 115, 255}, m_fontSmall, true);
    } else if (numResults == 0) {
        drawText("Không tìm thấy kênh phù hợp.", rPanelX + rPanelW / 2, rPanelY + 60, {239, 68, 68, 255}, m_fontSmall, true);
    } else {
        int pageSize = 10;
        int itemH = 56;
        int listStartY = rPanelY;

        for (int i = 0; i < pageSize && (m_iptvSearchScrollOffset + i) < numResults; i++) {
            int idx = m_iptvSearchScrollOffset + i;
            const auto& chan = m_iptvSearchResults[idx];
            bool isSel = (m_iptvKbInResults && idx == m_iptvSearchSelectedIndex);

            int itemY = listStartY + i * (itemH + 6);
            SDL_Color rowBg = isSel ? SDL_Color{30, 58, 95, 255} : SDL_Color{22, 28, 38, 255};
            drawRoundedRect(rPanelX, itemY, rPanelW, itemH, 8, rowBg, true);
            if (isSel) {
                drawRoundedBorder(rPanelX, itemY, rPanelW, itemH, 8, {0, 180, 216, 255}, 2);
            }

            // Channel number
            char numBuf[16];
            snprintf(numBuf, sizeof(numBuf), "%02d", idx + 1);
            drawText(numBuf, rPanelX + 16, itemY + 16, {0, 180, 216, 255}, m_fontMedium);

            // Channel name
            std::string chanName = chan.name;
            if (chanName.length() > 16) {
                chanName = chanName.substr(0, 14) + "..";
            }
            drawText(chanName, rPanelX + 55, itemY + 15, {255, 255, 255, 255}, m_fontMedium);

            // Favorite star badge
            if (chan.isFavorite) {
                drawBadge(rPanelX + rPanelW - 225, itemY + 15, 28, 26, "★", {202, 138, 4, 255}, {255, 255, 255, 255});
            }

            // Group tag
            if (!chan.group.empty()) {
                std::string grp = chan.group;
                if (grp.length() > 10) grp = grp.substr(0, 8) + "..";
                drawText(grp, rPanelX + rPanelW - 190, itemY + 18, {130, 140, 155, 255}, m_fontSmall);
            }

            // Source tag column
            std::string src = chan.source.empty() ? "Nguồn" : chan.source;
            if (src.length() > 9) src = src.substr(0, 8) + "..";
            drawBadge(rPanelX + rPanelW - 95, itemY + 14, 90, 28, src, {28, 42, 62, 255}, {147, 197, 253, 255});
            drawRoundedBorder(rPanelX + rPanelW - 95, itemY + 14, 90, 28, 6, {59, 130, 246, 120}, 1);
        }
    }

    // Footer
    drawRect(0, 715, 1024, 53, {18, 22, 30, 255}, true);
    drawRect(0, 715, 1024, 1, {40, 48, 62, 255}, true);

    std::string footerText = (!m_iptvKbInResults)
        ? "[A] Nhập phím   [B] Trở về   [X] Xóa chữ   [OK / START] Xem kết quả"
        : "[A] Phát kênh   [B] Bàn phím   [X] Thích ★   [L1/R1] Sang trang   [▲ ▼] Chọn kênh";
    drawText(footerText, 512, 730, {210, 220, 230, 255}, m_fontSmall, true);
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
        case UIState::CONFIRM_BATCH_DELETE: renderGameListState(); renderConfirmBatchDeleteDialog(); break;
        case UIState::DISCLAIMER:       renderDisclaimerState(); break;
        case UIState::CLOUD_LOGIN:      renderCloudLoginState(); break;
        case UIState::SETTINGS:         renderSettingsState(); break;
        case UIState::DIAGNOSTICS:      renderDiagnosticsState(); break;
        case UIState::OTA_UPDATE:       renderOTAUpdateState(); break;
        case UIState::REVERSE_SYNC:     renderReverseSyncState(); break;
        case UIState::IPTV_LIST:        renderIPTVState(); break;
        case UIState::IPTV_SEARCH:      renderIPTVSearchState(); break;
        case UIState::YOUTUBE_SEARCH:   renderYouTubeSearchState(); break;
        case UIState::YOUTUBE_RESULTS:  renderYouTubeResultsState(); break;
        default: break;
    }

    renderFooter();
    renderToast();
    renderSyncOverlay();
    renderUploadOverlay();
    SDL_RenderPresent(m_renderer);
}

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::stringstream ss(s);
    std::string token;
    while (std::getline(ss, token, delim)) parts.push_back(token);
    return parts;
}

static std::string formatDuration(const std::string& raw) {
    if (raw.empty() || raw == "NA" || raw == "None") return "";
    try {
        long sec = std::stol(raw);
        if (sec <= 0) return "";
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

static std::string formatViews(const std::string& raw) {
    if (raw.empty() || raw == "NA" || raw == "None") return "";
    try {
        long long views = std::stoll(raw);
        if (views <= 0) return "";
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

static std::string decodeJsonText(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            char next = raw[i + 1];
            if (next == '\"') { out += '\"'; i++; }
            else if (next == '\\') { out += '\\'; i++; }
            else if (next == '/') { out += '/'; i++; }
            else if (next == 'n' || next == 'r' || next == 't') { out += ' '; i++; }
            else if (next == 'u' && i + 5 < raw.size()) {
                std::string hex = raw.substr(i + 2, 4);
                try {
                    unsigned long code = std::stoul(hex, nullptr, 16);
                    if (code == 0x0026) out += '&';
                    else if (code == 0x0027) out += '\'';
                    else if (code == 0x0022) out += '\"';
                    else if (code < 128) out += static_cast<char>(code);
                    else {
                        if (code < 0x800) {
                            out += static_cast<char>(0xC0 | (code >> 6));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (code >> 12));
                            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        }
                    }
                    i += 5;
                } catch (...) {
                    out += raw[i];
                }
            } else {
                out += next;
                i++;
            }
        } else if (raw[i] == '|') {
            out += '-';
        } else {
            out += raw[i];
        }
    }
    return out;
}

static std::string extractField(const std::string& block, const std::string& startKey, const std::string& valKey) {
    size_t sPos = block.find(startKey);
    if (sPos == std::string::npos) return "";
    size_t vPos = block.find(valKey, sPos);
    if (vPos == std::string::npos || vPos > sPos + 400) return "";
    size_t quoteStart = block.find('\"', vPos + valKey.length());
    if (quoteStart == std::string::npos) return "";
    size_t quoteEnd = quoteStart + 1;
    while (quoteEnd < block.length()) {
        if (block[quoteEnd] == '\"' && block[quoteEnd - 1] != '\\') break;
        quoteEnd++;
    }
    if (quoteEnd >= block.length()) return "";
    return block.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
}

static std::vector<std::string> parseInnertubeSearchResponse(const std::string& json) {
    std::vector<std::string> results;
    size_t searchPos = 0;

    while (true) {
        size_t p1 = json.find("\"videoWithContextRenderer\":", searchPos);
        size_t p2 = json.find("\"videoRenderer\":", searchPos);
        size_t matchPos = std::string::npos;

        if (p1 != std::string::npos && (p2 == std::string::npos || p1 < p2)) {
            matchPos = p1;
        } else if (p2 != std::string::npos) {
            matchPos = p2;
        } else {
            break;
        }

        size_t bracePos = json.find('{', matchPos);
        if (bracePos == std::string::npos) break;
        searchPos = bracePos + 1;

        // Find end of this item block (next renderer or max 35000 chars)
        size_t nextP1 = json.find("\"videoWithContextRenderer\":", searchPos);
        size_t nextP2 = json.find("\"videoRenderer\":", searchPos);
        size_t nextItem = std::min(nextP1, nextP2);
        size_t blockEnd = (nextItem != std::string::npos) ? nextItem : std::min(json.length(), searchPos + 35000);
        std::string block = json.substr(searchPos, blockEnd - searchPos);

        std::string vid;
        // Extract video ID: either from thumbnail URL or "videoId"
        size_t imgPos = block.find("i.ytimg.com/vi/");
        if (imgPos != std::string::npos) {
            size_t slashPos = block.find('/', imgPos + 15);
            if (slashPos != std::string::npos && slashPos - (imgPos + 15) == 11) {
                vid = block.substr(imgPos + 15, 11);
            }
        }
        if (vid.empty()) {
            vid = extractField(block, "\"videoId\"", ":");
        }
        if (vid.empty() || vid.length() != 11) continue;

        // Title: headline or title
        std::string title = decodeJsonText(extractField(block, "\"headline\"", "\"text\""));
        if (title.empty()) title = decodeJsonText(extractField(block, "\"headline\"", "\"content\""));
        if (title.empty()) title = decodeJsonText(extractField(block, "\"title\"", "\"text\""));
        if (title.empty()) title = decodeJsonText(extractField(block, "\"title\"", "\"content\""));
        if (title.empty()) continue;

        // Channel / Uploader
        std::string channel = decodeJsonText(extractField(block, "\"shortBylineText\"", "\"text\""));
        if (channel.empty()) channel = decodeJsonText(extractField(block, "\"longBylineText\"", "\"text\""));
        if (channel.empty()) channel = decodeJsonText(extractField(block, "\"ownerText\"", "\"text\""));
        if (channel.empty()) channel = "YouTube";

        // Duration: lengthText or thumbnailOverlayTimeStatusRenderer
        std::string duration = extractField(block, "\"lengthText\"", "\"simpleText\"");
        if (duration.empty()) duration = extractField(block, "\"lengthText\"", "\"text\"");
        if (duration.empty()) duration = extractField(block, "\"thumbnailOverlayTimeStatusRenderer\"", "\"text\"");
        if (duration.empty()) duration = "--:--";

        // Views
        std::string views = decodeJsonText(extractField(block, "\"shortViewCountText\"", "\"simpleText\""));
        if (views.empty()) views = decodeJsonText(extractField(block, "\"shortViewCountText\"", "\"text\""));
        if (views.empty()) views = decodeJsonText(extractField(block, "\"viewCountText\"", "\"simpleText\""));
        if (views.empty()) views = decodeJsonText(extractField(block, "\"viewCountText\"", "\"text\""));

        bool dup = false;
        for (const auto& item : results) {
            if (item.compare(0, 11, vid) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            results.push_back(vid + "|" + title + "|" + duration + "|" + channel + "|" + views);
        }
    }
    return results;
}

std::vector<std::string> UIManager::runYouTubeSearch(const std::string& query, int page) {
    std::vector<std::string> results;
    if (query.empty()) return results;

    // 1. Ultra-fast YouTube Innertube MWEB API via HttpClient (~1-2 seconds)
    try {
        std::string jsonEscaped;
        for (char c : query) {
            if (c == '"') jsonEscaped += "\\\"";
            else if (c == '\\') jsonEscaped += "\\\\";
            else jsonEscaped += c;
        }
        std::string postBody = "{\"context\":{\"client\":{\"clientName\":\"MWEB\",\"clientVersion\":\"2.20231201.00.00\",\"hl\":\"vi\",\"gl\":\"VN\"}},\"query\":\"" + jsonEscaped + "\"}";
        std::string apiUrl = "https://www.youtube.com/youtubei/v1/search?key=AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8";

        Logger::info("[YouTube] Fast search via Innertube MWEB API: " + query);
        HttpResponse resp = HttpClient::instance().post(apiUrl, postBody, {"Content-Type: application/json"}, 10);
        if (resp.success && resp.statusCode == 200 && !resp.body.empty()) {
            results = parseInnertubeSearchResponse(resp.body);
            if (!results.empty()) {
                Logger::info("[YouTube] Innertube API returned " + std::to_string(results.size()) + " items");
                return results;
            }
        }
        Logger::warn("[YouTube] Innertube API empty or failed (status=" + std::to_string(resp.statusCode) + ", err=" + resp.error + "), falling back to script");
    } catch (const std::exception& e) {
        Logger::warn("[YouTube] Innertube search exception: " + std::string(e.what()));
    }

    // 2. Fallback to youtube_search.sh (yt-dlp with --flat-playlist)
    std::string appRoot = AppConfig::instance().getAppRoot();
    if (appRoot.empty()) appRoot = "/mnt/SDCARD/Apps/RomCloud";
    std::string scriptPath = appRoot + "/scripts/youtube_search.sh";

    std::string escapedQuery;
    for (char c : query) {
        if (c == '"' || c == '\\') escapedQuery += '\\';
        escapedQuery += c;
    }

    std::string cmd = "\"" + scriptPath + "\" search \"" + escapedQuery + "\" " + std::to_string(page) + " 6 2>/dev/null";
    Logger::info("[YouTube] Fallback script search: " + cmd);

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return results;

    char buffer[2048];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        if (line.find("ERROR:") == 0 || line.find("WARNING:") == 0) continue;
        if (std::count(line.begin(), line.end(), '|') >= 4) {
            results.push_back(line);
        }
    }
    pclose(pipe);
    return results;
}

std::string UIManager::resolveYouTubeStreamUrl(const std::string& videoId) {
    if (videoId.empty()) return "";

    {
        std::lock_guard<std::mutex> lock(s_ytStreamMutex);
        auto it = m_ytStreamUrlCache.find(videoId);
        if (it != m_ytStreamUrlCache.end() && !it->second.empty()) {
            Logger::info("[YouTube] Using cached stream URL for: " + videoId);
            return it->second;
        }
    }

    std::string appRoot = AppConfig::instance().getAppRoot();
    if (appRoot.empty()) appRoot = "/mnt/SDCARD/Apps/RomCloud";
    std::string scriptPath = appRoot + "/scripts/youtube_search.sh";
    std::string cmd = "\"" + scriptPath + "\" url \"" + videoId + "\" 360 2>/dev/null";

    Logger::info("[YouTube] Resolving video URL: " + cmd);
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        Logger::error("[YouTube] popen failed for url resolver");
        return "";
    }

    char buffer[4096];
    std::string streamUrl;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        if (line.find("http://") == 0 || line.find("https://") == 0) {
            streamUrl = line;
            break;
        }
    }
    pclose(pipe);

    if (!streamUrl.empty()) {
        std::lock_guard<std::mutex> lock(s_ytStreamMutex);
        m_ytStreamUrlCache[videoId] = streamUrl;
    }
    return streamUrl;
}

void UIManager::preloadYouTubeStreamUrl(const std::string& videoId) {
    if (videoId.empty()) return;
    {
        std::lock_guard<std::mutex> lock(s_ytStreamMutex);
        if (m_ytStreamUrlCache.find(videoId) != m_ytStreamUrlCache.end()) return;
    }
    std::thread([this, videoId]() {
        resolveYouTubeStreamUrl(videoId);
    }).detach();
}

void UIManager::clearThumbnailCache() {
    for (auto& pair : m_ytThumbnails) {
        if (pair.second) {
            SDL_DestroyTexture(pair.second);
        }
    }
    m_ytThumbnails.clear();
}

void UIManager::startThumbnailDownloads(const std::vector<std::string>& videoIds) {
    if (videoIds.empty()) return;
    std::thread([videoIds]() {
        mkdir("/tmp/yt_thumbs", 0777);
        std::string batchCmd;
        int count = 0;
        for (const auto& vid : videoIds) {
            if (vid.empty()) continue;
            std::string outPath = "/tmp/yt_thumbs/" + vid + ".jpg";
            struct stat st;
            if (stat(outPath.c_str(), &st) == 0 && st.st_size > 1500) {
                FILE* f = fopen(outPath.c_str(), "rb");
                bool complete = false;
                if (f) {
                    if (fseek(f, -2, SEEK_END) == 0) {
                        unsigned char eofBytes[2];
                        if (fread(eofBytes, 1, 2, f) == 2 && eofBytes[0] == 0xFF && eofBytes[1] == 0xD9) {
                            complete = true;
                        }
                    }
                    fclose(f);
                }
                if (complete) continue;
            }
            std::string tmpPath = outPath + ".tmp";
            std::string url = "https://i.ytimg.com/vi/" + vid + "/mqdefault.jpg";
            batchCmd += "curl -4 -k -s -L --max-time 6 -o \"" + tmpPath + "\" \"" + url + "\" && mv -f \"" + tmpPath + "\" \"" + outPath + "\" >/dev/null 2>&1 & ";
            count++;
            if (count >= 6) {
                batchCmd += "wait; ";
                count = 0;
            }
        }
        if (count > 0) {
            batchCmd += "wait; ";
        }
        if (!batchCmd.empty()) {
            system(batchCmd.c_str());
        }
    }).detach();
}

void UIManager::triggerYouTubeSearch() {
    if (m_ytSearchQuery.empty()) {
        showToast("Vui lòng nhập từ khóa tìm kiếm", {245, 158, 11, 255}, 2000);
        return;
    }
    if (m_ytIsSearching) return;

    m_ytCurrentPage = 1;
    m_ytLastSearchQuery = m_ytSearchQuery;
    m_ytAllCachedResults.clear();
    clearThumbnailCache();

    m_ytIsSearching = true;
    m_ytSearchFinished = false;
    m_ytErrorMessage.clear();
    std::string query = m_ytSearchQuery;

    std::thread([this, query]() {
        auto results = runYouTubeSearch(query, 1);
        if (!results.empty()) {
            m_ytAllCachedResults = results;
            int pageCount = std::min<int>(6, static_cast<int>(results.size()));
            m_ytSearchResults.assign(results.begin(), results.begin() + pageCount);
        } else {
            m_ytSearchResults.clear();
            m_ytErrorMessage = "Không tìm thấy video nào";
        }
        m_ytSearchSelectedIndex = 0;
        m_ytSearchScrollOffset = 0;
        m_ytSearchFinished = true;
    }).detach();
}

void UIManager::playYouTubeVideo(const std::string& videoId) {
    if (videoId.empty() || m_ytIsLoadingVideo) return;

    // Check if already in memory cache
    auto it = m_ytStreamUrlCache.find(videoId);
    if (it != m_ytStreamUrlCache.end() && !it->second.empty()) {
        m_ytPendingVideoId = videoId;
        m_ytPendingStreamUrl = it->second;
        m_ytVideoReady = true;
        return;
    }

    m_ytIsLoadingVideo = true;
    m_ytVideoReady = false;
    m_ytPendingStreamUrl.clear();
    m_ytPendingVideoId = videoId;

    std::thread([this, videoId]() {
        std::string streamUrl = resolveYouTubeStreamUrl(videoId);
        m_ytPendingStreamUrl = streamUrl;
        m_ytVideoReady = true;
    }).detach();
}

static std::string trimUtf8(const std::string& str) {
    if (str.empty()) return "";
    size_t start = 0;
    while (start < str.size()) {
        unsigned char c = static_cast<unsigned char>(str[start]);
        if (c <= 32) {
            start++;
        } else if (c == 0xC2 && start + 1 < str.size() && static_cast<unsigned char>(str[start + 1]) == 0xA0) {
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

static std::pair<std::string, std::string> wrapUtf8TwoLines(const std::string& text, size_t maxCharsPerLine) {
    std::string cleanText = trimUtf8(text);
    auto chars = TelexHelper::splitUtf8(cleanText);
    if (chars.empty()) return {"", ""};

    // Filter out unrenderable 4-byte emojis and control codes that cause square box glyphs
    std::vector<std::string> cleanChars;
    for (const auto& c : chars) {
        if (c.empty()) continue;
        unsigned char b0 = static_cast<unsigned char>(c[0]);
        if (b0 < 32) continue;
        if (b0 >= 0xF0) continue; // 4-byte emojis (often missing in handheld TTF)
        cleanChars.push_back(c);
    }

    if (cleanChars.size() <= maxCharsPerLine) {
        std::string l1;
        for (const auto& c : cleanChars) l1 += c;
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
    for (size_t i = 0; i < breakIdx; ++i) l1 += cleanChars[i];

    size_t start2 = (breakIdx < cleanChars.size() && cleanChars[breakIdx] == " ") ? breakIdx + 1 : breakIdx;
    std::string l2;
    size_t count2 = cleanChars.size() - start2;
    if (count2 <= maxCharsPerLine) {
        for (size_t i = start2; i < cleanChars.size(); ++i) l2 += cleanChars[i];
    } else {
        size_t end2 = start2 + maxCharsPerLine - 1;
        for (size_t i = start2; i < end2 && i < cleanChars.size(); ++i) l2 += cleanChars[i];
        l2 += "..";
    }

    return {trimUtf8(l1), trimUtf8(l2)};
}

static int getBatteryLevel() {
    static uint32_t lastCheck = 0;
    static int cachedLevel = 100;
    uint32_t now = SDL_GetTicks();
    if (now - lastCheck > 10000 || lastCheck == 0) {
        lastCheck = now;
        FILE* f = fopen("/sys/class/power_supply/battery/capacity", "r");
        if (f) {
            int cap = 100;
            if (fscanf(f, "%d", &cap) == 1 && cap >= 0 && cap <= 100) cachedLevel = cap;
            fclose(f);
        }
    }
    return cachedLevel;
}

static std::string getCurrentTimeString() {
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char buf[16];
    if (t) {
        strftime(buf, sizeof(buf), "%H:%M", t);
    } else {
        strcpy(buf, "12:00");
    }
    return std::string(buf);
}

void UIManager::renderYouTubeSearchState() {
    // ─── TrimUI Stock OS Ambient Teal Theme ───
    drawRect(0, 0, 1024, 768, {7, 24, 33, 255}, true);

    // Header bar
    drawRect(0, 0, 1024, 52, {10, 32, 44, 255}, true);
    drawRect(0, 52, 1024, 1, {20, 54, 70, 255}, true);

    // Green chevron back arrow "<" badge
    int backBadgeX = 44;
    int backBadgeY = 12;
    int backBadgeW = 28;
    int backBadgeH = 28;
    drawRoundedRect(backBadgeX, backBadgeY, backBadgeW, backBadgeH, 6, {0, 200, 83, 255}, true);
    drawText("<", backBadgeX + backBadgeW / 2, backBadgeY + backBadgeH / 2 - 2, {255, 255, 255, 255}, m_fontSmall, true);

    // Search header title
    drawText("Search", 82, 16, {245, 250, 255, 255}, m_fontMedium, false);

    // Current input mode badge (TELEX vs US)
    std::string modeText = m_ytTelexMode ? "TELEX [R1]" : "US [R1]";
    SDL_Color modeBg = m_ytTelexMode ? SDL_Color{0, 200, 83, 255} : SDL_Color{25, 60, 78, 255};
    drawBadge(164, 12, 108, 28, modeText, modeBg, {255, 255, 255, 255});

    // Right-side indicators: Battery & Clock (matching TrimUI Stock OS status bar)
    int bat = getBatteryLevel();
    std::string batStr = std::to_string(bat) + "%";
    drawText(batStr, 910, 18, {190, 215, 228, 255}, m_fontSmall, false);

    // Battery icon
    drawRoundedBorder(962, 19, 24, 13, 3, {190, 215, 228, 255}, 1);
    drawRect(986, 22, 2, 7, {190, 215, 228, 255}, true);
    int bFill = (20 * bat) / 100;
    if (bFill > 0) {
        SDL_Color bColor = (bat <= 20) ? SDL_Color{239, 68, 68, 255} : SDL_Color{0, 200, 83, 255};
        drawRect(964, 21, bFill, 9, bColor, true);
    }

    std::string timeStr = getCurrentTimeString();
    drawText(timeStr, 842, 18, {190, 215, 228, 255}, m_fontSmall, false);

    // ─── Input Field Box (Wide rounded rectangle) ───
    int inX = 46;
    int inY = 62;
    int inW = 932;
    int inH = 88;
    drawRoundedRect(inX, inY, inW, inH, 8, {12, 38, 50, 230}, true);
    drawRoundedBorder(inX, inY, inW, inH, 8, {26, 72, 92, 255}, 1);

    std::string dispQ = m_ytSearchQuery.empty() ? "Nhập từ khóa tìm kiếm..." : (m_ytSearchQuery + " _");
    SDL_Color qCol = m_ytSearchQuery.empty() ? SDL_Color{75, 115, 135, 255} : SDL_Color{255, 255, 255, 255};
    drawText(dispQ, inX + 22, inY + 28, qCol, m_fontLarge, false);

    // ─── TrimUI Stock OS 5-Row Virtual Keyboard Grid ───
    static const char* lowerRows[] = {
        "1234567890",
        "qwertyuiop",
        "asdfghjkl-",
        "zxcvbnm()/"
    };
    static const char* upperRows[] = {
        "1234567890",
        "QWERTYUIOP",
        "ASDFGHJKL-",
        "ZXCVBNM()/"
    };

    int kbStartX = 46;
    int kbStartY = 162;
    int cellW = 86;
    int cellH = 92;
    int gap = 8;

    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 10; col++) {
            int cx = kbStartX + col * (cellW + gap);
            int cy = kbStartY + row * (cellH + gap);
            bool isSel = (m_ytKbRow == row && m_ytKbCol == col);

            char ch = m_ytKbShift ? upperRows[row][col] : lowerRows[row][col];
            std::string label(1, ch);

            if (isSel) {
                // Focused: Vibrant TrimUI Emerald Green
                drawRoundedRect(cx, cy, cellW, cellH, 6, {0, 200, 83, 255}, true);
                drawRoundedBorder(cx, cy, cellW, cellH, 6, {130, 255, 180, 255}, 2);
                drawText(label, cx + cellW / 2, cy + cellH / 2 - 2, {255, 255, 255, 255}, m_fontLarge, true);
            } else {
                // Inactive: Dark translucent teal keycap
                drawRoundedRect(cx, cy, cellW, cellH, 6, {14, 36, 47, 210}, true);
                drawRoundedBorder(cx, cy, cellW, cellH, 6, {22, 58, 74, 255}, 1);
                drawText(label, cx + cellW / 2, cy + cellH / 2 - 2, {210, 228, 238, 255}, m_fontLarge, true);
            }
        }
    }

    // Row 4: 5 Action Keys (Width = 180 each, spanning 2 columns)
    int actW = 180;
    int row4Y = kbStartY + 4 * (cellH + gap);

    struct ActionKey {
        std::string label;
        std::string badge;
    };
    ActionKey actKeys[5] = {
        {m_ytKbShift ? "⇧ (HOA)" : "⇧ (thường)", "L"},
        {m_ytTelexMode ? "TELEX" : "abc", "R"},
        {"␣ Cách", "X"},
        {"⌫ Xóa", "Y"},
        {"OK Tìm", "START"}
    };

    for (int i = 0; i < 5; i++) {
        int cx = kbStartX + i * (actW + gap);
        bool isSel = (m_ytKbRow == 4 && (m_ytKbCol / 2) == i);

        if (isSel) {
            drawRoundedRect(cx, row4Y, actW, cellH, 6, {0, 200, 83, 255}, true);
            drawRoundedBorder(cx, row4Y, actW, cellH, 6, {130, 255, 180, 255}, 2);
            drawText(actKeys[i].label, cx + actW / 2 - 18, row4Y + cellH / 2 - 2, {255, 255, 255, 255}, m_fontMedium, true);
            drawBadge(cx + actW - 46, row4Y + cellH / 2 - 13, 38, 26, actKeys[i].badge, {20, 100, 50, 255}, {255, 255, 255, 255});
        } else {
            drawRoundedRect(cx, row4Y, actW, cellH, 6, {14, 36, 47, 210}, true);
            drawRoundedBorder(cx, row4Y, actW, cellH, 6, {22, 58, 74, 255}, 1);
            drawText(actKeys[i].label, cx + actW / 2 - 18, row4Y + cellH / 2 - 2, {210, 228, 238, 255}, m_fontMedium, true);
            drawBadge(cx + actW - 46, row4Y + cellH / 2 - 13, 38, 26, actKeys[i].badge, {24, 60, 76, 255}, {180, 210, 225, 255});
        }
    }

    // Bottom Bar (TrimUI Stock style: [A] OK badge on bottom left, hints on right)
    drawRect(0, 715, 1024, 53, {9, 24, 32, 255}, true);
    drawBadge(46, 727, 82, 30, "[A] OK", {0, 200, 83, 255}, {255, 255, 255, 255});
    drawText("[X] Cách   [Y] Xóa   [L1] Viết hoa   [R1] Telex/US   [START] Tìm kiếm   [B] Trở về",
        560, 732, {160, 185, 200, 255}, m_fontSmall, true);

    // Searching modal overlay
    if (m_ytIsSearching) {
        drawRect(0, 0, 1024, 768, {0, 0, 0, 200}, true);
        drawRoundedRect(280, 290, 464, 140, 10, SDL_Color{14, 38, 50, 255}, true);
        drawRoundedBorder(280, 290, 464, 140, 10, {0, 200, 83, 255}, 2);
        drawText("ĐANG TÌM KIẾM...", 512, 325, {0, 200, 83, 255}, m_fontMedium, true);
        std::string qText = "\"" + m_ytSearchQuery + "\"";
        if (qText.length() > 36) qText = qText.substr(0, 33) + "...\"";
        drawText(qText, 512, 370, {240, 240, 240, 255}, m_fontSmall, true);
    }
}

void UIManager::renderYouTubeResultsState() {
    // RAM Management: clean old thumbnails when switching pages or if cache exceeds 24
    if (m_ytThumbnails.size() > 24) {
        std::unordered_set<std::string> currentVisible;
        for (const auto& item : m_ytSearchResults) {
            size_t p = item.find('|');
            if (p != std::string::npos) currentVisible.insert(item.substr(0, p));
        }
        for (auto it = m_ytThumbnails.begin(); it != m_ytThumbnails.end(); ) {
            if (currentVisible.find(it->first) == currentVisible.end()) {
                if (it->second) SDL_DestroyTexture(it->second);
                it = m_ytThumbnails.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Pure dark background matching Image 1
    drawRect(0, 0, 1024, 768, {18, 18, 18, 255}, true);

    // Top Bar: YouTube Logo + Search Query on top left
    int logoH = 45;
    int logoW = 45;
    SDL_Texture* ytLogo = nullptr;
    auto itLogo = m_gridIconCache.find("YOUTUBE.png");
    if (itLogo != m_gridIconCache.end()) {
        ytLogo = itLogo->second;
    } else {
        std::string iconPath = AppConfig::instance().getAssetsDir() + "/apps_icons/YOUTUBE.png";
        SDL_Surface* surf = IMG_Load(iconPath.c_str());
        if (surf) {
            ytLogo = SDL_CreateTextureFromSurface(m_renderer, surf);
            SDL_FreeSurface(surf);
            m_gridIconCache["YOUTUBE.png"] = ytLogo;
        }
    }
    int textStartX = 28;
    if (ytLogo) {
        int texW = 0, texH = 0;
        SDL_QueryTexture(ytLogo, nullptr, nullptr, &texW, &texH);
        if (texH > 0) {
            logoW = (texW * logoH) / texH;
        }
        int slx = PlatformInfo::instance().scaleX(26);
        int sly = PlatformInfo::instance().scaleY(12);
        int slw = PlatformInfo::instance().scaleW(logoW);
        int slh = PlatformInfo::instance().scaleH(logoH);
        SDL_Rect dst = {slx, sly, slw, slh};
        SDL_RenderCopy(m_renderer, ytLogo, nullptr, &dst);
        textStartX = 26 + logoW + 10;
    }

    std::string queryDisplay = m_ytSearchQuery.empty() ? "" : (": " + m_ytSearchQuery);
    if (!queryDisplay.empty()) {
        if (queryDisplay.length() > 38) queryDisplay = queryDisplay.substr(0, 35) + "...";
        drawText(queryDisplay, textStartX, 22, {245, 245, 245, 255}, m_fontMedium, false);
    }

    // Top Right: Battery Indicator & Clock matching Image 1
    int bat = getBatteryLevel();
    std::string batStr = std::to_string(bat) + "%";
    drawText(batStr, 910, 20, {180, 185, 195, 255}, m_fontSmall, false);
    drawRoundedBorder(962, 21, 24, 13, 3, {180, 185, 195, 255}, 1);
    drawRect(986, 24, 2, 7, {180, 185, 195, 255}, true);
    int bFill = (20 * bat) / 100;
    if (bFill > 0) {
        SDL_Color bColor = (bat <= 20) ? SDL_Color{239, 68, 68, 255} : SDL_Color{34, 197, 94, 255};
        drawRect(964, 23, bFill, 9, bColor, true);
    }

    std::string timeStr = getCurrentTimeString();
    drawText(timeStr, 842, 20, {180, 185, 195, 255}, m_fontSmall, false);

    // 3 columns x 2 rows = 6 cards per page
    const int itemsPerPage = 6;
    const int cols = 3;
    int totalResults = static_cast<int>(m_ytSearchResults.size());
    int pageStartIndex = (m_ytSearchSelectedIndex / itemsPerPage) * itemsPerPage;

    int cardW = 316;
    int cardH = 295;
    int gapX = 12;
    int gapY = 14;
    int startX = 26;
    int startY = 62;

    int pad = 8;
    int thumbW = cardW - pad * 2;
    int thumbH = (thumbW * 9) / 16;  // ~168 (16:9 ratio)

    for (int i = 0; i < itemsPerPage; i++) {
        int idx = pageStartIndex + i;
        if (idx >= totalResults) break;

        int row = i / cols;
        int col = i % cols;
        int cx = startX + col * (cardW + gapX);
        int cy = startY + row * (cardH + gapY);
        bool isSelected = (idx == m_ytSearchSelectedIndex);

        // Card Background
        SDL_Color cardBg = isSelected ? SDL_Color{34, 34, 34, 255} : SDL_Color{24, 24, 24, 255};
        drawRoundedRect(cx, cy, cardW, cardH, 8, cardBg, true);

        // Selected Card Focus: Crisp White Border
        if (isSelected) {
            drawRoundedBorder(cx, cy, cardW, cardH, 8, {255, 255, 255, 255}, 2);
        }

        auto parts = split(m_ytSearchResults[idx], '|');
        std::string vid = !parts.empty() ? parts[0] : "";

        // Thumbnail (16:9 with rounded border)
        int thumbX = cx + pad;
        int thumbY = cy + pad;

        if (!vid.empty()) {
            if (m_ytThumbnails.find(vid) == m_ytThumbnails.end()) {
                std::string thumbPath = "/tmp/yt_thumbs/" + vid + ".jpg";
                struct stat st;
                if (stat(thumbPath.c_str(), &st) == 0 && st.st_size > 1500) {
                    bool complete = false;
                    FILE* f = fopen(thumbPath.c_str(), "rb");
                    if (f) {
                        if (fseek(f, -2, SEEK_END) == 0) {
                            unsigned char eofBytes[2];
                            if (fread(eofBytes, 1, 2, f) == 2 && eofBytes[0] == 0xFF && eofBytes[1] == 0xD9) {
                                complete = true;
                            }
                        }
                        fclose(f);
                    }
                    if (complete) {
                        SDL_Surface* surf = IMG_Load(thumbPath.c_str());
                        if (surf) {
                            SDL_Texture* tex = SDL_CreateTextureFromSurface(m_renderer, surf);
                            SDL_FreeSurface(surf);
                            if (tex) m_ytThumbnails[vid] = tex;
                        }
                    }
                }
            }
        }

        if (!vid.empty() && m_ytThumbnails.find(vid) != m_ytThumbnails.end() && m_ytThumbnails[vid]) {
            int stx = PlatformInfo::instance().scaleX(thumbX);
            int sty = PlatformInfo::instance().scaleY(thumbY);
            int stw = PlatformInfo::instance().scaleW(thumbW);
            int sth = PlatformInfo::instance().scaleH(thumbH);
            SDL_Rect dstRect = {stx, sty, stw, sth};
            SDL_RenderCopy(m_renderer, m_ytThumbnails[vid], nullptr, &dstRect);
            drawRoundedBorder(thumbX, thumbY, thumbW, thumbH, 6, {45, 45, 45, 180}, 1);
        } else {
            drawRoundedRect(thumbX, thumbY, thumbW, thumbH, 6, SDL_Color{32, 32, 32, 255}, true);
            drawText("YouTube", thumbX + thumbW / 2, thumbY + thumbH / 2 - 6,
                SDL_Color{229, 9, 20, 255}, m_fontSmall, true);
        }

        // Duration pill bottom-right of thumbnail (Image 1 style)
        std::string durStr = parts.size() >= 3 ? formatDuration(parts[2]) : "";
        if (!durStr.empty()) {
            int pillW = 54;
            int pillH = 20;
            int pillX = thumbX + thumbW - pillW - 6;
            int pillY = thumbY + thumbH - pillH - 6;
            drawRoundedRect(pillX, pillY, pillW, pillH, 4, SDL_Color{0, 0, 0, 215}, true);
            drawText(durStr, pillX + pillW / 2, pillY + 2, {255, 255, 255, 255}, m_fontSmall, true);
        }

        // Info area below thumbnail: infoX is EXACTLY thumbX (strict left-alignment)
        int infoX = thumbX;
        int infoY = thumbY + thumbH + 8;

        // Title (UTF-8 safe wrapping & trimmed, left-aligned)
        std::string title = parts.size() >= 2 ? parts[1] : "";
        auto lines = wrapUtf8TwoLines(title, 24);

        SDL_Color titleCol = {255, 255, 255, 255};
        drawText(lines.first, infoX, infoY, titleCol, m_fontSmall, false);
        if (!lines.second.empty()) {
            drawText(lines.second, infoX, infoY + 22, titleCol, m_fontSmall, false);
        }

        // Channel & Views on a SINGLE line matching Image 1: [Channel] • [Views]
        std::string uploader = parts.size() >= 4 ? trimUtf8(parts[3]) : "";
        if (uploader.length() > 18) uploader = uploader.substr(0, 16) + "..";
        std::string viewStr = parts.size() >= 5 ? formatViews(parts[4]) : "";
        std::string metaLine = uploader;
        if (!viewStr.empty()) {
            if (!metaLine.empty()) metaLine += " • ";
            metaLine += viewStr;
        }
        int metaY = infoY + (lines.second.empty() ? 26 : 48);
        drawText(metaLine, infoX, metaY, {156, 163, 175, 255}, m_fontSmall, false);
    }

    // Footer matching Image 1
    drawRect(0, 715, 1024, 53, {18, 18, 18, 255}, true);
    drawRect(0, 715, 1024, 1, {35, 35, 35, 255}, true);

    char pageInfo[64];
    int maxPage = std::max(1, (static_cast<int>(m_ytAllCachedResults.size()) + itemsPerPage - 1) / itemsPerPage);
    snprintf(pageInfo, sizeof(pageInfo), "Page %d/%d (%d videos)", m_ytCurrentPage, maxPage, totalResults);
    drawText(pageInfo, 32, 730, {156, 163, 175, 255}, m_fontSmall, false);

    drawText("[A] Play   [X] Search   [L1/R1] Page   [B] Exit",
        992, 730, {210, 215, 225, 255}, m_fontSmall, true);

    // Resolving stream overlay (Borderless)
    if (m_ytIsLoadingVideo) {
        drawRect(0, 0, 1024, 768, {0, 0, 0, 210}, true);
        drawRoundedRect(272, 285, 480, 150, 12, SDL_Color{28, 28, 28, 255}, true);
        drawText("ĐANG TẢI VIDEO...", 512, 325, {255, 255, 255, 255}, m_fontMedium, true);
        drawText("Đang kết nối luồng phát...", 512, 372, {180, 190, 205, 255}, m_fontSmall, true);
    }
}

} // namespace RomCloud

