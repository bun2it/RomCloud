#include "PortalBrowser.h"
#include "../logging/Logger.h"
#include "../network/HttpClient.h"

#include <Ultralight/Ultralight.h>
#include <Ultralight/Renderer.h>
#include <Ultralight/platform/Platform.h>
#include <Ultralight/platform/Config.h>
#include <Ultralight/platform/FontLoader.h>
#include <Ultralight/platform/FileSystem.h>
#include <Ultralight/platform/Surface.h>
#include <Ultralight/platform/Logger.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_map>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace RomCloud {
namespace {

struct Impl {
    ultralight::RefPtr<ultralight::Renderer> renderer;
    ultralight::RefPtr<ultralight::View> view;
    std::string fontPath;
};

class DiskFS : public ultralight::FileSystem {
  public:
    bool FileExists(const ultralight::String &p) override {
        std::string s = p.utf8().data();
        return access(s.c_str(), R_OK) == 0;
    }
    ultralight::String GetFileMimeType(const ultralight::String &) override {
        return "application/octet-stream";
    }
    ultralight::String GetFileCharset(const ultralight::String &) override {
        return "utf-8";
    }
    ultralight::RefPtr<ultralight::Buffer> OpenFile(const ultralight::String &p) override {
        std::string s = p.utf8().data();
        FILE *f = fopen(s.c_str(), "rb");
        if (!f) return nullptr;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n <= 0 || n > 64 * 1024 * 1024) {
            fclose(f);
            return nullptr;
        }
        std::string data;
        data.resize(n);
        size_t got = fread(&data[0], 1, n, f);
        fclose(f);
        if ((long)got != n) return nullptr;
        return ultralight::Buffer::CreateFromCopy(data.data(), data.size());
    }
};

class OneFont : public ultralight::FontLoader {
  public:
    explicit OneFont(const std::string &path) : m_path(path) {}
    ultralight::String fallback_font() const override { return "sans-serif"; }
    ultralight::String fallback_font_for_characters(const ultralight::String &,
                                                    int, bool) const override {
        return "sans-serif";
    }
    ultralight::RefPtr<ultralight::FontFile> Load(const ultralight::String &, int,
                                                  bool) override {
        return ultralight::FontFile::Create(m_path.c_str());
    }
  private:
    std::string m_path;
};

class NullLogger : public ultralight::Logger {
  public:
    void LogMessage(ultralight::LogLevel, const ultralight::String &) override {}
};

class ConsoleCap : public ultralight::ViewListener {
  public:
    ConsoleCap(std::vector<PortalConsoleMsg> *out, std::string *nav, std::string *els)
        : m_out(out), m_nav(nav), m_els(els) {}
    void OnAddConsoleMessage(ultralight::View *,
                             const ultralight::ConsoleMessage &m) override {
        std::string t = m.message().utf8().data();
        if (m_out) m_out->push_back({t});
        if (m_els && t.rfind("ELS:", 0) == 0) *m_els = t.substr(4);
    }
    void OnChangeURL(ultralight::View *, const ultralight::String &url) override {
        if (m_nav) *m_nav = url.utf8().data();
    }
  private:
    std::vector<PortalConsoleMsg> *m_out;
    std::string *m_nav;
    std::string *m_els;
};

} // namespace

PortalBrowser& PortalBrowser::instance() {
    static PortalBrowser inst;
    return inst;
}

bool PortalBrowser::init(const std::string& resDir, const std::string& fontPath,
                         int width, int height) {
    if (m_ready) return true;
    Impl* im = new Impl();
    ultralight::Config config;
    config.resource_path_prefix = resDir.c_str();
    config.cache_path = (resDir + "../ultest-cache/").c_str();
    auto& platform = ultralight::Platform::instance();
    platform.set_config(config);
    static DiskFS fs;
    static NullLogger logger;
    platform.set_logger(&logger);
    platform.set_file_system(&fs);
    // OneFont phải sống suốt engine: giữ trong Impl.
    im->fontPath = fontPath;
    // NOTE: set_font_loader cần con trỏ sống lâu — dùng static theo fontPath.
    static OneFont* fonts = nullptr;
    delete fonts;
    fonts = new OneFont(fontPath);
    platform.set_font_loader(fonts);

    im->renderer = ultralight::Renderer::Create();
    if (!im->renderer) {
        delete im;
        Logger::error("PortalBrowser: Renderer::Create failed");
        return false;
    }
    ultralight::ViewConfig cfg;
    cfg.is_accelerated = false;
    im->view = im->renderer->CreateView(width, height, cfg, nullptr);
    if (!im->view) {
        delete im;
        Logger::error("PortalBrowser: CreateView failed");
        return false;
    }
    static ConsoleCap* cap = nullptr;
    delete cap;
    cap = new ConsoleCap(&m_console, &m_pendingNav, &m_lastEls);
    im->view->set_view_listener(cap);

    m_impl = im;
    m_w = width;
    m_h = height;
    m_ready = true;
    Logger::info("PortalBrowser: init ok");
    return true;
}

void PortalBrowser::shutdown() {
    Impl* im = static_cast<Impl*>(m_impl);
    if (!im) return;
    if (im->view) im->view->set_view_listener(nullptr);
    im->view = nullptr;
    im->renderer = nullptr;
    delete im;
    m_impl = nullptr;
    m_console.clear();
    m_url.clear();
    m_ready = false;
    Logger::info("PortalBrowser: shutdown, memory released");
}

void PortalBrowser::storeCookies(const std::string& line) {
    // "Set-Cookie: name=value; Path=/; ..." (1 cookie/dòng trong map).
    size_t c = line.find(':');
    std::string v = (c == std::string::npos) ? line : line.substr(c + 1);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
    size_t sc = v.find(';');
    std::string pair = v.substr(0, sc);
    size_t eq = pair.find('=');
    if (eq == std::string::npos) return;
    std::string name = pair.substr(0, eq);
    // Thay cookie cùng tên.
    std::string jar;
    bool replaced = false;
    size_t s = 0;
    while (s < m_cookieJar.size()) {
        size_t e = m_cookieJar.find("; ", s);
        std::string kv = m_cookieJar.substr(s, e == std::string::npos ? e : e - s);
        size_t ke = kv.find('=');
        if (ke != std::string::npos && kv.substr(0, ke) == name) {
            kv = pair;
            replaced = true;
        }
        if (!jar.empty()) jar += "; ";
        jar += kv;
        if (e == std::string::npos) break;
        s = e + 2;
    }
    if (!replaced) {
        if (!jar.empty()) jar += "; ";
        jar += pair;
    }
    m_cookieJar = jar;
}

std::string PortalBrowser::cookieHeader() const {
    return m_cookieJar;
}

std::string PortalBrowser::pollNav() {
    std::string u = m_pendingNav;
    m_pendingNav.clear();
    if (u.empty() || u == "about:blank" || u == m_url) return "";
    return u;
}

std::string PortalBrowser::resolveUrl(const std::string& base, const std::string& ref) {
    if (ref.empty()) return "";
    if (ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0) return ref;
    if (ref.rfind("data:", 0) == 0 || ref.rfind("javascript:", 0) == 0 ||
        ref.rfind("mailto:", 0) == 0 || ref[0] == '#')
        return "";
    size_t scheme = base.find("://");
    if (scheme == std::string::npos) return "";
    std::string origin = base.substr(0, base.find('/', scheme + 3));
    if (ref.rfind("//", 0) == 0) return base.substr(0, scheme) + ":" + ref;
    if (ref[0] == '/') return origin + ref;
    std::string dir = base;
    size_t q = dir.find('?');
    if (q != std::string::npos) dir = dir.substr(0, q);
    size_t s = dir.rfind('/');
    if (s != std::string::npos && s > scheme + 2) dir = dir.substr(0, s + 1);
    else dir = origin + "/";
    return dir + ref;
}

std::string PortalBrowser::fetchPage(const std::string& url, std::string& outFinalUrl) {
    std::vector<std::string> headers;
    if (!m_cookieJar.empty()) headers.push_back("Cookie: " + m_cookieJar);
    HttpResponse r = HttpClient::instance().get(url, headers, 20);
    if (!r.success || r.body.empty()) return "";
    auto it = r.headers.find("Set-Cookie");
    if (it != r.headers.end()) storeCookies(it->second);
    outFinalUrl = r.effectiveUrl.empty() ? url : r.effectiveUrl;
    return r.body;
}

static std::string base64enc(const std::string& in) {
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < in.size(); i += 3) {
        uint32_t n = (uint8_t)in[i] << 16;
        int pad = 0;
        if (i + 1 < in.size()) n |= (uint8_t)in[i + 1] << 8;
        else pad++;
        if (i + 2 < in.size()) n |= (uint8_t)in[i + 2];
        else pad++;
        o += b64[(n >> 18) & 63];
        o += b64[(n >> 12) & 63];
        o += pad > 1 ? '=' : b64[(n >> 6) & 63];
        o += pad > 0 ? '=' : b64[n & 63];
    }
    return o;
}

std::string PortalBrowser::inlineResources(const std::string& html, const std::string& baseUrl) {
    std::string out = html;
    int fetched = 0;
    const int kMaxFiles = 12;
    // <base> để URL còn sót tự nhất quán (engine không tải được nhưng JS thấy đúng).
    {
        size_t hp = out.find("<head");
        if (hp != std::string::npos) {
            size_t he = out.find('>', hp);
            if (he != std::string::npos)
                out.insert(he + 1, "<base href=\"" + baseUrl + "\">");
        }
    }
    auto fetchBin = [&](const std::string& u, size_t cap) -> std::string {
        if (fetched >= kMaxFiles || u.empty()) return "";
        std::vector<std::string> headers;
        if (!m_cookieJar.empty()) headers.push_back("Cookie: " + m_cookieJar);
        HttpResponse r = HttpClient::instance().get(u, headers, 15);
        if (!r.success || r.body.empty() || r.body.size() > cap) return "";
        auto it = r.headers.find("Set-Cookie");
        if (it != r.headers.end()) storeCookies(it->second);
        fetched++;
        return r.body;
    };
    // CSS: <link rel=stylesheet href>
    {
        size_t pos = 0;
        while (fetched < kMaxFiles) {
            size_t lt = out.find("<link", pos);
            if (lt == std::string::npos) break;
            size_t ge = out.find('>', lt);
            if (ge == std::string::npos) break;
            std::string tag = out.substr(lt, ge - lt + 1);
            std::string low = tag;
            for (auto& c : low) c = (char)tolower((unsigned char)c);
            if (low.find("stylesheet") == std::string::npos) { pos = ge + 1; continue; }
            std::string href;
            {
                size_t hp = low.find("href");
                if (hp != std::string::npos) {
                    size_t q0 = tag.find_first_of("\"'", hp);
                    if (q0 != std::string::npos) {
                        size_t q1 = tag.find(tag[q0], q0 + 1);
                        if (q1 != std::string::npos) href = tag.substr(q0 + 1, q1 - q0 - 1);
                    }
                }
            }
            std::string css = fetchBin(resolveUrl(baseUrl, href), 256 * 1024);
            if (css.empty()) { pos = ge + 1; continue; }
            out.replace(lt, ge - lt + 1, "<style>" + css + "</style>");
            pos = lt + 7;
        }
    }
    // JS: <script src=...></script> (không body).
    {
        size_t pos = 0;
        while (fetched < kMaxFiles) {
            size_t st = out.find("<script", pos);
            if (st == std::string::npos) break;
            size_t ge = out.find('>', st);
            if (ge == std::string::npos) break;
            std::string tag = out.substr(st, ge - st + 1);
            size_t ce = out.find("</script>", ge);
            if (ce == std::string::npos) break;
            std::string inner = out.substr(ge + 1, ce - ge - 1);
            bool blank = true;
            for (char c : inner) {
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n') { blank = false; break; }
            }
            if (!blank) { pos = ce + 9; continue; }
            std::string low = tag;
            for (auto& c : low) c = (char)tolower((unsigned char)c);
            std::string src;
            {
                size_t hp = low.find("src");
                if (hp != std::string::npos) {
                    size_t q0 = tag.find_first_of("\"'", hp);
                    if (q0 != std::string::npos) {
                        size_t q1 = tag.find(tag[q0], q0 + 1);
                        if (q1 != std::string::npos) src = tag.substr(q0 + 1, q1 - q0 - 1);
                    }
                }
            }
            std::string js = fetchBin(resolveUrl(baseUrl, src), 512 * 1024);
            if (js.empty()) { pos = ce + 9; continue; }
            out.replace(st, ce + 9 - st, "<script>" + js + "</script>");
            pos = st + 8;
        }
    }
    // IMG: <img src> -> data URI (portal nhẹ).
    {
        size_t pos = 0;
        while (fetched < kMaxFiles) {
            size_t it = out.find("<img", pos);
            if (it == std::string::npos) break;
            size_t ge = out.find('>', it);
            if (ge == std::string::npos) break;
            std::string tag = out.substr(it, ge - it + 1);
            std::string low = tag;
            for (auto& c : low) c = (char)tolower((unsigned char)c);
            std::string src;
            {
                size_t hp = low.find("src");
                if (hp != std::string::npos) {
                    size_t q0 = tag.find_first_of("\"'", hp);
                    if (q0 != std::string::npos) {
                        size_t q1 = tag.find(tag[q0], q0 + 1);
                        if (q1 != std::string::npos) src = tag.substr(q0 + 1, q1 - q0 - 1);
                    }
                }
            }
            std::string u = resolveUrl(baseUrl, src);
            std::string mime = "image/png";
            {
                std::string lu = u;
                for (auto& c : lu) c = (char)tolower((unsigned char)c);
                if (lu.find(".jpg") != std::string::npos || lu.find(".jpeg") != std::string::npos)
                    mime = "image/jpeg";
                else if (lu.find(".gif") != std::string::npos) mime = "image/gif";
                else if (lu.find(".svg") != std::string::npos) mime = "image/svg+xml";
            }
            std::string bin = fetchBin(u, 300 * 1024);
            if (bin.empty()) { pos = ge + 1; continue; }
            std::string rep = tag;
            {
                size_t hp = low.find("src");
                size_t q0 = tag.find_first_of("\"'", hp);
                size_t q1 = tag.find(tag[q0], q0 + 1);
                rep.replace(q0 + 1, q1 - q0 - 1, "data:" + mime + ";base64," + base64enc(bin));
            }
            out.replace(it, ge - it + 1, rep);
            pos = it + 4;
        }
    }
    // Bridge click/form: báo về console để app fetch tiếp (engine không mạng).
    {
        static const char* bridge =
            "<script>(function(){"
            "window.addEventListener('load',function(){var s=[];"
            "var els=document.querySelectorAll('a,button,input,select,textarea');"
            "for(var i=0;i<els.length;i++){var r=els[i].getBoundingClientRect();"
            "s.push(i+':'+els[i].tagName+'|'+(els[i].type||'')+'|'+(els[i].name||'')+' @'+Math.round(r.x)+','+Math.round(r.y)+','+Math.round(r.width)+'x'+Math.round(r.height));}"
            "console.log('ELS:'+s.join(' | '));});"
            "var __elsT=null;"
            "new MutationObserver(function(){if(__elsT)return;"
            "__elsT=setTimeout(function(){__elsT=null;var s=[];"
            "var els=document.querySelectorAll('a,button,input,select,textarea');"
            "for(var i=0;i<els.length;i++){var r=els[i].getBoundingClientRect();"
            "s.push(i+':'+els[i].tagName+'|'+(els[i].type||'')+'|'+(els[i].name||'')+' @'+Math.round(r.x)+','+Math.round(r.y)+','+Math.round(r.width)+'x'+Math.round(r.height));}"
            "console.log('ELS:'+s.join(' | '));},800);}).observe(document.documentElement,{childList:true,subtree:true});"
            "document.addEventListener('focusin',function(e){"
            "var t=e.target;console.log('FOCUS:'+t.tagName+' '+(t.name||t.type||''));});"
            "document.addEventListener('click',function(e){"
            "var a=e.target.closest?e.target.closest('a'):null;"
            "if(a&&a.href){console.log('PNAV:'+a.href);"
            "e.preventDefault();e.stopPropagation();}},true);"
            "document.addEventListener('submit',function(e){"
            "var f=e.target;if(!f||!f.action)return;var q=[];"
            "for(var i=0;i<f.elements.length;i++){var el=f.elements[i];"
            "if(!el.name||el.disabled)continue;"
            "var t=(el.type||'text').toLowerCase();"
            "if(t=='submit'||t=='button'||t=='image'||t=='file')continue;"
            "if((t=='checkbox'||t=='radio')&&!el.checked)continue;"
            "q.push(encodeURIComponent(el.name)+\"=\"+encodeURIComponent(el.value));}"
            "console.log('PFORM:'+(f.method||'get').toUpperCase()+'|'+f.action+'|'+q.join('&'));"
            "e.preventDefault();e.stopPropagation();},true);})();</script>";
        std::string low = out;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        size_t bp = low.rfind("</body>");
        if (bp != std::string::npos) out.insert(bp, bridge);
        else out += bridge;
    }
    return out;
}

bool PortalBrowser::navigate(const std::string& url) {
    if (!m_ready) return false;
    std::string finalUrl;
    std::string body = fetchPage(url, finalUrl);
    if (body.empty()) {
        Logger::warn("PortalBrowser: fetch failed: " + url);
        return false;
    }
    return showHtml(body, finalUrl);
}

bool PortalBrowser::fetchUrl(const std::string& url, std::string& outBody,
                             std::string& outFinalUrl) {
    outBody = fetchPage(url, outFinalUrl);
    return !outBody.empty();
}

bool PortalBrowser::showHtml(const std::string& body, const std::string& url) {
    if (!m_ready) return false;
    Impl* im = static_cast<Impl*>(m_impl);
    m_url = url;
    m_pendingNav.clear();
    pushHistory(url);
    pushHistory(url); // mọi trang hiện lên đều vào lịch sử (trừ trùng)
    m_lastEls.clear();
    m_elsCache.clear();
    m_elsParsedFrom.clear();
    m_sel = -1;
    // SIMPLETEST: body "1" -> trang tối giản (phân biệt lỗi content/môi trường).
    std::string useBody = body;
    if (useBody == "1") {
        useBody = "<html><body style='background:#16212e;color:#fff'>"
                  "<h1>SIMPLE OK</h1></body></html>";
    }
    size_t inlinedLen = 0;
    {
        std::string inlined = inlineResources(body, url);
        inlinedLen = inlined.size();
        im->view->LoadHTML(inlined.c_str(), url.c_str());
    }
    im->view->Focus();
    im->view->set_needs_paint(true); // ép full repaint (nghi dirty-tracking)
    // Pump tới khi paint xong (giới hạn, thoát khi ổn định 30 frame).
    int lastPaint = -1, iters = 0;
    for (int i = 0; i < 400; ++i) {
        iters = i;
        im->renderer->Update();
        im->renderer->Render();
        if (im->view->needs_paint()) lastPaint = i;
        if (i > 10 && lastPaint >= 0 && i - lastPaint > 30) break;
    }
    {
        char lb[160];
        std::snprintf(lb, sizeof(lb),
                      "PortalBrowser: show body=%zu inlined=%zu iters=%d lastPaint=%d",
                      body.size(), inlinedLen, iters, lastPaint);
        Logger::info(lb);
    }
    m_pendingNav.clear(); // bỏ nav do chính LoadHTML gây ra
    return true;
}

bool PortalBrowser::submitFetch(const std::string& method, const std::string& action,
                                const std::string& query, std::string& outBody,
                                std::string& outFinal) {
    std::string m = method;
    for (auto& c : m) c = (char)toupper((unsigned char)c);
    std::vector<std::string> headers;
    if (!m_cookieJar.empty()) headers.push_back("Cookie: " + m_cookieJar);
    HttpResponse r;
    if (m == "POST") {
        std::unordered_map<std::string, std::string> fm;
        size_t s = 0;
        while (s < query.size()) {
            size_t e = query.find('&', s);
            std::string kv = query.substr(s, e == std::string::npos ? e : e - s);
            size_t eq = kv.find('=');
            if (eq != std::string::npos) fm[kv.substr(0, eq)] = kv.substr(eq + 1);
            if (e == std::string::npos) break;
            s = e + 1;
        }
        r = HttpClient::instance().postForm(action, fm, headers, 20);
    } else {
        std::string u = action + (action.find('?') == std::string::npos ? "?" : "&") + query;
        r = HttpClient::instance().get(u, headers, 20);
    }
    if (!r.success) {
        Logger::warn("PortalBrowser: submit failed: " + action);
        return false;
    }
    auto it = r.headers.find("Set-Cookie");
    if (it != r.headers.end()) storeCookies(it->second);
    outFinal = r.effectiveUrl.empty() ? action : r.effectiveUrl;
    outBody = r.body;
    return true;
}

bool PortalBrowser::update() {
    if (!m_ready) return false;
    Impl* im = static_cast<Impl*>(m_impl);
    im->renderer->Update();
    im->renderer->Render();
    bool dirty = im->view->needs_paint();
    if (dirty) im->view->set_needs_paint(false);
    return dirty;
}

bool PortalBrowser::copyPixels(unsigned char* dst, int& outW, int& outH) {
    if (!m_ready || !dst) return false;
    Impl* im = static_cast<Impl*>(m_impl);
    ultralight::Surface* surf = im->view->surface();
    if (!surf) {
        Logger::warn("PortalBrowser: surface null");
        return false;
    }
    ultralight::BitmapSurface* bs = static_cast<ultralight::BitmapSurface*>(surf);
    ultralight::RefPtr<ultralight::Bitmap> bmp = bs->bitmap();
    if (!bmp) {
        Logger::warn("PortalBrowser: bitmap null");
        return false;
    }
    outW = (int)bmp->width();
    outH = (int)bmp->height();
    if (outW != m_w || outH != m_h) {
        char lb[96];
        std::snprintf(lb, sizeof(lb), "PortalBrowser: bitmap %dx%d != view %dx%d",
                      outW, outH, m_w, m_h);
        Logger::warn(lb);
        return false;
    }
    memcpy(dst, bmp->LockPixels(), (size_t)outW * outH * 4);
    bmp->UnlockPixels();
    return true;
}

static void fireKey(ultralight::View* view, int vk) {
    ultralight::KeyEvent down;
    down.type = ultralight::KeyEvent::kType_RawKeyDown;
    down.virtual_key_code = vk;
    down.native_key_code = 0;
    view->FireKeyEvent(down);
    ultralight::KeyEvent up;
    up.type = ultralight::KeyEvent::kType_KeyUp;
    up.virtual_key_code = vk;
    view->FireKeyEvent(up);
}

void PortalBrowser::pressTab(bool shift) {
    if (!m_ready) return;
    Impl* im = static_cast<Impl*>(m_impl);
    if (shift) {
        // Shift+Tab: gửi Shift down, Tab, Shift up (tối giản: chỉ Tab).
    }
    fireKey(im->view.get(), 0x09);
}

void PortalBrowser::pressEnter() {
    if (!m_ready) return;
    fireKey(static_cast<Impl*>(m_impl)->view.get(), 0x0D);
}

void PortalBrowser::pressArrow(int dx, int dy) {
    if (!m_ready) return;
    int vk = 0;
    if (dx < 0) vk = 0x25;
    else if (dx > 0) vk = 0x27;
    else if (dy < 0) vk = 0x26;
    else if (dy > 0) vk = 0x28;
    if (vk) fireKey(static_cast<Impl*>(m_impl)->view.get(), vk);
}

void PortalBrowser::clickAt(int x, int y) {
    if (!m_ready) return;
    Impl* im = static_cast<Impl*>(m_impl);
    ultralight::MouseEvent mv;
    mv.type = ultralight::MouseEvent::kType_MouseMoved;
    mv.x = x;
    mv.y = y;
    mv.button = ultralight::MouseEvent::kButton_None;
    im->view->FireMouseEvent(mv);
    ultralight::MouseEvent dn;
    dn.type = ultralight::MouseEvent::kType_MouseDown;
    dn.x = x;
    dn.y = y;
    dn.button = ultralight::MouseEvent::kButton_Left;
    im->view->FireMouseEvent(dn);
    ultralight::MouseEvent up;
    up.type = ultralight::MouseEvent::kType_MouseUp;
    up.x = x;
    up.y = y;
    up.button = ultralight::MouseEvent::kButton_Left;
    im->view->FireMouseEvent(up);
}

void PortalBrowser::typeText(const std::string& utf8) {
    if (!m_ready) return;
    Impl* im = static_cast<Impl*>(m_impl);
    // Gõ từng codepoint qua kType_Char (UTF-8 -> UTF-16, kèm surrogate).
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned char c = utf8[i];
        uint32_t cp = 0;
        size_t len = 1;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            cp = ((c & 0x1F) << 6) | (utf8[i + 1] & 0x3F); len = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
            cp = ((c & 0x0F) << 12) | ((utf8[i + 1] & 0x3F) << 6) | (utf8[i + 2] & 0x3F);
            len = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < utf8.size()) {
            cp = ((c & 0x07) << 18) | ((utf8[i + 1] & 0x3F) << 12) |
                 ((utf8[i + 2] & 0x3F) << 6) | (utf8[i + 3] & 0x3F);
            len = 4;
        } else { i++; continue; }
        i += len;
        ultralight::Char16 u16[2];
        size_t ulen = 0;
        if (cp < 0x10000) { u16[0] = (ultralight::Char16)cp; ulen = 1; }
        else {
            cp -= 0x10000;
            u16[0] = (ultralight::Char16)(0xD800 + (cp >> 10));
            u16[1] = (ultralight::Char16)(0xDC00 + (cp & 0x3FF));
            ulen = 2;
        }
        ultralight::KeyEvent ev;
        ev.type = ultralight::KeyEvent::kType_Char;
        ev.text = ultralight::String(ultralight::String16(u16, ulen));
        im->view->FireKeyEvent(ev);
    }
}

void PortalBrowser::pushHistory(const std::string& url) {
    if (url.empty()) return;
    if (m_histIdx >= 0 && m_histIdx < (int)m_hist.size() && m_hist[m_histIdx] == url)
        return;
    // Cắt nhánh forward khi rẽ hướng mới.
    if (m_histIdx + 1 < (int)m_hist.size())
        m_hist.erase(m_hist.begin() + m_histIdx + 1, m_hist.end());
    m_hist.push_back(url);
    if (m_hist.size() > 50) m_hist.erase(m_hist.begin());
    m_histIdx = (int)m_hist.size() - 1;
}

bool PortalBrowser::historyGo(int delta, std::string& outUrl) {
    int ni = m_histIdx + delta;
    if (ni < 0 || ni >= (int)m_hist.size()) return false;
    m_histIdx = ni;
    outUrl = m_hist[ni];
    return true;
}

void PortalBrowser::clearHistory() {
    m_hist.clear();
    m_histIdx = -1;
}

std::vector<PortalConsoleMsg> PortalBrowser::drainConsole() {
    std::vector<PortalConsoleMsg> o;
    o.swap(m_console);
    return o;
}

const std::vector<PortalEl>& PortalBrowser::elements() {
    if (m_lastEls != m_elsParsedFrom) {
        m_elsParsedFrom = m_lastEls;
        m_elsCache.clear();
        m_sel = -1;
        // "i:TAG|type|name @x,y,wxh | ..."
        size_t s = 0;
        while (s < m_lastEls.size()) {
            size_t e = m_lastEls.find(" | ", s);
            std::string el = m_lastEls.substr(s, e == std::string::npos ? e : e - s);
            size_t at = el.find('@');
            size_t c1 = el.find(':');
            size_t p1 = el.find('|');
            if (at != std::string::npos && c1 != std::string::npos && p1 != std::string::npos && p1 < at) {
                size_t p2 = el.find('|', p1 + 1);
                PortalEl pe;
                pe.tag = el.substr(c1 + 1, p1 - c1 - 1);
                if (p2 != std::string::npos && p2 < at) {
                    pe.type = el.substr(p1 + 1, p2 - p1 - 1);
                    pe.name = el.substr(p2 + 1, at - p2 - 1);
                } else {
                    pe.name = el.substr(p1 + 1, at - p1 - 1);
                }
                int x = 0, y = 0, w = 0, h = 0;
                if (sscanf(el.c_str() + at + 1, "%d,%d,%dx%d", &x, &y, &w, &h) >= 2 && w > 0 && h > 0) {
                    pe.x = x; pe.y = y; pe.w = w; pe.h = h;
                    // Bỏ ô ẩn (0x0) và hidden inputs.
                    if (pe.type != "hidden") m_elsCache.push_back(pe);
                }
            }
            if (e == std::string::npos) break;
            s = e + 3;
        }
    }
    return m_elsCache;
}

void PortalBrowser::selectFirst() {
    elements();
    m_sel = m_elsCache.empty() ? -1 : 0;
}

bool PortalBrowser::moveSel(int dx, int dy) {
    elements();
    if (m_elsCache.empty()) return false;
    if (m_sel < 0 || m_sel >= (int)m_elsCache.size()) {
        m_sel = 0;
        return true;
    }
    const PortalEl& cur = m_elsCache[m_sel];
    int fx = cur.x + cur.w / 2, fy = cur.y + cur.h / 2;
    int best = -1, bestScore = 1 << 30;
    for (size_t i = 0; i < m_elsCache.size(); ++i) {
        if ((int)i == m_sel) continue;
        const PortalEl& c = m_elsCache[i];
        int nx = c.x + c.w / 2 - fx, ny = c.y + c.h / 2 - fy;
        if (dx > 0 && nx < 8) continue;
        if (dx < 0 && nx > -8) continue;
        if (dy > 0 && ny < 8) continue;
        if (dy < 0 && ny > -8) continue;
        int primary = dx != 0 ? abs(nx) : abs(ny);
        int second = dx != 0 ? abs(ny) : abs(nx);
        int score = primary + second * 2;
        if (score < bestScore) { bestScore = score; best = (int)i; }
    }
    if (best < 0) return false;
    m_sel = best;
    return true;
}

bool PortalBrowser::clickSelected() {
    elements();
    if (m_sel < 0 || m_sel >= (int)m_elsCache.size()) return false;
    const PortalEl& c = m_elsCache[m_sel];
    clickAt(c.x + c.w / 2, c.y + c.h / 2);
    return true;
}

void PortalBrowser::scrollBy(int dx, int dy) {
    if (!m_ready) return;
    Impl* im = static_cast<Impl*>(m_impl);
    ultralight::ScrollEvent ev;
    ev.type = ultralight::ScrollEvent::kType_ScrollByPixel;
    ev.delta_x = dx * 120;
    ev.delta_y = dy * 120;
    im->view->FireScrollEvent(ev);
}

} // namespace RomCloud
