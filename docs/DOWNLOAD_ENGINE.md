# RomCloud — On-Demand Download & Integrity Engine Specification (Phase 6)

This document details the on-demand ROM download engine, streaming transfer mechanics, and cryptographic integrity verification in **Phase 6** of RomCloud on the TrimUI Brick Pro.

---

## 1. On-Demand Transfer Architecture

```
  ┌────────────────────────────────────────────────────────┐
  │         User Selects Cloud ROM in Game Browser         │
  │                  (Presses [A] Button)                  │
  └───────────────────────────┬────────────────────────────┘
                              │
                              ▼
  ┌────────────────────────────────────────────────────────┐
  │                 Storage Capacity Check                 │
  │  Ensures: free_bytes >= game_size + 50 MB buffer       │
  └───────────────────────────┬────────────────────────────┘
                              │ Passed
                              ▼
  ┌────────────────────────────────────────────────────────┐
  │              DownloadManager (Worker Thread)           │
  │  1. Request: GET /files/<id>?alt=media (Auth Bearer)   │
  │  2. Stream writes to: temp/<filename>.part             │
  │  3. Real-time progress callback: bytes, speed, ETA     │
  └───────────────────────────┬────────────────────────────┘
                              │ Download Complete
                              ▼
  ┌────────────────────────────────────────────────────────┐
  │            Streaming MD5 Checksum Verification         │
  │  Calculates file hash & compares against Drive MD5     │
  └───────────────────────────┬────────────────────────────┘
                              │
               ┌──────────────┴──────────────┐
               │ Match                       │ Mismatch / Corruption
               ▼                             ▼
  ┌─────────────────────────┐   ┌─────────────────────────┐
  │ Move to:                │   │ Delete corrupt .part    │
  │ /mnt/SDCARD/Roms/...    │   │ State: ERROR            │
  │ SQLite: local_state = 1 │   │ Notify User             │
  └─────────────────────────┘   └─────────────────────────┘
```

---

## 2. Cryptographic Integrity & Verification (`HashHelper`)

- **Standard:** RFC 1321 MD5 Checksum (matches Google Drive REST API v3 `md5Checksum`).
- **Memory Safety:** Streaming 64 KB chunk-based buffer hashing ensures ROMs up to multiple gigabytes (e.g. PS1/PSP) are verified without exceeding the 1 GB physical RAM limit.
- **Fail-Safe Rollback:** If the calculated checksum does not match the master copy, the temporary file is deleted immediately and the game record remains in the cloud-only state.

---

## 3. UI Overlay (`renderDownloadOverlay`)

- **Status Banner:** Shows current step (`Preparing...`, `Downloading...`, `Verifying MD5...`).
- **Progress Bar:** High-contrast cyan/green bar rendered with exact percentage.
- **Transfer Metrics:** Downloaded bytes vs Total bytes, current speed (`MB/s`), and ETA.
- **Cancellation:** Pressing `[B]` safely aborts the active curl connection and removes partial `.part` files.
