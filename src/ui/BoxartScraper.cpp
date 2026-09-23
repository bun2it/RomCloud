#include "BoxartScraper.h"
#include "../network/HttpClient.h"
#include "../network/JsonHelper.h"
#include "../filesystem/FileSystemManager.h"
#include "../logging/Logger.h"
#include "../config/AppConfig.h"

#include <algorithm>
#include <cctype>

namespace RomCloud {

static std::string extractFirstOpensearchResult(const std::string& json) {
    size_t firstBracket = json.find('[');
    if (firstBracket == std::string::npos) return "";
    size_t secondBracket = json.find('[', firstBracket + 1);
    if (secondBracket == std::string::npos) return "";
    size_t closeBracket = json.find(']', secondBracket + 1);
    size_t q1 = json.find('\"', secondBracket + 1);
    if (q1 == std::string::npos || (closeBracket != std::string::npos && q1 > closeBracket)) return "";
    size_t q2 = json.find('\"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

static std::string detectReleaseYear(const std::string& text) {
    for (size_t i = 0; i + 3 < text.size(); ++i) {
        if ((i == 0 || !std::isdigit(static_cast<unsigned char>(text[i - 1]))) &&
            (i + 4 == text.size() || !std::isdigit(static_cast<unsigned char>(text[i + 4])))) {
            if ((text[i] == '1' && text[i+1] == '9' && text[i+2] >= '7' && text[i+2] <= '9' && std::isdigit(text[i+3])) ||
                (text[i] == '2' && text[i+1] == '0' && (text[i+2] == '0' || text[i+2] == '1' || text[i+2] == '2') && std::isdigit(text[i+3]))) {
                return text.substr(i, 4);
            }
        }
    }
    return "";
}

static std::string detectGenre(const std::string& text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find("role-playing") != std::string::npos || lower.find("rpg") != std::string::npos) return "Role-Playing (RPG)";
    if (lower.find("platform") != std::string::npos) return "Platformer";
    if (lower.find("action-adventure") != std::string::npos) return "Action-Adventure";
    if (lower.find("fighting") != std::string::npos) return "Fighting";
    if (lower.find("beat 'em up") != std::string::npos || lower.find("beat-em-up") != std::string::npos) return "Beat 'em up";
    if (lower.find("shoot 'em up") != std::string::npos || lower.find("shmup") != std::string::npos) return "Shoot 'em up";
    if (lower.find("run and gun") != std::string::npos) return "Run and Gun";
    if (lower.find("racing") != std::string::npos) return "Racing";
    if (lower.find("puzzle") != std::string::npos) return "Puzzle";
    if (lower.find("sports") != std::string::npos) return "Sports";
    if (lower.find("strategy") != std::string::npos) return "Strategy";
    if (lower.find("survival horror") != std::string::npos) return "Survival Horror";
    if (lower.find("action") != std::string::npos) return "Action";
    if (lower.find("adventure") != std::string::npos) return "Adventure";
    return "";
}

static std::string detectDeveloper(const std::string& text) {
    size_t devPos = text.find("developed by ");
    if (devPos != std::string::npos) {
        std::string sub = text.substr(devPos + 13);
        size_t endPos = sub.find_first_of(".,;");
        size_t andPub = sub.find(" and published by ");
        if (andPub != std::string::npos && (endPos == std::string::npos || andPub < endPos)) {
            std::string dev = sub.substr(0, andPub);
            std::string pubSub = sub.substr(andPub + 18);
            size_t pubEnd = pubSub.find_first_of(".,;(");
            if (pubEnd != std::string::npos) pubSub = pubSub.substr(0, pubEnd);
            size_t forPos = pubSub.find(" for ");
            if (forPos != std::string::npos) pubSub = pubSub.substr(0, forPos);
            return dev + " / " + pubSub;
        } else if (endPos != std::string::npos) {
            std::string dev = sub.substr(0, endPos);
            size_t forPos = dev.find(" for ");
            if (forPos != std::string::npos) dev = dev.substr(0, forPos);
            return dev;
        }
    }
    size_t pubPos = text.find("published by ");
    if (pubPos != std::string::npos) {
        std::string pub = text.substr(pubPos + 13);
        size_t endPos = pub.find_first_of(".,;(");
        if (endPos != std::string::npos) pub = pub.substr(0, endPos);
        size_t forPos = pub.find(" for ");
        if (forPos != std::string::npos) pub = pub.substr(0, forPos);
        return pub;
    }
    return "";
}

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
    for (char& c : name) {
        if (c == '&') c = '_';
        else if (c == '/' || c == '\\' || c == '*' || c == '?' || c == '\"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    return name;
}

std::string BoxartScraper::cleanSearchQuery(const std::string& filenameOrTitle) {
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

bool BoxartScraper::downloadCoverFromUrl(const std::string& url, const std::string& targetPath) {
    if (url.empty() || targetPath.empty()) return false;
    std::vector<std::string> headers = {
        "User-Agent: RomCloud-TrimUI-Scraper/1.0 (https://github.com/bun2it/RomCloud)"
    };
    HttpResponse resp = HttpClient::instance().get(url, headers, 8000);
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
    std::string libretroSys = getLibretroSystemName(sys.code);
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

    if (!libretroSys.empty()) {
        const char* subdirs[] = { "Named_Boxarts", "Named_Titles", "Named_Snaps" };
        for (const char* subdir : subdirs) {
            std::string encodedSys = HttpClient::instance().urlEncode(libretroSys);
            std::string encodedName = HttpClient::instance().urlEncode(cleanName);
            std::string url = "https://thumbnails.libretro.com/" + encodedSys + "/" + subdir + "/" + encodedName + ".png";

            Logger::info("BoxartScraper: Checking Libretro " + url);
            HttpResponse resp = HttpClient::instance().get(url, {}, 5000);

            if (resp.statusCode == 200 && !resp.body.empty() && resp.body.size() > 512) {
                FILE* fp = fopen(targetPath.c_str(), "wb");
                if (fp) {
                    fwrite(resp.body.data(), 1, resp.body.size(), fp);
                    fclose(fp);
                    outCoverPath = targetPath;
                    Logger::info("BoxartScraper: Saved cover to " + targetPath);
                    return true;
                }
            }
        }
    }

    return false;
}

GameScrapeResult BoxartScraper::scrapeGameInfo(const GameRecord& game, const SystemRecord& sys) {
    GameScrapeResult result;
    result.title = game.title;

    // 1. First attempt to scrape/reuse cover via Libretro CDN
    std::string coverPath;
    if (scrapeCover(game, sys, coverPath)) {
        result.coverPath = coverPath;
        result.coverFound = true;
    }

    // 2. Fetch game info & metadata via Wikipedia Open REST API
    std::string query = cleanSearchQuery(game.filename.empty() ? game.title : game.filename);
    if (query.empty()) query = game.title;

    std::vector<std::string> headers = {
        "User-Agent: RomCloud-TrimUI-Scraper/1.0 (https://github.com/bun2it/RomCloud)"
    };

    std::string opensearchUrl = "https://en.wikipedia.org/w/api.php?action=opensearch&search=" +
                               HttpClient::instance().urlEncode(query) + "&limit=1&format=json";
    Logger::info("BoxartScraper: Querying Wikipedia metadata for: " + query);
    HttpResponse oresp = HttpClient::instance().get(opensearchUrl, headers, 6000);

    std::string targetTitle = extractFirstOpensearchResult(oresp.body);
    if (targetTitle.empty()) {
        // Fallback: try searching with system code or video game suffix
        std::string fallbackQuery = query + " video game";
        std::string fallbackUrl = "https://en.wikipedia.org/w/api.php?action=opensearch&search=" +
                                  HttpClient::instance().urlEncode(fallbackQuery) + "&limit=1&format=json";
        oresp = HttpClient::instance().get(fallbackUrl, headers, 6000);
        targetTitle = extractFirstOpensearchResult(oresp.body);
    }

    if (!targetTitle.empty()) {
        std::string summaryUrl = "https://en.wikipedia.org/api/rest_v1/page/summary/" +
                                 HttpClient::instance().urlEncode(targetTitle);
        HttpResponse sresp = HttpClient::instance().get(summaryUrl, headers, 6000);
        if (sresp.statusCode == 200 && !sresp.body.empty()) {
            std::string wikiTitle = JsonHelper::extractString(sresp.body, "title");
            std::string wikiDesc = JsonHelper::extractString(sresp.body, "description");
            std::string wikiExtract = JsonHelper::extractString(sresp.body, "extract");
            std::string thumbUrl = JsonHelper::extractString(sresp.body, "source");

            if (!wikiTitle.empty()) result.title = wikiTitle;
            if (!wikiExtract.empty()) result.description = wikiExtract;

            // Detect year
            result.releaseYear = detectReleaseYear(wikiDesc);
            if (result.releaseYear.empty()) result.releaseYear = detectReleaseYear(wikiExtract);

            // Detect genre
            result.genre = detectGenre(wikiDesc);
            if (result.genre.empty()) result.genre = detectGenre(wikiExtract);

            // Detect developer/publisher
            result.developer = detectDeveloper(wikiExtract);

            // If Libretro did not find cover, download Wikipedia thumbnail as boxart
            if (!result.coverFound && !thumbUrl.empty()) {
                std::string cleanName = cleanNameForLibretro(game.filename.empty() ? game.title : game.filename);
                std::string targetDir = AppConfig::instance().getImgsDir() + "/" + sys.code;
                FileSystemManager::instance().createDirectoryRecursive(targetDir);
                std::string targetPath = targetDir + "/" + cleanName + ".png";

                if (downloadCoverFromUrl(thumbUrl, targetPath)) {
                    result.coverPath = targetPath;
                    result.coverFound = true;
                }
            }

            result.success = true;
        }
    }

    if (result.coverFound && !result.success) {
        // At least cover was found
        result.success = true;
    }

    // 3. Save whatever metadata was scraped into SQLite
    if (result.success) {
        DatabaseManager::instance().updateGameMetadata(game.id, result.description, result.releaseYear,
                                                      result.developer, result.genre, result.coverPath);
    }

    return result;
}

} // namespace RomCloud
