# RomCloud — Database Specification (Phase 2)

This document details the SQLite database architecture, schema definitions, indexing mechanisms, and data integrity guarantees for **RomCloud** on the TrimUI Brick Pro.

---

## 1. Storage & Concurrency Architecture

- **Database File Location:** `/mnt/SDCARD/Apps/RomCloud/data/library.db`
- **Journal Mode:** Write-Ahead Logging (`PRAGMA journal_mode = WAL;`)
- **Synchronous Mode:** Normal (`PRAGMA synchronous = NORMAL;`)
- **Foreign Keys:** Enforced (`PRAGMA foreign_keys = ON;`)
- **Schema Version:** Managed via `PRAGMA user_version = 1;`

```
                               ┌────────────────────────────────┐
                               │       RomCloud Engine          │
                               └───────────────┬────────────────┘
                                               │
                        ┌──────────────────────┴──────────────────────┐
                        │                                             │
                        ▼                                             ▼
             ┌─────────────────────┐                       ┌─────────────────────┐
             │   DatabaseManager   │                       │     RomIndexer      │
             │ (CRUD, Transactions)│                       │ (SD Card Scanner)   │
             └──────────┬──────────┘                       └──────────┬──────────┘
                        │                                             │
                        └──────────────────────┬──────────────────────┘
                                               │
                                               ▼
                               ┌────────────────────────────────┐
                               │  /mnt/SDCARD/.../library.db    │
                               │   (WAL Mode + Auto Migration)  │
                               └────────────────────────────────┘
```

---

## 2. Relational Schema Definition

### 2.1 Table: `systems`
Represents gaming platforms supported on TrimUI Brick Pro. Seeded with 24 verified TrimUI platform definitions.

```sql
CREATE TABLE systems (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    code TEXT NOT NULL UNIQUE,          -- e.g., 'GBA', 'PS', 'N64', 'NEOGEO'
    name TEXT NOT NULL,                 -- e.g., 'Game Boy Advance', 'Sony PlayStation'
    rom_dir TEXT NOT NULL,              -- e.g., '/mnt/SDCARD/Roms/GBA'
    img_dir TEXT NOT NULL,              -- e.g., '/mnt/SDCARD/Imgs/GBA'
    ext_list TEXT NOT NULL,             -- Comma-separated: 'gba,zip,7z'
    icon_path TEXT,                     -- Path to system icon
    sort_order INTEGER NOT NULL DEFAULT 0
);
```

### 2.2 Table: `games`
Represents unified ROM records spanning local SD storage and remote Google Drive storage.

```sql
CREATE TABLE games (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    cloud_file_id TEXT UNIQUE,          -- Google Drive unique File ID (NULL for un-synced local ROMs)
    system_id INTEGER NOT NULL REFERENCES systems(id) ON DELETE CASCADE,
    filename TEXT NOT NULL,             -- e.g., 'Super Mario 64.z64'
    title TEXT NOT NULL,                -- Normalized display title
    size_bytes INTEGER NOT NULL DEFAULT 0,
    mime_type TEXT,
    drive_modified_time TEXT,           -- ISO8601 timestamp from Drive
    checksum_sha256 TEXT,               -- SHA-256 for integrity verification
    local_path TEXT,                    -- Absolute local path on SD Card
    local_state INTEGER NOT NULL DEFAULT 0, -- 0=CLOUD, 1=LOCAL, 2=DOWNLOADING, 3=ERROR
    cover_path TEXT,                    -- Local cover image path
    created_at TEXT NOT NULL,           -- ISO8601
    updated_at TEXT NOT NULL            -- ISO8601
);

CREATE INDEX idx_games_system_state ON games(system_id, local_state);
CREATE INDEX idx_games_title ON games(title);
CREATE INDEX idx_games_filename ON games(filename);
CREATE INDEX idx_games_cloud_id ON games(cloud_file_id);
```

### 2.3 Table: `settings`
Key-value store for user configurations and preferences.

```sql
CREATE TABLE settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
```

### 2.4 Table: `sync_state`
Tracks delta synchronization state and Drive change tokens.

```sql
CREATE TABLE sync_state (
    key TEXT PRIMARY KEY,
    last_sync_time TEXT,
    sync_token TEXT,
    total_cloud_games INTEGER NOT NULL DEFAULT 0,
    total_local_games INTEGER NOT NULL DEFAULT 0
);
```

---

## 3. Game State Lifecycle (`local_state`)

```
      [Google Drive Master]
               │
               ▼ (Drive Sync)
        ┌─────────────┐
        │ 0: CLOUD    │ ◄── Only metadata on SD card, ROM is on Google Drive
        └──────┬──────┘
               │ (User selects Download)
               ▼
        ┌─────────────┐
        │ 2: DOWNLOADING (Async background download + SHA256 check)
        └──────┬──────┘
               ├─────────────────────────┐ (Network/Disk failure)
               ▼ (Success)               ▼
        ┌─────────────┐           ┌─────────────┐
        │ 1: LOCAL    │           │ 3: ERROR    │
        └──────┬──────┘           └─────────────┘
               │ (User selects Delete Local ROM)
               ▼
        ┌─────────────┐
        │ 0: CLOUD    │ (If synced to Drive; removed if local-only)
        └─────────────┘
```

---

## 4. Local Indexing & Reconciliation Engine

The `RomIndexer` module executes fast parallel scans across `/mnt/SDCARD/Roms/<SYSTEM>`:
1. **Target Extension Filtering:** Queries `ext_list` from `systems` table.
2. **Title Normalization:** Automatically strips file extensions, removes dump tags `(USA)`, `[!]`, and cleans whitespace for clean UI display.
3. **Reconciliation:**
   - If a local ROM file is added manually via USB/PC: Indexer detects file, upserts into `games` with `local_state = LOCAL`.
   - If a local ROM file was deleted externally: Indexer detects missing file; if backed up on Drive, transitions `local_state = CLOUD`; if local-only, removes record cleanly.
4. **Performance:** Sub-second indexing across 24 gaming systems.
