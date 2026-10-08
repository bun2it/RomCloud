#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Exporter - Xuất bản Live Playlist M3U và File Manifest OTA chuẩn hóa.
Chọn lọc các luồng tốt nhất (ping thấp nhất, uptime cao nhất, metadata đầy đủ).
"""

import json
import hashlib
import time
from typing import List, Dict, Any, Optional


class LiveChannelEntry:
    def __init__(self, name: str, group: str, url: str, logo: str = "", tvg_id: str = "", latency_ms: int = 0, score: int = 0, is_backup: bool = False):
        self.name = name
        self.group = group
        self.url = url
        self.logo = logo
        self.tvg_id = tvg_id
        self.latency_ms = latency_ms
        self.score = score
        self.is_backup = is_backup


class PlaylistExporter:
    @staticmethod
    def generate_m3u_content(entries: List[LiveChannelEntry], playlist_title: str = "RomCloud Live IPTV") -> str:
        """Sinh chuỗi nội dung file M3U chuẩn cho thiết bị cầm tay"""
        lines = [
            f'#EXTM3U name="{playlist_title}"'
        ]

        # Sắp xếp kênh theo nhóm trước, sau đó theo tên kênh
        sorted_entries = sorted(entries, key=lambda e: (e.group, e.name, e.is_backup))

        for item in sorted_entries:
            display_name = item.name
            if item.is_backup:
                display_name += " (Backup)"

            extinf_attrs = []
            if item.group:
                extinf_attrs.append(f'group-title="{item.group}"')
            if item.tvg_id:
                extinf_attrs.append(f'tvg-id="{item.tvg_id}"')
            if item.name:
                extinf_attrs.append(f'tvg-name="{item.name}"')
            if item.logo:
                extinf_attrs.append(f'tvg-logo="{item.logo}"')

            attr_str = (" " + " ".join(extinf_attrs)) if extinf_attrs else ""
            lines.append(f"#EXTINF:-1{attr_str},{display_name}")
            lines.append(item.url)

        return "\n".join(lines) + "\n"

    @staticmethod
    def export_files(entries: List[LiveChannelEntry], m3u_output_path: str, manifest_output_path: str, base_download_url: str = "") -> Dict[str, Any]:
        """Ghi ra file M3U và file manifest JSON kèm mã băm SHA256"""
        m3u_content = PlaylistExporter.generate_m3u_content(entries)

        with open(m3u_output_path, "w", encoding="utf-8") as f:
            f.write(m3u_content)

        # Tính mã băm SHA256 của file m3u
        sha256_hash = hashlib.sha256(m3u_content.encode("utf-8")).hexdigest()

        # Thu thập các nhóm duy nhất
        unique_groups = sorted(list(set(e.group for e in entries if e.group)))

        version_str = time.strftime("%Y%m%d_%H%M%S")
        updated_at_str = time.strftime("%Y-%m-%d %H:%M:%S")

        manifest_data = {
            "version": version_str,
            "updated_at": updated_at_str,
            "channel_count": len([e for e in entries if not e.is_backup]),
            "total_streams": len(entries),
            "sha256": sha256_hash,
            "groups": unique_groups,
            "playlist_file": "live.m3u",
            "download_url": base_download_url
        }

        with open(manifest_output_path, "w", encoding="utf-8") as f:
            json.dump(manifest_data, f, ensure_ascii=False, indent=2)

        return manifest_data
