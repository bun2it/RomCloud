#pragma once
#include <string>

namespace RomCloud {

constexpr int CURRENT_SCHEMA_VERSION = 1;

const char* const SCHEMA_V1_SQL = R"(
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS systems (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    code TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    rom_dir TEXT NOT NULL,
    img_dir TEXT NOT NULL,
    ext_list TEXT NOT NULL,
    icon_path TEXT,
    sort_order INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS games (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    cloud_file_id TEXT UNIQUE,
    system_id INTEGER NOT NULL REFERENCES systems(id) ON DELETE CASCADE,
    filename TEXT NOT NULL,
    title TEXT NOT NULL,
    size_bytes INTEGER NOT NULL DEFAULT 0,
    mime_type TEXT,
    drive_modified_time TEXT,
    checksum_sha256 TEXT,
    local_path TEXT,
    local_state INTEGER NOT NULL DEFAULT 0, -- 0=CLOUD, 1=LOCAL, 2=DOWNLOADING, 3=ERROR
    cover_path TEXT,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_games_system_state ON games(system_id, local_state);
CREATE INDEX IF NOT EXISTS idx_games_title ON games(title);
CREATE INDEX IF NOT EXISTS idx_games_filename ON games(filename);
CREATE INDEX IF NOT EXISTS idx_games_cloud_id ON games(cloud_file_id);

CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS sync_state (
    key TEXT PRIMARY KEY,
    last_sync_time TEXT,
    sync_token TEXT,
    total_cloud_games INTEGER NOT NULL DEFAULT 0,
    total_local_games INTEGER NOT NULL DEFAULT 0
);
)";

struct DefaultSystemInfo {
    const char* code;
    const char* name;
    const char* rom_dir;
    const char* img_dir;
    const char* ext_list;
    int sort_order;
};

const DefaultSystemInfo DEFAULT_SYSTEMS[] = {
    {"GBA",      "Game Boy Advance",            "GBA",          "GBA",      "gba|bin|zip|7z",                   10},
    {"GBC",      "Game Boy Color",              "GBC",          "GBC",      "gb|gbc|zip|7z",                    20},
    {"GB",       "Game Boy",                    "GB",           "GB",       "gb|zip|7z",                        30},
    {"FC",       "NES / Famicom",               "FC",           "FC",       "nes|fds|unf|unif|zip|7z",          40},
    {"SFC",      "Super Nintendo / SFC",        "SFC",          "SFC",      "smc|fig|sfc|gd3|dx2|swc|zip|7z",   50},
    {"MD",       "Genesis / Mega Drive",        "MD",           "MD",       "bin|gen|md|smd|32x|zip|7z",        60},
    {"PS",       "Sony PlayStation",            "PS",           "PS",       "bin|cue|img|mdf|pbp|toc|cbn|m3u|chd|iso", 70},
    {"PSP",      "PlayStation Portable",        "PSP",          "PSP",      "iso|cso|pbp|chd",                  80},
    {"N64",      "Nintendo 64",                 "N64",          "N64",      "n64|v64|z64|zip|7z",               90},
    {"NDS",      "Nintendo DS",                 "NDS",          "NDS",      "nds|zip|7z",                       100},
    {"ARCADE",   "Arcade (MAME / FBNeo)",       "ARCADE",       "ARCADE",   "zip|7z",                           110},
    {"NEOGEO",   "SNK Neo Geo",                 "NEOGEO",       "NEOGEO",   "zip|7z",                           120},
    {"PCE",      "PC Engine / TurboGrafx",      "PCE",          "PCE",      "pce|cue|zip|7z|chd",               130},
    {"DC",       "Sega Dreamcast",              "DC",           "DC",       "gdi|cdi|chd",                      140},
    {"SS",       "Sega Saturn",                 "SS",           "SS",       "chd|cue|iso|bin",                  150},
    {"WS",       "WonderSwan / Color",          "WS",           "WS",       "ws|wsc|zip|7z",                    160},
    {"PICO8",    "PICO-8",                      "PICO8",        "PICO8",    "p8|png|zip",                       170},
    {"ATARI2600","Atari 2600",                  "ATARI2600",    "ATARI2600","a26|bin|zip|7z",                   180},
    {"ATARI7800","Atari 7800",                  "ATARI7800",    "ATARI7800","a78|bin|zip|7z",                   190},
    {"LYNX",     "Atari Lynx",                  "LYNX",         "LYNX",     "lnx|zip|7z",                       200},
    {"MS",       "Master System",               "MS",           "MS",       "sms|rom|gg|sg|zip|7z",             210},
    {"GG",       "Game Gear",                   "GG",           "GG",       "gg|zip|7z",                        220},
    {"SEGACD",   "Sega CD",                     "SEGACD",       "SEGACD",   "bin|chd|cue|iso|zip",              230},
    {"OPENBOR",  "OpenBOR",                     "OPENBOR",      "OPENBOR",  "pak|zip",                          240}
};

const size_t DEFAULT_SYSTEMS_COUNT = sizeof(DEFAULT_SYSTEMS) / sizeof(DEFAULT_SYSTEMS[0]);

} // namespace RomCloud
