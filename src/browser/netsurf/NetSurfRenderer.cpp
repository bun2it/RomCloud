#include "NetSurfRenderer.h"
#include "../../logging/Logger.h"
#include "../../network/HttpClient.h"
#include <algorithm>
#include <cmath>
#include <thread>

namespace RomCloud {

static const int SCREEN_W = 1024;
static const int CONTENT_Y = 112; // Strictly below URL bar (Y = 64..111)
static const int CONTENT_H = 603; // 715 - 112 = 603px visible height

NetSurfRenderer& NetSurfRenderer::instance() {
    static NetSurfRenderer inst;
    return inst;
}

NetSurfRenderer::~NetSurfRenderer() {
    reset();
}

void NetSurfRenderer::init(SDL_Renderer* renderer, TTF_Font* font,
                          const std::string& fontPath, int baseFontSize) {
    m_renderer = renderer;
    m_font = font;
    m_fontPath = fontPath;
    m_baseFontSize = baseFontSize > 0 ? baseFontSize : 24;
    clearTextCache();
    clearImageCache();
}

void NetSurfRenderer::reset() {
    clearTextCache();
    clearImageCache();
    for (auto& pair : m_fonts) {
        if (pair.second && pair.second != m_font) {
            TTF_CloseFont(pair.second);
        }
    }
    m_fonts.clear();
    m_root.reset();
    m_focusables.clear();
    m_focusedIndex = -1;
    m_scrollY = 0;
    m_maxScroll = 0;
    m_totalHeight = 0;
}

void NetSurfRenderer::clearTextCache() {
    for (auto& kv : m_textCache) {
        if (kv.second.tex) {
            SDL_DestroyTexture(kv.second.tex);
        }
    }
    m_textCache.clear();
}

void NetSurfRenderer::clearImageCache() {
    std::lock_guard<std::mutex> lock(m_imageMutex);
    for (auto& kv : m_textures) {
        if (kv.second.tex) {
            SDL_DestroyTexture(kv.second.tex);
        }
    }
    m_textures.clear();
    m_pendingImages.clear();

    for (auto& s : m_readySurfaces) {
        if (s.surface) {
            SDL_FreeSurface(s.surface);
        }
    }
    m_readySurfaces.clear();
}

TTF_Font* NetSurfRenderer::fontFor(int px) {
    if (px <= 0 || m_fontPath.empty()) return m_font;
    if (px < 12) px = 12;
    if (px > 48) px = 48;
    auto it = m_fonts.find(px);
    if (it != m_fonts.end()) return it->second ? it->second : m_font;
    TTF_Font* f = TTF_OpenFont(m_fontPath.c_str(), px);
    m_fonts[px] = f;
    return f ? f : m_font;
}

void NetSurfRenderer::setRenderTree(std::unique_ptr<RenderBox> root) {
    m_root = std::move(root);
    m_scrollY = 0;

    // Calculate total document height
    m_totalHeight = 0;
    if (m_root) {
        std::function<void(const RenderBox*)> scanHeight = [&](const RenderBox* b) {
            if (!b) return;
            int bottom = b->y + b->height;
            if (bottom > m_totalHeight) m_totalHeight = bottom;
            for (const auto& ch : b->children) {
                scanHeight(ch.get());
            }
        };
        scanHeight(m_root.get());
    }

    m_maxScroll = std::max(0, m_totalHeight - CONTENT_H);
    rebuildFocusables();

    if (!m_focusables.empty()) {
        m_focusedIndex = 0;
        m_focusables[0]->isFocused = true;
    } else {
        m_focusedIndex = -1;
    }
}

void NetSurfRenderer::rebuildFocusables() {
    m_focusables.clear();
    if (!m_root) return;

    std::function<void(RenderBox*)> collect = [&](RenderBox* b) {
        if (!b || b->phantom) return;
        b->isFocused = false;

        // Requirement 3: Only hyperlinks (with href) and input fields are focusable.
        // Plain text and non-hyperlink images must NEVER be focusable or highlighted.
        bool isHyperlink = !b->href.empty() && (b->type == RenderBoxType::LINK || b->type == RenderBoxType::IMAGE);
        bool isInputField = (b->type == RenderBoxType::INPUT || b->type == RenderBoxType::BUTTON);

        if ((isHyperlink || isInputField) && b->width > 0 && b->height > 0) {
            // Avoid duplicate focusable box if parent and child share same href
            bool duplicate = false;
            if (!m_focusables.empty() && !b->href.empty() && m_focusables.back()->href == b->href) {
                duplicate = true;
            }
            if (!duplicate) {
                m_focusables.push_back(b);
            }
        }

        for (auto& ch : b->children) {
            collect(ch.get());
        }
    };

    collect(m_root.get());
}

RenderBox* NetSurfRenderer::focusedBox() const {
    if (m_focusedIndex >= 0 && m_focusedIndex < (int)m_focusables.size()) {
        return m_focusables[m_focusedIndex];
    }
    return nullptr;
}

bool NetSurfRenderer::setFocusIndex(int index) {
    if (index < 0 || index >= (int)m_focusables.size()) return false;
    if (m_focusedIndex >= 0 && m_focusedIndex < (int)m_focusables.size()) {
        m_focusables[m_focusedIndex]->isFocused = false;
    }
    m_focusedIndex = index;
    m_focusables[index]->isFocused = true;
    ensureVisible(m_focusables[index]);
    return true;
}

void NetSurfRenderer::focusFirst() {
    if (!m_focusables.empty()) {
        setFocusIndex(0);
    }
}

void NetSurfRenderer::focusLast() {
    if (!m_focusables.empty()) {
        setFocusIndex((int)m_focusables.size() - 1);
    }
}

bool NetSurfRenderer::navigate(NavDirection dir) {
    if (m_focusables.empty()) return false;
    if (m_focusedIndex < 0 || m_focusedIndex >= (int)m_focusables.size()) {
        return setFocusIndex(0);
    }

    RenderBox* cur = m_focusables[m_focusedIndex];
    int curRight = cur->x + cur->width;
    int curBottom = cur->y + cur->height;

    int bestIdx = -1;
    float bestScore = 1e9f;

    for (size_t i = 0; i < m_focusables.size(); ++i) {
        if ((int)i == m_focusedIndex) continue;
        RenderBox* cand = m_focusables[i];
        int candRight = cand->x + cand->width;
        int candBottom = cand->y + cand->height;

        switch (dir) {
            case NavDirection::RIGHT: {
                if (cand->x >= cur->x + 10 || candRight > curRight + 15) {
                    int dx = std::max(0, cand->x - curRight);
                    int dy = 0;
                    if (candBottom >= cur->y && cand->y <= curBottom) {
                        dy = std::abs(cand->y - cur->y);
                    } else if (cand->y > curBottom) {
                        dy = cand->y - curBottom;
                    } else {
                        dy = cur->y - candBottom;
                    }
                    float score = (float)dx + 3.0f * (float)dy;
                    if (score < bestScore) {
                        bestScore = score;
                        bestIdx = (int)i;
                    }
                }
                break;
            }
            case NavDirection::LEFT: {
                if (candRight <= curRight - 10 || cand->x < cur->x - 15) {
                    int dx = std::max(0, cur->x - candRight);
                    int dy = 0;
                    if (candBottom >= cur->y && cand->y <= curBottom) {
                        dy = std::abs(cand->y - cur->y);
                    } else if (cand->y > curBottom) {
                        dy = cand->y - curBottom;
                    } else {
                        dy = cur->y - candBottom;
                    }
                    float score = (float)dx + 3.0f * (float)dy;
                    if (score < bestScore) {
                        bestScore = score;
                        bestIdx = (int)i;
                    }
                }
                break;
            }
            case NavDirection::DOWN: {
                if (cand->y >= cur->y + 15 && candBottom > curBottom) {
                    int dy = std::max(0, cand->y - curBottom);
                    int dx = 0;
                    if (candRight >= cur->x && cand->x <= curRight) {
                        dx = std::abs(cand->x - cur->x);
                    } else if (cand->x > curRight) {
                        dx = cand->x - curRight;
                    } else {
                        dx = cur->x - candRight;
                    }
                    float score = (float)dy + 3.0f * (float)dx;
                    if (score < bestScore) {
                        bestScore = score;
                        bestIdx = (int)i;
                    }
                }
                break;
            }
            case NavDirection::UP: {
                if (candBottom <= curBottom - 15 && cand->y < cur->y) {
                    int dy = std::max(0, cur->y - candBottom);
                    int dx = 0;
                    if (candRight >= cur->x && cand->x <= curRight) {
                        dx = std::abs(cand->x - cur->x);
                    } else if (cand->x > curRight) {
                        dx = cand->x - curRight;
                    } else {
                        dx = cur->x - candRight;
                    }
                    float score = (float)dy + 3.0f * (float)dx;
                    if (score < bestScore) {
                        bestScore = score;
                        bestIdx = (int)i;
                    }
                }
                break;
            }
        }
    }

    if (bestIdx >= 0) {
        return setFocusIndex(bestIdx);
    }
    return false;
}

void NetSurfRenderer::setScrollY(int sy) {
    m_scrollY = std::max(0, std::min(sy, m_maxScroll));
}

void NetSurfRenderer::scrollBy(int delta) {
    setScrollY(m_scrollY + delta);
}

void NetSurfRenderer::ensureVisible(const RenderBox* box) {
    if (!box) return;
    int sy = box->y - m_scrollY;
    if (sy < CONTENT_Y + 16) {
        setScrollY(box->y - (CONTENT_Y + 16));
    } else if (sy + box->height > CONTENT_Y + CONTENT_H - 16) {
        setScrollY(box->y + box->height - (CONTENT_Y + CONTENT_H - 16));
    }
}

void NetSurfRenderer::queueImage(const std::string& url) {
    if (url.empty()) return;

    {
        std::lock_guard<std::mutex> lock(m_imageMutex);
        if (m_textures.find(url) != m_textures.end() ||
            m_pendingImages.find(url) != m_pendingImages.end()) {
            return;
        }
        m_pendingImages.insert(url);
    }

    std::thread([this, url]() {
        std::vector<std::string> headers = {
            "User-Agent: Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Mobile Safari/537.36",
            "Accept: image/jpeg,image/png,image/*;q=0.8,*/*;q=0.5"
        };
        HttpResponse resp = HttpClient::instance().get(url, headers, 15);
        // Diagnostic: one line per image so debug.log tells network apart
        // from decode failures (e.g. WebP when the device build lacks it).
        auto logImg = [&](const std::string& why) {
            Logger::info("NetSurf IMG " + why + " " + url.substr(0, 110) +
                         " http=" + std::to_string(resp.statusCode) +
                         " bytes=" + std::to_string(resp.body.size()));
        };
        if (resp.success && resp.statusCode == 200 && !resp.body.empty()) {
            SDL_RWops* rw = SDL_RWFromConstMem(resp.body.data(), (int)resp.body.size());
            if (rw) {
                SDL_Surface* surf = IMG_Load_RW(rw, 1);
                if (surf) {
                    std::lock_guard<std::mutex> lock(m_imageMutex);
                    m_readySurfaces.push_back(DecodedSurface{url, surf, surf->w, surf->h});
                    logImg("ok");
                    return;
                }
                logImg("DECODE_FAIL");
            } else {
                logImg("RW_FAIL");
            }
        } else {
            logImg("FETCH_FAIL");
        }
        std::lock_guard<std::mutex> lock(m_imageMutex);
        m_pendingImages.erase(url);
    }).detach();
}

void NetSurfRenderer::pumpImages() {
    std::vector<DecodedSurface> ready;
    {
        std::lock_guard<std::mutex> lock(m_imageMutex);
        if (m_readySurfaces.empty()) return;
        ready.swap(m_readySurfaces);
    }

    for (auto& item : ready) {
        SDL_Texture* tex = nullptr;
        if (m_renderer && item.surface) {
            tex = SDL_CreateTextureFromSurface(m_renderer, item.surface);
        }
        {
            std::lock_guard<std::mutex> lock(m_imageMutex);
            m_textures[item.url] = ImageEntry{tex, item.w, item.h};
            m_pendingImages.erase(item.url);
        }
        if (item.surface) {
            SDL_FreeSurface(item.surface);
        }
    }
}

bool NetSurfRenderer::decodeImageForTest(const std::string& url, const std::vector<uint8_t>& bytes) {
    if (url.empty() || bytes.empty()) return false;
    SDL_RWops* rw = SDL_RWFromConstMem(bytes.data(), (int)bytes.size());
    if (!rw) return false;
    SDL_Surface* surf = IMG_Load_RW(rw, 1);
    if (!surf) return false;

    SDL_Texture* tex = nullptr;
    if (m_renderer) {
        tex = SDL_CreateTextureFromSurface(m_renderer, surf);
    }
    int w = surf->w;
    int h = surf->h;
    SDL_FreeSurface(surf);

    std::lock_guard<std::mutex> lock(m_imageMutex);
    m_textures[url] = ImageEntry{tex, w, h};
    m_pendingImages.erase(url);
    return true;
}

bool NetSurfRenderer::isImageLoaded(const std::string& url) const {
    return m_textures.find(url) != m_textures.end();
}

void NetSurfRenderer::drawText(const std::string& text, int x, int y, SDL_Color color, int fontSizePx) {
    if (!m_renderer || text.empty()) return;
    TTF_Font* f = fontFor(fontSizePx);
    if (!f) return;

    std::string key = text;
    key.push_back('\x01');
    key.push_back((char)color.r);
    key.push_back((char)color.g);
    key.push_back((char)color.b);
    key.push_back((char)color.a);
    key.push_back((char)(fontSizePx & 0xFF));

    auto it = m_textCache.find(key);
    if (it == m_textCache.end()) {
        if (m_textCache.size() > 512) {
            clearTextCache();
        }
        SDL_Surface* s = TTF_RenderUTF8_Blended(f, text.c_str(), color);
        if (!s) return;
        SDL_Texture* t = SDL_CreateTextureFromSurface(m_renderer, s);
        int tw = s->w;
        int th = s->h;
        SDL_FreeSurface(s);
        if (!t) return;

        it = m_textCache.emplace(std::move(key), CachedText{t, tw, th, ++m_texTick}).first;
    } else {
        it->second.lastUsed = ++m_texTick;
    }

    SDL_Rect dst = {x, y, it->second.w, it->second.h};
    SDL_RenderCopy(m_renderer, it->second.tex, nullptr, &dst);
}

void NetSurfRenderer::render() {
    pumpImages();
    if (!m_renderer) return;

    SDL_Rect contentClip = {0, CONTENT_Y, SCREEN_W, CONTENT_H};

    // Strictly clip rendering to content viewport (112..714)
    SDL_RenderSetClipRect(m_renderer, &contentClip);

    // Paint white canvas inside content viewport
    SDL_SetRenderDrawColor(m_renderer, 255, 255, 255, 255);
    SDL_RenderFillRect(m_renderer, &contentClip);

    if (m_root) {
        renderBox(m_root.get(), m_scrollY);
    }

    // Reset clip rect so UI header, URL bar, footer and modals draw unimpeded
    SDL_RenderSetClipRect(m_renderer, nullptr);
}

std::string NetSurfRenderer::fitText(const std::string& text, int maxW, int px, bool tail) {
    if (text.empty() || maxW <= 0) return "";
    TTF_Font* f = fontFor(px);
    if (!f) return text;
    int tw = 0;
    TTF_SizeUTF8(f, text.c_str(), &tw, nullptr);
    if (tw <= maxW) return text;
    auto popChar = [](std::string& s, bool fromFront) {
        if (s.empty()) return;
        if (!fromFront) {
            do { s.pop_back(); }
            while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80);
        } else {
            size_t i = 0;
            do { i++; }
            while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80);
            s.erase(0, i);
        }
    };
    std::string out = text;
    while (!out.empty()) {
        popChar(out, tail);
        TTF_SizeUTF8(f, out.c_str(), &tw, nullptr);
        if (tw <= maxW) break;
    }
    return out;
}

// P17: form controls for the FULL engine. Mirrors the LITE engine's widget
// look (white field, gray border, cyan when focused).
void NetSurfRenderer::drawFormControl(const RenderBox* box, int sy) {
    if (!m_renderer || !box || box->phantom) return;
    bool focused = box->isFocused;
    SDL_Color edge = focused ? SDL_Color{0, 180, 216, 255}
                             : SDL_Color{170, 170, 170, 255};
    int px = box->fontSize > 0 ? box->fontSize : 22;
    if (px < 16) px = 16;
    const std::string& t = box->inputType;

    // --- Buttons (<button>, <input type=submit|button>) ---
    if (box->type == RenderBoxType::BUTTON || t == "submit" ||
        t == "button" || t == "image") {
        SDL_Rect r = {box->x, sy, box->width, box->height};
        if (focused) SDL_SetRenderDrawColor(m_renderer, 200, 235, 250, 255);
        else SDL_SetRenderDrawColor(m_renderer, 235, 240, 245, 255);
        SDL_RenderFillRect(m_renderer, &r);
        SDL_SetRenderDrawColor(m_renderer, edge.r, edge.g, edge.b, 255);
        SDL_RenderDrawRect(m_renderer, &r);
        // <input type=submit value="..."> carries its label in value;
        // <button> labels render via TEXT children (section 4).
        if (box->type == RenderBoxType::INPUT && !box->value.empty() &&
            box->height >= 20) {
            int tw = 0;
            TTF_Font* f = fontFor(px);
            if (f) TTF_SizeUTF8(f, box->value.c_str(), &tw, nullptr);
            int tx = box->x + std::max(8, (box->width - tw) / 2);
            int ty = sy + std::max(0, (box->height - px) / 2);
            drawText(fitText(box->value, box->width - 16, px), tx, ty,
                     SDL_Color{30, 30, 30, 255}, px);
        }
        return;
    }

    // --- Checkbox / radio ---
    if (t == "checkbox" || t == "radio") {
        int s = std::min(box->width, box->height) - 8;
        if (s < 16) s = 16;
        if (s > 32) s = 32;
        int bx = box->x + 4;
        int by = sy + (box->height - s) / 2;
        SDL_Rect r = {bx, by, s, s};
        SDL_SetRenderDrawColor(m_renderer, 255, 255, 255, 255);
        SDL_RenderFillRect(m_renderer, &r);
        SDL_SetRenderDrawColor(m_renderer, edge.r, edge.g, edge.b, 255);
        SDL_RenderDrawRect(m_renderer, &r);
        if (box->checked) {
            SDL_SetRenderDrawColor(m_renderer, 0, 140, 200, 255);
            SDL_RenderDrawLine(m_renderer,
                               bx + s / 5, by + s / 2, bx + s * 2 / 5, by + s * 4 / 5);
            SDL_RenderDrawLine(m_renderer,
                               bx + s * 2 / 5, by + s * 4 / 5, bx + s * 4 / 5, by + s / 5);
        }
        return;
    }

    // --- Select: field chrome + current option + chevron ---
    if (t == "select") {
        SDL_Rect r = {box->x, sy, box->width, box->height};
        SDL_SetRenderDrawColor(m_renderer, 248, 248, 248, 255);
        SDL_RenderFillRect(m_renderer, &r);
        SDL_SetRenderDrawColor(m_renderer, edge.r, edge.g, edge.b, 255);
        SDL_RenderDrawRect(m_renderer, &r);
        int ty = sy + std::max(0, (box->height - px) / 2);
        if (!box->value.empty()) {
            drawText(fitText(box->value, box->width - 44, px), box->x + 10, ty,
                     SDL_Color{30, 30, 30, 255}, px);
        }
        int ax = box->x + box->width - 24;
        int ay = sy + box->height / 2;
        SDL_SetRenderDrawColor(m_renderer, 100, 100, 100, 255);
        SDL_RenderDrawLine(m_renderer, ax, ay - 5, ax + 7, ay + 4);
        SDL_RenderDrawLine(m_renderer, ax + 7, ay + 4, ax + 14, ay - 5);
        return;
    }

    // --- Text-like fields (text/password/textarea) ---
    SDL_Rect r = {box->x, sy, box->width, box->height};
    SDL_SetRenderDrawColor(m_renderer, 255, 255, 255, 255);
    SDL_RenderFillRect(m_renderer, &r);
    SDL_SetRenderDrawColor(m_renderer, edge.r, edge.g, edge.b, 255);
    SDL_RenderDrawRect(m_renderer, &r);
    std::string shown;
    SDL_Color ink = {30, 30, 30, 255};
    if (!box->value.empty()) {
        if (t == "password") {
            size_t nchars = 0;
            for (unsigned char c : box->value)
                if ((c & 0xC0) != 0x80) nchars++;
            shown.assign(nchars, '*');
        } else {
            shown = box->value;
        }
    } else if (!box->placeholder.empty()) {
        shown = box->placeholder;
        ink = SDL_Color{150, 150, 150, 255};
    }
    // While the VK is open, keep the tail visible + a cursor.
    bool tail = box->editing;
    if (box->editing) shown += "_";
    int ty = sy + std::max(0, (box->height - px) / 2);
    if (!shown.empty() && box->width > 20) {
        drawText(fitText(shown, box->width - 16, px, tail), box->x + 8, ty,
                 ink, px);
    }
}

void NetSurfRenderer::renderBox(const RenderBox* box, int scrollY) {
    if (!box || box->phantom) return;
    int sy = box->y - scrollY;

    bool inView = (sy + box->height >= CONTENT_Y && sy <= CONTENT_Y + CONTENT_H);

    if (inView) {
        // 1. Background (skip full white as canvas is already white)
        if ((box->backgroundColor & 0xFF) > 0 && box->backgroundColor != 0xFFFFFFFF) {
            Uint8 r = (box->backgroundColor >> 24) & 0xFF;
            Uint8 g = (box->backgroundColor >> 16) & 0xFF;
            Uint8 b = (box->backgroundColor >> 8) & 0xFF;
            Uint8 a = box->backgroundColor & 0xFF;
            SDL_Rect rBg = {box->x, sy, box->width, box->height};
            SDL_SetRenderDrawColor(m_renderer, r, g, b, a);
            SDL_RenderFillRect(m_renderer, &rBg);
        }

        // 2. Explicit Border
        if (box->borderWidth > 0 && (box->borderColor & 0xFF) > 0) {
            Uint8 r = (box->borderColor >> 24) & 0xFF;
            Uint8 g = (box->borderColor >> 16) & 0xFF;
            Uint8 b = (box->borderColor >> 8) & 0xFF;
            Uint8 a = box->borderColor & 0xFF;
            SDL_Rect rBorder = {box->x, sy, box->width, box->height};
            SDL_SetRenderDrawColor(m_renderer, r, g, b, a);
            SDL_RenderDrawRect(m_renderer, &rBorder);
        }

        // 3. Image rendering
        if (box->type == RenderBoxType::IMAGE) {
            SDL_Rect rImg = {box->x, sy, box->width, box->height};
            auto it = m_textures.find(box->src);
            if (it != m_textures.end() && it->second.tex) {
                SDL_RenderCopy(m_renderer, it->second.tex, nullptr, &rImg);
            } else {
                // Stylish image / video skeleton placeholder
                if (box->tagName == "video") {
                    SDL_SetRenderDrawColor(m_renderer, 24, 30, 40, 255);
                } else {
                    SDL_SetRenderDrawColor(m_renderer, 240, 243, 246, 255);
                }
                SDL_RenderFillRect(m_renderer, &rImg);
                SDL_SetRenderDrawColor(m_renderer, 205, 210, 218, 255);
                SDL_RenderDrawRect(m_renderer, &rImg);
                if (!box->src.empty()) {
                    const_cast<NetSurfRenderer*>(this)->queueImage(box->src);
                }
            }

            if (box->tagName == "video") {
                // Video indicator overlay: badge circle with white play triangle
                int cx = box->x + box->width / 2;
                int cy = sy + box->height / 2;
                int rad = std::min(28, std::min(box->width, box->height) / 4);
                if (rad > 12) {
                    SDL_Rect badge = {cx - rad, cy - rad, rad * 2, rad * 2};
                    SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 180);
                    SDL_RenderFillRect(m_renderer, &badge);
                    SDL_SetRenderDrawColor(m_renderer, 255, 255, 255, 220);
                    SDL_RenderDrawRect(m_renderer, &badge);
                    int triSize = rad / 2;
                    for (int o = 0; o <= triSize; o++) {
                        SDL_RenderDrawLine(m_renderer, cx - triSize / 2 + o, cy - o, cx - triSize / 2 + o, cy + o);
                    }
                }
            }
        }

        // 4. Multi-line text rendering
        if (!box->lines.empty()) {
            SDL_Color textColor;
            bool isHyperlink = !box->href.empty();
            if (box->isFocused && isHyperlink) {
                textColor = SDL_Color{0, 180, 216, 255}; // Hot cyan for focused hyperlink
            } else if (isHyperlink || box->type == RenderBoxType::LINK) {
                textColor = SDL_Color{30, 64, 175, 255}; // Dark blue for links (#1E40AF)
            } else if ((box->color & 0xFF) > 0 && box->color != 0xFFFFFFFF) {
                textColor = SDL_Color{
                    (Uint8)((box->color >> 24) & 0xFF),
                    (Uint8)((box->color >> 16) & 0xFF),
                    (Uint8)((box->color >> 8) & 0xFF),
                    (Uint8)(box->color & 0xFF)
                };
            } else {
                textColor = SDL_Color{34, 34, 34, 255}; // Dark gray default text (#222222)
            }

            for (size_t i = 0; i < box->lines.size(); ++i) {
                int lineY = sy + (int)i * box->lineHeight;
                if (lineY + box->lineHeight >= CONTENT_Y && lineY <= CONTENT_Y + CONTENT_H) {
                    drawText(box->lines[i], box->x, lineY, textColor, box->fontSize);
                }
            }
        }

        // 4b. P17: form controls (field chrome, values, checkboxes, buttons).
        // (BUTTON text labels render via their TEXT children in section 4.)
        if (box->type == RenderBoxType::INPUT || box->type == RenderBoxType::BUTTON) {
            drawFormControl(box, sy);
        }

        // 5. Focus outline highlight cue (Requirement 3: Only hyperlink and input field!)
        bool isHyperlink = !box->href.empty();
        bool isInputField = (box->type == RenderBoxType::INPUT || box->type == RenderBoxType::BUTTON);
        if (box->isFocused && (isHyperlink || isInputField)) {
            SDL_Rect rF1 = {box->x - 2, sy - 2, box->width + 4, box->height + 4};
            SDL_Rect rF2 = {box->x - 1, sy - 1, box->width + 2, box->height + 2};
            SDL_SetRenderDrawColor(m_renderer, 0, 180, 216, 255);
            SDL_RenderDrawRect(m_renderer, &rF1);
            SDL_RenderDrawRect(m_renderer, &rF2);
        }
    }

    // Recursively render children
    for (const auto& ch : box->children) {
        renderBox(ch.get(), scrollY);
    }
}

} // namespace RomCloud
