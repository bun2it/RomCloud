#!/bin/bash
# =============================================================================
# RomCloud AI Agent - Daily Issue Check Script
# =============================================================================
# Chạy script này hàng ngày để:
# 1. Kiểm tra issues mới trên GitHub
# 2. Phân tích lỗi
# 3. Tự động fix nếu có thể
# 4. Đẩy phiên bản mới
# =============================================================================

set -e

REPO="bun2it/RomCloud"
cd "$(dirname "$0")"

echo "============================================"
echo "RomCloud AI Agent - Daily Issue Check"
echo "============================================"
echo "Date: $(date)"
echo ""

# 1. Lấy danh sách issues chưa đóng
echo "📋 Checking open issues..."
ISSUES=$(gh issue list --repo $REPO --state open --limit 10 --json number,title --jq '.[] | "\(.number): \(.title)"')

if [ -z "$ISSUES" ]; then
    echo "✅ No open issues found!"
    exit 0
fi

echo "$ISSUES"
echo ""

# 2. Xử lý từng issue
echo "$ISSUES" | while IFS= read -r line; do
    ISSUE_NUM=$(echo "$line" | cut -d: -f1)
    ISSUE_TITLE=$(echo "$line" | cut -d: -f2- | xargs)

    echo "--------------------------------------------"
    echo "🔍 Processing Issue #$ISSUE_NUM: $ISSUE_TITLE"
    echo ""

    # Xem chi tiết issue
    gh issue view $ISSUE_NUM --repo $REPO --json body,labels --jq '.body, .labels[].name' > /tmp/issue_${ISSUE_NUM}.txt 2>/dev/null || true

    # Kiểm tra xem issue có phải là bug crash không
    if echo "$ISSUE_TITLE" | grep -qiE "crash|lỗi|bug|error"; then
        echo "⚠️  This is a bug/crash issue - requires attention"
        echo ""
        echo "📝 To fix this issue:"
        echo "   1. Read: gh issue view $ISSUE_NUM --repo $REPO"
        echo "   2. Analyze the error and find the relevant source file"
        echo "   3. Fix the bug in src/"
        echo "   4. Build: ./build.sh"
        echo "   5. Update version in src/ota/UpdateManager.h"
        echo "   6. Commit: git add -A && git commit -m 'fix: <fix description> #$ISSUE_NUM'"
        echo "   7. Push: git push origin main"
        echo "   8. Create release and upload binary"
        echo "   9. Close: gh issue close $ISSUE_NUM --comment 'Fixed in vX.Y.Z'"
    else
        echo "📌 This is a feature request or question"
        echo "   Consider adding label: 'enhancement' or 'question'"
    fi

    echo ""
done

echo "============================================"
echo "Daily check complete!"
echo "Next run: Tomorrow"
echo "============================================"
