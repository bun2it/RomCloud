# RomCloud Development Guidelines for AI Agents

All AI agents working on this codebase must adhere strictly to the established architectural decisions and verified hardware behaviors documented in [.agents/rules/app_conventions_and_standards.md](file://.agents/rules/app_conventions_and_standards.md) and the OTA release rules in [.agents/rules/release_ota.md](file://.agents/rules/release_ota.md).

## Critical Directives (DO NOT RE-TEST OR BREAK):
1. **Vietnamese Unicode Support is Complete:** The system font (`regular.ttf`) fully supports Vietnamese diacritics. Never write test scripts to verify font or suspect font rendering for Vietnamese text.
2. **String Truncation Rule:** NEVER use `std::string::substr()` by byte count to shorten strings containing user/Vietnamese text. ALWAYS use `truncateToWidth(text, font, maxWidthPx)`.
3. **Screen Resolution:** Hardcoded 1024x768 (4:3). Header is 64px, Footer is 53px, Content area is 651px (Y = 64..715).
4. **Button B Standard:** Button B is exclusively **Back / Cancel / Exit**. NEVER bind Button B to Backspace/Delete in virtual keyboards or text fields.
5. **Footer Bar:** `drawAppFooter()` handles its own background and line separator. NEVER draw redundant manual rectangles over `drawAppFooter()`.
6. **GameCast Defaults:**
   - Port 8090 (never 6666).
   - Default resolution: 512x384 @ 60 FPS Turbo Mode (`m_nativeRes = false`).
   - TCP Send Buffer: 32KB (`SO_SNDBUF = 32768`).
   - Do NOT initialize `/dev/uinput` on daemon boot (only lazy-init on actual web gamepad button press).
7. **Build & Deploy:**
   - Build: `./build.sh` (cross-compiles for `aarch64-linux-gnu.2.33`).
   - Binaries: `bin/RomCloud` and `bin/gamecast_d`.
   - Deploy: `adb push bin/RomCloud ... && adb push bin/gamecast_d ... && adb shell sync`.
8. **TrimUI Brick Chin Buttons & Icons Standard:**
   - Order (Left to Right): **MENU - SELECT - START**.
   - Input mapping: Left (`GUIDE` / Joy 8) = `Button::MENU`, Middle (`BACK` / Joy 6) = `Button::SELECT`, Right (`START` / Joy 7) = `Button::START`. NEVER swap BACK and GUIDE.
   - Icons: Use dedicated `assets/button_icons/MENU.png`, `SELECT.png`, and `START.png`. NEVER use legacy generic icons (`options.png`, `view.png`, `start_icon.png`).
   - Footer hints: Place `SELECT` before `START` to match physical layout.
