// BrickPadUI — 1 màn hình duy nhất (trạng thái + thiết bị đã lưu) vẽ theo
// chuẩn RomCloud (1024x768, header 64 + footer 53, card x=24/w=976,
// highlight drawFocusRow, footer SELECT trước START, B = lùi,
// truncateToWidth cho chữ Việt).
// Nguyên tắc: D-pad + A (chọn) + B (lùi), focus sáng rõ, chữ lớn,
// SELECT mở overlay chọn Platform & Chế độ, F1 thoát HID.
// Chưa nối: nút chỉ điều khiển UI. Đã nối: nút đi thẳng ra host.
#include "../brickpad/BrickPadBt.h"
#include "../brickpad/BrickPadManager.h"
#include "../input/InputManager.h"
#include "../platform/PlatformInfo.h"
#include "../ui/SearchInputModal.h"
#include "../ui/UIManager.h"
#include "../ui/UiTheme.h"

namespace RomCloud {

using BP = BrickPad::BrickPadManager;
using BpScreen = BrickPad::BpScreen;

namespace {
// Geometry chuẩn (§9-§10 rules).
constexpr int kCardX = 24;
constexpr int kCardW = 976;
constexpr int kCardY = 114;
constexpr int kPad = 24;
} // namespace

void UIManager::handleBrickPadInput() {
  InputManager &input = InputManager::instance();
  BP &bp = BP::instance();

  // Toast từ manager (pair xong, nối/mất kết nối, đổi preset...).
  BrickPad::ToastMsg tm;
  while (bp.pollToast(tm)) {
    SDL_Color c = {0, 180, 216, 255};
    if (tm.kind == 1)
      c = {34, 197, 94, 255};
    else if (tm.kind == 2)
      c = {239, 68, 68, 255};
    else if (tm.kind == 3)
      c = {245, 158, 11, 255};
    showToast(tm.text, c, 2500);
  }
  // Kết quả pair (async).
  if (bp.pairBusy()) {
    // đang pair — input vẫn đi lại bình thường, B hủy về HOME.
  } else {
    bool ok;
    std::string msg;
    if (bp.pollPairResult(ok, msg)) {
      showToast(msg,
                ok ? SDL_Color{34, 197, 94, 255} : SDL_Color{239, 68, 68, 255},
                2500);
      bp.refreshDevs();
      if (ok) {
        bp.leaveScan();
        bp.goScreen(BpScreen::DEVICES);
        return;
      }
    }
  }

  auto up = input.isButtonJustPressed(Button::UP);
  auto down = input.isButtonJustPressed(Button::DOWN);
  auto left = input.isButtonJustPressed(Button::LEFT);
  auto right = input.isButtonJustPressed(Button::RIGHT);
  auto a = input.isButtonJustPressed(Button::A);
  auto b = input.isButtonJustPressed(Button::B);
  auto x = input.isButtonJustPressed(Button::X);
  auto y = input.isButtonJustPressed(Button::Y);
  auto start = input.isButtonJustPressed(Button::START);

  // Bàn phím ảo RomCloud đang mở (đổi tên thiết bị): route hết vào modal.
  if (bp.vkOpen()) {
    bool l1 = input.isButtonJustPressed(Button::L1);
    bool r1 = input.isButtonJustPressed(Button::R1);
    bool sel = input.isButtonJustPressed(Button::SELECT);
    auto res = SearchInputModal::handleInput(
        m_bpModalCfg, input.isButtonRepeat(Button::A), b, start, up, down, left,
        right, input.isButtonRepeat(Button::X), input.isButtonRepeat(Button::Y),
        l1, r1, sel);
    if (res == SearchInputModal::Result::Commit) {
      bp.setVkOpen(false);
      std::string q = m_bpVk.query;
      if (!m_bpRenameMac.empty()) {
        if (!q.empty()) {
          bool ok = bp.renameDevice(m_bpRenameMac, q);
          showToast(ok ? "Đã đổi tên thiết bị" : "Đổi tên thất bại",
                    ok ? SDL_Color{34, 197, 94, 255}
                       : SDL_Color{239, 68, 68, 255},
                    1500);
        }
        m_bpRenameMac.clear();
      } else if (!q.empty()) {
        // VK mở để gõ chữ ra host (chế độ Bàn phím): gửi qua HID.
        bp.typeText(q);
      }
    } else if (res == SearchInputModal::Result::Cancel) {
      bp.setVkOpen(false);
      m_bpRenameMac.clear();
    }
    return;
  }

  // F1: thoát HID.
  if (input.isFnLeftTriggered()) {
    bool wasConn = bp.connected();
    if (wasConn) {
      bp.disconnectHost();
      showToast("Đã thoát HID", {245, 158, 11, 255}, 1500);
    }
    setBpBacklightOff(false);
    m_bpSessionStartMs = 0;
    if (!goBack())
      setState(UIState::MENU);
    return;
  }

  // NẾU ĐANG KẾT NỐI (SESSION ĐANG CHẠY):
  // Tuyệt đối không để UI chiếm nút! Tất cả nút bấm (A, B, X, Y, D-pad, Analog,
  // Vai L/R, SELECT, START) được phát thẳng qua Bluetooth HID để điều khiển
  // game. Chỉ có nút F1 ở trên mới được xử lý để ngắt kết nối và thoát về Home.
  if (bp.connected()) {
    if (input.isFnRightTriggered()) {
      // Nút F2 đánh thức màn hình sáng lại 30s
      m_bpSessionStartMs = SDL_GetTicks();
      setBpBacklightOff(false);
    }
    return;
  }

  // Overlay chọn Platform & Chế độ & Tốc độ chuột (mở bằng SELECT): Up/Down
  // chuyển hàng, Trái/Phải hoặc A đổi giá trị, B đóng.
  if (bp.chooser() == 3) {
    auto curId = (!m_bpConfigMac.empty()) ? bp.store().deviceIdentity(m_bpConfigMac)
                                          : bp.identity();
    bool isXbox = (curId == BrickPad::ControllerIdentity::XBOX);
    int maxRows = isXbox ? 2 : 3;
    if (bp.chooserCursor() >= maxRows) {
      bp.setChooserCursor(0);
    }

    if (up) {
      int c = bp.chooserCursor() - 1;
      if (c < 0)
        c = maxRows - 1;
      bp.setChooserCursor(c);
    } else if (down) {
      int c = bp.chooserCursor() + 1;
      if (c >= maxRows)
        c = 0;
      bp.setChooserCursor(c);
    } else if (left || right || a) {
      if (bp.chooserCursor() == 0) {
        if (!m_bpConfigMac.empty()) {
          auto newId = (curId == BrickPad::ControllerIdentity::XBOX)
                           ? BrickPad::ControllerIdentity::PC_LAPTOP
                           : BrickPad::ControllerIdentity::XBOX;
          bp.store().setDeviceIdentity(m_bpConfigMac, newId);
          int newMode = (newId == BrickPad::ControllerIdentity::PC_LAPTOP) ? 1 : 0;
          bp.store().setDeviceMode(m_bpConfigMac, newMode);
          bp.store().lastPlatform = newId;
          bp.store().lastMode = newMode;
          bp.saveStore();
        } else {
          bp.cyclePlatform();
        }
        showToast("Thoát RomCloud vào lại để áp dụng thay đổi",
                  {245, 158, 11, 255}, 1800);
      } else if (bp.chooserCursor() == 1) {
        if (isXbox) {
          showToast("Xbox chỉ hỗ trợ chế độ Tay cầm",
                    {245, 158, 11, 255}, 1800);
        } else {
          if (!m_bpConfigMac.empty()) {
            int curMode = bp.store().deviceMode(m_bpConfigMac);
            int newMode = (curMode == 1 ? 0 : 1);
            bp.store().setDeviceMode(m_bpConfigMac, newMode);
            bp.store().lastMode = newMode;
            bp.saveStore();
          } else {
            bp.setMode(bp.mode() == BrickPad::PadMode::GAMEPAD
                           ? BrickPad::PadMode::KEYBOARD
                           : BrickPad::PadMode::GAMEPAD);
          }
          showToast("Thoát RomCloud vào lại để áp dụng thay đổi",
                    {245, 158, 11, 255}, 1800);
        }
      } else if (bp.chooserCursor() == 2 && !isXbox) {
        int delta = left ? -1 : 1;
        int s = bp.mouseSpeed() + delta;
        if (s < 1)
          s = 10;
        if (s > 10)
          s = 1;
        bp.setMouseSpeed(s);
      }
    } else if (b) {
      bp.setChooser(0);
      bp.saveStore();
      if (!m_bpConfigMac.empty()) {
        auto devId = bp.store().deviceIdentity(m_bpConfigMac);
        int devM = bp.store().deviceMode(m_bpConfigMac);
        bool sameCfg = (devId == bp.identity() &&
                        (devM == 1) == (bp.mode() == BrickPad::PadMode::KEYBOARD));
        if (!sameCfg) {
          showToast("Đã lưu. Thoát RomCloud vào lại để áp dụng cho thiết bị này!",
                    {245, 158, 11, 255}, 2500);
        } else {
          showToast("Đã lưu cấu hình thiết bị", {34, 197, 94, 255}, 1800);
        }
        m_bpConfigMac.clear();
      } else {
        showToast("Đã lưu cấu hình", {34, 197, 94, 255}, 1800);
      }
    }
    return;
  }

  auto select = input.isButtonJustPressed(Button::SELECT);
  auto menu = input.isButtonJustPressed(Button::MENU);

  switch (bp.screen()) {
  case BpScreen::SCAN: {
    auto devs = bp.scanDevs();
    int n = (int)devs.size();
    if (n > 0 && bp.cursor() >= n)
      bp.setCursor(0);
    int ci = bp.cursor();
    if (up && n > 0) {
      bp.setCursor((bp.cursor() + n - 1) % n);
      bp.forgetArm = -1;
    } else if (down && n > 0) {
      bp.setCursor((bp.cursor() + 1) % n);
      bp.forgetArm = -1;
    } else if (a && n > 0 && !bp.pairBusy()) {
      const auto &d = devs[(size_t)ci];
      if (d.paired) {
        BrickPad::ControllerIdentity devId = bp.store().deviceIdentity(d.mac);
        int devM = bp.store().deviceMode(d.mac);
        bool sameConfig = (devId == bp.identity() &&
                           (devM == 1) == (bp.mode() == BrickPad::PadMode::KEYBOARD));
        if (!sameConfig) {
          bp.store().lastPlatform = devId;
          bp.store().lastMode = devM;
          bp.saveStore();
          showToast("Thiết bị này dùng cấu hình khác. Thoát RomCloud vào lại để kết nối!",
                    {239, 68, 68, 255}, 3000);
        } else {
          std::string err;
          if (!bp.connectHost(d.mac, err))
            showToast(err, {239, 68, 68, 255}, 2500);
          else
            bp.goScreen(BpScreen::DEVICES);
        }
      } else {
        bp.startPair(d.mac, d.name);
        showToast("Đang pair — chờ thiết bị xác nhận...", {245, 158, 11, 255},
                  2500);
      }
    } else if (a && n == 0 && !bp.scanning()) {
      bp.enterScan();
    } else if (b) {
      bp.leaveScan();
      bp.goScreen(BpScreen::DEVICES);
    }
    break;
  }
  case BpScreen::DEVICES:
  default: {
    std::vector<int> rows;
    auto devs = bp.scanDevs();
    for (size_t i = 0; i < devs.size(); i++)
      if (devs[i].paired || devs[i].connected)
        rows.push_back((int)i);
    int n = (int)rows.size();
    if (n > 0 && bp.cursor() >= n)
      bp.setCursor(0);
    int ci = bp.cursor();

    if (up && n > 0) {
      bp.setCursor((bp.cursor() + n - 1) % n);
      bp.forgetArm = -1;
    } else if (down && n > 0) {
      bp.setCursor((bp.cursor() + 1) % n);
      bp.forgetArm = -1;
    } else if (a && n > 0) {
      // A: Kết nối
      const auto &d = devs[(size_t)rows[(size_t)ci]];
      if (d.connected) {
        bp.disconnectHost();
        showToast("Đã ngắt kết nối", {245, 158, 11, 255}, 1500);
      } else {
        BrickPad::ControllerIdentity devId = bp.store().deviceIdentity(d.mac);
        int devM = bp.store().deviceMode(d.mac);
        bool sameConfig = (devId == bp.identity() &&
                           (devM == 1) == (bp.mode() == BrickPad::PadMode::KEYBOARD));
        if (!sameConfig) {
          bp.store().lastPlatform = devId;
          bp.store().lastMode = devM;
          bp.saveStore();
          showToast("Thiết bị này dùng cấu hình khác. Thoát RomCloud vào lại để kết nối!",
                    {239, 68, 68, 255}, 3000);
        } else {
          bp.startConnect(d.mac);
        }
      }
    } else if (x && n > 0 && ci < n) {
      // X: Quên thiết bị (2 nhịp: arm rồi xác nhận).
      if (bp.forgetArm == ci) {
        const auto &d = devs[(size_t)rows[(size_t)ci]];
        bool ok = bp.removePaired(d.mac);
        showToast(ok ? "Đã quên thiết bị" : "Quên thiết bị thất bại",
                  ok ? SDL_Color{245, 158, 11, 255}
                     : SDL_Color{239, 68, 68, 255},
                  1500);
        bp.forgetArm = -1;
        if (bp.cursor() > 0)
          bp.setCursor(bp.cursor() - 1);
      } else {
        bp.forgetArm = ci;
        showToast("Bấm X lần nữa để quên thiết bị", {245, 158, 11, 255}, 1800);
      }
    } else if (y && n > 0 && ci < n) {
      // Y: Sửa tên (dùng SearchInputModal)
      const auto &d = devs[(size_t)rows[(size_t)ci]];
      m_bpRenameMac = d.mac;
      m_bpVk.query = d.name.empty() ? d.mac : d.name;
      m_bpModalCfg.ui = &m_ui;
      m_bpModalCfg.vk = &m_bpVk;
      m_bpModalCfg.history = &m_bpVkHist;
      m_bpModalCfg.fSmall = m_fontSmall;
      m_bpModalCfg.fMedium = m_fontMedium;
      m_bpModalCfg.fLarge = m_fontLarge;
      m_bpModalCfg.title = "ĐỔI TÊN THIẾT BỊ";
      m_bpModalCfg.placeholder = "Nhập tên mới...";
      m_bpModalCfg.actionLabels[4] = "Lưu";
      m_bpModalCfg.focusMode = 1;
      bp.setVkOpen(true);
    } else if (select) {
      // SELECT: mở overlay chọn Platform & Chế độ cho thiết bị đang chọn (nếu có)
      if (n > 0 && ci < n) {
        const auto &d = devs[(size_t)rows[(size_t)ci]];
        m_bpConfigMac = d.mac;
      } else {
        m_bpConfigMac.clear();
      }
      bp.setChooser(3);
    } else if (menu) {
      // MENU: Quét mới
      bp.enterScan();
      bp.goScreen(BpScreen::SCAN);
    } else if (b) {
      if (bp.forgetArm >= 0)
        bp.forgetArm = -1;
      else if (!goBack())
        setState(UIState::MENU);
    }
    break;
  }
  }
}

} // namespace RomCloud

namespace RomCloud {
// ==================== RENDER ====================

namespace {
void ensureVisible(BP &bp, int cursor, int visCount) {
  if (cursor < bp.scrollTop)
    bp.scrollTop = cursor;
  if (cursor >= bp.scrollTop + visCount)
    bp.scrollTop = cursor - visCount + 1;
  if (bp.scrollTop < 0)
    bp.scrollTop = 0;
}
} // namespace

void UIManager::renderBpScan() {
  BP &bp = BP::instance();
  using PB = UiTheme::PadBtn;
  drawRoundedRect(kCardX, kCardY, kCardW, 84, UiTheme::RADIUS_CARD,
                  UiTheme::CARD_SOLID, true);
  if (bp.pairBusy()) {
    drawText("Đang pair — chờ TV/Host xác nhận...", kCardX + kCardW / 2,
             kCardY + 16, UiTheme::ACCENT_GOLD, m_fontMedium, true);
  } else if (bp.scanning()) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Đang quét... còn %lds",
                  bp.scanMsLeft() / 1000);
    drawText(buf, kCardX + kCardW / 2, kCardY + 16, UiTheme::ACCENT_CYAN,
             m_fontMedium, true);
  } else {
    drawText("A: quét lại • A trên thiết bị: Ghép nối • B: Về danh sách",
             kCardX + kCardW / 2, kCardY + 20, UiTheme::TEXT_SUB, m_fontSmall,
             true);
  }
  auto devs = bp.scanDevs();
  int n = (int)devs.size();
  if (bp.cursor() >= n && n > 0)
    bp.setCursor(0);
  if (bp.scrollTop >= n && n > 0)
    bp.scrollTop = 0;
  if (n == 0 && !bp.scanning()) {
    drawText("Chưa thấy thiết bị nào — bấm A để quét", kCardX + kCardW / 2,
             kCardY + 220, UiTheme::TEXT_SUB, m_fontMedium, true);
  }
  const int vis = 6, rowH = 72;
  ensureVisible(bp, bp.cursor(), vis);
  for (int r = 0; r < vis && bp.scrollTop + r < n; r++) {
    int i = bp.scrollTop + r;
    int yy = kCardY + 100 + r * rowH;
    if (i == bp.cursor())
      drawFocusRow(kCardX + 8, yy, kCardW - 16, 64);
    const auto &d = devs[(size_t)i];
    bool foc = (i == bp.cursor());
    std::string dName = d.name.empty() ? d.mac : d.name;
    drawText(truncateToWidth(dName, m_fontSmall, 560), kCardX + kPad, yy + 2,
             foc ? UiTheme::TEXT_MAIN : UiTheme::TEXT_SUB, m_fontSmall);
    char sub[160];
    std::snprintf(sub, sizeof(sub), "%s   %s", d.mac.c_str(),
                  d.paired ? "Đã ghép nối" : "Mới");
    drawText(sub, kCardX + kPad, yy + 34, UiTheme::TEXT_DIM, m_fontSmall);
    if (foc) {
      int bw = badgeWidth(d.paired ? "Đã lưu" : "Ghép nối", 32);
      drawBadge(kCardX + kCardW - kPad - bw, yy + 16, bw, 32,
                d.paired ? "Đã lưu" : "Ghép nối", {37, 99, 235, 255},
                {255, 255, 255, 255});
    }
  }
  drawAppFooter({{PB::A, "Quét / Ghép nối"}, {PB::B, "Về danh sách"}});
}

void UIManager::renderBpDevices() {
  BP &bp = BP::instance();
  using PB = UiTheme::PadBtn;

  // 1. Card 1: Status Card có viền standard, chấm tròn xanh/đỏ, bỏ chữ trạng
  // thái
  int statusY = 74;
  int statusH = 76;
  drawRoundedRect(kCardX, statusY, kCardW, statusH, UiTheme::RADIUS_CARD,
                  UiTheme::CARD_SOLID, true);
  drawRoundedRect(kCardX, statusY, kCardW, statusH, UiTheme::RADIUS_CARD,
                  UiTheme::CARD_BORDER, false);

  // Chấm tròn: xanh nếu đã kết nối, đỏ nếu chưa kết nối
  int dotSize = 14;
  int dotX = kCardX + kPad;
  int dotY = statusY + (statusH - dotSize) / 2;
  SDL_Color dotColor = bp.connected() ? SDL_Color{34, 197, 94, 255}
                                      : SDL_Color{239, 68, 68, 255};
  drawRoundedRect(dotX, dotY, dotSize, dotSize, dotSize / 2, dotColor, true);

  // Dòng thông tin cấu hình (bỏ chữ trạng thái)
  std::string modeText = (bp.store().lastMode == 1)
                             ? ("Bàn phím + Chuột (Tốc độ " +
                                std::to_string(bp.mouseSpeed()) + ")")
                             : "Tay cầm";
  std::string infoLine =
      "Platform: " + bp.platformNameOf(bp.store().lastPlatform) +
      "  |  Chế độ: " + modeText;
  int textX = dotX + dotSize + 16;
  int textY = textYCentered(statusY, statusH, m_fontMedium);
  drawText(truncateToWidth(infoLine, m_fontMedium, kCardW - 80), textX, textY,
           UiTheme::TEXT_MAIN, m_fontMedium);

  // 2. Danh sách thiết bị: title to lên, màu xanh cyan như header
  int listTitleY = statusY + statusH + 20;
  drawText("DANH SÁCH THIẾT BỊ", kCardX + kPad, listTitleY,
           UiTheme::ACCENT_CYAN, m_fontMedium);
  int listY = listTitleY + 50;

  std::vector<int> rows;
  auto devs = bp.scanDevs();
  for (size_t i = 0; i < devs.size(); i++)
    if (devs[i].paired || devs[i].connected)
      rows.push_back((int)i);
  int n = (int)rows.size();

  if (n == 0) {
    drawRoundedRect(kCardX, listY, kCardW, 140, UiTheme::RADIUS_CARD,
                    UiTheme::CARD_SOLID, true);
    drawRoundedRect(kCardX, listY, kCardW, 140, UiTheme::RADIUS_CARD,
                    UiTheme::CARD_BORDER, false);
    drawText("Chưa có thiết bị nào được lưu", kCardX + kCardW / 2, listY + 40,
             UiTheme::TEXT_MAIN, m_fontMedium, true);
    drawText("Bấm MENU để quét tìm thiết bị xung quanh", kCardX + kCardW / 2,
             listY + 80, UiTheme::ACCENT_CYAN, m_fontSmall, true);
  } else {
    if (bp.cursor() >= n)
      bp.setCursor(0);
    const int vis = 5, cardH = 64, gap = 8;
    ensureVisible(bp, bp.cursor(), vis);

    for (int r = 0; r < vis && bp.scrollTop + r < n; r++) {
      int idx = bp.scrollTop + r;
      int yy = listY + r * (cardH + gap);
      bool foc = (idx == bp.cursor());

      // Card thiết bị: có border như Card 1
      drawRoundedRect(kCardX, yy, kCardW, cardH, UiTheme::RADIUS_CARD,
                      foc ? UiTheme::FOCUS_BG : UiTheme::CARD_SOLID, true);
      drawRoundedRect(kCardX, yy, kCardW, cardH, UiTheme::RADIUS_CARD,
                      foc ? UiTheme::CARD_BORDER : UiTheme::FOOTER_LINE, false);

      int ri = rows[(size_t)idx];
      const auto &d = devs[(size_t)ri];
      std::string dName =
          (d.name.empty() || BrickPad::isMacAddress(d.name)) ? d.mac : d.name;

      // Format 1 hàng duy nhất: tên thiết bị - platform - chế độ tay cầm / bàn
      // phím + chuột tốc độ XX
      std::string plat = bp.getPlatformNameFor(d.mac);
      int dMode = bp.store().deviceMode(d.mac);
      std::string modeDesc =
          (dMode == 1)
              ? ("Bàn phím + Chuột (tốc độ " + std::to_string(bp.mouseSpeed()) + ")")
              : "Tay cầm";
      std::string rowText = dName + " - " + plat + " - " + modeDesc;
      int rowTextY = textYCentered(yy, cardH, m_fontMedium);

      drawText(truncateToWidth(rowText, m_fontMedium, 720), kCardX + kPad,
               rowTextY, foc ? UiTheme::TEXT_MAIN : UiTheme::TEXT_SUB,
               m_fontMedium);

      if (bp.forgetArm == idx) {
        drawTextRight("X lần nữa để quên", kCardX + kCardW - kPad, yy + 22,
                      UiTheme::ACCENT_GOLD, m_fontSmall);
      } else if (d.connected) {
        int bw = badgeWidth("Đang kết nối", 32);
        drawBadge(kCardX + kCardW - kPad - bw, yy + 16, bw, 32, "Đang kết nối",
                  {20, 83, 45, 255}, {134, 239, 172, 255});
      } else {
        const char *act = "Kết nối";
        int bw = badgeWidth(act, 32);
        drawBadge(kCardX + kCardW - kPad - bw, yy + 16, bw, 32, act,
                  foc ? SDL_Color{37, 99, 235, 255}
                      : SDL_Color{51, 65, 85, 255},
                  foc ? SDL_Color{255, 255, 255, 255} : UiTheme::TEXT_SUB);
      }
    }
  }

  // Dòng chữ cảnh báo nhiễu sóng lăn liên tục như banner ngay trên footer
  TTF_Font *fItalic = m_fontSmallItalic ? m_fontSmallItalic : m_fontSmall;
  std::string warnText =
      "Wi-Fi và Bluetooth trên TrimUI Brick Pro dùng chung băng tần 2.4GHz nên "
      "có thể gây nhiễu, chập chờn hoặc rớt kết nối.";
  std::string fullMsg = warnText + "        •        ";
  int msgW = textWidth(fullMsg, fItalic);
  int warnY = UiTheme::FOOTER_Y - 28;
  int bannerX = kCardX;
  int bannerW = kCardW;

  if (msgW > 0) {
    SDL_Rect clip = {
        PlatformInfo::instance().scaleX(bannerX),
        PlatformInfo::instance().scaleY(warnY - 4),
        PlatformInfo::instance().scaleW(bannerW),
        PlatformInfo::instance().scaleH(30)
    };
    SDL_RenderSetClipRect(m_renderer, &clip);

    int scrollX = static_cast<int>((SDL_GetTicks() * 50) / 1000) % msgW;
    int drawX = bannerX - scrollX;
    while (drawX < bannerX + bannerW) {
      drawText(fullMsg, drawX, warnY, UiTheme::TEXT_SUB, fItalic);
      drawX += msgW;
    }

    SDL_RenderSetClipRect(m_renderer, nullptr);
  } else {
    drawText(truncateToWidth(warnText, fItalic, bannerW),
             UiTheme::APP_W / 2, warnY, UiTheme::TEXT_SUB, fItalic, true);
  }

  // Hint sắp xếp: A chọn, B thoát, X quên, Y đổi tên, SELECT: Cài đặt
  drawAppFooter({{PB::A, "Chọn"},
                 {PB::B, "Thoát"},
                 {PB::X, "Quên"},
                 {PB::Y, "Đổi tên"},
                 {PB::SELECT, "Cài đặt"}});
}

void UIManager::renderBpPlatformOverlay() {
  BP &bp = BP::instance();
  if (bp.chooser() != 3)
    return;
  beginModalDim();

  auto curId = (!m_bpConfigMac.empty()) ? bp.store().deviceIdentity(m_bpConfigMac)
                                        : bp.identity();
  bool isXbox = (curId == BrickPad::ControllerIdentity::XBOX);
  int numRows = isXbox ? 2 : 3;
  int w = 680, h = isXbox ? 260 : 310;
  int x = (1024 - w) / 2, y = (768 - h) / 2;
  drawModalDialog(x, y, w, h, UiTheme::RADIUS_MODAL, UiTheme::CARD_SOLID,
                  UiTheme::CARD_SOLID, 56);

  std::string title = "PLATFORM & CẤU HÌNH";
  if (!m_bpConfigMac.empty()) {
    std::string dName = m_bpConfigMac;
    for (const auto &d : bp.store().devices) {
      std::string normWant, normHave;
      BrickPad::BrickPadStore::normalizeMac(m_bpConfigMac, normWant);
      BrickPad::BrickPadStore::normalizeMac(d.address, normHave);
      if (normWant == normHave) {
        dName = (d.alias.empty() || BrickPad::isMacAddress(d.alias)) ? d.address
                                                                     : d.alias;
        break;
      }
    }
    title = "CẤU HÌNH: " + dName;
  }
  drawText(truncateToWidth(title, m_fontLarge, w - 56), x + 28, y + 12,
           UiTheme::TEXT_MAIN, m_fontLarge);
  const char *rows[3] = {"Platform", "Chế độ", "Tốc độ chuột (1-10)"};
  for (int i = 0; i < numRows; i++) {
    int ry = y + 70 + i * 56;
    if (i == bp.chooserCursor())
      drawFocusRow(x + 16, ry, w - 32, 48);
    std::string val;
    if (i == 0) {
      if (!m_bpConfigMac.empty()) {
        val = bp.platformNameOf(bp.store().deviceIdentity(m_bpConfigMac));
      } else {
        val = bp.platformName();
      }
    } else if (i == 1) {
      if (isXbox) {
        val = "Tay cầm (Cố định)";
      } else {
        int dm = (!m_bpConfigMac.empty()) ? bp.store().deviceMode(m_bpConfigMac)
                                          : (int)bp.mode();
        val = (dm == 1 ? "Bàn phím + Chuột" : "Tay cầm");
      }
    } else {
      val = "Mức " + std::to_string(bp.mouseSpeed()) + " / 10";
    }
    drawText(rows[i], x + 40, ry + 8,
             i == bp.chooserCursor() ? UiTheme::TEXT_MAIN : UiTheme::TEXT_SUB,
             m_fontSmall);
    std::string valTxt = (i == 1 && isXbox) ? val : ("< " + val + " >");
    drawTextRight(valTxt, x + w - 40, ry + 8,
                  i == bp.chooserCursor() ? UiTheme::ACCENT_CYAN
                                          : UiTheme::TEXT_DIM,
                  m_fontSmall);
  }
  int promptY = isXbox ? (y + 198) : (y + 252);
  drawText("Bấm [B] Đóng & lưu cấu hình cho thiết bị", x + w / 2, promptY,
           UiTheme::ACCENT_CYAN, m_fontSmall, true);
  using PB = UiTheme::PadBtn;
  drawAppFooter({{PB::DPAD, "Chọn / Đổi"},
                 {PB::A, "Thay đổi"},
                 {PB::B, "Đóng & Lưu"}});
}

void UIManager::setBpBacklightOff(bool off) {
#if defined(__linux__)
  static int s_savedBrightness = 110;
  static bool s_isOff = false;
  const char *path = "/sys/class/led/tg5040_exterme_brightness";

  if (off && !s_isOff) {
    FILE *rf = fopen(path, "r");
    if (rf) {
      int b = 0;
      if (fscanf(rf, "%d", &b) == 1 && b > 0) {
        s_savedBrightness = b;
      }
      fclose(rf);
    }
    FILE *wf = fopen(path, "w");
    if (wf) {
      fprintf(wf, "0\n");
      fclose(wf);
    }
    s_isOff = true;
  } else if (!off && s_isOff) {
    FILE *wf = fopen(path, "w");
    if (wf) {
      fprintf(wf, "%d\n", s_savedBrightness > 0 ? s_savedBrightness : 110);
      fclose(wf);
    }
    s_isOff = false;
  }
#else
  (void)off;
#endif
}

void UIManager::renderBpSession() {
  BP &bp = BP::instance();

  // 1. Header: BRICKPAD - <tên thiết bị>
  drawRect(0, 0, UiTheme::APP_W, UiTheme::HEADER_H, UiTheme::FOOTER_BG, true);
  drawRect(0, UiTheme::HEADER_H - 1, UiTheme::APP_W, 1, UiTheme::FOOTER_LINE,
           true);
  int logoH = 40, logoW = 40;
  int logoY = (UiTheme::HEADER_H - logoH) / 2;
  drawGridIcon("BRICKPAD.png", 24, logoY, logoW, logoH);
  int titleX = 24 + logoW + 10;
  int titleY = textYCentered(0, UiTheme::HEADER_H, m_fontLarge);
  std::string mainTitle = "BRICKPAD";
  drawText(mainTitle, titleX, titleY, UiTheme::ACCENT_CYAN, m_fontLarge);
  int tw = textWidth(mainTitle, m_fontLarge);
  std::string hostName = bp.peerName();
  if (hostName.empty() || BrickPad::isMacAddress(hostName)) {
    hostName = bp.peerMac();
  }
  std::string connStr = " - " + hostName;
  drawText(truncateToWidth(connStr, m_fontLarge, 500), titleX + tw + 8, titleY,
           UiTheme::TEXT_SUB, m_fontLarge);
  drawHeaderStatus();

  // 2. Thẻ thiết bị đang kết nối (Card trên)
  int card1Y = 84;
  int card1H = 280;
  drawRoundedRect(kCardX, card1Y, kCardW, card1H, UiTheme::RADIUS_CARD,
                  UiTheme::CARD_SOLID, true);
  drawRoundedRect(kCardX, card1Y, kCardW, card1H, UiTheme::RADIUS_CARD,
                  UiTheme::CARD_BORDER, false);

  // Header badge trong card: Trạng thái kết nối (không emoji)
  int bwConn = badgeWidth("ĐANG KẾT NỐI (HID ACTIVE)", 32);
  drawBadge(kCardX + kPad, card1Y + 18, bwConn, 32, "ĐANG KẾT NỐI (HID ACTIVE)",
            {20, 83, 45, 255}, {134, 239, 172, 255});

  int bwProto = badgeWidth("Bluetooth Classic (PSM 0x11/0x13)", 32);
  drawBadge(kCardX + kCardW - kPad - bwProto, card1Y + 18, bwProto, 32,
            "Bluetooth Classic (PSM 0x11/0x13)", {30, 41, 59, 255},
            {148, 163, 184, 255});

  // Tên Host (không emoji)
  drawText(truncateToWidth(hostName, m_fontLarge, 900), kCardX + kPad,
           card1Y + 62, UiTheme::TEXT_MAIN, m_fontLarge);

  std::string macSub = "Địa chỉ MAC máy chủ: " + bp.peerMac();
  drawText(macSub, kCardX + kPad, card1Y + 104, UiTheme::TEXT_DIM, m_fontSmall);

  // Kẻ phân cách
  drawRect(kCardX + kPad, card1Y + 136, kCardW - 2 * kPad, 1, {30, 41, 59, 255},
           true);

  // Cấu hình Platform & Chế độ
  drawText("Cấu hình tay cầm đang giả lập:", kCardX + kPad, card1Y + 152,
           UiTheme::TEXT_SUB, m_fontSmall);

  std::string platName = bp.platformNameOf(bp.store().lastPlatform);
  std::string modeName =
      (bp.store().lastMode == 1 ? ("Bàn phím + Chuột (Tốc độ " +
                                   std::to_string(bp.mouseSpeed()) + ")")
                                : "Tay cầm chơi game");

  // 2 pill xích xuống dưới theo standard để không sát dòng trên
  int platBw = badgeWidth(platName, 34);
  drawBadge(kCardX + kPad, card1Y + 192, platBw, 34, platName,
            {37, 99, 235, 255}, {255, 255, 255, 255});

  int modeBw = badgeWidth(modeName, 34);
  drawBadge(kCardX + kPad + platBw + 12, card1Y + 192, modeBw, 34, modeName,
            {15, 118, 110, 255}, {204, 251, 241, 255});

  std::string idSub = "Identity: " + bp.platformName() + "  •  Report: 60 FPS";
  drawText(idSub, kCardX + kPad, card1Y + 240, UiTheme::TEXT_DIM, m_fontSmall);

  // 3. Thẻ Hướng dẫn (Card dưới - bỏ đếm 30s, bỏ emoji)
  int card2Y = card1Y + card1H + 16;
  int card2H = 250;
  drawRoundedRect(kCardX, card2Y, kCardW, card2H, UiTheme::RADIUS_CARD,
                  UiTheme::CARD_SOLID, true);
  drawRoundedRect(kCardX, card2Y, kCardW, card2H, UiTheme::RADIUS_CARD,
                  UiTheme::CARD_BORDER, false);

  int noteY = card2Y + 24;
  int noteGap = 36;
  drawText("• Tất cả nút bấm, D-pad và cần analog đang điều khiển trực tiếp "
           "trên máy chủ.",
           kCardX + kPad, noteY, UiTheme::TEXT_MAIN, m_fontSmall);
  drawText("• Kết nối Bluetooth duy trì liên tục và tuyệt đối không bị ngắt "
           "khi tắt màn hình.",
           kCardX + kPad, noteY + noteGap, UiTheme::TEXT_SUB, m_fontSmall);
  drawText("• Giữ SELECT + START trong 1 giây để chuyển đổi nhanh giữa Tay cầm "
           "và Bàn phím.",
           kCardX + kPad, noteY + noteGap * 2, UiTheme::ACCENT_CYAN,
           m_fontSmall);
  drawText("• Bấm F1 để ngắt kết nối và thoát về Home. Bấm F2 để bật/tắt sáng "
           "màn hình.",
           kCardX + kPad, noteY + noteGap * 3, UiTheme::TEXT_DIM, m_fontSmall);

  // 4. Footer: 2 Pill màu trắng nằm ngay chính giữa, chữ center vertical chuẩn
  // xác
  drawRect(0, UiTheme::FOOTER_Y, UiTheme::APP_W, UiTheme::FOOTER_H,
           UiTheme::FOOTER_BG, true);
  drawRect(0, UiTheme::FOOTER_Y, UiTheme::APP_W, 1, UiTheme::FOOTER_LINE, true);

  uint32_t elapsed =
      (m_bpSessionStartMs > 0) ? (SDL_GetTicks() - m_bpSessionStartMs) : 0;
  int secLeft = 30 - static_cast<int>(elapsed / 1000);
  if (secLeft < 0)
    secLeft = 0;
  std::string countText = "Ẩn: " + std::to_string(secLeft) + "s";

  int pillW = 150, pillH = 34, pillGap = 16;
  int totalPillW = pillW * 2 + pillGap;
  int startPillX = (UiTheme::APP_W - totalPillW) / 2;
  int pillY = UiTheme::FOOTER_Y + (UiTheme::FOOTER_H - pillH) / 2;

  drawBadge(startPillX, pillY, pillW, pillH, "F1: Tắt", {255, 255, 255, 255},
            {15, 23, 42, 255});
  drawBadge(startPillX + pillW + pillGap, pillY, pillW, pillH, countText,
            {255, 255, 255, 255}, {15, 23, 42, 255});
}

void UIManager::renderBrickPadState() {
  BP &bp = BP::instance();

  if (bp.connected()) {
    if (m_bpSessionStartMs == 0) {
      m_bpSessionStartMs = SDL_GetTicks();
    }
    uint32_t elapsed = SDL_GetTicks() - m_bpSessionStartMs;
    if (elapsed >= 30000) {
      // Màn hình đen sau 30s để tiết kiệm pin (tuyệt đối không ngắt kết nối trừ
      // khi bấm F1)
      setBpBacklightOff(true);
      SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
      SDL_RenderClear(m_renderer);
      return;
    }
    setBpBacklightOff(false);
    drawAppBackground();
    renderBpSession();
    return;
  }

  m_bpSessionStartMs = 0;
  setBpBacklightOff(false);

  drawAppBackground();

  // 1. Header: BRICKPAD [ - <tên thiết bị đang kết nối>]
  drawRect(0, 0, UiTheme::APP_W, UiTheme::HEADER_H, UiTheme::FOOTER_BG, true);
  drawRect(0, UiTheme::HEADER_H - 1, UiTheme::APP_W, 1, UiTheme::FOOTER_LINE,
           true);
  int logoH = 40, logoW = 40;
  int logoY = (UiTheme::HEADER_H - logoH) / 2;
  drawGridIcon("BRICKPAD.png", 24, logoY, logoW, logoH);
  int titleX = 24 + logoW + 10;
  int titleY = textYCentered(0, UiTheme::HEADER_H, m_fontLarge);
  std::string mainTitle = "BRICKPAD";
  drawText(mainTitle, titleX, titleY, UiTheme::ACCENT_CYAN, m_fontLarge);
  if (bp.connected()) {
    int tw = textWidth(mainTitle, m_fontLarge);
    std::string peer = bp.peerName();
    if (peer.empty() || BrickPad::isMacAddress(peer))
      peer = bp.peerMac();
    std::string connStr = " - " + peer;
    drawText(truncateToWidth(connStr, m_fontLarge, 500), titleX + tw + 8,
             titleY, UiTheme::TEXT_SUB, m_fontLarge);
  }
  drawHeaderStatus();

  if (bp.screen() == BpScreen::SCAN)
    renderBpScan();
  else
    renderBpDevices();
  // Overlay chọn Platform & Chế độ (mở bằng SELECT).
  renderBpPlatformOverlay();

  if (bp.vkOpen()) {
    SearchInputModal::render(m_bpModalCfg);
  }
}

} // namespace RomCloud
