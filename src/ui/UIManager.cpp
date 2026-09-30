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
#include "../database/DatabaseManager.h"
#include "../database/RomIndexer.h"
#include "../ota/UpdateManager.h"
#include "../app/Application.h"
#include "../backup/BackupManager.h"
#include "../sync/UploadManager.h"
#include "../iptv/IPTVManager.h"
#include "../localsend/LocalSendManager.h"
#include "BoxartScraper.h"
#include "UiStrings.h"
#include "UiTheme.h"
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
#include <cctype>
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
        {"localsend", "LOCAL SEND", "LOCALSEND.png", "Chia se P2P trong LAN"},
        {"explorer", "FILE EXPLORER", "FOLDER.png", "Duyet file SD"},
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
    // Ưu tiên NotoSans-Regular của app (full TV), font hệ thống fallback sau.
    const char* fallbackFonts[] = {
        "/mnt/SDCARD/Apps/RomCloud/assets/fonts/NotoSans-Regular.ttf",
        "assets/fonts/NotoSans-Regular.ttf",
        "/rom/usr/trimui/res/full.ttf",
        "/usr/trimui/res/full.ttf",
        "/usr/trimui/res/regular.ttf",
        "/mnt/SDCARD/Themes/TRIMUI YaHei/msyh.ttf",
        "/mnt/SDCARD/Apps/RomCloud/assets/fonts/NotoSansTC.ttf",
        fontPath.c_str(),
        "/mnt/SDCARD/Apps/RomCloud/assets/fonts/font.ttf",
        "assets/fonts/font.ttf"
    };

    for (const char* path : fallbackFonts) {
        if (!m_fontTitle) m_fontTitle = TTF_OpenFont(path, 42);
        if (!m_fontLarge) m_fontLarge = TTF_OpenFont(path, 36);
        if (!m_fontMedium) m_fontMedium = TTF_OpenFont(path, 30);
        if (!m_fontSmall) m_fontSmall = TTF_OpenFont(path, 24);
        if (m_fontTitle && m_fontLarge && m_fontMedium && m_fontSmall) {
            Logger::info(std::string("Loaded TTF font from: ") + path + " (Sizes: 42, 36, 30, 24)");
            break;
        }
    }

    if (!m_fontLarge || !m_fontMedium || !m_fontSmall) {
        Logger::warn("Could not load desired font point sizes.");
    }

    CoverManager::instance().init(m_renderer);
    m_ui.bind(m_renderer, m_fontSmall, m_fontMedium, m_fontLarge,
              AppConfig::instance().getAssetsDir());
    refreshSystems();

    // Auto-check for OTA updates in background on launch
    UpdateManager::instance().checkForUpdatesAsync([this](bool hasUpdate, const UpdateInfo& info) {
        if (hasUpdate) {
            showToast(std::string(UiStrings::TOAST_NEW_OTA_PREFIX) + info.remoteVersion + "!", {34, 197, 94, 255}, 6000);
        }
    });

    // LocalSend: wire callback từ background thread (HTTP) sang UI state.
    // Modal overlay toàn màn hình — switch sang LOCALSEND_INCOMING bất kể
    // user đang ở state nào (Game List, IPTV, Settings...).
    LocalSendManager::instance().setOnUserPrompt([this](const LsUploadRequest& req) {
        m_localSendCurrentPrompt = req;
        m_localSendPendingSessionId = req.sessionId;
        // Reset folder picker về mặc định: Downloads
        m_localSendFolderCurrentPath = "/mnt/SDCARD/Downloads";
        m_localSendFolderLoaded = false;
        m_localSendFolderSelected = 0;
        m_localSendFolderFocus = 1;  // mặc định focus nút Chốt ở panel phải
        m_localSendIncomingMode = 0;  // bắt đầu ở screen picker
        m_localSendIncomingSavePath = "";
        // Tự tạo Downloads nếu chưa có
        FileSystemManager::instance().createDirectoryRecursive(
            "/mnt/SDCARD/Downloads");
        setState(UIState::LOCALSEND_INCOMING);
    });

    return true;
}

void UIManager::shutdown() {
    CoverManager::instance().shutdown();
    m_ui.unbind();

    // P2-2: icon/logo/button/grid do m_ui.images() giu (unbind da clear).
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
    } else if (state == UIState::YOUTUBE_SEARCH) {
        loadYouTubeHistory();
        m_ytFocusInTags = false;
        m_ytSelectedTagIndex = 0;
    } else if (state == UIState::CONFIRM_DELETE) {
        openConfirmDeleteDialog();
    } else if (state == UIState::CONFIRM_BATCH_DELETE) {
        openConfirmBatchDeleteDialog();
    }
}

void UIManager::showToast(const std::string& message, SDL_Color color, uint32_t durationMs) {
    uint32_t packed = ((uint32_t)color.r << 24) | ((uint32_t)color.g << 16) |
                      ((uint32_t)color.b << 8) | (uint32_t)color.a;
    m_dialogs.toastMsg(message, packed, durationMs);
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
    if (DriveSyncEngine::instance().isSyncing() || isIndexing()) {
        showToast(UiStrings::TOAST_SYNCING_DRIVE, {245, 158, 11, 255});
        return;
    }
    showToast(UiStrings::TOAST_SCANNING_SD, {0, 180, 216, 255}, 2000);

    // P0-3: dua tien trinh quet SD len ProgressDialog dung chung (thay vi chi toast).
    m_dialogs.progress.open("Dang quet the SD...", false);
    auto systems = DatabaseManager::instance().getSystems(false);
    uint64_t totalSys = systems.empty() ? 1 : systems.size();
    m_indexTask.run([this, totalSys](TaskProgress& p) {
        p.total = totalSys;
        p.done = 0;
        std::string romsDir = AppConfig::instance().getRomsDir();
        auto all = DatabaseManager::instance().getSystems(false);
        uint64_t idx = 0;
        for (const auto& sys : all) {
            if (p.cancel.load()) break;
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

    // P0-3: poll tien trinh quet SD tu BackgroundTask -> ProgressDialog dung chung.
    // Dat truoc early-return sync de dialog tu dong dong khi sync noi tiep.
    if (isIndexing()) {
        const auto& tp = m_indexTask.progress();
        m_dialogs.progress.update(tp.done.load(), tp.total.load() == 0 ? 1 : tp.total.load());
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%llu / %llu he may",
                      (unsigned long long)tp.done.load(),
                      (unsigned long long)tp.total.load());
        m_dialogs.progress.detail = buf;
    } else if (m_dialogs.progress.visible &&
               m_dialogs.progress.title == "Dang quet the SD...") {
        m_dialogs.progress.close();
    }
    // P2-1: Explorer task (copy/xoa) -> ProgressDialog dung chung + forward toast/confirm.
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
            } else if (input.isButtonJustPressed(Button::A)) {
                // Handle menu selection based on id
                std::string selectedId = m_gridMenuItems[m_selectedMenuIndex].id;

                if (selectedId == "games") {
                    setState(UIState::SYSTEM_SELECT);
                    triggerManualSync();
                } else if (selectedId == "iptv") {
                    if (IPTVManager::instance().playlistCount() == 0) {
                        IPTVManager::instance().loadPlaylists();
                    }
                    m_selectedPlaylistIndex = 0;
                    m_playlistScrollOffset  = 0;
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
                    setState(UIState::YOUTUBE_SEARCH);
                } else if (selectedId == "localsend") {
                    LocalSendManager::instance().start();
                    LocalSendManager::instance().refreshDiscovery();
                    m_localSendSelectedDevice = 0;
                    m_localSendFolderSelected = 0;
                    setState(UIState::LOCALSEND_HOME);
                } else if (selectedId == "explorer") {
                    m_explorer.open("/mnt/SDCARD");
                    m_explorerScroll = 0;
                    setState(UIState::FILE_EXPLORER);
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
            } else if (input.isButtonJustPressed(Button::B) || input.isButtonJustPressed(Button::X)) {
                m_dialogs.confirm.cancel();
                setState(UIState::GAME_LIST);
            }
            break;
        }

        case UIState::CONFIRM_BATCH_DELETE: {
            if (input.isButtonJustPressed(Button::A)) {
                m_dialogs.confirm.confirm();
                setState(UIState::GAME_LIST);
            } else if (input.isButtonJustPressed(Button::B) || input.isButtonJustPressed(Button::X)) {
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
                    VkAction act = VirtualKeyboard::pressA(m_searchVk, [this](const char* m) {
                        showToast(m_searchVk.telexMode ? "Che do: TELEX" : "Che do: TIENG ANH (US)", {0, 180, 216, 255}, 1200);
                    }, true);
                    if (act == VkAction::Commit) {
                        if (!m_searchResults.empty()) {
                            m_searchVk.inResults = true;
                            m_searchSelectedIndex = 0;
                            m_searchScrollOffset = 0;
                        }
                    } else {
                        // Auto-search as user types
                        if (m_searchVk.query.length() >= 2) {
                            m_searchResults = DatabaseManager::instance().searchAllGames(m_searchVk.query, 50);
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
                        m_searchResults = DatabaseManager::instance().searchAllGames(m_searchVk.query, 50);
                    } else {
                        m_searchResults.clear();
                    }
                    m_searchSelectedIndex = 0;
                    m_searchScrollOffset = 0;
                } else if (input.isButtonJustPressed(Button::L1)) {
                    m_searchVk.shift = !m_searchVk.shift;
                } else if (input.isButtonJustPressed(Button::R1)) {
                    m_searchVk.telexMode = !m_searchVk.telexMode;
                    showToast(m_searchVk.telexMode ? "Che do: TELEX" : "Che do: TIENG ANH (US)", {0, 180, 216, 255}, 1200);
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
                                    m_searchResults = DatabaseManager::instance().searchAllGames(m_searchVk.query, 50);
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
                                m_searchResults = DatabaseManager::instance().searchAllGames(m_searchVk.query, 50);
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
                if (m_searchVk.inResults) {
                    m_searchVk.inResults = false; // ve ban phim nhu IPTV
                } else if (!m_searchVk.query.empty()) {
                    // Unified nhu YouTube: con chu thi xoa dan, het chu thi thoat.
                    VirtualKeyboard::backspace(m_searchVk);
                    if (m_searchVk.query.length() >= 2) {
                        m_searchResults = DatabaseManager::instance().searchAllGames(m_searchVk.query, 50);
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

        case UIState::IPTV_PLAYLIST_SELECT: {
            const auto& playlists = IPTVManager::instance().getPlaylists();
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
                    m_selectedPlaylistIndex = std::min(plCount - 1, m_selectedPlaylistIndex + 1);
                    if (m_selectedPlaylistIndex >= m_playlistScrollOffset + visibleItems)
                        m_playlistScrollOffset = m_selectedPlaylistIndex - visibleItems + 1;
                }
            } else if (input.isButtonJustPressed(Button::A)) {
                if (plCount > 0 && m_selectedPlaylistIndex < plCount) {
                    const Playlist* pl = IPTVManager::instance().getPlaylist(
                        static_cast<size_t>(m_selectedPlaylistIndex));
                    if (pl) {
                        showToast("Đang mở: " + pl->name + " (" +
                                  std::to_string(pl->channelCount()) + " kênh)...",
                                  {0, 180, 216, 255}, 1500);
                    }
                    m_activePlaylistIndex = m_selectedPlaylistIndex;
                    m_iptvSelectedGroup.clear();
                    m_selectedIPTVChannelIndex = 0;
                    m_iptvScrollOffset = 0;
                    setState(UIState::IPTV_LIST);
                }
            } else if (input.isButtonJustPressed(Button::Y)) {
                // Manual refresh playlist co URL nguon
                if (plCount > 0 && m_selectedPlaylistIndex < plCount) {
                    const Playlist* pl = IPTVManager::instance().getPlaylist(
                        static_cast<size_t>(m_selectedPlaylistIndex));
                    if (pl) {
                        const auto& sources = IPTVManager::instance().getSources();
                        bool hasUrl = false;
                        for (const auto& s : sources)
                            if (s.filename == pl->sourceFile && s.type == "url") { hasUrl = true; break; }

                        if (hasUrl) {
                            showToast("Đang cập nhật playlist: " + pl->name + "...",
                                      {250, 204, 21, 255}, 3000);
                            render();
                            std::string err;
                            if (IPTVManager::instance().refreshPlaylistFromUrl(pl->sourceFile, err)) {
                                std::string ts = IPTVManager::instance().getLastRefreshedStr(pl->sourceFile);
                                showToast("Cập nhật thành công! (" + ts + ")", {34, 197, 94, 255}, 3000);
                            } else {
                                showToast("Cập nhật thất bại: " + err, {239, 68, 68, 255}, 4000);
                            }
                        } else {
                            showToast("Playlist này không có URL nguồn (chỉ có thể refresh playlist được thêm qua URL)",
                                      {148, 163, 184, 255}, 3000);
                        }
                    }
                }
            } else if (input.isButtonJustPressed(Button::X)) {
                // Thay doi interval refresh: 6h -> 12h -> 24h -> 72h -> 0(tat) -> 6h
                if (plCount > 0 && m_selectedPlaylistIndex < plCount) {
                    const Playlist* pl = IPTVManager::instance().getPlaylist(
                        static_cast<size_t>(m_selectedPlaylistIndex));
                    if (pl) {
                        const auto& sources = IPTVManager::instance().getSources();
                        for (const auto& s : sources) {
                            if (s.filename == pl->sourceFile && s.type == "url") {
                                int cur = s.refreshIntervalHours;
                                int next = (cur == 0) ? 6 : (cur == 6) ? 12 : (cur == 12) ? 24 : (cur == 24) ? 72 : 0;
                                IPTVManager::instance().setRefreshInterval(pl->sourceFile, next);
                                std::string msg = (next == 0)
                                    ? "Đã tắt tự động refresh"
                                    : "Tự động refresh mỗi " + std::to_string(next) + " giờ";
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
            // ------- Build filtered channel list -------
            std::vector<IPTVChannel> allChannels;
            if (m_iptvShowFavoritesOnly) {
                allChannels = IPTVManager::instance().getFavoriteChannels();
            } else if (m_activePlaylistIndex >= 0) {
                const Playlist* pl = IPTVManager::instance().getPlaylist(
                    static_cast<size_t>(m_activePlaylistIndex));
                if (pl) {
                    for (const auto& item : pl->channels)
                        allChannels.push_back(IPTVChannel::fromItem(item, pl->name, pl->sourceFile));
                }
            } else {
                allChannels = IPTVManager::instance().getChannels();
            }

            // Build group list
            std::vector<std::string> groups;
            groups.push_back("");
            {
                std::unordered_set<std::string> seen;
                for (const auto& ch : allChannels) {
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
                for (const auto& ch : allChannels)
                    if (ch.group == m_iptvSelectedGroup) channels.push_back(ch);
            }

            int channelCount = static_cast<int>(channels.size());
            int visibleItems = 8;

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
                    // Auto-scroll group bar to keep selected visible
                    int gx = 8 - m_iptvGroupBarOffset;
                    for (int gi = 0; gi < static_cast<int>(groups.size()); gi++) {
                        std::string label = groups[gi].empty() ? "Tất cả" : groups[gi];
                        label = truncateToWidth(label, m_fontSmall, UiTheme::PILL_MAX_W - UiTheme::PILL_PAD_X * 2);
                        int gW = pillWidth(label, m_fontSmall);
                        if (gi == std::distance(groups.begin(), it)) break;
                        gx += gW + UiTheme::PILL_GAP;
                    }
                    if (gx < 10) m_iptvGroupBarOffset = std::max(0, m_iptvGroupBarOffset + gx - 10);
                }
            } else if (input.isButtonJustPressed(Button::RIGHT)) {
                auto it = std::find(groups.begin(), groups.end(), m_iptvSelectedGroup);
                if (it != groups.end()) {
                    ++it;
                    if (it != groups.end()) {
                        m_iptvSelectedGroup = *it;
                        m_selectedIPTVChannelIndex = 0;
                        m_iptvScrollOffset = 0;
                        // Auto-scroll group bar to keep selected visible
                        int gx = 8 - m_iptvGroupBarOffset;
                        for (int gi = 0; gi < static_cast<int>(groups.size()); gi++) {
                            std::string label = groups[gi].empty() ? "Tất cả" : groups[gi];
                            label = truncateToWidth(label, m_fontSmall, UiTheme::PILL_MAX_W - UiTheme::PILL_PAD_X * 2);
                            int gW = pillWidth(label, m_fontSmall);
                            if (gi == std::distance(groups.begin(), it)) break;
                            gx += gW + UiTheme::PILL_GAP;
                        }
                        if (gx + 150 > 1010) m_iptvGroupBarOffset += (gx + 150) - 1010;
                    }
                }
            } else if (input.isButtonJustPressed(Button::L1)) {
                if (channelCount > 0) {
                    int step = std::max(1, visibleItems);
                    int lastPageStart = std::max(0, channelCount - step);
                    if (m_selectedIPTVChannelIndex > 0) {
                        m_selectedIPTVChannelIndex = std::max(0, m_selectedIPTVChannelIndex - step);
                    } else {
                        // Loop: tu trang dau -> trang cuoi
                        m_selectedIPTVChannelIndex = lastPageStart;
                    }
                    m_iptvScrollOffset = std::max(0, m_selectedIPTVChannelIndex - (step - 1));
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
                        m_iptvScrollOffset = std::min(lastPageStart,
                                                     m_selectedIPTVChannelIndex - visibleItems + 1);
                }
            } else if (input.isButtonJustPressed(Button::A)) {
                if (channelCount > 0 && m_selectedIPTVChannelIndex >= 0 && m_selectedIPTVChannelIndex < channelCount) {
                    showToast("Đang kết nối: " + channels[m_selectedIPTVChannelIndex].name + "...", {0, 180, 216, 255}, 3000);
                    render();
                    if (!IPTVManager::instance().playChannel(channels[m_selectedIPTVChannelIndex], m_selectedIPTVChannelIndex, channels)) {
                        showToast("Không thể phát video (Lỗi kết nối hoặc player)", {239, 68, 68, 255}, 4000);
                    }
                }
            } else if (input.isButtonJustPressed(Button::B)) {
                IPTVManager::instance().stop();
                if (!m_iptvSelectedGroup.empty()) {
                    m_iptvSelectedGroup.clear();
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
                if (channelCount > 0 && m_selectedIPTVChannelIndex >= 0 && m_selectedIPTVChannelIndex < channelCount) {
                    std::string chanName = channels[m_selectedIPTVChannelIndex].name;
                    bool wasFav = IPTVManager::instance().isFavorite(chanName);
                    IPTVManager::instance().toggleFavorite(chanName);
                    showToast(wasFav ? "☆ Đã xóa khỏi yêu thích: " + chanName
                                     : "★ Đã thêm vào yêu thích: " + chanName,
                              wasFav ? SDL_Color{148, 163, 184, 255} : SDL_Color{250, 204, 21, 255}, 2000);
                }
            } else if (input.isButtonJustPressed(Button::Y)) {
                m_iptvShowFavoritesOnly = !m_iptvShowFavoritesOnly;
                m_iptvSelectedGroup.clear();
                m_selectedIPTVChannelIndex = 0;
                m_iptvScrollOffset = 0;
                showToast(m_iptvShowFavoritesOnly ? "★ Đang lọc: Kênh Yêu Thích" : "Đang lọc: Tất cả kênh", {0, 180, 216, 255}, 2000);
            } else if (input.isButtonJustPressed(Button::SELECT) || input.isButtonJustPressed(Button::START)) {
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
            if (!m_iptvVk.inResults) {
                // Unified keyboard (P0-2): giong YouTube — move/pressA stride2.
                if (input.isButtonJustPressed(Button::UP)) {
                    VirtualKeyboard::move(m_iptvVk, -1, 0, true);
                } else if (input.isButtonJustPressed(Button::DOWN)) {
                    if (m_iptvVk.row == 4) {
                        if (!m_iptvSearchResults.empty()) {
                            m_iptvVk.inResults = true;
                            m_iptvSearchSelectedIndex = 0;
                            m_iptvSearchScrollOffset = 0;
                        }
                    } else {
                        VirtualKeyboard::move(m_iptvVk, 1, 0, true);
                    }
                } else if (input.isButtonJustPressed(Button::LEFT)) {
                    VirtualKeyboard::move(m_iptvVk, 0, -1, true);
                } else if (input.isButtonJustPressed(Button::RIGHT)) {
                    VirtualKeyboard::move(m_iptvVk, 0, 1, true);
                } else if (input.isButtonJustPressed(Button::A)) {
                    VkAction act = VirtualKeyboard::pressA(m_iptvVk, [this](const char* m) {
                        showToast(m_iptvVk.telexMode ? "Che do: TELEX" : "Che do: TIENG ANH (US)", {0, 180, 216, 255}, 1200);
                    }, true);
                    if (act == VkAction::Commit) {
                        if (!m_iptvSearchResults.empty()) {
                            m_iptvVk.inResults = true;
                            m_iptvSearchSelectedIndex = 0;
                            m_iptvSearchScrollOffset = 0;
                        }
                    } else {
                        if (!m_iptvVk.query.empty()) {
                            m_iptvSearchResults = IPTVManager::instance().search(m_iptvVk.query);
                        } else {
                            m_iptvSearchResults.clear();
                        }
                        m_iptvSearchSelectedIndex = 0;
                        m_iptvSearchScrollOffset = 0;
                    }
                } else if (input.isButtonJustPressed(Button::X)) {
                    m_iptvVk.query.clear();
                    m_iptvSearchResults.clear();
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
                    showToast(m_iptvVk.telexMode ? "Che do: TELEX" : "Che do: TIENG ANH (US)", {0, 180, 216, 255}, 1200);
                } else if (input.isButtonJustPressed(Button::START)) {
                    if (!m_iptvSearchResults.empty()) {
                        m_iptvVk.inResults = true;
                        m_iptvSearchSelectedIndex = 0;
                        m_iptvSearchScrollOffset = 0;
                    }
                } else if (input.isButtonJustPressed(Button::B)) {
                    // Unified nhu YouTube: con chu thi xoa dan, het chu thi ve list.
                    if (!m_iptvVk.query.empty()) {
                        VirtualKeyboard::backspace(m_iptvVk);
                        if (!m_iptvVk.query.empty()) {
                            m_iptvSearchResults = IPTVManager::instance().search(m_iptvVk.query);
                        } else {
                            m_iptvSearchResults.clear();
                        }
                        m_iptvSearchSelectedIndex = 0;
                        m_iptvSearchScrollOffset = 0;
                    } else {
                        setState(UIState::IPTV_LIST);
                    }
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
                        m_iptvVk.inResults = false;
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
                    m_iptvVk.inResults = false;
                } else if (input.isButtonJustPressed(Button::A)) {
                    if (resultCount > 0 && m_iptvSearchSelectedIndex >= 0 && m_iptvSearchSelectedIndex < resultCount) {
                        const auto& selChan = m_iptvSearchResults[m_iptvSearchSelectedIndex];
                        showToast("Đang kết nối: " + selChan.name + "...", {0, 180, 216, 255}, 3000);
                        render();
                        if (!IPTVManager::instance().playChannel(selChan, m_iptvSearchSelectedIndex, m_iptvSearchResults)) {
                            showToast("Không thể phát video (Lỗi kết nối hoặc player)", {239, 68, 68, 255}, 4000);
                        }
                        m_iptvSearchSelectedIndex = IPTVManager::instance().getLastPlayingIndex();
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
                    m_iptvVk.inResults = false;
                }
            }
            break;
        }

        case UIState::YOUTUBE_SEARCH: {
            if (m_ytIsSearching) {
                break; // Ignore input while searching
            }

            if (m_ytFocusInTags) {
                int tagCount = std::min(8, static_cast<int>(m_ytSearchHistory.size()));
                if (input.isButtonJustPressed(Button::UP)) {
                    if (m_ytSelectedTagIndex >= 4) {
                        m_ytSelectedTagIndex -= 4;
                    }
                } else if (input.isButtonJustPressed(Button::DOWN)) {
                    if (m_ytSelectedTagIndex + 4 < tagCount) {
                        m_ytSelectedTagIndex += 4;
                    } else {
                        m_ytFocusInTags = false;
                        m_ytVk.row = 0;
                        m_ytVk.col = (m_ytSelectedTagIndex % 4) * 2 + 1;
                    }
                } else if (input.isButtonJustPressed(Button::LEFT)) {
                    if (m_ytSelectedTagIndex > 0) m_ytSelectedTagIndex--;
                    else m_ytSelectedTagIndex = tagCount - 1;
                } else if (input.isButtonJustPressed(Button::RIGHT)) {
                    if (m_ytSelectedTagIndex + 1 < tagCount) m_ytSelectedTagIndex++;
                    else m_ytSelectedTagIndex = 0;
                } else if (input.isButtonJustPressed(Button::A) || input.isButtonJustPressed(Button::START)) {
                    if (m_ytSelectedTagIndex >= 0 && m_ytSelectedTagIndex < tagCount) {
                        m_ytVk.query = m_ytSearchHistory[m_ytSelectedTagIndex];
                        triggerYouTubeSearch();
                    }
                } else if (input.isButtonJustPressed(Button::B)) {
                    m_ytFocusInTags = false;
                } else if (input.isButtonJustPressed(Button::SELECT)) {
                    setState(UIState::MENU);
                }
                break;
            }

            // Dieu huong unified (stride2 = kieu media cu: action col chan).
            if (input.isButtonJustPressed(Button::UP)) {
                if (m_ytVk.row == 0 && !m_ytSearchHistory.empty()) {
                    m_ytFocusInTags = true;
                    int tagCount = std::min(8, static_cast<int>(m_ytSearchHistory.size()));
                    m_ytSelectedTagIndex = std::min(tagCount - 1, m_ytVk.col / 2);
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
                VkAction act = VirtualKeyboard::pressA(m_ytVk, [this](const char* m) {
                    showToast(m_ytVk.telexMode ? "Chế độ: TELEX" : "Chế độ: TIẾNG ANH (US)", {0, 200, 83, 255}, 1200);
                }, true);
                if (act == VkAction::Commit) {
                    triggerYouTubeSearch();
                } else if (act == VkAction::Cancel && !m_ytSearchResults.empty()) {
                    setState(UIState::YOUTUBE_RESULTS);
                }
            } else if (input.isButtonJustPressed(Button::L1)) {
                m_ytVk.shift = !m_ytVk.shift;
            } else if (input.isButtonJustPressed(Button::R1)) {
                m_ytVk.telexMode = !m_ytVk.telexMode;
                showToast(m_ytVk.telexMode ? "Chế độ: TELEX" : "Chế độ: TIẾNG ANH (US)", {0, 200, 83, 255}, 1200);
            } else if (input.isButtonJustPressed(Button::X)) {
                VirtualKeyboard::typeSpace(m_ytVk);
            } else if (input.isButtonJustPressed(Button::Y)) {
                VirtualKeyboard::backspace(m_ytVk);
            } else if (input.isButtonJustPressed(Button::START)) {
                if (!m_ytVk.query.empty()) {
                    triggerYouTubeSearch();
                } else if (!m_ytSearchResults.empty()) {
                    setState(UIState::YOUTUBE_RESULTS);
                }
            } else if (input.isButtonJustPressed(Button::B)) {
                if (!m_ytVk.query.empty()) {
                    TelexHelper::popUtf8(m_ytVk.query);
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
                    m_ytSearchTask.run([this, query, targetPage, startIdx](TaskProgress&) {
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
                    });
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

        // ===========================================================\r\n        // LocalSend P2P states
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
                    m_lsRomSelected = 0; m_lsRomScrollOffset = 0;
                    showToast("Đã refresh ROMs", {100, 116, 139, 255}, 1000);
                } else {
                    m_localSendFolderSelected = 0;
                    setState(UIState::LOCALSEND_FOLDER);
                }
            } else if (input.isButtonJustPressed(Button::LEFT) || input.isButtonJustPressed(Button::RIGHT)) {
                // L/R: chọn thiết bị trong SEND (Up/Down đã dùng để toggle Send/Receive)
                if (m_localSendMode == 0 && devCount > 0) {
                    int dir = input.isButtonJustPressed(Button::RIGHT) ? 1 : -1;
                    m_localSendSelectedDevice = (m_localSendSelectedDevice + dir + devCount) % devCount;
                }
            } else if (input.isButtonJustPressed(Button::UP) || input.isButtonJustPressed(Button::DOWN)) {
                // DPad Up/Down: toggle Send/Receive
                m_localSendMode = (m_localSendMode == 0) ? 1 : 0;
            } else if (input.isButtonJustPressed(Button::A) && m_localSendMode == 0 && devCount > 0) {
                // Vào SEND: rescan /Roms mỗi lần (flag false → render quét lại)
                m_lsPickerTab = 0;
                m_lsRomListLoaded = false;
                m_lsRomSelected = 0; m_lsRomScrollOffset = 0;
                setState(UIState::LOCALSEND_GAME_PICKER);
            }
            break;
        }

        case UIState::LOCALSEND_INCOMING: {
            // m_localSendIncomingMode: 0=picker 2 cột, 1=confirm cuối
            if (m_localSendIncomingMode == 0) {
                // ===== Screen 1: Folder picker 2 cột =====
                // Up/Down = chọn folder, L/R = chuyển focus, A = chọn folder → confirm
                auto& entries = m_localSendFolderEntries;
                if (input.isButtonJustPressed(Button::B)) {
                    if (m_localSendFolderFocus == 1) {
                        m_localSendFolderFocus = 0;
                    } else {
                        LocalSendManager::instance().rejectUpload(m_localSendPendingSessionId);
                        showToast("Đã từ chối", {239, 68, 68, 255}, 1500);
                        setState(UIState::LOCALSEND_HOME);
                    }
                } else if (input.isButtonJustPressed(Button::LEFT) ||
                           input.isButtonJustPressed(Button::RIGHT)) {
                    m_localSendFolderFocus = (m_localSendFolderFocus == 0) ? 1 : 0;
                } else if (input.isButtonJustPressed(Button::UP)) {
                    if (m_localSendFolderFocus == 0) {
                        int n = (int)entries.size();
                        if (n > 0) m_localSendFolderSelected =
                            (m_localSendFolderSelected - 1 + n) % n;
                    }
                } else if (input.isButtonJustPressed(Button::DOWN)) {
                    if (m_localSendFolderFocus == 0) {
                        int n = (int)entries.size();
                        if (n > 0) m_localSendFolderSelected =
                            (m_localSendFolderSelected + 1) % n;
                    }
                } else if (input.isButtonJustPressed(Button::A)) {
                    if (m_localSendFolderFocus == 1) {
                        // Chốt folder hiện tại → chuyển sang screen confirm
                        m_localSendIncomingSavePath = m_localSendFolderCurrentPath;
                        m_localSendIncomingMode = 1;
                    } else {
                        // Vào folder con
                        if (m_localSendFolderSelected >= 0 &&
                            m_localSendFolderSelected < (int)entries.size()) {
                            m_localSendFolderCurrentPath = entries[m_localSendFolderSelected];
                            m_localSendFolderSelected = 0;
                            m_localSendFolderLoaded = false;
                        }
                    }
                }
            } else {
                // ===== Screen 2: Confirm cuối =====
                if (input.isButtonJustPressed(Button::B)) {
                    m_localSendIncomingMode = 0;
                } else if (input.isButtonJustPressed(Button::Y)) {
                    // Quay lại folder picker
                    m_localSendFolderCurrentPath = "/mnt/SDCARD/Downloads";
                    m_localSendFolderLoaded = false;
                    m_localSendFolderSelected = 0;
                    m_localSendFolderFocus = 0;
                    m_localSendIncomingMode = 0;
                } else if (input.isButtonJustPressed(Button::A)) {
                    // Chốt save path
                    std::string rel = m_localSendIncomingSavePath;
                    const std::string root = "/mnt/SDCARD";
                    if (rel.compare(0, root.size(), root) == 0)
                        rel = rel.substr(root.size());
                    if (!rel.empty() && rel.back() != '/') rel += '/';
                    if (rel == "/") rel = "";
                    LocalSendManager::instance().setPendingTargetFolder(
                        m_localSendPendingSessionId, rel);
                    uint64_t need = m_localSendCurrentPrompt.file.size;
                    if (need > 0) {
                        uint64_t freeB = LsUtil::sdFreeBytes("/mnt/SDCARD");
                        if (freeB < need) {
                            showToast("Thẻ đầy: cần " + LsUtil::humanSize(need) +
                                      ", trống " + LsUtil::humanSize(freeB),
                                      {239, 68, 68, 255}, 3000);
                            m_localSendIncomingMode = 0;
                            break;
                        }
                    }
                    LocalSendManager::instance().approveUpload(m_localSendPendingSessionId);
                    showToast("Đã chấp nhận: " + m_localSendCurrentPrompt.file.fileName,
                              {34, 197, 94, 255}, 2000);
                    setState(UIState::LOCALSEND_PROGRESS);
                }
            }
            break;
        }

        case UIState::LOCALSEND_FOLDER: {
            // File Explorer full-screen tone xanh IPTV.
            // Ban phim rename/new-folder uu tien xu ly truoc.
            if (m_lsFolderRenaming) {
                handleLsFolderKeyboardInput();
                break;
            }
            // Up/Down = di chuyen, A = vao folder, B = lui,
            // X = chon folder lam dich, Y = thu muc moi (ban phim),
            // START = doi ten folder dang chon, SELECT = tai lai.
            auto& entries = m_localSendFolderEntries;
            if (input.isButtonJustPressed(Button::B)) {
                if (m_localSendFolderFocus == 1) {
                    m_localSendFolderFocus = 0;
                } else if (m_localSendFolderCurrentPath == "/mnt/SDCARD") {
                    setState(UIState::LOCALSEND_HOME);
                } else {
                    auto pos = m_localSendFolderCurrentPath.find_last_of('/');
                    if (pos != std::string::npos && pos > 0) {
                        m_localSendFolderCurrentPath = m_localSendFolderCurrentPath.substr(0, pos);
                    }
                    if (m_localSendFolderCurrentPath.empty()) m_localSendFolderCurrentPath = "/mnt/SDCARD";
                    m_localSendFolderLoaded = false;
                }
            } else if (input.isButtonJustPressed(Button::LEFT) ||
                       input.isButtonJustPressed(Button::RIGHT)) {
                m_localSendFolderFocus = (m_localSendFolderFocus == 0) ? 1 : 0;
            } else if (input.isButtonJustPressed(Button::UP)) {
                if (m_localSendFolderFocus == 0) {
                    int n = (int)entries.size();
                    if (n > 0) m_localSendFolderSelected =
                        (m_localSendFolderSelected - 1 + n) % n;
                }
            } else if (input.isButtonJustPressed(Button::DOWN)) {
                if (m_localSendFolderFocus == 0) {
                    int n = (int)entries.size();
                    if (n > 0) m_localSendFolderSelected =
                        (m_localSendFolderSelected + 1) % n;
                }
            } else if (input.isButtonJustPressed(Button::A)) {
                // Mở folder đang chọn (vào trong)
                if (m_localSendFolderSelected >= 0 &&
                    m_localSendFolderSelected < (int)entries.size()) {
                    m_localSendFolderCurrentPath = entries[m_localSendFolderSelected];
                    m_localSendFolderSelected = 0;
                    m_localSendFolderLoaded = false;
                }
            } else if (input.isButtonJustPressed(Button::X)) {
                // Chọn folder hiện tại làm target cho LocalSend
                std::string rel = m_localSendFolderCurrentPath;
                const std::string root = "/mnt/SDCARD";
                if (rel.compare(0, root.size(), root) == 0)
                    rel = rel.substr(root.size());
                if (!rel.empty() && rel.back() != '/') rel += '/';
                if (rel == "/") rel = "";
                LocalSendManager::instance().setTargetFolder(rel);
                showToast("Đã chọn thư mục: " + (rel.empty()
                                               ? std::string("(Tự động)")
                                               : rel),
                          {34, 197, 94, 255}, 2000);
                setState(UIState::LOCALSEND_HOME);
            } else if (input.isButtonJustPressed(Button::Y)) {
                // Thu muc moi: mo ban phim QWERTY de dat ten (mac dinh NewFolder...)
                startLsFolderRename(false);
            } else if (input.isButtonJustPressed(Button::START)) {
                // Doi ten folder dang chon
                startLsFolderRename(true);
            } else if (input.isButtonJustPressed(Button::SELECT)) {
                m_localSendFolderLoaded = false;
                showToast("Đang tải lại...", {100, 116, 139, 255}, 800);
            }
            break;
        }

        case UIState::FILE_EXPLORER: {
            if (!handleExplorerInput()) setState(UIState::MENU);
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
                if (devices.empty() ||
                    m_localSendSelectedDevice < 0 ||
                    m_localSendSelectedDevice >= (int)devices.size()) {
                    showToast("Chưa chọn thiết bị đích", {239, 68, 68, 255}, 1500);
                    break;
                }
                const auto& dev = devices[m_localSendSelectedDevice];

                // ROMS: gửi với relative path chính xác để receiver tái tạo
                // đúng cấu trúc thư mục (vd Roms/GBA/sub/...).
                if (m_lsRomSelected<0||m_lsRomSelected>=(int)m_lsRomList.size()) break;
                const auto& e0 = m_lsRomList[m_lsRomSelected];
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
                        meta.relativePath = (slash == std::string::npos)
                            ? "" : sub.substr(0, slash + 1);
                    } else {
                        meta.relativePath = "Roms/" + e0.systemDir + "/";
                    }
                }
                meta.gameTitle = e0.name;
                std::string sid = LocalSendManager::instance().sendFileMetaAsync(
                    meta, e0.path, dev);
                if (sid.empty()) {
                    showToast("Gửi thất bại (mạng?)", {239, 68, 68, 255}, 2000);
                } else {
                    m_localSendProgressSel = 0;
                    m_localSendProgressScroll = 0;
                    showToast("Đang gửi: " + e0.name,
                              {34, 197, 94, 255}, 2000);
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
            if (input.isButtonJustPressed(Button::UP) && total > 0) {
                m_localSendProgressSel = (m_localSendProgressSel - 1 + total) % total;
            } else if (input.isButtonJustPressed(Button::DOWN) && total > 0) {
                m_localSendProgressSel = (m_localSendProgressSel + 1) % total;
            }
            // Tu dong ve Home sau 3s khi tat ca xong (send DONE/FAILED + recv DONE/FAILED/REJECTED).
            bool allDone = total > 0;
            for (auto& s : sends) {
                if (s.state != LsSendProgress::DONE && s.state != LsSendProgress::FAILED) { allDone = false; break; }
            }
            if (allDone) {
                for (auto& r : recvs) {
                    if (r.state != LsUploadRequest::DONE && r.state != LsUploadRequest::FAILED &&
                        r.state != LsUploadRequest::REJECTED) { allDone = false; break; }
                }
            }
            uint32_t now = SDL_GetTicks();
            if (allDone) {
                if (m_localSendProgressDoneMs == 0) m_localSendProgressDoneMs = now;
                if (now - m_localSendProgressDoneMs > 3000) {
                    m_localSendProgressDoneMs = 0;
                    setState(UIState::LOCALSEND_HOME);
                }
            } else {
                m_localSendProgressDoneMs = 0;
            }
            break;
        }

        default: break;
    }
}

std::string UIManager::suggestNewFolderName(const std::string& parentPath) {
    // Đề xuất tên folder mới không trùng: NewFolder, NewFolder_2, NewFolder_3, ...
    auto exists = [&](const std::string& p) {
        return FileSystemManager::instance().directoryExists(p);
    };
    std::string base = parentPath + "/NewFolder";
    if (!exists(base)) return "NewFolder";
    for (int i = 2; i < 1000; ++i) {
        std::string cand = base + "_" + std::to_string(i);
        if (!exists(cand)) return cand.substr(parentPath.size() + 1);
    }
    return "NewFolder";
}

void UIManager::clearTextCache() { m_ui.clearTextCache(); }

void UIManager::drawText(const std::string& text, int x, int y, SDL_Color color, TTF_Font* font, bool centered) {
    m_ui.drawText(text, x, y, color, font, centered);
}

void UIManager::drawRect(int x, int y, int w, int h, SDL_Color color, bool filled) {
    m_ui.drawRect(x, y, w, h, color, filled);
}

void UIManager::drawBorder(int x, int y, int w, int h, SDL_Color color, int thickness) {
    m_ui.drawBorder(x, y, w, h, color, thickness);
}

void UIManager::drawRoundedRect(int x, int y, int w, int h, int radius, SDL_Color color, bool filled) {
    m_ui.drawRoundedRect(x, y, w, h, radius, color, filled);
}

void UIManager::drawRoundedBorder(int x, int y, int w, int h, int radius, SDL_Color color, int thickness) {
    m_ui.drawRoundedBorder(x, y, w, h, radius, color, thickness);
}

void UIManager::drawBadge(int x, int y, int w, int h, const std::string& text, SDL_Color bg, SDL_Color fg) {
    m_ui.drawBadge(x, y, w, h, text, bg, fg);
}

void UIManager::drawIcon(const std::string& iconName, int x, int y, int w, int h) {
    m_ui.drawIcon(iconName, x, y, w, h);
}

void UIManager::drawPlayerIcon(const std::string& iconName, int x, int y, int w, int h) {
    m_ui.drawPlayerIcon(iconName, x, y, w, h);
}

void UIManager::drawButtonIcon(const std::string& button, int x, int y, int size) {
    m_ui.drawButtonIcon(button, x, y, size);
}

int UIManager::textHeight(TTF_Font *font) {
    return m_ui.textHeight(font);
}

int UIManager::textWidth(const std::string &text, TTF_Font *font) {
    return m_ui.textWidth(text, font);
}

std::string UIManager::truncateToWidth(const std::string &text, TTF_Font *font, int maxPx) {
    return m_ui.truncateToWidth(text, font, maxPx);
}

int UIManager::pillWidth(const std::string &text, TTF_Font *font) {
    return m_ui.pillWidth(text, font);
}

void UIManager::drawPill(int x, int y, int w, int h, const std::string &text, bool active, TTF_Font *font) {
    m_ui.drawPill(x, y, w, h, text, active, font);
}

void UIManager::drawButton(int x, int y, int w, int h, const std::string &label, bool focused, bool danger) {
    m_ui.drawButton(x, y, w, h, label, focused, danger);
}

void UIManager::drawRow(int x, int y, int w, int h, bool focused, bool dim) {
    m_ui.drawRow(x, y, w, h, focused, dim);
}

int UIManager::textYCentered(int y, int h, TTF_Font *font) {
    return m_ui.textYCentered(y, h, font);
}

void UIManager::drawRowMainSub(int x, int y, int h, const std::string &main, TTF_Font *fMain,
                               const std::string &sub, TTF_Font *fSub, int maxW, int gap) {
    m_ui.drawRowMainSub(x, y, h, main, fMain, sub, fSub, maxW, gap);
}

void UIManager::drawTextRight(const std::string &text, int rightX, int y, SDL_Color color, TTF_Font *font) {
    m_ui.drawTextRight(text, rightX, y, color, font);
}

int UIManager::drawFooterHint(const std::string &button, const std::string &label, int x,
                              int barY, int barH, SDL_Color color, TTF_Font *font,
                              int iconSize, int gap) {
    return m_ui.drawFooterHint(button, label, x, barY, barH, color, font, iconSize, gap);
}

void UIManager::drawFooterHintsCentered(
    const std::vector<std::pair<std::string,std::string>> &hints,
    int barY, int barH, SDL_Color color, TTF_Font *font,
    int iconSize, int gap, int hintGap) {
    m_ui.drawFooterHintsCentered(hints, barY, barH, color, font, iconSize, gap, hintGap);
}

void UIManager::drawBadgeDual(int x, int y, int w, int h,
                     const std::string &btn1, const std::string &label1,
                     const std::string &btn2, const std::string &label2,
                     SDL_Color bg, SDL_Color fg) {
    m_ui.drawBadgeDual(x, y, w, h, btn1, label1, btn2, label2, bg, fg);
}

void UIManager::drawInlineHintsCentered(const std::string &text, int centerX, int y,
                               SDL_Color color, TTF_Font *font, int iconSize, int gap) {
    m_ui.drawInlineHintsCentered(text, centerX, y, color, font, iconSize, gap);
}

void UIManager::drawAppBackground() { m_ui.drawAppBackground(); }
void UIManager::drawCard(int x, int y, int w, int h) { m_ui.drawCard(x, y, w, h); }
void UIManager::drawFocusRow(int x, int y, int w, int h) { m_ui.drawFocusRow(x, y, w, h); }
void UIManager::drawAppHeader(const std::string &title, const std::string &sub) { m_ui.drawAppHeader(title, sub); }
void UIManager::drawPadIcon(UiTheme::PadBtn btn, int x, int y, int size) { m_ui.drawPadIcon(btn, x, y, size); }
void UIManager::drawAppFooter(const std::vector<UiTheme::FooterHint> &hints) { m_ui.drawAppFooter(hints); }
void UIManager::beginModalDim() { m_ui.beginModalDim(); }

void UIManager::drawGridIcon(const std::string &iconFile, int x, int y, int w, int h) {
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
    drawRoundedRect(panelX, panelY, panelW, 52, UiTheme::RADIUS_ROW, {22, 32, 46, 255}, true);
    drawRoundedBorder(panelX, panelY, panelW, 52, UiTheme::RADIUS_ROW, {0, 180, 216, 255}, 2);
    std::string displayQuery = m_searchVk.query.empty() ? UiStrings::SEARCH_PROMPT_INPUT : m_searchVk.query + "_";
    SDL_Color qColor = m_searchVk.query.empty() ? SDL_Color{80, 95, 115, 255} : SDL_Color{255, 255, 255, 255};
    drawText(displayQuery, panelX + 16, panelY + 14, qColor, m_fontMedium);

    // Unified keyboard (P0-2): charset media + action row, stride2 nhu YouTube.
    {
        static std::string s0, s1, s2, s3, s4;
        s0 = m_searchVk.shift ? "HOA" : "Thuong";
        s1 = m_searchVk.telexMode ? "TELEX" : "US";
        s2 = "Cach"; s3 = "Xoa"; s4 = "Tim";
        const char* sActs[5] = {s0.c_str(), s1.c_str(), s2.c_str(), s3.c_str(), s4.c_str()};
        VkState sDraw = m_searchVk;
        if (m_searchVk.inResults) sDraw.row = -1;
        m_ui.drawVirtualKeyboard(sDraw, panelX + 12, panelY + 68, 37, 40, 4, 6,
            SDL_Color{0, 180, 216, 255}, SDL_Color{160, 230, 255, 255}, sActs, true, true, 2);
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
            bool isSel = (m_searchVk.inResults && idx == m_searchSelectedIndex);

            int itemY = listStartY + i * itemH;
            SDL_Color rowBg = isSel ? SDL_Color{2, 55, 82, 255} : SDL_Color{20, 28, 42, 255};
            drawRoundedRect(rPanelX, itemY, rPanelW, itemH - 4, UiTheme::RADIUS_CARD, rowBg, true);
            if (isSel) {
                drawRoundedBorder(rPanelX, itemY, rPanelW, itemH - 4, UiTheme::RADIUS_CARD, {0, 180, 216, 255}, 2);
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
    std::string hint = m_searchVk.inResults ? UiStrings::SEARCH_NAV_UP_HINT : UiStrings::SEARCH_NAV_DOWN_HINT;
    drawText(hint, 512, 705, {60, 75, 95, 255}, m_fontSmall, true);
}

void UIManager::renderFooter() {
    if (m_currentState == UIState::IPTV_LIST || m_currentState == UIState::IPTV_SEARCH ||
        m_currentState == UIState::YOUTUBE_SEARCH || m_currentState == UIState::YOUTUBE_RESULTS) {
        return;
    }

    const int barY = UiTheme::FOOTER_Y, barH = UiTheme::FOOTER_H, iconSize = UiTheme::FOOTER_ICON, gap = UiTheme::FOOTER_GAP;
    drawRect(0, barY, UiTheme::APP_W, barH, UiTheme::FOOTER_BG, true);
    drawRect(0, barY, UiTheme::APP_W, 1, UiTheme::FOOTER_LINE, true);

    SDL_Color fg = {210, 220, 230, 255};
    SDL_Color red = {239, 68, 68, 255};
    SDL_Color cyan = {0, 180, 216, 255};
    int x = 20;

    if (m_currentState == UIState::GAME_LIST) {
        bool isLocal = false;
        if (m_selectedGameIndex >= 0 && m_selectedGameIndex < static_cast<int>(m_cachedGames.size())) {
            isLocal = (m_cachedGames[m_selectedGameIndex].localState == GameState::LOCAL);
        }

        if (isLocal) {
            x = drawFooterHint("X", UiStrings::BTN_DELETE_ROM_SD, x, barY, barH, red, m_fontSmall, iconSize, gap) + 30;
        } else {
            x = drawFooterHint("A", UiStrings::FOOTER_ADD_QUEUE, x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
        }

        x = drawFooterHint("B", UiStrings::FOOTER_BACK, x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;

        if (!isLocal) {
            x = drawFooterHint("X", UiStrings::FOOTER_DELETE, x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
        }

        x = drawFooterHint("Y", UiStrings::BTN_JUMP_ALPHA, x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
        x = drawFooterHint("START", UiStrings::BTN_SEARCH, x, barY, barH, cyan, m_fontSmall, iconSize, gap) + 30;
        x = drawFooterHint("SELECT", UiStrings::FOOTER_FILTER, x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
    } else if (m_currentState == UIState::SEARCH) {
        if (!m_searchVk.inResults) {
            x = drawFooterHint("A", UiStrings::BTN_SELECT_CHAR, x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
            x = drawFooterHint("Y", "Xoa", x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
            x = drawFooterHint("X", UiStrings::BTN_CLEAR_ALL, x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
        } else {
            x = drawFooterHint("A", UiStrings::BTN_DOWNLOAD, x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
            x = drawFooterHint("X", UiStrings::BTN_DELETE_ROM, x, barY, barH, red, m_fontSmall, iconSize, gap) + 30;
        }

        x = drawFooterHint("B", UiStrings::BTN_BACK, x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
    } else if (m_currentState == UIState::CONFIRM_DELETE) {
        x = drawFooterHint("A", UiStrings::BTN_CONFIRM_DELETE_SD, x, barY, barH, red, m_fontSmall, iconSize, gap) + 30;
        // B + X cung la huy: ve 2 icon lien nhau roi den label
        {
            int centerY = barY + barH / 2;
            drawButtonIcon("B", x, centerY - iconSize / 2, iconSize);
            x += iconSize + 4;
            drawButtonIcon("X", x, centerY - iconSize / 2, iconSize);
            x += iconSize + gap;
            int th = textHeight(m_fontSmall);
            drawText(UiStrings::BTN_CANCEL_ACTION, x, centerY - th / 2, fg, m_fontSmall);
            x += textWidth(UiStrings::BTN_CANCEL_ACTION, m_fontSmall) + 30;
        }
    } else if (m_currentState == UIState::SYSTEM_SELECT) {
        x = drawFooterHint("A", UiStrings::FOOTER_ENTER_SYSTEM, x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
        x = drawFooterHint("B", UiStrings::FOOTER_MAIN_MENU, x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
        x = drawFooterHint("Y", UiStrings::FOOTER_SYNC_DRIVE, x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
        // L1 + R1 cung la chuyen trang
        {
            int centerY = barY + barH / 2;
            drawButtonIcon("L1", x, centerY - iconSize / 2, iconSize);
            x += iconSize + 4;
            drawButtonIcon("R1", x, centerY - iconSize / 2, iconSize);
            x += iconSize + gap;
            int th = textHeight(m_fontSmall);
            drawText(UiStrings::FOOTER_CHANGE_PAGE, x, centerY - th / 2, fg, m_fontSmall);
            x += textWidth(UiStrings::FOOTER_CHANGE_PAGE, m_fontSmall) + 30;
        }
    } else if (m_currentState == UIState::OTA_UPDATE) {
        x = drawFooterHint("A", UiStrings::FOOTER_CONFIRM, x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
        x = drawFooterHint("B", UiStrings::FOOTER_CANCEL, x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
    } else if (m_currentState == UIState::FILE_EXPLORER) {
        x = drawFooterHint("A", "Vao", x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
        x = drawFooterHint("B", "Lui", x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
        x = drawFooterHint("X", "Cut", x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
        x = drawFooterHint("Y", "Copy", x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
        x = drawFooterHint("R1", "Paste", x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
    } else {
        x = drawFooterHint("A", UiStrings::FOOTER_SELECT, x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
        x = drawFooterHint("B", UiStrings::FOOTER_BACK, x, barY, barH, fg, m_fontSmall, iconSize, gap) + 30;
    }

    std::string verText = "v" + std::string(APP_VERSION) + " Native";
    drawTextRight(verText, 1012, barY + (barH - textHeight(m_fontSmall)) / 2, UiTheme::TEXT_FAINT, m_fontSmall);
}

void UIManager::renderToast() { m_dialogs.renderToast(m_ui, m_fontSmall); }

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
        drawRoundedRect(x, y, w, h, UiTheme::RADIUS_MODAL, bg, true);

        if (isSel) {
            // Glowing cyan border on selected card
            drawRoundedBorder(x, y, w, h, UiTheme::RADIUS_MODAL, {0, 180, 216, 255}, 3);
        } else {
            drawRoundedBorder(x, y, w, h, UiTheme::RADIUS_MODAL, {38, 48, 64, 255}, 1);
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
            drawRoundedRect(x + w - 54, y + 10, 44, 22, UiTheme::RADIUS_ROW, {239, 68, 68, 255}, true);
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
    drawAppFooter({{UiTheme::PadBtn::DPAD, "Chuyển"}, {UiTheme::PadBtn::START, "Cài đặt"}});
}

void UIManager::renderSystemSelectState() {
    // ─── Borderless Full-Width Sub-Header ───
    drawRect(0, 64, 1024, 48, UiTheme::CARD_SOLID, true);
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

        SDL_Color bg = selected ? UiTheme::FOCUS_BG : UiTheme::ROW_BG;
        drawRoundedRect(rx, y, rowW, rowH, UiTheme::RADIUS_ROW, bg, true);

        if (selected) {
            drawRoundedBorder(rx, y, rowW, rowH, UiTheme::RADIUS_ROW, UiTheme::ACCENT_CYAN, 2);
            // Left neon accent
            drawRoundedRect(rx + 6, y + 12, 5, rowH - 24, 2, UiTheme::ACCENT_CYAN, true);
        }

        // System icon
        int iconSize = 64;
        drawIcon(sys.code, rx + 16, y + (rowH - iconSize) / 2, iconSize, iconSize);

        std::string sysName = truncateToWidth(sys.name, m_fontLarge, rowW - 320);
        std::string subtext = truncateToWidth(std::string("/Roms/") + sys.romDir + "  •  " + sys.extList,
                                              m_fontSmall, rowW - 320);
        drawRowMainSub(rx + 96, y, rowH, sysName, m_fontLarge, subtext, m_fontSmall);

        std::string localBadge = std::to_string(sys.localCount) + " local";
        std::string cloudBadge = std::to_string(sys.cloudCount) + " cloud";

        drawBadge(rx + rowW - 200, y + (rowH - 40) / 2, 90, 40, localBadge, {22, 101, 52, 255}, UiTheme::TEXT_MAIN);
        drawBadge(rx + rowW - 100, y + (rowH - 40) / 2, 90, 40, cloudBadge, {30, 58, 138, 255}, UiTheme::TEXT_MAIN);
    }
}

void UIManager::renderGameListState() {
    // Layout A 65/35: list 666px + detail 300px (gap + divider)
    int listY = 64;
    int listH = 651;
    int listW = UiTheme::LIST_W;
    int listX = 16;

    int detailX = UiTheme::DETAIL_X;
    int detailY = 64;
    int detailW = UiTheme::DETAIL_W;
    int detailH = 651;

    // Subtle 1px vertical divider between panes
    drawRect(listX + listW + 8, 64, 1, 651, {38, 48, 64, 255}, true);

    // 1. Render Left Games List
    int totalGames = static_cast<int>(m_cachedGames.size());
    int pageSize = 6;
    int rowH = 92;
    int rowW = listW - 30;
    int rowX = listX;
    int listStartY = listY + 12;

    if (totalGames == 0) {
        drawText(UiStrings::GAME_LIST_EMPTY, listX + listW / 2, listY + 280, {140, 150, 165, 255}, m_fontLarge, true);
        drawInlineHintsCentered(UiStrings::GAME_FILTER_HINT, listX + listW / 2, listY + 325, {0, 180, 216, 255}, m_fontSmall, 24);
    } else {
        for (int i = 0; i < pageSize && (m_gameScrollOffset + i) < totalGames; ++i) {
            int gameIdx = m_gameScrollOffset + i;
            const auto& game = m_cachedGames[gameIdx];
            bool selected = (gameIdx == m_selectedGameIndex);
            int y = listStartY + i * (rowH + 12);

            SDL_Color bg = selected ? UiTheme::FOCUS_BG : UiTheme::ROW_BG;
            if (selected) drawFocusRow(rowX, y, rowW, rowH);
            else drawRoundedRect(rowX, y, rowW, rowH, UiTheme::RADIUS_ROW, bg, true);

            // State Pill Badge
            bool isThisDownloading = DownloadManager::instance().isDownloading() &&
                                     DownloadManager::instance().getProgress().gameId == game.id;
            auto dlp = DownloadManager::instance().getProgress();
            int badgeY = y + (rowH - 38) / 2;

            if (game.localState == GameState::LOCAL) {
                drawBadge(rowX + 18, badgeY, 86, 38, UiStrings::BADGE_DOWNLOADED, {22, 101, 52, 255}, UiTheme::TEXT_MAIN);
            } else if (isThisDownloading) {
                char pctBuf[16];
                std::snprintf(pctBuf, sizeof(pctBuf), "%.0f%%", dlp.progressPct);
                drawBadge(rowX + 18, badgeY, 86, 38, std::string(pctBuf), {2, 132, 199, 255}, UiTheme::TEXT_MAIN);
            } else if (DownloadManager::instance().isInQueue(game.id)) {
                auto q = DownloadManager::instance().getQueue();
                int pos = 1;
                for (const auto& qi : q) {
                    if (qi.game.id == game.id) break;
                    pos++;
                }
                drawBadge(rowX + 18, badgeY, 86, 38, "#" + std::to_string(pos), {107, 33, 168, 255}, UiTheme::TEXT_MAIN);
            } else if (game.localState == GameState::CLOUD) {
                drawBadge(rowX + 18, badgeY, 86, 38, "CLOUD", {30, 58, 138, 255}, UiTheme::TEXT_MAIN);
            } else {
                drawBadge(rowX + 18, badgeY, 86, 38, UiStrings::BTN_SYNC, {217, 119, 6, 255}, UiTheme::TEXT_MAIN);
            }

            // Title allowed up to 34 chars
            std::string title = truncateToWidth(game.title, m_fontLarge, rowW - 240);
            SDL_Color titleCol = isThisDownloading ? UiTheme::ACCENT_CYAN : (selected ? UiTheme::TEXT_MAIN : UiTheme::TEXT_DIM);

            if (isThisDownloading) {
                drawText(title, rowX + 118, textYCentered(y, 56, m_fontLarge), titleCol, m_fontLarge);

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
                std::string sizeStr = FileSystemManager::instance().formatBytes(game.sizeBytes);
                std::string sub = truncateToWidth(game.filename + "  (" + sizeStr + ")",
                                                  m_fontSmall, rowW - 240);
                // Cum main/sub can giua doc trong row 92px (fix lech tam)
                int thM = m_fontLarge ? TTF_FontHeight(m_fontLarge) : 24;
                int thS = m_fontSmall ? TTF_FontHeight(m_fontSmall) : 16;
                int blockH = thM + 4 + thS;
                int ty = y + (rowH - blockH) / 2;
                drawText(title, rowX + 118, ty, titleCol, m_fontLarge);
                drawText(sub, rowX + 118, ty + thM + 4, UiTheme::TEXT_SUB, m_fontSmall);
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
    drawRoundedRect(coverBoxX - 4, coverBoxY - 4, coverBoxW + 8, coverBoxH + 8, UiTheme::RADIUS_MODAL, {16, 20, 28, 255}, true);
    drawRoundedBorder(coverBoxX - 4, coverBoxY - 4, coverBoxW + 8, coverBoxH + 8, UiTheme::RADIUS_MODAL, {38, 48, 64, 255}, 1);

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
        drawRoundedRect(detailX + 24, bY, detailW - 48, 62, UiTheme::RADIUS_ROW, {18, 28, 44, 255}, true);
        drawRoundedBorder(detailX + 24, bY, detailW - 48, 62, UiTheme::RADIUS_ROW, {0, 180, 216, 255}, 1);

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

void UIManager::openConfirmDeleteDialog() {
    if (m_selectedGameIndex < 0 || m_selectedGameIndex >= static_cast<int>(m_cachedGames.size())) return;
    int64_t gameId = m_cachedGames[m_selectedGameIndex].id;
    std::string gameTitle = m_cachedGames[m_selectedGameIndex].title;
    std::string fileLine = std::string("File: ") + m_cachedGames[m_selectedGameIndex].filename +
        " (" + FileSystemManager::instance().formatBytes(m_cachedGames[m_selectedGameIndex].sizeBytes) + ")";
    m_dialogs.confirm.open(std::string(UiStrings::DIALOG_DELETE_TITLE),
        {gameTitle, fileLine, std::string(UiStrings::DIALOG_DELETE_PROMPT)},
        [this, gameId, gameTitle]() {
            DatabaseManager::instance().markGameDeletedLocally(gameId);
            refreshSystems();
            refreshGames();
            showToast("Đã xóa \"" + gameTitle + "\" khỏi thẻ nhớ.", {239, 68, 68, 255}, 3000);
        }, true);
}

void UIManager::openConfirmBatchDeleteDialog() {
    int selCount = static_cast<int>(m_selectedGameIds.size());
    if (selCount <= 0) return;
    uint64_t totalSize = 0;
    std::vector<std::string> lines;
    lines.push_back("Bạn muốn xóa " + std::to_string(selCount) + " game khỏi thẻ nhớ?");
    for (int64_t gameId : m_selectedGameIds) {
        for (const auto& g : m_cachedGames) {
            if (g.id == gameId) { totalSize += g.sizeBytes; break; }
        }
    }
    lines.push_back("Tổng dung lượng: " + FileSystemManager::instance().formatBytes(totalSize));
    int shown = 0;
    for (const auto& g : m_cachedGames) {
        if (shown >= 5) break;
        if (std::find(m_selectedGameIds.begin(), m_selectedGameIds.end(), g.id) != m_selectedGameIds.end()) {
            std::string t = g.title;
            if (t.length() > 40) t = t.substr(0, 37) + "...";
            lines.push_back("- " + t);
            shown++;
        }
    }
    std::vector<int64_t> ids = m_selectedGameIds;
    m_dialogs.confirm.open(std::string(UiStrings::MULTI_BATCH_DELETE_TITLE), std::move(lines),
        [this, ids]() {
            for (int64_t gameId : ids) DatabaseManager::instance().markGameDeletedLocally(gameId);
            refreshSystems();
            refreshGames();
            showToast("Đã xóa " + std::to_string(ids.size()) + " game khỏi thẻ nhớ.", {239, 68, 68, 255}, 4000);
            m_multiSelectMode = false;
            m_selectedGameIds.clear();
        }, true);
}

void UIManager::renderConfirmDialogFromState() { m_dialogs.renderConfirm(m_ui, m_fontSmall, m_fontMedium, m_fontLarge); }

void UIManager::renderConfirmDeleteDialog() {
    renderConfirmDialogFromState();
}

void UIManager::renderConfirmBatchDeleteDialog() {
    renderConfirmDialogFromState();
}

void UIManager::renderProgressDialogFromState() { m_dialogs.renderProgress(m_ui, m_fontSmall, m_fontLarge); }
void UIManager::drawProgressBar(int x, int y, int w, int h, double frac, SDL_Color fill, bool rounded, SDL_Color bg) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int radius = rounded ? UiTheme::RADIUS_ROW : 0;
    if (rounded) {
        drawRoundedRect(x, y, w, h, radius, bg, true);
    } else {
        drawRect(x, y, w, h, bg, true);
    }
    int fw = (int)(w * frac);
    if (fw <= 0) return;
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

    // Main card background only
    drawRoundedRect(cardX, cardY, cardW, cardH, UiTheme::RADIUS_MODAL, {22, 27, 36, 255}, true);

    // Amber warning header bar
    drawRoundedRect(cardX, cardY, cardW, 58, UiTheme::RADIUS_ROW, {50, 32, 12, 255}, true);
    drawText(UiStrings::DISCLAIMER_TITLE, 512, cardY + 18, {245, 158, 11, 255}, m_fontLarge, true);

    // Inner panel - subtle inset without border
    int innerX = cardX + 30;
    int innerY = cardY + 76;
    int innerW = cardW - 60;
    int innerH = 435;
    drawRoundedRect(innerX, innerY, innerW, innerH, UiTheme::RADIUS_CARD, {16, 20, 28, 255}, true);

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
    drawRect(0, 64, 1024, 48, UiTheme::CARD_SOLID, true);
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

        drawRoundedRect(cardX, y, cardW, rowH, UiTheme::RADIUS_CARD, bg, true);

        if (selected) {
            drawRoundedBorder(cardX, y, cardW, rowH, UiTheme::RADIUS_CARD, {0, 180, 216, 255}, 2);
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

    drawAppFooter({{UiTheme::PadBtn::A, "Chọn"}});
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

void UIManager::renderSyncOverlay() { m_dialogs.renderSyncOverlay(m_ui, m_fontSmall, m_fontMedium, m_fontLarge); }

void UIManager::renderDownloadOverlay() { m_dialogs.renderDownloadOverlay(m_ui, m_fontSmall, m_fontLarge); }

void UIManager::renderDiagnosticsState() {
    // ─── Borderless Full-Width Sub-Header ───
    drawRect(0, 64, 1024, 48, UiTheme::CARD_SOLID, true);
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
            drawRoundedRect(cardX, y - 4, cardW, rowH, UiTheme::RADIUS_CARD, {22, 28, 38, 255}, true);
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

        drawRoundedRect(scrollBarX, scrollBarY, 6, scrollBarH, 4, {35, 42, 54, 255}, true);
        drawRoundedRect(scrollBarX, thumbY, 6, thumbH, 4, {0, 180, 216, 255}, true);
    }

    drawAppFooter({{UiTheme::PadBtn::B, "Lùi"}, {UiTheme::PadBtn::DPAD, "Cuộn"}});
}

void UIManager::renderReverseSyncState() {
    // ─── Borderless Full-Width Sub-Header ───
    drawRect(0, 64, 1024, 48, UiTheme::CARD_SOLID, true);
    drawRect(0, 111, 1024, 1, {38, 48, 64, 255}, true);
    drawText(UiStrings::REVERSE_SYNC_TITLE, 36, 78, {168, 85, 247, 255}, m_fontLarge);

    auto prog = UploadManager::instance().getProgress();

    int cardX = 100;
    int cardW = 824;
    int cardY = 126;
    int cardH = 480;

    drawRoundedRect(cardX, cardY, cardW, cardH, UiTheme::RADIUS_MODAL, {22, 28, 38, 255}, true);
    drawRoundedBorder(cardX, cardY, cardW, cardH, UiTheme::RADIUS_MODAL, {168, 85, 247, 255}, 2);

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
            float pct = std::max(0.0f, std::min(100.0f, (float)prog.progressPct));
            drawProgressBar(barX, barY, barW, barH, pct / 100.0, {168, 85, 247, 255}, true);

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
        drawBadgeDual(cardX + (cardW - 180) / 2, cardY + cardH - 70, 180, 46, "A", "/", "B", "Quay lại", {55, 65, 81, 255}, {255, 255, 255, 255});
    }
}

void UIManager::renderUploadOverlay() { m_dialogs.renderUploadOverlay(m_ui, m_fontSmall); }

void UIManager::renderOTAUpdateState() {
    // ─── Borderless Full-Width Sub-Header ───
    drawRect(0, 64, 1024, 48, UiTheme::CARD_SOLID, true);
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
    drawRoundedRect(cardX, contentBoxY, cardW, contentBoxH, UiTheme::RADIUS_MODAL, {18, 24, 34, 255}, true);
    drawRoundedBorder(cardX, contentBoxY, cardW, contentBoxH, UiTheme::RADIUS_MODAL, {38, 48, 64, 255}, 1);

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

            drawBadgeDual(362, contentBoxY + 235, 300, 48, "A", "Kiểm tra", "B", "Quay lại", {35, 45, 60, 255}, {255, 255, 255, 255});
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
            float pct = std::max(0.0, std::min(100.0, prog.progressPct));
            drawProgressBar(barX, barY, barW, barH, pct / 100.0, {34, 197, 94, 255}, true);

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

            drawBadgeDual(362, contentBoxY + 245, 300, 48, "A", "Thử lại", "B", "Quay lại", {35, 45, 60, 255}, {255, 255, 255, 255});
            break;
        }
    }

    drawAppFooter({{UiTheme::PadBtn::B, "Lùi"}});
}

// ---------------------------------------------------------------------------
// renderIPTVPlaylistSelectState
// Man hinh chon playlist khi co nhieu file .m3u
// ---------------------------------------------------------------------------
void UIManager::renderIPTVPlaylistSelectState() {
    // Header
    drawRect(0, 0, 1024, 64, {18, 22, 30, 255}, true);
    drawRect(0, 63, 1024, 1, {40, 48, 62, 255}, true);
    drawText("XEM TV - CHỌN PLAYLIST", 24, 18, {0, 180, 216, 255}, m_fontLarge);

    const auto& playlists = IPTVManager::instance().getPlaylists();
    int plCount = static_cast<int>(playlists.size());

    std::string countText = std::to_string(plCount) + " playlist";
    drawText(countText, 1024 - 24, 18, {130, 145, 165, 255}, m_fontSmall, true);

    int listY      = 80;
    int itemH      = 60;
    int visItems   = 10;
    int itemW      = 992;
    int itemX      = 16;

    if (playlists.empty()) {
        drawText("Chưa có playlist nào.", 512, 340, {150, 160, 175, 255}, m_fontMedium, true);
        drawText("Tải file .m3u qua Web Server (Cổng 8080)", 512, 385, {100, 110, 125, 255}, m_fontSmall, true);
    } else {
        for (int i = m_playlistScrollOffset;
             i < plCount && i < m_playlistScrollOffset + visItems; i++)
        {
            const Playlist& pl = playlists[static_cast<size_t>(i)];
            int y = listY + (i - m_playlistScrollOffset) * (itemH + 4);
            bool selected = (i == m_selectedPlaylistIndex);

            SDL_Color bg = selected ? UiTheme::FOCUS_BG : UiTheme::ROW_BG;
            drawRoundedRect(itemX, y, itemW, itemH, UiTheme::RADIUS_CARD, bg, true);
            if (selected)
                drawRoundedBorder(itemX, y, itemW, itemH, UiTheme::RADIUS_CARD, UiTheme::ACCENT_CYAN, 2);

            // Index badge
            char numBuf[8];
            snprintf(numBuf, sizeof(numBuf), "%02d", i + 1);
            drawText(numBuf, itemX + 18, textYCentered(y, itemH, m_fontMedium), UiTheme::ACCENT_CYAN, m_fontMedium);

            // Playlist name + source can giua doc (fix +10/+36 cung)
            std::string plName = truncateToWidth(pl.name, m_fontMedium, itemW - 260);
            std::string sfText = truncateToWidth(pl.sourceFile, m_fontSmall, itemW - 260);
            drawRowMainSub(itemX + 68, y, itemH, plName, m_fontMedium, sfText, m_fontSmall);

            // Channel count badge on right
            std::string cntText = std::to_string(pl.channelCount()) + " kênh";
            drawBadge(itemX + itemW - 140, y + (itemH - 28) / 2, 120, 28, cntText, {28, 42, 62, 255}, {147, 197, 253, 255});
            drawRoundedBorder(itemX + itemW - 140, y + (itemH - 28) / 2, 120, 28, UiTheme::RADIUS_ROW, {59, 130, 246, 120}, 1);

            // Last-refresh time (chi hien voi URL playlist)
            const auto& srcs = IPTVManager::instance().getSources();
            for (const auto& s : srcs) {
                if (s.filename == pl.sourceFile && s.type == "url") {
                    std::string tsText = "Refresh: " + s.lastRefreshedStr();
                    if (s.refreshIntervalHours > 0)
                        tsText += " | moi " + std::to_string(s.refreshIntervalHours) + "h";
                    else
                        tsText += " | auto: tat";
                    drawText(tsText, itemX + itemW - 400, y + 38, {100, 120, 145, 255}, m_fontSmall);
                    break;
                }
            }

        }

        // Scrollbar indicator
        if (plCount > visItems) {
            int sbH = 640;
            int sbX = 1010;
            int sbY = listY;
            drawRect(sbX, sbY, 4, sbH, {35, 45, 60, 255}, true);
            int thumbH = std::max(30, sbH * visItems / plCount);
            int thumbY = sbY + (sbH - thumbH) * m_playlistScrollOffset / std::max(1, plCount - visItems);
            drawRect(sbX, thumbY, 4, thumbH, {0, 180, 216, 200}, true);
        }
    }

    // Footer
    drawRect(0, 715, 1024, 53, {18, 22, 30, 255}, true);
    drawRect(0, 715, 1024, 1, {40, 48, 62, 255}, true);
    drawAppFooter({{UiTheme::PadBtn::Y, "Tải lại"}, {UiTheme::PadBtn::X, "Đổi"}, {UiTheme::PadBtn::B, "Menu"}});
}

void UIManager::renderIPTVState() {
    // Khi mpv dang phat: SDL khong render gi het.
    // mpv so huu toan bo framebuffer. Channel list hien qua mpv OSD (showIPTVChannelOSD).
    // Khong co conflict framebuffer, khong co flicker.
    if (IPTVManager::instance().isIPTVPlaying()) return;

    // Layout: toan man hinh khi chua phat (isIPTVPlaying() == true da return o tren roi)
    const int SCREEN_H = 768;
    const int UI_TOP = 0;
    const int UI_H = SCREEN_H;

    // Nen toan man hinh
    drawRect(0, UI_TOP, 1024, UI_H, {14, 18, 26, 255}, true);

    // Header
    int headerBottom = UI_TOP;
    drawRect(0, 0, 1024, 64, {18, 22, 30, 255}, true);
    drawRect(0, 63, 1024, 1, {40, 48, 62, 255}, true);
    std::string title = m_iptvShowFavoritesOnly ? "XEM TV - KENH YEU THICH ★" : UiStrings::IPTV_TITLE;
    drawText(title, 24, 18, {0, 180, 216, 255}, m_fontLarge);
    headerBottom = 64;

    // -----------------------------------------------------------------------
    // Build filtered channel list (dong bo voi input handler)
    // -----------------------------------------------------------------------
    std::vector<IPTVChannel> allChannels;
    if (m_iptvShowFavoritesOnly) {
        allChannels = IPTVManager::instance().getFavoriteChannels();
    } else if (m_activePlaylistIndex >= 0) {
        const Playlist* pl = IPTVManager::instance().getPlaylist(
            static_cast<size_t>(m_activePlaylistIndex));
        if (pl) {
            for (const auto& item : pl->channels)
                allChannels.push_back(IPTVChannel::fromItem(item, pl->name, pl->sourceFile));
        }
    } else {
        allChannels = IPTVManager::instance().getChannels();
    }

    // Build group list
    std::vector<std::string> groups;
    groups.push_back(""); // Tất cả
    {
        std::unordered_set<std::string> seen;
        for (const auto& ch : allChannels) {
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
        for (const auto& ch : allChannels)
            if (ch.group == m_iptvSelectedGroup) channels.push_back(ch);
    }
    int channelCount = static_cast<int>(channels.size());

    // -----------------------------------------------------------------------
    // Group filter bar (nam ngay duoi header)
    // -----------------------------------------------------------------------
    const int GROUP_BAR_Y = headerBottom;
    const int GROUP_BAR_H = UiTheme::GROUP_BAR_H;
    drawRect(0, GROUP_BAR_Y, 1024, GROUP_BAR_H, {20, 26, 38, 255}, true);
    drawRect(0, GROUP_BAR_Y + GROUP_BAR_H - 1, 1024, 1, {40, 48, 62, 255}, true);

    {
        // Chuan A: pill full-round, do rong theo pixel (TTF_SizeUTF8), chua 150px phai cho count
        const int COUNT_RESERVE = 150;
        const int VIEW_RIGHT = 1024 - COUNT_RESERVE;
        int gx = 8 - m_iptvGroupBarOffset;
        const int gH   = UiTheme::PILL_H;
        const int gY   = GROUP_BAR_Y + (GROUP_BAR_H - gH) / 2;

        // Scroll ngang dua tren m_iptvGroupBarOffset
        for (int gi = 0; gi < static_cast<int>(groups.size()); gi++) {
            const std::string& g = groups[gi];
            std::string label = g.empty() ? "Tất cả" : g;
            label = truncateToWidth(label, m_fontSmall, UiTheme::PILL_MAX_W - UiTheme::PILL_PAD_X * 2);

            int gW = pillWidth(label, m_fontSmall);

            // Skip if before viewport
            if (gx + gW < 0) {
                gx += gW + UiTheme::PILL_GAP;
                continue;
            }
            // Stop if after viewport (tru vung count phai)
            if (gx > VIEW_RIGHT) break;

            bool isActiveG = (g == m_iptvSelectedGroup);

            drawPill(gx, gY, gW, gH, label, isActiveG, m_fontSmall);

            gx += gW + UiTheme::PILL_GAP;
        }

        // Count badge ben phai
        if (true) {
            std::string countText = std::to_string(channelCount) +
                (m_iptvShowFavoritesOnly ? " kênh yêu thích" : " kênh");
            int th = m_fontSmall ? TTF_FontHeight(m_fontSmall) : 16;
            drawTextRight(countText, 1000, GROUP_BAR_Y + (GROUP_BAR_H - th) / 2, UiTheme::TEXT_SUB, m_fontSmall);
        }
    }

    // -----------------------------------------------------------------------
    // Channel list
    // -----------------------------------------------------------------------
    const int LIST_TOP     = GROUP_BAR_Y + GROUP_BAR_H + 2;
    const int FOOTER_H     = UiTheme::FOOTER_H;
    const int LIST_BOTTOM  = UiTheme::FOOTER_Y;
    const int LIST_H       = LIST_BOTTOM - LIST_TOP;
    const int itemH        = 50; // compact khi dang phat
    const int itemGap      = 4;
    const int visibleItems = std::max(3, LIST_H / (itemH + itemGap));

    if (channels.empty()) {
        int midY = LIST_TOP + LIST_H / 2;
        if (m_iptvShowFavoritesOnly) {
            drawText("Chưa có kênh yêu thích nào.", 512, midY - 16, {150, 160, 175, 255}, m_fontMedium, true);
            drawInlineHintsCentered("Bấm [X] trên danh sách kênh để đánh dấu yêu thích", 512, midY + 14, {100, 110, 125, 255}, m_fontSmall, 24);
        } else if (!m_iptvSelectedGroup.empty()) {
            drawText("Không có kênh trong nhóm \"" + m_iptvSelectedGroup + "\"", 512, midY, {150, 160, 175, 255}, m_fontMedium, true);
        } else {
            drawText(UiStrings::IPTV_NO_CHANNELS, 512, midY - 16, {150, 160, 175, 255}, m_fontMedium, true);
            drawText("Vui lòng tải playlist (.m3u) qua Web Server (Cổng 8080)", 512, midY + 14, {100, 110, 125, 255}, m_fontSmall, true);
        }
    } else {
        for (int i = m_iptvScrollOffset;
             i < channelCount && i < m_iptvScrollOffset + visibleItems; i++) {
            int y = LIST_TOP + (i - m_iptvScrollOffset) * (itemH + itemGap);
            bool isSelected = (i == m_selectedIPTVChannelIndex);

            SDL_Color bg = isSelected ? UiTheme::FOCUS_BG : UiTheme::ROW_BG_IPTV;
            drawRoundedRect(8, y, 1008, itemH, UiTheme::RADIUS_ROW, bg, true);
            if (isSelected) drawRoundedBorder(8, y, 1008, itemH, UiTheme::RADIUS_ROW, UiTheme::ACCENT_CYAN, 2);

            // So kenh + ten kenh can giua doc rieng theo font (fix Small/Medium chung Y)
            int numY = textYCentered(y, itemH, m_fontSmall);
            int nameY = textYCentered(y, itemH, m_fontMedium);

            // Channel number
            char numBuf[8];
            snprintf(numBuf, sizeof(numBuf), "%02d", i + 1);
            drawText(numBuf, 22, numY, UiTheme::ACCENT_CYAN, m_fontSmall);

            // Channel name (cat theo pixel, khong cat giua tieng Viet)
            std::string chanName = truncateToWidth(channels[i].name, m_fontMedium, 480);
            drawText(chanName, 58, nameY, UiTheme::TEXT_MAIN, m_fontMedium);

            // Playing indicator
            if (false &&
                IPTVManager::instance().getCurrentChannelName() == channels[i].name) {
                drawText("● DANG PHAT", 420, textYCentered(y, itemH, m_fontSmall), UiTheme::ACCENT_GREEN, m_fontSmall);
            }

            if (true) {
                int badgeCY = y + (itemH - 24) / 2;
                // Favorite star
                if (channels[i].isFavorite)
                    drawBadge(590, y + (itemH - 22) / 2, 28, 22, "★", UiTheme::ACCENT_GOLD, UiTheme::TEXT_MAIN);
                // Group tag
                if (!channels[i].group.empty() && m_iptvSelectedGroup.empty()) {
                    std::string grp = truncateToWidth(channels[i].group, m_fontSmall, 160);
                    drawText(grp, 632, textYCentered(y, itemH, m_fontSmall), UiTheme::TEXT_SUB, m_fontSmall);
                }
                // Source badge
                std::string srcText = truncateToWidth(channels[i].source.empty() ? "Mac dinh" : channels[i].source,
                                                      m_fontSmall, 150);
                drawBadge(820, badgeCY, 170, 24, srcText, {28, 42, 62, 255}, {147, 197, 253, 255});
                drawRoundedBorder(820, badgeCY, 170, 24, 5, {59, 130, 246, 120}, 1);
            }
        }

        // Scrollbar
        if (channelCount > visibleItems) {
            int sbH = LIST_H - 4;
            int sbX = 1014;
            drawRect(sbX, LIST_TOP + 2, 4, sbH, {35, 45, 60, 255}, true);
            int thumbH = std::max(20, sbH * visibleItems / channelCount);
            int thumbY = LIST_TOP + 2 + (sbH - thumbH) * m_iptvScrollOffset /
                         std::max(1, channelCount - visibleItems);
            drawRect(sbX, thumbY, 4, thumbH, {0, 180, 216, 200}, true);
        }
    }

    // -----------------------------------------------------------------------
    // Footer (ve qua drawAppFooter — tu ve nen + chong tran)
    // -----------------------------------------------------------------------
    if (m_iptvShowFavoritesOnly) {
        drawAppFooter({{UiTheme::PadBtn::B, "Lùi"}, {UiTheme::PadBtn::X, "Thích"}, {UiTheme::PadBtn::DPAD, "Nhóm"}});
    } else {
        drawAppFooter({{UiTheme::PadBtn::B, "Lùi"}, {UiTheme::PadBtn::X, "Thích"}, {UiTheme::PadBtn::Y, "Lọc"}, {UiTheme::PadBtn::DPAD, "Nhóm"}});
    }
}


void UIManager::renderIPTVSearchState() {
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
    drawRoundedRect(panelX, panelY, panelW, 52, UiTheme::RADIUS_ROW, {22, 32, 46, 255}, true);
    drawRoundedBorder(panelX, panelY, panelW, 52, UiTheme::RADIUS_ROW, {0, 180, 216, 255}, 2);
    std::string displayQuery = m_iptvVk.query.empty() ? "Nhập tên kênh (VTV, HBO...)" : m_iptvVk.query + "_";
    SDL_Color qColor = m_iptvVk.query.empty() ? SDL_Color{80, 95, 115, 255} : SDL_Color{255, 255, 255, 255};
    drawText(displayQuery, panelX + 16, panelY + 14, qColor, m_fontMedium);

    // Unified keyboard (P0-2): charset media + action row, stride2 nhu YouTube.
    {
        static std::string i0, i1, i2, i3, i4;
        i0 = m_iptvVk.shift ? "HOA" : "Thuong";
        i1 = m_iptvVk.telexMode ? "TELEX" : "US";
        i2 = "Cach"; i3 = "Xoa"; i4 = "Tim";
        const char* iActs[5] = {i0.c_str(), i1.c_str(), i2.c_str(), i3.c_str(), i4.c_str()};
        VkState iDraw = m_iptvVk;
        if (m_iptvVk.inResults) iDraw.row = -1;
        m_ui.drawVirtualKeyboard(iDraw, panelX + 12, panelY + 68, 37, 40, 4, 6,
            SDL_Color{0, 180, 216, 255}, SDL_Color{160, 230, 255, 255}, iActs, true, true, 2);
    }
    // Divider line
    drawRect(476, 64, 1, 651, {38, 48, 64, 255}, true);

    // Right panel: Results List
    int rPanelX = 496;
    int rPanelW = 1024 - rPanelX - 24;
    int rPanelY = panelY;

    if (m_iptvVk.query.empty()) {
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
            bool isSel = (m_iptvVk.inResults && idx == m_iptvSearchSelectedIndex);

            int itemY = listStartY + i * (itemH + 6);
            SDL_Color rowBg = isSel ? SDL_Color{30, 58, 95, 255} : SDL_Color{22, 28, 38, 255};
            drawRoundedRect(rPanelX, itemY, rPanelW, itemH, UiTheme::RADIUS_CARD, rowBg, true);
            if (isSel) {
                drawRoundedBorder(rPanelX, itemY, rPanelW, itemH, UiTheme::RADIUS_CARD, {0, 180, 216, 255}, 2);
            }

            // Channel number
            char numBuf[16];
            snprintf(numBuf, sizeof(numBuf), "%02d", idx + 1);
            drawText(numBuf, rPanelX + 16, textYCentered(itemY, itemH, m_fontMedium), UiTheme::ACCENT_CYAN, m_fontMedium);

            // Channel name (cat theo pixel, giu tieng Viet)
            std::string chanName = truncateToWidth(chan.name, m_fontMedium, rPanelW - 330);
            drawText(chanName, rPanelX + 55, textYCentered(itemY, itemH, m_fontMedium), UiTheme::TEXT_MAIN, m_fontMedium);

            // Favorite star badge
            if (chan.isFavorite) {
                drawBadge(rPanelX + rPanelW - 225, itemY + (itemH - 26) / 2, 28, 26, "★", UiTheme::ACCENT_GOLD, UiTheme::TEXT_MAIN);
            }

            // Group tag
            if (!chan.group.empty()) {
                std::string grp = truncateToWidth(chan.group, m_fontSmall, 90);
                drawText(grp, rPanelX + rPanelW - 190, textYCentered(itemY, itemH, m_fontSmall), UiTheme::TEXT_SUB, m_fontSmall);
            }

            // Source tag column
            std::string src = truncateToWidth(chan.source.empty() ? "Nguồn" : chan.source, m_fontSmall, 70);
            drawBadge(rPanelX + rPanelW - 95, itemY + (itemH - 28) / 2, 90, 28, src, {28, 42, 62, 255}, {147, 197, 253, 255});
            drawRoundedBorder(rPanelX + rPanelW - 95, itemY + (itemH - 28) / 2, 90, 28, UiTheme::RADIUS_ROW, {59, 130, 246, 120}, 1);
        }
    }

    // Footer (icons + labels, can giua)
    drawRect(0, 715, 1024, 53, {18, 22, 30, 255}, true);
    drawRect(0, 715, 1024, 1, {40, 48, 62, 255}, true);

    if (!m_iptvVk.inResults) {
        drawAppFooter({{UiTheme::PadBtn::B, "Lùi"}, {UiTheme::PadBtn::X, "Xóa"}});
    } else {
        drawAppFooter({{UiTheme::PadBtn::B, "Bàn phím"}, {UiTheme::PadBtn::X, "Thích"}, {UiTheme::PadBtn::L1, "Trang"}, {UiTheme::PadBtn::R1, "Trang"}});
    }
}

void UIManager::render() {
    // Khi IPTV dang phat: khong render SDL de tranh conflict framebuffer voi mpv.
    // mpv so huu man hinh, channel list hien qua mpv OSD native (IPC show-text).
    if (IPTVManager::instance().isIPTVPlaying() &&
        m_currentState == UIState::IPTV_LIST) {
        return;
    }

    SDL_SetRenderDrawColor(m_renderer, UiTheme::BG_APP.r, UiTheme::BG_APP.g, UiTheme::BG_APP.b, 255);
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
        case UIState::IPTV_PLAYLIST_SELECT: renderIPTVPlaylistSelectState(); break;
        case UIState::IPTV_LIST:        renderIPTVState(); break;
        case UIState::IPTV_SEARCH:      renderIPTVSearchState(); break;
        case UIState::YOUTUBE_SEARCH:   renderYouTubeSearchState(); break;
        case UIState::YOUTUBE_RESULTS:  renderYouTubeResultsState(); break;
        case UIState::LOCALSEND_HOME:       renderLocalSendHome(); break;
        case UIState::LOCALSEND_INCOMING:   renderLocalSendHome(); renderLocalSendIncomingDialog(); break;
        case UIState::LOCALSEND_FOLDER:     renderLocalSendFolderPicker(); break;
        case UIState::LOCALSEND_SEND:       renderLocalSendSendPicker(); break;
        case UIState::LOCALSEND_GAME_PICKER: renderLocalSendHome(); renderLocalSendGamePicker(); break;
        case UIState::LOCALSEND_PROGRESS:   renderLocalSendHome(); renderLocalSendProgress(); break;
        case UIState::FILE_EXPLORER:        renderFileExplorer(); break;
        default: break;
    }

    renderFooter();
    renderToast();
    renderProgressDialogFromState();
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

void UIManager::loadYouTubeHistory() {
    if (!m_ytSearchHistory.empty()) return;
    std::string appRoot = AppConfig::instance().getAppRoot();
    if (appRoot.empty()) appRoot = "/mnt/SDCARD/Apps/RomCloud";
    std::string histFile = appRoot + "/config/yt_history.txt";
    std::ifstream file(histFile);
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            if (!line.empty()) {
                m_ytSearchHistory.push_back(line);
                if (m_ytSearchHistory.size() >= 8) break;
            }
        }
    }
    if (m_ytSearchHistory.empty()) {
        m_ytSearchHistory = {
            "Nhac tre remix", "Karaoke bolero",
            "Tin tức 24h", "Review phim",
            "Hài sitcom", "Bóng đá highlights",
            "Phim hoạt hình", "Công nghệ mới"
        };
    }
}

void UIManager::saveYouTubeHistory(const std::string& query) {
    if (query.empty()) return;
    auto it = std::remove(m_ytSearchHistory.begin(), m_ytSearchHistory.end(), query);
    m_ytSearchHistory.erase(it, m_ytSearchHistory.end());
    m_ytSearchHistory.insert(m_ytSearchHistory.begin(), query);
    while (m_ytSearchHistory.size() > 8) {
        m_ytSearchHistory.pop_back();
    }
    std::string appRoot = AppConfig::instance().getAppRoot();
    if (appRoot.empty()) appRoot = "/mnt/SDCARD/Apps/RomCloud";
    std::string configDir = appRoot + "/config";
    mkdir(configDir.c_str(), 0755);
    std::string histFile = configDir + "/yt_history.txt";
    std::ofstream file(histFile);
    if (file.is_open()) {
        for (const auto& q : m_ytSearchHistory) {
            file << q << "\n";
        }
    }
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
    std::string ytdl = appRoot + "/bin/yt-dlp";
    std::string ytdlGlibc = appRoot + "/bin/yt-dlp-glibc";

    if (access(scriptPath.c_str(), X_OK) != 0 || (access(ytdl.c_str(), X_OK) != 0 && access(ytdlGlibc.c_str(), X_OK) != 0)) {
        Logger::warn("[YouTube] YouTube dependencies missing, auto-repairing...");
        UpdateManager::instance().checkAndInstallDependencies();
    }

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
    m_resolveTask.run([this, videoId](TaskProgress&) {
        resolveYouTubeStreamUrl(videoId);
    });
}

void UIManager::clearThumbnailCache() {
    m_ytThumbCache.clear();
}

void UIManager::startThumbnailDownloads(const std::vector<std::string>& videoIds) {
    if (videoIds.empty()) return;
    m_thumbTask.run([videoIds](TaskProgress&) {
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
    });
}

void UIManager::triggerYouTubeSearch() {
    if (m_ytVk.query.empty()) {
        showToast("Vui lòng nhập từ khóa tìm kiếm", {245, 158, 11, 255}, 2000);
        return;
    }
    if (m_ytIsSearching) return;

    m_ytCurrentPage = 1;
    m_ytLastSearchQuery = m_ytVk.query;
    m_ytAllCachedResults.clear();
    clearThumbnailCache();
    saveYouTubeHistory(m_ytVk.query);

    m_ytIsSearching = true;
    m_ytSearchFinished = false;
    m_ytErrorMessage.clear();
    std::string query = m_ytVk.query;

    m_ytSearchTask.run([this, query](TaskProgress&) {
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
    });
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

    m_resolveTask.run([this, videoId](TaskProgress&) {
        std::string streamUrl = resolveYouTubeStreamUrl(videoId);
        m_ytPendingStreamUrl = streamUrl;
        m_ytVideoReady = true;
    });
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
    loadYouTubeHistory();

    // ─── TrimUI Stock OS Ambient Teal Theme ───
    drawAppBackground();

    // Header bar
    drawRect(0, 0, 1024, 50, {10, 32, 44, 255}, true);
    drawRect(0, 50, 1024, 1, {20, 54, 70, 255}, true);

    // Green chevron back arrow "<" badge
    int backBadgeX = 46;
    int backBadgeY = 11;
    int backBadgeW = 28;
    int backBadgeH = 28;
    drawRoundedRect(backBadgeX, backBadgeY, backBadgeW, backBadgeH, UiTheme::RADIUS_ROW, {0, 200, 83, 255}, true);
    drawText("<", backBadgeX + backBadgeW / 2, backBadgeY + backBadgeH / 2 - 2, {255, 255, 255, 255}, m_fontSmall, true);

    // Search header title
    drawText("YouTube", 84, 15, {245, 250, 255, 255}, m_fontMedium, false);

    // Current input mode badge (TELEX vs US) positioned safely to the right
    // Dat [R1] o dau de drawBadge parse thanh icon that
    std::string modeText = m_ytVk.telexMode ? "[R1] TELEX" : "[R1] US";
    SDL_Color modeBg = m_ytVk.telexMode ? SDL_Color{0, 200, 83, 255} : SDL_Color{25, 60, 78, 255};
    drawBadge(220, 11, 108, 28, modeText, modeBg, {255, 255, 255, 255});

    // Right-side indicators: Battery & Clock
    int bat = getBatteryLevel();
    std::string batStr = std::to_string(bat) + "%";
    drawText(batStr, 910, 16, {190, 215, 228, 255}, m_fontSmall, false);

    // Battery icon
    drawRoundedBorder(962, 17, 24, 13, 3, {190, 215, 228, 255}, 1);
    drawRect(986, 20, 2, 7, {190, 215, 228, 255}, true);
    int bFill = (20 * bat) / 100;
    if (bFill > 0) {
        SDL_Color bColor = (bat <= 20) ? SDL_Color{239, 68, 68, 255} : SDL_Color{0, 200, 83, 255};
        drawRect(964, 19, bFill, 9, bColor, true);
    }

    std::string timeStr = getCurrentTimeString();
    drawText(timeStr, 842, 16, {190, 215, 228, 255}, m_fontSmall, false);

    // ─── Input Field Box (Slimmer, Y = 58 to 114) ───
    int inX = 46;
    int inY = 58;
    int inW = 932;
    int inH = 56;
    drawRoundedRect(inX, inY, inW, inH, UiTheme::RADIUS_CARD, {12, 38, 50, 230}, true);
    drawRoundedBorder(inX, inY, inW, inH, UiTheme::RADIUS_CARD, {26, 72, 92, 255}, 1);

    std::string dispQ = m_ytVk.query.empty() ? "Nhập từ khóa tìm kiếm video..." : (m_ytVk.query + " _");
    SDL_Color qCol = m_ytVk.query.empty() ? SDL_Color{75, 115, 135, 255} : SDL_Color{255, 255, 255, 255};
    drawText(dispQ, inX + 20, inY + (inH - textHeight(m_fontLarge)) / 2, qCol, m_fontLarge, false);

    // ─── Upper Section: Search History Chips (Y = 124 to 445) ───
    drawText("LỊCH SỬ TÌM KIẾM", 46, 124, {0, 180, 216, 255}, m_fontSmall, false);
    if (m_ytFocusInTags) {
        drawText("D-pad: Chọn từ khóa  |  [A]: Tìm ngay  |  [▼]: Xuống bàn phím", 500, 124, {0, 200, 83, 255}, m_fontSmall, false);
    } else {
        drawText("Bam [▲] tu hang phim tren cung de chon lich su", 610, 124, {120, 150, 170, 255}, m_fontSmall, false);
    }

    int tagCols = 4;
    int tagW = 222;
    int tagH = 46;
    int tagGapX = 14;
    int tagGapY = 14;
    int tagStartX = 46;
    int tagStartY = 152;

    int historyCount = std::min(8, static_cast<int>(m_ytSearchHistory.size()));
    for (int i = 0; i < historyCount; i++) {
        int col = i % tagCols;
        int row = i / tagCols;
        int tx = tagStartX + col * (tagW + tagGapX);
        int ty = tagStartY + row * (tagH + tagGapY);
        bool isFocused = (m_ytFocusInTags && m_ytSelectedTagIndex == i);

        std::string tagText = m_ytSearchHistory[i];
        if (tagText.length() > 22) tagText = tagText.substr(0, 20) + "..";

        if (isFocused) {
            drawRoundedRect(tx, ty, tagW, tagH, UiTheme::RADIUS_CARD, {0, 180, 80, 240}, true);
            drawRoundedBorder(tx, ty, tagW, tagH, UiTheme::RADIUS_CARD, {140, 255, 180, 255}, 2);
            drawText(tagText, tx + tagW / 2, ty + (tagH - textHeight(m_fontMedium)) / 2, {255, 255, 255, 255}, m_fontMedium, true);
        } else {
            drawRoundedRect(tx, ty, tagW, tagH, UiTheme::RADIUS_CARD, {14, 32, 44, 210}, true);
            drawRoundedBorder(tx, ty, tagW, tagH, UiTheme::RADIUS_CARD, {26, 62, 82, 255}, 1);
            drawText(tagText, tx + tagW / 2, ty + (tagH - textHeight(m_fontSmall)) / 2, {200, 225, 238, 255}, m_fontSmall, true);
        }
    }

    // Divider line above keyboard
    drawRect(46, 444, 932, 1, {20, 48, 64, 180}, true);

    // Compact Virtual Keyboard (Pinned to bottom, Y = 456 to 708)
    const char* ytActs[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    {
        static std::string s0, s1, s2, s3, s4;
        s0 = m_ytVk.shift ? "HOA" : "Thuong";
        s1 = m_ytVk.telexMode ? "TELEX" : "US";
        s2 = "Cach"; s3 = "Xoa"; s4 = "Tim";
        ytActs[0] = s0.c_str(); ytActs[1] = s1.c_str(); ytActs[2] = s2.c_str();
        ytActs[3] = s3.c_str(); ytActs[4] = s4.c_str();
        VkState ytDraw = m_ytVk;
        if (m_ytFocusInTags) ytDraw.row = -1;
        m_ui.drawVirtualKeyboard(ytDraw, 46, 456, 86, 44, 8, 6,
            SDL_Color{0, 200, 83, 255}, SDL_Color{140, 255, 190, 255}, ytActs, true, true, 2);
    }
    // ─── Bottom Bar ───
    drawRect(0, 716, 1024, 52, {9, 20, 28, 255}, true);
    if (m_ytFocusInTags) {
        drawBadge(46, 726, 120, 30, "[A] Tìm ngay", {0, 200, 83, 255}, {255, 255, 255, 255});
        drawAppFooter({{UiTheme::PadBtn::A, "Tìm"}, {UiTheme::PadBtn::UPDOWN, "Bàn phím"}});
    } else {
        drawBadge(46, 726, 82, 30, "[A] OK", {0, 200, 83, 255}, {255, 255, 255, 255});
        drawAppFooter({{UiTheme::PadBtn::X, "Cách"}, {UiTheme::PadBtn::Y, "Xóa"}, {UiTheme::PadBtn::L1, "Hoa"}, {UiTheme::PadBtn::R1, "Telex"}, {UiTheme::PadBtn::START, "Tìm"}});
    }

    // Searching modal overlay
    if (m_ytIsSearching) {
        beginModalDim();
        drawRoundedRect(280, 290, 464, 140, UiTheme::RADIUS_ROW, SDL_Color{14, 38, 50, 255}, true);
        drawRoundedBorder(280, 290, 464, 140, UiTheme::RADIUS_ROW, {0, 200, 83, 255}, 2);
        drawText("ĐANG TÌM KIẾM...", 512, 325, {0, 200, 83, 255}, m_fontMedium, true);
        std::string qText = "\"" + m_ytVk.query + "\"";
        if (qText.length() > 36) qText = qText.substr(0, 33) + "...\"";
        drawText(qText, 512, 370, {240, 240, 240, 255}, m_fontSmall, true);
    }
}

void UIManager::renderYouTubeResultsState() {
    // RAM Management: giu thumbnail dang hien, xoa phan con lai khi vuot 24 (LRU 24).
    if (m_ytThumbCache.size() > 24) {
        std::unordered_set<std::string> currentVisible;
        for (const auto& item : m_ytSearchResults) {
            size_t p = item.find('|');
            if (p != std::string::npos) currentVisible.insert(item.substr(0, p));
        }
        m_ytThumbCache.retainOnly(currentVisible);
    }

    // Pure dark background matching Image 1
    drawAppBackground();

    // Top Bar: YouTube Logo + Search Query on top left
    int logoH = 45;
    int logoW = 45;
    std::string ytLogoPath = AppConfig::instance().getAssetsDir() + "/apps_icons/YOUTUBE.png";
    SDL_Texture* ytLogo = m_ui.getOrLoadImage("grid/YOUTUBE.png", ytLogoPath);
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

    std::string queryDisplay = m_ytVk.query.empty() ? "" : (": " + m_ytVk.query);
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
        drawRoundedRect(cx, cy, cardW, cardH, UiTheme::RADIUS_CARD, cardBg, true);

        // Selected Card Focus: Crisp White Border
        if (isSelected) {
            drawRoundedBorder(cx, cy, cardW, cardH, UiTheme::RADIUS_CARD, {255, 255, 255, 255}, 2);
        }

        auto parts = split(m_ytSearchResults[idx], '|');
        std::string vid = !parts.empty() ? parts[0] : "";

        // Thumbnail (16:9 with rounded border)
        int thumbX = cx + pad;
        int thumbY = cy + pad;

        if (!vid.empty()) {
            if (!m_ytThumbCache.contains(vid)) {
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
                            int iw = surf->w, ih = surf->h;
                            SDL_FreeSurface(surf);
                            if (tex) m_ytThumbCache.put(vid, tex, iw, ih);
                        }
                    }
                }
            }
        }

        SDL_Texture* thumbTex = !vid.empty() ? m_ytThumbCache.get(vid) : nullptr;
        if (thumbTex) {
            int stx = PlatformInfo::instance().scaleX(thumbX);
            int sty = PlatformInfo::instance().scaleY(thumbY);
            int stw = PlatformInfo::instance().scaleW(thumbW);
            int sth = PlatformInfo::instance().scaleH(thumbH);
            SDL_Rect dstRect = {stx, sty, stw, sth};
            SDL_RenderCopy(m_renderer, thumbTex, nullptr, &dstRect);
            drawRoundedBorder(thumbX, thumbY, thumbW, thumbH, UiTheme::RADIUS_ROW, {45, 45, 45, 180}, 1);
        } else {
            drawRoundedRect(thumbX, thumbY, thumbW, thumbH, UiTheme::RADIUS_ROW, SDL_Color{32, 32, 32, 255}, true);
            drawText("YouTube", thumbX + thumbW / 2, thumbY + (thumbH - textHeight(m_fontSmall)) / 2,
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
            drawText(durStr, pillX + pillW / 2, pillY + (pillH - textHeight(m_fontSmall)) / 2, {255, 255, 255, 255}, m_fontSmall, true);
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

    drawAppFooter({{UiTheme::PadBtn::X, "Tìm"}, {UiTheme::PadBtn::L1, "Trang"}, {UiTheme::PadBtn::R1, "Trang"}});

    // Resolving stream overlay (Borderless)
    if (m_ytIsLoadingVideo) {
        beginModalDim();
        drawRoundedRect(272, 285, 480, 150, UiTheme::RADIUS_MODAL, SDL_Color{28, 28, 28, 255}, true);
        drawText("ĐANG TẢI VIDEO...", 512, 325, {255, 255, 255, 255}, m_fontMedium, true);
        drawText("Đang kết nối luồng phát...", 512, 372, {180, 190, 205, 255}, m_fontSmall, true);
    }
}

// =============================================================
// LocalSend render functions
// =============================================================
void UIManager::renderLocalSendHome() {
    // IP cua Brick nay (goc tren ben phai) de phan biet 2 may khi test.
    {
        std::string myIp = LocalSendManager::instance().ownIp();
        if (myIp.empty() || myIp == "0.0.0.0") myIp = LsUtil::getOwnIp("wlan0");
        if (!myIp.empty()) {
            drawBadge(1024 - 220, 14, 196, 30, myIp,
                      {30, 41, 59, 255}, {148, 163, 184, 255});
        }
    }
    // ===== Layout goc LocalSend: sidebar trai + content phai =====
    // Sidebar
    int sbW = 284;
    drawRect(0, 0, sbW, 768, {13, 17, 23, 255}, true);
    drawText("LocalSend", sbW/2, 110, {255, 255, 255, 255}, m_fontTitle, true);
    // m_localSendMode: 0=SEND, 1=RECEIVE — sidebar hien thi Receive tren, Send duoi
    const char* sbLabels[2] = {"Receive", "Send"};
    for (int i = 0; i < 2; ++i) {
        int modeVal = (i == 0) ? 1 : 0;  // row0=Receive(1), row1=Send(0)
        bool sel = (m_localSendMode == modeVal);
        int ry = 215 + i*62;
        if (sel) drawRoundedRect(14, ry, sbW-28, 52, 26, {55, 71, 79, 255}, true);
        // Icon PNG: Send -> assets/player_icons/send.png, Receive -> receive.png
        const char* iconFile = (i == 0) ? "receive" : "send";
        drawPlayerIcon(iconFile, 40, ry+8, 36, 36);
        drawText(sbLabels[i], 96, ry+10, {255,255,255,255}, m_fontMedium, false);
    }
    drawText("Settings", 96, 215+2*62+10, {180,180,180,255}, m_fontMedium, false);
    // Content phai
    int cx = sbW + 50;
    int cw = 1024 - cx - 40;
    if (m_localSendMode == 1) {
        // ===== RECEIVE: hien thi trang thai lang nghe =====
        drawText("Receive", cx, 120, {255,255,255,255}, m_fontLarge, false);
        int stY = 180;
        drawRoundedRect(cx, stY, cw, 130, UiTheme::RADIUS_MODAL, {55, 71, 79, 255}, true);
        std::string myAlias = LocalSendManager::instance().alias();
        drawText(myAlias.empty() ? "TrimUI" : myAlias, cx+30, stY+20, {255,255,255,255}, m_fontLarge, false);
        std::string tgt = LocalSendManager::instance().currentTargetFolder();
        drawText(tgt.empty() ? "Đang lắng nghe..." : tgt, cx+30, stY+64, {180,200,190,255}, m_fontSmall, false);
        drawText("Mở LocalSend trên máy khác và gử file tới thiết bị này.", cx, stY+160, {150,150,150,255}, m_fontSmall, false);
    } else {
        // ===== SEND: Nearby devices (Selection đã bỏ — A vào thẳng picker ROM) =====
        drawText("Nearby devices", cx, 118, {255,255,255,255}, m_fontLarge, false);
        int ny = 118 + 48;
        auto devices = LocalSendManager::instance().knownDevices();
        int devCount = (int)devices.size();
        if (devCount == 0) {
            drawRoundedRect(cx, ny, cw, 96, UiTheme::RADIUS_MODAL, {55, 71, 79, 255}, true);
            drawText("Đang tìm thiết bị...", cx+cw/2, ny+22, {200,200,200,255}, m_fontMedium, true);
            drawText("Hãy mở LocalSend trên máy khác", cx+cw/2, ny+56, {150,150,150,255}, m_fontSmall, true);
        } else {
            for (int i = 0; i < devCount; ++i) {
                int dy = ny + i*108;
                if (dy + 96 > 640) break;
                const auto& d = devices[i];
                bool sel = (i == m_localSendSelectedDevice);
                drawRoundedRect(cx, dy, cw, 96, UiTheme::RADIUS_MODAL, {55, 71, 79, 255}, true);
                if (sel) drawRoundedBorder(cx, dy, cw, 96, UiTheme::RADIUS_MODAL, {0, 200, 150, 255}, 3);
                // icon phone don gian
                drawRoundedBorder(cx+22, dy+18, 34, 60, UiTheme::RADIUS_ROW, {255,255,255,255}, 3);
                std::string nm = d.alias.empty() ? d.ip : d.alias;
                if ((int)nm.size() > 26) nm = nm.substr(0, 24) + "..";
                drawText(nm, cx+72, dy+16, {255,255,255,255}, m_fontLarge, false);
                std::string sub = d.deviceModel.empty() ? (d.ip + ":" + std::to_string(d.port)) : d.deviceModel;
                drawBadge(cx+72, dy+56, 86, 26, "HTTP", {120,130,135,255}, {40,50,55,255});
                drawText(sub, cx+170, dy+58, {180,190,185,255}, m_fontSmall, false);
            }
        }
        drawText("Troubleshoot", cx+cw/2, 648, {120, 200, 190, 255}, m_fontSmall, true);
        drawText("Hãy chắc chắn máy đích cùng mạng Wi-Fi.", cx+cw/2, 680, {140,140,140,255}, m_fontSmall, true);
    }
    // Footer hint
    if (m_localSendMode == 0) {
        drawAppFooter({{UiTheme::PadBtn::L1R1, "Chọn"}, {UiTheme::PadBtn::A, "Chọn"}, {UiTheme::PadBtn::Y, "Tải lại"}});
    } else {
        drawAppFooter({{UiTheme::PadBtn::X, "Chọn đích"}, {UiTheme::PadBtn::Y, "Tải lại"}});
    }
}

void UIManager::renderLocalSendIncomingDialog() {
    beginModalDim();
    int dlgX = 122, dlgY = 144, dlgW = 780, dlgH = 480;
    drawRoundedRect(dlgX, dlgY, dlgW, dlgH, UiTheme::RADIUS_MODAL, SDL_Color{15, 23, 42, 245}, true);

    drawText("CÓ FILE ĐẾN", dlgX + dlgW/2, dlgY + 36, {250, 204, 21, 255}, m_fontTitle, true);

    drawText("Từ thiết bị:", dlgX + 40, dlgY + 110, {148, 163, 184, 255}, m_fontMedium, false);
    drawText(m_localSendCurrentPrompt.fromAlias, dlgX + 40, dlgY + 142,
             {255, 255, 255, 255}, m_fontLarge, false);
    drawText("(" + m_localSendCurrentPrompt.fromIp + ")",
             dlgX + dlgW - 40, dlgY + 142, {148, 163, 184, 255}, m_fontMedium, true);

    // ---- Nếu sender gửi kèm game metadata (mở rộng RomCloud) → render cover + tên game
    const auto& file = m_localSendCurrentPrompt.file;
    if (file.gameId > 0) {
        // Cover bên trái
        int cvX = dlgX + 40, cvY = dlgY + 200, cvW = 180, cvH = 240;
        // Build temp GameRecord để CoverManager resolve
        GameRecord tmpG;
        tmpG.id = file.gameId;
        tmpG.systemId = file.systemId;
        tmpG.title = file.gameTitle;
        tmpG.filename = file.fileName;
        tmpG.coverPath = file.coverPath;
        // Resolve system info
        SystemRecord sys;
        if (!file.systemCode.empty() && DatabaseManager::instance().getSystemById(file.systemId, sys)) {
            // OK
        }
        int texW=0, texH=0;
        SDL_Texture* tex = CoverManager::instance().getCoverTexture(tmpG, sys, texW, texH);
        if (tex) {
            // Fit cover into cvW x cvH (preserve aspect ratio: cover = 4:3 thường)
            SDL_Rect dst{cvX, cvY, cvW, cvH};
            SDL_RenderCopy(m_renderer, tex, nullptr, &dst);
        } else {
            // Placeholder box với system code
            drawRoundedRect(cvX, cvY, cvW, cvH, UiTheme::RADIUS_CARD, SDL_Color{30, 41, 59, 255}, true);
            drawText(file.systemCode.empty() ? "?" : file.systemCode,
                     cvX + cvW/2, cvY + cvH/2 - 12, {100, 116, 139, 255}, m_fontLarge, true);
        }

        // Game title lớn (bên phải cover)
        int txtX = cvX + cvW + 30;
        std::string dispTitle = file.gameTitle.empty() ? file.fileName : file.gameTitle;
        drawText(dispTitle, txtX, cvY, {34, 197, 94, 255}, m_fontLarge, false);

        // System
        if (!file.systemName.empty() || !file.systemCode.empty()) {
            std::string sysStr = "Hệ máy: " + (file.systemName.empty() ? file.systemCode : file.systemName);
            if (!file.systemCode.empty()) sysStr += "  (" + file.systemCode + ")";
            drawText(sysStr, txtX, cvY + 42, {250, 204, 21, 255}, m_fontMedium, false);
        }

        // Filename
        drawText("File: " + file.fileName, txtX, cvY + 78, {148, 163, 184, 255}, m_fontSmall, false);

        // Size + dung lượng trống (đỏ nếu thiếu chỗ)
        std::string sizeStr = "Kích thước: " + LsUtil::humanSize(file.size);
        uint64_t freeB0 = LsUtil::sdFreeBytes("/mnt/SDCARD");
        sizeStr += " | Trống: " + LsUtil::humanSize(freeB0);
        drawText(sizeStr, txtX, cvY + 102,
                 (file.size > 0 && freeB0 < file.size)
                     ? SDL_Color{239, 68, 68, 255} : SDL_Color{203, 213, 225, 255},
                 m_fontMedium, false);

        // Chọn chỗ lưu tay (Up/Down) — 0 = Auto
        {
            static const char* kLbl[] = {"Auto", "GBA", "NES", "SNES", "PS1",
                                         "MD", "N64", "Inbox", "RetroArch", "system"};
            int idx = m_localSendIncomingFolderIdx;
            if (idx < 0 || idx > 9) idx = 0;
            drawText(std::string("Lưu vào: < ") + kLbl[idx] + " >",
                     dlgX + 40, dlgY + dlgH - 108,
                     SDL_Color{250, 204, 21, 255}, m_fontMedium, false);
        }

        // Saved path
        drawText("Sẽ lưu vào:", dlgX + 40, dlgY + dlgH - 60, {148, 163, 184, 255}, m_fontSmall, false);
        drawText(m_localSendCurrentPrompt.savedPath, dlgX + 40, dlgY + dlgH - 36,
                 {34, 197, 94, 255}, m_fontSmall, false);
    } else {
        // Fallback: raw file (không có game metadata)
        drawText("File:", dlgX + 40, dlgY + 200, {148, 163, 184, 255}, m_fontMedium, false);
        drawText(file.fileName, dlgX + 40, dlgY + 232,
                 {34, 197, 94, 255}, m_fontLarge, false);
        std::string sizeStr = "Kích thước: " + LsUtil::humanSize(file.size);
        uint64_t freeB1 = LsUtil::sdFreeBytes("/mnt/SDCARD");
        sizeStr += " | Trống: " + LsUtil::humanSize(freeB1);
        drawText(sizeStr, dlgX + 40, dlgY + 280,
                 (file.size > 0 && freeB1 < file.size)
                     ? SDL_Color{239, 68, 68, 255} : SDL_Color{203, 213, 225, 255},
                 m_fontMedium, false);

        if (!file.relativePath.empty()) {
            drawText("Đường dẫn: " + file.relativePath,
                     dlgX + 40, dlgY + 310, {148, 163, 184, 255}, m_fontMedium, false);
        }
        {
            static const char* kLbl[] = {"Auto", "GBA", "NES", "SNES", "PS1",
                                         "MD", "N64", "Inbox", "RetroArch", "system"};
            int idx = m_localSendIncomingFolderIdx;
            if (idx < 0 || idx > 9) idx = 0;
            drawText(std::string("Lưu vào: < ") + kLbl[idx] + " >  (Up/Down đổi)",
                     dlgX + 40, dlgY + 338, SDL_Color{250, 204, 21, 255}, m_fontMedium, false);
        }
        drawText("Sẽ lưu vào:", dlgX + 40, dlgY + 368, {148, 163, 184, 255}, m_fontMedium, false);
        drawText(m_localSendCurrentPrompt.savedPath, dlgX + 40, dlgY + 398,
                 {34, 197, 94, 255}, m_fontMedium, false);
    }

    // Footer với icon nút A (đồng ý) và B (từ chối) — pattern giống IPTV
    drawAppFooter({{UiTheme::PadBtn::A, "Chọn"}});
}

void UIManager::startLsFolderRename(bool existing) {
    m_lsFolderRenameExisting = existing;
    m_lsFolderRenameOriginal.clear();
    std::string init;
    if (existing) {
        if (m_localSendFolderSelected < 0 ||
            m_localSendFolderSelected >= (int)m_localSendFolderEntries.size()) {
            showToast("Chưa chọn thư mục để đổi tên", {239, 68, 68, 255}, 1500);
            return;
        }
        m_lsFolderRenameOriginal = m_localSendFolderEntries[m_localSendFolderSelected];
        init = m_lsFolderRenameOriginal.substr(
            m_lsFolderRenameOriginal.find_last_of('/') + 1);
    } else {
        init = suggestNewFolderName(m_localSendFolderCurrentPath);
    }
    VirtualKeyboard::reset(m_lsFolderVk, false);
    m_lsFolderVk.query = init;
    m_lsFolderRenaming = true;
}

void UIManager::cancelLsFolderRename() {
    m_lsFolderRenaming = false;
    m_lsFolderVk.query.clear();
    m_lsFolderRenameOriginal.clear();
}

static std::string lsTrimName(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

void UIManager::commitLsFolderRename() {
    std::string name = lsTrimName(m_lsFolderVk.query);
    if (name.empty() || name.find('/') != std::string::npos ||
        name == "." || name == "..") {
        showToast("Tên không hợp lệ", {239, 68, 68, 255}, 1500);
        return;
    }
    if (m_lsFolderRenameExisting) {
        std::string parent = m_lsFolderRenameOriginal.substr(
            0, m_lsFolderRenameOriginal.find_last_of('/'));
        std::string dst = parent + "/" + name;
        if (dst == m_lsFolderRenameOriginal) { cancelLsFolderRename(); return; }
        if (FileSystemManager::instance().directoryExists(dst) ||
            FileSystemManager::instance().fileExists(dst)) {
            showToast("Tên đã tồn tại", {239, 68, 68, 255}, 1500);
            return;
        }
        if (std::rename(m_lsFolderRenameOriginal.c_str(), dst.c_str()) == 0) {
            showToast("Đã đổi tên: " + name, {34, 197, 94, 255}, 1500);
            m_localSendFolderLoaded = false;
            m_lsFolderRenaming = false;
        } else {
            showToast("Lỗi: không đổi tên được", {239, 68, 68, 255}, 1500);
        }
    } else {
        std::string newPath = m_localSendFolderCurrentPath + "/" + name;
        if (FileSystemManager::instance().directoryExists(newPath)) {
            showToast("Tên đã tồn tại", {239, 68, 68, 255}, 1500);
            return;
        }
        if (FileSystemManager::instance().createDirectoryRecursive(newPath)) {
            showToast("Đã tạo thư mục: " + name, {34, 197, 94, 255}, 1500);
            m_localSendFolderLoaded = false;
            m_lsFolderRenaming = false;
        } else {
            showToast("Lỗi: không tạo được thư mục", {239, 68, 68, 255}, 1500);
        }
    }
}

bool UIManager::handleLsFolderKeyboardInput() {
    InputManager& input = InputManager::instance();
    VkState& vk = m_lsFolderVk;
    if (input.isButtonJustPressed(Button::B)) { cancelLsFolderRename(); return true; }
    if (input.isButtonJustPressed(Button::UP)) {
        VirtualKeyboard::move(vk, -1, 0);
        return true;
    }
    if (input.isButtonJustPressed(Button::DOWN)) {
        VirtualKeyboard::move(vk, 1, 0);
        return true;
    }
    if (input.isButtonJustPressed(Button::LEFT)) {
        VirtualKeyboard::move(vk, 0, -1);
        return true;
    }
    if (input.isButtonJustPressed(Button::RIGHT)) {
        VirtualKeyboard::move(vk, 0, 1);
        return true;
    }
    if (input.isButtonJustPressed(Button::X)) {
        VirtualKeyboard::backspace(vk);
        return true;
    }
    if (input.isButtonJustPressed(Button::Y)) {
        vk.shift = !vk.shift;
        return true;
    }
    if (input.isButtonJustPressed(Button::START)) { commitLsFolderRename(); return true; }
    if (input.isButtonJustPressed(Button::A)) {
        if (vk.row < 4) {
            char ch = VirtualKeyboard::charAt(vk);
            if (ch) VirtualKeyboard::typeChar(vk, ch);
        } else {
            int a = vk.col;
            if (a == 0) vk.shift = !vk.shift;
            else if (a == 1) VirtualKeyboard::typeSpace(vk);
            else if (a == 2) VirtualKeyboard::backspace(vk);
            else if (a == 3) commitLsFolderRename();
            else cancelLsFolderRename();
        }
        return true;
    }
    return true;
}

void UIManager::renderLsFolderKeyboard() {
    // Overlay ban phim QWERTY tai dung layout YouTube, tone xanh IPTV
    drawAppBackground();
    drawRect(0, 0, 1024, 100, {15, 23, 42, 255}, true);
    drawRect(0, 100, 1024, 1, {30, 41, 59, 255}, true);
    drawGridIcon("FOLDER.png", 32, 22, 52, 52);
    drawText(m_lsFolderRenameExisting ? "ĐỔI TÊN THƯ MỤC" : "THƯ MỤC MỚI",
             96, 24, {255, 255, 255, 255}, m_fontLarge, false);
    drawText(m_localSendFolderCurrentPath, 96, 66,
             {148, 163, 184, 255}, m_fontSmall, false);
    // O nhap ten
    drawRect(48, 130, 928, 64, {15, 23, 42, 255}, true);
    drawBorder(48, 130, 928, 64, {59, 130, 246, 255}, 2);
    std::string shown = m_lsFolderVk.query.empty() ? "Nhập tên..." : m_lsFolderVk.query;
    drawText(shown, 68, 146,
             m_lsFolderVk.query.empty() ? SDL_Color{100, 116, 139, 255}
                                           : SDL_Color{255, 255, 255, 255},
             m_fontLarge, false);
    { static std::string ls0,ls1,ls2,ls3,ls4; ls0="Shift"; ls1="Space"; ls2="Xoa"; ls3="Xong"; ls4="Huy"; const char* lsA[5]={ls0.c_str(),ls1.c_str(),ls2.c_str(),ls3.c_str(),ls4.c_str()}; int lsCw=(1024-96-8*9)/10; m_ui.drawVirtualKeyboard(m_lsFolderVk,48,220,lsCw,64,8,10,SDL_Color{37,99,235,255},SDL_Color{147,197,253,255},lsA,false,false,1); }
    drawRect(0, 715, 1024, 53, {15, 23, 42, 255}, true);
    drawRect(0, 715, 1024, 1, {30, 41, 59, 255}, true);
    drawAppFooter({{UiTheme::PadBtn::X, "Xóa"}, {UiTheme::PadBtn::Y, "Hoa"}, {UiTheme::PadBtn::START, "Xong"}});
}

void UIManager::renderLocalSendFolderPicker() {
    // Ban phim rename/new-folder uu tien render overlay
    if (m_lsFolderRenaming) { renderLsFolderKeyboard(); return; }
    // ===== File Explorer full-screen, tone xanh IPTV, khong border ngoai =====
    drawAppBackground();

    // Header bar kieu IPTV
    drawRect(0, 0, 1024, 112, {15, 23, 42, 255}, true);
    drawRect(0, 112, 1024, 1, {30, 41, 59, 255}, true);
    drawGridIcon("FOLDER.png", 28, 24, 56, 56);
    drawText("FILE EXPLORER", 96, 24, {255, 255, 255, 255}, m_fontLarge, false);
    {
        std::string p = m_localSendFolderCurrentPath;
        const std::string root = "/mnt/SDCARD";
        if (p.compare(0, root.size(), root) == 0) p = p.substr(root.size());
        if (p.empty()) p = "/";
        if ((int)p.size() > 48) p = ".." + p.substr(p.size() - 46);
        drawText(p, 96, 68, {148, 163, 184, 255}, m_fontSmall, false);
    }
    {
        uint64_t freeB = LsUtil::sdFreeBytes("/mnt/SDCARD");
        drawText("Trống: " + LsUtil::humanSize(freeB), 996, 68,
                 {34, 197, 94, 255}, m_fontSmall, false);
    }

    // Scan folder con neu can
    if (!m_localSendFolderLoaded) {
        m_localSendFolderLoaded = true;
        m_localSendFolderEntries.clear();
        if (FileSystemManager::instance().directoryExists(m_localSendFolderCurrentPath)) {
            auto dirs = FileSystemManager::instance().listDirectory(
                m_localSendFolderCurrentPath, true);
            for (const auto& d : dirs) {
                if (!d.name.empty() && d.name[0] == '.') continue;
                m_localSendFolderEntries.push_back(d.path);
            }
        }
        if (m_localSendFolderSelected >= (int)m_localSendFolderEntries.size())
            m_localSendFolderSelected = 0;
    }

    // Cot trai: danh sach folder (flat, khong border)
    int leftX = 24, leftY = 128, leftW = 624, leftH = 574;
    drawRect(leftX, leftY, leftW, leftW > 0 ? leftH : leftH, {15, 23, 42, 255}, true);
    int listX = leftX + 12, listY = leftY + 12;
    int rowH = 56;
    int visibleRows = (leftH - 24) / rowH;
    int total = (int)m_localSendFolderEntries.size();

    if (total == 0) {
        drawText("(Thư mục trống)", leftX + leftW / 2,
                 listY + leftH / 2 - 10, {100, 116, 139, 255}, m_fontMedium, true);
    } else {
        int scroll = 0;
        if (m_localSendFolderSelected >= visibleRows)
            scroll = m_localSendFolderSelected - visibleRows + 1;
        for (int i = 0; i < total && i < visibleRows; ++i) {
            int idx = i + scroll;
            int y = listY + i * rowH;
            bool sel = (m_localSendFolderFocus == 0 && idx == m_localSendFolderSelected);
            if (sel) {
                drawRect(listX, y, leftW - 24, rowH - 6, {30, 58, 138, 255}, true);
                drawRect(listX, y, 4, rowH - 6, {59, 130, 246, 255}, true);
            }
            // Icon folder that (bo badge chu DIR)
            drawGridIcon("FOLDER.png", listX + 10, y + 8, 36, 36);
            const std::string& fp = m_localSendFolderEntries[idx];
            std::string nm = fp.substr(fp.find_last_of('/') + 1);
            if ((int)nm.size() > 26) nm = nm.substr(0, 25) + "..";
            drawText(nm, listX + 56, y + 12,
                     sel ? SDL_Color{255, 255, 255, 255}
                         : SDL_Color{203, 213, 225, 255},
                     m_fontMedium, false);
        }
    }

    // Cot phai: thong tin + nut thao tac (flat, tone xanh)
    int rightX = leftX + leftW + 16;
    int rightW = 1024 - rightX - 24;
    int rightY = leftY;
    int rightH = leftH;
    drawRect(rightX, rightY, rightW, rightH, {15, 23, 42, 255}, true);
    std::string folderName = m_localSendFolderCurrentPath.substr(
        m_localSendFolderCurrentPath.find_last_of('/') + 1);
    if (folderName.empty()) folderName = "SDCARD";
    if ((int)folderName.size() > 18) folderName = folderName.substr(0, 17) + "..";
    drawText(folderName, rightX + 16, rightY + 16,
             {255, 255, 255, 255}, m_fontLarge, false);
    drawText("Directory", rightX + 16, rightY + 56,
             {148, 163, 184, 255}, m_fontSmall, false);
    drawText(std::to_string(total) + " thư mục con", rightX + 16, rightY + 84,
             {148, 163, 184, 255}, m_fontSmall, false);
    // Hop goi y thao tac
    drawRect(rightX + 16, rightY + 124, rightW - 32, 120, {29, 78, 216, 255}, true);
    drawText("Y: thư mục mới", rightX + rightW / 2, rightY + 138,
             {255, 255, 255, 255}, m_fontSmall, true);
    drawText("START: đổi tên", rightX + rightW / 2, rightY + 164,
             {255, 255, 255, 255}, m_fontSmall, true);
    drawText("X: chọn làm đích", rightX + rightW / 2, rightY + 190,
             {219, 234, 254, 255}, m_fontSmall, true);
    // Nut Xac nhan (focus phai)
    {
        bool focusBtn = (m_localSendFolderFocus == 1);
        SDL_Color bg = focusBtn ? SDL_Color{22, 163, 74, 255} : SDL_Color{20, 83, 45, 255};
        drawRect(rightX + 16, rightY + rightH - 76, rightW - 32, 56, bg, true);
        if (focusBtn) drawBorder(rightX + 16, rightY + rightH - 76, rightW - 32, 56,
                                 {134, 239, 172, 255}, 2);
        drawInlineHintsCentered("[A] Chọn thư mục này", rightX + rightW / 2,
                                rightY + rightH - 62, {255, 255, 255, 255},
                                m_fontMedium, 26, 6);
    }

    // Footer: icon nut that + label, dong nhat UI
    drawRect(0, 715, 1024, 53, {15, 23, 42, 255}, true);
    drawRect(0, 715, 1024, 1, {30, 41, 59, 255}, true);
    drawAppFooter({{UiTheme::PadBtn::B, "Lùi"}, {UiTheme::PadBtn::X, "Chọn đích"}, {UiTheme::PadBtn::Y, "Thư mục mới"}, {UiTheme::PadBtn::START, "Đổi tên"}});
}
void UIManager::renderLocalSendSendPicker() {
    drawText("GỬI FILE ĐẾN THIẾT BỊ", 512, 50, {255, 255, 255, 255}, m_fontTitle, true);
    drawText("Tính năng đang phát triển — CLI script sẵn:", 512, 102, {148, 163, 184, 255}, m_fontMedium, true);

    int dlgX = 80, dlgY = 150, dlgW = 864, dlgH = 480;
    drawRoundedRect(dlgX, dlgY, dlgW, dlgH, UiTheme::RADIUS_MODAL, SDL_Color{15, 23, 42, 220}, true);

    auto devices = LocalSendManager::instance().knownDevices();
    if (devices.empty()) {
        drawText("Chưa tìm thấy thiết bị nào", dlgX + dlgW/2, dlgY + 100,
                 {148, 163, 184, 255}, m_fontLarge, true);
    } else {
        for (size_t i = 0; i < devices.size(); ++i) {
            int y = dlgY + 60 + (int)i * 60;
            drawText(devices[i].alias + "  (" + devices[i].ip + ")",
                     dlgX + 40, y, {203, 213, 225, 255}, m_fontMedium, false);
        }
    }

    drawText("Hiện tại gửi file bằng CLI:", dlgX + 40, dlgY + 330, {250, 204, 21, 255}, m_fontMedium, false);
    drawText("  bash /mnt/SDCARD/Apps/RomCloud/scripts/localsend_send.sh \\",
             dlgX + 40, dlgY + 370, {148, 163, 184, 255}, m_fontSmall, false);
    drawText("    <alias_or_ip> <file_path>",
             dlgX + 40, dlgY + 402, {148, 163, 184, 255}, m_fontSmall, false);

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
                RomIndexer::instance().scanAllSystems(AppConfig::instance().getRomsDir(), nullptr);
                refreshSystems();
            }
        } catch (...) {}
        // DÙNG CHUNG NGUỒN VỚI ĐỒNG BỘ/THƯ VIỆN: đọc DB (RomIndexer đã quét
        // /mnt/SDCARD/Roms/<SYSTEM> và lưu localPath). Như vậy số lượng ROM
        // ở màn SEND luôn khớp với số lượng đồng bộ thấy.
        // Nếu DB chưa có (chưa scan lần nào) thì fallback quét filesystem đệ quy.
        auto systems = DatabaseManager::instance().getSystems(false);
        for (const auto& sys : systems) {
            auto games = DatabaseManager::instance().getGamesBySystem(
                sys.id, static_cast<int>(GameState::LOCAL));
            for (const auto& g : games) {
                if (g.localPath.empty()) continue;
                if (!FileSystemManager::instance().fileExists(g.localPath)) continue;
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
            auto topNameOf = [&](const std::string& p) -> std::string {
                std::string rel = (p.compare(0, romsRoot.size(), romsRoot) == 0)
                    ? p.substr(romsRoot.size()) : p;
                while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
                auto sl = rel.find('/');
                std::string top = (sl == std::string::npos) ? rel : rel.substr(0, sl);
                return top.empty() ? std::string("Roms") : top;
            };
            while (!stack.empty()) {
                std::string cur = stack.back(); stack.pop_back();
                auto entries = FileSystemManager::instance().listDirectory(cur, false);
                for (const auto& f : entries) {
                    if (!f.name.empty() && f.name[0] == '.') continue;
                    if (f.isDirectory) { stack.push_back(f.path); continue; }
                    LsRomEntry e; e.path = f.path; e.name = f.name;
                    e.systemDir = topNameOf(cur); e.sizeBytes = f.sizeBytes;
                    m_lsRomList.push_back(e);
                }
            }
        }
        std::sort(m_lsRomList.begin(), m_lsRomList.end(), [](const LsRomEntry& a, const LsRomEntry& b){
            if (a.systemDir!=b.systemDir) return a.systemDir<b.systemDir; return a.name<b.name; });
        Logger::info("LocalSend ROMS SD scan: "+std::to_string(m_lsRomList.size()));
        m_lsRomSelected = 0;
        m_lsRomScrollOffset = 0;
    }

    // (Apps picker đã tạm ẩn — không quét /Apps ở màn SEND.)

    // ---- Title (Apps tab tạm ẩn: chỉ ROMs) ----
    drawText("CHỌN ROM ĐỂ GỬI", 512, 50, {255, 255, 255, 255}, m_fontTitle, true);

    auto devices = LocalSendManager::instance().knownDevices();
    std::string sub = "Gửi tới: ";
    if (!devices.empty() && m_localSendSelectedDevice >= 0 &&
        m_localSendSelectedDevice < (int)devices.size()) {
        sub += devices[m_localSendSelectedDevice].alias;
    } else {
        sub += "(chưa chọn thiết bị)";
    }
    drawText(sub, 512, 102, {148, 163, 184, 255}, m_fontMedium, true);

    int dlgX = 60, dlgY = 150, dlgW = 904, dlgH = 544;
    drawRoundedRect(dlgX, dlgY, dlgW, dlgH, UiTheme::RADIUS_MODAL, SDL_Color{15, 23, 42, 220}, true);

    {
        int n = (int)m_lsRomList.size();
        if (n == 0) {
            drawText("Chưa có game LOCAL nào trên thẻ nhớ.",
                     dlgX + dlgW/2, dlgY + dlgH/2 - 20,
                     {148, 163, 184, 255}, m_fontLarge, true);
            drawText("Bấm Y để làm mới sau khi quét ROM",
                     dlgX + dlgW/2, dlgY + dlgH/2 + 20,
                     {100, 116, 139, 255}, m_fontSmall, true);
        } else {
            renderLsRomsList(dlgX, dlgY, dlgW, dlgH);
        }
    }

    drawAppFooter({{UiTheme::PadBtn::A, "Trang"}, {UiTheme::PadBtn::Y, "Tải lại"}});
}

void UIManager::renderLsProgressRow(bool isSend, int idx, int x, int y, int w, bool sel) { m_dialogs.renderLsRow(m_ui, m_fontSmall, isSend, idx, x, y, w, sel); }

void UIManager::renderLocalSendProgress() { m_dialogs.renderLocalSendProgress(m_ui, m_fontSmall, m_fontMedium, m_fontLarge, m_fontTitle, m_localSendProgressSel, m_localSendProgressScroll); }

// Helper: render ROMS list với track thực tế Y để tránh overlap khi có system separator.
void UIManager::renderLsRomsList(int dlgX, int dlgY, int dlgW, int dlgH) {
    int n = (int)m_lsRomList.size();
    if (n == 0) return;
    const int headerH = 22;
    const int itemH = 60;
    const int padding = 12;
    const int pageSize = 5;
    int availH = dlgH - 2 * padding;
    auto drawnRowsFrom = [&](int startIdx) -> int {
        int rows = 0;
        std::string firstSys;
        bool first = true;
        for (int i = startIdx; i < n; ++i) {
            const auto& e = m_lsRomList[i];
            int need = itemH;
            if (first || e.systemDir != firstSys) need += headerH;
            if (rows * headerH + need > availH) break;
            rows = (rows * headerH + need + headerH - 1) / headerH;
            firstSys = e.systemDir; first = false;
            if ((int)(i - startIdx + 1) >= pageSize + 2) break;
        }
        return rows;
    };
    if (m_lsRomSelected < m_lsRomScrollOffset) m_lsRomScrollOffset = m_lsRomSelected;
    while (m_lsRomScrollOffset < n) {
        int drawn = drawnRowsFrom(m_lsRomScrollOffset);
        if (drawn == 0) { m_lsRomScrollOffset = std::min(n - 1, m_lsRomScrollOffset + 1); continue; }
        if (m_lsRomSelected < m_lsRomScrollOffset + drawn) break;
        m_lsRomScrollOffset++;
    }
    if (m_lsRomScrollOffset >= n) m_lsRomScrollOffset = std::max(0, n - 1);
    int y = dlgY + padding;
    std::string curSys;
    bool firstRow = true;
    int startIdx = m_lsRomScrollOffset;
    for (int i = startIdx; i < n; ++i) {
        const auto& e = m_lsRomList[i];
        int need = itemH;
        if (firstRow || e.systemDir != curSys) need += headerH;
        if (y + need > dlgY + dlgH - 8) break;
        if (firstRow || e.systemDir != curSys) {
            curSys = e.systemDir;
            drawText(curSys.empty() ? "ROMS" : curSys, dlgX + padding, y + 2, {16, 185, 129, 255}, m_fontSmall);
            y += headerH;
        }
        firstRow = false;
        bool sel = (i == m_lsRomSelected);
        int itemX = dlgX + padding;
        int itemW = dlgW - 2 * padding;
        if (sel) drawRoundedRect(itemX, y, itemW, itemH, UiTheme::RADIUS_ROW, {16, 185, 129, 255}, true);
        else drawRoundedRect(itemX, y, itemW, itemH, UiTheme::RADIUS_ROW, {30, 41, 59, 255}, true);
        int tx = itemX + 14;
        int ty = y + 5;
        SDL_Color titleC = sel ? SDL_Color{6, 40, 28, 255} : SDL_Color{241, 245, 249, 255};
        SDL_Color metaC = sel ? SDL_Color{6, 60, 40, 255} : SDL_Color{148, 163, 184, 255};
        std::string title = e.name;
        if ((int)title.size() > 42) title = title.substr(0, 40) + "..";
        drawText(title, tx, ty, titleC, m_fontSmall);
        std::string meta = curSys + "  |  " + FileSystemManager::instance().formatBytes(e.sizeBytes);
        drawText(meta, tx, ty + 26, metaC, m_fontSmall);
        y += itemH;
    }
    int totalPages = (n + pageSize - 1) / pageSize;
    int curPage = (m_lsRomSelected / pageSize) + 1;
    char pgbuf[64]; snprintf(pgbuf, sizeof(pgbuf), "%d / %d", curPage, totalPages);
    drawText(pgbuf, dlgX + dlgW - 90, dlgY + dlgH - 26, {100, 116, 139, 255}, m_fontSmall);
}

void UIManager::renderLsAppsList(int dlgX, int dlgY, int dlgW, int dlgH) {
    int n = (int)m_lsAppList.size();
    if (n == 0) return;
    const int itemH = 50;
    const int padding = 4;
    int visibleCount = std::max(1, (dlgH - padding * 2) / itemH);

    if (m_lsAppSelected < m_lsAppScrollOffset) m_lsAppScrollOffset = m_lsAppSelected;
    if (m_lsAppSelected >= m_lsAppScrollOffset + visibleCount)
        m_lsAppScrollOffset = m_lsAppSelected - visibleCount + 1;
    if (m_lsAppScrollOffset < 0) m_lsAppScrollOffset = 0;
    if (m_lsAppScrollOffset > std::max(0, n - 1)) m_lsAppScrollOffset = std::max(0, n - 1);

    int renderY = dlgY + padding;
    for (int i = m_lsAppScrollOffset; i < n && i < m_lsAppScrollOffset + visibleCount; ++i) {
        const auto& e = m_lsAppList[i];
        if (renderY + itemH > dlgY + dlgH - padding) break;

        bool sel = (i == m_lsAppSelected);
        if (sel) {
            drawRoundedRect(dlgX + 8, renderY, dlgW - 16, itemH - 4, UiTheme::RADIUS_ROW,
                            SDL_Color{59, 130, 246, 90}, true);
        }

        std::string icon = e.isDirectory ? "[DIR]" : "[FILE]";
        SDL_Color iconColor = e.isDirectory
            ? SDL_Color{250, 204, 21, 255}
            : SDL_Color{148, 163, 184, 255};
        drawText(icon, dlgX + 20, renderY + 14, iconColor, m_fontSmall, false);

        SDL_Color nameColor = sel ? SDL_Color{255, 255, 255, 255}
                                  : SDL_Color{226, 232, 240, 255};
        drawText(e.name, dlgX + 90, renderY + 4, nameColor, m_fontMedium, false);

        std::string sub = e.isDirectory ? std::string("Bấm A để mở")
                                        : LsUtil::humanSize(e.sizeBytes);
        drawText(sub, dlgX + 90, renderY + 28,
                 {148, 163, 184, 255}, m_fontSmall, false);

        renderY += itemH;
    }

    drawText(std::to_string(m_lsAppSelected + 1) + "/" + std::to_string(n),
             dlgX + dlgW - 20, dlgY - 22,
             {148, 163, 184, 255}, m_fontSmall, true);
}

} // namespace RomCloud
