#!/usr/bin/env python3
# mkota.py — sinh gói OTA lite/completion/delta + manifest.
#
# Usage:
#   mkota.py manifest <TREE> <VERSION> [DELTA_BASE]
#       -> in ota-manifest.json ra stdout (TREE = thư mục app, vd staging/Apps/RomCloud)
#   mkota.py release <STAGING> <VERSION> <BASE_MANIFEST> <OUT_DIR>
#       -> OUT_DIR/ota-{core,media,patch}-v*.zip + ota-manifest.json
#       + in hashes + fragment version.json.
#
# Quy ước đặt tên (từ v2.5.3): CHỈ bản full cài-chạy-OTA được mới mang tên
# RomCloud (RomCloud-vX.Y.Z.zip). File máy (linh kiện OTA, user không tải
# tay) mang prefix ota- để nhìn là biết không phải bản cài:
#   ota-core-vX.Y.Z.zip   (= lite cũ: full trừ cụm media/dat)
#   ota-media-vX.Y.Z.zip  (= completion cũ: phần media/dat vá cho máy core)
#   ota-patch-vX.Y.Z.zip  (= delta cũ: file đổi so baseline)
# (v2.5.2 trở về trước dùng tên cũ RomCloud-v*-{lite,completion,delta}.zip;
#  code máy đọc URL tường minh trong version.json trước, suy tên mới khi
#  thiếu, nên vẫn tương thích.)
#
# Quy ước (khớp UpdateManager.cpp):
# - manifest: {"version": X, "delta_base": <bản nền cho delta>, "files": [{"path","sha256","size"}]}
#   path tương đối appRoot (vd "bin/RomCloud"). delta_base vắng = manifest
#   baseline (không phục vụ máy, chỉ để diff).
# - delta.zip/completion.zip/lite.zip layout trong: Apps/RomCloud/<relpath>
#   (giống full-zip để tái dùng kịp swap).
# - CORE = full trừ cụm media/dat (mpv/yt-dlp/ffmpeg/.dat): đủ chạy local,
#   media lấy lazy theo cơ chế mpv_bundle có sẵn.
# - MEDIA = full trừ CORE. PATCH = file đổi so baseline.
# - File user (data/cache/log/dotfiles/rc-web/.ota_flavor) KHÔNG vào manifest.
import hashlib
import json
import os
import sys
import zipfile
import fnmatch

APP_PREFIX = "Apps/RomCloud"

EXCLUDE_DIRS = {"data", "cache", "rc-web"}
EXCLUDE_FILES = {".ota_flavor"}
EXCLUDE_SUFFIX = (".log",)
EXCLUDE_BASENAME = (".DS_Store",)

# Cụm media/dat: không vào lite (lấy 1 lần qua completion/lazy).
LITE_EXCLUDES = [
    "bin/mpv", "bin/yt-dlp", "bin/yt-dlp-glibc", "bin/7zzs",
    "lib/libav*", "lib/libass*", "lib/libpostproc*", "lib/libdav1d*",
    "lib/libspeex*", "lib/libswresample*", "lib/libswscale*",
    "assets/md5systems.dat", "assets/fbneosets.dat",
]


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def walk_managed(tree):
    """Liệt kê file managed (relpath dùng /), đã sắp xếp."""
    out = []
    for root, dirs, files in os.walk(tree):
        dirs[:] = sorted(
            d for d in dirs
            if d not in EXCLUDE_DIRS and not d.startswith("."))
        for fn in sorted(files):
            if fn in EXCLUDE_FILES or fn.startswith("._") or \
               fn in EXCLUDE_BASENAME or fn.endswith(EXCLUDE_SUFFIX):
                continue
            full = os.path.join(root, fn)
            rel = os.path.relpath(full, tree).replace(os.sep, "/")
            out.append((rel, full))
    return out


def gen_manifest(tree, version, delta_base=None):
    files = []
    for rel, full in walk_managed(tree):
        st = os.stat(full)
        files.append({
            "path": rel,
            "sha256": sha256_file(full),
            "size": st.st_size,
        })
    mf = {"version": version, "files": files}
    if delta_base is not None:
        mf["delta_base"] = delta_base
    return mf


def is_lite_file(rel):
    return not any(fnmatch.fnmatchcase(rel, pat) for pat in LITE_EXCLUDES)


def zip_mode_for(rel):
    # File chạy được: +x (thiết bị giải nén giữ quyền; app cũng chmod lại).
    if rel == "launch.sh" or rel.startswith("bin/") or \
       (rel.startswith("scripts/") and rel.endswith(".sh")):
        return 0o755
    return 0o644


def write_zip(zip_path, tree, rels, flavor=None):
    if os.path.exists(zip_path):
        os.remove(zip_path)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        if flavor is not None:
            zi = zipfile.ZipInfo(APP_PREFIX + "/.ota_flavor")
            zi.external_attr = (0o644 & 0xFFFF) << 16
            z.writestr(zi, flavor + "\n")
        for rel in sorted(rels):
            full = os.path.join(tree, *rel.split("/"))
            arc = APP_PREFIX + "/" + rel
            zi = zipfile.ZipInfo(arc)
            zi.external_attr = (zip_mode_for(rel) & 0xFFFF) << 16
            with open(full, "rb") as f:
                z.writestr(zi, f.read())


def cmd_manifest(argv):
    tree, version = argv[0], argv[1]
    delta_base = argv[2] if len(argv) > 2 else None
    mf = gen_manifest(tree, version, delta_base)
    json.dump(mf, sys.stdout, separators=(",", ":"))
    sys.stdout.write("\n")


def cmd_release(argv):
    staging, version, base_path, out_dir = argv[0], argv[1], argv[2], argv[3]
    os.makedirs(out_dir, exist_ok=True)
    tree = os.path.join(staging, APP_PREFIX)
    with open(base_path, "r", encoding="utf-8") as f:
        base = json.load(f)
    base_map = {e["path"]: e["sha256"] for e in base.get("files", [])}

    mf = gen_manifest(tree, version, None)
    new_map = {e["path"]: e for e in mf["files"]}
    lite_rels = [r for r, _ in walk_managed(tree) if is_lite_file(r)]
    lite_set = set(lite_rels)
    completion_rels = [e["path"] for e in mf["files"]
                       if e["path"] not in lite_set]
    changed = [p for p, e in new_map.items()
               if base_map.get(p) != e["sha256"]]
    deleted = sorted(set(base_map) - set(new_map))

    stem = "ota-%s-v%s.zip"
    core_zip = os.path.join(out_dir, stem % ("core", version))
    media_zip = os.path.join(out_dir, stem % ("media", version))
    patch_zip = os.path.join(out_dir, stem % ("patch", version))
    write_zip(core_zip, tree, lite_rels, flavor="lite")
    write_zip(media_zip, tree, completion_rels)
    # delta_base = version của baseline (máy phải đúng bản đó mới áp delta).
    mf["delta_base"] = base.get("version", "")
    if changed:
        write_zip(patch_zip, tree, changed)
    else:
        print("WARNING: khong co file nao doi so baseline "
              "(delta rong)!", file=sys.stderr)

    mf_path = os.path.join(out_dir, "ota-manifest.json")
    with open(mf_path, "w", encoding="utf-8") as f:
        json.dump(mf, f, separators=(",", ":"))
        f.write("\n")

    def digest(p):
        return sha256_file(p), os.stat(p).st_size

    print("== OTA %s ==" % version)
    print("manifest: %d files (%s)" % (len(mf["files"]), mf_path))
    print("deleted-vs-baseline (%d, KHONG ship, orphans o lai tren may): %s"
          % (len(deleted), ",".join(deleted[:20])))
    import posixpath
    base_url = ("https://github.com/bun2it/RomCloud/releases/download/v%s/"
                % version)
    frag = {}
    for tag, zp in (("lite", core_zip), ("completion", media_zip),
                    ("delta", patch_zip if changed else None)):
        if zp is None or not os.path.exists(zp):
            continue
        h, s = digest(zp)
        aname = posixpath.basename(zp)
        print("%s: %s %d bytes sha256=%s" % (tag, aname, s, h))
        frag[tag + "_zip_url"] = base_url + aname
    frag["ota_manifest_url"] = base_url + "ota-manifest.json"
    print("== version.json fragment ==")
    print(json.dumps(frag, indent=2))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    if argv[1] == "manifest":
        if len(argv) < 4:
            print(__doc__)
            return 2
        cmd_manifest(argv[2:])
    elif argv[1] == "release":
        if len(argv) < 6:
            print(__doc__)
            return 2
        cmd_release(argv[2:])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
