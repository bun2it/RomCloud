#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Vilao.ai helper cho IPTV Curator: dùng AI đọc + phân loại kênh lạ.
Chuẩn OpenAI-compatible (/v1/chat/completions), chỉ dùng stdlib.
Key lưu ở ai_config.json (KHÔNG commit).
"""
import json
import os
import urllib.request
import urllib.error
from typing import List, Dict, Any, Tuple

CUR_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(CUR_DIR, "ai_config.json")
DEFAULT_BASE_URL = "https://api.vilao.ai/v1"

DEFAULT_PROMPT_TEMPLATE = (
    "Bạn là bộ phân loại kênh IPTV cho thị trường Việt Nam. "
    "Chỉ được dùng đúng 1 trong các nhóm chuẩn sau (giữ nguyên chữ): {GROUPS}. "
    "Với mỗi kênh, căn cứ NỘI DUNG kênh (bóng đá -> Thể Thao, phim -> Phim & Giải Trí, "
    "thiếu nhi/hoạt hình -> Thiếu Nhi, nhạc -> Âm Nhạc, tài liệu/khoa học -> Khám Phá, "
    "tin tức -> Tin Tức, mua sắm -> Mua Sắm, radio -> Radio, đài tỉnh -> Địa phương, "
    "nước ngoài -> Quốc Tế, trùng tên đài lớn thì theo đài đó). "
    "Kênh CÁ ĐỘ/nhà cái (W88, M88, Fun88, BK8, 188BET, Dafabet, VSBet, tài xỉu, "
    "casino, kèo nhà cái): đánh dấu \"betting\": true và xếp nhóm \"Kênh Chung\" "
    "để loại riêng, KHÔNG xếp vào nhóm khác. "
    'Trả về DUY NHẤT 1 JSON array, mỗi phần tử {"name": "<tên gốc>", '
    '"group": "<nhóm chuẩn>", "betting": true/false} (giữ nguyên tên gốc). '
    "Không giải thích, không markdown.\nKênh cần phân loại:\n{CHANNELS}"
)


def load_config() -> Dict[str, Any]:
    cfg = {"base_url": DEFAULT_BASE_URL, "api_key": "", "model": ""}
    try:
        if os.path.exists(CONFIG_PATH):
            with open(CONFIG_PATH, encoding="utf-8") as f:
                d = json.load(f)
            for k in ("base_url", "api_key", "model"):
                if d.get(k):
                    cfg[k] = d[k]
    except Exception:
        pass
    return cfg


def save_config(base_url: str = "", api_key: str = "",
                model: str = "", prompt_template: str = "") -> Dict[str, Any]:
    cfg = load_config()
    if base_url:
        cfg["base_url"] = base_url.rstrip("/")
    if api_key:
        cfg["api_key"] = api_key
    if model:
        cfg["model"] = model
    # Chuỗi rỗng = reset về prompt mặc định.
    if prompt_template is not None:
        if prompt_template.strip():
            cfg["prompt_template"] = prompt_template
        else:
            cfg.pop("prompt_template", None)
    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)
    try:
        os.chmod(CONFIG_PATH, 0o600)
    except Exception:
        pass
    return {"base_url": cfg["base_url"],
            "model": cfg["model"],
            "has_key": bool(cfg["api_key"]),
            "has_custom_prompt": bool((cfg.get("prompt_template") or "").strip())}


def build_classify_prompt(items: List[Dict[str, str]],
                          canonical_groups: List[str]) -> str:
    lines = [f'- "{it.get("name", "")}" (nhóm gốc: "{it.get("group", "")}")'
             for it in items]
    cfg = load_config()
    template = (cfg.get("prompt_template") or "").strip()
    if not template:
        template = DEFAULT_PROMPT_TEMPLATE
    groups = ", ".join(f'"{g}"' for g in canonical_groups)
    channels = "\n".join(lines)
    return template.replace("{GROUPS}", groups).replace("{CHANNELS}",
                                                        channels)


def chat(base_url: str, api_key: str, model: str, prompt: str,
         timeout: int = 90, max_tokens: int = 4000) -> Tuple[str, str]:
    """Trả về (content, error)."""
    url = base_url.rstrip("/") + "/chat/completions"
    body = json.dumps({
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": max_tokens,
        "temperature": 0.1,
    }).encode("utf-8")
    req = urllib.request.Request(
        url, data=body,
        headers={"Authorization": f"Bearer {api_key}",
                 "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read().decode("utf-8", errors="replace"))
    except urllib.error.HTTPError as e:
        try:
            detail = e.read().decode("utf-8", errors="replace")[:300]
        except Exception:
            detail = ""
        return "", f"HTTP {e.code}: {detail}"
    except Exception as e:
        return "", f"Lỗi kết nối AI: {e}"
    try:
        msg = data["choices"][0]["message"]
        content = (msg.get("content") or "").strip()
        if not content:
            # Model reasoning (MiniMax M2.7): nội dung có thể nằm ở
            # reasoning_content/reasoning/thinking.
            for k in ("reasoning_content", "reasoning", "thinking"):
                if msg.get(k):
                    content = str(msg[k]).strip()
                    break
        if not content:
            return "", "AI trả về rỗng (có thể bị giới hạn tốc độ, thử lại sau)"
        return content, ""
    except Exception:
        return "", "AI trả về sai định dạng"


def list_models(base_url: str, api_key: str,
                timeout: int = 20) -> Tuple[List[Dict[str, Any]], str]:
    """Danh sách model đã sub (id đầy đủ 'prefix/Name')."""
    url = base_url.rstrip("/") + "/models"
    req = urllib.request.Request(
        url, headers={"Authorization": f"Bearer {api_key}"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read().decode("utf-8", errors="replace"))
    except urllib.error.HTTPError as e:
        return [], f"HTTP {e.code}"
    except Exception as e:
        return [], f"Lỗi kết nối AI: {e}"
    out = []
    items = data.get("data") or []
    for m in items:
        if not isinstance(m, dict):
            continue
        mid = str(m.get("id", "")).strip()
        if not mid:
            continue
        prefix = str(m.get("provider_prefix", "") or "").strip()
        full = f"{prefix}/{mid}" if prefix and "/" not in mid else mid
        if m.get("active") is False:
            continue
        out.append({"id": full, "name": mid})
    return out, ""


def parse_suggestions(text: str, valid_groups: List[str]) -> List[Dict[str, str]]:
    """Bóc JSON array từ reply, chỉ giữ mục có group hợp lệ."""
    valid = set(valid_groups)
    start = text.find("[")
    end = text.rfind("]")
    if start < 0 or end <= start:
        return []
    try:
        arr = json.loads(text[start:end + 1])
    except Exception:
        return []
    out = []
    for it in arr:
        if not isinstance(it, dict):
            continue
        name = str(it.get("name", "")).strip()
        group = str(it.get("group", "")).strip()
        if name and group in valid:
            out.append({"name": name, "group": group,
                        "betting": bool(it.get("betting", False)) is True})
    return out


def classify_unknown(items: List[Dict[str, str]],
                     canonical_groups: List[str],
                     batch_size: int = 40) -> Dict[str, Any]:
    """Phân loại kênh lạ theo batch. Trả về {suggestions, errors}."""
    cfg = load_config()
    if not cfg["api_key"]:
        return {"suggestions": [], "error": "Chưa nhập API key (tab Rules)."}
    if not cfg["model"]:
        return {"suggestions": [], "error": "Chưa nhập tên model."}
    suggestions: List[Dict[str, str]] = []
    errors: List[str] = []
    import time as _time
    for i in range(0, len(items), batch_size):
        if i > 0:
            _time.sleep(3)  # giãn request tránh 429
        chunk = items[i:i + batch_size]
        chunk = items[i:i + batch_size]
        prompt = build_classify_prompt(chunk, canonical_groups)
        text, err = chat(cfg["base_url"], cfg["api_key"], cfg["model"],
                         prompt)
        if err:
            errors.append(f"batch {i // batch_size + 1}: {err}")
            continue
        got = parse_suggestions(text, canonical_groups)
        if not got:
            errors.append(f"batch {i // batch_size + 1}: AI không trả JSON hợp lệ")
            continue
        suggestions.extend(got)
    return {"suggestions": suggestions, "errors": errors}
