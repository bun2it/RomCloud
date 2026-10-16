#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Test variant_resolver với HTTP server fixture cục bộ (offline, determinist)."""

import os
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(__file__))

from variant_resolver import (
    resolve_720p_url, resolve_many, _parse_variants, _pick_720p,
)

MASTER = """#EXTM3U
#EXT-X-STREAM-INF:BANDWIDTH=622827,RESOLUTION=854x480
low.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=2255228,RESOLUTION=1280x720
mid.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=6686027,RESOLUTION=1920x1080
hi.m3u8
"""
PINNED = """#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:4
#EXTINF:3.84,
hi-avc1_5600000=10000-seg1.ts
"""
SINGLE = """#EXTM3U
#EXTINF:3.84,
only.ts
"""
TOKEN_MASTER = """#EXTM3U
#EXT-X-STREAM-INF:BANDWIDTH=6686027,RESOLUTION=1920x1080
https://cdn.example.com/v/hi.m3u8?token=EXPIRE123
"""


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split("?", 1)[0]
        body = {
            "/live/index.m3u8": MASTER,
            "/live/hi-avc1_5600000=10000.m3u8": PINNED,
            "/live/single.m3u8": SINGLE,
            "/live/token.m3u8": TOKEN_MASTER,
        }.get(path)
        if body is None:
            self.send_response(404)
            self.end_headers()
            return
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/vnd.apple.mpegurl")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *a):
        pass


class TestVariantResolver(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.port = cls.server.server_address[1]
        cls.thread = threading.Thread(target=cls.server.serve_forever,
                                      daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()

    def url(self, path):
        return f"http://127.0.0.1:{self.port}{path}"

    def test_master_caps_to_720p(self):
        out, info, _ms = resolve_720p_url(self.url("/live/index.m3u8"))
        self.assertEqual(info, "capped")
        self.assertTrue(out.endswith("/live/mid.m3u8"), out)

    def test_pinned_probes_sibling(self):
        out, info, _ms = resolve_720p_url(
            self.url("/live/hi-avc1_5600000=10000.m3u8"))
        self.assertEqual(info, "probed")
        self.assertTrue(out.endswith("/live/mid.m3u8"), out)

    def test_single_variant_kept(self):
        out, info, _ms = resolve_720p_url(self.url("/live/single.m3u8"))
        self.assertEqual(out, self.url("/live/single.m3u8"))
        self.assertEqual(info, "kept")

    def test_token_absolute_kept(self):
        out, info, _ms = resolve_720p_url(self.url("/live/token.m3u8"))
        self.assertEqual(out, self.url("/live/token.m3u8"))
        self.assertEqual(info, "kept")

    def test_non_hls_kept(self):
        out, info, _ms = resolve_720p_url("http://x/y.ts")
        self.assertEqual((out, info), ("http://x/y.ts", "kept"))

    def test_404_kept_safe(self):
        u = self.url("/live/nope.m3u8")
        out, info, _ms = resolve_720p_url(u)
        self.assertEqual(out, u)
        self.assertIn(info, ("failed", "kept"))

    def test_resolve_many_dedupes(self):
        a = self.url("/live/index.m3u8")
        res = resolve_many([a, a, self.url("/live/single.m3u8")])
        self.assertEqual(len(res), 2)
        self.assertTrue(res[a][0].endswith("/live/mid.m3u8"))

    def test_pick_prefers_highest_720(self):
        vs = _parse_variants(MASTER)
        self.assertEqual(len(vs), 3)
        best = _pick_720p(vs)
        self.assertEqual((best.width, best.height), (1280, 720))
        self.assertEqual(best.bandwidth, 2255228)


if __name__ == "__main__":
    unittest.main()
