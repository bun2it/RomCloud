#include "BoxartScraper.h"
#include "../network/HttpClient.h"
#include "../network/JsonHelper.h"
#include "../filesystem/FileSystemManager.h"
#include "../logging/Logger.h"
#include "../config/AppConfig.h"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace RomCloud {

// Default public ScreenScraper identification for open-source community
static const char* DEFAULT_DEV_ID = "bun2it";
static const char* DEFAULT_DEV_PASS = "RomCloudTrimUI2026";

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

std::string BoxartScraper::cleanNameForLibretro(const std::string& filenameOrTitle) {
    std::string name = filenameOrTitle;
    size_t lastDot = name.find_last_of('.');
    if (lastDot != std::string::npos && lastDot > 0) {
        name = name.substr(0, lastDot);
    }

    // Libretro naming conventions replace illegal filesystem characters with '_'
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

    // Strip parentheses and brackets: (USA, Europe), [!], (v1.1), (Track 1), etc.
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

    // Handle ", The": e.g. "Legend of Zelda, The - The Minish Cap" -> "The Legend of Zelda The Minish Cap"
    size_t thePos = clean.find(", The");
    if (thePos != std::string::npos) {
        std::string before = clean.substr(0, thePos);
        std::string after = clean.substr(thePos + 5);
        clean = "The " + before + after;
    }

    // Replace dashes and underscores with spaces
    for (char& c : clean) {
        if (c == '_' || c == '-') c = ' ';
    }

    // Collapse multiple whitespace
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
    // 1. If cover already exists locally on SD card, reuse it immediately
    std::string cleanName = cleanNameForLibretro(game.filename.empty() ? game.title : game.filename);
    std::string targetDir = AppConfig::instance().getImgsDir() + "/" + sys.code;
    FileSystemManager::instance().createDirectoryRecursive(targetDir);
    std::string targetPath = targetDir + "/" + cleanName + ".png";

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

    // 3. Fallback to Libretro Thumbnails CDN
    std::string libretroSys = getLibretroSystemName(sys.code);
    if (!libretroSys.empty()) {
        const char* subdirs[] = { "Named_Boxarts", "Named_Titles", "Named_Snaps" };
        for (const char* subdir : subdirs) {
            std::string encodedSys = HttpClient::instance().urlEncode(libretroSys);
            std::string encodedName = HttpClient::instance().urlEncode(cleanName);
            std::string url = "https://thumbnails.libretro.com/" + encodedSys + "/" + subdir + "/" + encodedName + ".png";

            Logger::info("BoxartScraper: Checking Libretro CDN " + url);
            if (downloadCoverFromUrl(url, targetPath)) {
                outCoverPath = targetPath;
                return true;
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

    // ScreenScraper API requires user credentials (free registration at screenscraper.fr)
    if (ssUser.empty() || ssPass.empty()) {
        return false;
    }

    int sysId = getScreenScraperSystemId(sys.code);
    std::string fn = game.filename.empty() ? game.title : game.filename;

    std::string url = "https://api.screenscraper.fr/api2/jeuInfos.php?devid=" + HttpClient::instance().urlEncode(ssDevId) +
                      "&devpassword=" + HttpClient::instance().urlEncode(ssDevPass) +
                      "&softname=RomCloud&output=json" +
                      "&ssid=" + HttpClient::instance().urlEncode(ssUser) +
                      "&sspassword=" + HttpClient::instance().urlEncode(ssPass) +
                      "&romnom=" + HttpClient::instance().urlEncode(fn);

    if (sysId > 0) {
        url += "&systemeid=" + std::to_string(sysId);
    }

    Logger::info("BoxartScraper: Querying ScreenScraper.fr for " + fn);
    std::vector<std::string> headers = { "User-Agent: RomCloud-TrimUI-Scraper/1.0" };
    HttpResponse resp = HttpClient::instance().get(url, headers, 8000);

    if (resp.statusCode != 200 || resp.body.empty() || resp.body.find("\"response\"") == std::string::npos) {
        Logger::warn("BoxartScraper: ScreenScraper query returned status " + std::to_string(resp.statusCode));
        return false;
    }

    // Parse ScreenScraper JSON response
    std::string body = resp.body;
    if (body.find("\"jeu\"") == std::string::npos) {
        return false;
    }

    outResult.source = "screenscraper";

    // 1. Title
    std::string ssNom = JsonHelper::extractString(body, "nom_us");
    if (ssNom.empty()) ssNom = JsonHelper::extractString(body, "nom_eu");
    if (ssNom.empty()) ssNom = JsonHelper::extractString(body, "nom");
    outResult.title = ssNom.empty() ? cleanRomTitle(fn) : ssNom;

    // 2. Release Year
    // Look for "dates":[{"region":"us","text":"1996-03-21"}] or similar
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

    // 5. Synopsis / Story (Look for Vietnamese first, then English)
    size_t synPos = body.find("\"synopsis\"");
    if (synPos != std::string::npos) {
        std::string synBlock = body.substr(synPos, 2000);
        size_t viPos = synBlock.find("\"langue\":\"vi\"");
        if (viPos != std::string::npos) {
            outResult.description = JsonHelper::extractString(synBlock.substr(viPos, 600), "texte");
        }
        if (outResult.description.empty()) {
            size_t enPos = synBlock.find("\"langue\":\"en\"");
            if (enPos != std::string::npos) {
                outResult.description = JsonHelper::extractString(synBlock.substr(enPos, 600), "texte");
            }
        }
        if (outResult.description.empty()) {
            outResult.description = JsonHelper::extractString(synBlock, "texte");
        }
    }

    // 6. Media Boxart (Check box-2d, box-3d, or screenshot)
    std::string mediaUrl;
    size_t mediaPos = body.find("\"media_box2d\"");
    if (mediaPos != std::string::npos) {
        mediaUrl = JsonHelper::extractString(body.substr(mediaPos, 400), "url");
    }
    if (mediaUrl.empty()) {
        mediaPos = body.find("\"media_box3d\"");
        if (mediaPos != std::string::npos) {
            mediaUrl = JsonHelper::extractString(body.substr(mediaPos, 400), "url");
        }
    }
    if (mediaUrl.empty()) {
        mediaPos = body.find("\"media_wheel\"");
        if (mediaPos != std::string::npos) {
            mediaUrl = JsonHelper::extractString(body.substr(mediaPos, 400), "url");
        }
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

    // Deduce system description
    outResult.description = "Trò chơi kinh điển trên hệ máy " + sys.name + " (" + sys.code + ").";

    // Attempt to download cover from Libretro Thumbnails
    std::string coverPath;
    if (scrapeCover(game, sys, coverPath)) {
        outResult.coverPath = coverPath;
        outResult.coverFound = true;
        outResult.success = true;
        return true;
    }

    outResult.success = true;
    return true;
}

GameScrapeResult BoxartScraper::scrapeGameInfo(const GameRecord& game, const SystemRecord& sys) {
    GameScrapeResult result;
    result.title = game.title.empty() ? cleanRomTitle(game.filename) : game.title;

    // 1. Try ScreenScraper first (high-precision retro database)
    bool ssSuccess = scrapeFromScreenScraper(game, sys, result);

    // 2. If ScreenScraper wasn't configured or didn't find boxart, query Libretro
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

    // 3. Save whatever metadata was scraped into SQLite database
    if (result.success) {
        DatabaseManager::instance().updateGameMetadata(game.id, result.description, result.releaseYear,
                                                      result.developer, result.genre, result.coverPath);
    }

    return result;
}

bool BoxartScraper::startAutoScrapeSdCard(bool forceAll) {
    if (m_isScraping.load()) {
        Logger::warn("BoxartScraper: Auto-scrape already running in background.");
        return false;
    }

    std::vector<GameRecord> candidates;
    if (forceAll) {
        candidates = DatabaseManager::instance().getGamesBySystem(0, 1); // state 1 = local
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

            // Small delay to be polite to servers
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

    // Test with a standard popular game query: Super Mario World on SFC (systemeid=4)
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
