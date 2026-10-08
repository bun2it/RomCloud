#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Database History & Reputation Scoring - Lưu trữ lịch sử kiểm tra và chấm điểm uy tín cho từng luồng stream.
Giải quyết bài toán "hôm nay link chết, mai lại sống": không xóa vội, dùng điểm số uy tín và số lần thất bại liên tiếp.
"""

import sqlite3
import os
import time
from typing import Dict, Any, List, Optional
from health_checker import ProbeResult


class HistoryDB:
    def __init__(self, db_path: Optional[str] = None):
        if db_path is None:
            db_path = os.path.join(os.path.dirname(__file__), "iptv_history.db")
        self.db_path = db_path
        self.init_db()

    def get_connection(self) -> sqlite3.Connection:
        conn = sqlite3.connect(self.db_path)
        conn.row_factory = sqlite3.Row
        return conn

    def init_db(self):
        with self.get_connection() as conn:
            conn.execute("""
            CREATE TABLE IF NOT EXISTS stream_history (
                url TEXT PRIMARY KEY,
                channel_name TEXT,
                group_name TEXT,
                logo TEXT,
                tvg_id TEXT,
                total_checks INTEGER DEFAULT 0,
                success_checks INTEGER DEFAULT 0,
                consecutive_fails INTEGER DEFAULT 0,
                last_latency_ms INTEGER DEFAULT 0,
                avg_latency_ms INTEGER DEFAULT 0,
                last_status TEXT DEFAULT 'UNKNOWN',
                last_error TEXT DEFAULT '',
                last_checked_at INTEGER DEFAULT 0,
                first_seen_at INTEGER DEFAULT 0,
                reputation_score INTEGER DEFAULT 50
            );
            """)
            conn.execute("CREATE INDEX IF NOT EXISTS idx_channel_name ON stream_history(channel_name);")
            conn.execute("CREATE INDEX IF NOT EXISTS idx_reputation_score ON stream_history(reputation_score);")
            conn.commit()

    def calculate_score(self, total_checks: int, success_checks: int, consecutive_fails: int, avg_latency_ms: int) -> int:
        """
        Tính điểm uy tín từ 0 đến 100:
        - Tỷ lệ uptime (40 điểm)
        - Số lần chết liên tiếp (phạt nặng, tối đa trừ 40 điểm)
        - Tốc độ phản hồi latency (20 điểm thưởng nếu < 200ms)
        """
        if total_checks == 0:
            return 50

        uptime_ratio = success_checks / total_checks
        score = uptime_ratio * 40.0

        # Phạt số lần thất bại liên tiếp (mỗi lần chết liên tiếp trừ 8 điểm)
        penalty = min(consecutive_fails * 8.0, 40.0)
        score -= penalty

        # Thưởng độ trễ ping
        if avg_latency_ms > 0:
            if avg_latency_ms < 150:
                score += 20.0
            elif avg_latency_ms < 350:
                score += 15.0
            elif avg_latency_ms < 800:
                score += 10.0
            else:
                score += 2.0

        return max(0, min(100, int(score)))

    def record_probe(self, probe: ProbeResult, channel_name: str = "", group_name: str = "", logo: str = "", tvg_id: str = ""):
        now = int(time.time())
        with self.get_connection() as conn:
            row = conn.execute("SELECT * FROM stream_history WHERE url = ?", (probe.url,)).fetchone()
            if row is None:
                # Tạo mới
                total = 1
                success = 1 if probe.is_alive else 0
                consecutive_fails = 0 if probe.is_alive else 1
                avg_latency = probe.latency_ms if probe.is_alive else 999
                status = "ALIVE" if probe.is_alive else "STANDBY"
                score = self.calculate_score(total, success, consecutive_fails, avg_latency)

                conn.execute("""
                INSERT INTO stream_history (
                    url, channel_name, group_name, logo, tvg_id,
                    total_checks, success_checks, consecutive_fails,
                    last_latency_ms, avg_latency_ms, last_status, last_error,
                    last_checked_at, first_seen_at, reputation_score
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """, (
                    probe.url, channel_name, group_name, logo, tvg_id,
                    total, success, consecutive_fails,
                    probe.latency_ms, avg_latency, status, probe.error,
                    now, now, score
                ))
            else:
                # Cập nhật bản ghi có sẵn
                total = row["total_checks"] + 1
                success = row["success_checks"] + (1 if probe.is_alive else 0)

                if probe.is_alive:
                    consecutive_fails = 0
                    status = "ALIVE"
                else:
                    consecutive_fails = row["consecutive_fails"] + 1
                    status = "DEAD" if consecutive_fails >= 5 else "STANDBY"

                # Tính trung bình cộng độ trễ
                if probe.is_alive:
                    if row["avg_latency_ms"] > 0:
                        avg_latency = int((row["avg_latency_ms"] * 0.7) + (probe.latency_ms * 0.3))
                    else:
                        avg_latency = probe.latency_ms
                else:
                    avg_latency = row["avg_latency_ms"]

                score = self.calculate_score(total, success, consecutive_fails, avg_latency)

                # Giữ metadata cũ nếu thông tin mới bị rỗng
                ch_name = channel_name if channel_name else row["channel_name"]
                grp_name = group_name if group_name else row["group_name"]
                ch_logo = logo if logo else row["logo"]
                ch_tvg = tvg_id if tvg_id else row["tvg_id"]

                conn.execute("""
                UPDATE stream_history SET
                    channel_name = ?, group_name = ?, logo = ?, tvg_id = ?,
                    total_checks = ?, success_checks = ?, consecutive_fails = ?,
                    last_latency_ms = ?, avg_latency_ms = ?, last_status = ?, last_error = ?,
                    last_checked_at = ?, reputation_score = ?
                WHERE url = ?
                """, (
                    ch_name, grp_name, ch_logo, ch_tvg,
                    total, success, consecutive_fails,
                    probe.latency_ms, avg_latency, status, probe.error,
                    now, score, probe.url
                ))

            conn.commit()

    def get_channel_streams(self, channel_name: str) -> List[Dict[str, Any]]:
        """Lấy danh sách các luồng của một kênh, sắp xếp theo điểm uy tín giảm dần và ping thấp dần"""
        with self.get_connection() as conn:
            cursor = conn.execute("""
                SELECT * FROM stream_history 
                WHERE channel_name = ?
                ORDER BY reputation_score DESC, last_latency_ms ASC
            """, (channel_name,))
            return [dict(row) for row in cursor.fetchall()]

    def get_all_records(self) -> List[Dict[str, Any]]:
        with self.get_connection() as conn:
            cursor = conn.execute("SELECT * FROM stream_history ORDER BY channel_name, reputation_score DESC")
            return [dict(row) for row in cursor.fetchall()]

    def get_stats(self) -> Dict[str, Any]:
        with self.get_connection() as conn:
            total = conn.execute("SELECT COUNT(*) FROM stream_history").fetchone()[0]
            alive = conn.execute("SELECT COUNT(*) FROM stream_history WHERE last_status = 'ALIVE'").fetchone()[0]
            standby = conn.execute("SELECT COUNT(*) FROM stream_history WHERE last_status = 'STANDBY'").fetchone()[0]
            dead = conn.execute("SELECT COUNT(*) FROM stream_history WHERE last_status = 'DEAD'").fetchone()[0]
            channels = conn.execute("SELECT COUNT(DISTINCT channel_name) FROM stream_history").fetchone()[0]
            return {
                "total_streams": total,
                "alive_streams": alive,
                "standby_streams": standby,
                "dead_streams": dead,
                "distinct_channels": channels
            }
