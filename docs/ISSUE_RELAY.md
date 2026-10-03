# Device Identity & Issue Relay Implementation

**Status:** Phase 1 & 2 Complete (Worker Deployed)
**Last Updated:** 2026-10-01
**Author:** Claude Agent

---

## Overview

Two new features are being implemented for RomCloud:

1. **Device Identity** - Stable, privacy-safe device identifier for bug reporting
2. **Issue Relay** - Cloudflare Worker to forward bug reports to GitHub Issues (private repo)

---

## Phase 1: Device Identity ✅

### Files Created
- `src/diagnostics/DeviceIdentity.h` - Header
- `src/diagnostics/DeviceIdentity.cpp` - Implementation

### How It Works

**Hardware ID Priority:**
1. Chip/Serial ID (`/sys/class/mtd/*/device/id`, `/proc/device-tree/serial-number`, etc.)
2. MAC address (`/sys/class/net/eth0/address`)
3. machine-id (`/etc/machine-id`)
4. Fallback: Persistent install ID (`config/device_id`)

**Validation:** Rejects empty, default, all-zeros, and placeholder values.

**Output Format:** `RC-<8-char-hash>`
- Example: `RC-a3f2b8c1`
- Uses SHA-256 with domain prefix "RomCloud-v2"
- Raw hardware IDs are NEVER exposed

### Key Methods
```cpp
DeviceIdentity::instance().getDeviceId()     // Returns "RC-xxxxxxxx"
DeviceIdentity::instance().hasHardwareId()   // true if real HW ID
DeviceIdentity::instance().getIdSource()     // "chip_id", "mac", "machine_id", or "install_id"
```

### Build Integration
Added to both `build.sh` (ARM) and `build_pc.sh` (PC simulator):
```bash
src/diagnostics/DeviceIdentity.cpp
```

---

## Phase 2: Cloudflare Worker Relay ✅

### Deployed Endpoint
```
https://romcloud-issue-relay.bun2it.workers.dev
```

### Architecture
```
┌──────────────┐     HTTPS      ┌─────────────────┐     GitHub API    ┌─────────────────┐
│  RomCloud    │ ────────────►  │ Cloudflare       │ ──────────────►  │ GitHub Issues   │
│  App         │   POST /report │ Worker           │   (with token)   │ Romcloud_Logs   │
└──────────────┘                └─────────────────┘                   └─────────────────┘
                                        │
                                        ▼
                               ┌─────────────────┐
                               │   KV Store      │
                               │ (Dedupe/Rate)   │
                               └─────────────────┘
```

### Files Created
```
deploy/issue-relay/
├── wrangler.toml          # Config with KV namespace ID
├── package.json           # Dependencies
├── tsconfig.json         # TypeScript config
├── README.md             # Setup documentation
└── src/
    ├── index.ts          # Main worker handler
    ├── github.ts         # GitHub API client
    ├── filter.ts         # Sensitive data filter
    ├── dedupe.ts         # Deduplication (24h TTL)
    └── rate-limit.ts     # Rate limiting (10 req/hour/IP)
```

### API Endpoints

#### POST /report
```json
// Request
{
  "deviceId": "RC-a3f2b8c1",
  "version": "2.2.0",
  "errorType": "NetworkError",
  "errorMessage": "Failed to connect",
  "stackTrace": "...",
  "context": "...",
  "labels": ["bug"],
  "timestamp": "2026-10-01T08:00:00Z"
}

// Response
{
  "success": true,
  "message": "Report submitted successfully",
  "issueNumber": 42
}
```

#### GET /health
```json
{
  "status": "healthy",
  "version": "1.0.0",
  "app": "RomCloud",
  "timestamp": "2026-10-01T08:00:00Z"
}
```

### Security Features
- GitHub token stored ONLY in Worker secrets (never in code)
- Client-side filtering: App filters before sending
- Server-side filtering: Worker filters remaining sensitive data
- Deduplication: Same error from same device ignored for 24h
- Rate limiting: 10 requests/hour/IP
- Payload size limit: 100KB

### Secrets Configured
- `GITHUB_TOKEN` - Fine-grained PAT (only Issues:ReadWrite for Romcloud_Logs)
- `GITHUB_REPO_OWNER` - "bun2it"
- `GITHUB_REPO_NAME` - "Romcloud_Logs"

### KV Namespace
- ID: `6c0aab5de6f847aab7b8d7d3666f4347`
- Used for: deduplication, rate limiting

---

## Phase 3: Client Integration ✅ (COMPLETED)

### Files Modified
| File | Changes |
|------|---------|
| `src/logging/IssueLogger.cpp` | Complete rewrite to use Worker relay |
| `src/logging/IssueLogger.h` | Simplified API, added `setEnabled()`, `filterSensitiveData()` |
| `src/diagnostics/DeviceIdentity.cpp` | New - Hardware ID detection + hashing |
| `src/diagnostics/DeviceIdentity.h` | New - DeviceIdentity class |
| `src/ui/UiStrings.h` | Added UI strings for device ID and reporting |
| `src/ui/UIManager.cpp` | Added toggle in Settings, include DeviceIdentity |
| `src/platform/PlatformInfo.h` | Added `totalSpace`, `freeSpace` fields |
| `src/platform/PlatformInfo.cpp` | Populate new fields |
| `build.sh` | Added DeviceIdentity.cpp |
| `build_pc.sh` | Added DeviceIdentity.cpp |

### What Was Implemented

1. **IssueLogger** - Now sends reports via Worker relay
   - Client-side sensitive data filtering
   - Device ID in payload
   - Configurable enable/disable

2. **DeviceIdentity** - New module
   - Hardware ID priority: chip_id → MAC → machine-id → install_id
   - SHA-256 hashing with domain prefix
   - Output format: `RC-xxxxxxxx`

3. **Settings Toggle** - In Settings menu
   - Shows Device ID
   - Toggle button to enable/disable reporting
   - Persisted to database

4. **UI Strings** - Vietnamese labels
   - Device ID display labels
   - Reporting toggle labels

### Worker URL to Use
```
https://romcloud-issue-relay.bun2it.workers.dev
```

### GitHub Repo
- **Private:** https://github.com/bun2it/Romcloud_Logs
- **Token:** Stored in Cloudflare secrets (never in app)

---

## Testing Checklist

- [x] DeviceIdentity module compiles
- [x] Worker health endpoint responds
- [x] Worker creates GitHub issue
- [x] Deduplication prevents duplicate issues
- [x] IssueLogger sends via Worker relay
- [x] Client-side sensitive data filtering
- [x] Settings toggle in UI
- [x] Device ID displays in Settings
- [ ] Build on TrimUI device (needs device)
- [ ] End-to-end test on real device
- [ ] Verify no token/secret in release binary

---

## References

- Reference implementation: `E:\Trimiu Brick Pro\Project APPS\chiaki-ng\files\rh\device_identity.py`
- Reference Worker: `E:\Trimiu Brick Pro\Project APPS\chiaki-ng\deploy\issue-relay`
- Reference Music Player: `E:\Trimiu Brick Pro\Project APPS\Music-Player`

---

## Maintenance

### To Redeploy Worker
```bash
cd deploy/issue-relay
wrangler deploy
```

### To Update Secrets
```bash
wrangler secret put GITHUB_TOKEN --name romcloud-issue-relay
wrangler secret put GITHUB_REPO_OWNER --name romcloud-issue-relay
wrangler secret put GITHUB_REPO_NAME --name romcloud-issue-relay
```

### To View Worker Logs
```bash
wrangler tail romcloud-issue-relay
```

### To Check KV Data
```bash
wrangler kv namespace list  # List all KV namespaces
# Use Cloudflare dashboard for detailed KV inspection
```
