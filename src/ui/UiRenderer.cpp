#include "UiRenderer.h"

#include <SDL2/SDL_image.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "../platform/PlatformInfo.h"

namespace RomCloud {

void UiRenderer::bind(SDL_Renderer *r, TTF_Font *small, TTF_Font *medium,
                      TTF_Font *large, const std::string &assetsDir) {
  m_r = r;
  m_fSmall = small;
  m_fMedium = medium;
  m_fLarge = large;
  m_assetsDir = assetsDir;
}

void UiRenderer::unbind() {
  clearTextCache();
  m_images.clear();
  m_r = nullptr;
}

void UiRenderer::clearTextCache() {
  for (auto &kv : m_textCache) {
    if (kv.second.texture)
      SDL_DestroyTexture(kv.second.texture);
  }
  m_textCache.clear();
}

SDL_Texture *UiRenderer::getImage(const std::string &key, int *outW,
                                  int *outH) {
  return m_images.get(key, outW, outH);
}

SDL_Texture *UiRenderer::getOrLoadImage(const std::string &key,
                                        const std::string &path) {
  if (key.empty() || path.empty() || !m_r)
    return nullptr;
  SDL_Texture *t = m_images.get(key);
  if (t)
    return t;
  SDL_Surface *surf = IMG_Load(path.c_str());
  if (!surf)
    return nullptr;
  t = SDL_CreateTextureFromSurface(m_r, surf);
  int iw = surf->w, ih = surf->h;
  SDL_FreeSurface(surf);
  if (!t)
    return nullptr;
  m_images.put(key, t, iw, ih);
  return t;
}

void UiRenderer::drawText(const std::string &text, int x, int y,
                          SDL_Color color, TTF_Font *font, bool centered) {
  if (!m_r || !font || text.empty())
    return;
  char keyBuf[128];
  uint32_t colorInt =
      (color.r << 24) | (color.g << 16) | (color.b << 8) | color.a;
  std::snprintf(keyBuf, sizeof(keyBuf), "%p_%08x_", (void *)font, colorInt);
  std::string key = std::string(keyBuf) + text;
  SDL_Texture *texture = nullptr;
  int texW = 0, texH = 0;
  auto it = m_textCache.find(key);
  if (it != m_textCache.end()) {
    texture = it->second.texture;
    texW = it->second.w;
    texH = it->second.h;
    it->second.lastUsed = SDL_GetTicks();
  } else {
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!surface)
      return;
    texture = SDL_CreateTextureFromSurface(m_r, surface);
    texW = surface->w;
    texH = surface->h;
    SDL_FreeSurface(surface);
    if (!texture)
      return;
    if (m_textCache.size() >= 256) {
      auto oldest = m_textCache.begin();
      for (auto iter = m_textCache.begin(); iter != m_textCache.end(); ++iter) {
        if (iter->second.lastUsed < oldest->second.lastUsed)
          oldest = iter;
      }
      if (oldest->second.texture)
        SDL_DestroyTexture(oldest->second.texture);
      m_textCache.erase(oldest);
    }
    m_textCache[key] = {texture, texW, texH, SDL_GetTicks()};
  }
  int sx = PlatformInfo::instance().scaleX(x);
  int sy = PlatformInfo::instance().scaleY(y);
  int drawX = centered ? (sx - texW / 2) : sx;
  SDL_Rect dstRect = {drawX, sy, texW, texH};
  SDL_RenderCopy(m_r, texture, nullptr, &dstRect);
}

void UiRenderer::drawRect(int x, int y, int w, int h, SDL_Color color,
                          bool filled) {
  if (!m_r)
    return;
  int sx = PlatformInfo::instance().scaleX(x);
  int sy = PlatformInfo::instance().scaleY(y);
  int sw = PlatformInfo::instance().scaleW(w);
  int sh = PlatformInfo::instance().scaleH(h);
  SDL_SetRenderDrawColor(m_r, color.r, color.g, color.b, color.a);
  SDL_Rect rect = {sx, sy, sw, sh};
  if (filled)
    SDL_RenderFillRect(m_r, &rect);
  else
    SDL_RenderDrawRect(m_r, &rect);
}

void UiRenderer::drawBorder(int x, int y, int w, int h, SDL_Color color,
                            int thickness) {
  if (!m_r)
    return;
  int sx = PlatformInfo::instance().scaleX(x);
  int sy = PlatformInfo::instance().scaleY(y);
  int sw = PlatformInfo::instance().scaleW(w);
  int sh = PlatformInfo::instance().scaleH(h);
  int st = PlatformInfo::instance().scaleW(thickness);
  SDL_SetRenderDrawColor(m_r, color.r, color.g, color.b, color.a);
  for (int i = 0; i < st; ++i) {
    SDL_Rect rect = {sx + i, sy + i, sw - 2 * i, sh - 2 * i};
    SDL_RenderDrawRect(m_r, &rect);
  }
}

void UiRenderer::drawRoundedRect(int x, int y, int w, int h, int radius,
                                 SDL_Color color, bool filled) {
  if (!m_r)
    return;
  int sx = PlatformInfo::instance().scaleX(x);
  int sy = PlatformInfo::instance().scaleY(y);
  int sw = PlatformInfo::instance().scaleW(w);
  int sh = PlatformInfo::instance().scaleH(h);
  int sr = PlatformInfo::instance().scaleW(radius);
  if (sw <= 0 || sh <= 0)
    return;
  int maxR = std::min(sw, sh) / 2;
  if (sr > maxR)
    sr = maxR;
  if (sr <= 0) {
    SDL_SetRenderDrawColor(m_r, color.r, color.g, color.b, color.a);
    SDL_Rect rect = {sx, sy, sw, sh};
    if (filled)
      SDL_RenderFillRect(m_r, &rect);
    else
      SDL_RenderDrawRect(m_r, &rect);
    return;
  }
  if (filled) {
    SDL_SetRenderDrawColor(m_r, color.r, color.g, color.b, color.a);
    SDL_Rect centerRect = {sx, sy + sr, sw, sh - 2 * sr};
    if (centerRect.h > 0)
      SDL_RenderFillRect(m_r, &centerRect);
    for (int dy = 0; dy < sr; ++dy) {
      int ry = sr - 1 - dy;
      int dx = static_cast<int>(std::sqrt(sr * sr - ry * ry));
      int lineW = sw - 2 * (sr - dx);
      int lineX = sx + sr - dx;
      if (lineW > 0) {
        SDL_Rect topSlice = {lineX, sy + dy, lineW, 1};
        SDL_RenderFillRect(m_r, &topSlice);
        SDL_Rect btmSlice = {lineX, sy + sh - 1 - dy, lineW, 1};
        SDL_RenderFillRect(m_r, &btmSlice);
      }
    }
  } else {
    drawRoundedBorder(x, y, w, h, radius, color, 1);
  }
}

void UiRenderer::drawRoundedBorder(int x, int y, int w, int h, int radius,
                                   SDL_Color color, int thickness) {
  if (!m_r)
    return;
  int sx = PlatformInfo::instance().scaleX(x);
  int sy = PlatformInfo::instance().scaleY(y);
  int sw = PlatformInfo::instance().scaleW(w);
  int sh = PlatformInfo::instance().scaleH(h);
  int sr = PlatformInfo::instance().scaleW(radius);
  int st = PlatformInfo::instance().scaleW(thickness);
  if (sw <= 0 || sh <= 0)
    return;
  int maxR = std::min(sw, sh) / 2;
  if (sr > maxR)
    sr = maxR;
  if (sr <= 0) {
    drawBorder(sx, sy, sw, sh, color, st);
    return;
  }
  SDL_SetRenderDrawColor(m_r, color.r, color.g, color.b, color.a);
  SDL_Rect topBar = {sx + sr, sy, sw - 2 * sr, st};
  SDL_Rect btmBar = {sx + sr, sy + sh - st, sw - 2 * sr, st};
  SDL_RenderFillRect(m_r, &topBar);
  SDL_RenderFillRect(m_r, &btmBar);
  SDL_Rect leftBar = {sx, sy + sr, st, sh - 2 * sr};
  SDL_Rect rightBar = {sx + sw - st, sy + sr, st, sh - 2 * sr};
  SDL_RenderFillRect(m_r, &leftBar);
  SDL_RenderFillRect(m_r, &rightBar);
  for (int dy = 0; dy < sr; ++dy) {
    int ry = sr - 1 - dy;
    int outerDx = static_cast<int>(std::sqrt(sr * sr - ry * ry));
    int innerR = std::max(0, sr - st);
    int innerDx = (ry < innerR)
                      ? static_cast<int>(std::sqrt(innerR * innerR - ry * ry))
                      : 0;
    int segW = std::max(st, outerDx - innerDx);
    SDL_Rect tl = {sx + sr - outerDx, sy + dy, segW, 1};
    SDL_RenderFillRect(m_r, &tl);
    SDL_Rect tr = {sx + sw - sr + outerDx - segW, sy + dy, segW, 1};
    SDL_RenderFillRect(m_r, &tr);
    SDL_Rect bl = {sx + sr - outerDx, sy + sh - 1 - dy, segW, 1};
    SDL_RenderFillRect(m_r, &bl);
    SDL_Rect br = {sx + sw - sr + outerDx - segW, sy + sh - 1 - dy, segW, 1};
    SDL_RenderFillRect(m_r, &br);
  }
}

void UiRenderer::drawBadge(int x, int y, int w, int h, const std::string &text,
                           SDL_Color bg, SDL_Color fg) {
  int rad = h / 2;
  drawRoundedRect(x, y, w, h, rad, bg, true);
  std::string btn, label = text;
  if (!text.empty() && text[0] == '[') {
    size_t end = text.find(']');
    if (end != std::string::npos && end >= 2 && end <= 9) {
      btn = text.substr(1, end - 1);
      label = text.substr(end + 1);
      while (!label.empty() && label[0] == ' ')
        label.erase(0, 1);
      std::string up = btn;
      for (char &c : up)
        c = (char)toupper((unsigned char)c);
      if (up == "OK")
        btn = "A";
      else if (up == "D-PAD" || up == "D-PAD]")
        btn = "DPAD";
      else
        btn = up;
    }
  }
  if (!btn.empty() && btn != "LEN" && m_fSmall) {
    int iconSize = h - 10;
    if (iconSize < 16)
      iconSize = 16;
    if (iconSize > 32)
      iconSize = 32;
    int lw = textWidth(label, m_fSmall);
    int gap = lw > 0 ? 6 : 0;
    int totalW = iconSize + gap + lw;
    int sx = x + (w - totalW) / 2;
    int centerY = y + h / 2;
    drawButtonIcon(btn, sx, centerY - iconSize / 2, iconSize);
    if (lw > 0) {
      int th = TTF_FontHeight(m_fSmall);
      drawText(label, sx + iconSize + gap, centerY - th / 2, fg, m_fSmall);
    }
    return;
  }
  int th = m_fSmall ? TTF_FontHeight(m_fSmall) : 16;
  drawText(text, x + w / 2, y + (h - th) / 2, fg, m_fSmall, true);
}

static void blitFit(SDL_Renderer *r, SDL_Texture *tex, int sx, int sy, int sw,
                    int sh) {
  int texW = 0, texH = 0;
  SDL_QueryTexture(tex, nullptr, nullptr, &texW, &texH);
  int drawW = sw, drawH = sh;
  if (texW > 0 && texH > 0) {
    float aspect = static_cast<float>(texW) / static_cast<float>(texH);
    if (aspect >= 1.0f) {
      drawW = std::min(sw, static_cast<int>(sh * aspect));
      drawH = static_cast<int>(drawW / aspect);
    } else {
      drawH = std::min(sh, static_cast<int>(sw / aspect));
      drawW = static_cast<int>(drawH * aspect);
    }
  }
  SDL_Rect dst = {sx + (sw - drawW) / 2, sy + (sh - drawH) / 2, drawW, drawH};
  SDL_RenderCopy(r, tex, nullptr, &dst);
}

void UiRenderer::drawIcon(const std::string &iconName, int x, int y, int w,
                          int h) {
  if (!m_r)
    return;
  std::string key = "sys/" + iconName;
  SDL_Texture *texture = m_images.get(key);
  if (!texture && !m_assetsDir.empty()) {
    std::string iconPath = m_assetsDir + "/icons/" + iconName + ".png";
    SDL_Surface *surface = IMG_Load(iconPath.c_str());
    if (surface) {
      texture = SDL_CreateTextureFromSurface(m_r, surface);
      SDL_FreeSurface(surface);
      if (texture) {
        int iw = 0, ih = 0;
        SDL_QueryTexture(texture, nullptr, nullptr, &iw, &ih);
        m_images.put(key, texture, iw, ih);
      }
    }
  }
  if (!texture) {
    drawRect(x, y, w, h, {60, 70, 85, 255}, true);
    return;
  }
  blitFit(m_r, texture, PlatformInfo::instance().scaleX(x),
          PlatformInfo::instance().scaleY(y),
          PlatformInfo::instance().scaleW(w),
          PlatformInfo::instance().scaleH(h));
}

void UiRenderer::drawPlayerIcon(const std::string &iconName, int x, int y,
                                int w, int h) {
  if (!m_r)
    return;
  std::string key = "player/" + iconName;
  SDL_Texture *texture = m_images.get(key);
  if (!texture && !m_assetsDir.empty()) {
    std::string iconPath = m_assetsDir + "/player_icons/" + iconName + ".png";
    SDL_Surface *surface = IMG_Load(iconPath.c_str());
    if (surface) {
      texture = SDL_CreateTextureFromSurface(m_r, surface);
      SDL_FreeSurface(surface);
      if (texture) {
        int iw = 0, ih = 0;
        SDL_QueryTexture(texture, nullptr, nullptr, &iw, &ih);
        m_images.put(key, texture, iw, ih);
      }
    }
  }
  if (!texture) {
    const char *fb = (iconName == "send") ? ">" : "((";
    drawText(fb, x, y, {255, 255, 255, 255}, m_fLarge, false);
    return;
  }
  SDL_SetTextureColorMod(texture, 255, 255, 255);
  blitFit(m_r, texture, PlatformInfo::instance().scaleX(x),
          PlatformInfo::instance().scaleY(y),
          PlatformInfo::instance().scaleW(w),
          PlatformInfo::instance().scaleH(h));
}

void UiRenderer::drawGridIcon(const std::string &iconFile, int x, int y, int w,
                              int h) {
  if (!m_r)
    return;
  std::string key = "grid/" + iconFile;
  SDL_Texture *texture = m_images.get(key);
  if (!texture && !m_assetsDir.empty()) {
    std::string iconPath = m_assetsDir + "/apps_icons/" + iconFile;
    SDL_Surface *surface = IMG_Load(iconPath.c_str());
    if (surface) {
      texture = SDL_CreateTextureFromSurface(m_r, surface);
      SDL_FreeSurface(surface);
      if (texture) {
        int iw = 0, ih = 0;
        SDL_QueryTexture(texture, nullptr, nullptr, &iw, &ih);
        m_images.put(key, texture, iw, ih);
      }
    }
  }
  if (!texture) {
    drawRoundedRect(x + 10, y + 10, w - 20, h - 20, UiTheme::RADIUS_CARD,
                    {60, 70, 85, 255}, true);
    return;
  }
  blitFit(m_r, texture, PlatformInfo::instance().scaleX(x),
          PlatformInfo::instance().scaleY(y),
          PlatformInfo::instance().scaleW(w),
          PlatformInfo::instance().scaleH(h));
}

void UiRenderer::drawButtonIcon(const std::string &button, int x, int y,
                                int size) {
  if (!m_r)
    return;
  static const std::unordered_map<std::string, std::string> kMap = {
      {"A", "a.png"},
      {"B", "b.png"},
      {"X", "x.png"},
      {"Y", "y.png"},
      {"L1", "l1.png"},
      {"R1", "r1.png"},
      {"L2", "l2.png"},
      {"R2", "r2.png"},
      {"START", "start_icon.png"},
      {"SELECT", "view.png"},
      {"MENU", "options.png"},
      {"HOME", "options.png"},
      {"VIEW", "view.png"},
      {"BACK", "back_icon.png"},
      {"DPAD", "dpad.png"},
      {"UP", "up.png"},
      {"DOWN", "down.png"},
      {"LEFT", "left.png"},
      {"RIGHT", "right.png"},
  };
  auto it = kMap.find(button);
  std::string iconFile = (it != kMap.end()) ? it->second : "";
  SDL_Texture *tex = nullptr;
  std::string key;
  if (!iconFile.empty() && !m_assetsDir.empty()) {
    key = "btn/" + iconFile;
    tex = m_images.get(key);
    if (!tex) {
      std::string iconPath = m_assetsDir + "/button_icons/" + iconFile;
      SDL_Surface *surf = IMG_Load(iconPath.c_str());
      if (surf) {
        tex = SDL_CreateTextureFromSurface(m_r, surf);
        SDL_FreeSurface(surf);
        if (tex) {
          int iw = 0, ih = 0;
          SDL_QueryTexture(tex, nullptr, nullptr, &iw, &ih);
          m_images.put(key, tex, iw, ih);
        }
      }
    }
  }
  if (tex) {
    int iw = 0, ih = 0;
    SDL_QueryTexture(tex, nullptr, nullptr, &iw, &ih);
    float scale = (float)size / std::max(iw, ih);
    int dw = (int)(iw * scale);
    int dh = (int)(ih * scale);
    SDL_Rect dst = {x + (size - dw) / 2, y + (size - dh) / 2, dw, dh};
    SDL_RenderCopy(m_r, tex, nullptr, &dst);
  } else {
    SDL_Color bg = {60, 60, 60, 255};
    if (button == "A")
      bg = {16, 150, 60, 255};
    else if (button == "B")
      bg = {200, 30, 30, 255};
    else if (button == "X")
      bg = {30, 90, 200, 255};
    else if (button == "Y")
      bg = {200, 180, 20, 255};
    drawRoundedRect(x, y, size, size, size / 4, bg, true);
    drawText(button, x + 4, y + 4, {255, 255, 255, 255}, m_fSmall);
  }
}

int UiRenderer::textHeight(TTF_Font *font) {
  if (!font)
    return 0;
  return TTF_FontHeight(font);
}

int UiRenderer::textWidth(const std::string &text, TTF_Font *font) {
  if (!font || text.empty())
    return 0;
  int w = 0, h = 0;
  if (TTF_SizeUTF8(font, text.c_str(), &w, &h) != 0)
    return 0;
  return w;
}

std::string UiRenderer::truncateToWidth(const std::string &text, TTF_Font *font,
                                        int maxPx) {
  if (!font || text.empty() || maxPx <= 0)
    return text;
  if (textWidth(text, font) <= maxPx)
    return text;
  if (maxPx - textWidth("...", font) <= 0)
    return "...";
  size_t end = text.size();
  while (end > 0) {
    size_t prev = end - 1;
    while (prev > 0 && (static_cast<unsigned char>(text[prev]) & 0xC0) == 0x80)
      prev--;
    std::string cand = text.substr(0, prev) + "...";
    if (textWidth(cand, font) <= maxPx)
      return cand;
    end = prev;
  }
  return "...";
}

int UiRenderer::pillWidth(const std::string &text, TTF_Font *font) {
  TTF_Font *f = font ? font : m_fSmall;
  int tw = textWidth(text, f);
  int w = tw + UiTheme::PILL_PAD_X * 2;
  if (w < UiTheme::PILL_MIN_W)
    w = UiTheme::PILL_MIN_W;
  if (w > UiTheme::PILL_MAX_W)
    w = UiTheme::PILL_MAX_W;
  return w;
}

void UiRenderer::drawPill(int x, int y, int w, int h, const std::string &text,
                          bool active, TTF_Font *font) {
  TTF_Font *f = font ? font : m_fSmall;
  int rad = h / 2;
  SDL_Color bg = active ? UiTheme::PILL_BG_ACTIVE : UiTheme::PILL_BG;
  drawRoundedRect(x, y, w, h, rad, bg, true);
  if (active)
    drawRoundedBorder(x, y, w, h, rad, UiTheme::FOCUS_GLOW, 1);
  else
    drawRoundedBorder(x, y, w, h, rad, UiTheme::PILL_BORDER, 1);
  if (!f)
    return;
  SDL_Color fg = active ? UiTheme::TEXT_MAIN : UiTheme::PILL_TEXT_DIM;
  std::string disp = truncateToWidth(text, f, w - UiTheme::PILL_PAD_X * 2);
  int th = TTF_FontHeight(f);
  drawText(disp, x + w / 2, y + (h - th) / 2, fg, f, true);
}

void UiRenderer::drawButton(int x, int y, int w, int h,
                            const std::string &label, bool focused,
                            bool danger) {
  if (w < UiTheme::BTN_MIN_W)
    w = UiTheme::BTN_MIN_W;
  SDL_Color bg;
  if (danger && focused)
    bg = UiTheme::ACCENT_RED;
  else if (focused)
    bg = UiTheme::FOCUS_BG;
  else
    bg = UiTheme::PILL_BG;
  drawRoundedRect(x, y, w, h, UiTheme::RADIUS_BTN, bg, true);
  if (focused)
    drawRoundedBorder(x, y, w, h, UiTheme::RADIUS_BTN, UiTheme::FOCUS_GLOW, 2);
  else
    drawRoundedBorder(x, y, w, h, UiTheme::RADIUS_BTN, UiTheme::PILL_BORDER, 1);
  if (!m_fSmall)
    return;
  std::string disp = truncateToWidth(label, m_fSmall, w - 24);
  int th = TTF_FontHeight(m_fSmall);
  drawText(disp, x + w / 2, y + (h - th) / 2, UiTheme::TEXT_MAIN, m_fSmall,
           true);
}

void UiRenderer::drawRow(int x, int y, int w, int h, bool focused, bool dim) {
  if (focused) {
    drawRoundedRect(x - 2, y - 2, w + 4, h + 4, UiTheme::RADIUS_ROW + 2,
                    UiTheme::FOCUS_GLOW, false);
    drawRoundedRect(x, y, w, h, UiTheme::RADIUS_ROW, UiTheme::FOCUS_BG, true);
  } else {
    SDL_Color bg = dim ? UiTheme::CARD_BG : UiTheme::CARD_SOLID;
    drawRoundedRect(x, y, w, h, UiTheme::RADIUS_ROW, bg, true);
    drawRoundedBorder(x, y, w, h, UiTheme::RADIUS_ROW, UiTheme::CARD_BORDER, 1);
  }
}

int UiRenderer::textYCentered(int y, int h, TTF_Font *font) {
  int th = font ? TTF_FontHeight(font) : 16;
  return y + (h - th) / 2;
}

void UiRenderer::drawRowMainSub(int x, int y, int h, const std::string &main,
                                TTF_Font *fMain, const std::string &sub,
                                TTF_Font *fSub, int maxW, int gap) {
  int thM = fMain ? TTF_FontHeight(fMain) : 0;
  int thS = (!sub.empty() && fSub) ? TTF_FontHeight(fSub) : 0;
  int blockH = thM + (thS > 0 ? gap + thS : 0);
  int ty = y + (h - blockH) / 2;
  std::string m = main;
  std::string s = sub;
  if (maxW > 0) {
    if (fMain)
      m = truncateToWidth(main, fMain, maxW);
    if (!sub.empty() && fSub)
      s = truncateToWidth(sub, fSub, maxW);
  }
  if (fMain)
    drawText(m, x, ty, UiTheme::TEXT_MAIN, fMain);
  if (thS > 0)
    drawText(s, x, ty + thM + gap, UiTheme::TEXT_SUB, fSub);
}

int UiRenderer::drawFooterHint(const std::string &button,
                               const std::string &label, int x, int barY,
                               int barH, SDL_Color color, TTF_Font *font,
                               int iconSize, int gap) {
  int centerY = barY + barH / 2;
  int iconY = centerY - iconSize / 2;
  drawButtonIcon(button, x, iconY, iconSize);
  int lx = x + iconSize + gap;
  int th = textHeight(font);
  int ty = centerY - th / 2;
  drawText(label, lx, ty, color, font);
  return lx + textWidth(label, font);
}

void UiRenderer::drawFooterHintsCentered(
    const std::vector<std::pair<std::string, std::string>> &hints, int barY,
    int barH, SDL_Color color, TTF_Font *font, int iconSize, int gap,
    int hintGap) {
  if (hints.empty() || !font)
    return;
  int totalW = 0;
  for (size_t i = 0; i < hints.size(); ++i) {
    totalW += iconSize + gap + textWidth(hints[i].second, font);
    if (i + 1 < hints.size())
      totalW += hintGap;
  }
  int x = (1024 - totalW) / 2;
  if (x < 8)
    x = 8;
  for (size_t i = 0; i < hints.size(); ++i) {
    x = drawFooterHint(hints[i].first, hints[i].second, x, barY, barH, color,
                       font, iconSize, gap);
    if (i + 1 < hints.size())
      x += hintGap;
  }
}

void UiRenderer::drawTextRight(const std::string &text, int rightX, int y,
                               SDL_Color color, TTF_Font *font) {
  if (!font || text.empty())
    return;
  drawText(text, rightX - textWidth(text, font), y, color, font);
}

void UiRenderer::drawBadgeDual(int x, int y, int w, int h,
                               const std::string &btn1,
                               const std::string &label1,
                               const std::string &btn2,
                               const std::string &label2, SDL_Color bg,
                               SDL_Color fg) {
  int rad = h / 2;
  drawRoundedRect(x, y, w, h, rad, bg, true);
  if (!m_fSmall)
    return;
  int iconSize = h - 12;
  if (iconSize < 16)
    iconSize = 16;
  if (iconSize > 32)
    iconSize = 32;
  int gap = 6;
  int sepW = textWidth("  -  ", m_fSmall);
  int totalW = iconSize + gap + textWidth(label1, m_fSmall) + sepW + iconSize +
               gap + textWidth(label2, m_fSmall);
  int sx = x + (w - totalW) / 2;
  int centerY = y + h / 2;
  int th = TTF_FontHeight(m_fSmall);
  drawButtonIcon(btn1, sx, centerY - iconSize / 2, iconSize);
  sx += iconSize + gap;
  if (!label1.empty()) {
    drawText(label1, sx, centerY - th / 2, fg, m_fSmall);
    sx += textWidth(label1, m_fSmall);
  }
  drawText("  -  ", sx, centerY - th / 2, fg, m_fSmall);
  sx += sepW;
  drawButtonIcon(btn2, sx, centerY - iconSize / 2, iconSize);
  sx += iconSize + gap;
  if (!label2.empty())
    drawText(label2, sx, centerY - th / 2, fg, m_fSmall);
}

static std::string normalizeBtn(const std::string &b) {
  std::string up = b;
  for (char &c : up)
    c = (char)toupper((unsigned char)c);
  if (up == "OK")
    return "A";
  if (up == "D-PAD")
    return "DPAD";
  if (up == "L")
    return "L1";
  if (up == "R")
    return "R1";
  return up;
}

void UiRenderer::drawInlineHintsCentered(const std::string &text, int centerX,
                                         int y, SDL_Color color, TTF_Font *font,
                                         int iconSize, int gap) {
  if (!font || text.empty())
    return;
  struct Seg {
    bool isBtn;
    std::string s;
  };
  std::vector<Seg> segs;
  size_t i = 0, n = text.size();
  std::string cur;
  while (i < n) {
    if (text[i] == '[') {
      size_t end = text.find(']', i + 1);
      if (end != std::string::npos && end - i >= 2 && end - i <= 10) {
        std::string inside = text.substr(i + 1, end - i - 1);
        bool ok = !inside.empty();
        for (char c : inside) {
          char u = (char)toupper((unsigned char)c);
          bool valid = ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') ||
                        c == '-' || c == '/');
          if (!valid) {
            ok = false;
            break;
          }
        }
        if (ok) {
          if (!cur.empty()) {
            segs.push_back({false, cur});
            cur.clear();
          }
          segs.push_back({true, normalizeBtn(inside)});
          i = end + 1;
          if (i < n && text[i] == ' ')
            i++;
          continue;
        }
      }
    }
    cur += text[i++];
  }
  if (!cur.empty())
    segs.push_back({false, cur});
  int th = textHeight(font);
  int totalW = 0;
  for (auto &s : segs) {
    if (s.isBtn)
      totalW += iconSize + gap;
    else
      totalW += textWidth(s.s, font);
  }
  int x = centerX - totalW / 2;
  for (auto &s : segs) {
    if (s.isBtn) {
      drawButtonIcon(s.s, x, y + (th - iconSize) / 2, iconSize);
      x += iconSize + gap;
    } else {
      drawText(s.s, x, y, color, font);
      x += textWidth(s.s, font);
    }
  }
}

void UiRenderer::drawAppBackground() {
  drawRect(0, 0, UiTheme::APP_W, UiTheme::APP_H, UiTheme::BG_APP, true);
}

void UiRenderer::drawCard(int x, int y, int w, int h) {
  drawRoundedRect(x, y, w, h, UiTheme::RADIUS_CARD, UiTheme::CARD_BG, true);
  drawRoundedBorder(x, y, w, h, UiTheme::RADIUS_CARD, UiTheme::CARD_BORDER, 1);
}

void UiRenderer::drawFocusRow(int x, int y, int w, int h) {
  drawRoundedRect(x - 2, y - 2, w + 4, h + 4, UiTheme::RADIUS_ROW + 2,
                  UiTheme::FOCUS_GLOW, false);
  drawRoundedRect(x, y, w, h, UiTheme::RADIUS_ROW, UiTheme::FOCUS_BG, true);
}

void UiRenderer::drawAppHeader(const std::string &title,
                               const std::string &sub) {
  if (!m_fLarge)
    return;
  drawText(title, 32, 22, UiTheme::TEXT_MAIN, m_fLarge);
  if (!sub.empty() && m_fSmall)
    drawText(sub, 34, 22 + TTF_FontHeight(m_fLarge) + 2, UiTheme::TEXT_DIM,
             m_fSmall);
}

void UiRenderer::drawPadIcon(UiTheme::PadBtn btn, int x, int y, int size) {
  using PB = UiTheme::PadBtn;
  if (btn == PB::L1R1) {
    int half = size / 2 - 1;
    drawButtonIcon("L1", x, y + (size - half) / 2, half);
    drawButtonIcon("R1", x + half + 2, y + (size - half) / 2, half);
    return;
  }
  if (btn == PB::UPDOWN) {
    int half = size / 2 - 1;
    drawButtonIcon("UP", x, y + (size - half) / 2, half);
    drawButtonIcon("DOWN", x + half + 2, y + (size - half) / 2, half);
    return;
  }
  const char *s = "A";
  switch (btn) {
  case PB::A:
    s = "A";
    break;
  case PB::B:
    s = "B";
    break;
  case PB::X:
    s = "X";
    break;
  case PB::Y:
    s = "Y";
    break;
  case PB::START:
    s = "START";
    break;
  case PB::SELECT:
    s = "SELECT";
    break;
  case PB::DPAD:
    s = "DPAD";
    break;
  case PB::L1:
    s = "L1";
    break;
  case PB::R1:
    s = "R1";
    break;
  case PB::AB:
    s = "A";
    break;
  default:
    s = "A";
    break;
  }
  drawButtonIcon(s, x, y, size);
  if (btn == PB::AB)
    drawButtonIcon("B", x + size / 2, y, size / 2);
}

void UiRenderer::drawAppFooter(const std::vector<UiTheme::FooterHint> &hints) {
  using PB = UiTheme::PadBtn;
  if (!m_fSmall || hints.empty())
    return;
  drawRect(0, UiTheme::FOOTER_Y, UiTheme::APP_W, UiTheme::FOOTER_H,
           UiTheme::FOOTER_BG, true);
  drawRect(0, UiTheme::FOOTER_Y, UiTheme::APP_W, 1, UiTheme::FOOTER_LINE, true);
  std::vector<std::pair<std::string, std::string>> legacy;
  legacy.reserve(hints.size());
  for (auto &h : hints) {
    const char *s = "A";
    switch (h.btn) {
    case PB::A:
      s = "A";
      break;
    case PB::B:
      s = "B";
      break;
    case PB::X:
      s = "X";
      break;
    case PB::Y:
      s = "Y";
      break;
    case PB::START:
      s = "START";
      break;
    case PB::SELECT:
      s = "SELECT";
      break;
    case PB::DPAD:
      s = "DPAD";
      break;
    case PB::L1:
      s = "L1";
      break;
    case PB::R1:
      s = "R1";
      break;
    default:
      s = "A";
      break;
    }
    legacy.emplace_back(s, std::string(h.label ? h.label : ""));
  }
  int iconSize = UiTheme::FOOTER_ICON, gap = UiTheme::FOOTER_GAP;
  int hintGap = UiTheme::FOOTER_HINT_GAP;
  int totalW = 0;
  for (size_t k = 0; k < legacy.size(); ++k) {
    totalW += iconSize + gap + textWidth(legacy[k].second, m_fSmall);
    if (k + 1 < legacy.size())
      totalW += hintGap;
  }
  if (totalW > 1008)
    hintGap = 20;
  totalW = 0;
  for (size_t k = 0; k < legacy.size(); ++k) {
    totalW += iconSize + gap + textWidth(legacy[k].second, m_fSmall);
    if (k + 1 < legacy.size())
      totalW += hintGap;
  }
  if (totalW > 1008 && iconSize > 22)
    iconSize = 22;
  drawFooterHintsCentered(legacy, UiTheme::FOOTER_Y, UiTheme::FOOTER_H,
                          UiTheme::TEXT_DIM, m_fSmall, iconSize, gap, hintGap);
}

void UiRenderer::drawVirtualKeyboard(const VkState &vk, int x, int y, int cellW,
                                     int cellH, int gapX, int gapY,
                                     SDL_Color accent, SDL_Color accentEdge,
                                     const char *actionLabels[5],
                                     bool withIcons, bool rounded,
                                     int actionStride) {
  if (!m_r)
    return;
  TTF_Font *fKey = m_fLarge ? m_fLarge : m_fMedium;
  TTF_Font *fAct = m_fMedium ? m_fMedium : m_fSmall;
  auto rectFill = [&](int cx, int cy, int w, int h, SDL_Color c) {
    if (rounded)
      drawRoundedRect(cx, cy, w, h, UiTheme::RADIUS_ROW, c, true);
    else
      drawRect(cx, cy, w, h, c, true);
  };
  auto rectEdge = [&](int cx, int cy, int w, int h, SDL_Color c, int t) {
    if (rounded)
      drawRoundedBorder(cx, cy, w, h, UiTheme::RADIUS_ROW, c, t);
    else
      drawBorder(cx, cy, w, h, c, t);
  };
  SDL_Color keyBg{14, 32, 44, 210}, keyEdge{22, 54, 70, 255};
  SDL_Color keyFg{210, 228, 238, 255}, selFg{255, 255, 255, 255};
  if (!withIcons) {
    keyBg = {15, 23, 42, 255};
    keyEdge = {30, 41, 59, 255};
    keyFg = {203, 213, 225, 255};
  }
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 10; c++) {
      int cx = x + c * (cellW + gapX);
      int cy = y + r * (cellH + gapY);
      bool sel = (vk.row == r && vk.col == c);
      VkState t = vk;
      t.row = r;
      t.col = c;
      char ch = VirtualKeyboard::charAt(t);
      char buf[2] = {ch ? ch : ' ', '\0'};
      if (sel) {
        rectFill(cx, cy, cellW, cellH, accent);
        rectEdge(cx, cy, cellW, cellH, accentEdge, 2);
        if (fKey)
          drawText(std::string(buf), cx + cellW / 2,
                   cy + (cellH - textHeight(fKey)) / 2, selFg, fKey, true);
      } else {
        rectFill(cx, cy, cellW, cellH, keyBg);
        rectEdge(cx, cy, cellW, cellH, keyEdge, 1);
        if (fKey)
          drawText(std::string(buf), cx + cellW / 2,
                   cy + (cellH - textHeight(fKey)) / 2, keyFg, fKey, true);
      }
    }
  }
  int totalW = 10 * cellW + 9 * gapX;
  int actW = (totalW - 4 * gapX) / 5;
  int actY = y + 4 * (cellH + gapY);
  static const char *actBtns[5] = {"L1", "R1", "X", "Y", "START"};
  for (int i = 0; i < 5; i++) {
    int cx = x + i * (actW + gapX);
    bool sel =
        (vk.row == 4) && (actionStride == 2 ? (vk.col / 2) == i : vk.col == i);
    std::string label =
        (actionLabels && actionLabels[i]) ? actionLabels[i] : "";
    if (withIcons) {
      int iconSz = 30, kgap = 8;
      int lw = (fAct && !label.empty()) ? textWidth(label, fAct) : 0;
      int totW = iconSz + kgap + lw;
      int kxc = cx + (actW - totW) / 2;
      int kyc = actY + (cellH - 30) / 2;
      int kth = fAct ? textHeight(fAct) : 0;
      if (sel) {
        rectFill(cx, actY, actW, cellH, accent);
        rectEdge(cx, actY, actW, cellH, accentEdge, 2);
        drawButtonIcon(actBtns[i], kxc, kyc, iconSz);
        if (fAct)
          drawText(label, kxc + iconSz + kgap, kyc + (30 - kth) / 2, selFg,
                   fAct);
      } else {
        rectFill(cx, actY, actW, cellH, keyBg);
        rectEdge(cx, actY, actW, cellH, keyEdge, 1);
        drawButtonIcon(actBtns[i], kxc, kyc, iconSz);
        if (fAct)
          drawText(label, kxc + iconSz + kgap, kyc + (30 - kth) / 2, keyFg,
                   fAct);
      }
    } else {
      SDL_Color bg = sel ? accent : keyBg;
      if (i == 3)
        bg = sel ? SDL_Color{22, 163, 74, 255} : SDL_Color{20, 83, 45, 255};
      if (i == 4)
        bg = sel ? SDL_Color{220, 38, 38, 255} : SDL_Color{69, 10, 10, 255};
      if (i == 0 && vk.shift && !sel)
        bg = SDL_Color{29, 78, 216, 255};
      rectFill(cx, actY, actW, cellH, bg);
      rectEdge(cx, actY, actW, cellH, sel ? accentEdge : keyEdge, sel ? 2 : 1);
      if (fAct)
        drawText(label, cx + actW / 2, actY + (cellH - textHeight(fAct)) / 2,
                 selFg, fAct, true);
    }
  }
}

void UiRenderer::beginModalDim() {
  drawRect(0, 0, UiTheme::APP_W, UiTheme::APP_H, UiTheme::DIM_OVERLAY, true);
}

} // namespace RomCloud
