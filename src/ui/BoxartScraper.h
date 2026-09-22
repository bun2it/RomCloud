#pragma once

#include <string>
#include <vector>
#include "../database/DatabaseManager.h"

namespace RomCloud {

class BoxartScraper {
public:
    static BoxartScraper& instance();

    // Map system code (e.g. "GBA", "FC") to Libretro system name
    std::string getLibretroSystemName(const std::string& systemCode);

    // Clean ROM filename/title according to Libretro thumbnail naming conventions
    std::string cleanNameForLibretro(const std::string& filenameOrTitle);

    // Scrape boxart cover for a specific game, saves to /mnt/SDCARD/Imgs/<sys>/<name>.png
    bool scrapeCover(const GameRecord& game, const SystemRecord& sys, std::string& outCoverPath);

private:
    BoxartScraper() = default;
};

} // namespace RomCloud
