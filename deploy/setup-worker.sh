#!/bin/bash
# setup-worker.sh - Setup và deploy Cloudflare Worker
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/issue-relay"

echo "=== RomCloud Issue Relay - Worker Setup ==="

# 1. Check wrangler
if ! command -v wrangler &>/dev/null; then
    echo "ERROR: wrangler not found. Install with: npm install -g wrangler"
    exit 1
fi

# 2. Check login
echo "Checking Cloudflare login..."
wrangler whoami &>/dev/null || {
    echo "ERROR: Not logged in. Run: wrangler login"
    exit 1
}

# 3. Install dependencies
echo "Installing dependencies..."
npm install

# 4. Create KV namespace
echo "Creating KV namespace..."
KV_OUTPUT=$(wrangler kv:namespace create "issues_kv")
KV_ID=$(echo "$KV_OUTPUT" | grep -oP '(?<=id = ")[^"]+' || echo "$KV_OUTPUT" | grep -oP '(?<=id: ")[^"]+')

if [ -z "$KV_ID" ]; then
    echo "Failed to get KV ID from output"
    echo "Output was: $KV_OUTPUT"
    exit 1
fi

echo "KV Namespace ID: $KV_ID"

# 5. Update wrangler.toml with KV ID
echo "Updating wrangler.toml..."
sed -i.bak "s/id = \"\"/id = \"$KV_ID\"/" wrangler.toml
echo "Updated wrangler.toml"

# 6. Set secrets (token is passed via stdin to avoid history)
echo ""
echo "=== Setting Secrets ==="
echo "Note: Enter values when prompted"

echo -n "GitHub Token (ghp_...): "
read -s GITHUB_TOKEN
echo
echo "$GITHUB_TOKEN" | wrangler secret put GITHUB_TOKEN

echo -n "GitHub Repo Owner (bun2it): "
read -s REPO_OWNER
echo
echo "$REPO_OWNER" | wrangler secret put GITHUB_REPO_OWNER

echo -n "GitHub Repo Name (Romcloud_Logs): "
read -s REPO_NAME
echo
echo "$REPO_NAME" | wrangler secret put GITHUB_REPO_NAME

# 7. Deploy
echo ""
echo "=== Deploying Worker ==="
wrangler deploy

echo ""
echo "=== Setup Complete ==="
echo "Worker URL will be shown above"
echo "Update config.json with the worker URL"
