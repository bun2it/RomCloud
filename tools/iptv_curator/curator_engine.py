#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Curator Engine - Bộ điều phối trung tâm của Phase 1.
Kết nối Parser -> Normalizer -> HealthChecker -> HistoryDB -> Exporter thành quy trình hoàn chỉnh.
"""

import os
from typing import List, Dict, Any, Optional, Callable
from m3u_parser import M3UParser, RawChannel
from normalizer import Normalizer, is_betting_channel
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
        self.prefix_buckets: Dict[str, List[str]] = {}

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
        Cuối cùng gắn tag prefix tầng 2 cho mọi kênh (không đổi nhóm chuẩn
        tầng 1). Việc đôn prefix lên nhóm chính xét sau, theo kênh SỐNG,
        lúc xuất file (compute_promotion).
        """
        self.clustered_channels.clear()
        self.prefix_buckets = {}
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
                    "prefix_sub": None,
                    "url": ch.url,
                    "logo": ch.logo,
                    "tvg_id": ch.tvg_id,
                    "source_file": ch.source_file
                })
                unique_urls.add(ch.url)

        prefix_info = self._tag_prefix_groups()

        return {
            "total_raw": len(self.raw_channels),
            "distinct_channels": len(self.clustered_channels),
            "unique_stream_urls": len(unique_urls),
            "prefix_buckets": prefix_info.get("buckets", {}),
            "prefix_tagged_channels": prefix_info.get("tagged_channels", 0),
        }

    # Thứ tự tầng 1 cố định: 17 nhóm chuẩn theo rules + Kênh Chung cuối.
    FALLBACK_GROUP = "Kênh Chung"

    def standard_group_order(self) -> List[str]:
        order = list(self.normalizer.group_mappings.keys())
        if self.FALLBACK_GROUP not in order:
            order.append(self.FALLBACK_GROUP)
        return order

    def _tag_prefix_groups(self) -> Dict[str, Any]:
        """Gắn tag prefix tầng 2 cho mọi kênh (không đổi nhóm chuẩn tầng 1).

        - Tách prefix qua Normalizer.extract_prefix (uppercase, bỏ số TT
          "001:" và ký tự rác, stop-list THE/TV/HD/VIP/NEW/LIVE...).
        - Bỏ qua nhóm bảo vệ (Địa phương, LIVE EVENTS...): kênh tỉnh và
          fixture bóng đá ở yên, không tham gia tầng prefix.
        - Chỉ tag prefix có >= prefix_group_min kênh (chống nhiễu lẻ).
        - Không ghi rules.json: tầng 2 tính động mỗi lần gom.
        """
        norm = self.normalizer
        min_count = int(getattr(norm, "prefix_group_min", 4) or 4)
        protected = set(getattr(norm, "prefix_protected_groups", []) or [])

        bucket: Dict[str, List[str]] = {}
        for canon_name, streams in self.clustered_channels.items():
            if not streams:
                continue
            if streams[0].get("canonical_group") in protected:
                continue
            prefix = norm.extract_prefix(canon_name)
            if not prefix:
                continue
            bucket.setdefault(prefix, []).append(canon_name)

        self.prefix_buckets = {
            p: names for p, names in bucket.items() if len(names) >= min_count
        }

        tagged = 0
        for prefix, names in self.prefix_buckets.items():
            for nm in names:
                for s in self.clustered_channels.get(nm, []):
                    s["prefix_sub"] = prefix
                    tagged += 1

        return {"buckets": {k: len(v) for k, v in self.prefix_buckets.items()},
                "tagged_channels": tagged}

    def _channel_is_alive(self, ch_name: str, streams: List[Dict[str, Any]],
                          url_alive: Optional[Dict[str, bool]]) -> bool:
        """Kênh coi là SỐNG nếu còn ít nhất 1 luồng sống (probe mới nhất,
        không thì trạng thái DB, chưa có dữ liệu thì mặc định sống)."""
        db_streams = self.db.get_channel_streams(ch_name)
        for s in streams:
            url = s["url"]
            if url_alive is not None and url in url_alive:
                if url_alive[url]:
                    return True
                continue
            rec = next((r for r in db_streams if r["url"] == url), None)
            if rec:
                if rec["last_status"] == "ALIVE":
                    return True
            else:
                return True
        return False

    def compute_promotion(self, url_alive: Optional[Dict[str, bool]] = None) -> Dict[str, int]:
        """Prefix nào có > ngưỡng kênh SỐNG (mặc định trên 10, tức >= 11)
        thì được đôn lên nhóm chính ở file xuất/UI. Trả về {prefix: số kênh sống}."""
        norm = self.normalizer
        promote_min = int(getattr(norm, "prefix_promote_alive_min", 11) or 11)

        promoted: Dict[str, int] = {}
        for prefix, names in (self.prefix_buckets or {}).items():
            alive = sum(
                1 for nm in names
                if self._channel_is_alive(
                    nm, self.clustered_channels.get(nm, []), url_alive)
            )
            if alive >= promote_min:
                promoted[prefix] = alive
        return promoted

    def resolve_group(self, canon_name: str, canonical_group: str,
                      promoted: Dict[str, int]) -> str:
        """Nhóm hiển thị/xuất file: prefix được đôn thì lên nhóm chính,
        còn lại giữ 1 trong 18 nhóm tầng 1."""
        for s in self.clustered_channels.get(canon_name, []):
            sub = s.get("prefix_sub")
            if sub and sub in promoted:
                return sub
            break
        return canonical_group

    def display_group_order(self, url_alive: Optional[Dict[str, bool]] = None) -> List[str]:
        """Thứ tự 2 tầng cho UI: 18 nhóm chuẩn trước, nhóm prefix được đôn sau."""
        order = self.standard_group_order()
        promoted = self.compute_promotion(url_alive)
        for p in sorted(promoted.keys()):
            if p not in order:
                order.append(p)
        return order

    def check_health(self, max_workers: int = 30, timeout: int = 4, target_mode: str = "ALL", progress_callback: Optional[Callable[[int, int, ProbeResult], None]] = None) -> Dict[str, ProbeResult]:
        """
        Bước 2: Quét đa luồng kiểm tra độ sống/chết và đo độ trễ theo chuẩn trình duyệt (Browser-Grade Probe).
        target_mode:
          - "ALL": Quét toàn bộ link
          - "STANDBY": Chỉ quét những link đang ở trạng thái STANDBY hoặc UNKNOWN
          - "DEAD": Chỉ quét những link đang bị coi là DEAD
        """
        all_urls = []
        url_meta_map = {}

        for ch_name, stream_list in self.clustered_channels.items():
            db_streams = self.db.get_channel_streams(ch_name)
            for item in stream_list:
                u = item["url"]
                if u not in url_meta_map:
                    db_item = next((r for r in db_streams if r["url"] == u), None)
                    st = db_item["last_status"] if db_item else "UNKNOWN"

                    if target_mode == "STANDBY" and st not in ("STANDBY", "UNKNOWN"):
                        continue
                    if target_mode == "DEAD" and st != "DEAD":
                        continue

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

    def generate_live_entries(self, include_backup: bool = True, probe_results: Optional[Dict[str, ProbeResult]] = None, publish_filters: Optional[Dict[str, Any]] = None, resolve_720p: bool = True, resolve_workers: int = 10, resolve_timeout: int = 5) -> List[LiveChannelEntry]:
        """
        Bước 3: Chọn lọc luồng tốt nhất cho mỗi kênh dựa trên Ping ms và Điểm uy tín.
        Nhóm xuất file theo 2 tầng: prefix nào có > ngưỡng kênh SỐNG
        (prefix_promote_alive_min, mặc định trên 10) thì đôn lên nhóm chính,
        còn lại xuất theo 1 trong 18 nhóm chuẩn tầng 1.
        resolve_720p=True: ép sẵn mỗi URL đã chọn về variant <=720p
        (variant_resolver) rồi mới xuất — máy phát thẳng, khỏi dò lúc bấm phát.
        """
        live_entries: List[LiveChannelEntry] = []
        pending: List[Any] = []  # (best_tam, second_tam|None) cho phase 2

        url_alive = ({u: p.is_alive for u, p in probe_results.items()}
                     if probe_results else None)
        promoted = self.compute_promotion(url_alive)

        for ch_name, stream_list in self.clustered_channels.items():
            if not stream_list:
                continue

            out_group = self.resolve_group(
                ch_name, stream_list[0].get("canonical_group",
                                            self.FALLBACK_GROUP), promoted)

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
                    "group": out_group,
                    "logo": s["logo"],
                    "tvg_id": s["tvg_id"],
                    "is_alive": is_alive,
                    "latency": latency,
                    "score": score,
                    "last_status": stream_record["last_status"] if stream_record else ("ALIVE" if is_alive else "STANDBY")
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

            # Chọn link chính (tạm) + dự phòng (tạm): gom lại resolve
            # 720p hàng loạt ở dưới, rồi mới lọc/xếp cuối bằng ms tươi.
            pending.append((
                candidate_streams[0],
                candidate_streams[1]
                if include_backup and len(candidate_streams) > 1
                else None,
            ))


        # Ép sẵn 720p + đo ms tươi: resolve 1 lượt cho mọi URL tạm chọn
        # (dedupe trong resolve_many). Rewrite thành công (capped/probed)
        # thì ms = thời gian fetch đã chứng minh sống; còn lại giữ số cũ.
        # Lọc publish + vai best/backup đều dùng ms tươi này.
        self.last_resolve_stats: Dict[str, int] = {"capped": 0, "probed": 0,
                                                   "kept": 0, "failed": 0}
        mapping: Dict[str, Any] = {}
        if resolve_720p and pending:
            try:
                from variant_resolver import resolve_many, is_hls_url
                targets = []
                for _best, _second in pending:
                    if is_hls_url(_best["url"]):
                        targets.append(_best["url"])
                    if _second is not None and is_hls_url(_second["url"]):
                        targets.append(_second["url"])
                mapping = resolve_many(targets, workers=resolve_workers,
                                       timeout=resolve_timeout)
            except Exception as ex:
                print(f"[resolve-720p] bỏ qua (lỗi {ex}), giữ URL gốc.")

        def _pair_key(x):
            return (0 if x["is_alive"] else 1,
                    x["latency"] if x["is_alive"] else 9999,
                    -x["score"])

        for best, second in pending:
            cands = [best] if second is None else [best, second]
            for cand in cands:
                u = cand["url"]
                if u in mapping:
                    new_url, info, ms = mapping[u]
                    self.last_resolve_stats[info] = \
                        self.last_resolve_stats.get(info, 0) + 1
                    if info in ("capped", "probed"):
                        cand["url"] = new_url
                        cand["is_alive"] = True
                        if ms > 0:
                            cand["latency"] = ms
                        cand["last_status"] = "ALIVE"
            # Xếp lại cặp best/backup bằng ms tươi (vai trò có thể đổi)
            cands.sort(key=_pair_key)
            best = cands[0]
            second = cands[1] if len(cands) > 1 else None

            # Kiểm tra bộ lọc xuất bản (Publish Filters) do Admin thiết lập
            if publish_filters:
                # 0. Loại kênh cá độ tự động (trừ khi admin bật lại)
                if not bool(publish_filters.get("allow_betting", False)):
                    if is_betting_channel(best.get("name", "")):
                        continue
                # 1. Lọc theo nhóm kênh (Selected Groups) nếu có chỉ định
                selected_groups = publish_filters.get("selected_groups")
                if selected_groups is not None:
                    ch_grp = best.get("group") or "Khác"
                    if ch_grp not in selected_groups:
                        continue

                # 2. Lọc theo tiêu chí chất lượng đường truyền (Ping & Trạng thái)
                good_ms = int(publish_filters.get("good_threshold_ms", self.normalizer.ping_thresholds.get("good_ms", 500)))
                fair_ms = int(publish_filters.get("fair_threshold_ms", self.normalizer.ping_thresholds.get("fair_ms", 1500)))
                allow_good = bool(publish_filters.get("allow_good", True))
                allow_fair = bool(publish_filters.get("allow_fair", True))
                allow_poor = bool(publish_filters.get("allow_poor", True))
                allow_standby = bool(publish_filters.get("allow_standby", True))
                allow_dead = bool(publish_filters.get("allow_dead", False))

                st = best.get("last_status", "UNKNOWN")
                lat = best.get("latency", 0)

                if st == "DEAD":
                    if not allow_dead:
                        continue
                elif st in ("STANDBY", "UNKNOWN"):
                    if not allow_standby:
                        continue
                elif best.get("is_alive", False):
                    if lat <= good_ms:
                        if not allow_good:
                            continue
                    elif lat <= fair_ms:
                        if not allow_fair:
                            continue
                    else:
                        if not allow_poor:
                            continue
                else:
                    if not allow_dead:
                        continue

            # Nếu link chính thỏa mãn bộ lọc, thêm vào danh sách live
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
            if second is not None:
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

    def export(self, output_dir: Optional[str] = None, include_backup: bool = True, probe_results: Optional[Dict[str, ProbeResult]] = None, publish_filters: Optional[Dict[str, Any]] = None, resolve_720p: bool = True) -> Dict[str, Any]:
        """
        Bước 4: Xuất ra live.m3u và iptv_manifest.json
        """
        if output_dir is None:
            output_dir = os.path.join(self.work_dir, "output")
        os.makedirs(output_dir, exist_ok=True)

        m3u_path = os.path.join(output_dir, "live.m3u")
        manifest_path = os.path.join(output_dir, "iptv_manifest.json")

        # Xóa sạch file cũ trước khi xuất mới để đảm bảo không bị dính rác hay dữ liệu cũ
        for old_file in [m3u_path, manifest_path]:
            if os.path.exists(old_file):
                try:
                    os.remove(old_file)
                except Exception as e:
                    print(f"Không thể xóa file cũ {old_file}: {e}")

        entries = self.generate_live_entries(include_backup=include_backup, probe_results=probe_results, publish_filters=publish_filters, resolve_720p=resolve_720p)
        manifest = PlaylistExporter.export_files(entries, m3u_path, manifest_path)

        return {
            "m3u_path": m3u_path,
            "manifest_path": manifest_path,
            "manifest": manifest,
            "exported_entries": len(entries),
            "resolved_720p": getattr(self, "last_resolve_stats", {})
        }
