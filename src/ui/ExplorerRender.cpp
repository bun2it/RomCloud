#include "../filesystem/FileSystemManager.h"
#include "UIManager.h"


namespace RomCloud {

void UIManager::renderFileExplorer() {
  if (expA().creatingFolder() || expA().renaming()) {
    renderExplorerKeyboard();
    return;
  }
  drawAppBackground();
  // Header chuẩn app (giống Settings): bar + title cyan + cụm status phải.
  // Clipboard đang giữ thì hiện ở dòng sub của header.
  if (m_expClip.active()) {
    std::string b = m_expClip.op == ExplorerClipboard::Op::CUT ? "CUT" : "COPY";
    drawAppHeader("FILE EXPLORER", b + ": " + m_expClip.srcPath);
  } else {
    drawAppHeader("FILE EXPLORER");
  }
  // Icon pane đích top-center (giữ nguyên yêu cầu trước)
  // Icon chỉ pane ĐÍCH (nơi copy/move tới): active L -> R, active R -> L.
  drawGridIcon(m_expActive == 0 ? "RIGHT.png" : "LEFT.png", 494, 14, 36, 36);
  // 2 panes: 65..714, divider 1px tai 512
  const int paneY = 65, paneH = 650, paneW = 511;
  const int lx = 0, rx = 513;
  FileExplorer *exps[2] = {&m_expL, &m_expR};
  for (int p = 0; p < 2; ++p) {
    FileExplorer &ex = *exps[p];
    int px = (p == 0) ? lx : rx;
    bool active = (m_expActive == p);
    drawRect(px, paneY, paneW, paneH, {15, 23, 42, 255}, true);
    if (active)
      drawBorder(px, paneY, paneW, paneH, {59, 130, 246, 255}, 2);
    // Path bar to, font medium
    std::string path = ex.currentPath();
    const std::string root = "/mnt/SDCARD";
    std::string sp = path;
    if (sp.compare(0, root.size(), root) == 0)
      sp = sp.substr(root.size());
    if (sp.empty())
      sp = "/";
    if ((int)sp.size() > 30)
      sp = ".." + sp.substr(sp.size() - 28);
    drawText((p == 0 ? "L: " : "R: ") + sp, px + 12, paneY + 8,
             active ? SDL_Color{255, 255, 255, 255}
                    : SDL_Color{148, 163, 184, 255},
             m_fontMedium, false);
    const auto &entries = ex.entries();
    int total = (int)entries.size();
    int rowH = 49;
    int listTop = paneY + 48;
    int visibleRows = (paneH - 48 - 8) / rowH;
    if (visibleRows < 1)
      visibleRows = 1;
    m_expScroll[p] =
        FileListView::calcScroll(ex.selected(), visibleRows, m_expScroll[p]);
    for (int i = 0; i < visibleRows && m_expScroll[p] + i < total; ++i) {
      int idx = m_expScroll[p] + i;
      int y = listTop + i * rowH;
      bool sel = (idx == ex.selected()) && active;
      if (sel) {
        drawHighlight(px + 8, y + 3, paneW - 16, rowH - 6);
        drawRect(px + 8, y, 4, rowH - 6, {59, 130, 246, 255}, true);
      }
      const auto &e = entries[(size_t)idx];
      // Icon can giua doc trong row (rowH-6=42, icon 32 -> offset 5)
      drawGridIcon(e.isDir ? "FOLDER.png" : "FILES.png", px + 16, y + 5, 32,
                   32);
      std::string badge;
      if (m_expClip.active() && e.path == m_expClip.srcPath)
        badge = (m_expClip.op == ExplorerClipboard::Op::CUT ? "CUT" : "COPY");
      std::string sub =
          badge.empty()
              ? (e.isDir
                     ? ""
                     : FileSystemManager::instance().formatBytes(e.sizeBytes))
              : badge;
      // Tên trái + dung lượng cùng hàng, right-align theo pane
      int rowRight = px + paneW - 16;
      int sizeW = sub.empty() ? 0 : textWidth(sub, m_fontSmall);
      int nameMaxW = rowRight - (px + 56) - (sizeW > 0 ? sizeW + 16 : 0);
      if (nameMaxW < 50) nameMaxW = 50;
      drawText(truncateToWidth(e.name, m_fontMedium, nameMaxW), px + 56,
               textYCentered(y, rowH - 6, m_fontMedium), UiTheme::TEXT_MAIN,
               m_fontMedium, false);
      if (!sub.empty())
        drawTextRight(sub, rowRight, textYCentered(y, rowH - 6, m_fontSmall),
                      UiTheme::TEXT_SUB, m_fontSmall);
    }
    if (total == 0)
      drawText("(Trống)", px + paneW / 2, listTop + 60, {100, 116, 139, 255},
               m_fontMedium, true);
  }
  drawRect(512, paneY, 1, paneH, {30, 41, 59, 255}, true);
  // Footer 715 context
  drawRect(0, 715, 1024, 53, {18, 22, 30, 255}, true);
  drawRect(0, 715, 1024, 1, {40, 48, 62, 255}, true);
  if (m_expDeleteArmed)
    drawAppFooter({{UiTheme::PadBtn::A, "XÁC NHẬN"}, {UiTheme::PadBtn::B, "HỦY"}});
  else
    drawAppFooter({{UiTheme::PadBtn::A, "CHỌN"},
                   {UiTheme::PadBtn::B, "THOÁT"},
                   {UiTheme::PadBtn::X, "CHUYỂN ĐI"},
                   {UiTheme::PadBtn::Y, "COPY"},
                   {UiTheme::PadBtn::MENU, "XÓA"},
                   {UiTheme::PadBtn::START, "THƯ MỤC MỚI"}});
  if (m_expMenuOpen)
    renderExpMenu();
}

void UIManager::renderExpMenu() {  int n = (int)m_expMenuItems.size();
  if (n == 0) return;
  // Popup overlay giữa 2 pane: căn giữa màn hình, KHÔNG phủ mờ để
  // vẫn thấy cả 2 pane xung quanh. Nhỏ vừa đủ nội dung.
  const int boxW = 440;
  const int titleH = 52, rowH = 46, hintH = 40;
  const int boxH = titleH + n * rowH + hintH;
  int boxX = (1024 - boxW) / 2;
  int boxY = (715 - boxH) / 2;
  if (boxY < 70) boxY = 70;
  drawModalDialog(boxX, boxY, boxW, boxH, UiTheme::RADIUS_MODAL,
                  {24, 28, 38, 255}, {18, 55, 95, 255}, titleH);
  std::string title = m_expMenuFile;
  size_t sl = title.find_last_of('/');
  if (sl != std::string::npos) title = title.substr(sl + 1);
  drawText(truncateToWidth(title, m_fontSmall, boxW - 32), boxX + 16,
           boxY + (titleH - textHeight(m_fontSmall)) / 2,
           {255, 255, 255, 255}, m_fontSmall, false);
  int y = boxY + titleH;
  for (int i = 0; i < n; ++i) {
    if (i == m_expMenuSel)
      drawHighlight(boxX + 10, y + 3, boxW - 20, rowH - 6);
    drawText(m_expMenuItems[i], boxX + 24, y + (rowH - textHeight(m_fontSmall)) / 2,
             i == m_expMenuSel ? SDL_Color{255, 255, 255, 255}
                               : SDL_Color{203, 213, 225, 255},
             m_fontSmall, false);
    y += rowH;
  }
  drawInlineHintsCentered("A Chọn  •  B Đóng", boxX + boxW / 2, y + 8,
                          {148, 163, 184, 255}, m_fontSmall, 24);
}

void UIManager::renderTextViewer() {
  drawAppBackground();
  std::string title = m_textViewPath;
  size_t sl = title.find_last_of('/');
  if (sl != std::string::npos) title = title.substr(sl + 1);
  drawAppHeader(truncateToWidth(title, m_fontLarge, 700).c_str());

  const int cardX = 24, cardW = 1024 - 48;
  const int cardY = 124;
  const int cardH = 715 - cardY - 16;
  drawRoundedRect(cardX, cardY, cardW, cardH, UiTheme::RADIUS_CARD,
                  {22, 28, 38, 255}, true);
  drawRoundedBorder(cardX, cardY, cardW, cardH, UiTheme::RADIUS_CARD,
                    {51, 65, 85, 255}, 1);

  const int padL = 28, padT = 18;
  const int textW = cardW - padL * 2;
  const int lineGap = textHeight(m_fontSmall) + 4;
  const int contentTopY = cardY + padT;
  const int contentH = cardH - padT * 2;
  const int visibleLines = std::max(1, contentH / lineGap);
  int totalLines = (int)m_textViewLines.size();
  int maxScroll = std::max(0, totalLines - visibleLines);
  if (m_textViewScroll > maxScroll) m_textViewScroll = maxScroll;
  if (m_textViewScroll < 0) m_textViewScroll = 0;

  SDL_Rect clip = {cardX + padL, contentTopY, textW, contentH};
  SDL_RenderSetClipRect(m_renderer, &clip);
  int drawY = contentTopY;
  for (int i = m_textViewScroll; i < totalLines; ++i) {
    if (drawY + lineGap > contentTopY + contentH) break;
    if (!m_textViewLines[i].empty())
      drawText(m_textViewLines[i], cardX + padL, drawY,
               {203, 213, 225, 255}, m_fontSmall, false);
    drawY += lineGap;
  }
  SDL_RenderSetClipRect(m_renderer, nullptr);

  // Scrollbar
  if (totalLines > visibleLines) {
    int sbH = contentH;
    int sbX = cardX + cardW - 10;
    drawRect(sbX, contentTopY, 4, sbH, {35, 45, 60, 255}, true);
    int thumbH = std::max(20, sbH * visibleLines / totalLines);
    int thumbY = contentTopY + (sbH - thumbH) * m_textViewScroll /
                                  std::max(1, totalLines - visibleLines);
    drawRect(sbX, thumbY, 4, thumbH, {0, 180, 216, 200}, true);
  }

  drawAppFooter({{UiTheme::PadBtn::B, "Lùi"},
                 {UiTheme::PadBtn::DPAD, "Cuộn"},
                 {UiTheme::PadBtn::L1R1, "Trang"}});
}

} // namespace RomCloud
