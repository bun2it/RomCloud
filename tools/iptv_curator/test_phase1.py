#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Test Suite cho Phase 1 - Kiểm tra toàn diện Parser, Normalizer, Health Checker, DB History và Exporter.
"""

import os
import sys
import unittest
import tempfile
import shutil

# Thêm thư mục hiện tại vào sys.path
sys.path.insert(0, os.path.dirname(__file__))

from m3u_parser import M3UParser, RawChannel
from normalizer import Normalizer
from health_checker import HealthChecker, ProbeResult
from db_history import HistoryDB
from exporter import PlaylistExporter, LiveChannelEntry
from curator_engine import CuratorEngine


class TestPhase1(unittest.TestCase):
    def setUp(self):
        self.test_dir = tempfile.mkdtemp()
        self.rules_file = os.path.join(os.path.dirname(__file__), "rules.json")

    def tearDown(self):
        shutil.rmtree(self.test_dir, ignore_errors=True)

    def test_01_m3u_parser(self):
        sample_m3u = """#EXTM3U url-tvg="https://epg.xml"
#EXTINF:-1 group-title="THỂ THAO QUỐC TẾ" tvg-id="vtv3" tvg-logo="https://logo.png",VTV 3 HD
http://stream.example.com/vtv3.m3u8
#EXTINF:-1 group-title="⚽| THỂ THAO" tvg-id="kplus1" tvg-logo="https://kplus.png",K+ Sport 1 (1080p)
http://stream.example.com/kplus1.ts
"""
        channels = M3UParser.parse_content(sample_m3u, "sample.m3u")
        self.assertEqual(len(channels), 2)
        self.assertEqual(channels[0].name, "VTV 3 HD")
        self.assertEqual(channels[0].group, "THỂ THAO QUỐC TẾ")
        self.assertEqual(channels[0].url, "http://stream.example.com/vtv3.m3u8")
        self.assertEqual(channels[1].name, "K+ Sport 1 (1080p)")
        self.assertEqual(channels[1].group, "⚽| THỂ THAO")

    def test_02_normalizer(self):
        normalizer = Normalizer(self.rules_file)

        # Test clean_channel_name
        self.assertEqual(normalizer.clean_channel_name("VTV 3 [HD]"), "VTV 3")
        self.assertEqual(normalizer.clean_channel_name("K+ Sport 1 (1080p) [50fps]"), "K+ Sport 1")
        self.assertEqual(normalizer.clean_channel_name("⚽ VTV1 4K VIP 🔥"), "VTV1")

        # Test canonicalize_channel
        self.assertEqual(normalizer.canonicalize_channel("vtv 3"), "VTV3")
        self.assertEqual(normalizer.canonicalize_channel("vtv3 hd"), "VTV3")
        self.assertEqual(normalizer.canonicalize_channel("k+ sport 1"), "K+ Sport 1")
        self.assertEqual(normalizer.canonicalize_channel("k+ 1"), "K+ Sport 1")
        self.assertEqual(normalizer.canonicalize_channel("canal+ live 1"), "Canal+ Sport")

        # Test canonicalize_group
        self.assertEqual(normalizer.canonicalize_group("⚽| THỂ THAO QUỐC TẾ"), "Thể Thao")
        self.assertEqual(normalizer.canonicalize_group("KÊNH VTV"), "VTV / Quốc Gia")
        self.assertEqual(normalizer.canonicalize_group("", "VTV3"), "VTV / Quốc Gia")

    def test_03_db_history_scoring(self):
        db_path = os.path.join(self.test_dir, "test_history.db")
        db = HistoryDB(db_path)

        url1 = "http://stream.test/vtv3"
        # Lần 1: Thành công với ping 120ms
        probe1 = ProbeResult(url=url1, is_alive=True, latency_ms=120, status_code=200)
        db.record_probe(probe1, channel_name="VTV3", group_name="VTV / Quốc Gia")

        records = db.get_channel_streams("VTV3")
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["last_status"], "ALIVE")
        self.assertGreater(records[0]["reputation_score"], 50)

        # Lần 2: Thất bại 1 lần -> KHÔNG xóa, chuyển sang STANDBY
        probe2 = ProbeResult(url=url1, is_alive=False, latency_ms=3000, error="Timeout")
        db.record_probe(probe2, channel_name="VTV3", group_name="VTV / Quốc Gia")

        records2 = db.get_channel_streams("VTV3")
        self.assertEqual(records2[0]["last_status"], "STANDBY")
        self.assertEqual(records2[0]["consecutive_fails"], 1)

        # Lần 3: Hôm sau sống lại -> phục hồi ALIVE
        probe3 = ProbeResult(url=url1, is_alive=True, latency_ms=100, status_code=200)
        db.record_probe(probe3, channel_name="VTV3", group_name="VTV / Quốc Gia")

        records3 = db.get_channel_streams("VTV3")
        self.assertEqual(records3[0]["last_status"], "ALIVE")
        self.assertEqual(records3[0]["consecutive_fails"], 0)

    def test_04_exporter(self):
        entries = [
            LiveChannelEntry(name="VTV1", group="VTV / Quốc Gia", url="http://s/vtv1", logo="http://l/vtv1.png", latency_ms=50),
            LiveChannelEntry(name="VTV1", group="VTV / Quốc Gia", url="http://s/vtv1_bk", logo="http://l/vtv1.png", latency_ms=150, is_backup=True),
            LiveChannelEntry(name="K+ Sport 1", group="Thể Thao", url="http://s/k1", logo="http://l/k1.png", latency_ms=80)
        ]

        m3u_file = os.path.join(self.test_dir, "live.m3u")
        manifest_file = os.path.join(self.test_dir, "iptv_manifest.json")

        manifest = PlaylistExporter.export_files(entries, m3u_file, manifest_file)

        self.assertTrue(os.path.exists(m3u_file))
        self.assertTrue(os.path.exists(manifest_file))
        self.assertEqual(manifest["channel_count"], 2)
        self.assertEqual(manifest["total_streams"], 3)
        self.assertIn("Thể Thao", manifest["groups"])
        self.assertIn("VTV / Quốc Gia", manifest["groups"])

        with open(m3u_file, "r", encoding="utf-8") as f:
            content = f.read()
        self.assertIn("#EXTM3U", content)
        self.assertIn('group-title="VTV / Quốc Gia"', content)
        self.assertIn("VTV1 (Backup)", content)

    def test_05_curator_engine_with_default_m3u(self):
        # Test tích hợp toàn bộ pipeline với file iptv/default.m3u có sẵn
        repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
        default_m3u = os.path.join(repo_root, "iptv", "default.m3u")

        self.assertTrue(os.path.exists(default_m3u), f"File {default_m3u} must exist")

        engine = CuratorEngine(work_dir=self.test_dir)
        count = engine.add_source_file(default_m3u)
        self.assertGreater(count, 0)

        # Gom cụm và chuẩn hóa
        cluster_stats = engine.process_and_cluster()
        self.assertGreater(cluster_stats["distinct_channels"], 0)
        self.assertLessEqual(cluster_stats["distinct_channels"], cluster_stats["total_raw"])

        # Xuất file (không cần probe mạng để test chạy offline siêu nhanh)
        result = engine.export(output_dir=self.test_dir, include_backup=True)
        self.assertTrue(os.path.exists(result["m3u_path"]))
        self.assertTrue(os.path.exists(result["manifest_path"]))
        self.assertGreater(result["exported_entries"], 0)


if __name__ == "__main__":
    unittest.main()
