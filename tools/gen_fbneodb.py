#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Sinh assets/fbneosets.dat tu FBNeo Arcade DAT (ClrMamePro XML).
Moi dong output: "<stem>|<romname>|<crc>" (crc chu thuong).
  - stem = ten set (ten file zip khong duoi, chu thuong)
  - romname/crc = ten + CRC32 file con FBNeo doi cho set do

May (FbneoCompatDb) dung bang nay de ket luan 1 file zip arcade:
  - FBNEO_OK: ruot zip khop du bo file FBNeo doi (chay duoc core moi)
  - MAME_ONLY: ten set quen ma ruot lech (dump kieu MAME cu) -> de o MAME

Cach dung:
  python3 tools/gen_fbneodb.py --download
  -> tai DAT ve dat/ roi sinh assets/fbneosets.dat
"""
import os
import sys
import urllib.request
import xml.etree.ElementTree as ET

DAT_URL = ("https://raw.githubusercontent.com/libretro/fbneo/master/dats/"
           "FinalBurn%20Neo%20%28ClrMame%20Pro%20XML%2C%20Arcade%20only%29.dat")
DAT_LOCAL = os.path.join("dat", "fbneo-arcade.dat")


def main():
    cli = sys.argv[1:]
    out_path = "assets/fbneosets.dat"
    for a in cli:
        if not a.startswith("--"):
            out_path = a
    if "--download" in cli or not os.path.exists(DAT_LOCAL):
        os.makedirs("dat", exist_ok=True)
        print("  dang tai FBNeo Arcade DAT ...")
        urllib.request.urlretrieve(DAT_URL, DAT_LOCAL)

    tree = ET.parse(DAT_LOCAL)
    root = tree.getroot()
    table = {}  # (stem, romname) -> crc (giu muc dau tien)
    sets = 0
    for game in root.iter("game"):
        name = (game.get("name") or "").strip().lower()
        if not name:
            continue
        sets += 1
        for rom in game.iter("rom"):
            rn = (rom.get("name") or "").strip()
            crc = (rom.get("crc") or "").strip().lower()
            if not rn or len(crc) != 8:
                continue
            key = (name, rn)
            if key not in table:
                table[key] = crc

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("# fbneosets.dat - sinh tu FBNeo Arcade DAT boi tools/gen_fbneodb.py\n")
        f.write("# dinh dang: <stem>|<romname>|<crc>\n")
        for (stem, rn) in sorted(table):
            # ten file co the chua '|' (hiem) -> thay bang '_' de parser an toan
            f.write(f"{stem}|{rn.replace('|', '_')}|{table[(stem, rn)]}\n")
    size_kb = os.path.getsize(out_path) // 1024
    print(f"Xong: {len(table)} muc ({sets} set) "
          f"({size_kb} KB) -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
