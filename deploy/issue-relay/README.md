# RomCloud Issue Relay - Cloudflare Worker

Relays filtered bug reports from RomCloud app to GitHub Issues (private repo).

## Architecture

```
┌──────────────┐     HTTPS      ┌─────────────────┐     GitHub API    ┌─────────────────┐
│  RomCloud    │ ────────────►  │ Cloudflare       │ ──────────────►  │ GitHub Issues   │
│  App         │   POST /report │ Worker (Relay)   │   (with token)   │ Romcloud_Logs   │
└──────────────┘                └─────────────────┘                   └─────────────────┘
                                        │
                                        ▼
                               ┌─────────────────┐
                               │   KV Store      │
                               │ (Dedupe/Rate)   │
                               └─────────────────┘
```

## Security

- **GitHub token**: Stored ONLY in Cloudflare Worker secrets, never in code
- **Client-side filtering**: App filters sensitive data before sending
- **Server-side filtering**: Worker filters any remaining sensitive data
- **Deduplication**: Same error from same device ignored for 24h
- **Rate limiting**: 10 requests/hour/IP
- **No repo info exposed**: Repo name only in Worker env, not in client

## Setup

### 1. Prerequisites

- Node.js 18+
- Wrangler CLI: `npm install -g wrangler`
- Cloudflare account

### 2. Install dependencies

```bash
cd deploy/issue-relay
npm install
```

### 3. Login to Cloudflare

```bash
wrangler login
```

### 4. Create KV Namespace

```bash
wrangler kv:namespace create "issues_kv"
# Copy the ID, update wrangler.toml
```

### 5. Set Secrets

```bash
# GitHub fine-grained PAT (only Issues:ReadWrite for Romcloud_Logs)
wrangler secret put GITHUB_TOKEN

# Repo owner (bun2it)
wrangler secret put GITHUB_REPO_OWNER

# Repo name (Romcloud_Logs)
wrangler secret put GITHUB_REPO_NAME
```

### 6. Update wrangler.toml

Add KV namespace ID from step 4.

### 7. Deploy

```bash
wrangler deploy
```

## API Endpoints

### POST /report

Submit a bug report.

**Request:**
```json
{
  "deviceId": "RC-a3f2b8c1",
  "version": "2.2.0",
  "errorType": "NetworkError",
  "errorMessage": "Failed to connect to server",
  "stackTrace": "...",
  "context": "...",
  "labels": ["bug"],
  "timestamp": "2024-01-01T12:00:00Z"
}
```

**Response:**
```json
{
  "success": true,
  "message": "Report submitted successfully",
  "issueNumber": 42
}
```

### GET /health

Health check endpoint.

**Response:**
```json
{
  "status": "healthy",
  "version": "1.0.0",
  "app": "RomCloud",
  "timestamp": "2024-01-01T12:00:00Z"
}
```

## GitHub Token Requirements

Create a fine-grained PAT with:
- **Repository access**: Only `bun2it/Romcloud_Logs`
- **Permissions**:
  - Issues: Read and write

Generate at: https://github.com/settings/tokens?type=beta

## Troubleshooting

### Check Worker logs
```bash
wrangler tail
```

### Test locally
```bash
wrangler dev
```

### Check KV data
```bash
wrangler kv:key list --binding=ISSUES_KV
```

## Files

```
deploy/issue-relay/
├── wrangler.toml      # Cloudflare config
├── package.json       # Dependencies
├── tsconfig.json      # TypeScript config
├── src/
│   ├── index.ts       # Main worker
│   ├── github.ts     # GitHub API client
│   ├── filter.ts     # Sensitive data filter
│   ├── dedupe.ts     # Deduplication logic
│   └── rate-limit.ts # Rate limiting
└── README.md         # This file
```
