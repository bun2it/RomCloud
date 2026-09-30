#!/bin/sh
# tiktok_search.sh - Real TikTok stream extraction for RomCloud
# Usage:
#   ./tiktok_search.sh search "<query_or_tag>" [page] [per_page]
#   ./tiktok_search.sh trending [page] [per_page]
#   ./tiktok_search.sh url "<video_id_or_url>"
#
# Output format (search/trending):
#   <video_id>|<clean_title>|<uploader>|<play_url>
#   (pipe-separated, one line per result)

clean_title() {
    echo "$1" | sed \
        -e 's/#[a-zA-Z0-9_]* *#*[a-zA-Z0-9_]* *$//' \
        -e 's/#[a-zA-Z0-9_]*$//' \
        -e 's/#fyp[# ]*//g' \
        -e 's/#foryou[# ]*//g' \
        -e 's/#viral[# ]*//g' \
        -e 's/#duet[# ]*//g' \
        -e 's/#stitch[# ]*//g' \
        -e 's/#tiktok[# ]*//g' \
        -e 's/#trending[# ]*//g' \
        -e 's/#shorts[# ]*//g' \
        -e 's/  */ /g' \
        -e 's/ *$//'
}

fetch_challenge_posts() {
    TAG="$1"
    COUNT="${2:-20}"
    CLEAN_TAG=$(echo "$TAG" | tr -d '#' | tr ' ' '_')
    if [ -z "$CLEAN_TAG" ]; then
        CLEAN_TAG="haihuoc"
    fi

    # 1. Search challenge ID
    CID=$(curl -k -s -m 6 "https://www.tikwm.com/api/challenge/search?keywords=${CLEAN_TAG}" 2>/dev/null | grep -o '"id":"[0-9]*"' | head -n 1 | cut -d'"' -f4)
    if [ -z "$CID" ]; then
        CID="57577487" # Fallback haihuoc CID
    fi

    # 2. Fetch posts
    curl -k -s -m 10 "https://www.tikwm.com/api/challenge/posts?challenge_id=${CID}&count=${COUNT}&cursor=0" 2>/dev/null | \
    sed 's/},/}\n/g' | \
    while read -r line; do
        vid=$(echo "$line" | grep -o '"video_id":"[0-9]*"' | head -n 1 | cut -d'"' -f4)
        if [ -n "$vid" ]; then
            raw_title=$(echo "$line" | grep -o '"title":"[^"]*"' | head -n 1 | cut -d'"' -f4)
            title=$(clean_title "$raw_title")
            author=$(echo "$line" | grep -o '"nickname":"[^"]*"' | head -n 1 | cut -d'"' -f4)
            if [ -z "$author" ]; then
                author=$(echo "$line" | grep -o '"unique_id":"[^"]*"' | head -n 1 | cut -d'"' -f4)
            fi
            play=$(echo "$line" | grep -o '"play":"[^"]*"' | head -n 1 | cut -d'"' -f4 | sed 's/\\//g')
            if [ -n "$play" ] && [ "${play#http}" != "$play" ]; then
                echo "${vid}|${title}|${author}|${play}"
            fi
        fi
    done
}

case "$1" in
    search)
        QUERY="$2"
        COUNT="${4:-20}"
        if [ -z "$QUERY" ]; then
            fetch_challenge_posts "trend" "$COUNT"
        else
            fetch_challenge_posts "$QUERY" "$COUNT"
        fi
        exit 0
        ;;

    trending)
        COUNT="${3:-20}"
        fetch_challenge_posts "trend" "$COUNT"
        exit 0
        ;;

    url)
        URL="$2"
        if [ -z "$URL" ]; then
            echo "ERROR:NOSTREAM No URL provided" >&2
            exit 1
        fi

        # Already a direct stream URL — use as-is
        if [ "${URL#http}" != "$URL" ]; then
            echo "$URL"
            exit 0
        fi

        # Construct full TikTok URL
        TIK_URL="$URL"
        case "$URL" in
            [0-9]*) TIK_URL="https://www.tiktok.com/@user/video/${URL}" ;;
        esac

        # Fetch from TikWM API (returns CDN URLs directly)
        RESP=$(curl -k -s -m 8 "https://www.tikwm.com/api/?url=${TIK_URL}&hd=1" 2>/dev/null)

        # Priority: hd (720p) > wmplay > play (360p)
        HD_URL=$(echo "$RESP" | grep -o '"hd":"[^"]*"' | head -n 1 | cut -d'"' -f4 | sed 's/\\//g')
        WM_URL=$(echo "$RESP" | grep -o '"wmplay":"[^"]*"' | head -n 1 | cut -d'"' -f4 | sed 's/\\//g')
        SD_URL=$(echo "$RESP" | grep -o '"play":"[^"]*"' | head -n 1 | cut -d'"' -f4 | sed 's/\\//g')

        # Prefer 720p HD, fallback to wmplay, fallback to SD
        if [ -n "$HD_URL" ] && [ "${HD_URL#http}" != "$HD_URL" ]; then
            echo "$HD_URL"
        elif [ -n "$WM_URL" ] && [ "${WM_URL#http}" != "$WM_URL" ]; then
            echo "$WM_URL"
        elif [ -n "$SD_URL" ] && [ "${SD_URL#http}" != "$SD_URL" ]; then
            echo "$SD_URL"
        else
            echo "ERROR:NOSTREAM Failed to extract stream URL" >&2
            exit 1
        fi
        ;;

    *)
        echo "Usage: tiktok_search.sh search|trending|url ..." >&2
        exit 1
        ;;
esac
