#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include "../database/DatabaseManager.h"

namespace RomCloud {

struct GameScrapeResult {
    bool success = false;
    bool coverFound = false;
    std::string coverPath;
    std::string title;
    std::string releaseYear;
    std::string developer;
    std::string genre;
    std::string description;
    std::string source; // "screenscraper" or "libretro"
};

struct ScrapeCandidate {
    std::string title;
    std::string releaseYear;
    std::string developer;
    std::string genre;
    std::string description;
    std::string coverUrl;
    std::string source;
};

struct AutoScrapeStatus {
    bool isScraping = false;
    int totalGames = 0;
    int scrapedCount = 0;
    int successCount = 0;
    std::string currentGame;
    std::string currentSystem;
    int progressPct = 0;
    std::string lastMessage;
};

class BoxartScraper {
public:
    static BoxartScraper& instance();
    ~BoxartScraper();

    // Map system code (e.g. "GBA", "FC", "NEOGEO") to ScreenScraper system ID
    int getScreenScraperSystemId(const std::string& systemCode);

    // Map system code to Libretro system name
    std::string getLibretroSystemName(const std::string& systemCode);

    // Translate short arcade zip names (mslug, kof97, dino) to full title
    std::string resolveArcadeTitle(const std::string& romName);

    // Clean ROM filename/title according to Libretro thumbnail naming conventions
    std::string cleanNameForLibretro(const std::string& filenameOrTitle);

    // Clean ROM title by removing tags like (USA), (Japan), [!], (v1.1)
    std::string cleanRomTitle(const std::string& filenameOrTitle);

    // Generate smart candidate variations for Libretro CDN (e.g. USA, World, Europe, etc.)
    std::vector<std::string> generateLibretroCandidates(const std::string& filenameOrTitle, const std::string& systemCode);

    // Extract release year from filename tags e.g. (1996) or [1994]
    std::string extractYearFromFilename(const std::string& filename);

    // Scrape boxart cover for a specific game, saves to /mnt/SDCARD/Imgs/<sys>/<name>.png
    bool scrapeCover(const GameRecord& game, const SystemRecord& sys, std::string& outCoverPath);

    // Scrape both boxart cover AND rich metadata (ScreenScraper + Libretro, NO Wikipedia)
    GameScrapeResult scrapeGameInfo(const GameRecord& game, const SystemRecord& sys);

    // Search candidates for manual selection (like Scrape-Edit)
    std::vector<ScrapeCandidate> searchCandidates(const std::string& query, const std::string& systemCode);

    // Apply a selected candidate to a game
    bool applyCandidate(int64_t gameId, const ScrapeCandidate& candidate);

    // Background auto-scraper for all ROMs on the SD card
    bool startAutoScrapeSdCard(bool forceAll = false);
    void cancelAutoScrape();
    AutoScrapeStatus getAutoScrapeStatus();

    // Test ScreenScraper credentials
    bool testScreenScraperAuth(const std::string& user, const std::string& pass,
                               const std::string& devId, const std::string& devPass,
                               std::string& outError);

private:
    BoxartScraper();

    bool scrapeFromScreenScraper(const GameRecord& game, const SystemRecord& sys, GameScrapeResult& outResult);
    bool scrapeFromLibretro(const GameRecord& game, const SystemRecord& sys, GameScrapeResult& outResult);
    bool downloadCoverFromUrl(const std::string& url, const std::string& targetPath);

    std::thread m_autoScrapeThread;
    std::atomic<bool> m_isScraping{false};
    std::atomic<bool> m_cancelRequested{false};
    std::mutex m_statusMutex;
    AutoScrapeStatus m_status;
};

} // namespace RomCloud
