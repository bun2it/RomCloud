#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Normalizer - Làm sạch, chuẩn hóa tên kênh và danh mục (group).
Hỗ trợ khử trùng lặp và ánh xạ vào từ điển kênh chuẩn.
"""

import re
import json
import os
import unicodedata
from typing import Dict, List, Tuple, Optional
from m3u_parser import RawChannel


def is_betting_channel(name: str) -> bool:
    """Nhận diện kênh cá độ/nhà cái để tự động loại khỏi xuất bản.
    Chỉ khớp thương hiệu rõ ràng (không dùng 'BET'/'88' chung chung
    để khỏi ăn nhầm phim như 'Betting With Ghost')."""
    up = unicodedata.normalize("NFC", name or "").upper()
    for kw in ("W88", "M88", "FUN88", "BK8", "188BET", "DAFABET", "VSBET",
               "DEBET", "HI88", "GO88", "12BET", "1XBET", "8XBET",
               "M88.COM", "W88.COM", "NHÀ CÁI", "NHA CAI", "CÁ ĐỘ", "CA DO",
               "TÀI XỈU", "TAI XIU", "KÈO NHÀ CÁI", "KEO NHA CAI"):
        if kw in up:
            return True
    return False


class Normalizer:
    # Stop-list mặc định cho prefix rác: không bao giờ lập nhóm từ mấy chữ này.
    DEFAULT_PREFIX_STOPLIST = [
        "THE", "TV", "HD", "VIP", "NEW", "LIVE", "FHD", "UHD", "SD",
        "ALL", "ONE", "AND", "FOR", "MAX", "PRO", "PLUS", "CHANNEL",
        "SPORT", "SPORTS", "MOVIE", "MOVIES", "NEWS", "MUSIC", "KIDS",
        "FILM", "SHOW", "BEST", "TOP", "FREE", "FULL", "STAR", "PLAY",
    ]
    # Prefix tối thiểu 2 ký tự để hỗ trợ case "ON" (user yêu cầu rõ),
    # còn lại đa số prefix thực tế đều >= 3 (HTV, HTVC, BLV, CCTV...).
    MIN_PREFIX_LEN = 2

    def __init__(self, rules_path: Optional[str] = None):
        if rules_path is None:
            rules_path = os.path.join(os.path.dirname(__file__), "rules.json")

        self.rules_path = rules_path
        self.group_mappings: Dict[str, List[str]] = {}
        self.channel_aliases: Dict[str, List[str]] = {}
        self.strip_patterns: List[str] = []
        self._compiled_strips: List[re.Pattern] = []
        self.ping_thresholds: Dict[str, int] = {"good_ms": 500, "fair_ms": 1500}
        # Ngưỡng gom prefix: prefix nào có >= N kênh thì lập nhóm mới.
        self.prefix_group_min: int = 4
        self.prefix_stoplist: List[str] = list(self.DEFAULT_PREFIX_STOPLIST)
        # Nhóm bảo vệ: kênh đã thuộc các nhóm này thì pass prefix không xé ra
        # (VD Địa phương giữ kênh tỉnh, LIVE EVENTS giữ fixture bóng đá).
        self.prefix_protected_groups: List[str] = ["Địa phương", "LIVE EVENTS"]

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
        self.prefix_group_min = int(data.get("prefix_group_min", 4))
        raw_stoplist = data.get("prefix_stoplist", list(self.DEFAULT_PREFIX_STOPLIST))
        self.prefix_stoplist = [str(x).upper().strip() for x in raw_stoplist if str(x).strip()]
        self.prefix_protected_groups = data.get(
            "prefix_protected_groups", ["Địa phương", "LIVE EVENTS"])
        # Prefix có >= N kênh SỐNG mới được đôn lên nhóm chính ở file xuất
        # (mặc định 11, tức "trên 10").
        self.prefix_promote_alive_min = int(data.get("prefix_promote_alive_min", 11))

        self._compiled_strips = [
            re.compile(p, re.IGNORECASE) for p in self.strip_patterns
        ]

    def save_rules(self):
        data = {
            "group_mappings": self.group_mappings,
            "channel_aliases": self.channel_aliases,
            "strip_patterns": self.strip_patterns,
            "ping_thresholds": self.ping_thresholds,
            "prefix_group_min": self.prefix_group_min,
            "prefix_stoplist": self.prefix_stoplist,
            "prefix_protected_groups": self.prefix_protected_groups,
            "prefix_promote_alive_min": self.prefix_promote_alive_min
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

    def extract_prefix(self, channel_name: str) -> Optional[str]:
        """Tách prefix nhóm từ tên kênh: 2+ ký tự chữ ASCII đầu tên (uppercase).

        - Bỏ số thứ tự đầu ("001: VTV1", "12 - HTV7").
        - Bỏ ký tự rác đầu (emoji, ngoặc, dấu).
        - Chỉ lấy run A-Z đầu (HTV1 -> HTV, "HTVC Thuần Việt" -> HTVC,
          "ON VIE" -> ON). Tên bắt đầu bằng số/tiếng Việt có dấu -> None.
        - Trả None nếu prefix ngắn hơn MIN_PREFIX_LEN hoặc nằm trong stop-list.
        """
        if not channel_name:
            return None
        s = unicodedata.normalize("NFC", channel_name).strip()
        # Bỏ số thứ tự đầu, lặp để xử lý "001: ..." nhiều lớp
        for _ in range(3):
            ns = re.sub(r'^\d+\s*[:.\-–—]\s*', '', s).strip()
            if ns == s:
                break
            s = ns
        # Bỏ ký tự rác đầu (emoji, ngoặc, +, số thứ tự còn sót...): bắt đầu từ
        # chữ cái Unicode đầu tiên (giữ nguyên chữ có dấu để luật token bên
        # dưới loại đúng: "ĐỒNG NAI" giữ "Đ" -> không khớp A-Z -> None,
        # chứ không cắt thành "NG" rác).
        m0 = re.search(r'[^\W\d_]', s)
        if not m0:
            return None
        s = s[m0.start():].strip()
        if not s:
            return None
        m = re.match(r'([A-Za-z]{2,})', s)
        if not m:
            return None
        prefix = unicodedata.normalize("NFC", m.group(1)).upper()
        # Chỉ nhận ASCII A-Z (tránh tạo nhóm rác từ chữ có dấu như "TRUY")
        if not re.fullmatch(r'[A-Z]{2,}', prefix):
            return None
        # Prefix phải là token hoàn chỉnh: ký tự ngay sau nó KHÔNG được là
        # chữ (kể cả chữ có dấu). VD "Thám Tử" -> "TH"+ắ (còn chữ) -> loại;
        # "ON Cine" -> "ON"+space -> nhận; "HTV1" -> "HTV"+số -> nhận;
        # "THĐT1" -> "TH"+Đ -> loại. Chặn nhóm rác TH/CU từ tiếng Việt.
        rest = s[m.end():]
        if rest and rest[0].isalpha():
            return None
        if len(prefix) < self.MIN_PREFIX_LEN:
            return None
        if prefix in (x.upper() for x in self.prefix_stoplist):
            return None
        return prefix

    def canonicalize_channel(self, cleaned_name: str) -> str:
        """Ánh xạ tên kênh đã làm sạch vào tên chuẩn trong từ điển"""
        norm_key = unicodedata.normalize("NFC", cleaned_name).lower().replace(" ", "").replace("-", "").replace("_", "")

        for canonical_name, aliases in self.channel_aliases.items():
            for alias in aliases:
                alias_norm = unicodedata.normalize("NFC", alias).lower().replace(" ", "").replace("-", "").replace("_", "")
                if norm_key == alias_norm:
                    return canonical_name

        # Nếu không có trong từ điển, viết hoa chữ cái đầu đẹp mắt
        return cleaned_name

    def canonicalize_group(self, raw_group: str, canonical_ch_name: str = "") -> str:
        """Chuẩn hóa tên nhóm theo danh mục chuẩn và logic series kênh"""
        # Chuẩn hóa Unicode NFC trước mọi so sánh (file M3U lẫn lộn
        # tổ hợp/phựng sẵn: "CA NHẠC" != "CA NHẠC" nếu không NFC).
        norm_group = unicodedata.normalize(
            "NFC", raw_group.strip(' \t\n\r"\''))
        # Loại bỏ icon
        norm_group = re.sub(r'[⚽🔥📺⚡⭐♦️🔴|]+', '', norm_group).strip(' \t\n\r"\'')
        norm_group = unicodedata.normalize("NFC", norm_group)

        # Chuẩn hóa tên kênh để so khớp: bỏ số thứ tự đầu ("001: VTV1"),
        # gạch nối/gạch dưới thành cách ("VIETNAM-TODAY", "HTV_THETHAO").
        ch_key = re.sub(r'^\d+\s*[:.\-–—]\s*', '', canonical_ch_name.strip())
        ch_key = re.sub(r'[_‑–—-]', ' ', ch_key)
        ch_key = re.sub(r'\s+', ' ', ch_key).strip()
        ch_upper = unicodedata.normalize("NFC", ch_key).upper().strip()
        grp_upper = norm_group.upper().strip()

        # 1. SERIES ON & VTVcab -> Đưa chung vào VTV / Quốc Gia
        # (tầng chính luôn giữ ON ở đây; ON chỉ hiện nhóm riêng ở tầng 2
        # / file xuất khi đủ >10 kênh sống - xem CuratorEngine.compute_promotion).
        if (ch_upper.startswith("ON ") or ch_upper.startswith("ON-") or ch_upper == "ON" or
            grp_upper in ["ON", "VTVCAB", "VTV CAB", "VTVPRIME", "VTV PRIME", "KÊNH ON"] or
            any(k in ch_upper for k in ["VTVCAB", "VTV CAB", "VTVPRIME", "VTV PRIME"])):
            return "VTV / Quốc Gia"

        # 2. Logic cũ: Nếu raw_group khớp với danh mục chuẩn trong group_mappings
        # - Pass 1 (khớp tuyệt đối): alias == group, mọi độ dài.
        # - Pass 2 (chứa nhau): alias >= 3 ký tự (alias ngắn như "ON"
        #   ăn bừa "COMPETITION", "OPINION" -> sai nhóm).
        if norm_group:
            up = grp_upper
            hit = None
            for canonical_grp, aliases in self.group_mappings.items():
                for alias in aliases:
                    if alias.upper() == up:
                        hit = canonical_grp
                        break
                if hit:
                    break
            if hit:
                return hit
            for canonical_grp, aliases in self.group_mappings.items():
                for alias in aliases:
                    a = unicodedata.normalize("NFC", alias).upper()
                    if len(a.strip()) < 3:
                        continue
                    if a in up or up in a:
                        return canonical_grp

        # 3. Phân loại theo series kênh (khi group rỗng hoặc không khớp nhóm chuẩn)
        # Series VTV
        if ch_upper.startswith("VTV") or any(k in ch_upper for k in ["VIETNAM TODAY", "TRUYỀN HÌNH QUỐC HỘI", "ANTV", "AN NINH TV", "NHÂN DÂN", "QUỐC PHÒNG", "QPVN"]):
            return "VTV / Quốc Gia"

        # Series HTV
        if ch_upper.startswith("HTV") or ch_upper.startswith("HTVC"):
            if any(k in ch_upper for k in ["THỂ THAO", "THETHAO", "SPORT"]):
                return "Thể Thao"
            return "HTV / TP.HCM"

        # Series SCTV
        if ch_upper.startswith("SCTV"):
            return "SCTV"

        # Series VTC & Tin tức
        if ch_upper.startswith("VTC") or any(k in ch_upper for k in ["THỜI SỰ", "INFO TV", "FBNC", "TIN TỨC", "NEWS", "CNA"]):
            return "Tin Tức"

        # Series THVL & Đài Tỉnh / Địa Phương (so cả dạng không dấu:
        # "Can Tho TV" vẫn bắt được như "CẦN THƠ").
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
            "VPPT", "PTN", "CBTV", "TNV", "BGTV", "DAKLAK"
        ]
        ch_fold = "".join(
            c for c in unicodedata.normalize("NFD", ch_upper)
            if unicodedata.category(c) != "Mn").replace("Đ", "D")
        if any(lp in ch_upper for lp in local_prefixes) or \
                any("".join(c for c in unicodedata.normalize("NFD", lp)
                            if unicodedata.category(c) != "Mn").replace(
                                "Đ", "D") in ch_fold
                    for lp in local_prefixes) or \
                re.search(r"\bRTV\b", ch_fold) or \
                any(k in grp_upper for k in ["ĐỊA PHƯƠNG", "TỈNH", "THVL", "PROVINCE"]):
            return "Địa phương"

        # Series K+ & Thể thao
        sports_keywords = [
            "K+", "K PLUS", "CANAL", "SPORT", "SPORTS", "BÓNG ĐÁ", "BONGDA",
            "FOOTBALL", "TENNIS", "GOLF", "F1", "BEIN", "SPOTV", "NBA", "MUTV", "EUROSPORT", "UFC"
        ]
        if any(k in ch_upper for k in sports_keywords) or any(k in grp_upper for k in ["THỂ THAO", "SPORTS", "FOOTBALL", "BÓNG ĐÁ"]):
            return "Thể Thao"

        # Series Phim & Giải trí
        ent_keywords = [
            "HBO", "CINEMA", "CINEMAX", "WARNER", "AXN", "DISNEY", "CARTOON",
            "ANIMAX", "DREAMWORKS", "PHIM", "GIẢI TRÍ", "ENTERTAINMENT", "MOVIES", "CINE", "KIX"
        ]
        if any(k in ch_upper for k in ent_keywords) or any(k in grp_upper for k in ["PHIM", "CINEMA", "GIẢI TRÍ", "ENTERTAINMENT"]):
            return "Phim & Giải Trí"

        # Series Thiếu Nhi (sau Phim để CARTOON/DISNEY vẫn về Phim)
        if any(k in ch_upper for k in ["KIDS", "THIẾU NHI", "HOẠT HÌNH", "ANIME", "BABY", "BOOMERANG", "DA VINCI"]) or any(k in grp_upper for k in ["THIẾU NHI", "HOẠT HÌNH", "KIDS", "ANIME"]):
            return "Thiếu Nhi"

        # Series Âm Nhạc
        if any(k in ch_upper for k in ["MTV", "MUSIC", "ÂM NHẠC", "CA NHẠC", "AM NHAC"]) or any(k in grp_upper for k in ["ÂM NHẠC", "CA NHẠC", "MUSIC", "MTV", "AM NHAC"]):
            return "Âm Nhạc"

        # Series Quốc tế
        intl_keywords = [
            "DISCOVERY", "NATIONAL", "NAT GEO", "CNN", "BBC", "NHK", "CGTN",
            "DW", "TV5MONDE", "KBS", "ARIRANG", "AL JAZEERA", "BLOOMBERG", "ABC", "CBS", "NBC", "TVB",
            "AFN", "FRANCE", "TV5", "C+", "ARRIANG"
        ]
        if any(k in ch_upper for k in intl_keywords) or any(k in grp_upper for k in ["QUỐC TẾ", "INTERNATIONAL", "KÊNH NƯỚC NGOÀI"]):
            return "Quốc Tế"

        # Series Khám Phá (sau Quốc Tế để DISCOVERY vẫn về Quốc Tế)
        if any(k in ch_upper for k in ["HISTORY", "SCIENCE", "ANIMALS", "NATURE", "TRUE CRIME", "DOCUMENTARY", "KHÁM PHÁ", "TÀI LIỆU", "ANIMAL PLANET", "OUTDOOR", "TLC"]) or any(k in grp_upper for k in ["KHÁM PHÁ", "TÀI LIỆU", "DOCUMENTARY", "HISTORY", "TRUE CRIME"]):
            return "Khám Phá"

        # Series Radio
        if any(k in ch_upper for k in ["VOV", "VOH", "RADIO", " FM", "FM ", "CRI", "SPUTNIK", "WRN", "РАДИО", "МАЯК"]) or "RADIO" in grp_upper:
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
