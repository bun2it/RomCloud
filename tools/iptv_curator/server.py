#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
IPTV Curator Localhost Server - Máy chủ Web UI cục bộ.
Sử dụng thư viện chuẩn của Python (không cần pip install), cung cấp REST API và giao diện quản trị.
"""

import os
import sys
import json
import time
import urllib.parse
from http.server import HTTPServer, BaseHTTPRequestHandler
import threading
from typing import Dict, Any

# Nạp các module lõi từ thư mục hiện tại
CUR_DIR = os.path.dirname(os.path.abspath(__file__))
if CUR_DIR not in sys.path:
    sys.path.insert(0, CUR_DIR)

from curator_engine import CuratorEngine

PORT = 8080
WEB_DIR = os.path.join(CUR_DIR, "web")
REPO_ROOT = os.path.abspath(os.path.join(CUR_DIR, "..", ".."))

# Khởi tạo engine toàn cục
engine = CuratorEngine(work_dir=CUR_DIR)

# Trạng thái tiến trình Health Check
probe_state = {
    "is_running": False,
    "completed": 0,
    "total": 0,
    "pct": 0.0,
    "alive_count": 0,
    "dead_count": 0,
    "last_result": {}
}


class CuratorRequestHandler(BaseHTTPRequestHandler):
    def end_headers(self):
        # Hỗ trợ CORS nếu mở từ tool khác
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(200)
        self.end_headers()

    def send_json(self, data: Any, status: int = 200):
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def read_json_body(self) -> Dict[str, Any]:
        content_length = int(self.headers.get("Content-Length", 0))
        if content_length > 0:
            raw = self.rfile.read(content_length).decode("utf-8", errors="replace")
            try:
                return json.loads(raw)
            except Exception:
                return {}
        return {}

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

        # 1. API: Thống kê tổng quan
        if path == "/api/stats":
            db_stats = engine.db.get_stats()
            cluster_stats = {
                "total_raw": len(engine.raw_channels),
                "distinct_channels": len(engine.clustered_channels)
            }
            self.send_json({
                "sources_count": len(engine.raw_channels),
                "cluster": cluster_stats,
                "db": db_stats,
                "probe": probe_state
            })
            return

        # 2. API: Lấy danh sách kênh đã gom nhóm
        if path == "/api/channels":
            group_filter = query.get("group", [""])[0]
            search_query = query.get("q", [""])[0].lower()

            results = []
            for ch_name, streams in engine.clustered_channels.items():
                if not streams:
                    continue
                first = streams[0]
                grp = first["canonical_group"]

                if group_filter and group_filter != "Tất cả" and grp != group_filter:
                    continue
                if search_query and (search_query not in ch_name.lower()) and (search_query not in grp.lower()):
                    continue

                db_streams = engine.db.get_channel_streams(ch_name)
                stream_list = []
                for s in streams:
                    u = s["url"]
                    db_item = next((r for r in db_streams if r["url"] == u), None)
                    stream_list.append({
                        "url": u,
                        "logo": s["logo"],
                        "source": s["source_file"],
                        "last_status": db_item["last_status"] if db_item else "UNKNOWN",
                        "latency_ms": db_item["last_latency_ms"] if db_item else 0,
                        "score": db_item["reputation_score"] if db_item else 50
                    })

                # Sắp xếp stream: ALIVE lên trước, ping thấp lên trước
                stream_list.sort(key=lambda x: (
                    0 if x["last_status"] == "ALIVE" else (1 if x["last_status"] == "STANDBY" else 2),
                    x["latency_ms"] if x["last_status"] == "ALIVE" else 9999,
                    -x["score"]
                ))

                results.append({
                    "name": ch_name,
                    "group": grp,
                    "logo": first["logo"],
                    "stream_count": len(stream_list),
                    "best_stream": stream_list[0] if stream_list else None,
                    "streams": stream_list
                })

            # Sắp xếp theo nhóm và tên kênh
            results.sort(key=lambda x: (x["group"], x["name"]))
            self.send_json({"channels": results, "total": len(results)})
            return

        # 3. API: Tiến độ quét Ping & Health Check
        if path == "/api/health_progress":
            self.send_json(probe_state)
            return

        # 4. API: Lấy danh sách nhóm
        if path == "/api/groups":
            groups = set()
            for ch_name, streams in engine.clustered_channels.items():
                if streams:
                    groups.add(streams[0]["canonical_group"])
            sorted_groups = sorted(list(groups))
            self.send_json({"groups": ["Tất cả"] + sorted_groups})
            return

        # 5. API: Lấy cấu hình Rules
        if path == "/api/rules":
            self.send_json({
                "group_mappings": engine.normalizer.group_mappings,
                "channel_aliases": engine.normalizer.channel_aliases
            })
            return

        # 6. Tải file xuất bản (Download)
        if path == "/download/live.m3u":
            out_file = os.path.join(CUR_DIR, "output", "live.m3u")
            if os.path.exists(out_file):
                self.serve_file(out_file, "application/x-mpegurl")
            else:
                self.send_error(404, "Not Found: live.m3u not yet exported")
            return

        if path == "/download/manifest.json":
            out_file = os.path.join(CUR_DIR, "output", "iptv_manifest.json")
            if os.path.exists(out_file):
                self.serve_file(out_file, "application/json")
        # 7. API: Stream CORS Proxy (dành cho các luồng bị chặn CORS trên trình duyệt)
        if path == "/api/proxy_stream":
            target_url = query.get("url", [""])[0]
            if not target_url:
                self.send_error(400, "Bad Request: Missing url parameter")
                return
            try:
                import ssl
                ctx = ssl._create_unverified_context()
                headers = {
                    "User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
                }
                req = urllib.request.Request(target_url, headers=headers)
                with urllib.request.urlopen(req, timeout=8, context=ctx) as resp:
                    data = resp.read()
                    content_type = resp.headers.get_content_type() or "application/vnd.apple.mpegurl"

                    # Nếu là m3u8 playlist, rewrite relative URLs để đi qua proxy
                    if "mpegurl" in content_type or target_url.endswith(".m3u8") or b"#EXTM3U" in data:
                        try:
                            text = data.decode("utf-8", errors="replace")
                            base_url = target_url.rsplit("/", 1)[0] + "/"
                            lines = text.splitlines()
                            new_lines = []
                            for line in lines:
                                line_str = line.strip()
                                if line_str and not line_str.startswith("#"):
                                    if not line_str.startswith("http://") and not line_str.startswith("https://"):
                                        abs_chunk_url = urllib.parse.urljoin(base_url, line_str)
                                    else:
                                        abs_chunk_url = line_str
                                    new_lines.append(f"/api/proxy_stream?url={urllib.parse.quote(abs_chunk_url)}")
                                else:
                                    new_lines.append(line)
                            data = "\n".join(new_lines).encode("utf-8")
                            content_type = "application/vnd.apple.mpegurl"
                        except Exception:
                            pass

                    self.send_response(200)
                    self.send_header("Content-Type", content_type)
                    self.send_header("Access-Control-Allow-Origin", "*")
                    self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
            except Exception as e:
                self.send_error(502, f"Proxy error: {e}")
            return

        # 8. Serve Static Files (HTML / CSS / JS)
        if path == "/" or path == "/index.html":
            file_path = os.path.join(WEB_DIR, "index.html")
            self.serve_file(file_path, "text/html; charset=utf-8")
            return

        # Static assets
        safe_path = os.path.normpath(path.lstrip("/"))
        file_path = os.path.join(WEB_DIR, safe_path)
        if os.path.exists(file_path) and os.path.isfile(file_path):
            content_type = "text/plain"
            if file_path.endswith(".css"):
                content_type = "text/css; charset=utf-8"
            elif file_path.endswith(".js"):
                content_type = "application/javascript; charset=utf-8"
            elif file_path.endswith(".png"):
                content_type = "image/png"
            elif file_path.endswith(".svg"):
                content_type = "image/svg+xml"
            self.serve_file(file_path, content_type)
            return

        self.send_error(404, "Not Found")

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        # 1. API: Nạp nguồn mặc định từ repo (iptv/default.m3u)
        if path == "/api/sources/load_default":
            default_path = os.path.join(REPO_ROOT, "iptv", "default.m3u")
            if os.path.exists(default_path):
                cnt = engine.add_source_file(default_path)
                engine.process_and_cluster()
                self.send_json({"success": True, "loaded_channels": cnt})
            else:
                self.send_json({"success": False, "error": "Không tìm thấy iptv/default.m3u"}, status=404)
            return

        # 2. API: Thêm link M3U từ xa
        if path == "/api/sources/add_url":
            body = self.read_json_body()
            url = body.get("url", "").strip()
            if not url:
                self.send_json({"success": False, "error": "URL không được để trống"}, status=400)
                return
            try:
                cnt = engine.add_source_url(url)
                engine.process_and_cluster()
                self.send_json({"success": True, "added_channels": cnt})
            except Exception as e:
                self.send_json({"success": False, "error": str(e)}, status=500)
            return

        # 3. API: Tải lên nội dung M3U trực tiếp
        if path == "/api/sources/upload_text":
            body = self.read_json_body()
            content = body.get("content", "")
            filename = body.get("filename", "upload.m3u")
            if not content:
                self.send_json({"success": False, "error": "Nội dung rỗng"}, status=400)
                return
            cnt = engine.add_source_content(content, source_name=filename)
            engine.process_and_cluster()
            self.send_json({"success": True, "added_channels": cnt})
            return

        # 4. API: Xóa toàn bộ nguồn đã nạp để làm mới
        if path == "/api/sources/clear":
            engine.clear_sources()
            self.send_json({"success": True})
            return

        # 5. API: Kích hoạt quét Health Check (đa luồng ngầm)
        if path == "/api/health_check":
            global probe_state
            if probe_state["is_running"]:
                self.send_json({"success": False, "error": "Đang có tiến trình quét đang chạy"}, status=400)
                return

            def run_probe_worker():
                global probe_state
                probe_state["is_running"] = True
                probe_state["completed"] = 0
                probe_state["total"] = 0
                probe_state["pct"] = 0.0
                probe_state["alive_count"] = 0
                probe_state["dead_count"] = 0

                def progress_cb(completed, total, res):
                    probe_state["completed"] = completed
                    probe_state["total"] = total
                    probe_state["pct"] = round((completed / total) * 100, 1) if total > 0 else 100
                    if res.is_alive:
                        probe_state["alive_count"] += 1
                    else:
                        probe_state["dead_count"] += 1

                try:
                    results = engine.check_health(max_workers=30, timeout=3, progress_callback=progress_cb)
                    probe_state["last_result"] = {k: v.to_dict() for k, v in results.items()}
                except Exception as e:
                    print(f"Lỗi khi quét health check: {e}")
                finally:
                    probe_state["is_running"] = False

            t = threading.Thread(target=run_probe_worker, daemon=True)
            t.start()
            self.send_json({"success": True, "message": "Bắt đầu quét đa luồng ngầm"})
            return

        # 6. API: Xuất bản file Live M3U & Manifest
        if path == "/api/export":
            body = self.read_json_body()
            include_backup = body.get("include_backup", True)
            res = engine.export(include_backup=include_backup)
            self.send_json({"success": True, "result": res})
            return

        # 7. API: Đẩy trực tiếp vào thư mục RomCloud iptv/ (OTA deploy cục bộ)
        if path == "/api/publish_ota":
            dest_dir = os.path.join(REPO_ROOT, "iptv")
            res = engine.export(output_dir=dest_dir, include_backup=True)
            self.send_json({
                "success": True,
                "message": f"Đã xuất bản thành công vào thư mục {dest_dir}",
                "result": res
            })
            return

        # 8. API: Cập nhật Rules
        if path == "/api/rules":
            body = self.read_json_body()
            if "group_mappings" in body:
                engine.normalizer.group_mappings = body["group_mappings"]
            if "channel_aliases" in body:
                engine.normalizer.channel_aliases = body["channel_aliases"]
            engine.normalizer.save_rules()
            engine.process_and_cluster()
            self.send_json({"success": True})
            return

        # 9. API: Test Ping thử 1 luồng đơn lẻ
        if path == "/api/stream/probe":
            body = self.read_json_body()
            url = body.get("url", "").strip()
            if not url:
                self.send_json({"success": False, "error": "URL trống"}, status=400)
                return
            from health_checker import HealthChecker
            res = HealthChecker.probe_single_stream(url, timeout=3)
            ch_name = body.get("channel_name", "")
            grp = body.get("group_name", "")
            engine.db.record_probe(res, channel_name=ch_name, group_name=grp)
            self.send_json({"success": True, "probe": res.to_dict()})
            return

        # 10. API: Đặt luồng làm Nguồn ưu tiên số 1 thủ công
        if path == "/api/channels/set_primary":
            body = self.read_json_body()
            ch_name = body.get("channel_name", "").strip()
            url = body.get("url", "").strip()
            if not ch_name or not url or ch_name not in engine.clustered_channels:
                self.send_json({"success": False, "error": "Kênh hoặc URL không hợp lệ"}, status=400)
                return
            streams = engine.clustered_channels[ch_name]
            target_idx = next((i for i, s in enumerate(streams) if s["url"] == url), None)
            if target_idx is not None and target_idx > 0:
                item = streams.pop(target_idx)
                streams.insert(0, item)
        # 11. API: Kích hoạt ứng dụng VLC trên máy tính để phát stream trực tiếp
        if path == "/api/open_vlc":
            body = self.read_json_body()
            url = body.get("url", "").strip()
            if not url:
                self.send_json({"success": False, "error": "Thiếu URL stream"}, status=400)
                return
            import subprocess
            import platform
            try:
                sys_plat = platform.system()
                if sys_plat == "Darwin":
                    # macOS: chạy thẳng binary VLC nếu có, hoặc dùng open -a
                    vlc_bin = "/Applications/VLC.app/Contents/MacOS/VLC"
                    if os.path.exists(vlc_bin):
                        subprocess.Popen([vlc_bin, url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    else:
                        subprocess.Popen(["open", "-a", "VLC", url])
                elif sys_plat == "Windows":
                    # Windows: chạy vlc
                    subprocess.Popen(["cmd", "/c", "start", "vlc", url], shell=True)
                else:
                    # Linux
                    subprocess.Popen(["vlc", url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                self.send_json({"success": True, "message": "Đã khởi chạy VLC thành công"})
            except Exception as e:
                self.send_json({"success": False, "error": f"Lỗi khởi chạy VLC: {e}"}, status=500)
            return

        self.send_error(404, "Not Found: Invalid POST endpoint")

    def serve_file(self, file_path: str, content_type: str):
        try:
            with open(file_path, "rb") as f:
                data = f.read()
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except Exception as e:
            self.send_error(500, f"Internal Server Error: {e}")


def run_server(port: int = PORT):
    # Tự động nạp default.m3u ban đầu nếu có
    default_m3u = os.path.join(REPO_ROOT, "iptv", "default.m3u")
    if os.path.exists(default_m3u):
        cnt = engine.add_source_file(default_m3u)
        engine.process_and_cluster()
        print(f"[*] Tự động nạp {cnt} kênh từ iptv/default.m3u")

    server_address = ("", port)
    httpd = HTTPServer(server_address, CuratorRequestHandler)
    print(f"=======================================================")
    print(f"🚀 RomCloud IPTV Curator Web UI đang chạy tại:")
    print(f"👉 http://localhost:{port}")
    print(f"=======================================================")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[*] Đang tắt server...")
        httpd.server_close()


if __name__ == "__main__":
    p = PORT
    if len(sys.argv) > 1 and sys.argv[1].isdigit():
        p = int(sys.argv[1])
    run_server(p)
