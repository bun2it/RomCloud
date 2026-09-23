#include "BoxartScraper.h"
#include "../network/HttpClient.h"
#include "../network/JsonHelper.h"
#include "../filesystem/FileSystemManager.h"
#include "../logging/Logger.h"
#include "../config/AppConfig.h"
#include "../utils/HashHelper.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

namespace RomCloud {

static const char* DEFAULT_DEV_ID = "bun2it";
static const char* DEFAULT_DEV_PASS = "RomCloudTrimUI2026";

// Fast Arcade / NeoGeo filename mapping dictionary
static const std::unordered_map<std::string, std::string>& getArcadeMap() {
    static const std::unordered_map<std::string, std::string> arcadeMap = {
        {"mslug", "Metal Slug - Super Vehicle-001"},
        {"mslug2", "Metal Slug 2 - Super Vehicle-001_II"},
        {"mslugx", "Metal Slug X - Super Vehicle-001"},
        {"mslug3", "Metal Slug 3"},
        {"mslug4", "Metal Slug 4"},
        {"mslug5", "Metal Slug 5"},
        {"kof94", "The King of Fighters '94"},
        {"kof95", "The King of Fighters '95"},
        {"kof96", "The King of Fighters '96"},
        {"kof97", "The King of Fighters '97"},
        {"kof98", "The King of Fighters '98 - The Slugfest"},
        {"kof99", "The King of Fighters '99 - Millennium Battle"},
        {"kof2000", "The King of Fighters 2000"},
        {"kof2001", "The King of Fighters 2001"},
        {"kof2002", "The King of Fighters 2002 - Challenge to Ultimate Battle"},
        {"kof2003", "The King of Fighters 2003"},
        {"sf2", "Street Fighter II - The World Warrior"},
        {"sf2ce", "Street Fighter II' - Champion Edition"},
        {"sf2hf", "Street Fighter II' Turbo - Hyper Fighting"},
        {"ssf2", "Super Street Fighter II - The New Challengers"},
        {"ssf2t", "Super Street Fighter II Turbo"},
        {"sfa", "Street Fighter Alpha - Warriors' Dreams"},
        {"sfa2", "Street Fighter Alpha 2"},
        {"sfa3", "Street Fighter Alpha 3"},
        {"sfiii", "Street Fighter III - New Generation"},
        {"sfiii2", "Street Fighter III 2nd Impact - Giant Attack"},
        {"sfiii3", "Street Fighter III 3rd Strike - Fight for the Future"},
        {"dino", "Cadillacs and Dinosaurs"},
        {"captcomm", "Captain Commando"},
        {"punisher", "The Punisher"},
        {"wof", "Warriors of Fate"},
        {"ffight", "Final Fight"},
        {"garou", "Garou - Mark of the Wolves"},
        {"neobomb", "Neo Bomberman"},
        {"neobomber", "Neo Bomberman"},
        {"shocktro", "Shock Troopers"},
        {"shocktr2", "Shock Troopers - 2nd Squad"},
        {"samsho", "Samurai Shodown"},
        {"samsho2", "Samurai Shodown II"},
        {"samsho3", "Samurai Shodown III"},
        {"samsho4", "Samurai Shodown IV - Amakusa's Revenge"},
        {"samsho5", "Samurai Shodown V"},
        {"samsh5sp", "Samurai Shodown V Special"},
        {"pulstar", "Pulstar"},
        {"blazing", "Blazing Star"},
        {"nam1975", "NAM-1975"},
        {"spinmast", "Spin Master"},
        {"magdrop2", "Magical Drop II"},
        {"magdrop3", "Magical Drop III"},
        {"windj", "Windjammers"},
        {"sonicwi2", "Aero Fighters 2"},
        {"sonicwi3", "Aero Fighters 3"},
        {"pacman", "Pac-Man"},
        {"mspacman", "Ms. Pac-Man"},
        {"galaga", "Galaga"},
        {"dkong", "Donkey Kong"},
        {"mwalk", "Michael Jackson's Moonwalker"},
        {"mk", "Mortal Kombat"},
        {"mk2", "Mortal Kombat II"},
        {"mk3", "Mortal Kombat 3"},
        {"umk3", "Ultimate Mortal Kombat 3"},
        {"1941", "1941 - Counter Attack"},
        {"1942", "1942"},
        {"1943", "1943 - The Battle of Midway"},
        {"1944", "1944 - The Loop Master"},
        {"19xx", "19XX - The War Against Destiny"},
        {"avsp", "Alien vs. Predator"},
        {"armwar", "Armored Warriors"},
        {"ddragon", "Double Dragon"},
        {"ddragon2", "Double Dragon II - The Revenge"},
        {"fatfury1", "Fatal Fury - King of Fighters"},
        {"fatfury2", "Fatal Fury 2"},
        {"fatfury3", "Fatal Fury 3 - Road to the Final Victory"},
        {"fatfursp", "Fatal Fury Special"},
        {"rbff1", "Real Bout Fatal Fury"},
        {"rbff2", "Real Bout Fatal Fury 2 - The Newcomers"},
        {"rbffspec", "Real Bout Fatal Fury Special"},
        {"kov", "Knights of Valour"},
        {"kovplus", "Knights of Valour Plus"},
        {"olds", "Oriental Legend"},
        {"strider", "Strider"},
        {"ghouls", "Ghouls'n Ghosts"},
        {"gnw", "Ghosts'n Goblins"},
        {"sunsetbl", "Sunset Riders"},
        {"ssriders", "Sunset Riders"},
        {"tmnt", "Teenage Mutant Ninja Turtles"},
        {"tmnt2", "Teenage Mutant Ninja Turtles - Turtles in Time"},
        {"xmen", "X-Men"},
        {"simpsons", "The Simpsons"},
        {"bucky", "Bucky O'Hare"},
        {"contra", "Contra"},
        {"scontra", "Super Contra"},
        {"viewpoin", "Viewpoint"},
        {"turfmast", "Neo Turf Masters"},
        {"wjammers", "Windjammers"},
        {"lastblad", "The Last Blade"},
        {"lastbld2", "The Last Blade 2"},
        {"aof", "Art of Fighting"},
        {"aof2", "Art of Fighting 2"},
        {"aof3", "Art of Fighting 3 - Path of the Warrior"},
        {"wakuwak7", "Waku Waku 7"},
        {"breakers", "Breakers"},
        {"breakrev", "Breakers Revenge"},
        {"matrim", "Matrimelee"},
        {"kabukikl", "Kabuki Klash - Far East of Eden"},
        {"rotd", "Rage of the Dragons"},
        {"sengoku", "Sengoku"},
        {"sengoku2", "Sengoku 2"},
        {"sengoku3", "Sengoku 3"},
        {"mutnat", "Mutation Nation"},
        {"cyberlip", "Cyber-Lip"},
        {"tophunt", "Top Hunter - Roddy & Cathy"},
        {"crsword", "Crossed Swords"},
        {"superspy", "The Super Spy"},
        {"roboarmy", "Robo Army"},
        {"eightman", "Eight Man"},
        {"sonicwi", "Aero Fighters"},
        {"gunbird", "Gunbird"},
        {"gunbird2", "Gunbird 2"},
        {"s1945", "Strikers 1945"},
        {"s1945ii", "Strikers 1945 II"},
        {"s1945iii", "Strikers 1945 III"},
        {"batrider", "Armed Police Batrider"},
        {"bgaregga", "Battle Garegga"},
        {"ddonpach", "DoDonPachi"},
        {"donpachi", "DonPachi"},
        {"esprade", "ESP Ra.De."},
        {"guwange", "Guwange"},
        {"progear", "Progear"},
        {"daraku", "The Fallen Angels"}
    };
    return arcadeMap;
}

BoxartScraper& BoxartScraper::instance() {
    static BoxartScraper instance;
    return instance;
}

BoxartScraper::BoxartScraper() {
    m_status.isScraping = false;
    m_status.totalGames = 0;
    m_status.scrapedCount = 0;
    m_status.successCount = 0;
    m_status.progressPct = 0;
}

BoxartScraper::~BoxartScraper() {
    cancelAutoScrape();
    if (m_autoScrapeThread.joinable()) {
        m_autoScrapeThread.join();
    }
}

int BoxartScraper::getScreenScraperSystemId(const std::string& systemCode) {
    std::string code = systemCode;
    std::transform(code.begin(), code.end(), code.begin(), ::toupper);

    if (code == "FC" || code == "NES") return 3;
    if (code == "SFC" || code == "SNES") return 4;
    if (code == "GB") return 9;
    if (code == "GBC") return 10;
    if (code == "GBA") return 12;
    if (code == "MD" || code == "GENESIS") return 1;
    if (code == "PS" || code == "PSX") return 57;
    if (code == "PSP") return 61;
    if (code == "N64") return 14;
    if (code == "NDS") return 15;
    if (code == "ARCADE" || code == "MAME") return 75;
    if (code == "NEOGEO") return 142;
    if (code == "PCE") return 31;
    if (code == "DC") return 23;
    if (code == "SS") return 22;
    if (code == "WS") return 45;
    if (code == "WSC") return 46;
    if (code == "ATARI2600" || code == "A2600") return 40;
    if (code == "ATARI7800" || code == "A7800") return 42;
    if (code == "LYNX") return 28;
    if (code == "MS" || code == "SMS") return 2;
    if (code == "GG") return 21;
    if (code == "SEGACD") return 20;
    if (code == "CPS1") return 6;
    if (code == "CPS2") return 7;
    if (code == "CPS3") return 8;

    return 0;
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
    if (code == "PSP") return "Sony - PlayStation Portable";
    if (code == "N64") return "Nintendo - Nintendo 64";
    if (code == "NDS") return "Nintendo - Nintendo DS";
    if (code == "NEOGEO") return "SNK - Neo Geo";
    if (code == "MAME" || code == "ARCADE") return "FBNeo - Arcade Games";
    if (code == "CPS1") return "Capcom - CP System I";
    if (code == "CPS2") return "Capcom - CP System II";
    if (code == "CPS3") return "Capcom - CP System III";
    if (code == "PCE") return "NEC - PC Engine - TurboGrafx 16";
    if (code == "SMS" || code == "MS") return "Sega - Master System - Mark III";
    if (code == "GG") return "Sega - Game Gear";
    if (code == "WS") return "Bandai - WonderSwan";
    if (code == "WSC") return "Bandai - WonderSwan Color";
    if (code == "ATARI" || code == "A2600" || code == "ATARI2600") return "Atari - 2600";
    if (code == "ATARI7800" || code == "A7800") return "Atari - 7800";
    if (code == "LYNX") return "Atari - Lynx";
    if (code == "DC") return "Sega - Dreamcast";
    if (code == "SS") return "Sega - Saturn";
    if (code == "SEGACD") return "Sega - Mega-CD - Sega CD";
    return "";
}

std::string BoxartScraper::resolveArcadeTitle(const std::string& romName) {
    std::string base = romName;
    size_t lastDot = base.find_last_of('.');
    if (lastDot != std::string::npos && lastDot > 0) {
        base = base.substr(0, lastDot);
    }
    std::transform(base.begin(), base.end(), base.begin(), ::tolower);

    const auto& map = getArcadeMap();
    auto it = map.find(base);
    if (it != map.end()) {
        return it->second;
    }
    return "";
}

std::string BoxartScraper::cleanNameForLibretro(const std::string& filenameOrTitle) {
    std::string name = filenameOrTitle;
    size_t lastDot = name.find_last_of('.');
    if (lastDot != std::string::npos && lastDot > 0) {
        name = name.substr(0, lastDot);
    }

    for (char& c : name) {
        if (c == '&') c = '_';
        else if (c == '/' || c == '\\' || c == '*' || c == '?' || c == '\"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    return name;
}

std::string BoxartScraper::cleanRomTitle(const std::string& filenameOrTitle) {
    std::string s = filenameOrTitle;
    size_t lastDot = s.find_last_of('.');
    if (lastDot != std::string::npos && lastDot > 0) {
        s = s.substr(0, lastDot);
    }

    std::string clean;
    clean.reserve(s.size());
    int parenDepth = 0;
    int bracketDepth = 0;
    for (char c : s) {
        if (c == '(') { parenDepth++; continue; }
        if (c == ')') { if (parenDepth > 0) parenDepth--; continue; }
        if (c == '[') { bracketDepth++; continue; }
        if (c == ']') { if (bracketDepth > 0) bracketDepth--; continue; }
        if (parenDepth == 0 && bracketDepth == 0) {
            clean += c;
        }
    }

    size_t thePos = clean.find(", The");
    if (thePos != std::string::npos) {
        std::string before = clean.substr(0, thePos);
        std::string after = clean.substr(thePos + 5);
        clean = "The " + before + after;
    }

    for (char& c : clean) {
        if (c == '_' || c == '-') c = ' ';
    }

    std::string res;
    bool prevSpace = false;
    for (char c : clean) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!prevSpace && !res.empty()) {
                res += ' ';
                prevSpace = true;
            }
        } else {
            res += c;
            prevSpace = false;
        }
    }
    while (!res.empty() && res.back() == ' ') res.pop_back();
    return res.empty() ? filenameOrTitle : res;
}

std::vector<std::string> BoxartScraper::generateLibretroCandidates(const std::string& filenameOrTitle, const std::string& systemCode) {
    std::vector<std::string> candidates;
    std::unordered_set<std::string> seen;

    auto addCandidate = [&](const std::string& cand) {
        std::string clean = cleanNameForLibretro(cand);
        while (!clean.empty() && clean.back() == ' ') clean.pop_back();
        if (!clean.empty() && seen.find(clean) == seen.end()) {
            seen.insert(clean);
            candidates.push_back(clean);
        }
    };

    // 1. If Arcade or NeoGeo, check Arcade dictionary first!
    std::string upSys = systemCode;
    std::transform(upSys.begin(), upSys.end(), upSys.begin(), ::toupper);
    if (upSys == "ARCADE" || upSys == "MAME" || upSys == "NEOGEO" || upSys == "CPS1" || upSys == "CPS2" || upSys == "CPS3") {
        std::string arcadeTitle = resolveArcadeTitle(filenameOrTitle);
        if (!arcadeTitle.empty()) {
            addCandidate(arcadeTitle);
            addCandidate(arcadeTitle + " (World)");
            addCandidate(arcadeTitle + " (USA)");
            addCandidate(arcadeTitle + " (Japan)");
        }
    }

    // 2. Exact filename without extension
    std::string noExt = filenameOrTitle;
    size_t lastDot = noExt.find_last_of('.');
    if (lastDot != std::string::npos && lastDot > 0) {
        noExt = noExt.substr(0, lastDot);
    }
    addCandidate(noExt);

    // 3. Clean base title without tags (e.g. "Super Mario World")
    std::string base = cleanRomTitle(filenameOrTitle);
    addCandidate(base);

    // 4. Generate common region variations (crucial for Libretro Thumbnails)
    addCandidate(base + " (USA)");
    addCandidate(base + " (USA, Europe)");
    addCandidate(base + " (World)");
    addCandidate(base + " (Europe)");
    addCandidate(base + " (Japan)");
    addCandidate(base + " (En,Ja)");
    addCandidate(base + " (En,Fr,De)");
    addCandidate(base + " (Rev 1)");
    addCandidate(base + " (USA) (Rev 1)");

    // 5. Handle "The " inversions: "The Legend of Zelda" <-> "Legend of Zelda, The"
    if (base.rfind("The ", 0) == 0) {
        std::string inverted = base.substr(4) + ", The";
        addCandidate(inverted);
        addCandidate(inverted + " (USA)");
        addCandidate(inverted + " (World)");
        addCandidate(inverted + " (Europe)");
    } else {
        size_t commaThe = base.find(", The");
        if (commaThe != std::string::npos) {
            std::string inverted = "The " + base.substr(0, commaThe) + base.substr(commaThe + 5);
            addCandidate(inverted);
            addCandidate(inverted + " (USA)");
            addCandidate(inverted + " (World)");
            addCandidate(inverted + " (Europe)");
        }
    }

    // 6. If title has a subtitle after "-", try base part before "-"
    size_t dash = base.find(" - ");
    if (dash != std::string::npos && dash > 2) {
        std::string subBase = base.substr(0, dash);
        addCandidate(subBase);
        addCandidate(subBase + " (USA)");
        addCandidate(subBase + " (World)");
    }

    return candidates;
}

std::string BoxartScraper::extractYearFromFilename(const std::string& filename) {
    for (size_t i = 0; i + 5 < filename.size(); ++i) {
        if ((filename[i] == '(' || filename[i] == '[') &&
            (filename[i + 5] == ')' || filename[i + 5] == ']')) {
            std::string sub = filename.substr(i + 1, 4);
            if ((sub[0] == '1' && sub[1] == '9' && std::isdigit(sub[2]) && std::isdigit(sub[3])) ||
                (sub[0] == '2' && sub[1] == '0' && std::isdigit(sub[2]) && std::isdigit(sub[3]))) {
                return sub;
            }
        }
    }
    return "";
}

bool BoxartScraper::downloadCoverFromUrl(const std::string& url, const std::string& targetPath) {
    if (url.empty() || targetPath.empty()) return false;
    std::vector<std::string> headers = {
        "User-Agent: RomCloud-TrimUI-Scraper/1.0"
    };
    HttpResponse resp = HttpClient::instance().get(url, headers, 10000);

    if (resp.statusCode == 200 && !resp.body.empty() && resp.body.size() > 512) {
        FILE* fp = fopen(targetPath.c_str(), "wb");
        if (fp) {
            fwrite(resp.body.data(), 1, resp.body.size(), fp);
            fclose(fp);
            Logger::info("BoxartScraper: Saved artwork to " + targetPath);
            return true;
        }
    }
    return false;
}

bool BoxartScraper::scrapeCover(const GameRecord& game, const SystemRecord& sys, std::string& outCoverPath) {
    std::string cleanName = cleanNameForLibretro(game.filename.empty() ? game.title : game.filename);
    std::string targetDir = AppConfig::instance().getImgsDir() + "/" + sys.code;
    FileSystemManager::instance().createDirectoryRecursive(targetDir);
    std::string targetPath = targetDir + "/" + cleanName + ".png";

    // 1. Reuse existing local cover
    if (FileSystemManager::instance().fileExists(targetPath)) {
        outCoverPath = targetPath;
        return true;
    }

    // 2. Try ScreenScraper if credentials configured
    GameScrapeResult ssResult;
    if (scrapeFromScreenScraper(game, sys, ssResult) && ssResult.coverFound) {
        outCoverPath = ssResult.coverPath;
        return true;
    }

    // 3. Fallback to Libretro Thumbnails GitHub with Smart Multi-Candidate Matching
    std::string libretroSys = getLibretroSystemName(sys.code);
    if (!libretroSys.empty()) {
        std::string rawName = game.filename.empty() ? game.title : game.filename;
        std::vector<std::string> candidates = generateLibretroCandidates(rawName, sys.code);

        const char* subdirs[] = { "Named_Boxarts", "Named_Titles", "Named_Snaps" };
        std::string encodedSys = HttpClient::instance().urlEncode(libretroSys);

        for (const auto& cand : candidates) {
            std::string encodedCand = HttpClient::instance().urlEncode(cand);
            for (const char* subdir : subdirs) {
                // Use GitHub raw URL instead of CDN (CDN may have issues)
                std::string url = "https://raw.githubusercontent.com/libretro/libretro-thumbnails/master/" + encodedSys + "/" + subdir + "/" + encodedCand + ".png";
                if (downloadCoverFromUrl(url, targetPath)) {
                    outCoverPath = targetPath;
                    Logger::info("BoxartScraper: Match found on Libretro GitHub (" + cand + ") -> " + targetPath);
                    return true;
                }
            }
        }
    }

    return false;
}

bool BoxartScraper::scrapeFromScreenScraper(const GameRecord& game, const SystemRecord& sys, GameScrapeResult& outResult) {
    std::string ssUser = DatabaseManager::instance().getSetting("screenscraper_user", "");
    std::string ssPass = DatabaseManager::instance().getSetting("screenscraper_pass", "");
    std::string ssDevId = DatabaseManager::instance().getSetting("screenscraper_devid", DEFAULT_DEV_ID);
    std::string ssDevPass = DatabaseManager::instance().getSetting("screenscraper_devpass", DEFAULT_DEV_PASS);

    if (ssUser.empty() || ssPass.empty()) {
        return false;
    }

    int sysId = getScreenScraperSystemId(sys.code);
    std::string fn = game.filename.empty() ? game.title : game.filename;

    // Checksum & File Size matching for 100% exact identification
    std::string crc;
    std::string md5;
    std::string fileSizeStr;
    if (!game.localPath.empty() && FileSystemManager::instance().fileExists(game.localPath)) {
        crc = HashHelper::computeFileCrc32(game.localPath);
        md5 = HashHelper::computeFileMd5(game.localPath);
        size_t sz = FileSystemManager::instance().getFileSize(game.localPath);
        if (sz > 0) fileSizeStr = std::to_string(sz);
    }

    // Step A: Query by romnom / crc / md5
    std::string url = "https://api.screenscraper.fr/api2/jeuInfos.php?devid=" + HttpClient::instance().urlEncode(ssDevId) +
                      "&devpassword=" + HttpClient::instance().urlEncode(ssDevPass) +
                      "&softname=RomCloud&output=json" +
                      "&ssid=" + HttpClient::instance().urlEncode(ssUser) +
                      "&sspassword=" + HttpClient::instance().urlEncode(ssPass) +
                      "&romnom=" + HttpClient::instance().urlEncode(fn);

    if (sysId > 0) url += "&systemeid=" + std::to_string(sysId);
    if (!crc.empty()) url += "&crc=" + crc;
    if (!md5.empty()) url += "&md5=" + md5;
    if (!fileSizeStr.empty()) url += "&romtaille=" + fileSizeStr;

    Logger::info("BoxartScraper: Querying ScreenScraper.fr for " + fn);
    std::vector<std::string> headers = { "User-Agent: RomCloud-TrimUI-Scraper/1.0" };
    HttpResponse resp = HttpClient::instance().get(url, headers, 8000);

    // Step B: If exact lookup failed, fallback to search by title (jeuRecherche.php)
    if (resp.statusCode != 200 || resp.body.find("\"jeu\"") == std::string::npos) {
        std::string searchTitle = cleanRomTitle(fn);
        std::string searchUrl = "https://api.screenscraper.fr/api2/jeuRecherche.php?devid=" + HttpClient::instance().urlEncode(ssDevId) +
                                "&devpassword=" + HttpClient::instance().urlEncode(ssDevPass) +
                                "&softname=RomCloud&output=json" +
                                "&ssid=" + HttpClient::instance().urlEncode(ssUser) +
                                "&sspassword=" + HttpClient::instance().urlEncode(ssPass) +
                                "&recherche=" + HttpClient::instance().urlEncode(searchTitle);
        if (sysId > 0) searchUrl += "&systemeid=" + std::to_string(sysId);

        Logger::info("BoxartScraper: Fallback search ScreenScraper.fr for: " + searchTitle);
        HttpResponse sresp = HttpClient::instance().get(searchUrl, headers, 8000);

        if (sresp.statusCode == 200 && sresp.body.find("\"id\"") != std::string::npos) {
            std::string gameIdStr = JsonHelper::extractString(sresp.body, "id");
            if (!gameIdStr.empty()) {
                std::string detailUrl = "https://api.screenscraper.fr/api2/jeuInfos.php?devid=" + HttpClient::instance().urlEncode(ssDevId) +
                                        "&devpassword=" + HttpClient::instance().urlEncode(ssDevPass) +
                                        "&softname=RomCloud&output=json" +
                                        "&ssid=" + HttpClient::instance().urlEncode(ssUser) +
                                        "&sspassword=" + HttpClient::instance().urlEncode(ssPass) +
                                        "&gameid=" + gameIdStr;
                resp = HttpClient::instance().get(detailUrl, headers, 8000);
            }
        }
    }

    if (resp.statusCode != 200 || resp.body.empty() || resp.body.find("\"jeu\"") == std::string::npos) {
        return false;
    }

    std::string body = resp.body;
    outResult.source = "screenscraper";

    // 1. Title
    std::string ssNom = JsonHelper::extractString(body, "nom_us");
    if (ssNom.empty()) ssNom = JsonHelper::extractString(body, "nom_eu");
    if (ssNom.empty()) ssNom = JsonHelper::extractString(body, "nom");
    outResult.title = ssNom.empty() ? cleanRomTitle(fn) : ssNom;

    // 2. Release Year
    size_t datesPos = body.find("\"dates\"");
    if (datesPos != std::string::npos) {
        size_t textPos = body.find("\"text\":", datesPos);
        if (textPos != std::string::npos && textPos - datesPos < 300) {
            size_t q1 = body.find('\"', textPos + 7);
            if (q1 != std::string::npos && q1 + 5 < body.size()) {
                std::string dateStr = body.substr(q1 + 1, 4);
                if (std::isdigit(dateStr[0]) && std::isdigit(dateStr[1])) {
                    outResult.releaseYear = dateStr;
                }
            }
        }
    }
    if (outResult.releaseYear.empty()) {
        outResult.releaseYear = extractYearFromFilename(fn);
    }

    // 3. Developer & Publisher
    size_t devPos = body.find("\"developpeur\"");
    if (devPos != std::string::npos) {
        outResult.developer = JsonHelper::extractString(body.substr(devPos, 200), "nom");
    }
    size_t pubPos = body.find("\"editeur\"");
    if (pubPos != std::string::npos) {
        std::string pub = JsonHelper::extractString(body.substr(pubPos, 200), "nom");
        if (!pub.empty() && !outResult.developer.empty() && pub != outResult.developer) {
            outResult.developer += " / " + pub;
        } else if (outResult.developer.empty()) {
            outResult.developer = pub;
        }
    }

    // 4. Genre
    size_t genrePos = body.find("\"genres\"");
    if (genrePos != std::string::npos) {
        outResult.genre = JsonHelper::extractString(body.substr(genrePos, 250), "nom");
    }

    // 5. Synopsis / Story (Vietnamese > English > French)
    size_t synPos = body.find("\"synopsis\"");
    if (synPos != std::string::npos) {
        std::string synBlock = body.substr(synPos, 2500);
        size_t viPos = synBlock.find("\"langue\":\"vi\"");
        if (viPos != std::string::npos) {
            outResult.description = JsonHelper::extractString(synBlock.substr(viPos, 800), "texte");
        }
        if (outResult.description.empty()) {
            size_t enPos = synBlock.find("\"langue\":\"en\"");
            if (enPos != std::string::npos) {
                outResult.description = JsonHelper::extractString(synBlock.substr(enPos, 800), "texte");
            }
        }
        if (outResult.description.empty()) {
            outResult.description = JsonHelper::extractString(synBlock, "texte");
        }
    }

    // 6. Media Boxart (2D, 3D, Wheel, or Screenshot)
    std::string mediaUrl;
    size_t mediaPos = body.find("\"media_box2d\"");
    if (mediaPos != std::string::npos) mediaUrl = JsonHelper::extractString(body.substr(mediaPos, 400), "url");
    if (mediaUrl.empty()) {
        mediaPos = body.find("\"media_box3d\"");
        if (mediaPos != std::string::npos) mediaUrl = JsonHelper::extractString(body.substr(mediaPos, 400), "url");
    }
    if (mediaUrl.empty()) {
        mediaPos = body.find("\"media_wheel\"");
        if (mediaPos != std::string::npos) mediaUrl = JsonHelper::extractString(body.substr(mediaPos, 400), "url");
    }
    if (mediaUrl.empty()) {
        mediaPos = body.find("\"media_screenshoot\"");
        if (mediaPos != std::string::npos) mediaUrl = JsonHelper::extractString(body.substr(mediaPos, 400), "url");
    }

    if (!mediaUrl.empty()) {
        std::string cleanName = cleanNameForLibretro(fn);
        std::string targetDir = AppConfig::instance().getImgsDir() + "/" + sys.code;
        FileSystemManager::instance().createDirectoryRecursive(targetDir);
        std::string targetPath = targetDir + "/" + cleanName + ".png";

        if (downloadCoverFromUrl(mediaUrl, targetPath)) {
            outResult.coverPath = targetPath;
            outResult.coverFound = true;
        }
    }

    outResult.success = true;
    return true;
}

bool BoxartScraper::scrapeFromLibretro(const GameRecord& game, const SystemRecord& sys, GameScrapeResult& outResult) {
    std::string fn = game.filename.empty() ? game.title : game.filename;
    outResult.title = cleanRomTitle(fn);
    outResult.releaseYear = extractYearFromFilename(fn);
    outResult.source = "libretro";
    outResult.description = "Trò chơi kinh điển trên hệ máy " + sys.name + " (" + sys.code + ").";

    std::string coverPath;
    if (scrapeCover(game, sys, coverPath)) {
        outResult.coverPath = coverPath;
        outResult.coverFound = true;
    }

    outResult.success = true;
    return true;
}

GameScrapeResult BoxartScraper::scrapeGameInfo(const GameRecord& game, const SystemRecord& sys) {
    GameScrapeResult result;
    result.title = game.title.empty() ? cleanRomTitle(game.filename) : game.title;

    // 1. Try ScreenScraper first
    bool ssSuccess = scrapeFromScreenScraper(game, sys, result);

    // 2. If ScreenScraper wasn't configured or failed to find cover, use Libretro candidate pipeline
    if (!ssSuccess || !result.coverFound) {
        GameScrapeResult libResult;
        scrapeFromLibretro(game, sys, libResult);
        if (!result.coverFound && libResult.coverFound) {
            result.coverPath = libResult.coverPath;
            result.coverFound = true;
        }
        if (result.title.empty()) result.title = libResult.title;
        if (result.releaseYear.empty()) result.releaseYear = libResult.releaseYear;
        if (result.description.empty()) result.description = libResult.description;
        result.success = true;
    }

    // 3. Save metadata into SQLite
    if (result.success) {
        DatabaseManager::instance().updateGameMetadata(game.id, result.description, result.releaseYear,
                                                      result.developer, result.genre, result.coverPath);
    }

    return result;
}

std::vector<ScrapeCandidate> BoxartScraper::searchCandidates(const std::string& query, const std::string& systemCode) {
    std::vector<ScrapeCandidate> results;
    if (query.empty()) return results;

    std::string ssUser = DatabaseManager::instance().getSetting("screenscraper_user", "");
    std::string ssPass = DatabaseManager::instance().getSetting("screenscraper_pass", "");
    std::string ssDevId = DatabaseManager::instance().getSetting("screenscraper_devid", DEFAULT_DEV_ID);
    std::string ssDevPass = DatabaseManager::instance().getSetting("screenscraper_devpass", DEFAULT_DEV_PASS);
    int sysId = getScreenScraperSystemId(systemCode);

    // 1. Search on ScreenScraper.fr if user configured
    if (!ssUser.empty() && !ssPass.empty()) {
        std::string searchUrl = "https://api.screenscraper.fr/api2/jeuRecherche.php?devid=" + HttpClient::instance().urlEncode(ssDevId) +
                                "&devpassword=" + HttpClient::instance().urlEncode(ssDevPass) +
                                "&softname=RomCloud&output=json" +
                                "&ssid=" + HttpClient::instance().urlEncode(ssUser) +
                                "&sspassword=" + HttpClient::instance().urlEncode(ssPass) +
                                "&recherche=" + HttpClient::instance().urlEncode(query);
        if (sysId > 0) searchUrl += "&systemeid=" + std::to_string(sysId);

        std::vector<std::string> headers = { "User-Agent: RomCloud-TrimUI-Scraper/1.0" };
        HttpResponse resp = HttpClient::instance().get(searchUrl, headers, 8000);

        if (resp.statusCode == 200 && resp.body.find("\"jeux\"") != std::string::npos) {
            std::string body = resp.body;
            size_t pos = 0;
            while ((pos = body.find("\"id\":", pos)) != std::string::npos && results.size() < 6) {
                ScrapeCandidate cand;
                cand.source = "ScreenScraper.fr";
                size_t idEnd = body.find_first_of(",}", pos);
                std::string gId = body.substr(pos + 5, idEnd - (pos + 5));

                size_t blockEnd = body.find("},", pos);
                if (blockEnd == std::string::npos) blockEnd = body.find("}]", pos);
                std::string block = (blockEnd != std::string::npos) ? body.substr(pos, blockEnd - pos) : "";

                cand.title = JsonHelper::extractString(block, "nom_us");
                if (cand.title.empty()) cand.title = JsonHelper::extractString(block, "nom");
                if (cand.title.empty()) cand.title = query;

                // Media URL
                size_t mPos = block.find("\"media_box2d\"");
                if (mPos != std::string::npos) cand.coverUrl = JsonHelper::extractString(block.substr(mPos, 300), "url");
                if (cand.coverUrl.empty()) {
                    mPos = block.find("\"media_box3d\"");
                    if (mPos != std::string::npos) cand.coverUrl = JsonHelper::extractString(block.substr(mPos, 300), "url");
                }

                results.push_back(cand);
                pos = (blockEnd != std::string::npos) ? blockEnd + 2 : pos + 10;
            }
        }
    }

    // 2. Generate Libretro GitHub Candidates
    std::string libretroSys = getLibretroSystemName(systemCode);
    if (!libretroSys.empty()) {
        std::vector<std::string> cands = generateLibretroCandidates(query, systemCode);
        std::string encodedSys = HttpClient::instance().urlEncode(libretroSys);

        for (const auto& candName : cands) {
            if (results.size() >= 8) break;
            std::string encodedCand = HttpClient::instance().urlEncode(candName);
            // Use GitHub raw URL instead of CDN
            std::string url = "https://raw.githubusercontent.com/libretro/libretro-thumbnails/master/" + encodedSys + "/Named_Boxarts/" + encodedCand + ".png";

            ScrapeCandidate cand;
            cand.title = candName;
            cand.source = "Libretro Official";
            cand.coverUrl = url;
            cand.description = "Trò chơi thuộc hệ máy " + systemCode;
            results.push_back(cand);
        }
    }

    return results;
}

bool BoxartScraper::applyCandidate(int64_t gameId, const ScrapeCandidate& candidate) {
    GameRecord g;
    if (!DatabaseManager::instance().getGameById(gameId, g)) return false;

    SystemRecord sys;
    DatabaseManager::instance().getSystemById(g.systemId, sys);

    std::string cleanName = cleanNameForLibretro(g.filename.empty() ? g.title : g.filename);
    std::string targetDir = AppConfig::instance().getImgsDir() + "/" + sys.code;
    FileSystemManager::instance().createDirectoryRecursive(targetDir);
    std::string targetPath = targetDir + "/" + cleanName + ".png";

    bool coverSaved = false;
    if (!candidate.coverUrl.empty()) {
        coverSaved = downloadCoverFromUrl(candidate.coverUrl, targetPath);
    }

    std::string covPath = coverSaved ? targetPath : g.coverPath;
    std::string yr = candidate.releaseYear.empty() ? g.releaseYear : candidate.releaseYear;
    std::string dev = candidate.developer.empty() ? g.developer : candidate.developer;
    std::string gen = candidate.genre.empty() ? g.genre : candidate.genre;
    std::string desc = candidate.description.empty() ? g.description : candidate.description;

    return DatabaseManager::instance().updateGameMetadata(gameId, desc, yr, dev, gen, covPath);
}

bool BoxartScraper::startAutoScrapeSdCard(bool forceAll) {
    if (m_isScraping.load()) {
        Logger::warn("BoxartScraper: Auto-scrape already running in background.");
        return false;
    }

    std::vector<GameRecord> candidates;
    if (forceAll) {
        candidates = DatabaseManager::instance().getGamesBySystem(0, 1);
    } else {
        candidates = DatabaseManager::instance().getUnscrapedLocalGames(0);
    }

    if (candidates.empty()) {
        Logger::info("BoxartScraper: No unscraped local ROMs found on SD card.");
        return false;
    }

    if (m_autoScrapeThread.joinable()) {
        m_autoScrapeThread.join();
    }

    m_isScraping = true;
    m_cancelRequested = false;

    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_status.isScraping = true;
        m_status.totalGames = static_cast<int>(candidates.size());
        m_status.scrapedCount = 0;
        m_status.successCount = 0;
        m_status.progressPct = 0;
        m_status.lastMessage = "Bắt đầu tự động cào thông tin cho " + std::to_string(candidates.size()) + " game...";
    }

    Logger::info("BoxartScraper: Starting auto-scrape background worker for " +
                 std::to_string(candidates.size()) + " ROMs on SD card.");

    m_autoScrapeThread = std::thread([this, candidates]() {
        int count = 0;
        int success = 0;

        for (const auto& g : candidates) {
            if (m_cancelRequested.load()) {
                Logger::info("BoxartScraper: Auto-scrape cancelled by user.");
                break;
            }

            SystemRecord sys;
            if (!DatabaseManager::instance().getSystemById(g.systemId, sys)) {
                sys.code = g.systemCode;
                sys.name = g.systemCode;
            }

            {
                std::lock_guard<std::mutex> lock(m_statusMutex);
                m_status.currentGame = g.title.empty() ? g.filename : g.title;
                m_status.currentSystem = sys.code;
                m_status.scrapedCount = count;
                m_status.successCount = success;
                m_status.progressPct = (m_status.totalGames > 0) ? (count * 100 / m_status.totalGames) : 0;
                m_status.lastMessage = "Đang cào: " + m_status.currentGame + " (" + sys.code + ")";
            }

            auto res = scrapeGameInfo(g, sys);
            if (res.success && res.coverFound) {
                success++;
            }
            count++;

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_status.isScraping = false;
            m_status.scrapedCount = count;
            m_status.successCount = success;
            m_status.progressPct = 100;
            m_status.lastMessage = "Hoàn tất cào " + std::to_string(success) + "/" + std::to_string(count) + " game trên thẻ SD!";
        }

        m_isScraping = false;
        Logger::info("BoxartScraper: Auto-scrape finished (" + std::to_string(success) +
                     "/" + std::to_string(count) + " successful).");
    });

    return true;
}

void BoxartScraper::cancelAutoScrape() {
    if (m_isScraping.load()) {
        m_cancelRequested = true;
    }
}

AutoScrapeStatus BoxartScraper::getAutoScrapeStatus() {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    return m_status;
}

bool BoxartScraper::testScreenScraperAuth(const std::string& user, const std::string& pass,
                                         const std::string& devId, const std::string& devPass,
                                         std::string& outError) {
    if (user.empty() || pass.empty()) {
        outError = "Tên đăng nhập và mật khẩu ScreenScraper không được để trống.";
        return false;
    }

    std::string dId = devId.empty() ? DEFAULT_DEV_ID : devId;
    std::string dPass = devPass.empty() ? DEFAULT_DEV_PASS : devPass;

    std::string testUrl = "https://api.screenscraper.fr/api2/jeuInfos.php?devid=" + HttpClient::instance().urlEncode(dId) +
                          "&devpassword=" + HttpClient::instance().urlEncode(dPass) +
                          "&softname=RomCloud&output=json" +
                          "&ssid=" + HttpClient::instance().urlEncode(user) +
                          "&sspassword=" + HttpClient::instance().urlEncode(pass) +
                          "&systemeid=4&romnom=Super%20Mario%20World%20(USA).sfc";

    std::vector<std::string> headers = { "User-Agent: RomCloud-TrimUI-Scraper/1.0" };
    HttpResponse resp = HttpClient::instance().get(testUrl, headers, 8000);

    if (resp.statusCode == 200 && resp.body.find("\"response\"") != std::string::npos) {
        if (resp.body.find("\"Erreur\"") != std::string::npos || resp.body.find("\"error\"") != std::string::npos) {
            outError = "Thông tin đăng nhập ScreenScraper không hợp lệ hoặc tài khoản bị giới hạn.";
            return false;
        }
        return true;
    } else {
        outError = "Không thể kết nối đến máy chủ ScreenScraper.fr (Mã lỗi: " + std::to_string(resp.statusCode) + ")";
        return false;
    }
}

} // namespace RomCloud
