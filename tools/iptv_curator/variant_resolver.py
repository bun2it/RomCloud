#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Variant Resolver - Ép sẵn luồng HLS về <=720p ngay lúc xuất playlist.

Máy Brick (A53) giải mã 1080p bị giật, nên curator resolve mỗi URL đã chọn
(best + backup) về variant <=720p rồi mới ghi live.m3u. Máy phát thẳng URL
có sẵn, khỏi dò sibling master lúc bấm phát (nhanh + determinist).

Logic mirror resolveCappedHlsUrl() trong src/iptv/IPTVManager.cpp:
- URL không phải .m3u8 (mpd/ts/mp4...) -> giữ nguyên.
- Master có EXT-X-STREAM-INF -> chọn variant cao nhất mà height<=720
  (thiếu RESOLUTION thì bandwidth<=3Mbps, không có nữa thì thấp nhất).
- Media playlist ghim thẳng variant (kiểu FPT avc1_5600000): dò master
  cùng thư mục (index/master/playlist.m3u8) rồi chọn lại.
- AN TOÀN: chỉ ghi URL resolve khi nó ổn định (URI tương đối hoặc
  absolute không query). Variant absolute KÈM query (?token=...) thường
  hết hạn theo giờ -> GIỮ NGUYÊN URL master, máy sẽ ép lúc phát.
  Fetch lỗi/timeout -> giữ nguyên URL gốc (không bao giờ làm chết kênh).
"""

import re
import time
import unicodedata
from typing import Dict, List, Optional, Tuple
from urllib.parse import urlparse, urljoin
from urllib.request import Request, urlopen
from concurrent.futures import ThreadPoolExecutor, as_completed

CHROME_UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
             "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36")

MASTER_FILENAMES = ("index.m3u8", "master.m3u8", "playlist.m3u8")
# Tên file gợi ý đây là 1 variant trong bộ multi-bitrate (giống máy)
VARIANT_HINTS = ("avc", "=", "1080", "720", "480", "360",
                 "chunklist", "variant", "rendition", "bitrate", "quality")


class Variant:
    def __init__(self, width=0, height=0, bandwidth=0, uri_bw=0, uri=""):
        self.width = width
        self.height = height
        self.bandwidth = bandwidth
        self.uri_bw = uri_bw
        self.uri = uri

    def eff_bw(self) -> int:
        return self.bandwidth if self.bandwidth > 0 else self.uri_bw


def is_hls_url(url: str) -> bool:
    u = (url or "").lower()
    return ".m3u8" in u


def _fetch(url: str, timeout: int) -> Optional[str]:
    try:
        req = Request(url, headers={"User-Agent": CHROME_UA})
        with urlopen(req, timeout=timeout) as resp:
            raw = resp.read(1 << 20)  # playlist nhỏ, cắt 1MB chống treo
        try:
            return raw.decode("utf-8", errors="replace")
        except Exception:
            return None
    except Exception:
        return None


def _guess_uri_bw(uri: str) -> int:
    """Đoán bandwidth từ tên variant kiểu FPT (avc1_5600000=...)."""
    p = uri.find("avc1_")
    if p < 0:
        return 0
    m = re.match(r"(\d+)", uri[p + 5:])
    if not m:
        return 0
    v = int(m.group(1))
    return v if 100000 <= v <= 100000000 else 0


def _parse_variants(body: str) -> List[Variant]:
    variants: List[Variant] = []
    cur = Variant()
    has_variant = False
    for line in body.splitlines():
        line = line.strip(" \r\n\t")
        if line.upper().startswith("#EXT-X-STREAM-INF:"):
            cur = Variant()
            up = line.upper()
            rp = up.find("RESOLUTION=")
            if rp >= 0:
                xp = up.find("X", rp + 11)
                if xp >= 0:
                    try:
                        cur.width = int(line[rp + 11:xp])
                    except ValueError:
                        pass
                    m2 = re.match(r"(\d+)", line[xp + 1:])
                    if m2:
                        cur.height = int(m2.group(1))
            bp = up.find("BANDWIDTH=")
            if bp >= 0:
                m = re.match(r"(\d+)", line[bp + 10:])
                if m:
                    cur.bandwidth = int(m.group(1))
            has_variant = True
        elif has_variant and line and not line.startswith("#"):
            cur.uri = line
            cur.uri_bw = _guess_uri_bw(line)
            variants.append(cur)
            has_variant = False
    return variants


def _looks_like_variant(url: str) -> bool:
    low = url.split("?", 1)[0].lower()
    return any(h in low for h in VARIANT_HINTS)


def _probe_sibling_master(url: str, timeout: int) -> Tuple[List[Variant], str, int]:
    """Dò index/master/playlist.m3u8 cùng thư mục.
    Trả (variants, master_url, elapsed_ms của fetch dò trúng)."""
    no_query = url.split("?", 1)[0]
    if not _looks_like_variant(no_query):
        return [], url, 0
    slash = no_query.rfind("/")
    if slash < 0:
        return [], url, 0
    directory = no_query[:slash + 1]
    for name in MASTER_FILENAMES:
        sib = directory + name
        if sib == no_query:
            continue
        t0 = time.time()
        body = _fetch(sib, timeout)
        elapsed = int((time.time() - t0) * 1000)
        if not body or "#EXT-X-STREAM-INF" not in body:
            continue
        variants = _parse_variants(body)
        if variants:
            return variants, sib, elapsed
    return [], url, 0


def _pick_720p(variants: List[Variant], target: int = 720) -> Optional[Variant]:
    best: Optional[Variant] = None
    for v in variants:
        if v.height > 0:
            ok = v.height <= target
        else:
            bw = v.eff_bw()
            ok = bw > 0 and bw <= 3000000
        if ok:
            if (best is None or v.height > best.height or
                    (v.height == best.height and v.eff_bw() > best.eff_bw())):
                best = v
    if best is None:
        for v in variants:
            if best is None:
                best = v
                continue
            a, b = v.eff_bw(), best.eff_bw()
            if a > 0 and (b == 0 or a < b):
                best = v
            elif b == 0 and v.height > 0 and (
                    best.height == 0 or v.height < best.height):
                best = v
    return best


def _join_variant(master_url: str, uri: str) -> str:
    """Nối URI tương đối với master + giữ query gốc (giống máy)."""
    if uri.startswith("http://") or uri.startswith("https://"):
        return uri
    q = master_url.find("?")
    base = master_url[:q] if q >= 0 else master_url
    if uri.startswith("/"):
        p = urlparse(base)
        resolved = p.scheme + "://" + p.netloc + uri
    else:
        resolved = base.rsplit("/", 1)[0] + "/" + uri
    if q >= 0 and "?" not in resolved:
        resolved += master_url[q:]
    return unicodedata.normalize("NFC", resolved)


def _is_stable(resolved: str, was_relative: bool) -> bool:
    """Chỉ bake URL ổn định: URI tương đối, hoặc absolute không query.
    Absolute kèm query (?token=...) thường hết hạn -> không bake."""
    if was_relative:
        return True
    parts = urlparse(resolved)
    return not parts.query


def resolve_720p_url(url: str, timeout: int = 5) -> Tuple[str, str, int]:
    """Trả (url_out, info, elapsed_ms): info in {kept, capped, probed, failed}.

    - kept: giữ nguyên (không HLS / 1 variant / variant cần token / lỗi).
    - capped/probed: đã ép về variant <=720p (probed = qua dò sibling).
    - elapsed_ms: thời gian fetch đã chứng minh URL sống (0 nếu giữ gốc).
    Không bao giờ trả URL rỗng hay ném exception.
    """
    if not url or not is_hls_url(url):
        return url, "kept", 0
    try:
        t0 = time.time()
        body = _fetch(url, timeout)
        elapsed = int((time.time() - t0) * 1000)
        if not body or "#EXTM3U" not in body:
            return url, "failed", 0
        variants = _parse_variants(body)
        master_url = url
        probed = False
        if not variants and "#EXTINF" in body:
            variants, master_url, elapsed = _probe_sibling_master(url, timeout)
            probed = bool(variants)
            if not variants:
                return url, "kept", 0  # single-bitrate thật sự: giữ gốc
        if not variants:
            return url, "kept", 0
        best = _pick_720p(variants)
        if best is None or not best.uri:
            return url, "kept", 0
        was_relative = not (best.uri.startswith("http://") or
                            best.uri.startswith("https://"))
        resolved = _join_variant(master_url, best.uri)
        if not _is_stable(resolved, was_relative):
            return url, "kept", 0
        # Đã là chính nó (single-variant bake vô nghĩa) -> giữ gốc
        if resolved.split("?", 1)[0] == url.split("?", 1)[0]:
            return url, "kept", 0
        return resolved, ("probed" if probed else "capped"), elapsed
    except Exception:
        return url, "failed", 0


def resolve_many(urls: List[str], workers: int = 10,
                 timeout: int = 5) -> Dict[str, Tuple[str, str, int]]:
    """Resolve song song 1 tập URL (tự dedupe).
    Trả {url_goc: (url_out, info, elapsed_ms)}."""
    uniq = sorted(set(u for u in urls if u))
    out: Dict[str, Tuple[str, str]] = {}
    if not uniq:
        return out
    with ThreadPoolExecutor(max_workers=max(1, workers)) as pool:
        futs = {pool.submit(resolve_720p_url, u, timeout): u for u in uniq}
        for fut in as_completed(futs):
            u = futs[fut]
            try:
                out[u] = fut.result()
            except Exception:
                out[u] = (u, "failed", 0)
    return out
