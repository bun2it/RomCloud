#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Test Suite cho Phase 2 - Kiểm tra toàn diện Web Server và các API RESTful của Web Localhost.
"""

import os
import sys
import time
import json
import unittest
import urllib.request
import urllib.error
import threading
from http.server import HTTPServer

# Thêm thư mục iptv_curator vào sys.path
CUR_DIR = os.path.dirname(os.path.abspath(__file__))
if CUR_DIR not in sys.path:
    sys.path.insert(0, CUR_DIR)

from server import CuratorRequestHandler, engine

TEST_PORT = 8899
BASE_URL = f"http://127.0.0.1:{TEST_PORT}"


class TestPhase2Server(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Chạy HTTP Server trên cổng test 8899
        cls.httpd = HTTPServer(("127.0.0.1", TEST_PORT), CuratorRequestHandler)
        cls.server_thread = threading.Thread(target=cls.httpd.serve_forever, daemon=True)
        cls.server_thread.start()
        time.sleep(0.3)  # Đợi server sẵn sàng

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        cls.httpd.server_close()

    def request_get(self, path: str):
        req = urllib.request.Request(f"{BASE_URL}{path}")
        with urllib.request.urlopen(req, timeout=3) as resp:
            return resp.getcode(), resp.headers.get_content_type(), resp.read()

    def request_post_json(self, path: str, data: dict):
        body = json.dumps(data).encode("utf-8")
        req = urllib.request.Request(
            f"{BASE_URL}{path}",
            data=body,
            headers={"Content-Type": "application/json"}
        )
        with urllib.request.urlopen(req, timeout=3) as resp:
            return resp.getcode(), json.loads(resp.read().decode("utf-8"))

    def test_01_static_files(self):
        # 1. HTML
        code, ctype, body = self.request_get("/")
        self.assertEqual(code, 200)
        self.assertIn("text/html", ctype)
        self.assertIn(b"RomCloud IPTV Curator", body)

        # 2. CSS
        code, ctype, body = self.request_get("/app.css")
        self.assertEqual(code, 200)
        self.assertIn("text/css", ctype)
        self.assertIn(b"--accent-cyan", body)

        # 3. JS
        code, ctype, body = self.request_get("/app.js")
        self.assertEqual(code, 200)
        self.assertIn("javascript", ctype)
        self.assertIn(b"switchTab", body)

    def test_02_api_stats(self):
        code, ctype, body = self.request_get("/api/stats")
        self.assertEqual(code, 200)
        data = json.loads(body.decode("utf-8"))
        self.assertIn("cluster", data)
        self.assertIn("db", data)
        self.assertIn("probe", data)

    def test_03_api_upload_text_and_channels(self):
        # Upload sample m3u
        sample_m3u = """#EXTM3U
#EXTINF:-1 group-title="THỂ THAO" tvg-id="vtv3",VTV 3 [HD]
http://stream.test/vtv3_hd
#EXTINF:-1 group-title="THỂ THAO QUỐC TẾ" tvg-id="vtv3",VTV3 1080p
http://stream.test/vtv3_1080p
"""
        code, resp = self.request_post_json("/api/sources/upload_text", {
            "filename": "test_upload.m3u",
            "content": sample_m3u
        })
        self.assertEqual(code, 200)
        self.assertTrue(resp["success"])
        self.assertEqual(resp["added_channels"], 2)

        # Kiểm tra danh sách channels
        code, ctype, body = self.request_get("/api/channels")
        self.assertEqual(code, 200)
        data = json.loads(body.decode("utf-8"))
        self.assertGreater(data["total"], 0)

        # Kiểm tra VTV3 đã được gộp 2 luồng
        vtv3 = next((ch for ch in data["channels"] if ch["name"] == "VTV3"), None)
        self.assertIsNotNone(vtv3)
        self.assertEqual(vtv3["stream_count"], 2)
        self.assertEqual(vtv3["group"], "Thể Thao")

    def test_04_api_groups_and_rules(self):
        code, ctype, body = self.request_get("/api/groups")
        self.assertEqual(code, 200)
        groups_data = json.loads(body.decode("utf-8"))
        self.assertIn("Tất cả", groups_data["groups"])
        self.assertIn("Thể Thao", groups_data["groups"])

        code, ctype, body = self.request_get("/api/rules")
        self.assertEqual(code, 200)
        rules_data = json.loads(body.decode("utf-8"))
        self.assertIn("channel_aliases", rules_data)
        self.assertIn("group_mappings", rules_data)

    def test_05_api_export_and_download(self):
        code, resp = self.request_post_json("/api/export", {"include_backup": True})
        self.assertEqual(code, 200)
        self.assertTrue(resp["success"])

        # Test download live.m3u
        code, ctype, body = self.request_get("/download/live.m3u")
        self.assertEqual(code, 200)
        self.assertIn(b"#EXTM3U", body)
        self.assertIn(b"VTV3", body)

        # Test download iptv_manifest.json
        code, ctype, body = self.request_get("/download/manifest.json")
        self.assertEqual(code, 200)
        manifest = json.loads(body.decode("utf-8"))
        self.assertIn("sha256", manifest)
        self.assertIn("version", manifest)
        self.assertGreater(manifest["channel_count"], 0)


if __name__ == "__main__":
    unittest.main()
