#include <iostream>
#include <sqlite3.h>
#include "../src/database/Schema.h"

int main() {
    sqlite3* db = nullptr;
    sqlite3_open("/tmp/debug_library.db", &db);

    char* err = nullptr;
    int rc = sqlite3_exec(db, RomCloud::SCHEMA_V1_SQL, nullptr, nullptr, &err);
    std::cout << "Schema exec rc: " << rc << " err: " << (err ? err : "none") << std::endl;

    sqlite3_stmt* stmt = nullptr;
    const char* insertSql = "INSERT OR IGNORE INTO systems (code, name, rom_dir, img_dir, ext_list, sort_order) VALUES (?, ?, ?, ?, ?, ?);";
    rc = sqlite3_prepare_v2(db, insertSql, -1, &stmt, nullptr);
    std::cout << "Prepare rc: " << rc << std::endl;

    for (size_t i = 0; i < RomCloud::DEFAULT_SYSTEMS_COUNT; ++i) {
        const auto& sys = RomCloud::DEFAULT_SYSTEMS[i];
        sqlite3_bind_text(stmt, 1, sys.code, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, sys.name, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, sys.rom_dir, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, sys.img_dir, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, sys.ext_list, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 6, sys.sort_order);

        int stepRc = sqlite3_step(stmt);
        if (stepRc != SQLITE_DONE) {
            std::cout << "Step error on " << sys.code << ": " << stepRc << " - " << sqlite3_errmsg(db) << std::endl;
        }
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);

    const char* countSql = "SELECT COUNT(*) FROM systems;";
    sqlite3_prepare_v2(db, countSql, -1, &stmt, nullptr);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        std::cout << "Total systems in DB: " << sqlite3_column_int(stmt, 0) << std::endl;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return 0;
}
