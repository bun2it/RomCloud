#include "../calendar/CalManager.h"
#include "../clock/ClockStore.h"
#include "../common/TimeZone.h"
#include "../database/DatabaseManager.h"
#include "../input/InputManager.h"
#include "../weather/WeatherManager.h"
#include "UIManager.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

namespace RomCloud {

namespace {
// Giờ các thành phố tính từ đồng hồ máy Brick (localtime + đổi TZ),
// không NTP.
static bool cityLocal(const char *tz, struct tm &out, long &gmtoff) {
  std::time_t now = std::time(nullptr);
  // Không setenv ở đây (từng race với CalManager) — dùng TimeZone chung.
  long off = TimeZone::offsetFor(tz ? tz : "", now);
  gmtoff = off;
  std::time_t shifted = now + off;
  gmtime_r(&shifted, &out);
  return true;
}

// Nhãn UTC hiện tại của tz (tự theo DST lúc này): UTC+7, UTC+5:30...
static std::string utcLabel(const char *tz) {
  std::time_t now = std::time(nullptr);
  long off = TimeZone::offsetFor(tz ? tz : "", now);
  long oh = off / 3600, om = labs(off % 3600) / 60;
  char b[24];
  if (om == 0)
    snprintf(b, sizeof(b), "UTC%+ld", oh);
  else
    snprintf(b, sizeof(b), "UTC%+ld:%02ld", oh, om);
  return b;
}

// Danh sách thành phố đổi được, phủ UTC-10..+12.
struct CityDef {
  const char *name;
  const char *tz;
};
static const CityDef kAllCities[] = {
    {"Hà Nội", "Asia/Ho_Chi_Minh"},
    {"Bangkok", "Asia/Bangkok"},
    {"Singapore", "Asia/Singapore"},
    {"Bắc Kinh", "Asia/Shanghai"},
    {"Hồng Kông", "Asia/Hong_Kong"},
    {"Tokyo", "Asia/Tokyo"},
    {"Seoul", "Asia/Seoul"},
    {"Sydney", "Australia/Sydney"},
    {"Auckland", "Pacific/Auckland"},
    {"New Delhi", "Asia/Kolkata"},
    {"Karachi", "Asia/Karachi"},
    {"Dhaka", "Asia/Dhaka"},
    {"Dubai", "Asia/Dubai"},
    {"Moscow", "Europe/Moscow"},
    {"Cairo", "Africa/Cairo"},
    {"Paris", "Europe/Paris"},
    {"Berlin", "Europe/Berlin"},
    {"London", "Europe/London"},
    {"Azores", "Atlantic/Azores"},
    {"São Paulo", "America/Sao_Paulo"},
    {"Buenos Aires", "America/Argentina/Buenos_Aires"},
    {"New York", "America/New_York"},
    {"Mexico City", "America/Mexico_City"},
    {"Chicago", "America/Chicago"},
    {"Denver", "America/Denver"},
    {"Los Angeles", "America/Los_Angeles"},
    {"Anchorage", "America/Anchorage"},
    {"Honolulu", "Pacific/Honolulu"},
};
static const int kAllCityCount =
    (int)(sizeof(kAllCities) / sizeof(kAllCities[0]));
} // namespace

void UIManager::clkBlockRect(int idx, int &x, int &y, int &w, int &h) {
  // Rect KHỚP render thật (card báo thức co giãn theo số dòng)
  size_t n = ClockStore::instance().alarms().size();
  int listH = 40 + (int)n * 34;
  if (listH < 110)
    listH = 110;
  if (listH > 250)
    listH = 250;
  int py = 124 + listH + 12;
  switch (idx) {
  case 0:
    x = 24;
    y = 124;
    w = 480;
    h = 575;
    break; // giờ thế giới
  case 1:
    x = 520;
    y = 124;
    w = 480;
    h = listH;
    break; // báo thức
  default:
    x = 520;
    y = py;
    w = 480;
    h = 124 + 575 - py;
    break; // pomodoro
  }
}

void UIManager::clkFocusMove(int dx, int dy) {
  int cx, cy, cw, ch;
  clkBlockRect(m_clkFocus, cx, cy, cw, ch);
  int fx = cx + cw / 2, fy = cy + ch / 2;
  int best = -1, bestScore = 1 << 30;
  for (int i = 0; i < 3; i++) {
    if (i == m_clkFocus)
      continue;
    int bx, by, bw, bh;
    clkBlockRect(i, bx, by, bw, bh);
    int nx = bx + bw / 2 - fx, ny = by + bh / 2 - fy;
    if (dx > 0 && nx < cw / 3)
      continue;
    if (dx < 0 && nx > -cw / 3)
      continue;
    if (dy > 0 && ny < ch / 3)
      continue;
    if (dy < 0 && ny > -ch / 3)
      continue;
    // Cùng hàng/cột phải giao nhau vùng (thay vì chặn tâm xa):
    // card ngắn vẫn qua được card dài bên cạnh.
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
    m_clkFocus = best;
}

void UIManager::drawFlipCardBg(int x, int y, int w, int h) {
  // Nền thẻ số kiểu Fliqlo/gluqlo (vạch chia vẽ đè lên sau khi vẽ số).
  drawRoundedRect(x, y, w, h, 30, {15, 15, 15, 255}, true);
}

void UIManager::cycleClkFsStyle(int dir) {
  m_clkFsStyle = (m_clkFsStyle + dir + 3) % 3;
  DatabaseManager::instance().setSetting("clock_fs_style",
                                         std::to_string(m_clkFsStyle));
  for (int i = 0; i < 3; i++) {
    m_fsD[i].cur.clear();
    m_fsD[i].prev.clear();
    m_fsD[i].ani = false;
    m_fsD[i].prog = 0.0f;
  }
  const char* styleNames[] = {"Kiểu: Thẻ lật", "Kiểu: Phẳng", "Kiểu: QlockTwo"};
  m_clkFsMsg = styleNames[m_clkFsStyle];
  m_clkFsMsgUntil = SDL_GetTicks() + 1500;
}

void UIManager::renderClockFullscreen() {
  // Nền đen, chỉ đồng hồ (kiểu Fliqlo/gluqlo hoặc phẳng hoặc qlocktwo).
  drawRect(0, 0, 1024, 768, {0, 0, 0, 255}, true);
  std::time_t tnow = std::time(nullptr);
  struct tm *lt = std::localtime(&tnow);
  if (m_clkFsStyle == 1) {
    // Phẳng: số lớn giữa màn hình (giữ nguyên bản cũ).
    TTF_Font *f = m_fontClockFs
                      ? m_fontClockFs
                      : (m_fontClock ? m_fontClock : m_fontTitle);
    char hbuf[16];
    snprintf(hbuf, sizeof(hbuf), "%02d:%02d:%02d", lt->tm_hour, lt->tm_min,
             lt->tm_sec);
    drawText(truncateToWidth(hbuf, f, 1024 - 48), 512,
             (768 - textHeight(f)) / 2, {255, 255, 255, 255}, f, true);
  } else if (m_clkFsStyle == 2) {
    // Style 3: QlockTwo tiếng Việt - full màn hình 4:3
    renderQlockTwoStyle(lt);
  } else {
    // Thẻ lật HH MM SS (300px, gap 28) + animation lật 320ms khi đổi số.
    char ts[3][8];
    snprintf(ts[0], sizeof(ts[0]), "%02d", lt->tm_hour);
    snprintf(ts[1], sizeof(ts[1]), "%02d", lt->tm_min);
    snprintf(ts[2], sizeof(ts[2]), "%02d", lt->tm_sec);
    const int cardW = 300, cardH = 300, gap = 28;
    const int x0 = (1024 - (cardW * 3 + gap * 2)) / 2;
    const int y0 = (768 - cardH) / 2;
    // Dấu ":" không có trong font Fliqlo -> dùng Rajdhani 140
    // (mực ~28px vừa khít khe 28px; size 260 tràn lên thẻ).
    TTF_Font *fColon = m_fontClock;
    if (!fColon)
      fColon = m_fontTitle;
    uint32_t nowMs = SDL_GetTicks();
    float dtSec = (m_fsLastMs == 0 || nowMs < m_fsLastMs)
                      ? 0.016f
                      : (nowMs - m_fsLastMs) / 1000.0f;
    if (dtSec > 0.05f)
      dtSec = 0.05f; // kẹp hitch: giật frame không nuốt mất animation
    m_fsLastMs = nowMs;
    // gluqlo DURATION=260ms; hướng dẫn 300-500ms -> chọn 0.32s để thẻ giây
    // (đổi mỗi 1s) lật xong trước tick tiếp theo.
    constexpr float kFlipSecs = 0.32f; // thời gian lật 1 thẻ
    for (int i = 0; i < 3; i++) {
      std::string cur(ts[i]);
      FsDigit &dg = m_fsD[i];
      if (!dg.ani && dg.cur != cur) {
        if (!dg.cur.empty()) {
          dg.prev = dg.cur;
          dg.ani = true;
          dg.prog = 0.0f;
        } else {
          dg.cur = cur;
        }
      }
      float t = -1.0f; // <0 = tĩnh (drawFlipDigits vẽ số mới nguyên thẻ)
      std::string prev;
      if (dg.ani) {
        dg.prog += dtSec / kFlipSecs;
        if (dg.prog >= 1.0f) {
          dg.ani = false;
          dg.cur = cur;
          dg.prog = 0.0f;
        } else {
          t = dg.prog;
          prev = dg.prev;
        }
      }
      int cx = x0 + i * (cardW + gap);
      // Font flip clock chính gốc (Fliqlo), fallback font hệ nếu thiếu.
      // Cặp số tabular nên vừa khít, không cần bù số "1" hẹp.
      TTF_Font *f = m_fontFlip;
      if (!f || textWidth(cur, f) > cardW - 48)
        f = m_fontClockMd;
      if (!f || textWidth(cur, f) > cardW - 48)
        f = m_fontClock;
      if (!f || textWidth(cur, f) > cardW - 48)
        f = m_fontTitle;
      drawFlipCardBg(cx, y0, cardW, cardH);
      if (f)
        drawFlipDigits(cur, prev, t, cx, y0, cardW, cardH, f,
                       {183, 183, 183, 255});
    }
    // 2 dấu ":" giữa 3 thẻ (định dạng HH:MM:SS 24h).
    if (fColon) {
      for (int g = 0; g < 2; g++) {
        int gxc = x0 + cardW * (g + 1) + gap * g + gap / 2;
        drawText(":", gxc, y0 + (cardH - textHeight(fColon)) / 2,
                 {183, 183, 183, 255}, fColon, true);
      }
    }
  }
  // Gợi ý thoáng qua (toast không chạy ở fullscreen).
  if (!m_clkFsMsg.empty() && SDL_GetTicks() < m_clkFsMsgUntil)
    drawText(m_clkFsMsg, 512, 712, {150, 150, 160, 255}, m_fontSmall, true);
}

// ============================================================
// Style 3: QlockTwo tiếng Việt - Full 4:3
// ============================================================
void UIManager::renderQlockTwoStyle(struct tm* lt) {
    // Ma trận ký tự 15 hàng x 12 cột: mỗi ô đúng 1 chữ cái, chữ căn giữa
    // ô nên thẳng hàng dọc. Từ không bao giờ cắt xuống hàng; ô dư chèn
    // chữ mồi (B/H/BA/HAI/BHAI) không bao giờ sáng.
    static const char* GRID[15][12] = {
        {"K","H","Ô","N","G","M","Ư","Ờ","I","M","Ộ","T"},
        {"H","A","I","B","A","B","Ố","N","N","Ă","M","H"},
        {"S","Á","U","B","Ả","Y","T","Á","M","H","A","I"},
        {"C","H","Í","N","M","Ư","Ơ","I","M","Ố","T","B"},
        {"H","A","I","B","A","B","Ố","N","N","Ă","M","H"},
        {"S","Á","U","B","Ả","Y","T","Á","M","H","A","I"},
        {"C","H","Í","N","M","Ư","Ơ","I","G","I","Ờ","B"},
        {"K","É","M","R","Ư","Ỡ","I","K","H","Ô","N","G"},
        {"M","Ư","Ờ","I","M","Ộ","T","H","A","I","B","A"},
        {"B","Ố","N","N","Ă","M","S","Á","U","H","A","I"},
        {"B","Ả","Y","T","Á","M","C","H","Í","N","B","A"},
        {"M","Ư","Ơ","I","M","Ộ","T","H","A","I","B","A"},
        {"B","Ố","N","N","Ă","M","S","Á","U","H","A","I"},
        {"B","Ả","Y","T","Á","M","C","H","Í","N","B","A"},
        {"M","Ư","Ơ","I","P","H","Ú","T","B","H","A","I"},
    };
    constexpr int ROWS = 15, COLS = 12;

    // Font hệ hỗ trợ Unicode tiếng Việt; mỗi chữ căn giữa ô 78px.
    TTF_Font* f = m_fontLarge ? m_fontLarge : m_fontMedium;
    if (!f) f = m_fontSmall;
    if (!f) f = m_fontTitle;

    // Từ thuộc giờ/phút đang đọc sáng trắng, còn lại xám.
    const SDL_Color COLOR_INACTIVE = {70, 70, 78, 255};
    const SDL_Color COLOR_ACTIVE = {255, 255, 255, 255};

    // Lấy giờ và phút (đồng hồ máy Brick, 24h)
    int hour = lt->tm_hour;
    int minute = lt->tm_min;

    // Quy ước đọc: "X GIỜ [Y PHÚT]" | 30p = "X GIỜ RƯỠI" |
    // trên 40p = "(X+1) GIỜ KÉM (60-Y) PHÚT". 0 giờ = KHÔNG.
    bool lit[ROWS][COLS] = {{false}};
    auto light = [&](int r, int c) {
        if (r >= 0 && r < ROWS && c >= 0 && c < COLS) lit[r][c] = true;
    };
    auto lightWord = [&](int r, int c0, int len) {
        for (int i = 0; i < len; i++) light(r, c0 + i);
    };
    // Đơn vị giờ 1-9 (hàng 0-3), đơn vị phút 1-9 (hàng 8-10),
    // đơn vị phút hàng chục 2x-5x (hàng 11-13, sau MƯƠI).
    // Mỗi phần tử = (hàng, cột đầu, số ký tự).
    static const int HU[10][3] = {{-1,0,0},{0,9,3},{1,0,3},{1,3,2},
                                  {1,5,3},{1,8,3},{2,0,3},{2,3,3},
                                  {2,6,3},{3,0,4}};
    static const int MU[10][3] = {{-1,0,0},{8,4,3},{8,7,3},{8,10,2},
                                  {9,0,3},{9,3,3},{9,6,3},{10,0,3},
                                  {10,3,3},{10,6,4}};
    static const int MU2[10][3] = {{-1,0,0},{11,4,3},{11,7,3},{11,10,2},
                                   {12,0,3},{12,3,3},{12,6,3},{13,0,3},
                                   {13,3,3},{13,6,4}};
    auto lightHour = [&](int h) {
        if (h < 0 || h > 23) return;
        if (h == 0) { lightWord(0, 0, 5); return; } // KHÔNG
        if (h <= 9) { lightWord(HU[h][0], HU[h][1], HU[h][2]); return; }
        if (h <= 19) {
            lightWord(0, 5, 4); // MƯỜI
            if (h > 10) lightWord(HU[h - 10][0], HU[h - 10][1], HU[h - 10][2]);
            return;
        }
        // HAI MƯƠI ... đọc xuôi hàng 1 -> hàng 4.
        lightWord(1, 0, 3); // HAI (chục)
        lightWord(3, 4, 4); // MƯƠI
        int u = h - 20;
        if (u == 1) lightWord(3, 8, 3);      // MỐT
        else if (u == 2) lightWord(4, 0, 3); // HAI
        else if (u == 3) lightWord(4, 3, 2); // BA
    };
    // Chục phút 2-5 ở hàng 8-9, MƯƠI ở hàng 11, đơn vị ở hàng 11-13
    // sau MƯƠI — đọc xuôi "HAI MƯƠI NĂM".
    auto lightMinute = [&](int m) {
        if (m <= 0 || m > 59) return;
        if (m <= 9) { lightWord(MU[m][0], MU[m][1], MU[m][2]); return; }
        if (m <= 19) {
            lightWord(8, 0, 4); // MƯỜI
            if (m > 10) lightWord(MU[m - 10][0], MU[m - 10][1], MU[m - 10][2]);
            return;
        }
        int t = m / 10, u = m % 10;
        static const int TENS[6][3] = {{-1,0,0},{-1,0,0},{8,7,3},{8,10,2},
                                       {9,0,3},{9,3,3}};
        if (t >= 2 && t <= 5) lightWord(TENS[t][0], TENS[t][1], TENS[t][2]);
        lightWord(11, 0, 4); // MƯƠI
        if (u >= 1) lightWord(MU2[u][0], MU2[u][1], MU2[u][2]);
    };

    lightWord(6, 8, 3); // GIỜ luôn sáng
    if (minute == 0) {
        lightHour(hour);
    } else if (minute == 30) {
        lightHour(hour);
        lightWord(7, 3, 4); // RƯỠI
    } else if (minute > 40) {
        lightHour((hour + 1) % 24); // đọc giờ kế tiếp
        lightWord(7, 0, 3); // KÉM
        lightMinute(60 - minute);
        lightWord(14, 4, 4); // PHÚT
    } else {
        lightHour(hour);
        lightMinute(minute);
        lightWord(14, 4, 4); // PHÚT
    }

    // Ma trận full-bleed: padding 12px quanh màn 1024x768.
    // Mỗi chữ căn giữa ô riêng nên thẳng hàng dọc; từ sáng vẽ đè 3 pass
    // lệch 1px để giả bold (không có font bold tiếng Việt riêng).
    const int PAD = 12;
    const int GW = 1024 - 2 * PAD, GH = 768 - 2 * PAD;
    int th = f ? textHeight(f) : 40;
    for (int r = 0; r < ROWS; r++) {
        int ry = PAD + (r * GH) / ROWS;
        int rh = ((r + 1) * GH) / ROWS - (r * GH) / ROWS;
        int ty = ry + (rh - th) / 2; // căn giữa dọc trong hàng
        if (ty < ry) ty = ry;
        for (int c = 0; c < COLS; c++) {
            int cx0 = PAD + (c * GW) / COLS;
            int cx1 = PAD + ((c + 1) * GW) / COLS;
            int cx = (cx0 + cx1) / 2;
            SDL_Color color = lit[r][c] ? COLOR_ACTIVE : COLOR_INACTIVE;
            drawText(GRID[r][c], cx, ty, color, f, true);
            if (lit[r][c]) { // 2 pass phụ lệch 1px = nét đậm
                drawText(GRID[r][c], cx + 1, ty, color, f, true);
                drawText(GRID[r][c], cx, ty + 1, color, f, true);
            }
        }
    }
}

void UIManager::renderClock(
    int contentTop,
    int contentH) { // ---- Cột trái 1 cột 2 hàng: giờ to + world ngang ----
  const int lx = 24, lw = 480;
  // Focus = nền sáng hơn một chút (không viền, theo rule)
  SDL_Color cardBg = (m_clkFocus == 0) ? SDL_Color{30, 38, 54, 255}
                                       : SDL_Color{22, 28, 38, 255};
  // Card 1 chia 50-50: hàng 1 giờ địa phương, hàng 2 giờ thế giới.
  const int gap12 = 12;
  const int tH = (contentH - gap12) / 2;
  // Hàng 1: chỉ giờ, font khổ lớn full khối (lề 24px theo rule).
  TTF_Font *fHuge =
      m_fontClock ? m_fontClock : (m_fontHuge ? m_fontHuge : m_fontTitle);
  drawRoundedRect(lx, contentTop, lw, tH, UiTheme::RADIUS_CARD, cardBg, true);
  std::time_t tnow = std::time(nullptr);
  struct tm *lt = std::localtime(&tnow);
  char hbuf[16];
  snprintf(hbuf, sizeof(hbuf), "%02d:%02d:%02d", lt->tm_hour, lt->tm_min,
           lt->tm_sec);
  // Riêng khối này padding open: số tràn full khối, không clamp lề.
  drawText(hbuf, lx + lw / 2, contentTop + (tH - textHeight(fHuge)) / 2,
           {255, 255, 255, 255}, fHuge, true);

  // Hàng 2: 6 nước, lưới 3 cột x 2 hàng; mỗi ô 3 dòng: giờ, UTC, địa điểm.
  const int wY = contentTop + tH + gap12;
  const int wH = contentTop + contentH - wY;
  drawRoundedRect(lx, wY, lw, wH, UiTheme::RADIUS_CARD, cardBg, true);
  const int nCol = 3, nRow = 2;
  const int cellW = lw / nCol, cellH = wH / nRow;
  TTF_Font *fWx = m_fontClockSm ? m_fontClockSm : m_fontMedium;
  int hS = textHeight(m_fontSmall);
  // Canh theo mực in (đo thực tế): Rajdhani60 ink digits 17..56/hộp 77
  // (21px đáy trống), Noto24 ink 9..26/hộp 34. Xếp visual gap 6px:
  // code gap cũ 2px cho visual tới 32px (21 + 2 + 9).
  int yTrel = 0;
  int yUrel = 56 + 12 - 9;         // đáy ink giờ + gap - đầu bearing UTC
  int yNrel = yUrel + 26 + 12 - 9; // đáy ink UTC + gap - đầu bearing tên
  int block = yNrel + hS;
  auto wcities = ClockStore::instance().worldCities(); // luôn 6 TP (lưu DB)
  for (int i = 0; i < nCol * nRow; i++) {
    int cx0 = lx + (i % nCol) * cellW;
    int ry0 = wY + (i / nCol) * cellH;
    struct tm ct = {};
    long off = 0;
    char tstr[16] = "--:--";
    std::string ustr;
    if (cityLocal(wcities[i].tz.c_str(), ct, off)) {
      snprintf(tstr, sizeof(tstr), "%02d:%02d", ct.tm_hour, ct.tm_min);
      ustr = utcLabel(wcities[i].tz.c_str());
    }
    int cy0 = ry0 + (cellH - block) / 2;
    if (m_clkFocus == 0 && i == m_citySel)
      drawHighlight(cx0 + 6, cy0 - 4, cellW - 12, block + 8);
    int ccx = cx0 + cellW / 2;
    int yT = cy0 + yTrel;
    int yU = cy0 + yUrel;
    int yN = cy0 + yNrel;
    drawText(tstr, ccx, yT, {0, 180, 216, 255}, fWx, true);
    drawText(ustr, ccx, yU, {148, 163, 184, 255}, m_fontSmall, true);
    drawText(truncateToWidth(wcities[i].name, m_fontSmall, cellW - 16), ccx, yN,
             {255, 255, 255, 255}, m_fontSmall, true);
    if (i % nCol + 1 < nCol)
      drawRect(cx0 + cellW - 1, ry0 + 14, 1, cellH - 28, {30, 41, 59, 255},
               true);
  }
  drawRect(lx + 16, wY + cellH - 1, lw - 32, 1, {30, 41, 59, 255}, true);

  // ---- Cột phải: báo thức + pomodoro (rect từ clkBlockRect: 1 nguồn) ----
  const int rx = 520, rw = 480;
  auto alarms = ClockStore::instance().alarms();
  int _bx, _by, _bw, listH;
  clkBlockRect(1, _bx, _by, _bw, listH);
  int _px, _py, _pw, _ph;
  clkBlockRect(2, _px, _py, _pw, _ph);
  SDL_Color alarmBg = (m_clkFocus == 1) ? SDL_Color{30, 38, 54, 255}
                                        : SDL_Color{22, 28, 38, 255};
  drawRoundedRect(rx, contentTop, rw, listH, UiTheme::RADIUS_CARD, alarmBg,
                  true);
  drawText("BÁO THỨC", rx + 20, contentTop + 12, {0, 180, 216, 255},
           m_fontSmall, false);
  int ay = contentTop + 42;
  if (alarms.empty()) {
    drawText("Chưa có báo thức.", rx + 20, ay, {100, 110, 125, 255},
             m_fontSmall, false);
  }
  for (size_t i = 0; i < alarms.size() && i < 6; ++i) {
    if (i == (size_t)m_clkSel && m_clkFocus == 1)
      drawHighlight(rx + 8, ay + 1, rw - 16, 32);
    char ab[16];
    snprintf(ab, sizeof(ab), "%02d:%02d", alarms[i].hour, alarms[i].minute);
    drawText(ab, rx + 20, ay,
             alarms[i].enabled ? SDL_Color{255, 255, 255, 255}
                               : SDL_Color{100, 110, 125, 255},
             m_fontSmall, false);
    drawText(alarms[i].enabled ? "BẬT" : "TẮT", rx + 110, ay,
             alarms[i].enabled ? SDL_Color{34, 197, 94, 255}
                               : SDL_Color{100, 110, 125, 255},
             m_fontSmall, false);
    // Chế độ từng báo thức, right-align cuối hàng.
    drawTextRight(alarmModeLabel(alarms[i].mode), rx + rw - 20, ay,
                  alarms[i].enabled ? SDL_Color{0, 180, 216, 255}
                                    : SDL_Color{100, 110, 125, 255},
                  m_fontSmall);
    ay += 34;
  }

  // Pomodoro (rect từ clkBlockRect: 1 nguồn)
  int py = _py, ph = _ph;
  SDL_Color pomoBg = (m_clkFocus == 2) ? SDL_Color{30, 38, 54, 255}
                                       : SDL_Color{22, 28, 38, 255};
  drawRoundedRect(rx, py, rw, ph, UiTheme::RADIUS_CARD, pomoBg, true);
  drawText("POMODORO 25/5", rx + 20, py + 12, {0, 180, 216, 255}, m_fontSmall,
           false);
  uint32_t leftMs = m_pomoLeftMs;
  if (m_pomoRun) {
    uint32_t nowMs = SDL_GetTicks();
    leftMs = (m_pomoEndMs > nowMs) ? (m_pomoEndMs - nowMs) : 0;
  }
  char pbuf[16];
  snprintf(pbuf, sizeof(pbuf), "%02d:%02d", leftMs / 60000,
           (leftMs / 1000) % 60);
  drawText(pbuf, rx + 20, py + 40, {255, 255, 255, 255}, m_fontLarge, false);
  drawText(m_pomoWork ? "LÀM VIỆC" : "NGHỈ", rx + 170, py + 52,
           m_pomoWork ? SDL_Color{250, 204, 21, 255}
                      : SDL_Color{34, 197, 94, 255},
           m_fontSmall, false);
  // Thanh tiến trình phase
  uint32_t phaseTotal = (m_pomoWork ? 25 : 5) * 60 * 1000;
  float frac = phaseTotal > 0 ? 1.0f - (float)leftMs / (float)phaseTotal : 0;
  if (frac < 0)
    frac = 0;
  if (frac > 1)
    frac = 1;
  int barW = rw - 40;
  drawRect(rx + 20, py + 100, barW, 10, {35, 45, 60, 255}, true);
  drawRect(rx + 20, py + 100, (int)(barW * frac), 10,
           m_pomoWork ? SDL_Color{250, 204, 21, 255}
                      : SDL_Color{34, 197, 94, 255},
           true);
  drawText(m_pomoRun ? "A: Tạm dừng" : "A: Bắt đầu", rx + 20, py + 120,
           {148, 163, 184, 255}, m_fontSmall, false);

  // (Focus thể hiện bằng nền card sáng hơn — không viền, theo rule)

  if (m_clkFocus == 2) {
    drawAppFooter({{UiTheme::PadBtn::A, "Chạy/Dừng"},
                   {UiTheme::PadBtn::X, "Đặt lại"},
                   {UiTheme::PadBtn::SELECT, "Toàn màn hình"},
                   {UiTheme::PadBtn::DPAD, "Di chuyển"},
                   {UiTheme::PadBtn::B, "Lùi"},
                   {UiTheme::PadBtn::L1R1, "Tab"}});
  } else if (m_clkFocus == 0) {
    drawAppFooter({{UiTheme::PadBtn::A, "Đổi thành phố"},
                   {UiTheme::PadBtn::Y, "Thêm báo thức"},
                   {UiTheme::PadBtn::SELECT, "Toàn màn hình"},
                   {UiTheme::PadBtn::DPAD, "Di chuyển"},
                   {UiTheme::PadBtn::B, "Lùi"},
                   {UiTheme::PadBtn::L1R1, "Tab"}});
  } else {
    drawAppFooter({{UiTheme::PadBtn::A, "Bật/tắt"},
                   {UiTheme::PadBtn::Y, "Thêm báo thức"},
                   {UiTheme::PadBtn::X, "Xóa báo thức"},
                   {UiTheme::PadBtn::SELECT, "Toàn màn hình"},
                   {UiTheme::PadBtn::DPAD, "Di chuyển"},
                   {UiTheme::PadBtn::START, "Sửa"},
                   {UiTheme::PadBtn::B, "Lùi"},
                   {UiTheme::PadBtn::L1R1, "Tab"}});
  }

  // Popup đặt giờ / chọn thành phố phủ trên clock.
  if (m_apOpen)
    renderAlarmPopup();
  if (m_cityPickOpen)
    renderCityPicker();
}

bool UIManager::handleClockInput() {
  InputManager &input = InputManager::instance();
  // Popup đặt giờ / chọn thành phố mở thì route vào popup
  if (m_apOpen)
    return handleAlarmPopupInput();
  if (m_cityPickOpen)
    return handleCityPickerInput();
  // Fullscreen: B quay về trang đồng hồ, ◀/▶ đổi style, nuốt nút còn lại.
  if (m_clkFullscreen) {
    if (input.isButtonJustPressed(Button::B)) {
      m_clkFullscreen = false;
      unlink("/tmp/stay_awake");
    }
    else if (input.isButtonJustPressed(Button::LEFT))
      cycleClkFsStyle(-1);
    else if (input.isButtonJustPressed(Button::RIGHT))
      cycleClkFsStyle(1);
    return true;
  }
  if (input.isButtonJustPressed(Button::B)) {
    goBack(); // về trang trước
    return true;
  }
  if (input.isButtonJustPressed(Button::SELECT)) {
    int s = atoi(
        DatabaseManager::instance().getSetting("clock_fs_style", "0").c_str());
    m_clkFsStyle = (s >= 0 && s <= 1) ? s : 0;
    for (int i = 0; i < 3; i++) {
      m_fsD[i].cur.clear();
      m_fsD[i].prev.clear();
      m_fsD[i].ani = false;
      m_fsD[i].prog = 0.0f;
    }
    m_clkFullscreen = true;
    // Fullscreen clock: bypass sleep (always awake) theo cùng quy ước
    // /tmp/stay_awake như MpvPlayer/IPTV.
    {
      FILE* fw = fopen("/tmp/stay_awake", "w");
      if (fw) {
        fputs("1\n", fw);
        fclose(fw);
      }
    }
    m_clkFsMsg = "Trái/Phải: Đổi kiểu  •  B: Thoát";
    m_clkFsMsgUntil = SDL_GetTicks() + 2500;
    return true;
  }
  // Y thêm báo thức: mọi focus, không giới hạn khối.
  if (input.isButtonJustPressed(Button::Y)) {
    openAlarmPopup(-1);
    return true;
  }
  if (input.isButtonJustPressed(Button::START)) {
    // Sửa báo thức đang chọn (giờ + chế độ) qua popup
    if (m_clkFocus == 1)
      openAlarmPopup(m_clkSel);
    return true;
  }
  bool l1Tab = input.isButtonJustPressed(Button::L1);
  bool r1Tab = input.isButtonJustPressed(Button::R1);
  if (l1Tab || r1Tab) {
    // L1: lùi tab (sang trái), R1: tới tab (sang phải).
    m_wxTab = (m_wxTab + (l1Tab ? 3 : 1)) % 4;
    if (m_wxTab == 2) {
      m_camView = true;
      m_camVisDirty = true;
      restartCamView();
    }
    return true;
  }
  // Điều hướng không gian: trong khối di chuyển content, tới biên nhảy
  // sang khối liền kề (rule card>khối>content).
  if (input.isButtonJustPressed(Button::UP)) {
    if (m_clkFocus == 0) {
      if (m_citySel >= 3)
        m_citySel -= 3; // lưới 3 cột: lên hàng trên
      else
        clkFocusMove(0, -1);
    } else if (m_clkFocus == 1) {
      auto alarms = ClockStore::instance().alarms();
      if (!alarms.empty() && m_clkSel > 0)
        m_clkSel--;
      else
        clkFocusMove(0, -1);
    } else {
      clkFocusMove(0, -1); // pomodoro: lên là sang khối trên
    }
    return true;
  }
  if (input.isButtonJustPressed(Button::DOWN)) {
    if (m_clkFocus == 0) {
      if (m_citySel < 3)
        m_citySel += 3; // lưới 3 cột: xuống hàng dưới
      else
        clkFocusMove(0, 1);
    } else if (m_clkFocus == 1) {
      auto alarms = ClockStore::instance().alarms();
      if (!alarms.empty() && m_clkSel + 1 < (int)alarms.size())
        m_clkSel++;
      else
        clkFocusMove(0, 1);
    } else {
      clkFocusMove(0, 1);
    }
    return true;
  }
  if (input.isButtonJustPressed(Button::LEFT)) {
    if (m_clkFocus == 1) {
      auto alarms = ClockStore::instance().alarms();
      if (m_clkSel >= 0 && m_clkSel < (int)alarms.size())
        ClockStore::instance().nudgeAlarm(m_clkSel, 0, -5);
    } else if (m_clkFocus == 0) {
      if (m_citySel % 3 > 0)
        m_citySel--; // trong lưới 3 cột: sang ô trái
      else
        clkFocusMove(-1, 0); // tới biên trái: nhảy sang khối liền kề
    } else {
      clkFocusMove(-1, 0);
    }
    return true;
  }
  if (input.isButtonJustPressed(Button::RIGHT)) {
    if (m_clkFocus == 1) {
      auto alarms = ClockStore::instance().alarms();
      if (m_clkSel >= 0 && m_clkSel < (int)alarms.size())
        ClockStore::instance().nudgeAlarm(m_clkSel, 0, 5);
    } else if (m_clkFocus == 0) {
      if (m_citySel % 3 < 2)
        m_citySel++; // trong lưới 3 cột: sang ô phải
      else
        clkFocusMove(1, 0); // tới biên phải: nhảy sang khối liền kề
    } else {
      clkFocusMove(1, 0);
    }
    return true;
  }
  if (m_clkFocus == 2) {
    // Pomodoro: A chạy/dừng, X đặt lại
    if (input.isButtonJustPressed(Button::A)) {
      uint32_t nowMs = SDL_GetTicks();
      if (m_pomoRun) {
        m_pomoLeftMs = (m_pomoEndMs > nowMs) ? (m_pomoEndMs - nowMs) : 0;
        m_pomoRun = false;
      } else {
        if (m_pomoLeftMs == 0)
          m_pomoLeftMs = (m_pomoWork ? 25 : 5) * 60 * 1000;
        m_pomoEndMs = nowMs + m_pomoLeftMs;
        m_pomoRun = true;
      }
    } else if (input.isButtonJustPressed(Button::X)) {
      m_pomoRun = false;
      m_pomoLeftMs = (m_pomoWork ? 25 : 5) * 60 * 1000;
    }
    return true;
  }
  if (m_clkFocus == 0) {
    // Khối giờ thế giới: A đổi thành phố tại ô đang highlight.
    if (input.isButtonJustPressed(Button::A))
      openCityPicker();
    return true;
  }
  if (m_clkFocus != 1)
    return true; // pomodoro đã xử lý ở trên
  // Báo thức: A bật/tắt, X xóa, START sửa (popup). Y thêm đã xử lý
  // ở trên (mọi focus). UP/DOWN/LEFT/RIGHT đã xử lý ở trên.
  auto alarms = ClockStore::instance().alarms();
  if (input.isButtonJustPressed(Button::A) && !alarms.empty()) {
    if (m_clkSel >= 0 && m_clkSel < (int)alarms.size())
      ClockStore::instance().toggleAlarm(m_clkSel);
  } else if (input.isButtonJustPressed(Button::X)) {
    if (m_clkSel >= 0 && !alarms.empty()) {
      ClockStore::instance().removeAlarm(m_clkSel);
      if (m_clkSel > 0)
        m_clkSel--;
    }
  } else if (input.isButtonJustPressed(Button::START)) {
    if (m_clkSel >= 0 && !alarms.empty())
      openAlarmPopup(m_clkSel);
  }
  return true;
}

void UIManager::fireAlarm(const std::string &title,
                          const std::vector<std::string> &lines, int mode) {
  m_alarmTitle = title;
  m_alarmLines = lines;
  m_alarmRing = true;
  if (mode < 0 || mode > 2)
    mode = 0;
  // Kêu/rung liên tục tới khi user A tắt / B hoãn (stopAlert).
  if (mode == 0) {
    beepStart(0);
    vibrateStart(0);
  } else if (mode == 1) {
    beepStart(0);
  } else {
    vibrateStart(0);
  }
  setState(UIState::CLOCK_ALARM);
}

static void stopAlert() {
  beepStop();
  vibrateStop();
}

void UIManager::pollClock() {
  uint32_t nowMs = SDL_GetTicks();
  if (nowMs - m_lastClockPoll < 5000)
    return;
  m_lastClockPoll = nowMs;
  if (m_alarmRing)
    return;

  // Pomodoro hết phase
  if (m_pomoRun && nowMs >= m_pomoEndMs) {
    m_pomoRun = false;
    m_pomoWork = !m_pomoWork;
    m_pomoLeftMs = (m_pomoWork ? 25 : 5) * 60 * 1000;
    int mode = alarmMode();
    if (mode == 0) {
      beepStart(5);
      vibrateStart(5);
    } else if (mode == 1) {
      beepStart(5);
    } else {
      vibrateStart(5);
    }
    showToast(m_pomoWork ? "Hết giờ nghỉ! Vào việc."
                         : "Hết 25 phút! Nghỉ 5 phút.",
              {250, 204, 21, 255}, 5000);
  }

  // Báo thức khớp HH:MM (giờ Brick)
  std::time_t tnow = std::time(nullptr);
  struct tm *lt = std::localtime(&tnow);
  int dayKey =
      (lt->tm_year + 1900) * 10000 + (lt->tm_mon + 1) * 100 + lt->tm_mday;
  // Hoãn 10 phút
  if (m_alarmSnoozeMin >= 0 && dayKey == m_alarmSnoozeDay) {
    int curMin = lt->tm_hour * 60 + lt->tm_min;
    if (curMin >= m_alarmSnoozeMin &&
        m_alarmFiredKey != dayKey * 1440 + m_alarmSnoozeMin + 100000) {
      m_alarmFiredKey = dayKey * 1440 + m_alarmSnoozeMin + 100000;
      fireAlarm("Báo thức", {}, alarmMode());
      return;
    }
  }
  int curMin = lt->tm_hour * 60 + lt->tm_min;
  for (const auto &a : ClockStore::instance().alarms()) {
    if (!a.enabled)
      continue;
    if (a.hour * 60 + a.minute != curMin)
      continue;
    int key = dayKey * 1440 + curMin;
    if (key == m_alarmFiredKey)
      continue;
    m_alarmFiredKey = key;
    fireAlarm("Báo thức", {}, a.mode);
    return;
  }

  // Nhắc sự kiện lịch có cờ remind: tới giờ bắt đầu thì kêu 1 lần.
  if (dayKey != m_evFiredDayKey) {
    m_evFiredDayKey = dayKey;
    m_evFired.clear();
  }
  {
    int64_t nowTs = (int64_t)tnow;
    for (const auto &e : CalManager::instance().upcoming(20)) {
      if (!e.remind || e.allDay)
        continue;
      if (e.startTs > nowTs || nowTs - e.startTs > 300)
        continue;
      if (m_evFired.count(e.id))
        continue;
      m_evFired.insert(e.id);
      std::time_t tt = (time_t)e.startTs;
      struct tm *l = std::localtime(&tt);
      char tb[32];
      snprintf(tb, sizeof(tb), "%02d:%02d", l ? l->tm_hour : 0,
               l ? l->tm_min : 0);
      fireAlarm(e.title, {std::string("Tới giờ: ") + tb}, alarmMode());
      return;
    }
  }
}

void UIManager::openAlarmPopup(int editIdx) {
  m_apEditIdx = editIdx;
  m_apPart = 0;
  m_apCol = 0;
  if (editIdx >= 0) {
    auto v = ClockStore::instance().alarms();
    if (editIdx < (int)v.size()) {
      m_apH = v[editIdx].hour;
      m_apM = v[editIdx].minute;
      m_apMode = v[editIdx].mode;
    } else {
      m_apEditIdx = -1;
    }
  }
  if (m_apEditIdx < 0) {
    m_apH = 7;
    m_apM = 0;
    m_apMode = alarmMode();
  }
  if (m_apMode < 0 || m_apMode > 2)
    m_apMode = 0;
  m_apOpen = true;
}

void UIManager::renderAlarmPopup() {
  // Card 2 cột 1 hàng trong container ExpMenu (title 52):
  // trái = giờ 24h (HH:MM), phải = 3 chế độ. Phải/trái đi
  // HH → MM → chế độ → vòng lại; lên/xuống đổi giá trị tại chỗ.
  beginModalDim();
  const int boxW = 560, titleH = 52, contentH = 160, hintH = 40;
  const int boxH = titleH + contentH + hintH;
  const int boxX = (1024 - boxW) / 2;
  int boxY = (768 - boxH) / 2;
  if (boxY < 70)
    boxY = 70;
  drawModalDialog(boxX, boxY, boxW, boxH, UiTheme::RADIUS_MODAL,
                  {24, 28, 38, 255}, {18, 55, 95, 255}, titleH);
  drawText(m_apEditIdx < 0 ? "THÊM BÁO THỨC" : "SỬA BÁO THỨC", boxX + 16,
           boxY + (titleH - textHeight(m_fontSmall)) / 2, {255, 255, 255, 255},
           m_fontSmall, false);

  int y0 = boxY + titleH;
  // Cột trái: HH : MM (24h).
  const int leftX = boxX + 24, leftW = 250;
  {
    const int bw = 84, bh = 56, gap = 8;
    int sx = leftX + (leftW - (bw + gap + 16 + gap + bw)) / 2;
    int by = y0 + (contentH - bh) / 2;
    char hb[8], mb[8];
    snprintf(hb, sizeof(hb), "%02d", m_apH);
    snprintf(mb, sizeof(mb), "%02d", m_apM);
    const char *vals[2] = {hb, mb};
    for (int i = 0; i < 2; i++) {
      int bx = (i == 0) ? sx : sx + bw + gap + 16 + gap;
      bool foc = (m_apPart == 0 && m_apCol == i);
      drawRoundedRect(
          bx, by, bw, bh, 10,
          foc ? SDL_Color{30, 58, 138, 255} : SDL_Color{35, 45, 62, 255}, true);
      drawText(vals[i], bx + bw / 2, by + (bh - textHeight(m_fontLarge)) / 2,
               foc ? SDL_Color{255, 255, 255, 255}
                   : SDL_Color{203, 213, 225, 255},
               m_fontLarge, true);
    }
    drawText(":", sx + bw + gap, by + (bh - textHeight(m_fontLarge)) / 2,
             {100, 110, 125, 255}, m_fontLarge, false);
  }
  // Vạch chia 2 cột.
  drawRect(leftX + leftW + 8, y0 + 14, 1, contentH - 28, {30, 41, 59, 255},
           true);
  // Cột phải: 3 chế độ radio.
  const int rightX = leftX + leftW + 24, rightW = boxW - (rightX - boxX) - 24;
  static const char *modes[] = {"Chuông + Rung", "Chỉ chuông", "Chỉ rung"};
  const int rowH = 44;
  int ry = y0 + (contentH - 3 * rowH) / 2;
  for (int i = 0; i < 3; i++) {
    if (i == m_apMode) {
      if (m_apPart == 1)
        drawHighlight(rightX, ry + 3, rightW, rowH - 6);
      else
        drawRect(rightX, ry + 3, rightW, rowH - 6, {35, 45, 62, 255}, true);
    }
    drawText(modes[i], rightX + 14, ry + (rowH - textHeight(m_fontSmall)) / 2,
             i == m_apMode ? SDL_Color{255, 255, 255, 255}
                           : SDL_Color{148, 163, 184, 255},
             m_fontSmall, false);
    ry += rowH;
  }
  drawInlineHintsCentered("←→ Ô  •  ↑↓ Đổi  •  A Lưu  •  B Hủy", 512,
                          boxY + boxH - 32, {148, 163, 184, 255}, m_fontSmall,
                          24);
}

bool UIManager::handleAlarmPopupInput() {
  InputManager &input = InputManager::instance();
  if (input.isButtonJustPressed(Button::B)) {
    m_apOpen = false;
    return true;
  }
  // ◀▶ đi qua HH → MM → chế độ → vòng lại HH.
  if (input.isButtonJustPressed(Button::LEFT)) {
    if (m_apPart == 1) {
      m_apPart = 0;
      m_apCol = 1;
    } else if (m_apCol == 1) {
      m_apCol = 0;
    } else {
      m_apPart = 1; // hết HH vòng sang khối chế độ
    }
    return true;
  }
  if (input.isButtonJustPressed(Button::RIGHT)) {
    if (m_apPart == 1) {
      m_apPart = 0;
      m_apCol = 0;
    } else if (m_apCol == 0) {
      m_apCol = 1;
    } else {
      m_apPart = 1; // hết MM sang khối chế độ
    }
    return true;
  }
  if (input.isButtonJustPressed(Button::UP)) {
    if (m_apPart == 0) {
      if (m_apCol == 0)
        m_apH = (m_apH + 1) % 24; // giờ 24h bước 1
      else
        m_apM = (m_apM + 1) % 60; // phút bước 1
    } else {
      m_apMode = (m_apMode + 2) % 3; // radio lên
    }
    return true;
  }
  if (input.isButtonJustPressed(Button::DOWN)) {
    if (m_apPart == 0) {
      if (m_apCol == 0)
        m_apH = (m_apH + 23) % 24;
      else
        m_apM = (m_apM + 59) % 60;
    } else {
      m_apMode = (m_apMode + 1) % 3; // radio xuống
    }
    return true;
  }
  if (input.isButtonJustPressed(Button::A)) {
    auto v = ClockStore::instance().alarms();
    if (m_apEditIdx >= 0 && m_apEditIdx < (int)v.size()) {
      v[m_apEditIdx].hour = m_apH;
      v[m_apEditIdx].minute = m_apM;
      v[m_apEditIdx].mode = m_apMode;
      v[m_apEditIdx].enabled = true;
      ClockStore::instance().saveAlarms(v);
      showToast("Đã sửa báo thức!", {34, 197, 94, 255}, 2000);
    } else {
      if (v.size() < 10) {
        ClockAlarm a;
        a.hour = m_apH;
        a.minute = m_apM;
        a.enabled = true;
        a.mode = m_apMode;
        v.push_back(a);
        ClockStore::instance().saveAlarms(v);
        m_clkSel = (int)v.size() - 1;
        showToast("Đã thêm báo thức!", {34, 197, 94, 255}, 2000);
      } else {
        showToast("Tối đa 10 báo thức!", {239, 68, 68, 255}, 2000);
      }
    }
    m_apOpen = false;
    return true;
  }
  return true;
}

void UIManager::renderAlarmRing() {
  // Modal báo thức: briefing + A tắt / B hoãn 10 phút
  drawAppBackground();
  beginModalDim();
  const int boxW = 640;
  int n = (int)m_alarmLines.size();
  if (n > 6)
    n = 6;
  const int boxH = 120 + n * 34 + 60;
  const int boxX = (1024 - boxW) / 2;
  const int boxY = (768 - boxH) / 2;
  drawModalDialog(boxX, boxY, boxW, boxH, UiTheme::RADIUS_MODAL,
                  {24, 28, 38, 255}, UiTheme::ACCENT_BLUE, 56);
  drawText(truncateToWidth(m_alarmTitle, m_fontLarge, boxW - 40), 512,
           boxY + 14, {255, 255, 255, 255}, m_fontLarge, true);
  int y = boxY + 70;
  for (int i = 0; i < n; ++i) {
    drawText(truncateToWidth(m_alarmLines[i], m_fontSmall, boxW - 60),
             boxX + 30, y, {203, 213, 225, 255}, m_fontSmall, false);
    y += 34;
  }
  drawInlineHintsCentered("A Tắt  •  B Hoãn 10 phút", 512, boxY + boxH - 36,
                          {148, 163, 184, 255}, m_fontSmall, 24);
}

bool UIManager::handleAlarmRingInput() {
  InputManager &input = InputManager::instance();
  if (input.isButtonJustPressed(Button::A)) {
    stopAlert();
    m_alarmRing = false;
    m_alarmSnoozeMin = -1;
    goBack();
    return true;
  }
  if (input.isButtonJustPressed(Button::B)) {
    stopAlert();
    m_alarmRing = false;
    // Hoãn 10 phút từ giờ
    std::time_t tnow = std::time(nullptr);
    struct tm *lt = std::localtime(&tnow);
    m_alarmSnoozeDay =
        (lt->tm_year + 1900) * 10000 + (lt->tm_mon + 1) * 100 + lt->tm_mday;
    m_alarmSnoozeMin = lt->tm_hour * 60 + lt->tm_min + 10;
    if (m_alarmSnoozeMin >= 1440) {
      m_alarmSnoozeMin -= 1440;
      // qua ngày: cộng 1 vào day key (gần đúng, đủ cho hoãn)
      m_alarmSnoozeDay += 1;
    }
    showToast("Hoãn 10 phút", {245, 158, 11, 255}, 2000);
    goBack();
    return true;
  }
  return true;
}

void UIManager::openCityPicker() {
  m_cityPickOpen = true;
  // Nhảy tới TP đang gán ở ô highlight để thấy vị trí hiện tại.
  auto cur = ClockStore::instance().worldCities();
  std::string tz = (m_citySel >= 0 && m_citySel < (int)cur.size())
                       ? cur[m_citySel].tz
                       : "";
  m_cityPickSel = 0;
  for (int i = 0; i < kAllCityCount; i++) {
    if (tz == kAllCities[i].tz) {
      m_cityPickSel = i;
      break;
    }
  }
  m_cityPickScroll = 0;
  const int VIS = 7;
  if (m_cityPickSel >= VIS)
    m_cityPickScroll = m_cityPickSel - VIS + 1;
}

void UIManager::renderCityPicker() {
  beginModalDim();
  const int boxW = 640, titleH = 52, rowH = 56, VIS = 7, hintH = 44;
  const int contentH = VIS * rowH;
  const int boxH = titleH + contentH + hintH;
  const int boxX = (1024 - boxW) / 2;
  int boxY = (768 - boxH) / 2;
  if (boxY < 70)
    boxY = 70;
  drawModalDialog(boxX, boxY, boxW, boxH, UiTheme::RADIUS_MODAL,
                  {24, 28, 38, 255}, {18, 55, 95, 255}, titleH);
  drawText("ĐỔI THÀNH PHỐ", boxX + 16,
           boxY + (titleH - textHeight(m_fontSmall)) / 2, {255, 255, 255, 255},
           m_fontSmall, false);
  int y0 = boxY + titleH;
  for (int v = 0; v < VIS; v++) {
    int idx = m_cityPickScroll + v;
    if (idx >= kAllCityCount)
      break;
    int ry = y0 + v * rowH;
    int ty = ry + (rowH - textHeight(m_fontSmall)) / 2;
    if (idx == m_cityPickSel)
      drawHighlight(boxX + 8, ry + 4, boxW - 16, rowH - 8);
    drawText(kAllCities[idx].name, boxX + 24, ty,
             idx == m_cityPickSel ? SDL_Color{255, 255, 255, 255}
                                 : SDL_Color{203, 213, 225, 255},
             m_fontSmall, false);
    drawTextRight(utcLabel(kAllCities[idx].tz), boxX + boxW - 24, ty,
                  {0, 180, 216, 255}, m_fontSmall);
  }
  drawInlineHintsCentered("↑↓ Chọn  •  A Đổi  •  B Hủy", 512, boxY + boxH - 32,
                          {148, 163, 184, 255}, m_fontSmall, 24);
}

bool UIManager::handleCityPickerInput() {
  InputManager &input = InputManager::instance();
  const int VIS = 7;
  if (input.isButtonJustPressed(Button::B)) {
    m_cityPickOpen = false;
    return true;
  }
  if (input.isButtonJustPressed(Button::UP)) {
    m_cityPickSel = (m_cityPickSel + kAllCityCount - 1) % kAllCityCount;
    if (m_cityPickSel < m_cityPickScroll)
      m_cityPickScroll = m_cityPickSel;
    return true;
  }
  if (input.isButtonJustPressed(Button::DOWN)) {
    m_cityPickSel = (m_cityPickSel + 1) % kAllCityCount;
    if (m_cityPickSel >= m_cityPickScroll + VIS)
      m_cityPickScroll = m_cityPickSel - VIS + 1;
    return true;
  }
  if (input.isButtonJustPressed(Button::A)) {
    auto v = ClockStore::instance().worldCities();
    if (m_citySel >= 0 && m_citySel < (int)v.size() &&
        m_cityPickSel >= 0 && m_cityPickSel < kAllCityCount) {
      v[m_citySel].name = kAllCities[m_cityPickSel].name;
      v[m_citySel].tz = kAllCities[m_cityPickSel].tz;
      ClockStore::instance().saveWorldCities(v);
      showToast(std::string("Đã đổi thành ") + kAllCities[m_cityPickSel].name,
                {34, 197, 94, 255}, 2000);
    }
    m_cityPickOpen = false;
    return true;
  }
  return true;
}

} // namespace RomCloud
