#!/usr/bin/env python3
"""
youtube_search.py - YouTube search & stream URL fetcher for RomCloud

Usage:
    python3 youtube_search.py search "<query>" [max_results]
    python3 youtube_search.py smart  "<query>" [max_results]  (P0-3+: channel-aware)
    python3 youtube_search.py url "<video_id>"

Output (search / smart):
    One result per line: video_id|title|channel|duration|views
    For 'smart', first line may be: CHANNEL|<name>|<id>|<subs>|<vcount>
    UTF-8 encoded. Empty line = no results.

Output (url):
    Direct stream URL for mpv (720p max preferred).
    Fallback chain: best[height<=720] → bestvideo+bestaudio → worst

P0-4: YouTube Data API v3 used as fast path (~600ms vs ~6-10s yt-dlp).
Key loaded from config/youtube_api.key. Falls back to yt-dlp if missing/empty/fail.
Stream URL (`url`) always uses yt-dlp - API v3 cannot return direct stream URLs.

Exit codes:
    0 = success
    1 = error (network, yt-dlp not found, etc.)
"""

import subprocess
import sys
import os
import platform

# =====================================================================
# P0-5 device runtime env (TrimUI Brick):
#   - python3 not in $PATH (lives in /mnt/SDCARD/System/bin)
#   - system libssl.so.1.1 is OpenSSL 1.1.0i (too old for Python 3.11 _ssl)
#     but /mnt/SDCARD/System/lib has OpenSSL 1.1.1w (works)
#   - system /etc/ssl may lack CA bundle, but pip ships certifi
#
# IMPORTANT: setting os.environ["LD_LIBRARY_PATH"] in Python does NOT
# affect dlopen() for subsequent .so loads (verified on Brick). The
# loader has already cached paths. Solution: pre-load libssl via ctypes
# BEFORE any other module that triggers _ssl import.
# =====================================================================
_SYSTEM_BIN = "/mnt/SDCARD/System/bin"
_SYSTEM_LIB = "/mnt/SDCARD/System/lib"
# CA bundle priority: pip's certifi (Python-canonical) > system /etc/ssl > none
_CA_BUNDLE_CANDIDATES = [
    "/mnt/SDCARD/System/lib/python3.11/site-packages/pip/_vendor/certifi/cacert.pem",
    "/etc/ssl/certs/ca-certificates.crt",
]
if os.path.isdir(_SYSTEM_BIN):
    os.environ["PATH"] = _SYSTEM_BIN + os.pathsep + os.environ.get("PATH", "")
if "SSL_CERT_FILE" not in os.environ:
    for cand in _CA_BUNDLE_CANDIDATES:
        if os.path.isfile(cand):
            os.environ["SSL_CERT_FILE"] = cand
            break

# Pre-load the correct libssl via ctypes on TrimUI Brick. This binds
# libssl.so.1.1 (and libcrypto.so.1.1) to the bundled OpenSSL 1.1.1w,
# so when Python's `_ssl` is later imported, dlopen finds the right
# version. ctypes.CDLL is the only reliable way to redirect the loader
# from inside a running Python process.
#
# IMPORTANT: libssl depends on libcrypto. Loading libssl first triggers
# the loader to resolve libcrypto via the standard search order — which
# finds /usr/lib64 (OpenSSL 1.1.0i, wrong version). Loading libcrypto
# FIRST (alphabetical: c < s) makes both land in the correct slot.
if os.path.isdir(_SYSTEM_LIB):
    try:
        import ctypes
        for _lib in ("libcrypto.so.1.1", "libssl.so.1.1"):
            _path = os.path.join(_SYSTEM_LIB, _lib)
            if os.path.isfile(_path):
                try:
                    ctypes.CDLL(_path, mode=ctypes.RTLD_GLOBAL)
                except OSError:
                    pass  # already loaded or version mismatch — fall through to yt-dlp
    except ImportError:
        pass

# Now safe to import SSL-using modules
import json as _json
import unicodedata
import urllib.request
import urllib.parse
import urllib.error
import re as _re
import ssl
import shutil

# Clean up any leftover PyInstaller /tmp/_MEI* directories to reclaim tmpfs RAM
try:
    for _entry in os.listdir("/tmp"):
        if _entry.startswith("_MEI"):
            _p = os.path.join("/tmp", _entry)
            if os.path.isdir(_p):
                shutil.rmtree(_p, ignore_errors=True)
except Exception:
    pass

# Resolve paths relative to this script
SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
APP_ROOT = os.path.dirname(SCRIPT_DIR)

# Bundled yt-dlp on device (ARM glibc binary, self-contained)
DEVICE_YTDLP = os.path.join(APP_ROOT, "bin", "yt-dlp")
DEVICE_YTDLP_GLIBC = os.path.join(APP_ROOT, "bin", "yt-dlp-glibc")


def can_run_device_binary():
    """True if we're running on the TrimUI device (Linux aarch64/ARM)."""
    if os.path.isdir("/mnt/SDCARD"):
        return True
    return platform.system() == "Linux" and platform.machine() in ("aarch64", "arm64", "armv7l", "armv8l")


def get_yt_dlp_cmd():
    """Return command list to invoke yt-dlp.
    Device: use bundled ARM glibc binary (bin/yt-dlp-glibc).
    Dev machine: use python3 -m yt_dlp."""
    if can_run_device_binary():
        # Device: try bundled binary first
        if os.path.isfile(DEVICE_YTDLP) and os.access(DEVICE_YTDLP, os.X_OK):
            return [DEVICE_YTDLP]
        if os.path.isfile(DEVICE_YTDLP_GLIBC) and os.access(DEVICE_YTDLP_GLIBC, os.X_OK):
            return [DEVICE_YTDLP_GLIBC]
        return None

    # Dev machine: try python3 -m yt_dlp
    for cmd in ["python3", "python"]:
        result = subprocess.run(
            [cmd, "-m", "yt_dlp", "--version"],
            capture_output=True, timeout=10
        )
        if result.returncode == 0:
            return [cmd, "-m", "yt_dlp"]

    # Dev machine fallback: find yt-dlp in PATH
    for p in os.environ.get("PATH", "").split(os.pathsep):
        full = os.path.join(p, "yt-dlp")
        if os.path.isfile(full) and os.access(full, os.X_OK):
            return [full]
    return None


def run_yt_dlp(args, timeout=30):
    """Run yt-dlp and return stdout on success, None on error."""
    cmd = get_yt_dlp_cmd()
    if not cmd:
        print("ERROR:NOMODULE yt-dlp not found", file=sys.stderr)
        return None

    try:
        proc = subprocess.run(
            cmd + args,
            capture_output=True,
            timeout=timeout,
            env={**os.environ, "YTDLPTYPE": "ROMCLOUD"}
        )
        if proc.returncode == 0:
            return proc.stdout.decode("utf-8", errors="replace")

        stderr = proc.stderr.decode("utf-8", errors="replace")
        stderr_lower = stderr.lower()
        if "429" in stderr or "rate" in stderr_lower:
            print("ERROR:RATELIMIT", file=sys.stderr)
        elif "not found" in stderr_lower or "unavailable" in stderr_lower:
            print("ERROR:NOTFOUND video unavailable", file=sys.stderr)
        elif "timed out" in stderr_lower:
            print("ERROR:TIMEOUT", file=sys.stderr)
        else:
            print(f"ERROR:YTDLPDB {proc.returncode}: {stderr[:300]}", file=sys.stderr)
        return None

    except subprocess.TimeoutExpired:
        print("ERROR:TIMEOUT", file=sys.stderr)
        return None
    except Exception as e:
        print(f"ERROR:EXCEPTION {e}", file=sys.stderr)
        return None


def fmt_duration(seconds):
    if seconds is None:
        return "--:--"
    try:
        s = int(float(seconds))
    except (ValueError, TypeError):
        return "--:--"
    if s >= 3600:
        return f"{s//3600}:{(s%3600)//60:02d}:{s%60:02d}"
    return f"{s//60}:{s%60:02d}"


def fmt_views_raw(count):
    """Format view count as raw integer string (no K/M suffix).

    Caller (C++) does the formatting with "lượt xem" suffix.
    Returning the raw int keeps the wire payload minimal and avoids
    double-format bugs (e.g. C++ stoll("1M") fails, returns "1M" raw).
    Returns empty string if count is missing/invalid (→ C++ hides the field).
    """
    if count is None:
        return ""
    try:
        c = int(float(count))
    except (ValueError, TypeError):
        return ""
    return "" if c <= 0 else str(c)


def _safe(s):
    """Escape pipe character for pipe-separated output."""
    return str(s if s is not None else "").replace("|", "&#124;")


# =============================================================================
# P0-3: Smart search với channel detection
# =============================================================================

def _strip_diacritics(s):
    """Remove Vietnamese diacritics cho fuzzy matching.
    'Sơn Tùng' → 'Son Tung'. Dùng NFKD decomposition.
    """
    if not s:
        return ""
    try:
        nfkd = unicodedata.normalize("NFKD", s)
        return "".join(c for c in nfkd if not unicodedata.combining(c))
    except Exception:
        return s


def _fmt_subs_vn(count):
    """Format subscriber int -> display string kieu Innertube VN.
    Vi du: 1250000 -> '1,25 Tr người đăng ký'. Tra '' neu khong co."""
    try:
        n = int(float(count))
    except (ValueError, TypeError):
        return ""
    if n <= 0:
        return ""
    if n >= 1000000:
        t = f"{n / 1000000:.2f}".rstrip("0").rstrip(".").replace(".", ",")
        return f"{t} Tr người đăng ký"
    if n >= 1000:
        t = f"{n / 1000:.1f}".rstrip("0").rstrip(".").replace(".", ",")
        return f"{t} N người đăng ký"
    return f"{n} người đăng ký"


def _fmt_vcount_vn(count):
    """Format video count int -> '1.234 video'. Tra '' neu khong co."""
    try:
        n = int(float(count))
    except (ValueError, TypeError):
        return ""
    if n <= 0:
        return ""
    return f"{n:,}".replace(",", ".") + " video"


def _lookup_channel(query):
    """Dùng `search_filter=channel` để tìm channel đầu tiên match query.

    `ytsearchchannel:` syntax đã bị deprecated/broken trong nhiều phiên bản
    yt-dlp mới. Thay vào đó dùng `--extractor-args "youtube:search_filter=channel"`
    + `ytsearch5:<query>` để lấy video của channel match → extract channel info.

    Returns dict {id, name, subscribers, video_count} hoặc None.
    """
    output = run_yt_dlp([
        "--flat-playlist",
        "--playlist-end", "3",
        "--dump-json",
        "--extractor-args", "youtube:search_filter=channel",
        f"ytsearch3:{query}"
    ], timeout=15)
    if not output:
        return None

    # Parse video đầu tiên để lấy channel info
    for line in output.strip().splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            data = _json.loads(line)
            ch_id = (data.get("channel_id") or data.get("uploader_id") or
                     "")
            ch_name = (data.get("channel") or data.get("uploader") or "")
            # Channel id hợp lệ phải bắt đầu bằng UC
            if not ch_id or not ch_name or not ch_id.startswith("UC"):
                continue
            # Lấy thêm thông tin nếu có (sub count từ channel page nếu extractor cung cấp)
            subs = (data.get("channel_follower_count") or
                    data.get("subscriber_count") or 0)
            vcount = data.get("playlist_count") or 0
            return {
                "id": ch_id,
                "name": ch_name,
                "subscribers": int(subs) if subs else 0,
                "video_count": int(vcount) if vcount else 0,
            }
        except (IndexError, _json.JSONDecodeError, ValueError, AttributeError):
            continue
    return None


def is_matching_channel_name(query, channel_name):
    """Chỉ coi là match Channel nếu query đúng là tên của channel đó.
    Tuyệt đối không dùng substring lỏng lẻo để tránh biến các từ khóa chủ đề
    (nhạc trẻ, remix, bóng đá, phim...) thành kênh riêng lẻ.
    """
    if not query or not channel_name:
        return False
    if query.startswith("channel:"):
        return True
    q = _strip_diacritics(query.lower().strip())
    name = _strip_diacritics(channel_name.lower().strip())
    if not q or not name:
        return False
    if q == name:
        return True
    for word in ["official", "channel", "tv", "media", "entertainment", "music", "vevo", "studio"]:
        q_strip = q.replace(word, "").strip()
        name_strip = name.replace(word, "").strip()
        if q_strip and q_strip == name_strip:
            return True
    return False


def _is_strong_channel_match(query, channel_info):
    """Check query có đúng là tên channel không."""
    if not channel_info:
        return False
    ch_name = channel_info.get("name", "") if isinstance(channel_info, dict) else str(channel_info)
    return is_matching_channel_name(query, ch_name)


def _fetch_channel_videos(channel_id, max_results=20, channel_name=""):
    """Fetch latest videos từ channel URL.

    Channel URL flat-playlist chỉ trả (id, title, duration) - không có
    uploader/channel/view_count. Phải truyền channel_name riêng để fill vào.

    Returns count of lines printed.
    """
    url = f"https://www.youtube.com/channel/{channel_id}/videos"
    output = run_yt_dlp([
        "--flat-playlist",
        "--playlist-end", str(max_results),
        "--print", "%(id)s|%(title)s|%(duration)s",
        url
    ], timeout=30)
    if not output:
        return 0
    count = 0
    ch_name = channel_name or "Unknown"
    for line in output.strip().splitlines():
        line = line.strip()
        if not line or "|" not in line:
            continue
        parts = line.split("|", 2)  # Chỉ 3 fields từ channel URL
        if len(parts) >= 3:
            vid = parts[0]
            title = parts[1]
            duration = fmt_duration(parts[2])
            views = "N/A"  # Channel URL flat-playlist không trả view_count
            print(f"{_safe(vid)}|{_safe(title)}|{_safe(ch_name)}|{duration}|{views}")
            count += 1
    return count


# =============================================================================
# P0-4: YouTube Data API v3 (fast path)
# =============================================================================

API_BASE = "https://www.googleapis.com/youtube/v3"
API_KEY_PATH = os.path.join(APP_ROOT, "config", "youtube_api.key")


def load_api_key():
    """Load YouTube Data API v3 key từ config/youtube_api.key.
    Returns None nếu file missing/empty → caller sẽ fallback yt-dlp.
    """
    if not os.path.isfile(API_KEY_PATH):
        return None
    try:
        with open(API_KEY_PATH, "r", encoding="utf-8") as f:
            key = f.read().strip()
            return key if key else None
    except OSError:
        return None


def _api_get(endpoint, params, timeout=10):
    """HTTP GET tới YouTube Data API v3. Returns parsed JSON hoặc None."""
    key = load_api_key()
    if not key:
        return None
    params = dict(params)
    params["key"] = key
    url = API_BASE + endpoint + "?" + urllib.parse.urlencode(params)
    try:
        ctx = ssl._create_unverified_context()
        req = urllib.request.Request(url, headers={"User-Agent": "RomCloud/1.0"})
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
            body = resp.read().decode("utf-8", errors="replace")
            return _json.loads(body)
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace") if e.fp else ""
        try:
            err = _json.loads(body)
            msg = err.get("error", {}).get("message", body[:100])
        except Exception:
            msg = body[:100]
        print(f"WARN:API {endpoint} HTTP {e.code}: {msg}", file=sys.stderr)
        return None
    except (urllib.error.URLError, _json.JSONDecodeError, OSError) as e:
        print(f"WARN:API {endpoint} failed: {e}", file=sys.stderr)
        return None


def _iso8601_to_seconds(iso):
    """Convert ISO 8601 duration (PT1H2M3S) sang seconds. Returns None nếu invalid."""
    if not iso or not iso.startswith("PT"):
        return None
    try:
        m = _re.match(r"PT(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)S)?", iso)
        if not m:
            return None
        h = int(m.group(1) or 0)
        mi = int(m.group(2) or 0)
        s = int(m.group(3) or 0)
        return h * 3600 + mi * 60 + s
    except Exception:
        return None


def _api_get_channel_info(channel_id):
    """channels.list part=snippet,statistics. Cost: 1 unit.
    Returns dict {id, name, subscribers, video_count} hoặc None.
    """
    if not channel_id:
        return None
    data = _api_get("/channels", {
        "part": "snippet,statistics",
        "id": channel_id,
    })
    if not data or "items" not in data or not data["items"]:
        return None
    item = data["items"][0]
    name = item.get("snippet", {}).get("title", "")
    stats = item.get("statistics", {})
    if not name:
        return None
    thumbs = item.get("snippet", {}).get("thumbnails", {})
    avatar = ""
    for q in ("high", "medium", "default"):
        u = thumbs.get(q, {}).get("url", "")
        if u:
            avatar = u
            break
    return {
        "id": item.get("id", channel_id),
        "name": name,
        "subscribers": int(stats.get("subscriberCount", 0) or 0),
        "video_count": int(stats.get("videoCount", 0) or 0),
        "avatar": avatar,
    }


def _api_lookup_channel(query):
    """search.list type=channel. Cost: 100 units (search) + 1 unit (channels.list).
    Returns top channel dict {id, name, subscribers, video_count} hoặc None.
    """
    data = _api_get("/search", {
        "part": "snippet",
        "type": "channel",
        "maxResults": "1",
        "q": query,
        "relevanceLanguage": "vi",
    })
    if not data or "items" not in data or not data["items"]:
        return None
    item = data["items"][0]
    if item.get("id", {}).get("kind") != "youtube#channel":
        return None
    channel_id = item["id"].get("channelId", "")
    if not channel_id:
        return None
    info = _api_get_channel_info(channel_id)
    if info:
        return info
    # Fallback minimal (no statistics)
    return {
        "id": channel_id,
        "name": item.get("snippet", {}).get("channelTitle", ""),
        "subscribers": 0,
        "video_count": 0,
    }


def _api_video_details(video_ids):
    """videos.list part=contentDetails,statistics. Cost: 1 unit (max 50 IDs).
    Returns dict {vid: {duration_iso, views}}.
    """
    if not video_ids:
        return {}
    data = _api_get("/videos", {
        "part": "contentDetails,statistics",
        "id": ",".join(video_ids[:50]),
    })
    if not data or "items" not in data:
        return {}
    out = {}
    for v in data["items"]:
        vid = v.get("id", "")
        if not vid:
            continue
        out[vid] = {
            "duration_iso": v.get("contentDetails", {}).get("duration", ""),
            "views": int(v.get("statistics", {}).get("viewCount", 0) or 0),
        }
    return out


def _api_search_videos(query, max_results=20):
    """search.list type=video. Cost: 100 units.
    Returns list of dicts {id, title, channel} (cần enrich với _api_video_details).
    """
    data = _api_get("/search", {
        "part": "snippet",
        "type": "video",
        "maxResults": str(min(max_results, 50)),
        "q": query,
        "order": "relevance",
        "relevanceLanguage": "vi",
    })
    if not data or "items" not in data:
        return []
    items = []
    for item in data["items"]:
        if item.get("id", {}).get("kind") != "youtube#video":
            continue
        vid = item["id"].get("videoId", "")
        snippet = item.get("snippet", {})
        if vid:
            items.append({
                "id": vid,
                "title": snippet.get("title", ""),
                "channel": snippet.get("channelTitle", ""),
            })
    return items


def _api_channel_videos(channel_id, max_results=20):
    """search.list channelId=... type=video order=date. Cost: 100 + 1 units.
    Returns list of dicts {id, title, channel, duration_iso, views}.
    """
    data = _api_get("/search", {
        "part": "snippet",
        "type": "video",
        "channelId": channel_id,
        "order": "date",
        "maxResults": str(min(max_results, 50)),
    })
    if not data or "items" not in data:
        return []
    items = []
    video_ids = []
    for item in data["items"]:
        if item.get("id", {}).get("kind") != "youtube#video":
            continue
        vid = item["id"].get("videoId", "")
        snippet = item.get("snippet", {})
        if vid:
            video_ids.append(vid)
            items.append({
                "id": vid,
                "title": snippet.get("title", ""),
                "channel": snippet.get("channelTitle", ""),
            })
    details = _api_video_details(video_ids)
    for it in items:
        extra = details.get(it["id"], {})
        it["duration_iso"] = extra.get("duration_iso", "")
        it["views"] = extra.get("views", 0)
    return items




def innertube_search(query, max_results=20):
    """Zero-quota, ultra-fast YouTube search via Innertube web endpoint.
    Memory footprint is minimal (~5MB Python RSS). Never runs yt-dlp binary.
    """
    if not query:
        return False
    url = "https://www.youtube.com/youtubei/v1/search"
    payload = {
        "context": {
            "client": {
                "clientName": "WEB",
                "clientVersion": "2.20231201.00.00",
                "hl": "vi",
                "gl": "VN"
            }
        },
        "query": query
    }
    data_bytes = _json.dumps(payload).encode("utf-8")
    try:
        ctx = ssl._create_unverified_context()
        req = urllib.request.Request(
            url,
            data=data_bytes,
            headers={
                "Content-Type": "application/json",
                "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
            }
        )
        with urllib.request.urlopen(req, timeout=8, context=ctx) as resp:
            data = _json.loads(resp.read().decode("utf-8"))
    except Exception as e:
        print(f"WARN:innertube search failed: {e}", file=sys.stderr)
        return False

    channel_printed = False
    count = 0
    try:
        sections = (data.get("contents", {})
                    .get("twoColumnSearchResultsRenderer", {})
                    .get("primaryContents", {})
                    .get("sectionListRenderer", {})
                    .get("contents", []))
        for section in sections:
            items = section.get("itemSectionRenderer", {}).get("contents", [])
            for item in items:
                if not channel_printed:
                    if "channelRenderer" in item:
                        cr = item["channelRenderer"]
                        name = cr.get("title", {}).get("simpleText", "") or "".join(r.get("text", "") for r in cr.get("title", {}).get("runs", []))
                        if is_matching_channel_name(query, name):
                            subs = cr.get("videoCountText", {}).get("simpleText", "")
                            vcount = ""
                            avatar_url = ""
                            thumbs = cr.get("thumbnail", {}).get("thumbnails", [])
                            if thumbs:
                                avatar_url = thumbs[-1].get("url", "")
                                if avatar_url.startswith("//"):
                                    avatar_url = "https:" + avatar_url
                            print(f"CHANNEL|{_safe(name)}|{_safe(subs)}|{_safe(vcount)}|{_safe(avatar_url)}", flush=True)
                            channel_printed = True
                    elif "officialCardViewModel" in item:
                        card = item["officialCardViewModel"]
                        header = card.get("header", {}).get("pageHeaderViewModel", {})
                        name = header.get("title", {}).get("dynamicTextViewModel", {}).get("text", {}).get("content", "")
                        if is_matching_channel_name(query, name):
                            subs = ""
                            vcount = ""
                            avatar_url = ""
                            sources = header.get("image", {}).get("contentPreviewImageViewModel", {}).get("image", {}).get("sources", [])
                            if sources:
                                avatar_url = sources[0].get("url", "")
                            for row in header.get("metadata", {}).get("contentMetadataViewModel", {}).get("metadataRows", []):
                                for part in row.get("metadataParts", []):
                                    txt = part.get("text", {}).get("content", "")
                                    if "người đăng ký" in txt or "subscribers" in txt.lower():
                                        subs = txt
                                    elif "video" in txt.lower():
                                        vcount = txt
                            print(f"CHANNEL|{_safe(name)}|{_safe(subs)}|{_safe(vcount)}|{_safe(avatar_url)}", flush=True)
                            channel_printed = True

        for section in sections:
            items = section.get("itemSectionRenderer", {}).get("contents", [])
            for item in items:
                vr = item.get("videoRenderer")
                if not vr:
                    continue
                vid = vr.get("videoId")
                if not vid:
                    continue
                title = "".join(r.get("text", "") for r in vr.get("title", {}).get("runs", []))
                channel = "".join(r.get("text", "") for r in vr.get("ownerText", {}).get("runs", []))
                duration = vr.get("lengthText", {}).get("simpleText", "--:--")
                views_str = vr.get("viewCountText", {}).get("simpleText", "")
                views_num = "".join(c for c in views_str if c.isdigit())
                print(f"{_safe(vid)}|{_safe(title)}|{_safe(channel)}|{duration}|{views_num}")
                count += 1
                if count >= max_results:
                    break
            if count >= max_results:
                break
    except Exception as e:
        print(f"WARN:innertube parse failed: {e}", file=sys.stderr)
    try:
        del data
        del sections
        import gc
        gc.collect()
    except Exception:
        pass
    return count > 0 or channel_printed


def get_trending_feed(category_id="", max_results=24):
    """Fetch YouTube Trending (chart=mostPopular) via YouTube Data API v3.
    Cost: ONLY 1 unit! Returns results in standard format.
    Falls back to innertube_search if quota exceeded.
    """
    cat = str(category_id).strip()
    if cat.startswith("feed:"):
        cat = cat[len("feed:"):].strip()

    params = {
        "part": "snippet,contentDetails,statistics",
        "chart": "mostPopular",
        "regionCode": "VN",
        "maxResults": str(min(max(max_results, 6), 50)),
    }
    if cat and cat not in ("", "0", "all"):
        params["videoCategoryId"] = cat

    data = _api_get("/videos", params)
    if not data or "items" not in data or not data["items"]:
        if "videoCategoryId" in params:
            del params["videoCategoryId"]
            data = _api_get("/videos", params)
        if not data or "items" not in data or not data["items"]:
            print("WARN:trending feed empty, falling back to innertube search", file=sys.stderr)
            fallback_query = "nhac viet hay nhat" if cat == "10" else "thinh hanh viet nam"
            return innertube_search(fallback_query, max_results)

    for item in data["items"]:
        vid = item.get("id", "")
        snippet = item.get("snippet", {})
        content = item.get("contentDetails", {})
        stats = item.get("statistics", {})
        if not vid:
            continue
        title = snippet.get("title", "")
        channel = snippet.get("channelTitle", "")
        secs = _iso8601_to_seconds(content.get("duration", ""))
        duration = fmt_duration(secs)
        views = fmt_views_raw(stats.get("viewCount", 0))
        print(f"{_safe(vid)}|{_safe(title)}|{_safe(channel)}|{duration}|{views}")
    return True


def _api_smart_search(query, max_results=20):
    """API v3 version của smart_search. Returns True nếu có output in ra.
    Returns False nếu key missing / API fail / empty results → caller fallback yt-dlp.
    """
    if not query:
        return False

    # Feed prefix: "feed:<category_id>" → official Trending feed
    if query.startswith("feed:"):
        return get_trending_feed(query[len("feed:"):], max_results)

    if not load_api_key():
        return False

    # Special syntax: "channel:<id>" → direct fetch
    if query.startswith("channel:"):
        ch_id = query[len("channel:"):].strip()
        if not ch_id:
            return False
        info = _api_get_channel_info(ch_id)
        ch_name = info["name"] if info else "Unknown"
        items = _api_channel_videos(ch_id, max_results)
        if items and info:
            subs = _fmt_subs_vn(info.get("subscribers", 0))
            vcount = _fmt_vcount_vn(info.get("video_count", 0))
            avatar = info.get("avatar", "")
            # Format chuan 5 cot (giong Innertube): CHANNEL|name|subs|vcount|avatar
            print(f"CHANNEL|{_safe(ch_name)}|{_safe(subs)}|{_safe(vcount)}|{_safe(avatar)}", flush=True)
        for it in items:
            secs = _iso8601_to_seconds(it.get("duration_iso", ""))
            duration = fmt_duration(secs)
            views = fmt_views_raw(it.get("views", 0))
            print(f"{_safe(it['id'])}|{_safe(it['title'])}|{_safe(ch_name)}|{duration}|{views}")
        return len(items) > 0

    # Step 1: Channel lookup (search + channels.list)
    ch_info = _api_lookup_channel(query)
    if ch_info and _is_strong_channel_match(query, ch_info):
        items = _api_channel_videos(ch_info["id"], max_results - 1)
        if items:
            subs = _fmt_subs_vn(ch_info.get("subscribers", 0))
            vcount = _fmt_vcount_vn(ch_info.get("video_count", 0))
            avatar = ch_info.get("avatar", "")
            # Format chuan 5 cot (giong Innertube): CHANNEL|name|subs|vcount|avatar
            print(f"CHANNEL|{_safe(ch_info['name'])}|{_safe(subs)}|{_safe(vcount)}|{_safe(avatar)}", flush=True)
            for it in items:
                secs = _iso8601_to_seconds(it.get("duration_iso", ""))
                duration = fmt_duration(secs)
                views = fmt_views_raw(it.get("views", 0))
                print(f"{_safe(it['id'])}|{_safe(it['title'])}|"
                      f"{_safe(ch_info['name'])}|{duration}|{views}")
            return True
        print("WARN:api channel videos empty, fallback yt-dlp", file=sys.stderr)
        return False

    # Step 2: Plain video search
    items = _api_search_videos(query, max_results)
    if not items:
        return False
    details = _api_video_details([it["id"] for it in items])
    for it in items:
        extra = details.get(it["id"], {})
        it["duration_iso"] = extra.get("duration_iso", "")
        it["views"] = extra.get("views", 0)
    for it in items:
        secs = _iso8601_to_seconds(it.get("duration_iso", ""))
        duration = fmt_duration(secs)
        views = fmt_views_raw(it.get("views", 0))
        print(f"{_safe(it['id'])}|{_safe(it['title'])}|{_safe(it['channel'])}|{duration}|{views}")
    return True


def smart_search(query, max_results=20):
    """Smart search với channel detection.
    P0-4: Thử YouTube Data API v3 (~600ms) trước.
    Fallback: Innertube search (0 quota, pure Python, ~300ms, NO yt-dlp).
    On device: KHÔNG BAO GIỜ chạy yt-dlp binary cho search để chống kernel OOM kill.
    """
    if not query:
        return False

    # Feed prefix: "feed:<category_id>" → official Trending feed
    if query.startswith("feed:"):
        return get_trending_feed(query[len("feed:"):], max_results)

    # Fast path 1: YouTube Data API v3 (nếu có key và còn quota)
    try:
        api_out = _api_smart_search(query, max_results)
        if api_out:
            return True
    except Exception as e:
        print(f"WARN:api smart_search failed: {e}", file=sys.stderr)

    # Fast path 2: Innertube web endpoint (0 quota, siêu nhẹ, không tốn RAM, ~300ms)
    try:
        in_out = innertube_search(query, max_results)
        if in_out:
            return True
    except Exception as e:
        print(f"WARN:innertube search failed: {e}", file=sys.stderr)

    # Special syntax: "channel:<id>" → direct fetch
    if query.startswith("channel:"):
        ch_id = query[len("channel:"):].strip()
        if ch_id:
            if can_run_device_binary():
                return innertube_search(ch_id, max_results)
            return _fetch_channel_videos(ch_id, max_results) > 0
        return False

    # On device: NEVER fallback to heavy 38MB yt-dlp binary for search (causes kernel OOM kill)
    if can_run_device_binary():
        print("WARN:device search fallback exhausted, aborting yt-dlp to prevent OOM", file=sys.stderr)
        return False

    # Step 1: Try channel lookup (dev machine only)
    try:
        channel_info = _lookup_channel(query)
    except Exception as e:
        print(f"WARN:channel lookup failed: {e}", file=sys.stderr)
        channel_info = None

    if channel_info and _is_strong_channel_match(query, channel_info):
        # Step 2: Output CHANNEL marker (flush để C++ parse được ngay)
        # Format chuan 5 cot: CHANNEL|name|subs|vcount|avatar
        print(f"CHANNEL|{_safe(channel_info['name'])}|"
              f"{_fmt_subs_vn(channel_info['subscribers'])}|"
              f"{_fmt_vcount_vn(channel_info['video_count'])}|",
              flush=True)
        # Step 3: Output latest videos từ channel
        try:
            count = _fetch_channel_videos(
                channel_info["id"], max_results - 1, channel_info["name"]
            )
        except Exception as e:
            print(f"WARN:channel videos fetch failed: {e}", file=sys.stderr)
            count = 0
        if count > 0:
            return True
        # Nếu channel lookup OK nhưng fetch videos fail → fallback video search
        print(f"WARN:fallback to video search for '{query}'", file=sys.stderr)

    # Step 4: Fallback to video search
    return search(query, max_results)


def search(query, max_results=20):
    """Search YouTube and print results to stdout."""
    output = run_yt_dlp([
        "--flat-playlist",
        "--print", "%(id)s|%(title)s|%(uploader)s|%(duration)s|%(view_count)s",
        f"ytsearch{max_results}:{query}"
    ], timeout=30)

    if output is None:
        return False

    lines = [l.strip() for l in output.strip().splitlines() if l.strip() and "|" in l]
    if not lines:
        return _search_json_fallback(query, max_results)

    for line in lines:
        parts = line.split("|", 4)
        if len(parts) >= 4:
            vid, title, channel = parts[0], parts[1], parts[2]
            duration = fmt_duration(parts[3])
            views = fmt_views_raw(parts[4]) if len(parts) > 4 else ""
            print(f"{_safe(vid)}|{_safe(title)}|{_safe(channel)}|{duration}|{views}")
    return True


def _search_json_fallback(query, max_results=20):
    """Fallback using --dump-json when --flat-playlist output is empty."""
    output = run_yt_dlp([
        "--dump-json",
        "--playlist-end", str(max_results),
        f"ytsearch:{query}"
    ], timeout=30)

    if output is None:
        return False

    count = 0
    for line in output.strip().splitlines():
        if not line.strip():
            continue
        try:
            data = _json.loads(line)
            vid = data.get("id", "")
            title = data.get("title", "")
            channel = data.get("uploader", data.get("channel", ""))
            duration = fmt_duration(data.get("duration"))
            views = fmt_views_raw(data.get("view_count"))
            print(f"{_safe(vid)}|{_safe(title)}|{_safe(channel)}|{duration}|{views}")
            count += 1
        except _json.JSONDecodeError:
            continue
    return count > 0


def get_url(video_id, max_height=720):
    """Get direct stream URL for mpv, preferring <=max_height."""
    url = f"https://www.youtube.com/watch?v={video_id}"

    # Fast path 1: Android client (Format 22 progressive 720p MP4 or best <= 720p)
    output = run_yt_dlp([
        "-g",
        "--cache-dir", "/tmp/yt_cache",
        "--no-warnings",
        "--no-check-certificates",
        "--extractor-args", "youtube:player_client=android",
        "-f", f"22/best[height<={max_height}]/best",
        url
    ], timeout=12)
    if output and output.strip():
        stream_url = output.strip().splitlines()[0].strip()
        if stream_url.startswith("http"):
            print(stream_url)
            return True

    # Fast path 2: Multi-client fallback
    output = run_yt_dlp([
        "-g",
        "--cache-dir", "/tmp/yt_cache",
        "--no-warnings",
        "--no-check-certificates",
        "--extractor-args", "youtube:player_client=android,ios,web",
        "-f", f"best[height<={max_height}]/best",
        url
    ], timeout=12)
    if output and output.strip():
        stream_url = output.strip().splitlines()[0].strip()
        if stream_url.startswith("http"):
            print(stream_url)
            return True

    print(f"ERROR:NOSTREAM could not get stream URL for {video_id}", file=sys.stderr)
    return False


def main():
    if len(sys.argv) < 3:
        print("Usage: youtube_search.py search <query> [max_results]", file=sys.stderr)
        print("       youtube_search.py smart  <query> [max_results]", file=sys.stderr)
        print("       youtube_search.py url    <video_id>", file=sys.stderr)
        sys.exit(1)

    cmd = sys.argv[1].lower()
    arg = sys.argv[2]

    if cmd == "search":
        max_results = int(sys.argv[3]) if len(sys.argv) > 3 else 20
        ok = search(arg, max_results)
    elif cmd == "smart":
        # P0-3: channel-aware search (auto-detect channel match)
        max_results = int(sys.argv[3]) if len(sys.argv) > 3 else 20
        ok = smart_search(arg, max_results)
    elif cmd == "feed":
        # Official Trending feed by category (chart=mostPopular)
        max_results = int(sys.argv[3]) if len(sys.argv) > 3 else 24
        ok = get_trending_feed(arg, max_results)
    elif cmd == "url":
        ok = get_url(arg)
    else:
        print(f"Unknown command: {cmd}", file=sys.stderr)
        sys.exit(1)

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
