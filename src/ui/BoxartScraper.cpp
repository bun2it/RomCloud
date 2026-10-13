#include "BoxartScraper.h"
#include "../network/HttpClient.h"
#include "../network/JsonHelper.h"
#include "../filesystem/FileSystemManager.h"
#include "../logging/Logger.h"
#include "../config/AppConfig.h"
#include "../utils/HashHelper.h"
#include "../rom/Md5SystemDb.h"
#include "../rom/RomDetector.h"
#include "../rom/RomTitle.h"
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

namespace RomCloud {

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
        {"dino", "Cadillacs _ Dinosaurs (930201 USA)"},
        {"captcomm", "Captain Commando"},
        {"punisher", "The Punisher (930422 USA)"},
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
        {"1942", "1942 (Revision B)"},
        {"1943", "1943_ The Battle of Midway (US)"},
        {"1944", "1944 - the loop master (000620 USA)"},
        {"19xx", "19XX - the war against destiny (951207 USA)"},
        {"avsp", "Alien vs Predator (940520 Euro)"},
        {"armwar", "Armored Warriors (941024 USA)"},
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
        {"aof3", "Art of Fighting 3 - The Path of the Warrior _ Art of Fighting - Ryuuko no Ken Gaiden"},
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
        {"progear", "Progear (010117 USA)"},
        {"daraku", "The Fallen Angels"},
        {"raiden2", "Raiden II (US, set 1)"},
        {"captcommj", "Captain Commando (911202 Japan)"},
        {"blktiger", "Black Tiger"},
        {"1943kai", "1943 Kai"},
        {"ataxx", "Ataxx (set 1)"},
        {"bowlrama", "Bowl-O-Rama"},
        {"cbasebal", "Capcom Baseball (Japan)"},
        {"dimahoo", "Dimahoo (US 000121)"},
        {"mbombrd", "Bomber Man World (World)"},
        {"pbobble3", "Puzzle Bobble 3 (World)"},
        {"pbobble4", "Puzzle Bobble 4 (World)"},
        {"avengers", "Avengers (US set 1)"},
        {"batcir", "Battle Circuit (970319 Euro)"},
        {"bionicc", "Bionic Commando (Euro)"},
        {"cawing", "Carrier Air Wing (U.S. navy 901012 USA)"},
        {"commando", "Commando (World)"},
        {"cybots", "Cyberbots - fullmetal madness (950424 USA)"},
        {"ddsom", "Dungeons _ Dragons - shadow over mystara (960209 USA)"},
        {"ddtod", "Dungeons _ Dragons - tower of doom (940113 USA)"},
        {"dstlk", "Darkstalkers - the night warriors (940705 USA)"},
        {"ecofghtr", "Eco Fighters (940215 USA)"},
        {"exedexes", "Exed Exes"},
        {"f1dream", "F-1 Dream [Game is bugged, use the bootleg instead.]"},
        {"gigawing", "Giga Wing (990222 USA)"},
        {"gng", "Ghosts'n Goblins (World_ set 1)"},
        {"gunsmoke", "Gun.Smoke (World, 851115)"},
        {"higemaru", "Pirate Ship Higemaru"},
        {"jojo", "JoJo's Venture _ JoJo no Kimyou na Bouken (USA 981202)"},
        {"jojoba", "JoJo's Bizarre Adventure_ Heritage for the Future _ JoJo no Kimyou na Bouken_ Mirai e no Isan (Japan 990927)"},
        {"knights", "Knights of the Round (911127 etc)"},
        {"kod", "The King of Dragons (910711 etc)"},
        {"lastduel", "Last Duel (US New Ver.)"},
        {"lwings", "Legendary Wings (US set 1)"},
        {"madgear", "Mad Gear (US)"},
        {"megaman", "Mega Man - the power battle (951006 USA)"},
        {"megaman2", "Rockman 2 - the power fighters (960708 Japan)"},
        {"mercs", "Mercs (900302 USA)"},
        {"mpang", "Mighty! Pang (001010 Euro)"},
        {"msh", "Marvel Super Heroes (951024 Euro)"},
        {"mshvsf", "Marvel Super Heroes vs Street Fighter (970620 Asia)"},
        {"msword", "Magic Sword - heroic fantasy (25.07.1990 USA)"},
        {"mvsc", "Marvel vs Capcom - clash of super heroes (980123 Euro)"},
        {"nemo", "Nemo (90 11 20 Japan)"},
        {"nwarr", "Night Warriors - darkstalkers' revenge (950316 Euro)"},
        {"pang3", "Pang! 3 (950601 Euro)"},
        {"pbobble2", "Puzzle Bobble 2 _ Bust-A-Move Again (Neo-Geo)"},
        {"pnickj", "Pnickies (940608 Japan)"},
        {"redearth", "Red Earth _ War-Zard (Euro 961121)"},
        {"ringdest", "Ring of Destruction - slammasters II (940902 Euro)"},
        {"sectionz", "Section Z (set 1)"},
        {"sidearms", "Side Arms - Hyper Dyne (World, 861129)"},
        {"slammast", "Saturday Night Slam Masters (slam masters 930713 USA)"},
        {"sonson", "Son Son"},
        {"srumbler", "The Speed Rumbler (set 1)"},
        {"trojan", "Trojan (US set 1)"},
        {"unsquad", "U.N. Squadron (US)"},
        {"varth", "Varth - operation thunderstorm (920612 USA)"},
        {"vhunt2", "Vampire Hunter 2 - darkstalkers revenge (970929 Japan)"},
        {"vsav", "Vampire Savior - the lord of vampire (970519 Euro)"},
        {"vsav2", "Vampire Savior 2 - the lord of vampire (970913 Japan)"},
        {"vulgus", "Vulgus (Japan_)"},
        {"willow", "Willow (USA)"},
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
    // Họ arcade dùng chung "FB Alpha - Arcade Games" (tên đầy đủ):
    // "FBNeo - Arcade Games" và "Capcom - CP System I/II/III" không tồn tại
    // trên libretro-thumbnails (404 toàn bộ nếu map sai).
    if (code == "MAME" || code == "ARCADE" || code == "FBNEO" ||
        code == "MAME2003PLUS" || code == "MAME2010" || code == "PGM" ||
        code == "CPS1" || code == "CPS2" || code == "CPS3")
        return "FB Alpha - Arcade Games";
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

namespace {
// Họ arcade dùng chung art (FB Alpha + MAME): nhận diện + fallback thư mục.
bool isArcadeFamilySys(const std::string& systemCode) {
    std::string up = systemCode;
    std::transform(up.begin(), up.end(), up.begin(), ::toupper);
    return up == "ARCADE" || up == "MAME" || up == "FBNEO" ||
           up == "MAME2003PLUS" || up == "MAME2010" || up == "PGM" ||
           up == "NEOGEO" || up == "CPS1" || up == "CPS2" || up == "CPS3";
}
} // namespace

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
    // Chỉ cắt đuôi ext thật (ngắn, alnum, ở cuối): tên game có dấu chấm
    // ("U.S. navy", "Gun.Smoke", "25.07.1990") phải giữ nguyên.
    size_t lastDot = name.find_last_of('.');
    if (lastDot != std::string::npos && lastDot > 0 &&
        name.size() - lastDot - 1 <= 4) {
        bool extLike = true;
        for (size_t i = lastDot + 1; i < name.size(); ++i) {
            if (!isalnum((unsigned char)name[i])) {
                extLike = false;
                break;
            }
        }
        if (extLike) name = name.substr(0, lastDot);
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
    // Chỉ cắt đuôi ext thật (ngắn, alnum, ở cuối) — giữ dấu chấm trong tên.
    size_t lastDot = s.find_last_of('.');
    if (lastDot != std::string::npos && lastDot > 0 &&
        s.size() - lastDot - 1 <= 4) {
        bool extLike = true;
        for (size_t i = lastDot + 1; i < s.size(); ++i) {
            if (!isalnum((unsigned char)s[i])) {
                extLike = false;
                break;
            }
        }
        if (extLike) s = s.substr(0, lastDot);
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

std::vector<std::string> BoxartScraper::generateLibretroCandidates(const std::string& filenameOrTitle, const std::string& systemCode, const std::string& exactTitle) {
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

    // 0. Tên chuẩn No-Intro tra theo MD5/CRC (chính xác tuyệt đối).
    if (!exactTitle.empty()) addCandidate(exactTitle);

    // 1. If Arcade or NeoGeo, check Arcade dictionary first!
    if (isArcadeFamilySys(systemCode)) {
        std::string arcadeTitle = resolveArcadeTitle(filenameOrTitle);
        if (arcadeTitle.empty()) {
            // Clone MAME (captcommj, sfa2u...): bỏ hậu tố vùng 1 chữ cái
            // (j=Japan, u=USA, e=Europe) rồi tra tên set cha.
            std::string stemOnly = filenameOrTitle;
            size_t ld = stemOnly.find_last_of('.');
            if (ld != std::string::npos && ld > 0)
                stemOnly = stemOnly.substr(0, ld);
            if (stemOnly.size() >= 5) {
                char last = (char)tolower((unsigned char)stemOnly.back());
                const char* region = (last == 'j')   ? "Japan"
                                     : (last == 'u') ? "USA"
                                     : (last == 'e') ? "Europe"
                                                     : nullptr;
                if (region) {
                    std::string parentTitle = resolveArcadeTitle(
                        stemOnly.substr(0, stemOnly.size() - 1));
                    if (!parentTitle.empty()) {
                        addCandidate(parentTitle + " (" + region + ")");
                        addCandidate(parentTitle + " (World)");
                        addCandidate(parentTitle + " (USA)");
                        addCandidate(parentTitle + " (Japan)");
                    }
                }
            }
        } else {
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

std::string BoxartScraper::exactTitleByHash(const std::string& localPath) {
    if (localPath.empty() ||
        !FileSystemManager::instance().fileExists(localPath))
        return "";
    std::string ext;
    size_t dot = localPath.rfind('.');
    if (dot != std::string::npos) {
        ext = localPath.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }
    // Zip/7z: tra CRC từng file ruột (central directory, không giải nén).
    if (ext == "zip" || ext == "7z") {
        for (const auto& crc :
             RomDetector::instance().getZipEntryCrc32s(localPath, 10)) {
            std::string code, title;
            if (Md5SystemDb::instance().lookup(crc, code, title) &&
                !title.empty() &&
                RomDetector::instance().isKnownSystemCode(code))
                return title;
        }
        return "";
    }
    // File lẻ: băm MD5 (bỏ file đĩa >256MB như vòng checksum).
    struct stat st;
    if (stat(localPath.c_str(), &st) != 0 ||
        static_cast<uint64_t>(st.st_size) > 256ULL * 1024 * 1024)
        return "";
    std::string md5 = HashHelper::computeFileMd5(localPath);
    if (md5.empty()) return "";
    std::string code, title;
    if (Md5SystemDb::instance().lookup(md5, code, title) && !title.empty() &&
        RomDetector::instance().isKnownSystemCode(code))
        return title;
    return "";
}

int BoxartScraper::normalizeLocalTitles() {
    // systemId=-1: tất cả hệ; stateFilter=1: chỉ game local.
    auto games = DatabaseManager::instance().getGamesBySystem(-1, 1);
    int n = 0;
    for (const auto& g : games) {
        if (g.localPath.empty() ||
            !FileSystemManager::instance().fileExists(g.localPath))
            continue;
        // Giữ tên đã chuẩn / tên riêng (không phải auto theo filename).
        std::string autoTitle = cleanDisplayTitle(g.filename);
        if (!g.title.empty() && g.title != autoTitle && g.title != g.filename)
            continue;
        std::string exact = exactTitleByHash(g.localPath);
        if (exact.empty()) {
            // Zip arcade: từ điển theo stem (chỉ khi đúng hệ arcade).
            std::string ext;
            size_t dot = g.localPath.rfind('.');
            if (dot != std::string::npos) {
                ext = g.localPath.substr(dot + 1);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            }
            if (ext == "zip" || ext == "7z") {
                SystemRecord sys;
                std::string code;
                if (DatabaseManager::instance().getSystemById(g.systemId,
                                                              sys))
                    code = sys.code;
                else
                    code = g.systemCode;
                std::transform(code.begin(), code.end(), code.begin(),
                               ::toupper);
                if (code == "ARCADE" || code == "MAME" || code == "FBNEO" ||
                    code == "MAME2003PLUS" || code == "MAME2010" ||
                    code == "PGM" || code == "NEOGEO" || code == "CPS1" ||
                    code == "CPS2" || code == "CPS3") {
                    std::string stem = g.filename;
                    size_t d2 = stem.rfind('.');
                    if (d2 != std::string::npos) stem = stem.substr(0, d2);
                    exact = resolveArcadeTitle(stem);
                }
            }
        }
        if (!exact.empty() && exact != g.title) {
            if (DatabaseManager::instance().updateGameTitle(g.id, exact))
                ++n;
        }
    }
    return n;
}

bool BoxartScraper::downloadCoverFromUrl(const std::string& url, const std::string& targetPath) {
    if (url.empty() || targetPath.empty()) return false;
    std::vector<std::string> headers = {
        "User-Agent: RomCloud-TrimUI-Scraper/1.0"
    };
    HttpResponse resp = HttpClient::instance().get(url, headers, 12);

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

    // 2. Libretro Thumbnails GitHub (nguồn duy nhất, không acc).
    std::string libretroSys = getLibretroSystemName(sys.code);
    if (!libretroSys.empty()) {
        std::string rawName = game.filename.empty() ? game.title : game.filename;
        // Ưu tiên tên chuẩn No-Intro tra theo MD5/CRC (chính xác tuyệt đối).
        std::vector<std::string> candidates = generateLibretroCandidates(
            rawName, sys.code, exactTitleByHash(game.localPath));

        auto tryDir = [&](const std::string& dir) {
            std::string encDir = HttpClient::instance().urlEncode(dir);
            // Boxarts trước (ảnh cần hiện), rồi mới Titles/Snaps.
            const char* subdirs[] = { "Named_Boxarts", "Named_Titles",
                                      "Named_Snaps" };
            for (const char* subdir : subdirs) {
                for (const auto& cand : candidates) {
                    std::string encodedCand =
                        HttpClient::instance().urlEncode(cand);
                    // Use GitHub raw URL instead of CDN (CDN may have issues)
                    std::string url = "https://raw.githubusercontent.com/libretro/libretro-thumbnails/master/" + encDir + "/" + subdir + "/" + encodedCand + ".png";
                    if (downloadCoverFromUrl(url, targetPath)) {
                        outCoverPath = targetPath;
                        Logger::info("BoxartScraper: Match found on Libretro GitHub (" + cand + ") -> " + targetPath);
                        return true;
                    }
                }
            }
            return false;
        };

        if (tryDir(libretroSys)) return true;
        // Họ arcade rớt hết ở FB Alpha thì thử tiếp thư mục MAME
        // (một số set/hack chỉ có art ở đây: Ataxx, Dimahoo...).
        if (isArcadeFamilySys(sys.code) && libretroSys != "MAME") {
            if (tryDir("MAME")) return true;
        }
    }

    return false;
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

    // Chỉ dùng Libretro (ScreenScraper đã gỡ: dev chết).
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


} // namespace RomCloud
