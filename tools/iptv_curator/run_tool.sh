#!/bin/bash
# ==============================================================================
# RomCloud IPTV Curator - Khởi chạy Web UI Localhost 1 chạm
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT=8080

echo "=== Đang khởi động RomCloud IPTV Curator Localhost ==="

# Kiểm tra Python 3
if ! command -v python3 &>/dev/null; then
    echo "LỖI: Chưa cài đặt python3 trên máy."
    exit 1
fi

# Chạy server ở chế độ nền hoặc trực tiếp
cd "$SCRIPT_DIR"

# Mở trình duyệt web sau 1 giây
(sleep 1 && (open "http://localhost:$PORT" 2>/dev/null || xdg-open "http://localhost:$PORT" 2>/dev/null || true)) &

# Chạy server
exec python3 server.py "$PORT"
