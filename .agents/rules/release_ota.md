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
- **Ngoại lệ RC-Web (đã duyệt):** asset `rc-web-<rc_web_version>.zip`
  (~200M, do `package-rc-web.sh` sinh ra từ `~/Downloads/rc-web`) được upload
  KÈM release khi `rc_web_version` trong `version.json` đổi. Đây là gói
  trình duyệt riêng cho OTA thủ công (màn Cập nhật → X), không nằm trong
  full-zip app. Không đổi rc-web thì không upload asset này.
- **Ngoại lệ OTA delta (đã duyệt, từ 2.5.2):** mỗi release được kèm thêm
  3 linh kiện máy + manifest (do `package.sh` + `tools/mkota.py` sinh ra từ
  cùng 1 staging): `ota-core-vX.Y.Z.zip`, `ota-media-vX.Y.Z.zip`,
  `ota-patch-vX.Y.Z.zip`, `ota-manifest.json`. NGUYÊN TẮC ĐẶT TÊN: chỉ bản
  nào cài-chạy-OTA được mới mang tên `RomCloud` (full-zip duy nhất);
  linh kiện máy mang prefix `ota-` để user nhìn là biết không tải tay.
  (v2.5.2 dùng tên cũ `RomCloud-v*-{lite,completion,delta}.zip` — đã đổi
  ngay trên release đó; từ v2.5.3 dùng tên mới.) Đây là 1 cơ chế, không
  phải gói tùy chọn — không tính vào giới hạn 1 zip: full-zip vẫn bắt
  buộc (cài mới + fallback khi delta hỏng + cầu di trú cho máy cũ).
  Máy chỉ áp delta khi verify hash từng file xong, hỏng là rớt về
  full-zip (giữ bài học v2.3.0: không bao giờ vá nửa vời).
- **Ngoại lệ duy nhất (đã duyệt cho v2.3.2, đã xong):** asset binary cầu `RomCloud`
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
- `version.json` BẮT BUỘC có đủ fragment OTA do `tools/mkota.py` in ra
  (`ota_manifest_url`, `delta_zip_url`, `lite_zip_url`,
  `completion_zip_url`). Máy hiện tại tự suy URL theo quy ước khi thiếu,
  nhưng đó chỉ là fallback — đổi tên file là OTA gãy im lặng.
- Bump version PHẢI gồm cả `os_versions` (STOCK_PS/NEXTUI/SPRUCE_OS):
  code máy ưu tiên version theo OS (`checkForUpdatesSync` đọc key OS
  trước, `JsonHelper` tìm thấy key lồng trong `os_versions`) — sự cố
  v2.5.3: `version` đã 2.5.3 nhưng `os_versions` còn 2.5.2 nên máy báo
  "đã mới nhất", bấm kiểm tra cũng im lặng.
- Changelog trong `version.json` là nguồn feed release notes cho màn hình
  Cập nhật (trống thì máy hiện trống — không chế text giả).

## 3b. Release notes BẮT BUỘC có banner hướng dẫn tải (sự cố v2.5.2)

- Bài học v2.5.2: `completion.zip` 22 lượt tải nhầm vì tên nghe như "bản đầy
  đủ nhất" + to nhất (68.8MB > full 58.7MB), trong khi body release không
  có 1 chữ hướng dẫn. Mọi release BẮT BUỘC mở đầu body bằng banner sau
  (thay X.Y.Z + dung lượng thật từ `gh release view`):
  ```markdown
  > ⬇️ **TẢI FILE NÀO? — User mới / cài mới chỉ tải đúng 1 file:**
  > **`RomCloud-vX.Y.Z.zip` (NN.N MB)** — bản đầy đủ duy nhất mang tên RomCloud, chép vào máy là chạy.
  > Các file `ota-*` là linh kiện máy tự tải khi bấm Cập nhật, KHÔNG tải tay:
  > | File | Để làm gì (máy tự dùng) |
  > |---|---|
  > | `ota-core-vX.Y.Z.zip` | Gói lõi cho máy tự cập nhật |
  > | `ota-patch-vX.Y.Z.zip` | Miếng vá phần đổi cho máy tự cập nhật |
  > | `ota-media-vX.Y.Z.zip` | ⚠️ KHÔNG phải bản đầy đủ! Chỉ là phần media vá cho máy core |
  > | `ota-manifest.json` | Danh sách file để máy tự kiểm tra cập nhật |
  > Máy đang dùng bản cũ thì **không cần tải gì cả**: mở app → Cập nhật → máy tự lo.
  ```
- Không được đổi tên 4 file máy (`core/patch/media/manifest`) nếu chưa
  sửa đồng thời `UpdateManager.cpp` (tự suy URL theo tên), 2 script
  `docs/installers/cai-dat-*` + test máy thật.

## 4. Quy trình publish (đúng thứ tự)

1. `RELEASE=1 ./build.sh` (ra binary stripped).
2. `./package.sh` (ra `dist/RomCloud-vX.Y.Z.zip` duy nhất).
3. Verify zip: `unzip -l`, đối chiếu file đổi (mục 2), check stripped.
4. Gắn tag + tạo release **chỉ với 1 zip đó**.
5. Update `version.json` trên main (kèm đủ fragment OTA của `mkota.py`),
   verify từng URL (mục 3); body release mở đầu bằng banner tải file (mục 3b).
6. Test trên máy thật qua adb (`adb push`, mở app kiểm tra) trước khi
  báo user — đặc biệt các tính năng động tới file ngoài binary
  (YouTube, IPTV, icon mới, GameCast).
