#include "../filesystem/FileSystemManager.h"
#include "UIManager.h"


namespace RomCloud {

void UIManager::renderFileExplorer() {
  if (expA().creatingFolder() || expA().renaming()) {
    renderExplorerKeyboard();
    return;
  }
  drawAppBackground();
  // Header 0..64 borderless
  drawRect(0, 0, 1024, 64, {15, 23, 42, 255}, true);
  drawRect(0, 64, 1024, 1, {30, 41, 59, 255}, true);
  drawGridIcon("EXPLORER.png", 16, 14, 36, 36);
  drawText("FILE EXPLORER", 60, 16, {255, 255, 255, 255}, m_fontLarge, false);
  // Bao pane active: icon duy nhat top-center (36px, giua 1024x64)
  drawGridIcon(m_expActive == 0 ? "LEFT.png" : "RIGHT.png", 494, 14, 36, 36);
  if (m_expClip.active()) {
    std::string b = m_expClip.op == ExplorerClipboard::Op::CUT ? "CUT" : "COPY";
    drawText(b + ": " + m_expClip.srcPath, 60, 42, {245, 158, 11, 255},
             m_fontSmall, false);
  }
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
    // path bar
    std::string path = ex.currentPath();
    const std::string root = "/mnt/SDCARD";
    std::string sp = path;
    if (sp.compare(0, root.size(), root) == 0)
      sp = sp.substr(root.size());
    if (sp.empty())
      sp = "/";
    if ((int)sp.size() > 40)
      sp = ".." + sp.substr(sp.size() - 38);
    drawText((p == 0 ? "L: " : "R: ") + sp, px + 12, paneY + 6,
             active ? SDL_Color{255, 255, 255, 255}
                    : SDL_Color{148, 163, 184, 255},
             m_fontSmall, false);
    const auto &entries = ex.entries();
    int total = (int)entries.size();
    int rowH = 56;
    int listTop = paneY + 30;
    int visibleRows = (paneH - 30 - 8) / rowH;
    if (visibleRows < 1)
      visibleRows = 1;
    m_expScroll[p] =
        FileListView::calcScroll(ex.selected(), visibleRows, m_expScroll[p]);
    for (int i = 0; i < visibleRows && m_expScroll[p] + i < total; ++i) {
      int idx = m_expScroll[p] + i;
      int y = listTop + i * rowH;
      bool sel = (idx == ex.selected()) && active;
      if (sel) {
        drawRect(px + 8, y, paneW - 16, rowH - 6, {30, 58, 138, 255}, true);
        drawRect(px + 8, y, 4, rowH - 6, {59, 130, 246, 255}, true);
      }
      const auto &e = entries[(size_t)idx];
      // Icon can giua doc trong row (rowH-6=50, icon 32 -> offset 9)
      drawGridIcon(e.isDir ? "FOLDER.png" : "FILES.png", px + 16, y + 9, 32,
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
      // drawRowMainSub: khoi ten+size gon (gap 1), keo blockH <= h -> het de hang duoi
      drawRowMainSub(px + 56, y, rowH - 6, e.name, m_fontMedium, sub,
                     m_fontSmall, paneW - 16 - 56 - 12, 1);
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
}

} // namespace RomCloud
