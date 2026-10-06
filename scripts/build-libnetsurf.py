#!/usr/bin/env python3
"""build-libnetsurf.py — Cross-compile NetSurf libs for aarch64-linux-gnu.2.33.

Bypasses NetSurf's NSSHARED/Makefile.tools build system (assumes Linux host at
/opt/netsurf). Instead compile each .c with zig cc, archive with zig ar, and
install headers into sysroot/include/<lib>.

  ./scripts/build-libnetsurf.py --check         # verify toolchain + paths
  ./scripts/build-libnetsurf.py --lib wapcaplet # build one
  ./scripts/build-libnetsurf.py                # build all in dep order

Output:
  sysroot/include/<lib>/  - public headers (.h)
  sysroot/lib/lib<lib>.a  - static archive
  sysroot/lib/<lib>.o/    - per-file .o files (incremental rebuilds)
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
NETSURF_SRC = REPO_ROOT / "libnetsurf-src"
SYSROOT = REPO_ROOT / "sysroot"


def ensure_source_link():
    """Auto-create libnetsurf-src symlink if missing.

    Looks in common locations for the unpacked netsurf-all-3.11 directory
    and creates a symlink so subsequent builds just work.
    """
    if NETSURF_SRC.exists():
        return True
    candidates = [
        os.environ.get("NETSURF_SRC"),
        REPO_ROOT.parent / "Downloads/netsurf-all-3.11",
        Path.home() / "Downloads/netsurf-all-3.11",
        Path("/Users/tai/Downloads/netsurf-all-3.11"),
        Path("/Users/tai/RomCloud/netsurf-all-3.11"),
    ]
    for c in candidates:
        if c and Path(c).is_dir() and (Path(c) / "libwapcaplet").is_dir():
            print(f"[info] auto-linking {NETSURF_SRC} -> {c}")
            NETSURF_SRC.symlink_to(c)
            return True
    return False


def find_zig():
    for c in [
        os.environ.get("ZIG"),
        shutil.which("zig"),
        "/Users/tai/.gemini/antigravity-ide/brain/00d3b56c-d55c-4262-81a8-0cf5fe35825f/tools/zig-macos-aarch64-0.13.0/zig",
    ]:
        if c and Path(c).exists():
            return c
    print("ERROR: zig not found. Set $ZIG or install zig.", file=sys.stderr)
    sys.exit(1)


ZIG = find_zig()
TARGET = "aarch64-linux-gnu.2.33"

# Build order = dependency order. Each lib's `deps` must be installed first
# (headers in sysroot/include/<dep>) so #include <dep/foo.h> resolves.
LIBS = {
    "wapcaplet":   {"deps": [],                  "cflags": ["-D_BSD_SOURCE", "-D_DEFAULT_SOURCE"], "inc_dir": "lib"},  # include/libwapcaplet/
    "nslog":       {"deps": [],                  "cflags": ["-D_BSD_SOURCE", "-D_DEFAULT_SOURCE"], "inc_dir": "nolib"},  # include/nslog/
    "nsutils":     {"deps": [],                  "cflags": ["-D_BSD_SOURCE", "-D_DEFAULT_SOURCE"], "inc_dir": "nolib"},  # include/nsutils/
    "parserutils": {"deps": ["wapcaplet"],       "cflags": ["-D_BSD_SOURCE", "-D_DEFAULT_SOURCE"], "inc_dir": "nolib"},  # include/parserutils/
    "hubbub":      {"deps": ["wapcaplet", "parserutils"],
                    "cflags": ["-D_BSD_SOURCE", "-D_DEFAULT_SOURCE"], "inc_dir": "nolib"},  # include/hubbub/
    "css":         {"deps": ["wapcaplet", "parserutils"],
                    "cflags": ["-D_BSD_SOURCE", "-D_DEFAULT_SOURCE"], "inc_dir": "lib"},  # include/libcss/
    "dom":         {"deps": ["wapcaplet", "parserutils", "hubbub"],
                    "cflags": ["-D_BSD_SOURCE", "-D_DEFAULT_SOURCE"], "inc_dir": "nolib"},  # include/dom/
    "nsfb":        {"deps": ["nslog", "nsutils"],
                    "cflags": ["-D_BSD_SOURCE", "-D_DEFAULT_SOURCE"], "inc_dir": "flat",
                    "skip": True},  # broken in netsurf-all-3.11: stale includes
                                     # like utils/log.h, desktop/plotters.h. We use
                                     # SDL2 directly instead of NetSurf's framebuffer
                                     # abstraction layer.
}


def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        out = (r.stdout or b"") + (r.stderr or b"")
        sys.stderr.write("\n[FAIL] " + " ".join(cmd) + "\n" + out.decode(errors="replace") + "\n")
        raise SystemExit(r.returncode)
    return r


def find_sources(lib_dir):
    # Exclude directories that need optional system libs we don't ship:
    # test/    - depends on libcheck
    # examples/ - demo programs
    # bench/   - benchmarks
    # perf/    - performance comparisons
    # bindings/xml/ - optional XML parser (libxml, expat); we only need HTML
    return sorted(p for p in lib_dir.rglob("*.c")
                  if not any(seg in str(p) for seg in (
                      "/test/", "/examples/", "/bench/", "/perf/",
                      "/bindings/xml/",
                  )))


def pre_build(name, src_dir):
    """Generate files (perl/flex/bison) that NetSurf's Makefile would produce.

    Outputs land next to the .l/.y/.pl sources in src/ so find_sources() finds
    them on the next glob.
    """
    # libparserutils: build/make-aliases.pl reads build/Aliases and writes
    # src/charset/aliases.inc (aliases.c #include's it).
    if name == "libparserutils":
        aliases_inc = src_dir / "src" / "charset" / "aliases.inc"
        perl_script = src_dir / "build" / "make-aliases.pl"
        aliases_data = src_dir / "build" / "Aliases"
        if perl_script.exists() and aliases_data.exists():
            # The script uses cwd-relative 'src/charset/aliases.inc' as output,
            # so run from src_dir/.
            r = subprocess.run(
                ["perl", str(perl_script)],
                cwd=str(src_dir),
                capture_output=True, text=True,
            )
            if r.returncode != 0:
                sys.stderr.write(f"[pre_build] aliases.pl failed:\n{r.stderr}\n")
                raise SystemExit(r.returncode)
            if aliases_inc.exists():
                print(f"[pre_build] {name}: generated aliases.inc "
                      f"({aliases_inc.stat().st_size} bytes)")

    # libhubbub: build/make-entities.pl reads build/Entities and writes
    # src/tokeniser/entities.inc (entities.c #include's it).
    # Also: gperf element-type.gperf -> src/treebuilder/autogenerated-element-type.c
    if name == "libhubbub":
        entities_inc = src_dir / "src" / "tokeniser" / "entities.inc"
        perl_script = src_dir / "build" / "make-entities.pl"
        entities_data = src_dir / "build" / "Entities"
        if perl_script.exists() and entities_data.exists():
            r = subprocess.run(
                ["perl", str(perl_script)],
                cwd=str(src_dir),
                capture_output=True, text=True,
            )
            if r.returncode != 0:
                sys.stderr.write(f"[pre_build] entities.pl failed:\n{r.stderr}\n")
                raise SystemExit(r.returncode)
            if entities_inc.exists():
                print(f"[pre_build] {name}: generated entities.inc "
                      f"({entities_inc.stat().st_size} bytes)")

        # gperf: element-type.gperf -> treebuilder/autogenerated-element-type.c
        # The Makefile runs `gperf` then sed-rewrites the leading
        # `const struct element_type_map` to `static const struct ...` so the
        # symbol has internal linkage (no multiple-definition errors at link).
        gperf_in = src_dir / "src" / "treebuilder" / "element-type.gperf"
        gperf_out = src_dir / "src" / "treebuilder" / "autogenerated-element-type.c"
        if gperf_in.exists():
            r = subprocess.run(
                ["gperf", "--output-file=str_tmp.c", "element-type.gperf"],
                cwd=str(gperf_in.parent), capture_output=True, text=True,
            )
            if r.returncode != 0:
                sys.stderr.write(f"[pre_build] gperf failed:\n{r.stderr}\n")
                raise SystemExit(r.returncode)
            tmp = gperf_in.parent / "str_tmp.c"
            # Apply the sed: make the map struct static.
            with open(tmp) as f:
                content = f.read()
            content = content.replace(
                "const struct element_type_map",
                "static const struct element_type_map", 1)
            gperf_out.write_text(content)
            tmp.unlink()
            print(f"[pre_build] {name}: gperf -> autogenerated-element-type.c "
                  f"({gperf_out.stat().st_size} bytes)")

    # libnslog: filter-lexer.l -> filter-lexer.inc + filter-lexer.h (flex)
    #          filter-parser.y -> filter-parser.c + filter-parser.h (bison)
    if name == "libnslog":
        src = src_dir / "src"
        lexer_l = src / "filter-lexer.l"
        parser_y = src / "filter-parser.y"
        if lexer_l.exists():
            if shutil.which("flex") is None:
                sys.stderr.write(
                    "[pre_build] ERROR: flex not found in PATH. Install via:\n"
                    "          macOS:  brew install flex\n"
                    "          Linux:  apt install flex\n")
                raise SystemExit(1)
            r = subprocess.run(
                ["flex", "--outfile=filter-lexer.inc",
                 "--header-file=filter-lexer.h", "filter-lexer.l"],
                cwd=str(src), capture_output=True, text=True,
            )
            if r.returncode != 0:
                sys.stderr.write(f"[pre_build] flex failed:\n{r.stderr}\n")
                raise SystemExit(r.returncode)
            print(f"[pre_build] {name}: flex -> filter-lexer.inc + .h")
        if parser_y.exists():
            # macOS ships bison 2.3 which lacks %destructor (bison 2.6+).
            # The NetSurf .y file requires 2.6+, so prefer Homebrew bison 3.x.
            bison = shutil.which("bison")
            for candidate in [
                "/opt/homebrew/opt/bison/bin/bison",
                "/usr/local/opt/bison/bin/bison",
            ]:
                if Path(candidate).exists():
                    bison = candidate
                    break
            if bison is None:
                sys.stderr.write(
                    "[pre_build] ERROR: bison not found in PATH. Install via:\n"
                    "          macOS:  brew install bison\n"
                    "          Linux:  apt install bison  (need bison 2.6+)\n")
                raise SystemExit(1)
            # Bison 3.x with %pure-parser no longer auto-renames yylex/yyerror
            # from the %name-prefix directive, so the generated code calls
            # `yylex(...)` but the lexer file's `%option prefix=...` exposes
            # `filter_lex(...)`. Patch the .y to add the missing
            # `%name-prefix "filter_"` so bison emits #define yylex filter_lex.
            txt = parser_y.read_text()
            if "%name-prefix" not in txt:
                txt = txt.replace("%pure-parser",
                                  "%pure-parser\n%name-prefix \"filter_\"", 1)
                parser_y.write_text(txt)
                print(f"[pre_build] {name}: patched filter-parser.y "
                      f"(added %name-prefix filter_)")
            r = subprocess.run(
                [bison, "--output=filter-parser.c",
                 "--defines=filter-parser.h", "filter-parser.y"],
                cwd=str(src), capture_output=True, text=True,
            )
            if r.returncode != 0:
                sys.stderr.write(f"[pre_build] bison failed:\n{r.stderr}\n")
                raise SystemExit(r.returncode)
            print(f"[pre_build] {name}: bison -> filter-parser.c + .h")


def build_one(lib, force=False):
    name = f"lib{lib}"
    src_dir = NETSURF_SRC / name
    if LIBS[lib].get("skip"):
        print(f"[skip] {name}: marked as skip in LIBS dict")
        return
    if not src_dir.exists():
        print(f"[skip] {name}: source dir not found at {src_dir}")
        return

    out_obj = SYSROOT / "lib" / f"{name}.o"
    out_lib = SYSROOT / "lib" / f"lib{lib}.a"
    # Headers go under sysroot/include/<lib>/ (no "lib" prefix on the dir).
    # For libs whose consumers `#include <lib<lib>/foo.h>` (e.g. libcss uses
    # `<libwapcaplet/libwapcaplet.h>`), we ALSO install under
    # sysroot/include/lib<lib>/ so both styles resolve.
    inc_dst = SYSROOT / "include" / lib
    inc_dst_alt = SYSROOT / "include" / name  # with "lib" prefix

    if out_lib.exists() and not force:
        newest_src = max(find_sources(src_dir), default=None,
                         key=lambda p: p.stat().st_mtime)
        if newest_src and newest_src.stat().st_mtime < out_lib.stat().st_mtime:
            print(f"[skip] {name}: up-to-date")
            return

    inc_dst.mkdir(parents=True, exist_ok=True)
    out_obj.mkdir(parents=True, exist_ok=True)
    # Public headers: copy *.h from one of three layouts into sysroot/include/<libname>/
    #   "lib":   include/lib<name>/<name>/*.h      (libwapcaplet, libcss)
    #   "nolib": include/<name>/.../*.h           (parserutils, hubbub, dom, nslog, nsutils)
    #   "flat":  include/*.h                      (libnsfb)
    # Result: sysroot/include/<libname>/<subdir>/<file>.h, so consumers write
    # `#include <libname/foo.h>` matching NetSurf convention.
    inc_kind = LIBS[lib].get("inc_dir", "lib")
    if inc_kind == "lib":
        src_include = src_dir / "include" / name
        rel_prefix = name
    elif inc_kind == "nolib":
        src_include = src_dir / "include" / lib
        rel_prefix = lib
    else:  # flat
        src_include = src_dir / "include"
        rel_prefix = None
    if src_include.exists():
        for h in src_include.rglob("*.h"):
            rel = h.relative_to(src_include)
            # For "lib"/"nolib": rel is like <name>/foo.h or lib<name>/foo.h —
            # strip the leading lib segment so we end up with just foo.h.
            if rel_prefix and rel.parts[0] == rel_prefix:
                rel = Path(*rel.parts[1:]) if len(rel.parts) > 1 else Path(rel.name)
            if not str(rel):
                rel = Path(h.name)
            for dst in (inc_dst, inc_dst_alt):
                if dst == inc_dst_alt and inc_dst == inc_dst_alt:
                    continue  # no point installing twice to the same dir
                target = dst / rel
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(h, target)
    # Internal headers (in src/) are referenced via #include "foo.h", so add
    # src/ to the include path during compilation — no copy needed.

    # Pre-build: generate code that NetSurf's Makefiles would normally build
    # via PERL/FLEX/BISON. Outputs land in src/ so find_sources() picks them up.
    pre_build(name, src_dir)

    sources = find_sources(src_dir)
    print(f"[build] {name}: {len(sources)} .c files")
    objs = []
    for src in sources:
        rel_obj = out_obj / (str(src.relative_to(src_dir)).replace("/", "_") + ".o")
        objs.append(str(rel_obj))
        cmd = [
            ZIG, "cc", "-target", TARGET, "-c", "-O2", "-std=c99",
            "-Wall", "-Wno-unused-parameter", "-Wno-unused-variable",
            *LIBS[lib]["cflags"],
            f"-I{src_dir}/include",
            f"-I{src_dir}/src",
            f"-I{SYSROOT}/include",
            str(src), "-o", str(rel_obj),
        ]
        run(cmd, capture_output=True)

    if out_lib.exists():
        out_lib.unlink()
    run([ZIG, "ar", "rc", str(out_lib), *objs], capture_output=True)
    size = out_lib.stat().st_size
    print(f"[ok] {name}: lib{lib}.a ({size:,} bytes, {len(objs)} objects)")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lib", help="build only this lib")
    ap.add_argument("--force", action="store_true", help="rebuild even if up-to-date")
    ap.add_argument("--check", action="store_true", help="verify toolchain and exit")
    ap.add_argument("--list", action="store_true", help="list build order")
    args = ap.parse_args()

    if args.check:
        r = run([ZIG, "version"], capture_output=True, text=True)
        print(f"zig: {r.stdout.strip()} ({ZIG})")
        print(f"target: {TARGET}")
        # Ensure libnetsurf-src symlink (auto-detect common paths)
        if not NETSURF_SRC.exists():
            ensure_source_link()
        if not NETSURF_SRC.exists() or not (NETSURF_SRC / "libwapcaplet").exists():
            print("ERROR: netsurf-all-3.11 source missing. Run one of:")
            print("  ln -s /path/to/netsurf-all-3.11 libnetsurf-src")
            print("  export NETSURF_SRC=/path/to/netsurf-all-3.11")
            sys.exit(1)
        print(f"netsurf source: {NETSURF_SRC}")

        ok = True
        # Required tools
        for tool, brew_pkg, apt_pkg in [
            ("perl",    "perl",   "perl"),
            ("flex",    "flex",   "flex"),
            ("gperf",   "gperf",  "gperf"),
        ]:
            if shutil.which(tool):
                ver = subprocess.run([tool, "--version"], capture_output=True,
                                    text=True).stdout.splitlines()[0]
                print(f"{tool}: OK ({ver})")
            else:
                print(f"{tool}: MISSING (brew install {brew_pkg} | "
                      f"apt install {apt_pkg})")
                ok = False
        # Bison: prefer Homebrew 3.x; warn if system Bison is too old.
        # Many macOS systems have only the ancient /usr/bin/bison 2.3 which
        # lacks %destructor. The NetSurf .y files require >= 2.6.
        bison = None
        for c in ("/opt/homebrew/opt/bison/bin/bison",
                  "/usr/local/opt/bison/bin/bison"):
            if Path(c).exists():
                bison = c
                break
        if not bison:
            bison = shutil.which("bison")
        if not bison:
            for c in ("/opt/homebrew/opt/bison/bin/bison",
                      "/usr/local/opt/bison/bin/bison"):
                if Path(c).exists():
                    bison = c
                    break
        if bison:
            ver = subprocess.run([bison, "--version"], capture_output=True,
                                text=True).stdout.splitlines()[0]
            # Parse "bison (GNU Bison) 3.8.2" → (3, 8). Need >= 2.6.
            major = minor = 0
            for tok in ver.split():
                parts = tok.split(".")
                if parts[0].isdigit():
                    major = int(parts[0])
                    minor = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
                    break
            ok_ver = (major, minor) >= (2, 6)
            status = "OK" if ok_ver else "TOO OLD (need >= 2.6)"
            print(f"bison: {status} ({ver})")
            if not ok_ver: ok = False
        else:
            print("bison: MISSING (brew install bison | apt install bison)")
            ok = False

        print()
        print("OK: toolchain + source layout verified" if ok
              else "ERROR: missing prerequisites")
        sys.exit(0 if ok else 1)

    if args.list:
        print("Build order (deps first):")
        for lib in LIBS:
            deps = LIBS[lib]["deps"]
            print(f"  {lib:14s} <- {', '.join(deps) if deps else '(none)'}")
        return

    if args.lib:
        if not NETSURF_SRC.exists():
            ensure_source_link()
        if not NETSURF_SRC.exists():
            print("ERROR: netsurf-all-3.11 source not found.", file=sys.stderr)
            print("  Set NETSURF_SRC or `ln -s` the source dir as libnetsurf-src.",
                  file=sys.stderr)
            sys.exit(1)
        build_one(args.lib, force=args.force)
    else:
        if not NETSURF_SRC.exists():
            ensure_source_link()
        if not NETSURF_SRC.exists():
            print("ERROR: netsurf-all-3.11 source not found.", file=sys.stderr)
            print("  Set NETSURF_SRC or `ln -s` the source dir as libnetsurf-src.",
                  file=sys.stderr)
            sys.exit(1)
        for lib in LIBS:
            build_one(lib, force=args.force)


if __name__ == "__main__":
    main()
