#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Sinh assets/md5systems.dat tu cac file No-Intro DAT (.dat/.xml).
Ho tro 2 dinh dang: Logiqx XML (datomatic) va ClrMamePro text
(libretro-database/metadat/no-intro mirror).

Moi dong output: "<hash> <SYSTEMCODE>"
  - hash 32 ky tu hex = MD5 cua file ROM (dung cho file le)
  - hash 8 ky tu hex  = CRC32 (dung cho file ben trong .zip, doc tu
    central directory khong can giai nen)

Cach dung:
  1. Tai No-Intro DATs ve thu muc dat/ (VD tu
     https://github.com/libretro/libretro-database/tree/master/metadat/no-intro)
  2. Chay:  python3 tools/gen_md5db.py dat/
     -> ghi ra assets/md5systems.dat

RomCloud doc file nay luc chay (Md5SystemDb) de nhan dien ROM o "vong 2"
(MD5/CRC32) khi vong 1 (duoi file) khong xac dinh duoc he may.
"""
import os
import re
import sys
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

# Mirror No-Intro DATs (khong can tai khoan). Them --download de tu tai.
DAT_MIRROR = ("https://raw.githubusercontent.com/libretro/libretro-database"
              "/master/metadat/no-intro")
DAT_FILES = [
    "Nintendo - Game Boy Advance.dat",
    "Nintendo - Game Boy Color.dat",
    "Nintendo - Game Boy.dat",
    "Nintendo - Nintendo Entertainment System.dat",
    "Nintendo - Family Computer Disk System.dat",
    "Nintendo - Super Nintendo Entertainment System.dat",
    "Sega - Mega Drive - Genesis.dat",
    "Sega - 32X.dat",
    "Nintendo - Nintendo 64.dat",
    "Nintendo - Nintendo DS.dat",
    "NEC - PC Engine - TurboGrafx 16.dat",
    "Bandai - WonderSwan.dat",
    "Bandai - WonderSwan Color.dat",
    "Atari - 2600.dat",
    "Atari - 7800.dat",
    "Atari - Lynx.dat",
    "Sega - Master System - Mark III.dat",
    "Sega - Game Gear.dat",
    "Sega - PICO.dat",
    "Sega - SG-1000.dat",
    "SNK - Neo Geo Pocket.dat",
    "SNK - Neo Geo Pocket Color.dat",
]


def download_dats(dest_dir):
    os.makedirs(dest_dir, exist_ok=True)
    for fn in DAT_FILES:
        url = DAT_MIRROR + "/" + urllib.parse.quote(fn)
        dest = os.path.join(dest_dir, fn)
        if os.path.exists(dest) and os.path.getsize(dest) > 1024:
            print(f"  co san: {fn}")
            continue
        print(f"  dang tai: {fn} ...")
        urllib.request.urlretrieve(url, dest)
    print(f"Da tai {len(DAT_FILES)} DAT vao {dest_dir}")
    return dest_dir

# Map ten DAT -> ma he may trong RomCloud (Schema.h)
# Dung cho ca Logiqx XML (<header><name>) va ClrMamePro (name "...").
# QUAN TRONG: pattern con (super nintendo, game boy advance/color) phai dung
# TRUOC pattern cha (nintendo entertainment system, game boy) de khong nhan nham.
DAT_TO_SYSTEM = [
    (re.compile(r"game boy advance", re.I), "GBA"),
    (re.compile(r"game boy color", re.I), "GBC"),
    (re.compile(r"game boy\b", re.I), "GB"),
    (re.compile(r"family computer disk system", re.I), "FC"),
    (re.compile(r"super nintendo|super famicom|snes", re.I), "SFC"),
    (re.compile(r"nintendo entertainment system|famicom", re.I), "FC"),
    (re.compile(r"mega drive|genesis", re.I), "MD"),
    (re.compile(r"\b32x\b", re.I), "MD"),
    (re.compile(r"nintendo 64", re.I), "N64"),
    (re.compile(r"nintendo ds\b", re.I), "NDS"),
    (re.compile(r"pc engine|turbografx", re.I), "PCE"),
    (re.compile(r"wonderswan", re.I), "WS"),
    (re.compile(r"atari\D*2600", re.I), "ATARI2600"),
    (re.compile(r"atari\D*7800", re.I), "ATARI7800"),
    (re.compile(r"atari\D*lynx", re.I), "LYNX"),
    (re.compile(r"master system|mark iii", re.I), "MS"),
    (re.compile(r"sg[- ]?1000", re.I), "MS"),
    (re.compile(r"game gear", re.I), "GG"),
    (re.compile(r"neo geo pocket", re.I), "NGP"),
    (re.compile(r"\bpico\b", re.I), "PICO8"),
]

RE_CLRMAMEPRO_NAME = re.compile(r'^\s*name\s+"([^"]+)"', re.M)
RE_CRC = re.compile(r"\bcrc\s+([0-9a-fA-F]{8})\b")
RE_MD5 = re.compile(r"\bmd5\s+([0-9a-fA-F]{32})\b")


def iter_rom_blocks(text):
    """Yield noi dung ben trong moi khoi 'rom ( ... )', xu ly dung ten file
    co chua ngoac don (VD "Game (Japan).gba") bang cach ton trong chuoi
    trong ngoac kep."""
    idx = 0
    n = len(text)
    while True:
        m = re.search(r"\brom\s*\(", text[idx:])
        if not m:
            return
        p = idx + m.end()  # ngay sau 'rom ('
        depth = 1
        in_quote = False
        start = p
        while p < n and depth > 0:
            c = text[p]
            if in_quote:
                if c == '"':
                    in_quote = False
            elif c == '"':
                in_quote = True
            elif c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
            p += 1
        yield text[start:p - 1]
        idx = p


def dat_name_to_system(dat_name, filename):
    text = (dat_name or "") + " " + (filename or "")
    for pat, code in DAT_TO_SYSTEM:
        if pat.search(text):
            return code
    return None


def parse_logiqx(path):
    """Tra ve (system_code, [(md5, crc), ...])."""
    tree = ET.parse(path)
    root = tree.getroot()
    header_name = ""
    header = root.find("header")
    if header is not None:
        name_el = header.find("name")
        if name_el is not None and name_el.text:
            header_name = name_el.text.strip()
    code = dat_name_to_system(header_name, os.path.basename(path))
    entries = []
    if code:
        for game in root.iter("game"):
            for rom in game.iter("rom"):
                md5 = (rom.get("md5") or "").lower()
                crc = (rom.get("crc") or "").lower()
                entries.append((md5, crc))
    return code, entries


def parse_clrmamepro(path):
    """Tra ve (system_code, [(md5, crc), ...])."""
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    m = RE_CLRMAMEPRO_NAME.search(text)
    dat_name = m.group(1) if m else ""
    code = dat_name_to_system(dat_name, os.path.basename(path))
    entries = []
    if code:
        for body in iter_rom_blocks(text):
            md5_m = RE_MD5.search(body)
            crc_m = RE_CRC.search(body)
            md5 = md5_m.group(1).lower() if md5_m else ""
            crc = crc_m.group(1).lower() if crc_m else ""
            entries.append((md5, crc))
    return code, entries


def parse_dat(path):
    with open(path, "rb") as f:
        head = f.read(200)
    if head.lstrip().startswith(b"<"):
        try:
            return parse_logiqx(path)
        except ET.ParseError as e:
            print(f"  [bo qua] {path}: loi XML ({e})", file=sys.stderr)
            return None, []
    return parse_clrmamepro(path)


def main():
    cli = sys.argv[1:]
    if "--download" in cli:
        dat_dir = download_dats("dat")
        rest = [a for a in cli if a != "--download"]
        out_path = rest[0] if rest else "assets/md5systems.dat"
    else:
        if not cli:
            print(__doc__)
            return 1
        dat_dir = cli[0]
        out_path = cli[1] if len(cli) > 1 else "assets/md5systems.dat"

    table = {}  # hash -> code (giu muc dau tien)
    total_files = 0
    for root, _dirs, files in os.walk(dat_dir):
        for fn in sorted(files):
            if not fn.lower().endswith((".dat", ".xml")):
                continue
            path = os.path.join(root, fn)
            code, entries = parse_dat(path)
            total_files += 1
            if not code:
                print(f"  [bo qua] {path}: khong nhan dien duoc he may",
                      file=sys.stderr)
                continue
            added = 0
            for md5, crc in entries:
                if len(md5) == 32 and md5 not in table:
                    table[md5] = code
                    added += 1
                if len(crc) == 8 and crc not in table:
                    table[crc] = code
                    added += 1
            print(f"  {fn}: he {code}, {len(entries)} rom, +{added} muc moi")

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("# md5systems.dat - sinh tu No-Intro DATs boi tools/gen_md5db.py\n")
        f.write("# dinh dang: <md5 32 ky tu | crc32 8 ky tu> <MA_HE_MAY>\n")
        for h in sorted(table):
            f.write(f"{h} {table[h]}\n")
    size_kb = os.path.getsize(out_path) // 1024
    print(f"Xong: {len(table)} muc tu {total_files} file DAT "
          f"({size_kb} KB) -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
