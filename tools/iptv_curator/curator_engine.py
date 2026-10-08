#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Curator Engine - Bộ điều phối trung tâm của Phase 1.
Kết nối Parser -> Normalizer -> HealthChecker -> HistoryDB -> Exporter thành quy trình hoàn chỉnh.
"""

import os
from typing import List, Dict, Any, Optional, Callable
from m3u_parser import M3UParser, RawChannel
from normalizer import Normalizer
from health_checker import HealthChecker, ProbeResult
from db_history import HistoryDB
from exporter import PlaylistExporter, LiveChannelEntry


class CuratorEngine:
    def __init__(self, work_dir: Optional[str] = None):
        if work_dir is None:
            work_dir = os.path.dirname(__file__)
        self.work_dir = work_dir

        self.normalizer = Normalizer(rules_path=os.path.join(self.work_dir, "rules.json"))
        self.db = HistoryDB(db_path=os.path.join(self.work_dir, "iptv_history.db"))

        self.raw_channels: List[RawChannel] = []
        self.clustered_channels: Dict[str, List[Dict[str, Any]]] = {}

    def add_source_file(self, file_path: str) -> int:
        """Đọc và thêm các kênh từ file M3U cục bộ"""
        if not os.path.exists(file_path):
            raise FileNotFoundError(f"File not found: {file_path}")
        chs = M3UParser.parse_file(file_path)
        self.raw_channels.extend(chs)
        return len(chs)

    def add_source_url(self, url: str) -> int:
        """Tải và thêm các kênh từ link M3U trên mạng"""
        chs = M3UParser.parse_url(url)
        self.raw_channels.extend(chs)
        return len(chs)

    def add_source_content(self, content: str, source_name: str = "custom.m3u") -> int:
        """Thêm các kênh từ chuỗi nội dung văn bản"""
        chs = M3UParser.parse_content(content, source_file=source_name)
        self.raw_channels.extend(chs)
        return len(chs)

    def clear_sources(self):
        self.raw_channels.clear()
        self.clustered_channels.clear()

    def process_and_cluster(self) -> Dict[str, Any]:
        """
        Bước 1: Chuẩn hóa tên/nhóm kênh và gom cụm các luồng của cùng 1 kênh
        Loại trừ trùng lặp URL trong cùng 1 kênh.
        """
        self.clustered_channels.clear()
        unique_urls = set()

        for ch in self.raw_channels:
            if not ch.url:
                continue

            canon_name, canon_group = self.normalizer.normalize(ch)

            if canon_name not in self.clustered_channels:
                self.clustered_channels[canon_name] = []

            # Kiểm tra xem URL này đã có trong danh sách của kênh chưa
            existing_urls = [c["url"] for c in self.clustered_channels[canon_name]]
            if ch.url not in existing_urls:
                self.clustered_channels[canon_name].append({
                    "raw": ch,
                    "canonical_name": canon_name,
                    "canonical_group": canon_group,
                    "url": ch.url,
                    "logo": ch.logo,
                    "tvg_id": ch.tvg_id,
                    "source_file": ch.source_file
                })
                unique_urls.add(ch.url)

        return {
            "total_raw": len(self.raw_channels),
            "distinct_channels": len(self.clustered_channels),
            "unique_stream_urls": len(unique_urls)
        }

    def check_health(self, max_workers: int = 25, timeout: int = 3, progress_callback: Optional[Callable[[int, int, ProbeResult], None]] = None) -> Dict[str, ProbeResult]:
        """
        Bước 2: Quét đa luồng kiểm tra độ sống/chết và đo độ trễ cho toàn bộ link stream
        Cập nhật kết quả vào database lịch sử.
        """
        all_urls = []
        url_meta_map = {}

        for ch_name, stream_list in self.clustered_channels.items():
            for item in stream_list:
                u = item["url"]
                if u not in url_meta_map:
                    all_urls.append(u)
                    url_meta_map[u] = item

        probe_results = HealthChecker.probe_batch(
            all_urls,
            max_workers=max_workers,
            timeout=timeout,
            progress_callback=progress_callback
        )

        # Lưu vào database lịch sử
        for u, res in probe_results.items():
            meta = url_meta_map.get(u, {})
            self.db.record_probe(
                probe=res,
                channel_name=meta.get("canonical_name", ""),
                group_name=meta.get("canonical_group", ""),
                logo=meta.get("logo", ""),
                tvg_id=meta.get("tvg_id", "")
            )

        return probe_results

    def generate_live_entries(self, include_backup: bool = True, probe_results: Optional[Dict[str, ProbeResult]] = None) -> List[LiveChannelEntry]:
        """
        Bước 3: Chọn lọc luồng tốt nhất cho mỗi kênh dựa trên Ping ms và Điểm uy tín
        """
        live_entries: List[LiveChannelEntry] = []

        for ch_name, stream_list in self.clustered_channels.items():
            if not stream_list:
                continue

            # Lấy thông tin xếp hạng từ DB và probe hiện tại
            candidate_streams = []
            for s in stream_list:
                url = s["url"]
                db_info = self.db.get_channel_streams(ch_name)
                stream_record = next((r for r in db_info if r["url"] == url), None)

                # Ưu tiên kết quả probe mới nhất
                if probe_results and url in probe_results:
                    p = probe_results[url]
                    is_alive = p.is_alive
                    latency = p.latency_ms
                elif stream_record:
                    is_alive = (stream_record["last_status"] == "ALIVE")
                    latency = stream_record["last_latency_ms"]
                else:
                    is_alive = True
                    latency = 999

                score = stream_record["reputation_score"] if stream_record else 50

                candidate_streams.append({
                    "url": url,
                    "name": ch_name,
                    "group": s["canonical_group"],
                    "logo": s["logo"],
                    "tvg_id": s["tvg_id"],
                    "is_alive": is_alive,
                    "latency": latency,
                    "score": score
                })

            # Sắp xếp candidate:
            # 1. Kênh còn sống lên đầu
            # 2. Ping ms thấp lên đầu (nếu sống)
            # 3. Điểm uy tín cao hơn lên đầu
            candidate_streams.sort(
                key=lambda x: (
                    0 if x["is_alive"] else 1,
                    x["latency"] if x["is_alive"] else 9999,
                    -x["score"]
                )
            )

            # Chọn link chính
            best = candidate_streams[0]
            # Nếu link chính còn sống, hoặc chấp nhận link tốt nhất
            live_entries.append(LiveChannelEntry(
                name=best["name"],
                group=best["group"],
                url=best["url"],
                logo=best["logo"],
                tvg_id=best["tvg_id"],
                latency_ms=best["latency"],
                score=best["score"],
                is_backup=False
            ))

            # Chọn link dự phòng (nếu có hơn 1 stream và được bật)
            if include_backup and len(candidate_streams) > 1:
                second = candidate_streams[1]
                if second["is_alive"] or second["score"] >= 40:
                    live_entries.append(LiveChannelEntry(
                        name=second["name"],
                        group=second["group"],
                        url=second["url"],
                        logo=second["logo"] or best["logo"],
                        tvg_id=second["tvg_id"] or best["tvg_id"],
                        latency_ms=second["latency"],
                        score=second["score"],
                        is_backup=True
                    ))

        return live_entries

    def export(self, output_dir: Optional[str] = None, include_backup: bool = True, probe_results: Optional[Dict[str, ProbeResult]] = None) -> Dict[str, Any]:
        """
        Bước 4: Xuất ra live.m3u và iptv_manifest.json
        """
        if output_dir is None:
            output_dir = os.path.join(self.work_dir, "output")
        os.makedirs(output_dir, exist_ok=True)

        m3u_path = os.path.join(output_dir, "live.m3u")
        manifest_path = os.path.join(output_dir, "iptv_manifest.json")

        entries = self.generate_live_entries(include_backup=include_backup, probe_results=probe_results)
        manifest = PlaylistExporter.export_files(entries, m3u_path, manifest_path)

        return {
            "m3u_path": m3u_path,
            "manifest_path": manifest_path,
            "manifest": manifest,
            "exported_entries": len(entries)
        }
