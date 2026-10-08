#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
M3U Parser - Đọc và phân tích file / link M3U / M3U8 đa nguồn.
Hỗ trợ giải mã thuộc tính tvg-id, tvg-name, tvg-logo, group-title và link stream.
"""

import re
import urllib.request
import urllib.error
from typing import List, Dict, Any, Optional


class RawChannel:
    def __init__(self, name: str, url: str, group: str = "", logo: str = "", tvg_id: str = "", tvg_name: str = "", source_file: str = ""):
        self.name = name.strip()
        self.url = url.strip()
        self.group = group.strip()
        self.logo = logo.strip()
        self.tvg_id = tvg_id.strip()
        self.tvg_name = tvg_name.strip()
        self.source_file = source_file
        self.extra_headers: Dict[str, str] = {}

    def to_dict(self) -> Dict[str, Any]:
        return {
            "name": self.name,
            "url": self.url,
            "group": self.group,
            "logo": self.logo,
            "tvg_id": self.tvg_id,
            "tvg_name": self.tvg_name,
            "source_file": self.source_file,
            "extra_headers": self.extra_headers
        }


class M3UParser:
    @staticmethod
    def fetch_url(url: str, timeout: int = 10) -> str:
        """Tải nội dung M3U từ URL qua HTTP/HTTPS"""
        req = urllib.request.Request(
            url,
            headers={
                "User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) RomCloud/2.4"
            }
        )
        with urllib.request.urlopen(req, timeout=timeout) as response:
            charset = response.headers.get_content_charset() or "utf-8"
            return response.read().decode(charset, errors="replace")

    @staticmethod
    def parse_file(file_path: str) -> List[RawChannel]:
        """Đọc và parse file M3U cục bộ"""
        with open(file_path, "r", encoding="utf-8", errors="replace") as f:
            content = f.read()
        return M3UParser.parse_content(content, source_file=file_path)

    @staticmethod
    def parse_url(url: str, timeout: int = 10) -> List[RawChannel]:
        """Tải từ internet và parse file M3U"""
        content = M3UParser.fetch_url(url, timeout=timeout)
        return M3UParser.parse_content(content, source_file=url)

    @staticmethod
    def parse_content(content: str, source_file: str = "") -> List[RawChannel]:
        """Parse chuỗi text định dạng M3U"""
        lines = [line.strip() for line in content.splitlines()]
        channels: List[RawChannel] = []

        curr_extinf: Optional[str] = None
        curr_headers: Dict[str, str] = {}

        # Regex tìm thuộc tính trong #EXTINF (key="value" hoặc key=value)
        attr_regex = re.compile(r'([a-zA-Z0-9_-]+)=(?:"([^"]*)"|([^,\s]+))')

        for line in lines:
            if not line:
                continue

            if line.startswith("#EXTM3U"):
                continue

            if line.startswith("#EXTINF:"):
                curr_extinf = line
                curr_headers = {}
                continue

            if line.startswith("#EXTVLCOPT:") or line.startswith("#EXTHTTP:"):
                # Header bổ sung nếu có (ví dụ user-agent hoặc referrer)
                opt = line.split(":", 1)[1].strip()
                if "=" in opt:
                    k, v = opt.split("=", 1)
                    curr_headers[k.strip().lower()] = v.strip()
                continue

            if line.startswith("#"):
                # Bỏ qua các directive khác không rõ
                continue

            # Đây là dòng URL stream
            if curr_extinf:
                # Phân tích #EXTINF
                # Tách phần metadata trước dấu phẩy cuối cùng và tên kênh sau dấu phẩy
                comma_idx = curr_extinf.rfind(",")
                if comma_idx != -1:
                    meta_part = curr_extinf[:comma_idx]
                    ch_name = curr_extinf[comma_idx + 1:].strip()
                else:
                    meta_part = curr_extinf
                    ch_name = "Untitled"

                attrs = {}
                for match in attr_regex.finditer(meta_part):
                    key = match.group(1).lower()
                    val = match.group(2) if match.group(2) is not None else match.group(3)
                    attrs[key] = val.strip()

                group_title = attrs.get("group-title", "")
                tvg_logo = attrs.get("tvg-logo", "")
                tvg_id = attrs.get("tvg-id", "")
                tvg_name = attrs.get("tvg-name", "")

                channel = RawChannel(
                    name=ch_name if ch_name else tvg_name,
                    url=line,
                    group=group_title,
                    logo=tvg_logo,
                    tvg_id=tvg_id,
                    tvg_name=tvg_name,
                    source_file=source_file
                )
                channel.extra_headers = curr_headers
                channels.append(channel)

                curr_extinf = None
                curr_headers = {}
            else:
                # Dòng URL không có #EXTINF đi trước
                if line.startswith("http://") or line.startswith("https://") or line.startswith("rtmp://") or line.startswith("rtsp://"):
                    channels.append(RawChannel(
                        name="Stream " + str(len(channels) + 1),
                        url=line,
                        source_file=source_file
                    ))

        return channels
