#include "../calendar/CalManager.h"
#include "../market/MarketManager.h"
#include "../calendar/Lunar.h"
#include "../input/InputManager.h"
#include "../logging/Logger.h"
#include "../platform/PlatformInfo.h"
#include "../weather/WeatherManager.h"
#include "UIManager.h"

#include <cstdio>
#include <ctime>

namespace RomCloud {

// Ngày lễ VN quan trọng (dương cố định + âm; quét tiến để tìm gần nhất)
static std::string nextVnHoliday(int *outDaysLeft, char *outDateBuf,
                                 size_t bufSz) {
  struct SolarH {
    int d, m;
    const char *name;
  };
  static const SolarH solar[] = {
      {1, 1, "Tết Dương Lịch"},        {14, 2, "Valentine"},
      {8, 3, "Ngày Quốc Tế Phụ Nữ"},   {30, 4, "Ngày Giải Phóng"},
      {1, 5, "Ngày Quốc Tế Lao Động"}, {1, 6, "Ngày Quốc Tế Thiếu Nhi"},
      {2, 9, "Ngày Quốc Khánh"},       {20, 10, "Ngày Phụ Nữ VN"},
      {20, 11, "Ngày Nhà Giáo VN"},    {25, 12, "Giáng Sinh"},
  };
  struct LunarH {
    int d, m;
    const char *name;
  };
  static const LunarH lunar[] = {
      {1, 1, "Tết Nguyên Đán"},     {2, 1, "Mùng 2 Tết"},
      {3, 1, "Mùng 3 Tết"},         {15, 1, "Rằm Tháng Giêng"},
      {10, 3, "Giỗ Tổ Hùng Vương"}, {15, 4, "Lễ Phật Đản"},
      {5, 5, "Tết Đoan Ngọ"},       {15, 7, "Lễ Vu Lan"},
      {15, 8, "Tết Trung Thu"},     {23, 12, "Ông Công Ông Táo"},
  };
  std::time_t now = std::time(nullptr);
  for (int off = 0; off <= 400; off++) {
    std::time_t t = now + (int64_t)off * 86400;
    struct tm *lt = std::localtime(&t);
    int sm = lt->tm_mon + 1, sd = lt->tm_mday;
    for (const auto &h : solar) {
      if (h.m == sm && h.d == sd) {
        snprintf(outDateBuf, bufSz, "%02d/%02d", sd, sm);
        *outDaysLeft = off;
        return h.name;
      }
    }
    auto ld = Lunar::solarToLunar(sd, sm, lt->tm_year + 1900);
    if (!ld.leap) {
      for (const auto &h : lunar) {
        if (h.m == ld.month && h.d == ld.day) {
          snprintf(outDateBuf, bufSz, "%02d/%02d", sd, sm);
          *outDaysLeft = off;
          return h.name;
        }
      }
    }
  }
  return "";
}

void UIManager::wxBlockRect(int idx, int &x, int &y, int &w, int &h) {
  // Rect các khối tab thời tiết (khớp renderWeather, cập nhật khi đổi layout)
  switch (idx) {
  case 0:
    x = 24;
    y = 138;
    w = 560;
    h = 150;
    break; // now
  case 1:
    x = 24;
    y = 300;
    w = 560;
    h = 110;
    break; // forecast
  case 2:
    x = 24;
    y = 420;
    w = 560;
    h = 260;
    break; // calendar
  case 3:
    x = 600;
    y = 124;
    w = 400;
    h = 281;
    break; // sắp tới
  default:
    x = 600;
    y = 417;
    w = 400;
    h = 282;
    break; // lịch đồng bộ
  }
}

void UIManager::wxFocusMove(int dx, int dy) {
  int cx, cy, cw, ch;
  wxBlockRect(m_wxFocus, cx, cy, cw, ch);
  int fx = cx + cw / 2, fy = cy + ch / 2;
  int best = -1, bestScore = 1 << 30;
  for (int i = 0; i < 5; i++) {
    if (i == m_wxFocus)
      continue;
    int bx, by, bw, bh;
    wxBlockRect(i, bx, by, bw, bh);
    int nx = bx + bw / 2 - fx, ny = by + bh / 2 - fy;
    // Nửa mặt phẳng theo hướng (dung sai 1/3 kích thước)
    if (dx > 0 && nx < cw / 3)
      continue;
    if (dx < 0 && nx > -cw / 3)
      continue;
    if (dy > 0 && ny < ch / 3)
      continue;
    if (dy < 0 && ny > -ch / 3)
      continue;
    // Cùng hàng/cột phải giao nhau vùng (card ngắn vẫn qua được)
    if (dx != 0 && dy == 0) {
      int top1 = cy - ch / 2, bot1 = cy + ch / 2;
      int top2 = by - bh / 2, bot2 = by + bh / 2;
      if (bot1 < top2 || bot2 < top1)
        continue;
    }
    if (dy != 0 && dx == 0) {
      int l1 = cx - cw / 2, r1 = cx + cw / 2;
      int l2 = bx - bw / 2, r2 = bx + bw / 2;
      if (r1 < l2 || r2 < l1)
        continue;
    }
    int primary = dx != 0 ? abs(nx) : abs(ny);
    int second = dx != 0 ? abs(ny) : abs(nx);
    int score = primary + second * 2;
    if (score < bestScore) {
      bestScore = score;
      best = i;
    }
  }
  if (best >= 0)
    m_wxFocus = best;
}

void UIManager::openWeather() {
  WeatherManager::instance().loadWards();
  WeatherManager::instance().loadPlace();
  WeatherManager::instance().loadCache();
  CalManager::instance().ensureTables();
  // Vào trang hiện cache/DB ngay; sync (thời tiết + lịch) DELAY 1.5s sau,
  // chạy nền — không chặn frame đầu, hết cảm giác lag khi vào app.
  m_wxEnterMs = SDL_GetTicks();
  m_wxAutoSync = false;
  m_wxFetching = false;
  m_wxFocus = 2; // tab Thời tiết: focus khối Lịch
  m_wxMode = WeatherManager::instance().place().valid ? 0 : 1;
  m_wxProvSel = 0;
  m_wxWardSel = 0;
  m_wxCandSel = 0;
  m_wxCands.clear();
  // Lịch về tháng hiện tại
  std::time_t t = std::time(nullptr);
  struct tm *lt = std::localtime(&t);
  m_wxCalY = lt->tm_year + 1900;
  m_wxCalM = lt->tm_mon + 1;
  m_wxFetching = false;
  if (m_wxMode == 0 && WeatherManager::instance().place().valid) {
    m_wxFetching = true;
    m_wxTaskKind = 0;
    m_wxTask.run([](TaskProgress &) { WeatherManager::instance().fetch(); });
  }
  setState(UIState::WEATHER);
}

// Vẽ icon thời tiết: PNG user bổ sung sau (assets/weather_icons/<n>.png),
// chưa có thì vẽ hình khối.
void UIManager::drawWxIcon(const std::string &kind, int cx, int cy, int s) {
  std::string path =
      AppConfig::instance().getAssetsDir() + "/weather_icons/" + kind + ".png";
  SDL_Texture *tex = m_ui.getOrLoadImage("weather/" + kind, path);
  if (tex) {
    SDL_Rect dst = {cx - s / 2, cy - s / 2, s, s};
    SDL_RenderCopy(m_renderer, tex, nullptr, &dst);
    return;
  }
  int r = s / 2;
  if (kind == "sun") {
    drawDot(cx, cy, r * 2 / 3, {250, 204, 21, 255});
  } else if (kind == "partly") {
    drawDot(cx - r / 3, cy - r / 4, r / 2, {250, 204, 21, 255});
    drawRoundedRect(cx - r + 4, cy - r / 4, r * 2 - 8, r * 3 / 4, 10,
                    {148, 163, 184, 255}, true);
  } else if (kind == "rain" || kind == "heavy_rain" || kind == "storm") {
    drawRoundedRect(cx - r + 4, cy - r, r * 2 - 8, r, 10, {148, 163, 184, 255},
                    true);
    for (int k = -1; k <= 1; k++)
      drawRect(cx + k * r / 2 - 2, cy + r / 4, 4, r * 3 / 4,
               {59, 130, 246, 255}, true);
  } else if (kind == "snow") {
    drawRoundedRect(cx - r + 4, cy - r, r * 2 - 8, r, 10, {203, 213, 225, 255},
                    true);
    for (int k = -1; k <= 1; k++)
      drawDot(cx + k * r / 2, cy + r / 2, 3, {255, 255, 255, 255});
  } else if (kind == "fog") {
    for (int k = 0; k < 3; k++)
      drawRect(cx - r + 4, cy - r / 2 + k * r / 2, r * 2 - 8, 5,
               {148, 163, 184, 255}, true);
  } else { // cloud
    drawRoundedRect(cx - r + 4, cy - r / 2, r * 2 - 8, r, 12,
                    {148, 163, 184, 255}, true);
  }
}

static int wxDaysInMonth(int y, int m) {
  struct tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = m; // ngày 0 tháng sau = ngày cuối tháng này
  t.tm_mday = 0;
  std::mktime(&t);
  return t.tm_mday;
}

static int wxFirstCol(int y, int m) {
  // Cột T2=0..CN=6
  struct tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = m - 1;
  t.tm_mday = 1;
  std::mktime(&t);
  return (t.tm_wday + 6) % 7;
}

void UIManager::renderWeather() {
  // Sync delay: vào trang hiện ngay, 1.5s sau mới sync nền.
  // Bỏ qua nếu task bận (tránh BackgroundTask::run join block UI).
  if (!m_wxAutoSync && !m_wxTask.isRunning() &&
      SDL_GetTicks() - m_wxEnterMs >= 1500) {
    m_wxAutoSync = true;
    m_wxFetching = true;
    m_wxTaskKind = 0;
    m_wxTask.run([](TaskProgress &) {
      WeatherManager::instance().fetch();
      CalManager::instance().refreshAll();
    });
  }
  if (m_wxTab == 2) {
    // Tab camera: trang riêng (nền + header tên camera + ảnh + list)
    drawAppBackground();
    renderTraffic();
    return;
  }
  drawAppBackground();
  if (m_wxTab == 3) {
    // Header riêng tab giá: subtitle "Giá theo ..." nằm NGANG title,
    // giữ nguyên font/màu sub như drawAppHeader (small/TEXT_DIM).
    WxPlace plm = WeatherManager::instance().place();
    std::string sub = "Giá theo " + MarketManager::areaFor(
        plm.valid ? plm.province : std::string("Hà Nội"));
    drawRect(0, 0, UiTheme::APP_W, UiTheme::HEADER_H, UiTheme::FOOTER_BG, true);
    drawRect(0, UiTheme::HEADER_H - 1, UiTheme::APP_W, 1, UiTheme::FOOTER_LINE,
             true);
    const std::string title = "THỊ TRƯỜNG";
    drawText(title, 24, textYCentered(0, UiTheme::HEADER_H, m_fontLarge),
             UiTheme::ACCENT_CYAN, m_fontLarge);
    int tx = 24 + textWidth(title, m_fontLarge) + 16;
    drawText(truncateToWidth(sub, m_fontSmall, UiTheme::APP_W - tx - 300),
             tx, textYCentered(0, UiTheme::HEADER_H, m_fontSmall),
             UiTheme::TEXT_DIM, m_fontSmall, false);
    drawHeaderStatus();
  } else if (m_wxTab == 1) {
    drawAppHeader("ĐỒNG HỒ");
  } else {
    // wxTab == 0: title "THỜI TIẾT & LỊCH" + tên vị trí NGANG hàng
    // (cùng pattern với tab Thị trường: title cyan trái, sub dim phải title).
    WxPlace pl0 = WeatherManager::instance().place();
    std::string loc = pl0.valid ? (pl0.ward + ", " + pl0.province)
                                : "Chưa chọn vị trí (bấm Y)";
    drawRect(0, 0, UiTheme::APP_W, UiTheme::HEADER_H, UiTheme::FOOTER_BG, true);
    drawRect(0, UiTheme::HEADER_H - 1, UiTheme::APP_W, 1, UiTheme::FOOTER_LINE,
             true);
    const std::string title = "THỜI TIẾT & LỊCH";
    drawText(title, 24, textYCentered(0, UiTheme::HEADER_H, m_fontLarge),
             UiTheme::ACCENT_CYAN, m_fontLarge);
    int tx = 24 + textWidth(title, m_fontLarge) + 16;
    drawText(truncateToWidth(loc, m_fontSmall, UiTheme::APP_W - tx - 300),
             tx, textYCentered(0, UiTheme::HEADER_H, m_fontSmall),
             pl0.valid ? SDL_Color{0, 180, 216, 255}
                       : SDL_Color{245, 158, 11, 255},
             m_fontSmall, false);
    drawHeaderStatus();
  }
  // Tab pills: Thời tiết | Đồng hồ | Camera | Thị trường
  {
    int w0 = pillWidth("THỜI TIẾT", m_fontSmall) + 32;
    int w1 = pillWidth("ĐỒNG HỒ", m_fontSmall) + 32;
    int w2 = pillWidth("CAMERA", m_fontSmall) + 32;
    int w3 = pillWidth("THỊ TRƯỜNG", m_fontSmall) + 32;
    int x3 = 1024 - 24 - w3;
    int x2 = x3 - 8 - w2;
    int x1 = x2 - 8 - w1;
    int x0 = x1 - 8 - w0;
    drawPill(x0, 72, w0, 32, "THỜI TIẾT", m_wxTab == 0, m_fontSmall);
    drawPill(x1, 72, w1, 32, "ĐỒNG HỒ", m_wxTab == 1, m_fontSmall);
    drawPill(x2, 72, w2, 32, "CAMERA", m_wxTab == 2, m_fontSmall);
    drawPill(x3, 72, w3, 32, "THỊ TRƯỜNG", m_wxTab == 3, m_fontSmall);
  }
  if (m_wxTab == 1) {
    renderClock(124, 715 - 124 - 16);
    return;
  }
  if (m_wxTab == 3) {
    renderMarket(124, 715 - 124 - 16);
    return;
  }
  // (Tên vị trí đã được vẽ ngang title trong header phía trên.)
  if (m_wxTask.isRunning() == false && m_wxFetching) {
    m_wxFetching = false; // fetch/geocode xong (data đã vào manager)
  }

  const int contentTop = 124;
  const int contentH = 715 - contentTop - 16;

  if (m_wxMode == 1 || m_wxMode == 2 || m_wxMode == 3) {
    renderWxPicker(contentTop, contentH);
    return;
  }

  // ---- Cột trái: thời tiết gọn + 4 ngày + lịch ----
  // Focus khối Lịch = sáng cả card trái (rule card focus).
  const int lx = 24, lw = 560;
  drawRoundedRect(lx, contentTop, lw, contentH, UiTheme::RADIUS_CARD,
                  m_wxFocus == 2 ? SDL_Color{30, 38, 54, 255}
                                 : SDL_Color{22, 28, 38, 255},
                  true);
  WxPlace pl = WeatherManager::instance().place();
  WxNow now = WeatherManager::instance().now();
  int wy = contentTop + 14;
  if (!pl.valid) {
    drawText("Chưa chọn vị trí", lx + 28, wy, {255, 255, 255, 255}, m_fontLarge,
             false);
    drawText("Bấm [Y] để chọn phường/xã", lx + 28, wy + 44, {0, 180, 216, 255},
             m_fontSmall, false);
    wy += 100;
  } else {
    // Trạng thái tải/offline: góc phải cùng hàng icon (không chiếm dòng)
    if (m_wxFetching) {
      drawTextRight("Đang tải...", lx + lw - 20, contentTop + 20,
                    {245, 158, 11, 255}, m_fontSmall);
    } else if (!now.ok) {
      drawTextRight("Mất mạng (dữ liệu cũ)", lx + lw - 20, contentTop + 20,
                    {245, 158, 11, 255}, m_fontSmall);
    }
    if (now.ok) {
      // Hàng 1 hai cột, neo top card
      const int dayH = 104;
      // Hàng 1 hai cột: trái icon trên + trạng thái dưới,
      // phải 3 hàng thông tin chia đều theo block trái
      int iconSz = 88;
      int leftW = 210;
      int medH = textHeight(m_fontMedium);
      int blockH = iconSz + 8 + medH;
      int rowY0 = contentTop + 12; // icon cách top card 12px
      int lcx = lx + 20 + leftW / 2;
      drawWxIcon(WeatherManager::wmoIcon(now.code), lcx, rowY0 + iconSz / 2,
                 iconSz);
      std::string stLabel = WeatherManager::wmoLabel(now.code);
      drawText(stLabel, lcx, rowY0 + iconSz + 8, {0, 180, 216, 255},
               m_fontMedium, true);
      int tx = lx + 20 + leftW + 16;
      int beaufort = 0;
      double w = now.wind;
      const double lim[] = {1, 6, 12, 20, 29, 39, 47, 55, 62, 75, 89, 103, 118};
      for (int b = 0; b < 13; b++)
        if (w >= lim[b])
          beaufort = b + 1;
      char tbuf[32], hbuf[48], wbuf[64];
      snprintf(tbuf, sizeof(tbuf), "%.0f°C", now.temp);
      snprintf(hbuf, sizeof(hbuf), "Độ ẩm: %.0f%%", now.hum);
      snprintf(wbuf, sizeof(wbuf), "Gió cấp %d (%.1f km/h)", beaufort,
               now.wind);
      const char *rows3[3] = {tbuf, hbuf, wbuf};
      int rh = blockH / 3;
      int smallH = textHeight(m_fontSmall);
      for (int r = 0; r < 3; r++)
        drawText(rows3[r], tx, rowY0 + r * rh + (rh - smallH) / 2,
                 {170, 180, 195, 255}, m_fontSmall, false);
      // Forecast bắt đầu sau khối 1 đúng 10px
      wy = rowY0 + blockH + 10;
      // Hàng 2: 4 ngày (ngày / icon 36 / nhiệt, spacing đúng chuẩn)
      const auto &days = WeatherManager::instance().days();
      int colW = (lw - 56) / 4;
      for (size_t i = 0; i < days.size() && i < 4; ++i) {
        int cx = lx + 28 + (int)i * colW + colW / 2;
        std::string dstr =
            days[i].date.substr(8, 2) + "/" + days[i].date.substr(5, 2);
        drawText(dstr, cx, wy, {148, 163, 184, 255}, m_fontSmall, true);
        drawWxIcon(WeatherManager::wmoIcon(days[i].code), cx, wy + 48, 36);
        char t2[32];
        snprintf(t2, sizeof(t2), "%.0f/%.0f°", days[i].tmax, days[i].tmin);
        drawText(t2, cx, wy + 74, {255, 255, 255, 255}, m_fontSmall, true);
      }
      wy += dayH;
    } else {
      drawText("Chưa có dữ liệu. Bấm [A] để tải lại.", lx + 28, wy + 20,
               {170, 180, 195, 255}, m_fontSmall, false);
      wy += 60;
    }
  }

  // Lịch tháng align bottom cột trái (title + 12 gap + dow + grid).
  // Focus đã thể hiện bằng nền card trái, title cyan khi focus.
  {
    int rows = (wxFirstCol(m_wxCalY, m_wxCalM) +
                wxDaysInMonth(m_wxCalY, m_wxCalM) + 6) /
               7;
    const int calH = 30 + 12 + 28 + rows * 40 + 8;
    int calTop = contentTop + contentH - calH;
    char mbuf[32];
    snprintf(mbuf, sizeof(mbuf), "Tháng %d/%d", m_wxCalM, m_wxCalY);
    drawText(mbuf, lx + 28, calTop,
             m_wxFocus == 2 ? SDL_Color{0, 180, 216, 255}
                            : SDL_Color{100, 110, 125, 255},
             m_fontMedium, false);
    const char *dows[7] = {"T2", "T3", "T4", "T5", "T6", "T7", "CN"};
    int cellW = (lw - 56) / 7;
    int gx = lx + 28;
    int gy = calTop + 42;
    for (int c = 0; c < 7; c++)
      drawText(dows[c], gx + c * cellW + cellW / 2, gy,
               c == 6 ? SDL_Color{248, 113, 113, 255}
                      : SDL_Color{148, 163, 184, 255},
               m_fontSmall, true);
    gy += 28;
    int cellH = 40;
    int dim = wxDaysInMonth(m_wxCalY, m_wxCalM);
    int col = wxFirstCol(m_wxCalY, m_wxCalM);
    std::vector<char> hasEv(dim + 1, 0);
    {
      struct tm t0 = {};
      t0.tm_year = m_wxCalY - 1900;
      t0.tm_mon = m_wxCalM - 1;
      t0.tm_mday = 1;
      int64_t mStart = (int64_t)mktime(&t0);
      int64_t mEnd = mStart + 32LL * 86400;
      for (const auto &e :
           CalManager::instance().eventsForRange(mStart, mEnd)) {
        std::time_t tt2 = (time_t)e.startTs;
        struct tm *lt2 = std::localtime(&tt2);
        if (lt2 && lt2->tm_year + 1900 == m_wxCalY &&
            lt2->tm_mon + 1 == m_wxCalM && lt2->tm_mday >= 1 &&
            lt2->tm_mday <= dim)
          hasEv[lt2->tm_mday] = 1;
      }
    }
    std::time_t tt = std::time(nullptr);
    struct tm *nowlt = std::localtime(&tt);
    int todayY = nowlt->tm_year + 1900, todayM = nowlt->tm_mon + 1,
        todayD = nowlt->tm_mday;
    for (int d = 1; d <= dim; d++) {
      int cx = gx + col * cellW + cellW / 2;
      bool isToday = (d == todayD && m_wxCalM == todayM && m_wxCalY == todayY);
      char db[8];
      snprintf(db, sizeof(db), "%d", d);
      if (isToday) {
        // Hôm nay: dương bold + xanh dương, âm cam (FOCUS_ALT) cùng cỡ 75%
        drawText(db, cx, gy, {0, 180, 216, 255}, m_fontSmall, true);
        drawText(db, cx + 1, gy, {0, 180, 216, 255}, m_fontSmall, true);
      } else {
        drawText(db, cx, gy,
                 col == 6 ? SDL_Color{248, 113, 113, 255}
                          : SDL_Color{203, 213, 225, 255},
                 m_fontSmall, true);
      }
      // Ngày âm 75%, xéo phải so với ngày dương. Texture cache màu
      // TRẮNG để tint theo ngày bằng SDL_SetTextureColorMod (ngày
      // thường xám, hôm nay cam) — cùng cỡ, không to lên.
      // Cache texture theo tháng, rải 3 ngày/frame để không đơ frame đầu
      // (load + sync nặng đã chạy nền từ openWeather).
      {
        if (m_lunarCacheY != m_wxCalY || m_lunarCacheM != m_wxCalM) {
          for (auto t : m_lunarTex)
            SDL_DestroyTexture(t);
          m_lunarTex.assign(dim + 1, nullptr);
          m_lunarCacheY = m_wxCalY;
          m_lunarCacheM = m_wxCalM;
        }
        // Ưu tiên hôm nay trước để không nhấp nháy ngày âm hôm nay.
        if (isToday && d >= 1 && d <= dim &&
            (d >= (int)m_lunarTex.size() || !m_lunarTex[d])) {
          auto ld0 = Lunar::solarToLunar(d, m_wxCalM, m_wxCalY);
          char lb0[12];
          snprintf(lb0, sizeof(lb0), "%d%s", ld0.day, ld0.leap ? "N" : "");
          SDL_Surface *s0 = TTF_RenderUTF8_Blended(m_fontSmall, lb0,
                                                   SDL_Color{255, 255, 255, 255});
          if (s0) {
            SDL_Texture *t0 = SDL_CreateTextureFromSurface(m_renderer, s0);
            SDL_FreeSurface(s0);
            if (t0) {
              if (d >= (int)m_lunarTex.size())
                m_lunarTex.resize(d + 1, nullptr);
              if (m_lunarTex[d])
                SDL_DestroyTexture(m_lunarTex[d]);
              m_lunarTex[d] = t0;
            }
          }
        }
        int built = 0;
        for (int dd2 = 1; dd2 <= dim && built < 3; dd2++) {
          if (dd2 < (int)m_lunarTex.size() && m_lunarTex[dd2])
            continue;
          auto ld = Lunar::solarToLunar(dd2, m_wxCalM, m_wxCalY);
          char lb[12];
          snprintf(lb, sizeof(lb), "%d%s", ld.day, ld.leap ? "N" : "");
          SDL_Surface *s = TTF_RenderUTF8_Blended(
              m_fontSmall, lb, SDL_Color{255, 255, 255, 255});
          if (!s)
            continue;
          SDL_Texture *t = SDL_CreateTextureFromSurface(m_renderer, s);
          SDL_FreeSurface(s);
          if (t) {
            m_lunarTex[dd2] = t;
            built++;
          }
        }
        if (d <= dim && (int)m_lunarTex.size() > d && m_lunarTex[d]) {
          int tw = 0, th = 0;
          SDL_QueryTexture(m_lunarTex[d], nullptr, nullptr, &tw, &th);
          // Hôm nay: cam (FOCUS_ALT); ngày khác: xám như cũ. Cùng cỡ 75%.
          SDL_Color tint = isToday ? UiTheme::FOCUS_ALT
                                   : SDL_Color{100, 110, 125, 255};
          SDL_SetTextureColorMod(m_lunarTex[d], tint.r, tint.g, tint.b);
          SDL_Rect dst = {cx + 14 - (int)(tw * 0.75) / 2, gy + 20,
                          (int)(tw * 0.75), (int)(th * 0.75)};
          SDL_RenderCopy(m_renderer, m_lunarTex[d], nullptr, &dst);
          SDL_SetTextureColorMod(m_lunarTex[d], 255, 255, 255);
        }
      }
      if (hasEv[d]) {
        // Vạch | ngay trước số ngày (không đẩy số lệch), cùng màu
        // cyan với dot bên ghi chú
        char dbw[8];
        snprintf(dbw, sizeof(dbw), "%d", d);
        int numW = textWidth(dbw, m_fontSmall);
        drawText("|", cx - numW / 2 - 12, gy, {0, 180, 216, 255}, m_fontSmall,
                 false);
      }
      if (++col >= 7) {
        col = 0;
        gy += cellH;
      }
    }
  }

  // ---- Cột phải: SẮP TỚI trên, LỊCH ĐỒNG BỘ dưới (2 khối bằng nhau) ----
  const int rx = 600, rw = 1024 - 24 - 600; // 400
  const int topH = (contentH - 12) / 2;
  const int botTop = contentTop + topH + 12;
  const int botH = contentTop + contentH - botTop;
  // Khối trên: SẮP TỚI — lễ gần nhất + việc sắp tới, chữ lớn.
  drawRoundedRect(rx, contentTop, rw, topH, UiTheme::RADIUS_CARD,
                  {22, 28, 38, 255}, true);
  {
    int uy = contentTop + 12;
    drawText("CÁC SỰ KIỆN SẮP TỚI", rx + 20, uy + 2, {0, 180, 216, 255},
             m_fontSmall, false);
    uy += 30;
    {
      std::time_t tnow2 = std::time(nullptr);
      struct tm *ltn = std::localtime(&tnow2);
      int dayKey = (ltn->tm_year + 1900) * 10000 + (ltn->tm_mon + 1) * 100 +
                   ltn->tm_mday;
      if (dayKey != m_holDayKey) {
        int left = 0;
        char hdate[16] = "";
        std::string hol = nextVnHoliday(&left, hdate, sizeof(hdate));
        m_holText = hol; // chỉ tên lễ, không "Sắp tới:/Hôm nay:"
        m_holDate = hdate; // dd/MM vẽ trước tên, vd "20/10 ... "
        m_holDayKey = dayKey;
      }
      if (!m_holText.empty()) {
        std::string holPrefix = m_holDate.empty() ? std::string() : m_holDate + " ";
        int holX = rx + 42;
        if (!holPrefix.empty()) {
          drawText(holPrefix, holX, uy, {0, 180, 216, 255}, m_fontSmall,
                   false);
          holX += textWidth(holPrefix, m_fontSmall);
        }
        drawDot(rx + 28, uy + textHeight(m_fontSmall) / 2, 4,
                {250, 204, 21, 255});
        drawText(truncateToWidth(m_holText, m_fontSmall, rx + rw - 20 - holX),
                 holX, uy, {250, 204, 21, 255}, m_fontSmall, false);
      }
      uy += 26;
    }
    uy += 10;
    auto soon = CalManager::instance().upcoming(20);
    const int bottom = contentTop + topH - 8;
    int medH = textHeight(m_fontMedium);
    int shown = 0;
    for (size_t i = 0; i < soon.size() && shown < 6; ++i) {
      const int rh = 44;
      if (uy + rh > bottom)
        break;
      std::time_t tt = (time_t)soon[i].startTs;
      struct tm *l = std::localtime(&tt);
      char dd[24];
      if (soon[i].allDay)
        snprintf(dd, sizeof(dd), "%02d/%02d", l->tm_mday, l->tm_mon + 1);
      else
        snprintf(dd, sizeof(dd), "%02d/%02d %02d:%02d", l->tm_mday,
                 l->tm_mon + 1, l->tm_hour, l->tm_min);
      std::string prefix = std::string(dd) + " ";
      int ty = uy + (rh - medH) / 2;
      drawText(prefix, rx + 20, ty, {0, 180, 216, 255}, m_fontMedium, false);
      int tw = textWidth(prefix, m_fontMedium);
      int titleW = rw - 40 - tw - (soon[i].remind ? 30 : 0);
      if (titleW < 40)
        titleW = 40;
      drawText(truncateToWidth(soon[i].title, m_fontMedium, titleW),
               rx + 20 + tw, ty, {255, 255, 255, 255}, m_fontMedium, false);
      if (soon[i].remind)
        drawText("♪", rx + rw - 40, ty, {250, 204, 21, 255}, m_fontMedium,
                 false);
      uy += rh;
      shown++;
    }
  }
  // Khối dưới: LỊCH ĐỒNG BỘ — sự kiện tháng đang xem. Focus: nền sáng.
  {
    drawRoundedRect(rx, botTop, rw, botH, UiTheme::RADIUS_CARD,
                    m_wxFocus == 4 ? SDL_Color{30, 38, 54, 255}
                                   : SDL_Color{22, 28, 38, 255},
                    true);
    int uy = botTop + 12;
    // Sự kiện ICS trong tháng đang xem
    drawText("LỊCH ĐỒNG BỘ", rx + 20, uy,
             m_wxFocus == 4 ? SDL_Color{0, 180, 216, 255}
                            : SDL_Color{100, 110, 125, 255},
             m_fontSmall, false);
    // Chú thích: ♪ = có báo thức (font không có symbol loa/chuông).
    drawTextRight("♪ báo", rx + rw - 20, uy,
                  {100, 110, 125, 255}, m_fontSmall);
    uy += 30; // rule 10.4: tiêu đề khối → hàng đầu 28-34px
    struct tm t0 = {};
    t0.tm_year = m_wxCalY - 1900;
    t0.tm_mon = m_wxCalM - 1;
    t0.tm_mday = 1;
    int64_t mStart = (int64_t)mktime(&t0);
    auto up =
        CalManager::instance().eventsForRange(mStart, mStart + 32LL * 86400);
    if (up.empty()) {
      drawText("Chưa có việc nào.", rx + 20, uy, {100, 110, 125, 255},
               m_fontSmall, false);
    }
    // Cache dòng đang hiện cho input (chọn + X bật/tắt báo).
    // Event dài wrap tối đa 2 dòng, dòng 2 align-left với dòng 1
    // (cùng x, full width). Chiều cao hàng co theo nội dung:
    // 1 dòng → 36px, 2 dòng → 64px (không bao giờ quá 2 dòng).
    struct NoteRow {
      size_t evIdx;
      int y;
      int h;
      std::string line1;
      std::string line2;
    };
    const int rowH1 = 36;
    const int rowH2 = 64;
    const int lineStep = 28;
    std::vector<NoteRow> rows;
    {
      int y = uy;
      const int bottom = contentTop + contentH - 8;
      const int textX = rx + 42;
      const int textW = (rx + rw - 20) - textX;
      for (size_t i = 0; i < up.size() && rows.size() < 7; ++i) {
        int h = rowH1; // mặc định 1 dòng, tính lại sau khi wrap
        std::time_t tt3 = (time_t)up[i].startTs;
        struct tm *lt3 = std::localtime(&tt3);
        char dd[24];
        if (up[i].allDay)
          snprintf(dd, sizeof(dd), "%02d/%02d", lt3->tm_mday, lt3->tm_mon + 1);
        else
          snprintf(dd, sizeof(dd), "%02d/%02d %02d:%02d", lt3->tm_mday,
                   lt3->tm_mon + 1, lt3->tm_hour, lt3->tm_min);
        std::string prefix = std::string(dd) + " ";
        int wTitle1 = textW - textWidth(prefix, m_fontSmall);
        if (wTitle1 < 40) wTitle1 = 40;
        auto wl = wrapAboutText(up[i].title, m_fontSmall, wTitle1);
        std::string l1 = wl.empty() ? std::string() : wl[0];
        std::string l2;
        if (wl.size() > 1) {
          // Dòng 2 full width, align-left với dòng 1 (không indent).
          std::string rest = wl[1];
          for (size_t k = 2; k < wl.size(); ++k) rest += " " + wl[k];
          auto wl2 = wrapAboutText(rest, m_fontSmall, textW);
          l2 = wl2.empty() ? std::string() : wl2[0];
          for (size_t k = 1; k < wl2.size(); ++k) l2 += " " + wl2[k];
          l2 = truncateToWidth(l2, m_fontSmall, textW);
        }
        h = l2.empty() ? rowH1 : rowH2;
        if (y + h > bottom) break;
        rows.push_back({i, y, h, prefix + l1, l2});
        y += h;
      }
      if (rows.empty() && !up.empty()) {
        // Bảo đảm ít nhất 1 dòng như cũ (kể cả khi chật chỗ).
        std::time_t tt3 = (time_t)up[0].startTs;
        struct tm *lt3 = std::localtime(&tt3);
        char dd[24];
        if (up[0].allDay)
          snprintf(dd, sizeof(dd), "%02d/%02d", lt3->tm_mday, lt3->tm_mon + 1);
        else
          snprintf(dd, sizeof(dd), "%02d/%02d %02d:%02d", lt3->tm_mday,
                   lt3->tm_mon + 1, lt3->tm_hour, lt3->tm_min);
        rows.push_back({0, uy, rowH1,
                        std::string(dd) + " " +
                            truncateToWidth(up[0].title, m_fontSmall, rw - 118),
                        ""});
      }
    }
    m_noteVis.clear();
    for (auto &r : rows) m_noteVis.push_back(up[r.evIdx]);
    if (!m_noteVis.empty() && m_noteSel >= (int)m_noteVis.size())
      m_noteSel = (int)m_noteVis.size() - 1;
    if (m_noteSel < 0)
      m_noteSel = 0;
    for (size_t k = 0; k < rows.size(); ++k) {
      const auto &r = rows[k];
      int ry = r.y;
      // Highlight/dot/♪ căn giữa theo chiều cao thật của hàng;
      // chữ dòng 1 giữ optical cũ (ry-2), dòng 2 cùng x (align-left).
      int ty = ry - 2;
      if ((int)k == m_noteSel && m_wxFocus == 4 && !m_noteVis.empty())
        drawHighlight(rx + 8, ry + 5, rw - 16, r.h - 10);
      if (up[r.evIdx].remind)
        drawText("♪", rx + 20, ry + r.h / 2 - 19, {250, 204, 21, 255},
                 m_fontSmall, false);
      else
        drawDot(rx + 28, ry + r.h / 2, 4, {0, 180, 216, 255});
      drawText(r.line1, rx + 42, ty, {203, 213, 225, 255}, m_fontSmall, false);
      if (!r.line2.empty())
        drawText(r.line2, rx + 42, ty + lineStep, {203, 213, 225, 255},
                 m_fontSmall, false);
    }
  }

  drawAppFooter(
      {{UiTheme::PadBtn::A, m_wxFocus == 4 ? "Chuông báo nhắc" : "Tải lại"},
       {UiTheme::PadBtn::Y, "Địa phương"},
       {UiTheme::PadBtn::B, "Lùi"},
       {UiTheme::PadBtn::L1R1, "Tab"}});
}

void UIManager::renderWxPicker(int contentTop, int contentH) {
  // Hộp chọn tỉnh / phường / ứng viên geocode
  const int boxW = 640;
  const int boxX = (1024 - boxW) / 2;
  const int boxY = contentTop + 40;
  const int boxH = contentH - 80;
  drawRoundedRect(boxX, boxY, boxW, boxH, UiTheme::RADIUS_MODAL,
                  {24, 28, 38, 255}, true);
  std::string title = m_wxMode == 1   ? "CHỌN TỈNH/THÀNH"
                      : m_wxMode == 2 ? "CHỌN PHƯỜNG/XÃ"
                                      : "CHỌN VỊ TRÍ ĐÚNG";
  drawText(title, 512, boxY + 16, {0, 180, 216, 255}, m_fontLarge, true);
  int listTop = boxY + 64;
  int rowH = 46;
  int visible = (boxH - 64 - 52) / rowH;
  if (visible < 1)
    visible = 1;

  int total = 0, sel = 0;
  std::vector<WxGeoCand> candsCopy;
  if (m_wxMode == 1) {
    total = (int)WeatherManager::instance().provinceCount();
    sel = m_wxProvSel;
  } else if (m_wxMode == 2) {
    total = (int)WeatherManager::instance().wardsOf(m_wxProvSel).size();
    sel = m_wxWardSel;
  } else {
    std::lock_guard<std::mutex> lk(m_wxCandsMutex);
    candsCopy = m_wxCands;
    total = (int)candsCopy.size();
    sel = m_wxCandSel;
  }
  if (sel >= total)
    sel = total > 0 ? total - 1 : 0;
  int start = sel - sel % visible;
  for (int i = 0; i < visible && start + i < total; ++i) {
    int idx = start + i;
    int y = listTop + i * rowH;
    std::string label;
    if (m_wxMode == 1)
      label = WeatherManager::instance().provinceName(idx);
    else if (m_wxMode == 2)
      label = WeatherManager::instance().wardsOf(m_wxProvSel)[idx];
    else
      label = candsCopy[idx].name + " (" + candsCopy[idx].admin + ")";
    if (idx == sel)
      drawHighlight(boxX + 12, y + 3, boxW - 24, rowH - 6);
    drawText(truncateToWidth(label, m_fontSmall, boxW - 48), boxX + 24,
             y + (rowH - 6 - textHeight(m_fontSmall)) / 2,
             idx == sel ? SDL_Color{255, 255, 255, 255}
                        : SDL_Color{203, 213, 225, 255},
             m_fontSmall, false);
  }
  if (total == 0)
    drawText(m_wxFetching ? "Đang tìm..." : "Không có dữ liệu", 512,
             listTop + 40, {170, 180, 195, 255}, m_fontSmall, true);
  drawInlineHintsCentered("A Chọn  •  B Lùi", 512, boxY + boxH - 36,
                          {148, 163, 184, 255}, m_fontSmall, 24);
}

bool UIManager::handleWeatherInput() {
  InputManager &input = InputManager::instance();
  // B ở picker (mode 1/2/3): lùi cấp. Mode 0: rơi xuống logic tab bên dưới.
  if (input.isButtonJustPressed(Button::B) && m_wxMode != 0) {
    // Lùi cấp picker: candidates -> ward -> province -> view/menu
    if (m_wxMode == 3)
      m_wxMode = 2;
    else if (m_wxMode == 2)
      m_wxMode = 1;
    else if (WeatherManager::instance().place().valid)
      m_wxMode = 0;
    else
      setState(UIState::MENU);
    return true;
  }
  if (m_wxMode == 1) {
    int total = (int)WeatherManager::instance().provinceCount();
    if (input.isButtonJustPressed(Button::UP) && total > 0) {
      m_wxProvSel = (m_wxProvSel + total - 1) % total;
    } else if (input.isButtonJustPressed(Button::DOWN) && total > 0) {
      m_wxProvSel = (m_wxProvSel + 1) % total;
    } else if (input.isButtonJustPressed(Button::A) && total > 0) {
      m_wxWardSel = 0;
      m_wxMode = 2;
    }
    return true;
  }
  if (m_wxMode == 2) {
    int total = (int)WeatherManager::instance().wardsOf(m_wxProvSel).size();
    if (input.isButtonJustPressed(Button::UP) && total > 0) {
      m_wxWardSel = (m_wxWardSel + total - 1) % total;
    } else if (input.isButtonJustPressed(Button::DOWN) && total > 0) {
      m_wxWardSel = (m_wxWardSel + 1) % total;
    } else if (input.isButtonJustPressed(Button::A) && total > 0 &&
               !m_wxFetching) {
      // Geocode nền rồi qua chọn ứng viên
      std::string prov = WeatherManager::instance().provinceName(m_wxProvSel);
      std::string ward =
          WeatherManager::instance().wardsOf(m_wxProvSel)[m_wxWardSel];
      m_wxFetching = true;
      m_wxTaskKind = 1;
      m_wxTask.run([this, prov, ward](TaskProgress &) {
        auto cands = WeatherManager::instance().geocode(ward, prov);
        std::lock_guard<std::mutex> lk(m_wxCandsMutex);
        m_wxCands = std::move(cands);
      });
      m_wxMode = 3;
      m_wxCandSel = 0;
    }
    return true;
  }
  if (m_wxMode == 3) {
    if (m_wxFetching)
      return true; // đang geocode, chờ
    std::vector<WxGeoCand> cands;
    {
      std::lock_guard<std::mutex> lk(m_wxCandsMutex);
      cands = m_wxCands;
    }
    if (cands.empty()) {
      if (input.isButtonJustPressed(Button::A) ||
          input.isButtonJustPressed(Button::UP) ||
          input.isButtonJustPressed(Button::DOWN)) {
        showToast("Không tìm thấy vị trí. Kiểm tra mạng.", {239, 68, 68, 255},
                  2500);
        m_wxMode = 2;
      }
      return true;
    }
    int total = (int)cands.size();
    if (input.isButtonJustPressed(Button::UP)) {
      m_wxCandSel = (m_wxCandSel + total - 1) % total;
    } else if (input.isButtonJustPressed(Button::DOWN)) {
      m_wxCandSel = (m_wxCandSel + 1) % total;
    } else if (input.isButtonJustPressed(Button::A)) {
      const auto &c = cands[m_wxCandSel];
      WeatherManager::instance().selectPlace(
          WeatherManager::instance().provinceName(m_wxProvSel),
          WeatherManager::instance().wardsOf(m_wxProvSel)[m_wxWardSel], c.lat,
          c.lon);
      m_wxCands.clear();
      m_wxMode = 0;
      m_wxFetching = true;
      m_wxTaskKind = 0;
      m_wxTask.run([](TaskProgress &) { WeatherManager::instance().fetch(); });
    }
    return true;
  }
  // Mode 0: xem (trang chủ — B thoát app, START về lưới menu).
  // Tab camera (1) route trước để B không bị chặn nhầm.
  bool l1Tab = input.isButtonJustPressed(Button::L1);
  bool r1Tab = input.isButtonJustPressed(Button::R1);
  if (l1Tab || r1Tab) {
    // L1: lùi tab (sang trái), R1: tới tab (sang phải).
    m_wxTab = (m_wxTab + (l1Tab ? 3 : 1)) % 4;
    if (m_wxTab == 0)
      m_wxFocus = 2; // về tab Thời tiết: focus khối Lịch
    if (m_wxTab == 2) {
      m_camView = true;
      m_camVisDirty = true;
      restartCamView(); // vào tab là tải ảnh ngay
    }
    return true;
  }
  if (m_wxTab == 2)
    return handleTrafficInput();
  if (m_wxTab == 1)
    return handleClockInput();
  if (m_wxTab == 3)
    return handleMarketInput();
  if (input.isButtonJustPressed(Button::B)) {
    goBack(); // về trang trước (thường là launcher)
    return true;
  }
  if (input.isButtonJustPressed(Button::START)) {
    setState(UIState::MENU);
    return true;
  }
  // Rule navigation tab Thời tiết: 2 khối Lịch (2) / Lịch đồng bộ (4).
  // ◀▶ chuyển khối; ▲▼: ở Lịch = đổi tháng, ở Ghi chú = chọn việc.
  if (input.isButtonJustPressed(Button::LEFT) ||
      input.isButtonJustPressed(Button::RIGHT)) {
    m_wxFocus = (m_wxFocus == 2) ? 4 : 2;
    return true;
  }
  if (input.isButtonJustPressed(Button::UP)) {
    if (m_wxFocus == 4) {
      int nNote = (int)m_noteVis.size();
      if (nNote > 0)
        m_noteSel = (m_noteSel + nNote - 1) % nNote;
    } else if (--m_wxCalM < 1) {
      m_wxCalM = 12;
      m_wxCalY--;
    }
    return true;
  }
  if (input.isButtonJustPressed(Button::DOWN)) {
    if (m_wxFocus == 4) {
      int nNote = (int)m_noteVis.size();
      if (nNote > 0)
        m_noteSel = (m_noteSel + 1) % nNote;
    } else if (++m_wxCalM > 12) {
      m_wxCalM = 1;
      m_wxCalY++;
    }
    return true;
  }
  if (input.isButtonJustPressed(Button::A)) {
    if (m_wxFocus == 4) {
      // Toggle báo/không báo việc đang chọn.
      int nNote = (int)m_noteVis.size();
      if (nNote > 0 && m_noteSel >= 0 && m_noteSel < nNote) {
        bool on = !m_noteVis[m_noteSel].remind;
        if (CalManager::instance().setRemind(m_noteVis[m_noteSel].id, on)) {
          m_noteVis[m_noteSel].remind = on;
          showToast(on ? "Đã bật báo việc này" : "Đã tắt báo việc này",
                    on ? SDL_Color{250, 204, 21, 255}
                       : SDL_Color{148, 163, 184, 255},
                    1500);
        }
      }
    } else if (!m_wxFetching && !m_wxTask.isRunning()) {
      // A ở khối Lịch = sync cưỡng bức (thời tiết + lịch phone)
      m_wxFetching = true;
      m_wxTaskKind = 0;
      m_wxTask.run([](TaskProgress &) {
        WeatherManager::instance().fetch();
        CalManager::instance().refreshAll();
      });
    }
    return true;
  }
  if (input.isButtonJustPressed(Button::Y)) {
    m_wxMode = 1;
    return true;
  }
  return true;
}

} // namespace RomCloud
