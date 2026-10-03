#!/bin/sh
# youtube_search.sh - Optimized YouTube search for RomCloud
# Strategy:
#  - Pagination: 6 results per page via --playlist-start offset
#  - Lean flags: skip-download, no-playlist, no-warnings
#  - Falls back to bundled yt-dlp-glibc on Linux
# Usage:
#   ./youtube_search.sh search "<query>" [page=1] [per_page=6]
#   ./youtube_search.sh url "<video_id>"

BINDIR="$(cd "$(dirname "$0")" && pwd)/.."

if [ -x "${BINDIR}/bin/yt-dlp-glibc" ] && [ "$(uname -s)" = "Linux" ]; then
    YTDLP="${BINDIR}/bin/yt-dlp-glibc"
elif [ -x "${BINDIR}/bin/yt-dlp" ]; then
    YTDLP="${BINDIR}/bin/yt-dlp"
elif command -v yt-dlp >/dev/null 2>&1; then
    YTDLP="$(command -v yt-dlp)"
else
    echo "ERROR:NOMODULE Khong tim thay yt-dlp trong bin/" >&2
    exit 1
fi

trap 'rm -rf /tmp/_MEI* 2>/dev/null' EXIT INT TERM
# NOTE: /tmp/yt_cache (yt-dlp cache) duoc GIU LAI giua cac lan goi de
# tai su dung player JS/web cache -> video sau resolve nhanh hon.
# Chi xoa _MEI* (PyInstaller unpack) de giai phong tmpfs RAM.

PER_PAGE=6

case "$1" in
    search)
        QUERY="$2"
        PAGE="${3:-1}"
        PER_PAGE="${4:-$PER_PAGE}"
        if [ -z "$QUERY" ]; then
            echo "ERROR:EMPTY query is empty" >&2
            exit 1
        fi

        # Prefer lightweight python innertube search to avoid yt-dlp OOM
        if [ -x "/mnt/SDCARD/System/bin/python3" ] && [ -f "${BINDIR}/scripts/youtube_search.py" ]; then
            LD_LIBRARY_PATH=/mnt/SDCARD/System/lib:$LD_LIBRARY_PATH \
            SSL_CERT_FILE=/mnt/SDCARD/System/lib/python3.11/site-packages/pip/_vendor/certifi/cacert.pem \
            /mnt/SDCARD/System/bin/python3 "${BINDIR}/scripts/youtube_search.py" smart "$QUERY" "$PER_PAGE"
            exit 0
        fi

        TOTAL=$(( PAGE * PER_PAGE ))
        START=$(( (PAGE - 1) * PER_PAGE + 1 ))
        END=$(( PAGE * PER_PAGE ))

        # Clean stale PyInstaller temp folders to prevent decompression errors
        rm -rf /tmp/_MEI* 2>/dev/null

        # Lean query: minimal flags, only fetch metadata
        "$YTDLP" \
            --flat-playlist \
            --no-warnings \
            --skip-download \
            --socket-timeout 8 \
            --playlist-start "$START" \
            --playlist-end "$END" \
            --print '%(id)s|%(title)s|%(duration)s|%(uploader)s|%(view_count)s' \
            "ytsearch${END}:${QUERY}" 2>/dev/null
        exit 0
        ;;
    url)
        VIDEO_ID="$2"
        QUALITY="${3:-720}"
        if [ -z "$VIDEO_ID" ]; then
            echo "ERROR:NOSTREAM no video id" >&2
            exit 1
        fi

        # Clean stale PyInstaller temp folders to reclaim tmpfs RAM
        rm -rf /tmp/_MEI* 2>/dev/null

        # 720p: client mac dinh cho DASH (bv+ba) -> script in "VIDEO|AUDIO",
        # C++ noi bang '|' va phat audio qua --audio-file. Cham hon android
        # ~2s nhung co 720p that (android chi co toi 360p progressive).
        # Uu tien AVC 30fps (itag 135): nhe CPU nhat cho A53. AV1/VP9
        # 720p60 giai ma mem gay giat tren may yeu.
        FORMAT="bv*[height<=720][fps<=30][vcodec^=avc]+ba/bv*[height<=720][vcodec^=avc]+ba/bv*[height<=720][fps<=30]+ba/bv*[height<=720]+ba/best[height<=720]/best"
        case "$QUALITY" in
            360) FORMAT="18/best[height<=360]" ;;
            *)   ;;
        esac
        if [ "$QUALITY" = "360" ]; then
            EXTRACTOR_ARGS="youtube:player_client=android"
        else
            EXTRACTOR_ARGS=""
        fi

        # Fast direct stream URL via Android client (bypasses JS decipherer, ~2s)
        T0=$(date +%s)
        if [ -n "$EXTRACTOR_ARGS" ]; then
            RAW_URLS=$("$YTDLP" -g \
                --cache-dir /tmp/yt_cache \
                --no-warnings \
                --no-playlist \
                --no-check-certificates \
                --extractor-args "$EXTRACTOR_ARGS" \
                -f "$FORMAT" \
                --socket-timeout 8 \
                "https://www.youtube.com/watch?v=${VIDEO_ID}" 2>/dev/null)
        else
            RAW_URLS=$("$YTDLP" -g \
                --cache-dir /tmp/yt_cache \
                --no-warnings \
                --no-playlist \
                --no-check-certificates \
                -f "$FORMAT" \
                --socket-timeout 10 \
                "https://www.youtube.com/watch?v=${VIDEO_ID}" 2>/dev/null)
        fi
        echo "yt_url main_branch $(( $(date +%s) - T0 ))s ${VIDEO_ID} q=${QUALITY}" >> /tmp/yt_timing.log 2>/dev/null

        V_URL=$(echo "$RAW_URLS" | sed -n '1p')
        A_URL=$(echo "$RAW_URLS" | sed -n '2p')

        if [ -n "$V_URL" ] && [ "${V_URL#http}" != "$V_URL" ]; then
            if [ -n "$A_URL" ] && [ "${A_URL#http}" != "$A_URL" ]; then
                echo "${V_URL}|${A_URL}"
            else
                echo "${V_URL}"
            fi
            exit 0
        fi

        # Fallback: android progressive (luon co, toi da 360p) de dam bao
        # video phat duoc thay vi bao loi khi client mac dinh that bai.
        T0=$(date +%s)
        RAW_URLS=$("$YTDLP" -g --cache-dir /tmp/yt_cache --socket-timeout 8 --no-warnings --no-playlist --no-check-certificates --extractor-args "youtube:player_client=android" -f "18/best[height<=360]/best" "https://www.youtube.com/watch?v=${VIDEO_ID}" 2>/dev/null)
        echo "yt_url fallback_android360 $(( $(date +%s) - T0 ))s ${VIDEO_ID}" >> /tmp/yt_timing.log 2>/dev/null
        V_URL=$(echo "$RAW_URLS" | sed -n '1p')
        A_URL=$(echo "$RAW_URLS" | sed -n '2p')

        if [ -n "$V_URL" ] && [ "${V_URL#http}" != "$V_URL" ]; then
            if [ -n "$A_URL" ] && [ "${A_URL#http}" != "$A_URL" ]; then
                echo "${V_URL}|${A_URL}"
            else
                echo "${V_URL}"
            fi
            exit 0
        fi

        echo "ERROR:NOSTREAM Failed to extract stream URL" >&2
        exit 1
        ;;
    *)
        echo "Usage: youtube_search.sh search <query> [page] [per_page]" >&2
        echo "       youtube_search.sh url <video_id> [360|720|auto]" >&2
        exit 1
        ;;
esac
