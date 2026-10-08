#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Normalizer - Làm sạch, chuẩn hóa tên kênh và danh mục (group).
Hỗ trợ khử trùng lặp và ánh xạ vào từ điển kênh chuẩn.
"""

import re
import json
import os
from typing import Dict, List, Tuple, Optional
from m3u_parser import RawChannel


class Normalizer:
    def __init__(self, rules_path: Optional[str] = None):
        if rules_path is None:
            rules_path = os.path.join(os.path.dirname(__file__), "rules.json")

        self.rules_path = rules_path
        self.group_mappings: Dict[str, List[str]] = {}
        self.channel_aliases: Dict[str, List[str]] = {}
        self.strip_patterns: List[str] = []
        self._compiled_strips: List[re.Pattern] = []
        self.ping_thresholds: Dict[str, int] = {"good_ms": 500, "fair_ms": 1500}

        self.load_rules()

    def load_rules(self):
        if not os.path.exists(self.rules_path):
            return

        with open(self.rules_path, "r", encoding="utf-8") as f:
            data = json.load(f)

        self.group_mappings = data.get("group_mappings", {})
        self.channel_aliases = data.get("channel_aliases", {})
        self.strip_patterns = data.get("strip_patterns", [])
        self.ping_thresholds = data.get("ping_thresholds", {"good_ms": 500, "fair_ms": 1500})

        self._compiled_strips = [
            re.compile(p, re.IGNORECASE) for p in self.strip_patterns
        ]

    def save_rules(self):
        data = {
            "group_mappings": self.group_mappings,
            "channel_aliases": self.channel_aliases,
            "strip_patterns": self.strip_patterns,
            "ping_thresholds": self.ping_thresholds
        }
        with open(self.rules_path, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)

    def clean_channel_name(self, raw_name: str) -> str:
        """Loại bỏ ký tự rác, tag chất lượng, emoji khỏi tên kênh"""
        name = raw_name

        # Xóa các khối ngoặc vuông [HD], ngoặc tròn (FHD)
        name = re.sub(r'\[.*?\]', '', name)
        name = re.sub(r'\(.*?\)', '', name)

        # Xóa các tag chất lượng phổ biến
        name = re.sub(r'\b(fhd|uhd|hd|sd|4k|2k|1080p|720p|50fps|60fps|h265|hevc|raw|vip|backup|dự phòng|du phong)\b', '', name, flags=re.IGNORECASE)

        # Xóa emoji và icon đặc biệt
        name = re.sub(r'[⚽🔥📺⚡⭐♦️🔴]+', '', name)

        # Chuẩn hóa khoảng trắng
        name = re.sub(r'\s+', ' ', name).strip()

        return name if name else raw_name.strip()

    def canonicalize_channel(self, cleaned_name: str) -> str:
        """Ánh xạ tên kênh đã làm sạch vào tên chuẩn trong từ điển"""
        norm_key = cleaned_name.lower().replace(" ", "").replace("-", "").replace("_", "")

        for canonical_name, aliases in self.channel_aliases.items():
            for alias in aliases:
                alias_norm = alias.lower().replace(" ", "").replace("-", "").replace("_", "")
                if norm_key == alias_norm:
                    return canonical_name

        # Nếu không có trong từ điển, viết hoa chữ cái đầu đẹp mắt
        return cleaned_name

    def canonicalize_group(self, raw_group: str, canonical_ch_name: str = "") -> str:
        """Chuẩn hóa tên nhóm theo danh mục chuẩn"""
        norm_group = raw_group.strip()
        # Loại bỏ icon
        norm_group = re.sub(r'[⚽🔥📺⚡⭐♦️🔴|]+', '', norm_group).strip()

        if norm_group:
            upper_group = norm_group.upper()
            for canonical_grp, aliases in self.group_mappings.items():
                for alias in aliases:
                    if alias.upper() in upper_group or upper_group in alias.upper():
                        return canonical_grp

        # Nếu group rỗng hoặc không khớp, suy luận từ tên kênh chuẩn
        ch_upper = canonical_ch_name.upper()
        if ch_upper.startswith("VTV"):
            return "VTV / Quốc Gia"
        if ch_upper.startswith("HTV"):
            if "THỂ THAO" in ch_upper or "SPORT" in ch_upper:
                return "Thể Thao"
            return "HTV / TP.HCM"
        if ch_upper.startswith("VTC"):
            return "Tin Tức"
        if any(s in ch_upper for s in ["K+", "SPORT", "CANAL", "BONGDA", "FOOTBALL", "TENNIS", "F1"]):
            return "Thể Thao"
        if any(s in ch_upper for s in ["HBO", "CINEMA", "PHIM", "DISNEY", "CARTOON"]):
            return "Phim & Giải Trí"
        if any(s in ch_upper for s in ["DISCOVERY", "NATIONAL", "CNN", "BBC", "NHK"]):
            return "Quốc Tế"

        return norm_group if norm_group else "Khác"

    def normalize(self, ch: RawChannel) -> Tuple[str, str]:
        """Trả về tuple: (canonical_name, canonical_group)"""
        cleaned_name = self.clean_channel_name(ch.name)
        canon_name = self.canonicalize_channel(cleaned_name)
        canon_group = self.canonicalize_group(ch.group, canon_name)
        return canon_name, canon_group
