# RomCloud OTA Release Rules (BẮT BUỘC — sự cố v2.3.0)

Bài học từ sự cố v2.3.0: release chỉ gồm binary 2.33MB + `mpv_bundle.zip`
khiến user OTA bị lỗi YouTube (thiếu `scripts/youtube_search.py` mới) và
mất icon (thiếu `assets/button_icons/SELECT.png`, `MENU.png`, `START.png`),
vì OTA chỉ thay binary, còn script/asset mới không tới được máy user.
Mọi agent làm release BẮT BUỘC tuân thủ các quy tắc dưới đây.

## 1. Chỉ phát hành ĐÚNG 1 file zip duy nhất

- Mỗi version chỉ upload **1 asset**: `RomCloud-vX.Y.Z.zip` (full package do
  `package.sh` sinh ra, chứa `Apps/RomCloud/...` đầy đủ).
- **CẤM** upload thêm: `RomCloud-Lite-Installer.zip`, `mpv_bundle.zip`,
  `bundle-*.zip`, `RomCloud-Install-To-Apps.zip`, hay bất kỳ file linh tinh
  nào khác — trừ khi user yêu cầu rõ ràng.
- **Ngoại lệ duy nhất (đã duyệt cho v2.3.2):** asset binary cầu `RomCloud`
  kèm `binary_url` trong `version.json`, để máy đời cũ (code OTA chỉ biết
  tải binary) bước qua được rồi tự vá full-zip khi boot
  (`UpdateManager::repairIfBroken`). Từ version sau cầu này thì quay lại
  1 zip, không duy trì.
- `dist/` local cũng chỉ giữ zip của version hiện tại, xóa các zip version
  cũ để tránh upload nhầm.

## 2. Zip release phải bao gồm TẤT CẢ những gì đã cập nhật

- Không bao giờ release "chỉ binary". Mọi file runtime thay đổi trong
  version này đều phải có trong zip: `bin/*` (RomCloud, gamecast_d,
  mpv, yt-dlp...), `lib/*`, `scripts/*`, `assets/**/*` (icons, button_icons,
  player_icons, fonts...), `config/*` mặc định, `launch.sh`, `config.json`,
  `icon.png`.
- Cách kiểm tra: liệt kê file runtime đổi từ version trước
  (`git diff --name-only <tag-cũ> HEAD`, lọc các path runtime trên) rồi
  đối chiếu từng file có trong zip (`unzip -l`). Thiếu 1 file là FAIL,
  không được publish.
- Binary trong zip phải là bản **stripped** (`RELEASE=1 ./build.sh`,
  `package.sh` đã có gate cảnh báo "not stripped" — thấy warning là dừng).

## 3. version.json và code OTA phải khớp nhau TUYỆT ĐỐI

- `APP_VERSION` (`src/ota/UpdateManager.h`) == `"version"` trong
  `version.json` == tag release (`vX.Y.Z`). Lệch là OTA loop hoặc không
  thấy update.
- Mọi URL trong `version.json` (`binary_url`, `bundle_url`,
  `*_bundle_url`, `icon_url`) phải **tồn tại thật trên release** — verify
  bằng `gh release view <tag>` hoặc `curl -fI` từng URL trước khi publish.
  URL 404 (như `bundle-STOCK_PS.zip` của v2.3.0) là FAIL.
- Changelog trong `version.json` là nguồn feed release notes cho màn hình
  Cập nhật (trống thì máy hiện trống — không chế text giả).

## 4. Quy trình publish (đúng thứ tự)

1. `RELEASE=1 ./build.sh` (ra binary stripped).
2. `./package.sh` (ra `dist/RomCloud-vX.Y.Z.zip` duy nhất).
3. Verify zip: `unzip -l`, đối chiếu file đổi (mục 2), check stripped.
4. Gắn tag + tạo release **chỉ với 1 zip đó**.
5. Update `version.json` trên main, verify từng URL (mục 3).
6. Test trên máy thật qua adb (`adb push`, mở app kiểm tra) trước khi
  báo user — đặc biệt các tính năng động tới file ngoài binary
  (YouTube, IPTV, icon mới, GameCast).
