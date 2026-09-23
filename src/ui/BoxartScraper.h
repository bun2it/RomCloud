#pragma once

#include <string>
#include <vector>
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
};

class BoxartScraper {
public:
    static BoxartScraper& instance();

    // Map system code (e.g. "GBA", "FC") to Libretro system name
    std::string getLibretroSystemName(const std::string& systemCode);

    // Clean ROM filename/title according to Libretro thumbnail naming conventions
    std::string cleanNameForLibretro(const std::string& filenameOrTitle);

    // Clean ROM filename into a clean search query for metadata APIs (Wikipedia)
    std::string cleanSearchQuery(const std::string& filenameOrTitle);

    // Scrape boxart cover for a specific game, saves to /mnt/SDCARD/Imgs/<sys>/<name>.png
    bool scrapeCover(const GameRecord& game, const SystemRecord& sys, std::string& outCoverPath);

    // Scrape both boxart cover AND rich metadata (year, genre, developer, description)
    GameScrapeResult scrapeGameInfo(const GameRecord& game, const SystemRecord& sys);

private:
    BoxartScraper() = default;

    bool downloadCoverFromUrl(const std::string& url, const std::string& targetPath);
};

} // namespace RomCloud
