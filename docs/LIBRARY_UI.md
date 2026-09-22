# RomCloud — Library UI & Cover Flow Specification (Phase 3)

This document details the Native 1024x768 Library User Interface, State Machine, Cover Box Art renderer, and Gamepad navigation mechanics implemented in **Phase 3** of RomCloud on the TrimUI Brick Pro.

---

## 1. UI Hierarchy & State Flow

```
                      ┌────────────────────────┐
                      │      STATE: MENU       │ (Main Menu)
                      └───────────┬────────────┘
                                  │ [A] MY LIBRARY
                                  ▼
                      ┌────────────────────────┐
                      │  STATE: SYSTEM_SELECT  │ (24 Gaming Platforms Grid/List)
                      └───────────┬────────────┘
                                  │ [A] Select System (e.g. GBA, PS, N64)
                                  ▼
                      ┌────────────────────────┐
                      │   STATE: GAME_LIST     │ ◄───┐
                      └───────────┬────────────┘     │
                                  │ [X] Delete ROM   │ [B] Cancel
                                  ▼                  │
                      ┌────────────────────────┐     │
                      │ STATE: CONFIRM_DELETE  │ ────┘
                      └────────────────────────┘
```

---

## 2. Screen Layout Breakdown (1024 x 768)

### 2.1 Header Bar (`y: 0` to `64`)
- **Left:** `RomCloud` brand logo + Subtitle `FOR TRIMUI BRICK PRO`.
- **Center:** Active System Title (e.g., `N64 — Nintendo 64`).
- **Right:** Filter Badge Toggle (`[SELECT] FILTER: ALL` / `LOCAL` / `CLOUD`) or Wi-Fi status indicator.

### 2.2 Split-View Game Browser (`y: 75` to `705`)
- **Left Panel (Game List):** `x: 30`, `w: 540`, `h: 630`
  - 7 items visible per page with vertical scrollbar indicator.
  - Highlighting current selection with cyan left-accent bar and glowing border.
  - State badges: Green `LOCAL`, Blue `CLOUD`, Yellow `SYNC`.
  - Display game title + formatted file size (e.g., `24.26 MB`).
- **Right Panel (Details & Box Art):** `x: 590`, `w: 404`, `h: 630`
  - Top: 340x280 Box Art Frame with drop shadow, aspect-ratio fit, and procedural fallback box if image is not yet cached.
  - Bottom: Metadata summary (Platform, Exact File Size, Storage Location, SHA256 Checksum, Action Badge).

### 2.3 Footer Bar (`y: 715` to `768`)
- Dynamic button legends matching current state:
  - In Game List: `[A] Action  [B] Systems  [X] Delete ROM  [SELECT] Filter  [L1/R1] Page`
  - In System List: `[A] Select System  [B] Main Menu  [L1/R1] Page`

---

## 3. Cover Art Engine (`CoverManager`)

- **Format Support:** PNG, JPG, JPEG via `SDL2_image`.
- **Search Resolution Strategy:**
  1. `game.coverPath` (if already registered in database)
  2. `/mnt/SDCARD/Imgs/<SYSTEM_CODE>/<GAME_NAME>.png` (Standard TrimUI scraper directory)
  3. `/mnt/SDCARD/Imgs/<SYSTEM_CODE>/<GAME_NAME>.jpg`
  4. `/mnt/SDCARD/Apps/RomCloud/cache/covers/<SYSTEM_CODE>/<FILE_ID>.png` (Cloud downloaded covers)
  5. **Procedural Fallback:** High-contrast 2D/3D gradient cartridge box with console badge and title centering.
- **Memory Safety:** Hardware-accelerated texture cache with LRU eviction (maximum 64 concurrent textures) ensuring lightweight RAM usage (< 25MB).

---

## 4. Gamepad Mapping (TrimUI Brick Pro)

| Button | Function in Library View | Function in System View |
| :--- | :--- | :--- |
| **D-Pad Up / Down** | Navigate game list line by line | Navigate systems line by line |
| **L1 / R1** | Page jump (7 items at once) | Page jump (6 systems at once) |
| **A (South)** | Action (Launch local / Trigger download) | Enter selected system game list |
| **B (East)** | Back to System Selection | Back to Main Menu |
| **X (North)** | Delete Local ROM (Opens confirmation modal) | — |
| **SELECT** | Cycle Filter (`ALL` → `LOCAL ONLY` → `CLOUD ONLY`) | — |
| **START** | Quick menu / Options | — |
