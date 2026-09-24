#pragma once
#include "../database/DatabaseManager.h"
#include "../iptv/IPTVManager.h"
#include "CoverManager.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <vector>
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
  IPTV_LIST,
  IPTV_SEARCH,
  YOUTUBE_SEARCH,
  YOUTUBE_RESULTS,
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

  // IPTV State
  int m_selectedIPTVChannelIndex = 0;
  int m_iptvScrollOffset = 0;
  bool m_iptvShowFavoritesOnly = false;

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
  void renderIPTVState();
  void renderIPTVSearchState();
  void renderYouTubeSearchState();
  void renderYouTubeResultsState();

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
  void drawButtonIcon(const std::string &button, int x, int y, int size);
  void drawGridIcon(const std::string &iconFile, int x, int y, int w, int h);

  // Performance caches (60 FPS Smooth UI)
  std::unordered_map<std::string, SDL_Texture*> m_gridIconCache;
  std::unordered_map<std::string, SDL_Texture*> m_systemIconCache;

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
};

} // namespace RomCloud
