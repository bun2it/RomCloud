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
        """Chuẩn hóa tên nhóm theo danh mục chuẩn và logic series kênh"""
        norm_group = raw_group.strip(' \t\n\r"\'')
        # Loại bỏ icon
        norm_group = re.sub(r'[⚽🔥📺⚡⭐♦️🔴|]+', '', norm_group).strip(' \t\n\r"\'')

        ch_upper = canonical_ch_name.upper().strip()
        grp_upper = norm_group.upper().strip()

        # 1. SERIES ON & VTVcab -> Đưa chung vào VTV / Quốc Gia
        if (ch_upper.startswith("ON ") or ch_upper.startswith("ON-") or ch_upper == "ON" or
            grp_upper in ["ON", "VTVCAB", "VTV CAB", "VTVPRIME", "VTV PRIME", "KÊNH ON"] or
            any(k in ch_upper for k in ["VTVCAB", "VTV CAB", "VTVPRIME", "VTV PRIME"])):
            return "VTV / Quốc Gia"

        # 2. Logic cũ: Nếu raw_group khớp với danh mục chuẩn trong group_mappings
        if norm_group:
            for canonical_grp, aliases in self.group_mappings.items():
                for alias in aliases:
                    if alias.upper() in grp_upper or grp_upper in alias.upper():
                        return canonical_grp

        # 3. Phân loại theo series kênh (khi group rỗng hoặc không khớp nhóm chuẩn)
        # Series VTV
        if ch_upper.startswith("VTV") or any(k in ch_upper for k in ["VIETNAM TODAY", "TRUYỀN HÌNH QUỐC HỘI", "ANTV", "AN NINH TV", "NHÂN DÂN", "QUỐC PHÒNG"]):
            return "VTV / Quốc Gia"

        # Series HTV
        if ch_upper.startswith("HTV"):
            if "THỂ THAO" in ch_upper or "SPORT" in ch_upper:
                return "Thể Thao"
            return "HTV / TP.HCM"

        # Series SCTV
        if ch_upper.startswith("SCTV"):
            return "SCTV"

        # Series VTC & Tin tức
        if ch_upper.startswith("VTC") or any(k in ch_upper for k in ["THỜI SỰ", "INFO TV", "FBNC", "TIN TỨC", "NEWS"]):
            return "Tin Tức"

        # Series THVL & Đài Tỉnh / Địa Phương
        local_prefixes = [
            "THVL", "HÀ NỘI", "HANOI", "HẢI PHÒNG", "ĐÀ NẴNG", "CẦN THƠ", "BÌNH DƯƠNG", "ĐỒNG NAI",
            "BÀ RỊA", "VŨNG TÀU", "LONG AN", "TIỀN GIANG", "BẾN TRE", "TRÀ VINH", "VĨNH LONG",
            "ĐỒNG THÁP", "AN GIANG", "KIÊN GIANG", "CÀ MAU", "BẠC LIÊU", "SÓC TRĂNG", "HẬU GIANG",
            "BÌNH PHƯỚC", "TÂY NINH", "LÂM ĐỒNG", "ĐẮK LẮK", "ĐẮK NÔNG", "GIA LAI", "KON TUM",
            "NINH THUẬN", "BÌNH THUẬN", "KHÁNH HÒA", "PHÚ YÊN", "BÌNH ĐỊNH", "QUẢNG NGÃI",
            "QUẢNG NAM", "THỪA THIÊN", "HUẾ", "QUẢNG TRỊ", "QUẢNG BÌNH", "HÀ TĨNH", "NGHỆ AN",
            "THANH HÓA", "NINH BÌNH", "NAM ĐỊNH", "HÀ NAM", "THÁI BÌNH", "HẢI DƯƠNG", "HƯNG YÊN",
            "BẮC NINH", "BẮC GIANG", "VĨNH PHÚC", "PHÚ THỌ", "THÁI NGUYÊN", "TUYÊN QUANG",
            "HÀ GIANG", "CAO BẰNG", "BẮC KẠN", "LẠNG SƠN", "LÀO CAI", "YÊN BÁI", "ĐIỆN BIÊN",
            "LAI CHÂU", "SƠN LA", "HÒA BÌNH", "QUẢNG NINH", "BHTTV", "BTV", "ĐNRTV", "NTV",
            "QTV", "THP", "TTV", "KTV", "THTPCT", "HGTV", "TGV", "STV", "BLTV", "KGTV", "CMTV",
            "LTV", "BRT", "BPTV", "DRT", "QRT", "PTQ", "QBTV", "TRT", "TBTV", "NBTV", "HYTV",
            "VPPT", "PTN", "CBTV", "TNV", "BGTV"
        ]
        if any(lp in ch_upper for lp in local_prefixes) or any(k in grp_upper for k in ["ĐỊA PHƯƠNG", "TỈNH", "THVL", "PROVINCE"]):
            return "Địa phương"

        # Series K+ & Thể thao
        sports_keywords = [
            "K+", "K PLUS", "CANAL", "SPORT", "SPORTS", "BÓNG ĐÁ", "BONGDA",
            "FOOTBALL", "TENNIS", "GOLF", "F1", "BEIN", "SPOTV", "NBA", "MUTV", "EUROSPORT"
        ]
        if any(k in ch_upper for k in sports_keywords) or any(k in grp_upper for k in ["THỂ THAO", "SPORTS", "FOOTBALL", "BÓNG ĐÁ"]):
            return "Thể Thao"

        # Series Phim & Giải trí
        ent_keywords = [
            "HBO", "CINEMA", "CINEMAX", "WARNER", "AXN", "DISNEY", "CARTOON",
            "ANIMAX", "DREAMWORKS", "PHIM", "GIẢI TRÍ", "ENTERTAINMENT", "MOVIES", "CINE"
        ]
        if any(k in ch_upper for k in ent_keywords) or any(k in grp_upper for k in ["PHIM", "CINEMA", "GIẢI TRÍ", "ENTERTAINMENT"]):
            return "Phim & Giải Trí"

        # Series Quốc tế
        intl_keywords = [
            "DISCOVERY", "NATIONAL", "NAT GEO", "CNN", "BBC", "NHK", "CGTN",
            "DW", "TV5MONDE", "KBS", "ARIRANG", "AL JAZEERA", "BLOOMBERG", "ABC", "CBS", "NBC"
        ]
        if any(k in ch_upper for k in intl_keywords) or any(k in grp_upper for k in ["QUỐC TẾ", "INTERNATIONAL", "KÊNH NƯỚC NGOÀI"]):
            return "Quốc Tế"

        # Series Radio
        if any(k in ch_upper for k in ["VOV", "VOH", "RADIO"]) or "RADIO" in grp_upper:
            return "Radio"

        # Series Live Events
        if any(k in ch_upper for k in ["LIVE EVENTS", "TRỰC TIẾP", "SỰ KIỆN", "FPT PLAY", "TV360"]) or any(k in grp_upper for k in ["LIVE EVENTS", "TRỰC TIẾP", "SỰ KIỆN"]):
            return "LIVE EVENTS"

        # 4. Không xếp được vào series nào -> Đưa vào Kênh Chung
        return "Kênh Chung"

    def normalize(self, ch: RawChannel) -> Tuple[str, str]:
        """Trả về tuple: (canonical_name, canonical_group)"""
        cleaned_name = self.clean_channel_name(ch.name)
        canon_name = self.canonicalize_channel(cleaned_name)
        canon_group = self.canonicalize_group(ch.group, canon_name)
        return canon_name, canon_group
