# RomCloud Development Guidelines for AI Agents

All AI agents working on this codebase must adhere strictly to the established architectural decisions and verified hardware behaviors documented in [.agents/rules/app_conventions_and_standards.md](file://.agents/rules/app_conventions_and_standards.md) and the OTA release rules in [.agents/rules/release_ota.md](file://.agents/rules/release_ota.md).

## Critical Directives (DO NOT RE-TEST OR BREAK):
1. **Vietnamese Unicode Support is Complete:** The system font (`regular.ttf`) fully supports Vietnamese diacritics. Never write test scripts to verify font or suspect font rendering for Vietnamese text.
2. **String Truncation Rule:** NEVER use `std::string::substr()` by byte count to shorten strings containing user/Vietnamese text. ALWAYS use `truncateToWidth(text, font, maxWidthPx)`.
3. **Screen Resolution:** Hardcoded 1024x768 (4:3). Header is 64px, Footer is 53px, Content area is 651px (Y = 64..715).
4. **Button B Standard:** Button B is exclusively **Back / Cancel / Exit**. NEVER bind Button B to Backspace/Delete in virtual keyboards or text fields.
5. **Footer Bar:** `drawAppFooter()` handles its own background and line separator. NEVER draw redundant manual rectangles over `drawAppFooter()`.
6. **BrickPad Defaults (thay GameCast từ v2.6):**
   - Bluetooth Classic HID (PSM 0x11/0x13, SDP qua BlueZ ProfileManager).
   - Report/descriptor + VID/PID theo identity (Xbox/PlayStation/Switch/PC).
   - UI 1 màn duy nhất: trạng thái + thiết bị đã lưu. Chưa nối: nút điều
     khiển UI. Đã nối: nút đi thẳng ra host (chỉ F1 thoát được).
   - SELECT: overlay chọn Platform & Chế độ. F1: thoát HID (ngắt + rời màn).
   - Pair agent `NoInputNoOutput` (JustWorks). Đăng ký Profile KHÔNG truyền
     PSM (BlueZ build này double-bind EADDRINUSE — app tự bind cả 2 PSM).
   - Đổi mode nhanh: giữ SELECT+START 1s (phím FN1/FN2 không lộ qua SDL).
7. **Build & Deploy:**
   - Build: `./build.sh` (cross-compiles for `aarch64-linux-gnu.2.33`).
   - Binaries: `bin/RomCloud` (duy nhất — đã xóa `gamecast_d`).
   - Deploy máy test: `./deploy-adb.sh` (bung full-zip qua adb, giữ data/config/iptv user). CẤM chỉ `adb push bin/*` — máy test sẽ lệch file runtime (launch.sh/scripts/assets/lib).
8. **TrimUI Brick Chin Buttons & Icons Standard:**
   - Order (Left to Right): **MENU - SELECT - START**.
   - Input mapping: Left (`GUIDE` / Joy 8) = `Button::MENU`, Middle (`BACK` / Joy 6) = `Button::SELECT`, Right (`START` / Joy 7) = `Button::START`. NEVER swap BACK and GUIDE.
   - Icons: Use dedicated `assets/button_icons/MENU.png`, `SELECT.png`, and `START.png`. NEVER use legacy generic icons (`options.png`, `view.png`, `start_icon.png`).
   - Footer hints: Place `SELECT` before `START` to match physical layout.
