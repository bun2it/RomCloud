#include "BoxartScraper.h"
#include "../network/HttpClient.h"
#include "../filesystem/FileSystemManager.h"
#include "../logging/Logger.h"
#include "../config/AppConfig.h"

#include <algorithm>

namespace RomCloud {

BoxartScraper& BoxartScraper::instance() {
    static BoxartScraper instance;
    return instance;
}

std::string BoxartScraper::getLibretroSystemName(const std::string& systemCode) {
    std::string code = systemCode;
    std::transform(code.begin(), code.end(), code.begin(), ::toupper);

    if (code == "FC" || code == "NES") return "Nintendo - Nintendo Entertainment System";
    if (code == "SFC" || code == "SNES") return "Nintendo - Super Nintendo Entertainment System";
    if (code == "GBA") return "Nintendo - Game Boy Advance";
    if (code == "GB") return "Nintendo - Game Boy";
    if (code == "GBC") return "Nintendo - Game Boy Color";
    if (code == "MD" || code == "GENESIS") return "Sega - Mega Drive - Genesis";
    if (code == "PS" || code == "PSX") return "Sony - PlayStation";
    if (code == "N64") return "Nintendo - Nintendo 64";
    if (code == "NDS") return "Nintendo - Nintendo DS";
    if (code == "NEOGEO") return "SNK - Neo Geo";
    if (code == "MAME" || code == "ARCADE") return "FBNeo - Arcade Games";
    if (code == "CPS1") return "Capcom - CP System I";
    if (code == "CPS2") return "Capcom - CP System II";
    if (code == "CPS3") return "Capcom - CP System III";
    if (code == "PCE") return "NEC - PC Engine - TurboGrafx 16";
    if (code == "SMS") return "Sega - Master System - Mark III";
    if (code == "GG") return "Sega - Game Gear";
    if (code == "WS") return "Bandai - WonderSwan";
    if (code == "WSC") return "Bandai - WonderSwan Color";
    if (code == "ATARI" || code == "A2600") return "Atari - 2600";
    return "";
}

std::string BoxartScraper::cleanNameForLibretro(const std::string& filenameOrTitle) {
    std::string name = filenameOrTitle;
    size_t lastDot = name.find_last_of('.');
    if (lastDot != std::string::npos && lastDot > 0) {
        name = name.substr(0, lastDot);
    }

    // Libretro naming conventions replace illegal filesystem characters with '_'
    // Reference: https://docs.libretro.com/guides/roms-playlists-thumbnails/
    for (char& c : name) {
        if (c == '&') c = '_';
        else if (c == '/' || c == '\\' || c == '*' || c == '?' || c == '\"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    return name;
}

bool BoxartScraper::scrapeCover(const GameRecord& game, const SystemRecord& sys, std::string& outCoverPath) {
    std::string libretroSys = getLibretroSystemName(sys.code);
    if (libretroSys.empty()) {
        Logger::warn("BoxartScraper: No Libretro system mapping for code: " + sys.code);
        return false;
    }

    std::string cleanName = cleanNameForLibretro(game.filename);
    if (cleanName.empty()) cleanName = cleanNameForLibretro(game.title);

    std::string targetDir = AppConfig::instance().getImgsDir() + "/" + sys.code;
    FileSystemManager::instance().createDirectoryRecursive(targetDir);
    std::string targetPath = targetDir + "/" + cleanName + ".png";

    // If file already exists locally, reuse it
    if (FileSystemManager::instance().fileExists(targetPath)) {
        outCoverPath = targetPath;
        return true;
    }

    // Try types: Named_Boxarts -> Named_Titles -> Named_Snaps
    const char* subdirs[] = { "Named_Boxarts", "Named_Titles", "Named_Snaps" };

    for (const char* subdir : subdirs) {
        std::string encodedSys = HttpClient::instance().urlEncode(libretroSys);
        std::string encodedName = HttpClient::instance().urlEncode(cleanName);

        // Libretro uses %20 for spaces in CDN URL
        std::string url = "https://thumbnails.libretro.com/" + encodedSys + "/" + subdir + "/" + encodedName + ".png";

        Logger::info("BoxartScraper: Trying " + url);
        HttpResponse resp = HttpClient::instance().get(url, {}, 5000);

        if (resp.statusCode == 200 && !resp.body.empty() && resp.body.size() > 512) {
            FILE* fp = fopen(targetPath.c_str(), "wb");
            if (fp) {
                fwrite(resp.body.data(), 1, resp.body.size(), fp);
                fclose(fp);
                outCoverPath = targetPath;
                Logger::info("BoxartScraper: Successfully saved cover to " + targetPath);
                return true;
            }
        }
    }

    return false;
}

} // namespace RomCloud
