#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Test probe_batch quét lại ca timeout (kênh chậm vẫn sống, không kết luận chết vội)."""

import os
import sys
import time
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(__file__))

from health_checker import HealthChecker


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/slow.m3u8":
            time.sleep(4)  # chậm nhưng sống
            body = b"#EXTM3U\n#EXTINF:4,\nseg.ts\n"
        elif self.path == "/fast.m3u8":
            body = b"#EXTM3U\n#EXTINF:4,\nseg.ts\n"
        else:
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/vnd.apple.mpegurl")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *a):
        pass


class TestProbeRetry(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.port = cls.server.server_address[1]
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()

    def test_slow_rescued_by_retry(self):
        base = f"http://127.0.0.1:{self.port}"
        res = HealthChecker.probe_batch(
            [base + "/slow.m3u8", base + "/fast.m3u8"],
            max_workers=2, timeout=1, slow_retry_timeout=8)
        self.assertTrue(res[base + "/slow.m3u8"].is_alive)
        self.assertTrue(res[base + "/fast.m3u8"].is_alive)
        self.assertGreaterEqual(res[base + "/slow.m3u8"].latency_ms, 1000)

    def test_refused_stays_dead_fast(self):
        # Port đóng: refused ngay, không retry dài (không phải timeout)
        url = f"http://127.0.0.1:{self.port + 50000 if self.port < 20000 else 9}/x.m3u8"
        t0 = time.time()
        res = HealthChecker.probe_batch([url], max_workers=1, timeout=2,
                                        slow_retry_timeout=8)
        took = time.time() - t0
        self.assertFalse(res[url].is_alive)
        self.assertLess(took, 7)


if __name__ == "__main__":
    unittest.main()
