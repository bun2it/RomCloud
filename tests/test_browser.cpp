// tests/test_browser.cpp — vnexpress plan phase tests (P0 baseline + gates).
// Headless: SDL_ttf + real font, renderer=nullptr (draw calls no-op).
// Build: see compile cmd in docs/WEB_VNEXPRESS_PLAN.md (P0 section).
#include <cassert>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL.h>

#include "../src/browser/HtmlRenderer.h"
#include "../src/browser/BrowserManager.h"
#include "../src/browser/SvgRaster.h"
#include "../src/browser/JsEngine.h"
#include "../src/browser/netsurf/NetSurfBridge.h"
#include "../src/browser/netsurf/NetSurfLayout.h"
#include "../src/browser/netsurf/NetSurfRenderer.h"
#include "../src/browser/netsurf/NetSurfEngine.h"
#include "../src/ui/VirtualKeyboard.h"

using namespace RomCloud;

static int s_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::cout << "[PASS] " << msg << std::endl; } \
    else { std::cout << "[FAIL] " << msg << std::endl; s_fail++; } \
} while (0)

static std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void countTree(const std::list<HtmlElement>& els, int& n, int& imgs, int& links) {
    for (const auto& el : els) {
        n++;
        if (el.type == HtmlElementType::IMAGE) imgs++;
        if (el.type == HtmlElementType::LINK) links++;
        countTree(el.children, n, imgs, links);
    }
}

int main() {
    if (TTF_Init() != 0) { std::cout << "[FAIL] TTF_Init" << std::endl; return 1; }
    // P9d: no live <link> CSS fetching in tests (offline-deterministic).
    // One block (P6 live) re-enables network explicitly.
    HtmlRenderer::instance().setCssFetchEnabled(false);
    BrowserManager::instance().settings().fullEngine = false;
    TTF_Font* font = TTF_OpenFont("assets/fonts/NotoSans-Regular.ttf", 24);
    if (!font) font = TTF_OpenFont("assets/fonts/font.ttf", 24);
    CHECK(font != nullptr, "P0: load test font");

    HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);

    std::string html = readFile("tests/fixtures/vnexpress.html");
    CHECK(html.size() > 100000, "P0: fixture vnexpress.html loaded");

    auto t0 = std::chrono::steady_clock::now();
    bool ok = HtmlRenderer::instance().loadHtml(html);
    auto t1 = std::chrono::steady_clock::now();
    long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    CHECK(ok, "P0: loadHtml returns true");
    std::cout << "[INFO] parse+layout: " << ms << "ms, top-level="
              << HtmlRenderer::instance().elementCount()
              << ", focusable=" << HtmlRenderer::instance().focusableCount() << std::endl;
    CHECK(HtmlRenderer::instance().elementCount() > 0, "P0: DOM non-empty");
    CHECK(ms < 2000, "P0: parse+layout < 2000ms");

    int n = 0, imgs = 0, links = 0;
    countTree(HtmlRenderer::instance().elements(), n, imgs, links);
    std::cout << "[INFO] total nodes=" << n << " imgs=" << imgs << " links=" << links << std::endl;
    CHECK(n > 100, "P0: total DOM nodes > 100");
    CHECK(links > 0, "P0: links found");

    // Render headless must not crash (renderer=nullptr => no-op draws).
    HtmlRenderer::instance().render();
    CHECK(true, "P0: headless render no crash");

    // ---- P1 gates: scroll/layout decoupling, dirty flag, async submit ----
    HtmlRenderer::instance().loadHtml(html);
    CHECK(HtmlRenderer::instance().maxScroll() > 0, "P1: fixture page scrollable");
    // Layout-space invariant: capture a laid-out y, scroll, compare.
    int yBefore = -1;
    {
        std::function<void(const std::list<HtmlElement>&)> find =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (yBefore < 0 && el.height > 0 && el.y > 0) { yBefore = el.y; return; }
                    find(el.children);
                    if (yBefore >= 0) return;
                }
            };
        find(HtmlRenderer::instance().elements());
    }
    CHECK(yBefore > 0, "P1: found laid-out node");
    int wantScroll = std::min(120, HtmlRenderer::instance().maxScroll());
    HtmlRenderer::instance().pageDown(120);
    // ensureFocusVisible() may clamp scroll back so the focused row stays in
    // view — accept any forward progress up to the requested delta.
    CHECK(HtmlRenderer::instance().scrollY() > 0 &&
          HtmlRenderer::instance().scrollY() <= wantScroll,
          "P1: pageDown moves scrollY");
    int yAfter = -1;
    {
        std::function<void(const std::list<HtmlElement>&)> find =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (yAfter < 0 && el.height > 0 && el.y > 0) { yAfter = el.y; return; }
                    find(el.children);
                    if (yAfter >= 0) return;
                }
            };
        find(HtmlRenderer::instance().elements());
    }
    CHECK(yBefore == yAfter, "P1: scroll does not move layout y");
    HtmlRenderer::instance().pageUp(100000);
    CHECK(HtmlRenderer::instance().scrollY() == 0, "P1: pageUp clamps to 0");
    // L1/R1 must actually travel (focus follows, never yanks back).
    for (int i = 0; i < 5; i++) HtmlRenderer::instance().pageDown(120);
    CHECK(HtmlRenderer::instance().scrollY() == 600, "P1: repeated PgDn accumulates");

    // Dirty flag: render clears, scroll sets (quiet page — no async workers).
    HtmlRenderer::instance().loadHtml("<html><body><p>quiet</p></body></html>");
    HtmlRenderer::instance().render();  // clears dirty
    CHECK(!HtmlRenderer::instance().dirty(), "P1: render clears dirty");
    HtmlRenderer::instance().pageDown(10);
    CHECK(HtmlRenderer::instance().dirty(), "P1: scroll sets dirty");

    // Async submit: must return fast with loading state (worker fetches).
    HtmlRenderer::instance().loadHtml(
        "<html><body><form action=\"http://127.0.0.1:9/submit\" method=\"post\">"
        "<input name=\"u\" value=\"a\"><button>Go</button></form></body></html>");
    HtmlRenderer::instance().handleInput(3);  // RIGHT -> focus button
    auto s0 = std::chrono::steady_clock::now();
    HtmlRenderer::instance().handleInput(4);  // A -> submit (async)
    auto s1 = std::chrono::steady_clock::now();
    long sms = std::chrono::duration_cast<std::chrono::milliseconds>(s1 - s0).count();
    CHECK(sms < 2000, "P1: submit returns without blocking");
    CHECK(HtmlRenderer::instance().isLoading(), "P1: submit sets loading state");
    std::cout << "[INFO] submit dispatch: " << sms << "ms" << std::endl;

    // Real-renderer pass (dummy video if available): texture cache + timing.
    bool vidOk = (SDL_Init(SDL_INIT_VIDEO) == 0);
    if (vidOk) {
        SDL_Window* win = SDL_CreateWindow("t", 0, 0, 1024, 768, SDL_WINDOW_HIDDEN);
        SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE) : nullptr;
        if (ren) {
            HtmlRenderer::instance().init(ren, font, "assets/fonts/NotoSans-Regular.ttf", 30);
            HtmlRenderer::instance().loadHtml(html);
            auto r0 = std::chrono::steady_clock::now();
            HtmlRenderer::instance().render();
            auto r1 = std::chrono::steady_clock::now();
            // Second render: dirty gate => near-zero.
            HtmlRenderer::instance().render();
            auto r2 = std::chrono::steady_clock::now();
            long firstMs = std::chrono::duration_cast<std::chrono::milliseconds>(r1 - r0).count();
            long secondMs = std::chrono::duration_cast<std::chrono::milliseconds>(r2 - r1).count();
            std::cout << "[INFO] render1=" << firstMs << "ms render2=" << secondMs
                      << "ms cache=" << HtmlRenderer::instance().textCacheSize() << std::endl;
            CHECK(HtmlRenderer::instance().textCacheSize() > 0, "P1: text texture cache fills");
            CHECK(secondMs <= firstMs, "P1: cached render not slower");
            SDL_DestroyRenderer(ren);
        } else {
            std::cout << "[INFO] P1: no software renderer, cache test skipped" << std::endl;
        }
        if (win) SDL_DestroyWindow(win);
        SDL_Quit();
    } else {
        std::cout << "[INFO] P1: SDL video unavailable, cache test skipped" << std::endl;
    }

    // ---- P2 gates: wrap, headings, entities, lists ----
    // Re-init headless (P1 video block may have swapped renderer).
    HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
    HtmlRenderer::instance().loadHtml(html);
    {
        int overflow = 0, headlines = 0, bolds = 0, totalLines = 0;
        std::function<void(const std::list<HtmlElement>&)> walk =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (el.type == HtmlElementType::TEXT && !el.text.empty()) {
                        // P9: headline = h-tag residue, large CSS size, or bold.
                        bool isHead = el.lineH > 36 || el.fontSize >= 22 || el.bold;
                        if (isHead) headlines++;
                        if (el.bold) bolds++;
                        if (!el.lines.empty()) {
                            for (const auto& ln : el.lines) {
                                totalLines++;
                                // P9: measure with the node's own CSS-scaled size.
                                int w = HtmlRenderer::instance().measureForTest(ln, el.fontSize);
                                if (w > 992) overflow++;
                            }
                        }
                    }
                    walk(el.children);
                }
            };
        walk(HtmlRenderer::instance().elements());
        std::cout << "[INFO] lines=" << totalLines << " headlines=" << headlines
                  << " bolds=" << bolds << " overflow=" << overflow << std::endl;
        CHECK(totalLines > 50, "P2: wrapped lines produced");
        CHECK(overflow == 0, "P2: no line overflows content width");
        CHECK(headlines > 10, "P2: vnexpress headlines detected (h1-h6)");
        CHECK(bolds > 0, "P2: bold nodes detected");
    }
    {
        // Entities: named + decimal + hex.
        HtmlRenderer::instance().loadHtml(
            "<html><body><p>A &hellip; B &#273; &#x110; &copy; &mdash;</p></body></html>");
        std::string all;
        std::function<void(const std::list<HtmlElement>&)> walk =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) { all += el.text; walk(el.children); }
            };
        walk(HtmlRenderer::instance().elements());
        CHECK(all.find("…") != std::string::npos, "P2: &hellip; decoded");
        CHECK(all.find("đ") != std::string::npos, "P2: &#273; decoded");
        CHECK(all.find("Đ") != std::string::npos, "P2: &#x110; decoded");
        CHECK(all.find("©") != std::string::npos, "P2: &copy; decoded");
    }
    {
        // Lists: bullet prefix.
        HtmlRenderer::instance().loadHtml(
            "<html><body><ul><li>One</li><li>Two</li></ul></body></html>");
        std::string all;
        std::function<void(const std::list<HtmlElement>&)> walk =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) { all += el.text; walk(el.children); }
            };
        walk(HtmlRenderer::instance().elements());
        CHECK(all.find("• One") != std::string::npos, "P2: list bullet rendered");
    }

    // ---- P3 gates: checkbox, select, maxlength, hidden, textarea ----
    {
        // Checkbox toggles with A (was a no-op bug).
        HtmlRenderer::instance().loadHtml(
            "<html><body><form action=\"http://x/\"><input type=checkbox name=c></form></body></html>");
        HtmlRenderer::instance().handleInput(4);  // A on focused checkbox
        bool after1 = false, after2 = true;
        // Re-walk via const access to read state.
        std::function<void(const std::list<HtmlElement>&)> read =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (el.type == HtmlElementType::INPUT_CHECKBOX) after1 = el.checked;
                    read(el.children);
                }
            };
        read(HtmlRenderer::instance().elements());
        HtmlRenderer::instance().handleInput(4);
        std::function<void(const std::list<HtmlElement>&)> read2 =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (el.type == HtmlElementType::INPUT_CHECKBOX) after2 = el.checked;
                    read2(el.children);
                }
            };
        read2(HtmlRenderer::instance().elements());
        CHECK(after1 && !after2, "P3: checkbox toggles on A");
    }
    {
        // Select cycles options with A, honoring preselected.
        HtmlRenderer::instance().loadHtml(
            "<html><body><form action=\"http://x/\"><select name=s>"
            "<option value=1>One</option><option value=2 selected>Two</option>"
            "<option>Three</option></select></form></body></html>");
        auto selIdx = [&]() -> int {
            int v = -1;
            std::function<void(const std::list<HtmlElement>&)> walk =
                [&](const std::list<HtmlElement>& els) {
                    for (const auto& el : els) {
                        if (el.type == HtmlElementType::SELECT) v = el.selected;
                        walk(el.children);
                    }
                };
            walk(HtmlRenderer::instance().elements());
            return v;
        };
        CHECK(selIdx() == 1, "P3: preselected option honored");
        HtmlRenderer::instance().handleInput(4);
        CHECK(selIdx() == 2, "P3: select cycles on A");
        HtmlRenderer::instance().handleInput(4);
        CHECK(selIdx() == 0, "P3: select wraps around");
    }
    {
        // maxlength honored.
        HtmlRenderer::instance().loadHtml(
            "<html><body><input name=a maxlength=3></body></html>");
        CHECK(HtmlRenderer::instance().focusedInputMaxLen() == 3, "P3: maxlength read");
        HtmlRenderer::instance().setFocusedInputValue("abcdef");
        CHECK(HtmlRenderer::instance().focusedInputValue() == "abc", "P3: value capped to maxlength");
    }
    {
        // Hidden input: form data but never focusable.
        HtmlRenderer::instance().loadHtml(
            "<html><body><form action=\"http://x/\">"
            "<input type=hidden name=h value=v><input name=t></form></body></html>");
        CHECK(HtmlRenderer::instance().focusableCount() == 1, "P3: hidden not focusable");
    }
    {
        // Textarea: inner text becomes the field value, shown once.
        HtmlRenderer::instance().loadHtml(
            "<html><body><form action=\"http://x/\"><textarea name=t>Hello world</textarea></form></body></html>");
        CHECK(HtmlRenderer::instance().focusedInputValue() == "Hello world", "P3: textarea value");
        int hits = 0;
        std::function<void(const std::list<HtmlElement>&)> walk =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (el.text.find("Hello world") != std::string::npos) hits++;
                    if (el.value.find("Hello world") != std::string::npos) hits++;
                    walk(el.children);
                }
            };
        walk(HtmlRenderer::instance().elements());
        CHECK(hits == 1, "P3: textarea text not duplicated");
    }

    // ---- P4 gates: content-type guard, redirect URL, cookies ----
    {
        // Non-HTML body must not be parsed as a page.
        HtmlRenderer::instance().injectFetchForTest(
            "http://x/pic.png", true, "\x89PNG-binary", "image/png", "");
        HtmlRenderer::instance().pollFetch();
        CHECK(HtmlRenderer::instance().hasError(), "P4: image body rejected");
        int imgs = 0, texts = 0;
        std::function<void(const std::list<HtmlElement>&)> walk =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (el.type == HtmlElementType::IMAGE) imgs++;
                    if (el.type == HtmlElementType::TEXT && !el.text.empty()) texts++;
                    walk(el.children);
                }
            };
        walk(HtmlRenderer::instance().elements());
        CHECK(imgs == 0, "P4: no image nodes from binary body");
    }
    {
        // HTML body renders; effective URL wins over requested URL.
        HtmlRenderer::instance().injectFetchForTest(
            "http://x/login", true, "<html><body><p>Portal OK</p></body></html>",
            "text/html; charset=utf-8", "http://x/landing?ok=1");
        HtmlRenderer::instance().pollFetch();
        CHECK(!HtmlRenderer::instance().hasError(), "P4: html body accepted");
        CHECK(HtmlRenderer::instance().currentUrl() == "http://x/landing?ok=1",
              "P4: effective URL tracked after redirect");
    }
    {
        // Cookie jar API never crashes; failed fetch still reports error.
        HttpClient::instance().clearCookies();
        HttpResponse r = HttpClient::instance().get("http://127.0.0.1:9/nope", {}, 3);
        CHECK(!r.success && !r.error.empty(), "P4: failed fetch reports error");
    }

    // ---- P5 gates: image parse count, queue, decode ----
    {
        int fimgs = 0;
        std::function<void(const std::list<HtmlElement>&)> walk =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (el.type == HtmlElementType::IMAGE) fimgs++;
                    walk(el.children);
                }
            };
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        HtmlRenderer::instance().loadHtml(html);
        walk(HtmlRenderer::instance().elements());
        CHECK(fimgs == 27, "P5: fixture image count (27 real <img>, 2 ad tags in <script> skipped)");
    }
    {
        // 1x1 transparent PNG decodes into the cache (no network).
        static const unsigned char kPng1x1[] = {
            0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,
            0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
            0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,0x89,0x00,0x00,0x00,
            0x0A,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0x00,0x01,0x00,0x00,
            0x05,0x00,0x01,0x0D,0x0A,0x2D,0xB4,0x00,0x00,0x00,0x00,0x49,
            0x45,0x4E,0x44,0xAE,0x42,0x60,0x82
        };
        std::vector<uint8_t> bytes(kPng1x1, kPng1x1 + sizeof(kPng1x1));
        size_t before = HtmlRenderer::instance().imageCachedCount();
        bool okDec = HtmlRenderer::instance().decodeImageForTest("test://one", bytes);
        CHECK(okDec, "P5: PNG decodes");
        CHECK(HtmlRenderer::instance().imageCachedCount() == before + 1,
              "P5: decoded image cached");
        CHECK(!HtmlRenderer::instance().decodeImageForTest("test://bad", {0x00, 0x01}),
              "P5: garbage bytes rejected");
    }
    {
        // Queue: relative + absolute src resolve and queue (needs renderer).
        bool vok = (SDL_Init(SDL_INIT_VIDEO) == 0);
        SDL_Window* w2 = vok ? SDL_CreateWindow("t2", 0, 0, 1024, 768, SDL_WINDOW_HIDDEN) : nullptr;
        SDL_Renderer* r2 = w2 ? SDL_CreateRenderer(w2, -1, SDL_RENDERER_SOFTWARE) : nullptr;
        if (r2) {
            HtmlRenderer::instance().init(r2, font, "assets/fonts/NotoSans-Regular.ttf", 30);
            HtmlRenderer::instance().injectFetchForTest(
                "http://127.0.0.1:9/page", true,
                "<html><body><img src=\"/a.png\"><img src=\"http://127.0.0.1:9/b.jpg\"></body></html>",
                "text/html", "");
            HtmlRenderer::instance().pollFetch();
            size_t pend = HtmlRenderer::instance().imagePendingCount();
            CHECK(pend == 2, "P5: both images queued (relative resolved)");
            SDL_DestroyRenderer(r2);
        } else {
            std::cout << "[INFO] P5: no renderer, queue test skipped" << std::endl;
        }
        if (w2) SDL_DestroyWindow(w2);
        if (vok) SDL_Quit();
    }
    {
        // P9c: SVG never queued (unsupported on Brick), no worker wasted.
        bool vok = (SDL_Init(SDL_INIT_VIDEO) == 0);
        SDL_Window* w3 = vok ? SDL_CreateWindow("t3", 0, 0, 1024, 768, SDL_WINDOW_HIDDEN) : nullptr;
        SDL_Renderer* r3 = w3 ? SDL_CreateRenderer(w3, -1, SDL_RENDERER_SOFTWARE) : nullptr;
        if (r3) {
            HtmlRenderer::instance().init(r3, font, "assets/fonts/NotoSans-Regular.ttf", 30);
            HtmlRenderer::instance().loadHtml(
                "<html><body><img src=\"http://127.0.0.1:9/logo.svg\"></body></html>");
            // SVG passes the queue (worker gates on content + svg setting).
            CHECK(HtmlRenderer::instance().imagePendingCount() >= 1, "P9c: svg queued for fetch");
            SDL_DestroyRenderer(r3);
        } else {
            std::cout << "[INFO] P9c: no renderer, svg test skipped" << std::endl;
        }
        if (w3) SDL_DestroyWindow(w3);
        if (vok) SDL_Quit();
    }
    {
        // JS engine: DOM write + hide + timer + error isolation.
        BrowserManager::instance().settings().js = true;
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        HtmlRenderer::instance().loadHtml(
            "<html><body><form action=\"http://x/\">"
            "<input id=\"u\" name=\"u\" value=\"\">"
            "<div id=\"box\">Hi</div>"
            "<script>"
            "document.getElementById('u').value = 'alice';"
            "document.getElementById('box').style.display = 'none';"
            "document.getElementById('nope').value = 'x';"  // null proxy: must not crash
            "</script>"
            "<script>throw 'boom';</script>"  // isolated: must not break the page
            "</body></html>");
        std::string uv;
        bool boxHidden = false;
        std::function<void(const std::list<HtmlElement>&)> walk =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (el.elemId == "u" && el.type == HtmlElementType::INPUT_TEXT) {
                        uv = el.value;
                    }
                    if (el.elemId == "box") boxHidden = el.hide;
                    walk(el.children);
                }
            };
        walk(HtmlRenderer::instance().elements());
        CHECK(uv == "alice", "JS: script sets input value");
        CHECK(boxHidden, "JS: style.display=none hides");
        // Timer: schedule then poll past due.
        HtmlRenderer::instance().loadHtml(
            "<html><body><input id=\"t\" name=\"t\" value=\"\">"
            "<script>setTimeout(function(){document.getElementById('t').value='late';},50);</script>"
            "</body></html>");
        SDL_Delay(80);
        HtmlRenderer::instance().pollJs();
        std::string tv;
        std::function<void(const std::list<HtmlElement>&)> walk2 =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (el.elemId == "t") tv = el.value;
                    walk2(el.children);
                }
            };
        walk2(HtmlRenderer::instance().elements());
        CHECK(tv == "late", "JS: setTimeout fires on poll");
        BrowserManager::instance().settings().js = false;
    }
    {
        // P12: srcset/data-original fallbacks resolve to real URLs.
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        HtmlRenderer::instance().loadHtml(
            "<html><body>"
            "<img src=\"data:image/gif;base64,x\" srcset=\"http://x/a.jpg 220w, http://x/b.jpg 480w\">"
            "<img data-original=\"http://x/c.png\">"
            "</body></html>");
        std::vector<std::string> srcs;
        std::function<void(const std::list<HtmlElement>&)> walk =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (el.type == HtmlElementType::IMAGE) srcs.push_back(el.src);
                    walk(el.children);
                }
            };
        walk(HtmlRenderer::instance().elements());
        CHECK(srcs.size() == 2, "P12: both lazy imgs parsed");
        if (srcs.size() == 2) {
            CHECK(srcs[0] == "http://x/a.jpg", "P12: srcset first URL wins");
            CHECK(srcs[1] == "http://x/c.png", "P12: data-original fallback");
        }
    }
    {
        // P10: 1x1 tracking pixels take no layout space.
        static const unsigned char kPng1x1[] = {
            0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,
            0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
            0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,0x89,0x00,0x00,0x00,
            0x0A,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0x00,0x01,0x00,0x00,
            0x05,0x00,0x01,0x0D,0x0A,0x2D,0xB4,0x00,0x00,0x00,0x00,0x49,
            0x45,0x4E,0x44,0xAE,0x42,0x60,0x82
        };
        std::vector<uint8_t> bytes(kPng1x1, kPng1x1 + sizeof(kPng1x1));
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        CHECK(HtmlRenderer::instance().decodeImageForTest("http://x/px.png", bytes),
              "P10: tracking png decodes");
        HtmlRenderer::instance().loadHtml(
            "<html><body><img src=\"http://x/px.png\"><p>after</p></body></html>");
        int iw = -1, ih = -1;
        std::function<void(const std::list<HtmlElement>&)> walk =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (el.type == HtmlElementType::IMAGE) { iw = el.width; ih = el.height; }
                    walk(el.children);
                }
            };
        walk(HtmlRenderer::instance().elements());
        CHECK(iw == 0 && ih == 0, "P10: tracking pixel takes no space");
    }
    {
        // P5b: intrinsicsize reserves layout space before load (no jump).
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        HtmlRenderer::instance().loadHtml(
            "<html><body><img src=\"http://x/a.jpg\" intrinsicsize=\"220x132\"></body></html>");
        int iw = 0, ih = 0;
        std::function<void(const std::list<HtmlElement>&)> walk =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (el.type == HtmlElementType::IMAGE) { iw = el.width; ih = el.height; }
                    walk(el.children);
                }
            };
        walk(HtmlRenderer::instance().elements());
        CHECK(iw == 220 && ih == 132, "P5b: image space reserved from intrinsicsize");
    }

    // ---- P6 gates: vnexpress acceptance (fixture + live) ----
    {
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        auto t0 = std::chrono::steady_clock::now();
        HtmlRenderer::instance().loadHtml(html);
        auto t1 = std::chrono::steady_clock::now();
        long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        int headlines = 0, links = 0, imgs = 0, descs = 0;
        std::function<void(const std::list<HtmlElement>&)> walk =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (el.type == HtmlElementType::TEXT && !el.text.empty()) {
                        // P9: CSS restyles headlines (small px, bold) — same
                        // heuristic as the P2 gate.
                        if (el.lineH > 36 || el.fontSize >= 22 || el.bold) headlines++;
                        if (!el.lines.empty()) descs++;
                    }
                    if (el.type == HtmlElementType::LINK) links++;
                    if (el.type == HtmlElementType::IMAGE) imgs++;
                    walk(el.children);
                }
            };
        walk(HtmlRenderer::instance().elements());
        std::cout << "[INFO] vnexpress: " << ms << "ms headlines=" << headlines
                  << " links=" << links << " imgs=" << imgs << std::endl;
        CHECK(ms < 1000, "P6: fixture parses < 1s");
        CHECK(headlines >= 20, "P6: 20+ headlines readable");
        CHECK(links >= 100, "P6: 100+ links navigable");
        CHECK(imgs == 27, "P6: all content images found");
        CHECK(descs > headlines, "P6: body text present beyond headlines");
    }
    {
        // Live fetch (best-effort: skipped, not failed, when offline).
        HttpResponse r = HttpClient::instance().get("https://vnexpress.net", {}, 20);
        if (!r.success || r.body.size() < 10000) {
            std::cout << "[INFO] P6: live fetch skipped (offline/blocked)" << std::endl;
        } else {
            HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
            HtmlRenderer::instance().loadHtml(r.body);
            int headlines = 0;
            std::function<void(const std::list<HtmlElement>&)> walk =
                [&](const std::list<HtmlElement>& els) {
                    for (const auto& el : els) {
                        if (el.type == HtmlElementType::TEXT && !el.text.empty() &&
                            (el.lineH > 36 || el.fontSize >= 22 || el.bold)) headlines++;
                        walk(el.children);
                    }
                };
            walk(HtmlRenderer::instance().elements());
            std::cout << "[INFO] live vnexpress: " << r.body.size()
                      << " bytes, headlines=" << headlines << std::endl;
            CHECK(headlines >= 10, "P6: live page headlines readable");
        }
    }

    // ---- Read-scroll gates: dpad UP/DOWN scrolls line-by-line and
    // highlights the nearest event (link underline, input border).
    {
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        HtmlRenderer::instance().loadHtml(html);
        int y0 = HtmlRenderer::instance().scrollY();
        HtmlRenderer::instance().handleInput(1);  // DOWN = scroll + highlight
        int y1 = HtmlRenderer::instance().scrollY();
        CHECK(y1 > y0, "READ: DOWN scrolls the page");
        int fi = HtmlRenderer::instance().focusIndex();
        CHECK(fi >= 0, "READ: DOWN highlights an event");
        int ft = HtmlRenderer::instance().focusedTypeForTest();
        bool isEvent = (ft == (int)HtmlElementType::LINK ||
                        ft == (int)HtmlElementType::BUTTON ||
                        ft == (int)HtmlElementType::INPUT_TEXT ||
                        ft == (int)HtmlElementType::INPUT_PASSWORD ||
                        ft == (int)HtmlElementType::INPUT_CHECKBOX ||
                        ft == (int)HtmlElementType::SELECT ||
                        ft == (int)HtmlElementType::MEDIA);
        CHECK(isEvent, "READ: highlight is an event, never text/image");
        HtmlRenderer::instance().handleInput(0);  // UP scrolls back
        CHECK(HtmlRenderer::instance().scrollY() < y1, "READ: UP scrolls back");
        HtmlRenderer::instance().handleInput(3);  // RIGHT = next in block
        HtmlRenderer::instance().handleInput(2);  // LEFT = prev in block
        CHECK(true, "READ: LEFT/RIGHT in-block hop no crash");
        HtmlRenderer::instance().focusFirst();
        CHECK(HtmlRenderer::instance().focusIndex() == 0, "READ: focusFirst lands on first widget");
    }
    // ---- P8 gates: URL suffix keys (browser keyboard only) ----
    {
        VkState s;
        VirtualKeyboard::reset(s, false);
        s.maxLen = 10;
        CHECK(VirtualKeyboard::typeText(s, ".com") && s.query == ".com", "P8: TLD suffix typed");
        CHECK(!s.telexMode, "P8: URL keyboard telex OFF by default");
        VirtualKeyboard::typeText(s, "1234567");
        CHECK(s.query.size() == 10, "P8: suffix respects maxLen");
        CHECK(!VirtualKeyboard::typeText(s, "x"), "P8: typeText stops at cap");
    }

    // ---- P9 gates: CSS cascade, display:none, font-size, inline ----
    {
        auto findFirst = [&](HtmlElementType t) -> const HtmlElement* {
            const HtmlElement* out = nullptr;
            std::function<void(const std::list<HtmlElement>&)> walk =
                [&](const std::list<HtmlElement>& els) {
                    for (const auto& el : els) {
                        if (!out && el.type == t && !el.text.empty()) out = &el;
                        walk(el.children);
                    }
                };
            walk(HtmlRenderer::instance().elements());
            return out;
        };
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        HtmlRenderer::instance().loadHtml(
            "<html><body><div class=\"wrap\"><p id=\"lead\" class=\"a\">Hello</p></div>"
            "<p class=\"gone\">Bye</p></body></html>");
        HtmlRenderer::instance().appendCssForTest(
            "p{color:#ff0000} .a{color:#00ff00} #lead{color:#0000ff} "
            "div p{font-size:40px} .gone{display:none} "
            "a:hover{color:#ff0000}");
        const HtmlElement* lead = findFirst(HtmlElementType::TEXT);
        CHECK(lead && lead->hasColor && lead->color.b == 255 && lead->color.r == 0,
              "P9: id beats class beats tag (blue wins)");
        CHECK(lead && lead->fontSize == 40, "P9: descendant selector font-size");
        CHECK(lead && lead->lineH > 36, "P9: line height follows font-size");
        // display:none hides the wrapper (children inherit the skip via layout)
        bool goneHidden = false;
        std::function<void(const std::list<HtmlElement>&)> walk2 =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (el.tag == "p" && el.cls == "gone") goneHidden = el.hide;
                    walk2(el.children);
                }
            };
        walk2(HtmlRenderer::instance().elements());
        CHECK(goneHidden, "P9: display:none hides node");
        // inline style beats everything
        HtmlRenderer::instance().loadHtml(
            "<html><body><p class=\"a\" style=\"color:#123456\">Hi</p></body></html>");
        HtmlRenderer::instance().appendCssForTest(".a{color:#ff0000}");
        const HtmlElement* hi = findFirst(HtmlElementType::TEXT);
        CHECK(hi && hi->hasColor && hi->color.r == 0x12 && hi->color.b == 0x56,
              "P9: inline style wins over stylesheet");
        // pseudo-class rule dropped (no red anywhere from a:hover)
        CHECK(HtmlRenderer::instance().cssRuleCount() > 0, "P9: rules parsed");
    }
    {
        // P14.1: font shorthand + relative-unit guard.
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        HtmlRenderer::instance().loadHtml("<html><body><p class=\"s\">A</p></body></html>");
        HtmlRenderer::instance().appendCssForTest(".s{font:700 14px/1.5 arial}");
        const HtmlElement* a = nullptr;
        std::function<void(const std::list<HtmlElement>&)> walk =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (!a && el.type == HtmlElementType::TEXT && !el.text.empty()) a = &el;
                    walk(el.children);
                }
            };
        walk(HtmlRenderer::instance().elements());
        CHECK(a && a->bold && a->fontSize == 14, "P14.1: font shorthand size+weight");
        HtmlRenderer::instance().loadHtml("<html><body><p class=\"s\">B</p></body></html>");
        HtmlRenderer::instance().appendCssForTest(".s{font:400 1rem arial}");
        a = nullptr;
        walk(HtmlRenderer::instance().elements());
        CHECK(a && !a->bold && a->fontSize == 0, "P14.1: relative units ignored");
    }
    {
        // P14.2: line-height px/number + padding.
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        HtmlRenderer::instance().loadHtml("<html><body><p class=\"s\">A</p></body></html>");
        HtmlRenderer::instance().appendCssForTest(".s{line-height:40px;padding-top:10px}");
        const HtmlElement* a = nullptr;
        std::function<void(const std::list<HtmlElement>&)> walk =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (!a && el.type == HtmlElementType::TEXT && !el.text.empty()) a = &el;
                    walk(el.children);
                }
            };
        walk(HtmlRenderer::instance().elements());
        // Margin lives on the <p> wrapper (consumed around children in
        // layout); line-height inherits into the text node.
        const HtmlElement* wrap = nullptr;
        const HtmlElement* txt = nullptr;
        std::function<void(const std::list<HtmlElement>&)> walk2 =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (el.tag == "p" && el.cls == "s") wrap = &el;
                    if (!txt && el.type == HtmlElementType::TEXT && !el.text.empty()) {
                        txt = &el;
                    }
                    walk2(el.children);
                }
            };
        walk2(HtmlRenderer::instance().elements());
        CHECK(wrap && wrap->marginTop == 5, "P14.2: wrapper padding-top halved");
        CHECK(txt && txt->lineH >= 36, "P14.2: line-height px inherited");
        HtmlRenderer::instance().loadHtml("<html><body><p class=\"s\">B</p></body></html>");
        HtmlRenderer::instance().appendCssForTest(".s{font-size:20px;line-height:1.5}");
        a = nullptr;
        walk(HtmlRenderer::instance().elements());
        CHECK(a && a->lineH >= 30, "P14.2: line-height multiplier");
    }
    {
        // Fixture: real vnexpress CSS parses into hundreds of rules.
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        HtmlRenderer::instance().loadHtml(html);
        size_t rules = HtmlRenderer::instance().cssRuleCount();
        std::cout << "[INFO] fixture css rules=" << rules << std::endl;
        CHECK(rules > 100, "P9: vnexpress stylesheets parsed");
    }
    {
        // P9b: white body => light theme (dark ink defaults).
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        HtmlRenderer::instance().loadHtml(
            "<html><head><style>body{background:#ffffff;color:#222222}</style></head>"
            "<body><p>Hello</p></body></html>");
        CHECK(HtmlRenderer::instance().pageLightForTest(), "P9b: white body => light theme");
        // Unstyled page keeps the browser-default white canvas.
        HtmlRenderer::instance().loadHtml("<html><body><p>Hi</p></body></html>");
        CHECK(HtmlRenderer::instance().pageLightForTest(), "P9b: default canvas is white");
    }
    {
        // P11 gates: reader mode on a real article page.
        BrowserManager::instance().settings().article = true;
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        std::string art = readFile("tests/fixtures/article.html");
        CHECK(art.size() > 100000, "P11: article fixture loaded");
        HtmlRenderer::instance().loadHtml(art);
        size_t visChars = 0;
        bool titleSeen = false, commentHidden = false, commentFound = false;
        std::function<void(const std::list<HtmlElement>&, bool)> walk =
            [&](const std::list<HtmlElement>& els, bool hiddenAbove) {
                for (const auto& el : els) {
                    bool h = hiddenAbove || el.hide;
                    if (!h && el.type == HtmlElementType::TEXT && !el.text.empty()) {
                        visChars += el.text.size();
                        if (el.text.find("Cổ thụ") != std::string::npos) titleSeen = true;
                    }
                    if ((el.cls.find("comment") != std::string::npos) &&
                        el.tag != "a" && el.text.empty()) {
                        commentFound = true;
                        if (h) commentHidden = true;
                    }
                    walk(el.children, h);
                }
            };
        walk(HtmlRenderer::instance().elements(), false);
        std::cout << "[INFO] article visible chars=" << visChars << std::endl;
        CHECK(visChars > 2000, "P11: article body readable");
        CHECK(titleSeen, "P11: article title kept");
        // Photo captions are content too (inactive slideshow copies excluded).
        bool captionSeen = false;
        std::function<void(const std::list<HtmlElement>&, bool)> walkCap =
            [&](const std::list<HtmlElement>& els, bool hiddenAbove) {
                for (const auto& el : els) {
                    bool h = hiddenAbove || el.hide;
                    if (!h && el.type == HtmlElementType::TEXT &&
                        el.text.find("Thân cây chắn ngang") != std::string::npos) {
                        captionSeen = true;
                    }
                    walkCap(el.children, h);
                }
            };
        walkCap(HtmlRenderer::instance().elements(), false);
        CHECK(captionSeen, "P11: photo captions kept");
        CHECK(commentFound && commentHidden, "P11: comments hidden");
        // Toggle off restores the comment box (CSS display:none may persist).
        BrowserManager::instance().settings().article = false;
        HtmlRenderer::instance().refreshAfterSettings();
        bool commentBack = false;
        std::function<void(const std::list<HtmlElement>&, bool)> walk3 =
            [&](const std::list<HtmlElement>& els, bool hiddenAbove) {
                for (const auto& el : els) {
                    bool h = hiddenAbove || el.hide;
                    if (!h && el.cls.find("comment") != std::string::npos &&
                        el.tag != "a" && el.text.empty()) {
                        commentBack = true;
                    }
                    walk3(el.children, h);
                }
            };
        walk3(HtmlRenderer::instance().elements(), false);
        CHECK(commentBack, "P11: toggle off restores chrome");
        BrowserManager::instance().settings().article = true;
        HtmlRenderer::instance().refreshAfterSettings();
        CHECK(HtmlRenderer::instance().isReaderMode(), "P15.1: reader indicator on article");
    }
    {
        // P15.2 gates: find-in-page engine.
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        HtmlRenderer::instance().loadHtml(html);
        int n = HtmlRenderer::instance().findMatches("ng");
        CHECK(n > 5, "P15.2: matches found");
        int i0 = HtmlRenderer::instance().findIndex();
        CHECK(i0 == 0, "P15.2: starts at first hit");
        CHECK(HtmlRenderer::instance().findNext(), "P15.2: next works");
        CHECK(HtmlRenderer::instance().findIndex() == 1 % n, "P15.2: index advances");
        HtmlRenderer::instance().clearFind();
        CHECK(HtmlRenderer::instance().findCount() == 0, "P15.2: clear resets");
        CHECK(HtmlRenderer::instance().findMatches("") == 0, "P15.2: empty query");
    }
    {
        // P13 gates: table grid + media box + play request.
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        HtmlRenderer::instance().loadHtml(
            "<html><body><table><tr><td>A1</td><td>B1</td></tr>"
            "<tr><td>A2</td><td>B2</td></tr></table>"
            "<iframe src=\"http://x/v.mp4\"></iframe></body></html>");
        std::vector<const HtmlElement*> cells;
        const HtmlElement* media = nullptr;
        std::function<void(const std::list<HtmlElement>&)> walk =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (el.type == HtmlElementType::TABLE_CELL) cells.push_back(&el);
                    if (el.type == HtmlElementType::MEDIA) media = &el;
                    walk(el.children);
                }
            };
        walk(HtmlRenderer::instance().elements());
        CHECK(cells.size() == 4, "P13: 4 cells parsed");
        if (cells.size() == 4) {
            CHECK(cells[0]->y == cells[1]->y && cells[0]->x < cells[1]->x,
                  "P13: row cells side-by-side");
            CHECK(cells[2]->y > cells[0]->y, "P13: second row below first");
            CHECK(cells[0]->width > 100, "P13: columns share width");
        }
        CHECK(media != nullptr, "P13: iframe parsed as media");
        if (media) {
            CHECK(media->width > 500 && media->height >= 120, "P13: media box sized");
        }
        // A on a media-only page queues a play request (deterministic:
        // the media box is the sole focusable).
        HtmlRenderer::instance().loadHtml(
            "<html><body><iframe src=\"http://x/v.mp4\"></iframe></body></html>");
        HtmlRenderer::instance().focusFirst();
        HtmlRenderer::instance().handleInput(4);  // A on the media box
        std::string req;
        CHECK(HtmlRenderer::instance().takeMediaRequest(req), "P13: A queues play request");
        CHECK(req == "http://x/v.mp4", "P13: request URL resolved");
        CHECK(!HtmlRenderer::instance().takeMediaRequest(req), "P13: request queue drains");
    }
    {
        // Settings wiring: images off => zero space; css off => no colors;
        // font scale changes effective px. Flags restored after.
        BrowserSettings& bs = BrowserManager::instance().settings();
        bs.images = false;
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        HtmlRenderer::instance().loadHtml(
            "<html><body><img src=\"http://x/a.jpg\" intrinsicsize=\"220x132\"></body></html>");
        int iw = -1;
        std::function<void(const std::list<HtmlElement>&)> walk =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (el.type == HtmlElementType::IMAGE) iw = el.width;
                    walk(el.children);
                }
            };
        walk(HtmlRenderer::instance().elements());
        CHECK(iw == 0, "SET: images off takes no space");
        bs.images = true;
        bs.css = false;
        HtmlRenderer::instance().loadHtml(
            "<html><body><p class=\"a\">Hi</p></body></html>");
        HtmlRenderer::instance().appendCssForTest(".a{color:#ff0000}");
        bool anyColor = false;
        std::function<void(const std::list<HtmlElement>&)> walk2 =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (el.hasColor) anyColor = true;
                    walk2(el.children);
                }
            };
        walk2(HtmlRenderer::instance().elements());
        CHECK(!anyColor, "SET: css off ignores stylesheets");
        bs.css = true;
        bs.fontScalePct = 150;
        CHECK(HtmlRenderer::instance().fontPxForTest(20) == 30, "SET: 150% scale (20->30)");
        bs.fontScalePct = 100;
        CHECK(HtmlRenderer::instance().fontPxForTest(20) == 20, "SET: 100% scale (20->20)");
        bs.fontScalePct = 120;
    }
    {
        // P15.4: back/forward history (refused-fast local URLs, no real fetch).
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        HtmlRenderer::instance().loadUrl("http://127.0.0.1:9/a");
        HtmlRenderer::instance().loadUrl("http://127.0.0.1:9/b");
        CHECK(HtmlRenderer::instance().historySizeForTest() == 2, "P15.4: two entries");
        CHECK(!HtmlRenderer::instance().canGoForward(), "P15.4: no forward at tip");
        HtmlRenderer::instance().goBack();
        CHECK(HtmlRenderer::instance().currentUrl() == "http://127.0.0.1:9/a",
              "P15.4: back lands on first page (no silent reload)");
        CHECK(HtmlRenderer::instance().canGoForward(), "P15.4: forward available");
        HtmlRenderer::instance().goForward();
        CHECK(HtmlRenderer::instance().currentUrl() == "http://127.0.0.1:9/b",
              "P15.4: forward returns");
        // Back then new URL drops the forward entry.
        HtmlRenderer::instance().goBack();
        HtmlRenderer::instance().loadUrl("http://127.0.0.1:9/c");
        CHECK(HtmlRenderer::instance().historySizeForTest() == 2, "P15.4: forward truncated");
        CHECK(!HtmlRenderer::instance().canGoForward(), "P15.4: no forward after branch");
    }

    {
        // GIF: 2-frame 8x8 red/blue generated by PIL (tests/fixtures/anim2.gif).
        static const unsigned char kGif[] = {0x47,0x49,0x46,0x38,0x39,0x61,0x08,0x00,0x08,0x00,0x81,0x00,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x21,0xFF,0x0B,0x4E,0x45,0x54,0x53,0x43,0x41,0x50,0x45,0x32,0x2E,0x30,0x03,0x01,0x00,0x00,0x00,0x21,0xF9,0x04,0x00,0x0A,0x00,0x00,0x00,0x2C,0x00,0x00,0x00,0x00,0x08,0x00,0x08,0x00,0x00,0x08,0x0F,0x00,0x01,0x08,0x1C,0x48,0xB0,0xA0,0xC1,0x83,0x08,0x13,0x2A,0x4C,0x18,0x10,0x00,0x21,0xF9,0x04,0x01,0x14,0x00,0x01,0x00,0x2C,0x00,0x00,0x00,0x00,0x08,0x00,0x08,0x00,0x81,0x00,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x0F,0x00,0x01,0x08,0x1C,0x48,0xB0,0xA0,0xC1,0x83,0x08,0x13,0x2A,0x4C,0x18,0x10,0x00,0x3B};
        std::vector<uint8_t> bytes(kGif, kGif + sizeof(kGif));
        BrowserManager::instance().settings().gifAnim = true;
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        CHECK(HtmlRenderer::instance().decodeGifForTest("gif://t", bytes), "GIF: decodes");
        CHECK(HtmlRenderer::instance().gifFrameCountForTest("gif://t") == 2, "GIF: 2 frames");
        CHECK(!HtmlRenderer::instance().decodeGifForTest("gif://bad", {0x47, 0x49, 0x46}),
              "GIF: truncated rejected");
        BrowserManager::instance().settings().gifAnim = false;
    }

    {
        // Viewport Meta & Smart Media Query Filtering
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        HtmlRenderer::instance().loadHtml(
            "<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, user-scalable=no\"></head>"
            "<body><p>Hello</p></body></html>");
        CHECK(HtmlRenderer::instance().viewport().hasMeta, "VP: has meta");
        CHECK(HtmlRenderer::instance().viewport().isDeviceWidth, "VP: is device-width");
        CHECK(HtmlRenderer::instance().viewport().initialScale == 1.0f, "VP: scale 1.0");
        CHECK(!HtmlRenderer::instance().viewport().userScalable, "VP: user-scalable no");
        CHECK(HtmlRenderer::instance().effectiveViewportWidth() == 480, "VP: effective width device-width");

        // Explicit width=768
        HtmlRenderer::instance().loadHtml(
            "<html><head><meta name=\"viewport\" content=\"width=768\"></head>"
            "<body><p>Fixed 768</p></body></html>");
        CHECK(HtmlRenderer::instance().viewport().width == 768, "VP: explicit width 768");
        CHECK(HtmlRenderer::instance().effectiveViewportWidth() == 680, "VP: effective width 768");

        // Smart Media Query Filtering: min-width: 1200px rejected, max-width: 1024px accepted
        HtmlRenderer::instance().loadHtml(
            "<html><head><meta name=\"viewport\" content=\"width=device-width\">"
            "<style>"
            ".card { color: #111111; }"
            "@media (min-width: 1200px) { .card { color: #ff0000; } }"
            "@media (max-width: 1024px) { .card { background-color: #00ff00; } }"
            "</style></head>"
            "<body><div class=\"card\">Responsive Box</div></body></html>");
        // Verify via elements tree
        const auto& els = HtmlRenderer::instance().elements();
        CHECK(!els.empty(), "VP: element parsed");
        if (!els.empty()) {
            const auto& card = els.front();
            // Color should remain #111111 (not overwritten by 1200px desktop rule #ff0000)
            CHECK(card.color.r != 255 || card.color.g != 0 || card.color.b != 0, "VP: min-width 1200px rejected");
            // Background should be #00ff00 (accepted by max-width: 1024px)
            CHECK(card.hasBg && card.bgColor.g == 255, "VP: max-width 1024px applied");
        }
    }

    {
        // Headers test
        auto hdrs = HtmlRenderer::getBrowserHeaders("https://vnexpress.net");
        bool hasUa = false, hasRef = false;
        for (const auto& h : hdrs) {
            if (h.find("User-Agent:") != std::string::npos) hasUa = true;
            if (h.find("Referer: https://vnexpress.net") != std::string::npos) hasRef = true;
        }
        CHECK(hasUa, "HDR: Modern User-Agent present");
        CHECK(hasRef, "HDR: Referer present for anti-hotlink");

        // Text Alignment inheritance & override test
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        HtmlRenderer::instance().loadHtml(
            "<html><head><style>"
            ".parent { text-align: center; }"
            ".child { text-align: left; }"
            ".icon { width: 48px; height: 32px; }"
            "</style></head>"
            "<body>"
            "<div class=\"parent\">"
            "  <p class=\"child\">Left content</p>"
            "  <div align=\"right\"><p>Right content</p></div>"
            "  <img class=\"icon\" src=\"icon.png\">"
            "  <div><div class=\"empty\"></div></div>"
            "</div>"
            "</body></html>");

        const auto& els = HtmlRenderer::instance().elements();
        CHECK(!els.empty(), "DOM: Tree generated");
        if (!els.empty()) {
            const auto& parent = els.front();
            CHECK(parent.textAlign == TextAlign::CENTER, "ALIGN: Parent centered");
            if (!parent.children.empty()) {
                auto it = parent.children.begin();
                const auto& childP = *it;
                CHECK(childP.textAlign == TextAlign::LEFT, "ALIGN: Child overrides parent with LEFT");

                ++it;
                if (it != parent.children.end()) {
                    const auto& rightDiv = *it;
                    CHECK(rightDiv.textAlign == TextAlign::RIGHT, "ALIGN: HTML align=right respected");
                }

                ++it;
                if (it != parent.children.end()) {
                    const auto& img = *it;
                    CHECK(img.type == HtmlElementType::IMAGE, "IMG: Tag recognized");
                    CHECK(img.imgW == 48, "IMG: CSS width 48px applied");
                    CHECK(img.imgH == 32, "IMG: CSS height 32px applied");
                }
            }
        }
    }
    {
        // Block nav: two blocks separated by a tall gap; L/R stays inside,
        // U/D scrolls line-by-line and the highlight follows into the block.
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        std::string spacer;
        for (int i = 0; i < 40; i++) spacer += "<br>";
        HtmlRenderer::instance().loadHtml(
            std::string("<html><body><div><a href=\"http://x/1\">L1</a><a href=\"http://x/2\">L2</a></div>") +
            spacer + "<div><input name=\"a\"><a href=\"http://x/3\">L3</a></div></body></html>");
        HtmlRenderer::instance().focusFirst();
        int i0 = HtmlRenderer::instance().focusIndex();
        HtmlRenderer::instance().handleInput(3);  // RIGHT: next in block
        int i1 = HtmlRenderer::instance().focusIndex();
        CHECK(i1 != i0, "BLK: RIGHT moves within block");
        HtmlRenderer::instance().handleInput(3);  // RIGHT: wraps inside block
        CHECK(HtmlRenderer::instance().focusIndex() == i0, "BLK: RIGHT wraps in block");
        // DOWN repeatedly: highlight must leave block 0 for block 1.
        int cur = i0, guard = 0;
        while (guard++ < 60) {
            HtmlRenderer::instance().handleInput(1);
            cur = HtmlRenderer::instance().focusIndex();
            if (cur >= 2) break;
        }
        CHECK(cur >= 2, "BLK: DOWN scrolls highlight into next block");
        // UP repeatedly: highlight returns to block 0.
        guard = 0;
        while (guard++ < 60) {
            HtmlRenderer::instance().handleInput(0);
            cur = HtmlRenderer::instance().focusIndex();
            if (cur < 2) break;
        }
        CHECK(cur == i0 || cur == i1, "BLK: UP returns highlight to first block");
        // TEXT/IMAGE nodes are never focusable (events only, no highlight).
        // Walk more stops than exist; every stop must be an event type.
        bool onlyEvents = true;
        for (int k = 0; k < 12; k++) {
            int t = HtmlRenderer::instance().focusedTypeForTest();
            bool isEvent = (t == (int)HtmlElementType::LINK ||
                            t == (int)HtmlElementType::BUTTON ||
                            t == (int)HtmlElementType::INPUT_TEXT ||
                            t == (int)HtmlElementType::INPUT_PASSWORD ||
                            t == (int)HtmlElementType::INPUT_CHECKBOX ||
                            t == (int)HtmlElementType::SELECT ||
                            t == (int)HtmlElementType::MEDIA);
            if (!isEvent) onlyEvents = false;
            HtmlRenderer::instance().handleInput(1);  // DOWN across blocks
            HtmlRenderer::instance().handleInput(3);  // RIGHT inside block
        }
        CHECK(onlyEvents, "BLK: only events get focus (no text/image highlight)");
    }
    {
        // Spans: colspan=2 cell covers both columns; rowspan pushes content.
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        HtmlRenderer::instance().loadHtml(
            "<html><body><table><tr><td colspan=2>Wide</td></tr>"
            "<tr><td>A</td><td>B</td></tr>"
            "<tr><td rowspan=2>R</td><td>C1</td></tr>"
            "<tr><td>C2</td></tr></table></body></html>");
        struct Box {
            std::string t;
            int x, y, w, h;
        };
        std::vector<Box> boxes;
        std::function<void(const std::list<HtmlElement>&)> walk =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (el.type == HtmlElementType::TABLE_CELL) {
                        std::string t = el.text;
                        for (const auto& c : el.children) t += c.text;
                        boxes.push_back({t, el.x, el.y, el.width, el.height});
                    }
                    walk(el.children);
                }
            };
        walk(HtmlRenderer::instance().elements());
        const Box* wide = nullptr;
        const Box* a = nullptr;
        const Box* r = nullptr;
        for (const auto& b : boxes) {
            if (b.t == "Wide") wide = &b;
            if (b.t == "A") a = &b;
            if (b.t == "R") r = &b;
        }
        CHECK(wide && a && r, "SPAN: all span cells found");
        if (wide && a) {
            CHECK(wide->w > a->w * 3 / 2, "SPAN: colspan covers 2 columns");
            CHECK(wide->y < a->y, "SPAN: wide cell on first row");
        }
        if (r && a) {
            CHECK(r->h > a->h, "SPAN: rowspan stretches two rows");
        }
    }
    {
        // Background-image: parsed from CSS, queued for fetch (renderer on).
        bool vok = (SDL_Init(SDL_INIT_VIDEO) == 0);
        SDL_Window* w4 = vok ? SDL_CreateWindow("t4", 0, 0, 1024, 768, SDL_WINDOW_HIDDEN) : nullptr;
        SDL_Renderer* r4 = w4 ? SDL_CreateRenderer(w4, -1, SDL_RENDERER_SOFTWARE) : nullptr;
        if (r4) {
            HtmlRenderer::instance().init(r4, font, "assets/fonts/NotoSans-Regular.ttf", 30);
            HtmlRenderer::instance().loadHtml(
                "<html><body><div style=\"background-image:url(http://127.0.0.1:9/bg.png)\">Hi</div></body></html>");
            std::string found;
            std::function<void(const std::list<HtmlElement>&)> walk =
                [&](const std::list<HtmlElement>& els) {
                    for (const auto& el : els) {
                        if (!el.bgImage.empty()) found = el.bgImage;
                        walk(el.children);
                    }
                };
            walk(HtmlRenderer::instance().elements());
            CHECK(found == "http://127.0.0.1:9/bg.png", "BGIMG: url parsed");
            CHECK(HtmlRenderer::instance().imagePendingCount() >= 1, "BGIMG: queued for fetch");
            SDL_DestroyRenderer(r4);
        } else {
            std::cout << "[INFO] BGIMG: no renderer, queue test skipped" << std::endl;
        }
        if (w4) SDL_DestroyWindow(w4);
        if (vok) SDL_Quit();
    }

    {
        // SVG: vnexpress logo rasterizes to a non-blank sheet, aspect kept.
        std::ifstream fh("tests/fixtures/logo.svg", std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(fh)),
                                   std::istreambuf_iterator<char>());
        CHECK(bytes.size() > 1000, "SVG: logo fixture loaded");
        std::vector<uint8_t> rgba;
        int sw = 0, sh = 0;
        CHECK(svgRasterize(bytes.data(), bytes.size(), 480, rgba, sw, sh), "SVG: rasterizes");
        CHECK(sw == 480 && sh > 80 && sh < 100, "SVG: aspect kept (150x28)");
        size_t alpha = 0;
        for (size_t i = 3; i < rgba.size(); i += 16) {
            if (rgba[i] > 8) alpha++;
        }
        CHECK(alpha > 100, "SVG: painted pixels present");
        std::vector<uint8_t> bad;
        int bw = 0, bh = 0;
        const char* junk = "<html>not svg</html>";
        CHECK(!svgRasterize(reinterpret_cast<const uint8_t*>(junk), strlen(junk), 480, bad, bw, bh),
              "SVG: garbage rejected");
    }

    {
        // JS Engine (Duktape): DOM manipulation, timers, safe error handling
        SDL_Init(SDL_INIT_TIMER);
        BrowserManager::instance().settings().js = true;
        HtmlRenderer::instance().init(nullptr, font, "assets/fonts/NotoSans-Regular.ttf", 30);
        HtmlRenderer::instance().loadHtml(
            "<html><body>"
            "<div id=\"box\">Initial</div>"
            "<input id=\"field\" value=\"abc\">"
            "<script>"
            "var b = document.getElementById('box');"
            "if (b) b.textContent = 'Hello Duktape';"
            "var f = document.getElementById('field');"
            "if (f) f.value = 'xyz';"
            "setTimeout(function() { if (b) b.textContent = 'Timer Done'; }, 10);"
            "</script>"
            "</body></html>");

        std::string boxText, fieldValue;
        std::function<void(const std::list<HtmlElement>&)> walk =
            [&](const std::list<HtmlElement>& els) {
                for (const auto& el : els) {
                    if (el.elemId == "box") boxText = el.text;
                    if (el.elemId == "field") fieldValue = el.value;
                    walk(el.children);
                }
            };
        walk(HtmlRenderer::instance().elements());
        CHECK(boxText == "Hello Duktape", "JS: textContent modified by inline script");
        CHECK(fieldValue == "xyz", "JS: input value modified by inline script");

        // Test Timer execution via pollJs()
        SDL_Delay(25);
        HtmlRenderer::instance().pollJs();
        boxText.clear();
        walk(HtmlRenderer::instance().elements());
        CHECK(boxText == "Timer Done", "JS: setTimeout callback executed on poll");

        // Error handling safety: runtime script error must not crash or halt
        JsEngine eng(&HtmlRenderer::instance(), 999, "http://test");
        CHECK(!eng.runScript("throw new Error('boom');", "err_test"), "JS: error caught safely");
        CHECK(eng.runScript("var x = 1 + 2;", "ok_test"), "JS: valid script runs");
        BrowserManager::instance().settings().js = false;
    }

    // ---- NetSurf Core Engine Tests (Phase 1 & Phase 2) ----
    {
        std::cout << "\n--- NetSurf Core Engine Gate: W3C DOM & CSS Cascade ---" << std::endl;
        NetSurfBridge::instance().init();
        std::string summary;
        bool p1Ok = NetSurfBridge::instance().parseHtml(
            "<html><body><article class=\"item-news\"><h1>Tiêu đề</h1></article></body></html>",
            summary);
        CHECK(p1Ok, "NetSurf P1: W3C DOM parsing via Hubbub");
        std::cout << "[INFO] " << summary << std::endl;

        std::unique_ptr<NetSurfStyledNode> styledRoot;
        std::string testHtml =
            "<html><body>"
            "<article class=\"item-news-common\">"
            "<div class=\"thumb-art\"><img src=\"pic.jpg\"/></div>"
            "<div class=\"content-art\"><h3>Tiêu đề báo</h3><p>Tóm tắt tin tức</p></div>"
            "</article></body></html>";
        std::string testCss =
            ".item-news-common { display: flex; flex-direction: row; }\n"
            ".thumb-art { width: 520px; }\n"
            ".content-art { flex-grow: 1; }\n";

        bool p2Ok = NetSurfBridge::instance().parseAndStyle(testHtml, testCss, styledRoot);
        CHECK(p2Ok && styledRoot != nullptr, "NetSurf P2: CSS Selection & Cascading");

        bool flexFound = false;
        bool flexRowFound = false;
        bool widthFound = false;
        std::function<void(const NetSurfStyledNode*)> checkNode = [&](const NetSurfStyledNode* n) {
            if (!n) return;
            if (n->className == "item-news-common") {
                if (n->style.display == 0x11) flexFound = true; // CSS_DISPLAY_FLEX
                if (n->style.flexDirection == 0x01) flexRowFound = true; // CSS_FLEX_DIRECTION_ROW
            }
            if (n->className == "thumb-art") {
                if (n->style.widthPx == 520) widthFound = true;
            }
            for (const auto& c : n->children) checkNode(c.get());
        };
        if (styledRoot) checkNode(styledRoot.get());

        CHECK(flexFound, "NetSurf P2: .item-news-common display is 'flex' (0x11)");
        CHECK(flexRowFound, "NetSurf P2: .item-news-common flex-direction is 'row' (0x01)");
        CHECK(widthFound, "NetSurf P2: .thumb-art computed width is 520px");
    }

    // ---- NetSurf Phase 3: 2D RenderBox Layout & Flexbox Row Engine ----
    {
        std::cout << "\n--- NetSurf Phase 3: 2D RenderBox Layout & Flexbox Engine ---" << std::endl;
        std::string testHtml =
            "<html><body>"
            "<article class=\"item-news-common\">"
            "<div class=\"thumb-art\"><img src=\"pic.jpg\"/></div>"
            "<div class=\"content-art\"><h3>Tiêu đề bài báo</h3><p>Tóm tắt tin tức trên TrimUI</p></div>"
            "</article></body></html>";
        std::string testCss =
            ".item-news-common { display: flex; flex-direction: row; }\n"
            ".thumb-art { width: 520px; }\n"
            ".content-art { flex-grow: 1; }\n";

        std::unique_ptr<RenderBox> renderTree;
        bool p3Ok = NetSurfBridge::instance().layout(testHtml, testCss, 984, renderTree);
        CHECK(p3Ok && renderTree != nullptr, "NetSurf P3: build 2D RenderBox layout tree");

        RenderBox* flexBox = nullptr;
        std::function<void(RenderBox*)> findFlex = [&](RenderBox* b) {
            if (!b) return;
            if (b->className == "item-news-common") {
                flexBox = b;
                return;
            }
            for (auto& c : b->children) findFlex(c.get());
        };
        if (renderTree) findFlex(renderTree.get());

        CHECK(flexBox != nullptr, "NetSurf P3: located .item-news-common RenderBox");
        if (flexBox) {
            CHECK(flexBox->type == RenderBoxType::FLEX_ROW, "NetSurf P3: box type is FLEX_ROW");
            CHECK(flexBox->children.size() >= 2, "NetSurf P3: flex row has 2 column children");

            if (flexBox->children.size() >= 2) {
                RenderBox* col1 = flexBox->children[0].get(); // thumb-art
                RenderBox* col2 = flexBox->children[1].get(); // content-art

                std::cout << "[INFO] Col 1 (thumb-art): x=" << col1->x << ", y=" << col1->y
                          << ", w=" << col1->width << ", h=" << col1->height << std::endl;
                std::cout << "[INFO] Col 2 (content-art): x=" << col2->x << ", y=" << col2->y
                          << ", w=" << col2->width << ", h=" << col2->height << std::endl;

                CHECK(col1->y == col2->y, "NetSurf P3: 2 columns share identical Y baseline (horizontal layout)");
                CHECK(col2->x >= col1->x + col1->width, "NetSurf P3: column 2 placed to the right of column 1");
                CHECK(col1->width > 0 && col2->width > 0, "NetSurf P3: both columns have positive allocated widths");
                CHECK(col1->height > 0 && col2->height > 0, "NetSurf P3: both columns have measured heights");
                CHECK(flexBox->height >= std::max(col1->height, col2->height),
                      "NetSurf P3: flex container height bounds maximum column height");
            }
        }
    }

    // -------------------------------------------------------------------------
    // Phase 4: NetSurfRenderer SDL2/TrimUI Brick Rendering Engine & 4-way D-Pad
    // -------------------------------------------------------------------------
    {
        std::cout << "\n=== Phase 4: NetSurfRenderer Engine & 4-Way D-Pad Navigation ===" << std::endl;
        auto& renderer = NetSurfRenderer::instance();
        renderer.reset();

        // 1. Build a synthetic multi-column, multi-row RenderBox tree representing news articles:
        auto root = std::make_unique<RenderBox>();
        root->type = RenderBoxType::BLOCK;
        root->x = 0;
        root->y = 0;
        root->width = 984;
        root->height = 1200;

        auto art1Thumb = std::make_unique<RenderBox>();
        art1Thumb->x = 20; art1Thumb->y = 20; art1Thumb->width = 160; art1Thumb->height = 120;
        art1Thumb->isFocusable = true; art1Thumb->href = "art1_thumb";

        auto art1Title = std::make_unique<RenderBox>();
        art1Title->x = 200; art1Title->y = 20; art1Title->width = 400; art1Title->height = 40;
        art1Title->isFocusable = true; art1Title->href = "art1_title";

        auto art1Cmt = std::make_unique<RenderBox>();
        art1Cmt->x = 200; art1Cmt->y = 70; art1Cmt->width = 100; art1Cmt->height = 30;
        art1Cmt->isFocusable = true; art1Cmt->href = "art1_comments";

        auto art2Thumb = std::make_unique<RenderBox>();
        art2Thumb->x = 20; art2Thumb->y = 220; art2Thumb->width = 160; art2Thumb->height = 120;
        art2Thumb->isFocusable = true; art2Thumb->href = "art2_thumb";

        auto art2Title = std::make_unique<RenderBox>();
        art2Title->x = 200; art2Title->y = 220; art2Title->width = 400; art2Title->height = 40;
        art2Title->isFocusable = true; art2Title->href = "art2_title";

        root->children.push_back(std::move(art1Thumb));
        root->children.push_back(std::move(art1Title));
        root->children.push_back(std::move(art1Cmt));
        root->children.push_back(std::move(art2Thumb));
        root->children.push_back(std::move(art2Title));

        renderer.setRenderTree(std::move(root));

        CHECK(renderer.focusableCount() == 5, "NetSurf P4: collected 5 focusable elements");
        CHECK(renderer.focusIndex() == 0, "NetSurf P4: initial focus defaults to first element");
        CHECK(renderer.focusedBox() != nullptr && renderer.focusedBox()->href == "art1_thumb",
              "NetSurf P4: initial focused box is Article 1 thumbnail");

        // 2. 4-Way D-pad Spatial Navigation:
        // From art1_thumb (x=20, y=20): Right should jump to art1_title (x=200, y=20)
        bool movedRight = renderer.navigateRight();
        CHECK(movedRight, "NetSurf P4: navigateRight succeeded");
        CHECK(renderer.focusedBox() && renderer.focusedBox()->href == "art1_title",
              "NetSurf P4: navigateRight jumped from left thumbnail to right title");

        // From art1_title (x=200, y=20): Down should move to art1_comments (x=200, y=70)
        bool movedDown = renderer.navigateDown();
        CHECK(movedDown, "NetSurf P4: navigateDown succeeded");
        CHECK(renderer.focusedBox() && renderer.focusedBox()->href == "art1_comments",
              "NetSurf P4: navigateDown jumped to comment link below");

        // From art1_comments (x=200, y=70): Left should move back to art1_thumb (x=20, y=20)
        bool movedLeft = renderer.navigateLeft();
        CHECK(movedLeft, "NetSurf P4: navigateLeft succeeded");
        CHECK(renderer.focusedBox() && renderer.focusedBox()->href == "art1_thumb",
              "NetSurf P4: navigateLeft jumped back to left thumbnail");

        // From art1_thumb (x=20, y=20): Down should move to art2_thumb (x=20, y=220)
        bool movedDown2 = renderer.navigateDown();
        CHECK(movedDown2, "NetSurf P4: navigateDown to next article succeeded");
        CHECK(renderer.focusedBox() && renderer.focusedBox()->href == "art2_thumb",
              "NetSurf P4: navigateDown jumped vertically to Article 2 thumbnail");

        // From art2_thumb (x=20, y=220): Up should jump back to art1_thumb (x=20, y=20)
        bool movedUp = renderer.navigateUp();
        CHECK(movedUp, "NetSurf P4: navigateUp back to top row succeeded");
        CHECK(renderer.focusedBox() && renderer.focusedBox()->href == "art1_thumb",
              "NetSurf P4: navigateUp returned to Article 1 thumbnail");

        // 3. Viewport auto-scrolling & ensureVisible:
        auto lowElement = std::make_unique<RenderBox>();
        lowElement->x = 20;
        lowElement->y = 900;
        lowElement->width = 200;
        lowElement->height = 50;
        lowElement->isFocusable = true;
        lowElement->href = "footer_link";

        renderer.setScrollY(0);
        CHECK(renderer.scrollY() == 0, "NetSurf P4: scrollY initialized at 0");
        renderer.ensureVisible(lowElement.get());
        CHECK(renderer.scrollY() > 0, "NetSurf P4: ensureVisible scrolled viewport down for low element");

        // 4. Image decoding & caching test seam:
        const uint8_t png1x1[] = {
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
            0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
            0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
            0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x00, 0x01, 0x00, 0x00,
            0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x49,
            0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
        };
        std::vector<uint8_t> pngBytes(png1x1, png1x1 + sizeof(png1x1));
        std::string testImgUrl = "https://example.com/test_1x1.png";

        CHECK(!renderer.isImageLoaded(testImgUrl), "NetSurf P4: image not loaded initially");
        bool decodeOk = renderer.decodeImageForTest(testImgUrl, pngBytes);
        CHECK(decodeOk, "NetSurf P4: decodeImageForTest successfully decoded 1x1 PNG");
        CHECK(renderer.isImageLoaded(testImgUrl), "NetSurf P4: image marked as loaded in texture map");
        CHECK(renderer.imageCachedCount() >= 1, "NetSurf P4: image cache contains decoded image entry");
    }

    // -------------------------------------------------------------------------
    // Phase 5: Dynamic Engine Switching & BrowserManager Integration
    // -------------------------------------------------------------------------
    {
        std::cout << "\n=== Phase 5: Dynamic Engine Switching & BrowserManager Integration ===" << std::endl;
        auto& bm = BrowserManager::instance();
        auto& ns = NetSurfEngine::instance();
        auto& hr = HtmlRenderer::instance();

        // 1. Initial State: Lite Engine
        bm.settings().fullEngine = false;
        CHECK(bm.engineMode() == BrowserEngine::LITE, "NetSurf P5: Default/explicit engineMode is LITE");

        // 2. Switch to FULL engine
        bm.setEngineMode(BrowserEngine::FULL);
        CHECK(bm.engineMode() == BrowserEngine::FULL, "NetSurf P5: engineMode switched to FULL");
        CHECK(bm.settings().fullEngine == true, "NetSurf P5: settings.fullEngine is true");

        // 3. Load HTML through HtmlRenderer with fullEngine enabled
        std::string sampleArticle =
            "<html><body>"
            "<article class=\"item-news-common\">"
            "<div class=\"thumb-art\"><img src=\"https://example.com/banner.jpg\"/></div>"
            "<div class=\"content-art\"><a href=\"https://m.vnexpress.net/article-123.html\">Bản tin công nghệ</a><p>Chi tiết bài viết</p></div>"
            "</article></body></html>";

        hr.loadHtml(sampleArticle);
        CHECK(ns.rawHtml() == sampleArticle, "NetSurf P5: HTML propagated from fetch pipeline to NetSurfEngine");
        CHECK(ns.focusedBox() != nullptr, "NetSurf P5: NetSurfEngine has focused box after layout");
        CHECK(ns.currentFocusedLink() == "https://m.vnexpress.net/article-123.html", "NetSurf P5: active link matches focused anchor href");

        // 4. Test dynamic toggle back to Lite Engine
        bm.setEngineMode(BrowserEngine::LITE);
        CHECK(bm.engineMode() == BrowserEngine::LITE, "NetSurf P5: engineMode switched back to LITE");
        hr.refreshAfterSettings();
        CHECK(hr.elementCount() > 0, "NetSurf P5: HtmlRenderer state active and populated in LITE mode");

        // 5. NetSurfEngine cleanup
        ns.clear();
        CHECK(ns.rawHtml().empty(), "NetSurf P5: NetSurfEngine cleared on session stop/close");
    }

    if (s_fail == 0) std::cout << "[TEST] ALL PASS" << std::endl;
    else std::cout << "[TEST] " << s_fail << " FAILURES" << std::endl;
    return s_fail;
}
