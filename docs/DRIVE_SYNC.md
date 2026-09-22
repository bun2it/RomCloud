# RomCloud — Google Drive Master Library Sync Specification (Phase 5)

This document details the Google Drive Master ROM Library synchronization engine implemented in **Phase 5** of RomCloud on the TrimUI Brick Pro.

---

## 1. Master-to-Local Synchronization Architecture

```
  ┌────────────────────────────────────────────────────────┐
  │         Google Drive Master ROM Library                │
  │  /RomCloud/ (or /Roms/)                                │
  │    ├── GBA/     (Game Boy Advance ROMs)                │
  │    ├── PS/      (Sony PlayStation ISO/BINs)            │
  │    ├── N64/     (Nintendo 64 ROMs)                     │
  │    └── NEOGEO/  (SNK Neo Geo ROMs)                     │
  └───────────────────────────┬────────────────────────────┘
                              │
                              │ HTTPS Google Drive REST API v3
                              ▼
  ┌────────────────────────────────────────────────────────┐
  │                 DriveSyncEngine                        │
  │  1. Discovers Root & Platform Subfolders               │
  │  2. Enumerates Files & Metadata (ID, size, checksum)   │
  │  3. Reconciles with Local Files                        │
  └───────────────────────────┬────────────────────────────┘
                              │
                              ▼
  ┌────────────────────────────────────────────────────────┐
  │      TrimUI SQLite Database (data/library.db)          │
  │                                                        │
  │  • local_state = 1 (LOCAL): Preserved if file on SD    │
  │  • local_state = 0 (CLOUD): Downloadable on Demand     │
  └────────────────────────────────────────────────────────┘
```

---

## 2. Synchronization Rules & Data Integrity

1. **Local ROM Preservation:**
   - If a ROM file already exists on `/mnt/SDCARD/Roms/<SYSTEM>/...`:
     - Its `local_state` remains `1 (LOCAL)`.
     - Its `local_path` is preserved.
     - `cloud_file_id`, `drive_modified_time`, and `checksum_sha256` are linked to the cloud master copy.
2. **Cloud-Only Ingestion:**
   - If a ROM exists only on Google Drive:
     - A new record is inserted with `local_state = 0 (CLOUD)`.
     - `local_path` is empty (no local disk storage is consumed).
     - Full metadata (title, formatted size, checksum, drive file ID) is available for instant browsing.
3. **Transaction Batching:**
   - All metadata insertions and updates are wrapped in an atomic SQLite transaction (`BEGIN TRANSACTION` / `COMMIT`), ensuring sub-second sync time across thousands of games.
4. **Asynchronous Execution:**
   - Synchronization runs on a background worker thread (`std::thread`), with cancel support (`[B] Cancel`), ensuring the UI stays at 60 FPS.

---

## 3. UI Integration & Controls

| Screen | Action | Description |
| :--- | :--- | :--- |
| **Main Menu** | Select `SYNC DRIVE LIBRARY` | Starts background Google Drive synchronization |
| **System Selection** | Press `[Y]` Button | Triggers cloud sync directly from system screen |
| **Game List** | Press `[Y]` Button | Triggers cloud sync directly from game browser |
| **Sync Overlay** | Press `[B]` Button | Safely cancels ongoing sync operation |
