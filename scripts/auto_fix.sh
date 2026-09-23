#!/bin/bash
# =============================================================================
# RomCloud AI Agent - Auto Fix Script
# =============================================================================
# Script này AI agent chạy khi phát hiện issues mới
# Nó sẽ:
# 1. Clone repo (nếu chưa có)
# 2. Đọc issue chi tiết
# 3. Phân tích lỗi
# 4. Fix code
# 5. Build
# 6. Tạo release mới
# 7. Push lên GitHub
# =============================================================================

set -e

REPO_URL="https://github.com/bun2it/RomCloud.git"
REPO_DIR="/tmp/romcloud-agent"
ISSUE_NUM="${1:-}"

if [ -z "$ISSUE_NUM" ]; then
    echo "Usage: $0 <issue_number>"
    echo "Example: $0 42"
    exit 1
fi

echo "============================================"
echo "RomCloud AI Agent - Auto Fix"
echo "============================================"
echo "Issue: #$ISSUE_NUM"
echo "Date: $(date)"
echo ""

# 1. Clone hoặc pull repo
echo "📦 Checking repository..."
if [ -d "$REPO_DIR/.git" ]; then
    echo "Repo exists, pulling latest..."
    cd "$REPO_DIR"
    git pull origin main
else
    echo "Cloning repo..."
    git clone "$REPO_URL" "$REPO_DIR"
    cd "$REPO_DIR"
fi

# 2. Đọc issue chi tiết
echo ""
echo "📖 Reading issue #$ISSUE_NUM..."
gh issue view $ISSUE_NUM --repo bun2it/RomCloud > /tmp/issue_detail.txt 2>/dev/null || {
    echo "❌ Cannot read issue #$ISSUE_NUM"
    exit 1
}

cat /tmp/issue_detail.txt

# 3. Phân tích issue
echo ""
echo "🔍 Analyzing issue..."

# Extract error message
ERROR_MSG=$(grep -A5 "Mô tả lỗi\|Lỗi\|Error" /tmp/issue_detail.txt | head -10 || echo "")

# Extract version
VERSION=$(grep -oP 'v?\K[0-9]+\.[0-9]+\.[0-9]+' /tmp/issue_detail.txt | head -1 || echo "unknown")

echo "Detected version: $VERSION"
echo "Error message: $ERROR_MSG"

# 4. TODO: AI sẽ phân tích và fix ở đây
echo ""
echo "🤖 AI Analysis:"
echo "   Based on the issue, identify the relevant source file(s)"
echo "   Common patterns:"
echo "   - OTA install fails → src/ota/UpdateManager.cpp"
echo "   - UI display issues → src/ui/UIManager.cpp"
echo "   - Network errors → src/network/HttpClient.cpp"
echo "   - Database errors → src/database/DatabaseManager.cpp"
echo ""
echo "   After fixing:"
echo "   1. Edit the source file(s)"
echo "   2. Run: ./build.sh"
echo "   3. Update version"
echo "   4. Create release"
echo "   5. Upload binary"
echo "   6. Close issue"

echo ""
echo "============================================"
echo "AI Agent ready to fix Issue #$ISSUE_NUM"
echo "============================================"
