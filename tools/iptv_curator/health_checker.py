#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Health Checker - Kiểm tra trạng thái sống/chết (Health Probe) và đo độ trễ (Ping ms).
Chạy đa luồng song song (ThreadPoolExecutor), kiểm tra header/chunk đầu mà không tải cả luồng video.
"""

import time
import urllib.request
import urllib.error
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Dict, Any, List, Callable, Optional


class ProbeResult:
    def __init__(self, url: str, is_alive: bool, latency_ms: int, status_code: int = 0, content_type: str = "", error: str = ""):
        self.url = url
        self.is_alive = is_alive
        self.latency_ms = latency_ms
        self.status_code = status_code
        self.content_type = content_type
        self.error = error

    def to_dict(self) -> Dict[str, Any]:
        return {
            "url": self.url,
            "is_alive": self.is_alive,
            "latency_ms": self.latency_ms,
            "status_code": self.status_code,
            "content_type": self.content_type,
            "error": self.error
        }


class HealthChecker:
    @staticmethod
    def probe_single_stream(url: str, timeout: int = 3, extra_headers: Optional[Dict[str, str]] = None) -> ProbeResult:
        """Kiểm tra 1 link stream với request byte range nhỏ (0-512) để đo độ trễ và trạng thái HTTP"""
        headers = {
            "User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) VLC/3.0.18",
            "Range": "bytes=0-512"
        }
        if extra_headers:
            headers.update(extra_headers)

        req = urllib.request.Request(url, headers=headers)
        t0 = time.time()

        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                elapsed_ms = int((time.time() - t0) * 1000)
                code = resp.getcode()
                c_type = (resp.headers.get_content_type() or "").lower()

                # Đọc tối đa 512 bytes đầu tiên để nhận diện chuẩn video
                chunk = resp.read(512)
                is_m3u8 = b"#EXTM3U" in chunk or b"#EXTINF" in chunk
                is_ts = len(chunk) > 0 and chunk[0] == 0x47  # Sync byte 0x47 của MPEG-TS
                is_valid_type = ("text/html" not in c_type) or is_m3u8 or is_ts

                # Kiểm tra hợp lệ: mã HTTP 200/206/302 và không phải trang web lỗi HTML
                is_valid = (code in (200, 206, 302)) and is_valid_type and (len(chunk) > 0 or code == 200)

                return ProbeResult(
                    url=url,
                    is_alive=is_valid,
                    latency_ms=elapsed_ms,
                    status_code=code,
                    content_type=c_type,
                    error="" if is_valid else "Not a media stream or HTML error page"
                )

        except urllib.error.HTTPError as e:
            elapsed_ms = int((time.time() - t0) * 1000)
            return ProbeResult(
                url=url,
                is_alive=False,
                latency_ms=elapsed_ms,
                status_code=e.code,
                error=f"HTTP {e.code}"
            )
        except urllib.error.URLError as e:
            elapsed_ms = int((time.time() - t0) * 1000)
            err_str = str(e.reason)
            if "timed out" in err_str.lower():
                err_str = "Timeout"
            return ProbeResult(
                url=url,
                is_alive=False,
                latency_ms=elapsed_ms,
                status_code=0,
                error=err_str
            )
        except Exception as e:
            elapsed_ms = int((time.time() - t0) * 1000)
            return ProbeResult(
                url=url,
                is_alive=False,
                latency_ms=elapsed_ms,
                status_code=0,
                error=str(e)[:50]
            )

    @staticmethod
    def probe_batch(urls: List[str], max_workers: int = 25, timeout: int = 3, progress_callback: Optional[Callable[[int, int, ProbeResult], None]] = None) -> Dict[str, ProbeResult]:
        """Quét đa luồng danh sách stream URLs và trả về kết quả theo dict[url] = ProbeResult"""
        results: Dict[str, ProbeResult] = {}
        total = len(urls)
        completed = 0

        with ThreadPoolExecutor(max_workers=max_workers) as executor:
            future_to_url = {
                executor.submit(HealthChecker.probe_single_stream, url, timeout): url
                for url in urls
            }

            for future in as_completed(future_to_url):
                url = future_to_url[future]
                try:
                    res = future.result()
                except Exception as e:
                    res = ProbeResult(url=url, is_alive=False, latency_ms=timeout * 1000, error=str(e))

                results[url] = res
                completed += 1

                if progress_callback:
                    progress_callback(completed, total, res)

        return results
