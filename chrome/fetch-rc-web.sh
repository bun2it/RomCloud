#!/bin/bash
# fetch-rc-web.sh — ráp gói RC-Web của RomCloud trên máy Mac (chạy 1 lần).
#
# Gói này do RomCloud tự đóng gói: chỉ gồm thành phần mã nguồn mở lấy
# thẳng từ nơi phát hành gốc, cộng code của chính RomCloud:
#   - Electron 22.3.27 arm64 ....... electron/electron (GitHub releases)
#   - Thư viện hệ thống + Weston .... Debian bookworm arm64 (.deb gốc)
#   - gptokeyb2 + libinterpose ..... PortsMaster/PortMaster-GUI (releases)
#   - gamecontrollerdb.txt ......... SDL_GameControllerDB (upstream)
#   - launch.sh / browser.gptk / fonts.conf / app/ ... code của RomCloud
#     (nằm trong thư mục chrome/ của repo này)
# Binary KHÔNG commit vào git.
#
# Sau khi chạy xong, đẩy lên máy:
#   adb push ~/Downloads/rc-web /mnt/SDCARD/Apps/RomCloud/rc-web
set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HOME/Downloads/rc-web}"
ELECTRON_VER="22.3.27"
DEB_SUITE="${DEB_SUITE:-bullseye}"  # máy đích glibc 2.33 -> bullseye (cần 2.31); bookworm cần 2.36
DEB_MIRROR="https://deb.debian.org/debian"
PORTMASTER_TAG="2026.10.03-1041"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"/syslibs "$OUT"/weston/bin "$OUT"/weston/share \
         "$OUT"/weston/lib/aarch64-linux-gnu \
         "$OUT"/bin "$OUT"/app "$OUT"/fonts "$OUT"/licenses
cd "$OUT"

echo "=== 1/6 Electron $ELECTRON_VER linux-arm64 (electronjs.org) ==="
if [ ! -x "$OUT/electron/electron" ]; then
  curl -fL --progress-bar \
    "https://github.com/electron/electron/releases/download/v${ELECTRON_VER}/electron-v${ELECTRON_VER}-linux-arm64.zip" \
    -o "$TMP/electron.zip"
  mkdir -p "$OUT/electron"
  unzip -q -o "$TMP/electron.zip" -d "$OUT/electron"
  chmod +x "$OUT/electron/electron" "$OUT/electron/chrome_crashpad_handler" 2>/dev/null || true
else
  echo "Đã có electron/, bỏ qua."
fi
# License Electron: copy MỖI lần (không nằm trong nhánh skip ở trên —
# chạy lại script khi electron/ đã có vẫn đảm bảo đủ license).
cp -f "$OUT/electron/LICENSE" "$OUT/licenses/LICENSE.Electron.txt" 2>/dev/null || true

echo "=== 2/6 Thư viện Debian $DEB_SUITE arm64 cho Electron ==="
curl -fsSL "$DEB_MIRROR/dists/$DEB_SUITE/main/binary-arm64/Packages.gz" \
  -o "$TMP/Packages.gz"
# Tra Filename của một package trong index Debian.
deb_path() {
  gzip -dc "$TMP/Packages.gz" | awk -v want="$1" '
    /^Package: /  { p = $2 }
    /^Filename: / { if (p == want) { print $2; exit } }'
}
# deb_extract <pkg> <thư mục đích>: giải nén data.tar của gói .deb.
deb_extract() {
  local pkg="$1" dest="$2" rel deb data
  rel="$(deb_path "$pkg")"
  if [ -z "$rel" ]; then echo "  !! Không thấy gói $pkg trong index $DEB_SUITE"; return 1; fi
  deb="$TMP/$pkg.deb"
  curl -fsSL "$DEB_MIRROR/$rel" -o "$deb"
  data="$(ar t "$deb" | grep '^data\.tar' | head -1)"
  mkdir -p "$dest/$pkg"
  ar p "$deb" "$data" | tar -xf - -C "$dest/$pkg"
}
# Nhóm .so chạy Electron: cây Depends đầy đủ của bộ GTK/NSS trên bullseye
# (tính từ index Debian — loader cần gì có nấy, khỏi mò từng lib thiếu).
# Đã cắt các module không bao giờ nạp (codec video, RDP, driver GL).
SYS_PKGS="libacl1 libapparmor1 libargon2-1 libasound2 libasound2-data \
libatk-bridge2.0-0 libatk1.0-0 libatk1.0-data libatspi2.0-0 libaudit-common \
libaudit1 libavahi-client3 libavahi-common-data libavahi-common3 libblkid1 \
libbrotli1 libbsd0 libbz2-1.0 libcairo-gobject2 libcairo2 libcap-ng0 \
libcap2 libcolord2 libcom-err2 libcrypt1 libcryptsetup12 libcups2 \
libdatrie1 libdbus-1-3 libdconf1 libdeflate0 libdevmapper1.02.1 \
libdrm-common libdrm2 libegl-mesa0 libegl1 libepoxy0 libexpat1 libffi7 \
libfontconfig1 libfreetype6 libfribidi0 libgbm1 libgdk-pixbuf-2.0-0 \
libgdk-pixbuf2.0-common libglapi-mesa libgles2 libglib2.0-0 libglvnd0 \
libgmp10 libgnutls30 libgpg-error0 libgraphite2-3 libgssapi-krb5-2 \
libgtk-3-0 libgtk-3-common libharfbuzz0b libhogweed6 libicu67 libidn2-0 \
libip4tc2 libjbig0 libjpeg62-turbo libjson-c5 libjson-glib-1.0-0 \
libjson-glib-1.0-common libk5crypto3 libkeyutils1 libkmod2 libkrb5-3 \
libkrb5support0 liblcms2-2 liblz4-1 liblzma5 libmd0 libmount1 libnettle8 \
libnspr4 libnss3 libp11-kit0 libpam-modules libpam-runtime \
libpam-systemd libpam0g libpango-1.0-0 libpangocairo-1.0-0 \
libpangoft2-1.0-0 libpcre2-8-0 libpcre3 libpixman-1-0 libpng16-16 \
libproxy1v5 libpsl5 librest-0.7-0 libseccomp2 libselinux1 \
libsemanage-common libsemanage1 libsepol1 libsoup-gnome2.4-1 \
libsoup2.4-1 libsqlite3-0 libssl1.1 libstdc++6 libsystemd0 libtasn1-6 \
libthai-data libthai0 libtiff5 libudev1 libunistring2 libuuid1 \
libwayland-client0 libwayland-cursor0 libwayland-egl1 libwayland-server0 \
libwebp6 libx11-6 libx11-data libx11-xcb1 libxau6 libxcb-dri2-0 \
libxcb-dri3-0 libxcb-present0 libxcb-render0 libxcb-shm0 libxcb-sync1 \
libxcb-xfixes0 libxcb1 libxcomposite1 libxcursor1 libxdamage1 libxdmcp6 \
libxext6 libxfixes3 libxi6 libxinerama1 libxkbcommon0 libxml2 libxrandr2 \
libxrender1 libxshmfence1 libzstd1 zlib1g"
for pkg in $SYS_PKGS; do
  deb_extract "$pkg" "$TMP/sys" || { echo "  !! bỏ qua gói $pkg (không tải/giải nén được)"; continue; }
  find "$TMP/sys/$pkg" -name '*.so*' -type f -exec cp -f {} "$OUT/syslibs/" \; 2>/dev/null || true
  find "$TMP/sys/$pkg" -name '*.so*' -type l -exec cp -f {} "$OUT/syslibs/" \; 2>/dev/null || true
done
echo "  syslibs: $(ls "$OUT/syslibs" | wc -l | tr -d ' ') file"

# gdk-pixbuf (GTK cua Electron) chi nap loader anh tu mot thu muc rieng
# kem file cache; syslibs dang phang nen gom loader vao pixbuf-loaders/
# + cache viet san (duong dan tren may co dinh /mnt/SDCARD/Apps/RomCloud).
mkdir -p "$OUT/syslibs/pixbuf-loaders"
cp -f "$OUT/syslibs"/libpixbufloader-*.so "$OUT/syslibs/pixbuf-loaders/" 2>/dev/null || true
cp -f "$SCRIPT_DIR/pixbuf-loaders.cache" "$OUT/syslibs/pixbuf-loaders/loaders.cache" 2>/dev/null || true
echo "  pixbuf-loaders: $(ls "$OUT/syslibs/pixbuf-loaders" | wc -l | tr -d ' ') file (tinh ca loaders.cache)"

echo "=== 3/6 Weston từ Debian $DEB_SUITE arm64 (cây weston/ riêng) ==="
# Cây tây nguyên bản /usr của Debian gom vào $OUT/weston:
#   bin/ (weston, Xwayland, seatd), lib/aarch64-linux-gnu/, share/ (xkb...)
# Cây Depends đầy đủ của weston 9 + Xwayland trên bullseye (cùng cách tính
# như nhóm Electron; đã cắt module RDP/codec/driver GL không bao giờ nạp).
WESTON_PKGS="weston libweston-9-0 xwayland xkb-data \
libaudit-common libaudit1 libblkid1 libbrotli1 libbsd0 libbz2-1.0 \
libcairo2 libcap-ng0 libcolord2 libcrypt1 libdatrie1 libdbus-1-3 \
libdrm-common libdrm2 libegl-mesa0 libegl1 libegl1-mesa libepoxy0 \
libevdev2 libexpat1 libffi7 libfontconfig1 libfontenc1 libfreetype6 \
libfribidi0 libgbm1 libgcrypt20 libgl1 libglapi-mesa libgles2 \
libglib2.0-0 libglvnd0 libglx-mesa0 libglx0 libgpg-error0 \
libgraphite2-3 libgudev-1.0-0 libharfbuzz0b libice6 libinput-bin \
libinput10 libjpeg62-turbo liblcms2-2 liblzma5 libmd0 libmount1 \
liblz4-1 libmtdev1 libpam-modules libpam0g libpango-1.0-0 libpangocairo-1.0-0 \
libpangoft2-1.0-0 libpcre2-8-0 libpcre3 libpixman-1-0 libpng16-16 \
libselinux1 libsemanage-common libsemanage1 libsepol1 libsm6 \
libsystemd0 libthai-data libthai0 libudev1 libunwind8 libuuid1 \
libva-drm2 libva2 libwacom-common libwacom2 libwayland-client0 \
libwayland-cursor0 libwayland-egl1 libwayland-server0 libwebp6 \
libx11-6 libx11-data libx11-xcb1 libxau6 libxaw7 libxcb-composite0 \
libxcb-dri2-0 libxcb-dri3-0 libxcb-glx0 libxcb-present0 libxcb-render0 \
libxcb-shm0 libxcb-sync1 libxcb-xfixes0 libxcb-xkb1 libxcb1 libxcursor1 \
libxdamage1 libxdmcp6 libxext6 libxfixes3 libxfont2 libxkbcommon0 \
libxkbfile1 libxmu6 libxpm4 libxrender1 libxshmfence1 libxt6 \
libxxf86vm1 libzstd1 zlib1g x11-xkb-utils"
for pkg in $WESTON_PKGS; do
  deb_extract "$pkg" "$TMP/wes" || continue
  src="$TMP/wes/$pkg/usr"
  [ -d "$src/bin" ]  && cp -Rf "$src/bin"/.  "$OUT/weston/bin/"   2>/dev/null || true
  [ -d "$src/sbin" ] && cp -Rf "$src/sbin"/. "$OUT/weston/bin/"   2>/dev/null || true
  [ -d "$src/lib/aarch64-linux-gnu" ] && \
    cp -Rf "$src/lib/aarch64-linux-gnu"/. "$OUT/weston/lib/aarch64-linux-gnu/" 2>/dev/null || true
  # Một số gói nền của Debian (libpcre3, libselinux1, libmount1...) đặt
  # thư viện ở /lib thay vì /usr/lib — không gom là loader thiếu file.
  [ -d "$TMP/wes/$pkg/lib/aarch64-linux-gnu" ] && \
    cp -Rf "$TMP/wes/$pkg/lib/aarch64-linux-gnu"/. "$OUT/weston/lib/aarch64-linux-gnu/" 2>/dev/null || true
  [ -d "$src/share" ] && cp -Rf "$src/share"/. "$OUT/weston/share/" 2>/dev/null || true
  [ -d "$src/lib" ] && cp -Rf "$src/lib"/. "$OUT/weston/lib/" 2>/dev/null || true
done
chmod +x "$OUT/weston/bin/weston" "$OUT/weston/bin/Xwayland" "$OUT/weston/bin/seatd" 2>/dev/null || true
echo "  weston/bin: $(ls "$OUT/weston/bin" 2>/dev/null | head -20 | tr '\n' ' ')"
echo "  weston/lib: $(find "$OUT/weston/lib" -type f 2>/dev/null | wc -l | tr -d ' ') file, $(find "$OUT/weston/lib" -type d -name 'libweston-*' 2>/dev/null | tr '\n' ' ')"

echo "=== 4/6 gptokeyb2 + libinterpose (PortMaster, mã nguồn mở) ==="
if [ ! -x "$OUT/bin/gptokeyb2" ]; then
  curl -fL --progress-bar \
    "https://github.com/PortsMaster/PortMaster-GUI/releases/download/$PORTMASTER_TAG/PortMaster.zip" \
    -o "$TMP/PortMaster.zip"
  unzip -q -o "$TMP/PortMaster.zip" 'PortMaster/gptokeyb2' 'PortMaster/libinterpose.aarch64.so' -d "$TMP/pm"
  cp "$TMP/pm/PortMaster/gptokeyb2" "$OUT/bin/gptokeyb2"
  cp "$TMP/pm/PortMaster/libinterpose.aarch64.so" "$OUT/bin/"
  chmod +x "$OUT/bin/gptokeyb2"
else
  echo "Đã có bin/gptokeyb2, bỏ qua."
fi
if [ ! -f "$OUT/gamecontrollerdb.txt" ]; then
  curl -fsSL "https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/master/gamecontrollerdb.txt" \
    -o "$OUT/gamecontrollerdb.txt"
fi

echo "=== 5/6 Font (DejaVu, từ Debian) ==="
if [ -z "$(ls "$OUT/fonts"/*.ttf 2>/dev/null)" ]; then
  deb_extract fonts-dejavu-core "$TMP/font"
  find "$TMP/font/fonts-dejavu-core" -name '*.ttf' -exec cp -f {} "$OUT/fonts/" \; 2>/dev/null || true
fi

echo "=== 6/6 Code của RomCloud (chrome/) ==="
cp -f "$SCRIPT_DIR/launch.sh" "$SCRIPT_DIR/browser.gptk" "$SCRIPT_DIR/fonts.conf" "$OUT/"
cp -f "$SCRIPT_DIR/app-main.js"        "$OUT/app/main.js"
cp -f "$SCRIPT_DIR/app-preload.js"     "$OUT/app/preload.js"
cp -f "$SCRIPT_DIR/app-keyboard.html"  "$OUT/app/keyboard.html"
cp -f "$SCRIPT_DIR/app-package.json"   "$OUT/app/package.json"
chmod +x "$OUT/launch.sh"
# License các thành phần bên thứ ba (MIT/DejaVu yêu cầu kèm license text).
# Tải mỗi lần ráp để gói luôn đủ giấy tờ, kể cả chạy lại script.
curl -fsSL "https://raw.githubusercontent.com/PortsMaster/PortMaster-GUI/main/LICENSE" \
  -o "$OUT/licenses/LICENSE.PortMaster.txt" 2>/dev/null || \
  echo "  !! không tải được LICENSE PortMaster (giữ bản cũ nếu có)"
curl -fsSL "https://raw.githubusercontent.com/dejavu-fonts/dejavu-fonts/master/LICENSE" \
  -o "$OUT/licenses/LICENSE.DejaVu.txt" 2>/dev/null || \
  echo "  !! không tải được LICENSE DejaVu (giữ bản cũ nếu có)"
# Dọn file chết: rc-main.js + package.json ở gốc là tàn dư thử nghiệm
# offscreen cũ (không ai dùng — launcher chạy $OUT/app), .DS_Store của Mac.
rm -f "$OUT/rc-main.js" "$OUT/package.json"
find "$OUT" -name ".DS_Store" -delete 2>/dev/null || true
# version.txt: máy đối chiếu với rc_web_version trong version.json để biết
# có bản RC-Web mới qua OTA không. Đọc từ repo (script chạy từ repo).
RCVER="$(grep '"rc_web_version"' "$SCRIPT_DIR/../version.json" 2>/dev/null | head -n 1 | awk -F'"' '{print $4}')"
if [ -n "$RCVER" ]; then
  echo "$RCVER" > "$OUT/version.txt"
  echo "  version.txt = $RCVER"
else
  echo "  !! không đọc được rc_web_version từ version.json (giữ version.txt cũ nếu có)"
fi
cat > "$OUT/licenses/NOTICE.txt" <<EOF
RC-Web — goi trinh duyet cua RomCloud (https://github.com/bun2it/RomCloud).
Code cua RomCloud (launch.sh, browser.gptk, fonts.conf, app/, version.txt):
viet moi cho du an nay, khong lay tu phan mem khac.

Thanh phan ma nguon mo di kem (lay thang tu noi phat hanh goc):
- Electron $ELECTRON_VER linux-arm64 (https://github.com/electron/electron/releases/tag/v$ELECTRON_VER, MIT/BSD) — electron/ (LICENSE trong licenses/)
- Thu vien he thong Debian $DEB_SUITE arm64 ($DEB_MIRROR, LGPL/GPL/MIT) — syslibs/
- Weston 9 + Xwayland + seatd, Debian $DEB_SUITE arm64 (nguon nhu tren) — weston/ (copyright tung goi trong weston/share/doc/)
- gptokeyb2 + libinterpose.aarch64.so, PortMaster-GUI $PORTMASTER_TAG (https://github.com/PortsMaster/PortMaster-GUI, MIT) — bin/
- gamecontrollerdb.txt, SDL_GameControllerDB (https://github.com/mdqinc/SDL_GameControllerDB; upstream khong ghi license, dung theo huong dan cua upstream + ghi nguon tai day va trong file)
- Font DejaVu (https://dejavu-fonts.github.io, DejaVu Fonts License) — fonts/ (LICENSE trong licenses/)
EOF

# Thẻ SD (FAT/FAT32) không lưu được symlink: các gói .deb chứa rất nhiều
# symlink lib*.so.0 -> lib*.so.0.0.x; adb push lên thẻ biến chúng thành
# file text chứa tên đích (vài chục byte) -> loader báo "file too short".
# Thay mọi symlink trong gói bằng bản sao nội dung thật.
echo "=== Chuẩn hoá symlink thành file thật (để đẩy lên thẻ FAT) ==="
find "$OUT" -type l | while IFS= read -r l; do
  t="$l.deref.tmp"
  if cp -fL "$l" "$t" 2>/dev/null; then mv -f "$t" "$l"; else rm -f "$t"; echo "  !! không bỏ symlink được: $l"; fi
done
echo "  symlink còn lại: $(find "$OUT" -type l | wc -l | tr -d ' ') (cần 0)"

echo ""
echo "=== Xong. Kiểm tra nhanh ==="
ls "$OUT" && echo "" && ls -la "$OUT/electron/electron" "$OUT/launch.sh" "$OUT/bin/gptokeyb2" "$OUT/weston/bin/weston"
du -sh "$OUT"
echo ""
echo "Đẩy lên máy:"
echo "  adb push \"$OUT\" /mnt/SDCARD/Apps/RomCloud/rc-web"
echo "(Video/TikTok xem bằng mpv của app — browser không cần test video.)"
