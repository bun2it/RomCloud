// Test RomDetector/RomOrganizer (chay tren host, khong can thiet bi):
// - zip arcade doi ten van nhan ra set FBNeo qua CRC ruot
// - parse output `7z l -slt`
// - unknown-bucket: file khong nhan dien duoc thi bao, khong im lang
// - fixSystemFolder: sua dung cho + dem unknown
#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>

#include "../src/config/AppConfig.h"
#include "../src/logging/Logger.h"
#include "../src/database/DatabaseManager.h"
#include "../src/filesystem/FileSystemManager.h"
#include "../src/rom/FbneoCompatDb.h"
#include "../src/rom/Md5SystemDb.h"
#include "../src/rom/RomDetector.h"
#include "../src/rom/RomOrganizer.h"
#include "../src/database/RomIndexer.h"

namespace RomCloud {
// ---- Stub: AppConfig tro ve repo de doc assets/*.dat that ----
AppConfig::AppConfig() {
    char cwd[1024] = {0};
    if (getcwd(cwd, sizeof(cwd)) != nullptr) m_appRoot = cwd;
    m_sdRoot = m_appRoot;
    m_osType = OSType::STOCK_PS;
}
AppConfig& AppConfig::instance() {
    static AppConfig i;
    return i;
}
std::string AppConfig::getAssetsDir() const { return m_appRoot + "/assets"; }
std::string AppConfig::getRomsDir() const { return m_sdRoot + "/Roms"; }
std::string AppConfig::getSystemRomsDir(const std::string& systemName) const {
    return getRomsDir() + "/" + systemName;
}
std::string AppConfig::getSettingsPath() const {
    return "/tmp/romdetect_settings.json";
}
// ---- Stub: Logger cam ----
void Logger::debug(const std::string&) {}
void Logger::info(const std::string&) {}
void Logger::warn(const std::string&) {}
void Logger::error(const std::string&) {}
void Logger::debug(LogCategory, const std::string&, const char*, int, const char*) {}
void Logger::info(LogCategory, const std::string&, const char*, int, const char*) {}
void Logger::warn(LogCategory, const std::string&, const char*, int, const char*) {}
void Logger::error(LogCategory, const std::string&, const char*, int, const char*) {}
// ---- Stub: BoxartScraper (chi resolveArcadeTitle, tra rong nhu dict thieu).
// Dinh nghia NGOAI class de linker thay (inline trong class bi dead_strip
// xoa vi test TU khong goi truc tiep).
class BoxartScraper {
public:
    static BoxartScraper& instance();
    std::string resolveArcadeTitle(const std::string&);
};
BoxartScraper& BoxartScraper::instance() {
    static BoxartScraper i;
    return i;
}
std::string BoxartScraper::resolveArcadeTitle(const std::string&) {
    return "";
}
} // namespace RomCloud

using namespace RomCloud;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cout << "FAIL line " << __LINE__ << ": " #cond "\n";          \
            return 1;                                                          \
        } else {                                                               \
            std::cout << "PASS line " << __LINE__ << ": " #cond "\n";          \
        }                                                                      \
    } while (0)

int main() {
    // 0. DB that (seed 33 he) + DAT FBNeo that nap tu assets repo.
    DatabaseManager::instance().init("/tmp/romdetect_library.db");
    CHECK(FbneoCompatDb::instance().hasSet("raiden2dx"));

    // 1. Content-guess truc tiep: 15 cap (ten,crc) that cua raiden2dx
    // (lay tu fbneosets.dat, da verify voi file that cua user).
    std::vector<std::pair<std::string, std::string>> pairs = {
        {"7_u0724.bin", "c9ec9469"}, {"copx-d2.6s", "a6732ff9"},
        {"dx_6.3b", "9a9196da"}, {"dx_back1.1s", "90970355"},
        {"dx_back2.2s", "5799af3e"}, {"dx_obj3.4k", "ba381227"},
        {"dx_obj4.6k", "65e50d19"}, {"dx_pcm.3a", "8cf0d17e"},
        {"obj1", "ff08ef0b"}, {"obj2", "638eb771"},
        {"prg1_u1211.bin", "93491f56"}, {"u1110.bin", "b8ad8fe7"},
        {"u1210.bin", "413241e0"}, {"u1212.bin", "505423f4"},
        {"u129.bin", "e0932b6c"},
    };
    {
        std::string stem;
        size_t m = 0, e = 0;
        CHECK(FbneoCompatDb::instance().guessSetByContent(pairs, stem, m, e));
        CHECK(stem == "raiden2dx");
        CHECK(m == 15 && e == 15);
    }
    {
        // Am tinh: 2 CRC la thi false. BIOS chung 1-2 ROM cung false.
        std::string stem;
        size_t m = 0, e = 0;
        std::vector<std::pair<std::string, std::string>> junk = {
            {"a.bin", "00000000"}, {"b.bin", "ffffffff"}};
        CHECK(!FbneoCompatDb::instance().guessSetByContent(junk, stem, m, e));
    }

    // 2. Parser `7z l -slt` (canned output, khong can binary).
    {
        std::string sample =
            "7-Zip [64] 17.05 : Copyright (c) 1999-2021\n"
            "\n"
            "----------\n"
            "Path = game.7z\n"
            "Type = 7z\n"
            "\n"
            "----------\n"
            "Path = 7_u0724.bin\n"
            "Size = 131072\n"
            "CRC = C9EC9469\n"
            "Method = LZMA2:26\n"
            "\n"
            "----------\n"
            "Path = somedir\n"
            "Size = 0\n"
            "\n"
            "----------\n"
            "Path = readme.txt\n"
            "Size = 100\n"
            "CRC = \n"
            "\n";
        auto got = RomDetector::parseSevenZipListing(sample);
        CHECK(got.size() == 1);
        CHECK(got[0].first == "7_u0724.bin");
        CHECK(got[0].second == "C9EC9469");
    }

    const std::string base = "/tmp/romdetect/Roms";
    // 3. Zip doi ten (gamehay.zip, ruot raiden2dx that da patch CRC):
    // nhan ra ARCADE qua FBNEO_CRC, o ARCADE thi giu (cung ho).
    {
        std::string p = base + "/ARCADE/gamehay.zip";
        RomDetectionResult r = RomDetector::instance().detectSystem(p);
        CHECK(r.detected);
        CHECK(r.systemCode == "ARCADE");
        CHECK(r.confidence == "FBNEO_CRC");
        RomDetectionResult o;
        CHECK(RomDetector::instance().isFileInCorrectSystem(p, "ARCADE", o));
    }
    // 4. Ten chuan raiden2dx.zip o ARCADE -> lac, dich FBNEO.
    {
        std::string p = base + "/ARCADE/raiden2dx.zip";
        RomDetectionResult o;
        CHECK(!RomDetector::instance().isFileInCorrectSystem(p, "ARCADE", o));
        CHECK(o.systemCode == "FBNEO");
    }
    // 5. File la (mystery.bin): khong nhan dien, nhung plausible.
    {
        RomDetectionResult r =
            RomDetector::instance().detectSystem(base + "/FC/mystery.bin");
        CHECK(!r.detected);
    }
    // 6. Audit: lac=2 (FC/game.gba->GBA, ARCADE/raiden2dx.zip->FBNEO),
    // unknown=2 (FC/mystery.bin + FC/cart.png PICO-8). cover.png (anh bia)
    // + game.srm (save) bi loai im lang.
    {
        std::vector<UnknownRomRecord> unknown;
        auto items = RomOrganizer::instance().auditMisplacedRoms(
            "/tmp/romdetect/Roms", &unknown);
        CHECK(items.size() == 2);
        CHECK(unknown.size() == 2);
        bool sawMystery = false, sawCart = false;
        for (const auto& u : unknown) {
            if (u.filename == "mystery.bin") sawMystery = true;
            if (u.filename == "cart.png") sawCart = true;
        }
        CHECK(sawMystery);
        CHECK(sawCart);
        bool sawGba = false, sawFbneo = false;
        for (const auto& it : items) {
            if (it.filename == "game.gba" && it.detectedSystemCode == "GBA")
                sawGba = true;
            if (it.filename == "raiden2dx.zip" &&
                it.detectedSystemCode == "FBNEO")
                sawFbneo = true;
        }
        CHECK(sawGba);
        CHECK(sawFbneo);
    }
    // 7. fixSystemFolder FC: chuyen game.gba, unknown dem 2.
    {
        int u = -1;
        auto fixed = RomOrganizer::instance().fixSystemFolder(
            "FC", "/tmp/romdetect/Roms", &u);
        CHECK(u == 2);
        int n = 0;
        for (const auto& it : fixed)
            if (it.fixed) ++n;
        CHECK(n == 1);
        CHECK(FileSystemManager::instance().fileExists(
            "/tmp/romdetect/Roms/GBA/game.gba"));
        CHECK(!FileSystemManager::instance().fileExists(
            "/tmp/romdetect/Roms/FC/game.gba"));
    }
    // 8. fixSystemFolder ARCADE: chuyen raiden2dx.zip -> FBNEO, unknown 0.
    {
        int u = -1;
        auto fixed = RomOrganizer::instance().fixSystemFolder(
            "ARCADE", "/tmp/romdetect/Roms", &u);
        CHECK(u == 0);
        int n = 0;
        for (const auto& it : fixed)
            if (it.fixed) ++n;
        CHECK(n == 1);
        CHECK(FileSystemManager::instance().fileExists(
            "/tmp/romdetect/Roms/FBNEO/raiden2dx.zip"));
    }

    // 9. Quet folder sai hoa/thuong: Roms/gba/ van thay (chi doc).
    {
        SystemRecord gbaSys, fcSys;
        CHECK(DatabaseManager::instance().getSystemByCode("GBA", gbaSys));
        CHECK(DatabaseManager::instance().getSystemByCode("FC", fcSys));
        ScanProgress sp =
            RomIndexer::instance().scanSystem(gbaSys, "/tmp/romdetect2/Roms");
        CHECK(sp.filesFound == 5); // lower + a + unique + dup_a + dup_b
        GameRecord lr;
        CHECK(DatabaseManager::instance().getGameByFilename(gbaSys.id,
                                                            "lower.gba", lr));
        CHECK(lr.localPath.find("/gba/lower.gba") != std::string::npos);
        ScanProgress spFc =
            RomIndexer::instance().scanSystem(fcSys, "/tmp/romdetect2/Roms");
        // b.gba (đuôi .gba) không thuộc ext FC, cover.png cũng không ->
        // scan FC bỏ qua đúng, file lạc để audit/fix lo.
        CHECK(spFc.filesFound == 0);
    }
    // 10. Trung MD5: 2 nhom (a.gba+b.gba, dup_a.gba+dup_b.gba).
    {
        auto groups = RomOrganizer::instance().findDuplicateRoms(
            "/tmp/romdetect2/Roms");
        CHECK(groups.size() == 2);
        bool sawAB = false, sawDup = false;
        for (const auto& g : groups) {
            CHECK(g.files.size() == 2);
            bool a = false, b = false, da = false, db = false;
            for (const auto& f : g.files) {
                if (f.filename == "a.gba") a = true;
                if (f.filename == "b.gba") b = true;
                if (f.filename == "dup_a.gba") da = true;
                if (f.filename == "dup_b.gba") db = true;
            }
            if (a && b) sawAB = true;
            if (da && db) sawDup = true;
        }
        CHECK(sawAB);
        CHECK(sawDup);
    }
    // 11. Gom trung: giu a.gba + dup_a.gba (dung he duong ngan nhat),
    // doi b.gba + dup_b.gba -> _TRUNG, DB cap nhat.
    {
        int moved = RomOrganizer::instance().quarantineDuplicates(
            "/tmp/romdetect2/Roms");
        CHECK(moved == 2);
        CHECK(FileSystemManager::instance().fileExists(
            "/tmp/romdetect2/Roms/_TRUNG/b.gba"));
        CHECK(FileSystemManager::instance().fileExists(
            "/tmp/romdetect2/Roms/_TRUNG/dup_b.gba"));
        CHECK(FileSystemManager::instance().fileExists(
            "/tmp/romdetect2/Roms/GBA/a.gba"));
        CHECK(FileSystemManager::instance().fileExists(
            "/tmp/romdetect2/Roms/GBA/dup_a.gba"));
        CHECK(FileSystemManager::instance().fileExists(
            "/tmp/romdetect2/Roms/gba/lower.gba"));
        SystemRecord fcSys;
        CHECK(DatabaseManager::instance().getSystemByCode("FC", fcSys));
        GameRecord br;
        bool found =
            DatabaseManager::instance().getGameByFilename(fcSys.id, "b.gba", br);
        CHECK(!found || br.localState != GameState::LOCAL);
        SystemRecord gbaSys2;
        CHECK(DatabaseManager::instance().getSystemByCode("GBA", gbaSys2));
        GameRecord dr;
        bool foundDup = DatabaseManager::instance().getGameByFilename(
            gbaSys2.id, "dup_b.gba", dr);
        CHECK(!foundDup || dr.localState != GameState::LOCAL);
    }

    std::cout << "ALL ROMDETECT TESTS PASSED\n";
    return 0;
}
