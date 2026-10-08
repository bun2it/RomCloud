#include "UiRenderer.h"

#include <SDL2/SDL_image.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include <ctime>
#include <string>

#include "../platform/PlatformInfo.h"

namespace RomCloud {

static void splitBadgePrefix(const std::string &text, std::string &btn,
                             std::string &label);

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

void UiRenderer::drawFlipDigits(const std::string& newText,
                                const std::string& oldText, float t01, int x,
                                int y, int w, int h, TTF_Font* font,
                                SDL_Color color) {
  if (!m_r || !font || newText.empty() || w <= 0 || h <= 0)
    return;
  auto texOf = [&](const std::string& text, int& tw, int& th) -> SDL_Texture* {
    char keyBuf[128];
    uint32_t colorInt =
        (color.r << 24) | (color.g << 16) | (color.b << 8) | color.a;
    std::snprintf(keyBuf, sizeof(keyBuf), "%p_%08x_", (void*)font, colorInt);
    std::string key = std::string(keyBuf) + text;
    auto it = m_textCache.find(key);
    if (it != m_textCache.end()) {
      tw = it->second.w;
      th = it->second.h;
      it->second.lastUsed = SDL_GetTicks();
      return it->second.texture;
    }
    SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!surface)
      return nullptr;
    SDL_Texture* t = SDL_CreateTextureFromSurface(m_r, surface);
    tw = surface->w;
    th = surface->h;
    SDL_FreeSurface(surface);
    if (!t)
      return nullptr;
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
    m_textCache[key] = {t, tw, th, SDL_GetTicks()};
    return t;
  };
  int twN = 0, thN = 0;
  SDL_Texture* texN = texOf(newText, twN, thN);
  if (!texN || twN <= 0 || thN <= 0)
    return;
  int sx = PlatformInfo::instance().scaleX(x);
  int sy = PlatformInfo::instance().scaleY(y);
  int sw = PlatformInfo::instance().scaleW(w);
  int sh = PlatformInfo::instance().scaleH(h);
  int dxN = sx + (sw - twN) / 2;
  int dyN = sy + (sh - thN) / 2;
  // Bản lề GIỮA THẺ như gluqlo/Fliqlo (không bám theo mực glyph:
  // bám mực khiến cánh chỉ cao vài px với font nhỏ → nhìn như không lật).
  int midY = sy + sh / 2;
  int twO = 0, thO = 0;
  SDL_Texture* texO = nullptr;
  bool anim = (t01 >= 0.0f && t01 < 1.0f && !oldText.empty() &&
               oldText != newText);
  int dxO = dxN, dyO = dyN;
  if (anim) {
    texO = texOf(oldText, twO, thO);
    if (!texO || twO <= 0 || thO <= 0) {
      anim = false;
    } else {
      dxO = sx + (sw - twO) / 2;
      dyO = sy + (sh - thO) / 2;
    }
  }
  // Nửa tĩnh trên của 1 texture (hàng trên bản lề), kẹp trong thẻ.
  auto drawTopHalf = [&](SDL_Texture* tex, int dx, int dy, int tw, int th) {
    int srcH = midY - dy; // số hàng texture nằm trên bản lề
    if (srcH <= 0 || th <= 0)
      return;
    if (srcH > th)
      srcH = th;
    SDL_Rect src = {0, 0, tw, srcH};
    SDL_Rect dst = {dx, dy, tw, srcH};
    SDL_Rect clip = {sx, sy, sw, midY - sy};
    SDL_RenderSetClipRect(m_r, &clip);
    SDL_RenderCopy(m_r, tex, &src, &dst);
    SDL_RenderSetClipRect(m_r, nullptr);
  };
  // Nửa tĩnh dưới của 1 texture (hàng dưới bản lề), kẹp trong thẻ.
  auto drawBottomHalf = [&](SDL_Texture* tex, int dx, int dy, int tw, int th) {
    int skip = midY - dy; // số hàng texture nằm trên bản lề -> bỏ qua
    if (skip < 0)
      skip = 0;
    if (skip >= th)
      return;
    SDL_Rect src = {0, skip, tw, th - skip};
    SDL_Rect dst = {dx, dy + skip, tw, th - skip};
    SDL_Rect clip = {sx, midY, sw, sy + sh - midY};
    SDL_RenderSetClipRect(m_r, &clip);
    SDL_RenderCopy(m_r, tex, &src, &dst);
    SDL_RenderSetClipRect(m_r, nullptr);
  };
  if (!anim) {
    SDL_Rect dst = {dxN, dyN, twN, thN};
    SDL_RenderCopy(m_r, texN, nullptr, &dst);
  } else {
    float t = t01 < 0.0f ? 0.0f : (t01 > 1.0f ? 1.0f : t01);
    if (t < 0.5f) {
      // Phase A — Gấp nửa trên xuống (gluqlo upperhalf):
      // đáy = số cũ nguyên (giữ chỗ), mặt = số mới tĩnh đè dưới cánh cũ,
      // cánh = NỬA TRÊN số cũ co cos(p*pi/2) về bản lề, tối dần.
      float p = t * 2.0f;
      drawBottomHalf(texO, dxO, dyO, twO, thO);
      drawTopHalf(texN, dxN, dyN, twN, thN);
      int fullH = midY - dyO;
      if (fullH > thO)
        fullH = thO;
      if (fullH > 0) {
        float scale = std::cos((float)M_PI * p / 2.0f);
        int dstH = (int)(fullH * scale);
        if (dstH > 0) {
          SDL_Rect src = {0, 0, twO, fullH};
          SDL_Rect dst = {dxO, midY - dstH, twO, dstH};
          SDL_Rect clip = {sx, sy, sw, midY - sy};
          Uint8 shade = (Uint8)(255 - 185 * p); // sáng -> tối
          SDL_RenderSetClipRect(m_r, &clip);
          SDL_SetTextureColorMod(texO, shade, shade, shade);
          SDL_RenderCopy(m_r, texO, &src, &dst);
          SDL_SetTextureColorMod(texO, 255, 255, 255);
          SDL_RenderSetClipRect(m_r, nullptr);
        }
      }
    } else {
      // Phase B — Mở nửa dưới ra: mặt = số mới tĩnh, đáy cũ lót dưới
      // (vùng chưa phủ vẫn thấy số cũ), cánh = NỬA DƯỚI số mới giãn
      // sin(p*pi/2) từ bản lề xuống, sáng dần.
      float p = (t - 0.5f) * 2.0f;
      drawTopHalf(texN, dxN, dyN, twN, thN);
      drawBottomHalf(texO, dxO, dyO, twO, thO);
      int skip = midY - dyN;
      if (skip < 0)
        skip = 0;
      int fullH = thN - skip;
      if (fullH > 0) {
        float eased = std::sin((float)M_PI * p / 2.0f);
        int dstH = (int)(fullH * eased);
        if (dstH > 0) {
          SDL_Rect src = {0, skip, twN, fullH};
          SDL_Rect dst = {dxN, midY, twN, dstH};
          SDL_Rect clip = {sx, midY, sw, sy + sh - midY};
          Uint8 shade = (Uint8)(70 + 185 * p); // tối -> sáng
          SDL_RenderSetClipRect(m_r, &clip);
          SDL_SetTextureColorMod(texN, shade, shade, shade);
          SDL_RenderCopy(m_r, texN, &src, &dst);
          SDL_SetTextureColorMod(texN, 255, 255, 255);
          SDL_RenderSetClipRect(m_r, nullptr);
        }
      }
    }
  }
  // Vạch chia split-flap giữa thẻ (đè lên số) như gluqlo, dày 6px.
  SDL_SetRenderDrawColor(m_r, 0, 0, 0, 255);
  SDL_Rect line1 = {sx, midY - 3, sw, 6};
  SDL_RenderFillRect(m_r, &line1);
  SDL_SetRenderDrawColor(m_r, 26, 26, 26, 255);
  SDL_Rect line2 = {sx, midY + 3, sw, 1};
  SDL_RenderFillRect(m_r, &line2);
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

void UiRenderer::drawRoundedTopBar(int x, int y, int w, int h, int radius,
                                   SDL_Color color) {
  if (!m_r)
    return;
  if (radius <= 0 || h <= radius * 2) {
    drawRect(x, y, w, h, color, true);
    return;
  }
  // Top strip follows the dialog's top corners: rect body below the curve
  // + rounded cap on top, so no square corners stick out.
  drawRect(x, y + radius, w, h - radius, color, true);
  drawRoundedRect(x, y, w, radius * 2, radius, color, true);
  // Clip the bottom half of the cap back to a straight edge.
  drawRect(x, y + radius, w, radius, color, true);
}

void UiRenderer::drawModalDialog(int x, int y, int w, int h, int radius,
                                 SDL_Color bodyBg, SDL_Color titleBg,
                                 int titleH) {
  if (titleH < 0)
    titleH = 0;
  if (titleH > h)
    titleH = h;
  drawRoundedRect(x, y, w, h, radius, bodyBg, true);
  if (titleH > 0)
    drawRoundedTopBar(x, y, w, titleH, radius, titleBg);
  drawRoundedBorder(x, y, w, h, radius, UiTheme::CARD_BORDER, 1);
}

void UiRenderer::drawBadge(int x, int y, int w, int h, const std::string &text,
                           SDL_Color bg, SDL_Color fg) {
  if (w <= 0)
    w = badgeWidth(text, h, m_fSmall);
  int rad = h / 2;
  drawRoundedRect(x, y, w, h, rad, bg, true);
  std::string btn, label;
  splitBadgePrefix(text, btn, label);
  if (!btn.empty() && btn != "LEN" && m_fSmall) {
    int iconSize = h - 12;
    if (iconSize < 16)
      iconSize = 16;
    if (iconSize > 28)
      iconSize = 28;
    int lw = textWidth(label, m_fSmall);
    int gap = lw > 0 ? 6 : 0;
    int totalW = iconSize + gap + lw;
    int sx = x + (w - totalW) / 2;
    int centerY = y + h / 2;
    drawButtonIcon(btn, sx, centerY - iconSize / 2, iconSize);
    if (lw > 0) {
      std::string disp = truncateToWidth(label, m_fSmall, w - 24);
      drawText(disp, sx + iconSize + gap, textYCentered(y, h, m_fSmall), fg, m_fSmall);
    }
    return;
  }
  int th = m_fSmall ? TTF_FontHeight(m_fSmall) : 16;
  std::string disp = m_fSmall ? truncateToWidth(text, m_fSmall, w - 24) : text;
  (void)th;
  drawText(disp, x + w / 2, textYCentered(y, h, m_fSmall), fg, m_fSmall, true);
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
  // Icon kép: 2 icon full-size cạnh nhau, cao == iconSize (~cao chữ).
  if (button == "L1R1" || button == "UPDOWN") {
    std::string a = (button == "L1R1") ? "L1" : "UP";
    std::string b = (button == "L1R1") ? "R1" : "DOWN";
    drawButtonIcon(a, x, y, size);
    drawButtonIcon(b, x + size + 2, y, size);
    return;
  }
  static const std::unordered_map<std::string, std::string> kMap = {
      {"A", "a.png"},
      {"B", "b.png"},
      {"X", "x.png"},
      {"Y", "y.png"},
      {"L1", "l1.png"},
      {"R1", "r1.png"},
      {"L2", "l2.png"},
      {"R2", "r2.png"},
      {"PLAY", "START.png"},
      {"START", "START.png"},
      {"SELECT", "SELECT.png"},
      {"MENU", "MENU.png"},
      {"HOME", "MENU.png"},
      {"VIEW", "SELECT.png"},
      {"BACK", "back_icon.png"},
      {"DPAD", "dpad.png"},
      {"UP", "up.png"},
      {"DOWN", "down.png"},
      {"LEFT", "left.png"},
      {"RIGHT", "right.png"},
      {"UPDOWN", "vertical.png"},
      {"L1R1", "horizontal.png"},
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
    SDL_Color bg = UiTheme::PILL_BG;
    if (button == "A")
      bg = UiTheme::ACCENT_GREEN;
    else if (button == "B")
      bg = UiTheme::ACCENT_RED;
    else if (button == "X")
      bg = UiTheme::ACCENT_BLUE;
    else if (button == "Y")
      bg = UiTheme::ACCENT_GOLD;
    drawRoundedRect(x, y, size, size, size / 4, bg, true);
    drawText(button, x + 4, textYCentered(y, size, m_fSmall), {255, 255, 255, 255}, m_fSmall);
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

// Tach prefix "[X] " cua badge -> (btn, label). Tra btn rong neu khong co.
static void splitBadgePrefix(const std::string &text, std::string &btn,
                             std::string &label) {
  btn.clear();
  label = text;
  if (text.empty() || text[0] != '[')
    return;
  size_t end = text.find(']');
  if (end == std::string::npos || end < 2 || end > 9)
    return;
  std::string raw = text.substr(1, end - 1);
  label = text.substr(end + 1);
  while (!label.empty() && label[0] == ' ')
    label.erase(0, 1);
  std::string up = raw;
  for (char &c : up)
    c = (char)toupper((unsigned char)c);
  if (up == "OK")
    btn = "A";
  else if (up == "D-PAD" || up == "D-PAD]")
    btn = "DPAD";
  else
    btn = up;
}

int UiRenderer::badgeWidth(const std::string &text, int h, TTF_Font *font) {
  TTF_Font *f = font ? font : m_fSmall;
  std::string btn, label;
  splitBadgePrefix(text, btn, label);
  int contentW = 0;
  if (!btn.empty() && btn != "LEN" && f) {
    int iconSize = h - 12;
    if (iconSize < 16)
      iconSize = 16;
    if (iconSize > 28)
      iconSize = 28;
    int lw = textWidth(label, f);
    contentW = iconSize + (lw > 0 ? 6 + lw : 0);
  } else {
    contentW = textWidth(text, f);
  }
  int w = contentW + 24;  // pad 12 moi ben
  if (w < 48)
    w = 48;
  return w;
}

int UiRenderer::buttonWidth(const std::string &label, TTF_Font *font) {
  TTF_Font *f = font ? font : m_fSmall;
  int w = textWidth(label, f) + 32;  // pad 16 moi ben + border
  if (w < UiTheme::BTN_MIN_W)
    w = UiTheme::BTN_MIN_W;
  return w;
}

int UiRenderer::badgeDualWidth(const std::string &label1,
                               const std::string &label2, int h) {
  TTF_Font *f = m_fSmall;
  int iconSize = h - 12;
  if (iconSize < 16)
    iconSize = 16;
  if (iconSize > 32)
    iconSize = 32;
  int total = iconSize + 6 + textWidth(label1, f) +
              textWidth("  -  ", f) + iconSize + 6 +
              textWidth(label2, f);
  return total + 24;
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
  drawText(disp, x + w / 2, textYCentered(y, h, f), fg, f, true);
}

void UiRenderer::drawButton(int x, int y, int w, int h,
                            const std::string &label, bool focused,
                            bool danger) {
  if (w <= 0)
    w = buttonWidth(label, m_fSmall);
  else if (w < UiTheme::BTN_MIN_W)
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
  drawText(disp, x + w / 2, textYCentered(y, h, m_fSmall), UiTheme::TEXT_MAIN, m_fSmall,
           true);
}

void UiRenderer::drawRow(int x, int y, int w, int h, bool focused, bool dim) {
  if (focused) {
    drawRoundedRect(x - 2, y - 2, w + 4, h + 4, UiTheme::RADIUS_ROW + 2,
                    UiTheme::FOCUS_GLOW, false);
    SDL_BlendMode prev;
    SDL_GetRenderDrawBlendMode(m_r, &prev);
    SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
    drawRoundedRect(x, y, w, h, UiTheme::RADIUS_ROW, UiTheme::FOCUS_BG_SOFT, true);
    SDL_SetRenderDrawBlendMode(m_r, prev);
  } else {
    SDL_Color bg = dim ? UiTheme::CARD_BG : UiTheme::CARD_SOLID;
    drawRoundedRect(x, y, w, h, UiTheme::RADIUS_ROW, bg, true);
    drawRoundedBorder(x, y, w, h, UiTheme::RADIUS_ROW, UiTheme::CARD_BORDER, 1);
  }
}

int UiRenderer::textYCentered(int y, int h, TTF_Font *font) {
  if (!font) return y + h / 2 - 8;
  // Can giua quang hoc theo khoi glyph (ascent+|descent|) thay vi FontHeight
  // (FontHeight cong them lineGap -> chia doi lech len 1-2px, lo ro voi dau TV).
  int lineH = TTF_FontAscent(font) - TTF_FontDescent(font);
  if (lineH <= 0) lineH = TTF_FontHeight(font);
  return y + (h - lineH) / 2;
}

void UiRenderer::drawRowMainSub(int x, int y, int h, const std::string &main,
                                TTF_Font *fMain, const std::string &sub,
                                TTF_Font *fSub, int maxW, int gap) {
  int thM = fMain ? TTF_FontHeight(fMain) : 0;
  int thS = (!sub.empty() && fSub) ? TTF_FontHeight(fSub) : 0;
  int blockH = thM + (thS > 0 ? gap + thS : 0);
  if (blockH > h) {
    // Chu lon hon row: uu tien ten file, giam gap -> khong de hang duoi
    blockH = h;
    if (thS > 0 && thM + gap + thS > h)
      gap = 0;
  }
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

int UiRenderer::buttonIconWidth(const std::string& button, int size) {
  if (button == "L1R1" || button == "UPDOWN") return 2 * size + 2;
  return size;
}

int UiRenderer::drawFooterHint(const std::string &button,
                               const std::string &label, int x, int barY,
                               int barH, SDL_Color color, TTF_Font *font,
                               int iconSize, int gap) {
  int centerY = barY + barH / 2;
  int iw = buttonIconWidth(button, iconSize);
  int iconY = centerY - iconSize / 2;
  drawButtonIcon(button, x, iconY, iconSize);
  int lx = x + iw + gap;
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
    totalW += buttonIconWidth(hints[i].first, iconSize) + gap + textWidth(hints[i].second, font);
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
  if (w <= 0)
    w = badgeDualWidth(label1, label2, h);
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
      totalW += buttonIconWidth(s.s, iconSize) + gap;
    else
      totalW += textWidth(s.s, font);
  }
  int x = centerX - totalW / 2;
  for (auto &s : segs) {
    if (s.isBtn) {
      drawButtonIcon(s.s, x, y + (th - iconSize) / 2, iconSize);
      x += buttonIconWidth(s.s, iconSize) + gap;
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
  // Fill chọn trong suốt 25% (rule highlight) — bật blend cục bộ.
  SDL_BlendMode prev;
  SDL_GetRenderDrawBlendMode(m_r, &prev);
  SDL_SetRenderDrawBlendMode(m_r, SDL_BLENDMODE_BLEND);
  drawRoundedRect(x, y, w, h, UiTheme::RADIUS_ROW, UiTheme::FOCUS_BG_SOFT, true);
  SDL_SetRenderDrawBlendMode(m_r, prev);
}

void UiRenderer::drawHeaderStatus() {
  if (!m_r)
    return;
  // Pin: Brick dung axp2202-battery, fallback battery
  int pct = -1;
  bool charging = false;
  const char *capPaths[] = {
      "/sys/class/power_supply/axp2202-battery/capacity",
      "/sys/class/power_supply/battery/capacity",
  };
  for (auto p : capPaths) {
    FILE *f = fopen(p, "r");
    if (f) {
      int v = -1;
      if (fscanf(f, "%d", &v) == 1 && v >= 0 && v <= 100)
        pct = v;
      fclose(f);
      if (pct >= 0)
        break;
    }
  }
  const char *stPaths[] = {
      "/sys/class/power_supply/axp2202-battery/status",
      "/sys/class/power_supply/battery/status",
  };
  for (auto p : stPaths) {
    FILE *f = fopen(p, "r");
    if (f) {
      char s[32] = {0};
      if (fgets(s, sizeof(s), f)) {
        if (std::string(s).find("Charg") != std::string::npos)
          charging = true;
      }
      fclose(f);
      break;
    }
  }
  if (pct < 0)
    pct = 100;

  // Wifi: doc signal dBm qua `iw wlan0 link`, map 5 nac stock
  // >-50:5, >-60:4, >-67:3, >-75:2, connected:1, mat mang:off
  // Cache 5s de khong spawn process moi frame
  static uint32_t lastWifiCheck = 0;
  static bool cachedLinked = false;
  static int cachedLevel = 0;
  uint32_t ticks = SDL_GetTicks();
  bool linked = cachedLinked;
  int level = cachedLevel;
  if (ticks - lastWifiCheck > 5000 || lastWifiCheck == 0) {
    lastWifiCheck = ticks;
    linked = false;
    level = 0;
    FILE *pf = popen("iw wlan0 link 2>/dev/null | grep -i signal | head -1", "r");
    if (pf) {
      char line[128] = {0};
      if (fgets(line, sizeof(line), pf)) {
        int dbm = 0;
        if (sscanf(line, "%*[^0-9-]%d", &dbm) == 1) {
          linked = true;
          if (dbm >= -50)
            level = 5;
          else if (dbm >= -60)
            level = 4;
          else if (dbm >= -67)
            level = 3;
          else if (dbm >= -75)
            level = 2;
          else
            level = 1;
        }
      }
      pclose(pf);
    }
    if (!linked) {
      // fallback: operstate up = co wifi nhung khong do duoc dBm
      FILE *of = fopen("/sys/class/net/wlan0/operstate", "r");
      if (of) {
        char s[16] = {0};
        if (fgets(s, sizeof(s), of) && std::string(s).find("up") != std::string::npos) {
          linked = true;
          level = 3;
        }
        fclose(of);
      }
    }
    cachedLinked = linked;
    cachedLevel = level;
  }

  // Gio he thong HH:MM
  char tbuf[16] = "12:00";
  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  if (t)
    strftime(tbuf, sizeof(tbuf), "%H:%M", t);

  // Layout phai -> trai, can giua doc trong HEADER_H=64
  int cy = UiTheme::HEADER_H / 2;
  int x = UiTheme::APP_W - 24;
  SDL_Color fg = {190, 215, 228, 255};

  // Gio
  if (m_fSmall) {
    int tw = textWidth(tbuf, m_fSmall);
    drawText(tbuf, x - tw, textYCentered(0, UiTheme::HEADER_H, m_fSmall), fg, m_fSmall);
    x -= tw + 14;
  }
  // Icon pin stock theo muc + % ben trai
  std::string battIcon = "hdr_batt_100";
  if (pct <= 5)
    battIcon = "hdr_batt_0";
  else if (pct <= 30)
    battIcon = "hdr_batt_25";
  else if (pct <= 60)
    battIcon = "hdr_batt_50";
  else if (pct <= 85)
    battIcon = "hdr_batt_75";
  int bw = 30, bh = 30;
  drawIcon(battIcon, x - bw, cy - bh / 2, bw, bh);
  x -= bw + 6;
  if (charging)
    drawIcon("hdr_charging", x - 14, cy - 14, 14, 28), x -= 14 + 6;
  if (m_fSmall) {
    std::string pctStr = std::to_string(pct) + "%";
    int tw = textWidth(pctStr, m_fSmall);
    drawText(pctStr, x - tw, textYCentered(0, UiTheme::HEADER_H, m_fSmall), fg, m_fSmall);
    x -= tw + 14;
  }
  // Icon wifi stock
  std::string wifiIcon = "hdr_wifi_off";
  if (linked)
    wifiIcon = "hdr_wifi_" + std::to_string(level < 1 ? 1 : level);
  drawIcon(wifiIcon, x - 28, cy - 14, 28, 28);
}

void UiRenderer::drawAppHeader(const std::string &title,
                               const std::string &sub) {
  drawRect(0, 0, UiTheme::APP_W, UiTheme::HEADER_H, UiTheme::FOOTER_BG, true);
  drawRect(0, UiTheme::HEADER_H - 1, UiTheme::APP_W, 1, UiTheme::FOOTER_LINE,
           true);
  if (!m_fLarge)
    return;
  // Chua ~280px phai cho cum status [wifi][% pin][gio]
  const int statusReserve = 300;
  if (sub.empty()) {
    std::string t = truncateToWidth(title, m_fLarge, UiTheme::APP_W - 24 - statusReserve);
    drawText(t, 24, textYCentered(0, UiTheme::HEADER_H, m_fLarge),
             UiTheme::ACCENT_CYAN, m_fLarge);
  } else {
    int titleH = TTF_FontAscent(m_fLarge) - TTF_FontDescent(m_fLarge);
    int subH = (m_fSmall ? (TTF_FontAscent(m_fSmall) - TTF_FontDescent(m_fSmall)) : 14);
    if (titleH <= 0) titleH = TTF_FontHeight(m_fLarge);
    if (subH <= 0) subH = 14;
    const int gap = 4;
    int block = titleH + gap + subH;
    int startY = (UiTheme::HEADER_H - block) / 2;
    std::string t2 = truncateToWidth(title, m_fLarge, UiTheme::APP_W - 24 - statusReserve);
    drawText(t2, 24, startY, UiTheme::ACCENT_CYAN, m_fLarge);
    if (!sub.empty() && m_fSmall)
      drawText(truncateToWidth(sub, m_fSmall, UiTheme::APP_W - 24 - statusReserve),
               24, startY + titleH + gap, UiTheme::TEXT_DIM, m_fSmall);
  }
  drawHeaderStatus();
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
  case PB::MENU:
    s = "MENU";
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
  case PB::L2:
    s = "L2";
    break;
  case PB::R2:
    s = "R2";
    break;
  case PB::PLAY:
    s = "PLAY";
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
    case PB::MENU:
      s = "MENU";
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
    case PB::L2:
      s = "L2";
      break;
    case PB::R2:
      s = "R2";
      break;
    case PB::L1R1:
      s = "L1R1";
      break;
    case PB::UPDOWN:
      s = "UPDOWN";
      break;
    case PB::PLAY:
      s = "PLAY";
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
    totalW += buttonIconWidth(legacy[k].first, iconSize) + gap + textWidth(legacy[k].second, m_fSmall);
    if (k + 1 < legacy.size())
      totalW += hintGap;
  }
  if (totalW > 1008)
    hintGap = 20;
  totalW = 0;
  for (size_t k = 0; k < legacy.size(); ++k) {
    totalW += buttonIconWidth(legacy[k].first, iconSize) + gap + textWidth(legacy[k].second, m_fSmall);
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

  // Load stock keyboard textures if available
  std::string stockDir = m_assetsDir + "/stock_keyboard";
  std::string sysDir = "/usr/trimui/res/skin";
  auto loadKeyTex = [&](const std::string &fn) -> SDL_Texture * {
    std::string p = stockDir + "/" + fn;
    SDL_Texture *t = getOrLoadImage("skb/" + fn, p);
    if (!t)
      t = getOrLoadImage("sys/" + fn, sysDir + "/" + fn);
    return t;
  };
  SDL_Texture *texBtn01Sel = loadKeyTex("bg-button-01-selected.png");
  SDL_Texture *texBtn01Unsel = loadKeyTex("bg-button-01-unselect.png");
  SDL_Texture *texBtn02Sel = loadKeyTex("bg-button-02-selected.png");
  SDL_Texture *texBtn02Unsel = loadKeyTex("bg-button-02-unselect.png");
  SDL_Texture *texTipL = loadKeyTex("button-tips-L.png");
  SDL_Texture *texTipR = loadKeyTex("button-tips-R.png");
  SDL_Texture *texTipX = loadKeyTex("button-tips-X.png");
  SDL_Texture *texTipY = loadKeyTex("button-tips-Y.png");
  SDL_Texture *texTipStart = loadKeyTex("button-tips-START.png");
  SDL_Texture *texShift = loadKeyTex("ic-shift.png");
  SDL_Texture *texShiftAct = loadKeyTex("ic-shift-active.png");
  SDL_Texture *texSpace = loadKeyTex("ic-space.png");
  SDL_Texture *texDelete = loadKeyTex("ic-delete.png");
  SDL_Texture *texOK = loadKeyTex("ic-OK.png");

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
        if (texBtn01Sel) {
          SDL_Rect dst = {cx, cy, cellW, cellH};
          SDL_RenderCopy(m_r, texBtn01Sel, nullptr, &dst);
        } else {
          rectFill(cx, cy, cellW, cellH, accent);
          rectEdge(cx, cy, cellW, cellH, accentEdge, 2);
        }
        if (fKey)
          drawText(std::string(buf), cx + cellW / 2,
                   cy + (cellH - textHeight(fKey)) / 2, selFg, fKey, true);
      } else {
        if (texBtn01Unsel) {
          SDL_Rect dst = {cx, cy, cellW, cellH};
          SDL_RenderCopy(m_r, texBtn01Unsel, nullptr, &dst);
        } else {
          rectFill(cx, cy, cellW, cellH, keyBg);
          rectEdge(cx, cy, cellW, cellH, keyEdge, 1);
        }
        if (fKey)
          drawText(std::string(buf), cx + cellW / 2,
                   cy + (cellH - textHeight(fKey)) / 2, keyFg, fKey, true);
      }
    }
  }

  int totalW = 10 * cellW + 9 * gapX;
  int actW = (totalW - 4 * gapX) / 5;
  int actY = y + 4 * (cellH + gapY);
  SDL_Texture *actTips[5] = {texTipL, texTipR, texTipX, texTipY, texTipStart};

  for (int i = 0; i < 5; i++) {
    int cx = x + i * (actW + gapX);
    bool sel =
        (vk.row == 4) && (actionStride == 2 ? (vk.col / 2) == i : vk.col == i);
    std::string label =
        (actionLabels && actionLabels[i]) ? actionLabels[i] : "";

    if (texBtn02Sel && texBtn02Unsel) {
      SDL_Rect dst = {cx, actY, actW, cellH};
      SDL_RenderCopy(m_r, sel ? texBtn02Sel : texBtn02Unsel, nullptr, &dst);
    } else {
      rectFill(cx, actY, actW, cellH, sel ? accent : keyBg);
      rectEdge(cx, actY, actW, cellH, sel ? accentEdge : keyEdge, sel ? 2 : 1);
    }

    if (withIcons) {
      SDL_Texture *tipTex = actTips[i];
      int tipW = 0, tipH = 0;
      if (tipTex) {
        int tw = 0, th = 0;
        SDL_QueryTexture(tipTex, nullptr, nullptr, &tw, &th);
        if (th > 0) {
          tipH = std::min(cellH - 14, 26);
          tipW = (tw * tipH) / th;
        }
      }

      bool showSubIcon = (actW >= 90);

      if (i == 0) {
        // L1 / Shift
        SDL_Texture *shTex = showSubIcon
            ? (vk.shift ? (texShiftAct ? texShiftAct : texShift) : texShift)
            : nullptr;
        int shW = 24, shH = 24;
        std::string lbl = vk.shift ? "ABC" : "abc";
        int lw = fAct ? textWidth(lbl, fAct) : 0;
        int contentW = (tipW ? tipW + 6 : 0) + (shTex ? shW + 6 : 0) + lw;
        int curX = cx + (actW - contentW) / 2;
        if (tipTex && tipW > 0) {
          SDL_Rect td = {curX, actY + (cellH - tipH) / 2, tipW, tipH};
          SDL_RenderCopy(m_r, tipTex, nullptr, &td);
          curX += tipW + 6;
        }
        if (shTex) {
          SDL_Rect sd = {curX, actY + (cellH - shH) / 2, shW, shH};
          SDL_RenderCopy(m_r, shTex, nullptr, &sd);
          curX += shW + 6;
        }
        if (fAct) {
          SDL_Color tc =
              sel ? selFg
                  : (vk.shift ? SDL_Color{0, 180, 255, 255} : keyFg);
          drawText(lbl, curX, textYCentered(actY, cellH, fAct), tc, fAct);
        }
      } else if (i == 1) {
        // R1 / ABC-123 (stock): doi bo chu <-> so & ky tu dac biet
        std::string lbl = vk.symbolMode ? "123" : "ABC";
        int lw = fAct ? textWidth(lbl, fAct) : 0;
        int contentW = (tipW ? tipW + 8 : 0) + lw;
        int curX = cx + (actW - contentW) / 2;
        if (tipTex && tipW > 0) {
          SDL_Rect td = {curX, actY + (cellH - tipH) / 2, tipW, tipH};
          SDL_RenderCopy(m_r, tipTex, nullptr, &td);
          curX += tipW + 8;
        }
        if (fAct) {
          SDL_Color c = vk.symbolMode ? SDL_Color{0, 180, 255, 255}
                                      : SDL_Color{148, 163, 184, 255};
          if (sel)
            c = selFg;
          drawText(lbl, curX, textYCentered(actY, cellH, fAct), c, fAct);
        }
      } else if (i == 2) {
        // X / Cách
        SDL_Texture *spTex = showSubIcon ? texSpace : nullptr;
        int spW = 24, spH = 24;
        std::string lbl = (actionLabels && actionLabels[i]) ? actionLabels[i] : "Cách";
        int lw = fAct ? textWidth(lbl, fAct) : 0;
        int contentW = (tipW ? tipW + 6 : 0) + (spTex ? spW + 6 : 0) + lw;
        int curX = cx + (actW - contentW) / 2;
        if (tipTex && tipW > 0) {
          SDL_Rect td = {curX, actY + (cellH - tipH) / 2, tipW, tipH};
          SDL_RenderCopy(m_r, tipTex, nullptr, &td);
          curX += tipW + 6;
        }
        if (spTex) {
          SDL_Rect sd = {curX, actY + (cellH - spH) / 2, spW, spH};
          SDL_RenderCopy(m_r, spTex, nullptr, &sd);
          curX += spW + 6;
        }
        if (fAct) {
          drawText(lbl, curX, textYCentered(actY, cellH, fAct),
                   sel ? selFg : keyFg, fAct);
        }
      } else if (i == 3) {
        // Y / Xóa
        SDL_Texture *delTex = showSubIcon ? texDelete : nullptr;
        int delW = 24, delH = 24;
        std::string lbl = (actionLabels && actionLabels[i]) ? actionLabels[i] : "Xóa";
        int lw = fAct ? textWidth(lbl, fAct) : 0;
        int contentW = (tipW ? tipW + 6 : 0) + (delTex ? delW + 6 : 0) + lw;
        int curX = cx + (actW - contentW) / 2;
        if (tipTex && tipW > 0) {
          SDL_Rect td = {curX, actY + (cellH - tipH) / 2, tipW, tipH};
          SDL_RenderCopy(m_r, tipTex, nullptr, &td);
          curX += tipW + 6;
        }
        if (delTex) {
          SDL_Rect sd = {curX, actY + (cellH - delH) / 2, delW, delH};
          SDL_RenderCopy(m_r, delTex, nullptr, &sd);
          curX += delW + 6;
        }
        if (fAct) {
          drawText(lbl, curX, textYCentered(actY, cellH, fAct),
                   sel ? selFg : keyFg, fAct);
        }
      } else if (i == 4) {
        // START / Tìm / Xong
        SDL_Texture *okTex = showSubIcon ? texOK : nullptr;
        int okW = 34, okH = 20;
        std::string lbl = (actionLabels && actionLabels[i]) ? actionLabels[i] : "Tìm";
        int lw = fAct ? textWidth(lbl, fAct) : 0;
        int contentW = (tipW ? tipW + 6 : 0) + (okTex ? okW + 6 : 0) + lw;
        int curX = cx + (actW - contentW) / 2;
        if (tipTex && tipW > 0) {
          SDL_Rect td = {curX, actY + (cellH - tipH) / 2, tipW, tipH};
          SDL_RenderCopy(m_r, tipTex, nullptr, &td);
          curX += tipW + 6;
        }
        if (okTex) {
          SDL_Rect sd = {curX, actY + (cellH - okH) / 2, okW, okH};
          SDL_RenderCopy(m_r, okTex, nullptr, &sd);
          curX += okW + 6;
        }
        if (fAct) {
          drawText(lbl, curX, textYCentered(actY, cellH, fAct),
                   sel ? selFg : SDL_Color{0, 180, 255, 255}, fAct);
        }
      }
    } else {
      if (fAct)
        drawText(label, cx + actW / 2, actY + (cellH - textHeight(fAct)) / 2,
                 sel ? selFg : keyFg, fAct, true);
    }
  }
}

void UiRenderer::beginModalDim() {
  drawRect(0, 0, UiTheme::APP_W, UiTheme::APP_H, UiTheme::DIM_OVERLAY, true);
}

} // namespace RomCloud
