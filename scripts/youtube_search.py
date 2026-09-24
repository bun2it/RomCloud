#!/usr/bin/env python3
"""
youtube_search.py - YouTube search & stream URL fetcher for RomCloud

Usage:
    python3 youtube_search.py search "<query>" [max_results]
    python3 youtube_search.py url "<video_id>"

Output (search):
    One result per line: video_id|title|channel|duration|views
    UTF-8 encoded. Empty line = no results.

Output (url):
    Direct stream URL for mpv (720p max preferred).
    Fallback chain: best[height<=720] → bestvideo+bestaudio → worst

Exit codes:
    0 = success
    1 = error (network, yt-dlp not found, etc.)
"""

import subprocess
import sys
import os
import platform
import json as _json

# Resolve paths relative to this script
SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
APP_ROOT = os.path.dirname(SCRIPT_DIR)

# Bundled yt-dlp on device (ARM glibc binary, self-contained)
DEVICE_YTDLP = os.path.join(APP_ROOT, "bin", "yt-dlp")
DEVICE_YTDLP_GLIBC = os.path.join(APP_ROOT, "bin", "yt-dlp-glibc")


def can_run_device_binary():
    """True if we're running on the TrimUI device (Linux aarch64)."""
    return platform.system() == "Linux" and platform.machine() == "aarch64"


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


def fmt_views(count):
    if count is None:
        return "N/A"
    try:
        c = int(float(count))
    except (ValueError, TypeError):
        return "N/A"
    if c >= 1_000_000:
        return f"{c//1_000_000}M"
    if c >= 1_000:
        return f"{c//1_000}K"
    return str(c)


def _safe(s):
    """Escape pipe character for pipe-separated output."""
    return (s or "").replace("|", "&#124;")


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
            views = fmt_views(parts[4]) if len(parts) > 4 else "N/A"
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
            views = fmt_views(data.get("view_count"))
            print(f"{_safe(vid)}|{_safe(title)}|{_safe(channel)}|{duration}|{views}")
            count += 1
        except _json.JSONDecodeError:
            continue
    return count > 0


def get_url(video_id, max_height=720):
    """Get direct stream URL for mpv, preferring <=max_height."""
    url = f"https://www.youtube.com/watch?v={video_id}"

    # Format preference: video+audio (progressive) > video-only + audio > best
    formats = [
        f"best[height<={max_height}][vcodec!~=vp9]/best[height<={max_height}]",
        f"bestvideo[height<={max_height}]+bestaudio/best[height<={max_height}]",
        f"bestvideo[height<={max_height}]+bestaudio",
        "best",
        "worst",
    ]

    for fmt in formats:
        output = run_yt_dlp(["-g", "-f", fmt, url], timeout=20)
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
        print("       youtube_search.py url <video_id>", file=sys.stderr)
        sys.exit(1)

    cmd = sys.argv[1].lower()
    arg = sys.argv[2]

    if cmd == "search":
        max_results = int(sys.argv[3]) if len(sys.argv) > 3 else 20
        ok = search(arg, max_results)
    elif cmd == "url":
        ok = get_url(arg)
    else:
        print(f"Unknown command: {cmd}", file=sys.stderr)
        sys.exit(1)

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
