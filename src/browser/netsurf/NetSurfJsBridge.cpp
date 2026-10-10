// NetSurfJsBridge — Duktape bridge for NetSurfEngine (see header).
//
// Stack conventions (mirror src/browser/JsEngine.cpp):
//   - The NetSurfEngine* + epoch travel in the global stash ("\xff" "eng").
//   - Element proxies stash RenderBox* under "\xff" "el" and epoch under
//     "\xff" "ep". Every access revalidates the epoch; stale proxies
//     behave as null.
//   - The style proxy stashes the RenderBox* under "\xff" "el" so its
//     display getter/setter can find the box.
//
// Safety:
//   - All script bodies run inside duk_peval_string, errors logged.
//   - A longjmp fatal handler catches Duktape abort()s without crashing
//     the process.
//   - DOM mutations set m_dirty; the engine re-layouts on the next poll.
#include "NetSurfJsBridge.h"
#include "NetSurfLayout.h"
#include "NetSurfEngine.h"
#include "../../logging/Logger.h"
#include "../../network/HttpClient.h"
#include "../BrowserManager.h"

#include <algorithm>
#include <cctype>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sstream>

namespace RomCloud {

static jmp_buf s_fatalJmp;
static bool s_hasFatalJmp = false;

static void jsFatalHandler(void* udata, const char* msg) {
    (void)udata;
    Logger::error(std::string("NetSurfJsBridge FATAL: ") + (msg ? msg : "unknown"));
    if (s_hasFatalJmp) longjmp(s_fatalJmp, 1);
}

static std::string toLowerStr(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static std::string trimStr(std::string s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

NetSurfJsBridge* NetSurfJsBridge::engineOf(duk_context* ctx) {
    duk_push_global_stash(ctx);
    duk_get_prop_string(ctx, -1, "\xff" "eng");
    NetSurfJsBridge* br = static_cast<NetSurfJsBridge*>(duk_get_pointer(ctx, -1));
    duk_pop_2(ctx);
    return br;
}

RenderBox* NetSurfJsBridge::findById(RenderBox* root, const std::string& id) {
    if (!root || id.empty()) return nullptr;
    if (root->id == id) return root;
    for (auto& ch : root->children) {
        if (RenderBox* f = findById(ch.get(), id)) return f;
    }
    return nullptr;
}

RenderBox* NetSurfJsBridge::findFirst(RenderBox* root, const std::string& sel) {
    if (!root || sel.empty()) return nullptr;
    std::string tag, cls;
    size_t dot = sel.find('.');
    if (dot == std::string::npos) {
        tag = toLowerStr(sel);
    } else {
        tag = toLowerStr(sel.substr(0, dot));
        cls = sel.substr(dot + 1);
    }
    std::function<RenderBox*(RenderBox*)> walk = [&](RenderBox* b) -> RenderBox* {
        if (!b) return nullptr;
        bool ok = true;
        if (!tag.empty() && toLowerStr(b->tagName) != tag) ok = false;
        if (ok && !cls.empty()) {
            std::istringstream iss(b->className);
            std::string tok;
            bool found = false;
            while (iss >> tok) { if (tok == cls) { found = true; break; } }
            if (!found) ok = false;
        }
        if (ok) return b;
        for (auto& ch : b->children) {
            if (RenderBox* f = walk(ch.get())) return f;
        }
        return nullptr;
    };
    return walk(root);
}

NetSurfJsBridge::NetSurfJsBridge(NetSurfEngine* engine, uint64_t epoch,
                                 const std::string& pageUrl)
    : m_engine(engine), m_epoch(epoch), m_pageUrl(pageUrl) {
    m_ctx = duk_create_heap(nullptr, nullptr, nullptr, nullptr, jsFatalHandler);
    if (!m_ctx) {
        Logger::error("NetSurfJsBridge: duk_create_heap failed");
        return;
    }
    registerGlobals();
}

NetSurfJsBridge::~NetSurfJsBridge() {
    if (m_ctx) {
        duk_destroy_heap(m_ctx);
        m_ctx = nullptr;
    }
}

void NetSurfJsBridge::markDirty() { m_dirty = true; }

bool NetSurfJsBridge::runScript(const std::string& code, const std::string& name) {
    if (!m_ctx || code.empty() || code.size() > 64 * 1024) return false;
    s_hasFatalJmp = true;
    if (setjmp(s_fatalJmp) != 0) {
        s_hasFatalJmp = false;
        Logger::error("NetSurfJsBridge: safely recovered from fatal in " + name);
        return false;
    }
    if (duk_peval_string(m_ctx, code.c_str()) != 0) {
        Logger::warn(std::string("NetSurfJsBridge: ") + name + ": " +
                     duk_safe_to_string(m_ctx, -1));
        duk_pop(m_ctx);
        s_hasFatalJmp = false;
        return false;
    }
    duk_pop(m_ctx);
    s_hasFatalJmp = false;
    return true;
}

void NetSurfJsBridge::registerGlobals() {
    duk_push_global_stash(m_ctx);
    duk_push_pointer(m_ctx, this);
    duk_put_prop_string(m_ctx, -2, "\xff" "eng");
    duk_pop(m_ctx);

    duk_push_c_function(m_ctx, fnLog, DUK_VARARGS);
    duk_put_global_string(m_ctx, "netSurfConsoleLog");
    duk_eval_string(m_ctx,
        "globalThis.console = {"
        "  log: function(){var a=Array.prototype.slice.call(arguments);a.unshift('log');netSurfConsoleLog.apply(null,a);},"
        "  warn: function(){var a=Array.prototype.slice.call(arguments);a.unshift('warn');netSurfConsoleLog.apply(null,a);},"
        "  error: function(){var a=Array.prototype.slice.call(arguments);a.unshift('error');netSurfConsoleLog.apply(null,a);}"
        "};");
    duk_pop(m_ctx);

    duk_push_c_function(m_ctx, fnAlert, 1);
    duk_put_global_string(m_ctx, "alert");
    duk_push_c_function(m_ctx, fnSetTimeout, 2);
    duk_put_global_string(m_ctx, "setTimeout");
    duk_push_c_function(m_ctx, fnSetInterval, 2);
    duk_put_global_string(m_ctx, "setInterval");
    duk_push_c_function(m_ctx, fnClearTimeout, 1);
    duk_put_global_string(m_ctx, "clearTimeout");

    duk_push_object(m_ctx);
    duk_push_c_function(m_ctx, fnGetElementById, 1);
    duk_put_prop_string(m_ctx, -2, "getElementById");
    duk_push_c_function(m_ctx, fnQuerySelector, 1);
    duk_put_prop_string(m_ctx, -2, "querySelector");
    duk_push_c_function(m_ctx, fnGetBody, 0);
    duk_put_prop_string(m_ctx, -2, "getBody");
    duk_put_global_string(m_ctx, "document");

    duk_push_object(m_ctx);
    duk_push_string(m_ctx, "href");
    duk_push_c_function(m_ctx, locationGetHref, 0);
    duk_push_c_function(m_ctx, locationSetHref, 1);
    duk_def_prop(m_ctx, -4,
        DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER |
        DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_FORCE);
    duk_put_global_string(m_ctx, "location");
}

duk_ret_t NetSurfJsBridge::fnLog(duk_context* ctx) {
    if (duk_get_top(ctx) < 1) return 0;
    std::string level = duk_safe_to_string(ctx, 0);
    std::string out;
    for (duk_idx_t i = 1; i < duk_get_top(ctx); i++) {
        if (i > 1) out += " ";
        out += duk_safe_to_string(ctx, i);
    }
    if (out.size() > 300) out = out.substr(0, 300) + "...";
    if (level == "warn") Logger::warn("NetSurfJsBridge: " + out);
    else if (level == "error") Logger::error("NetSurfJsBridge: " + out);
    else Logger::info("NetSurfJsBridge: " + out);
    return 0;
}

duk_ret_t NetSurfJsBridge::fnAlert(duk_context* ctx) {
    std::string msg = duk_safe_to_string(ctx, 0);
    Logger::info("NetSurfJsBridge: alert: " + msg.substr(0, 200));
    return 0;
}

duk_ret_t NetSurfJsBridge::fnSetTimeout(duk_context* ctx) {
    NetSurfJsBridge* br = engineOf(ctx);
    if (!br || !duk_is_function(ctx, 0)) return DUK_RET_TYPE_ERROR;
    double ms = duk_is_number(ctx, 1) ? duk_get_number(ctx, 1) : 0.0;
    if (ms < 0) ms = 0;
    if (ms > 600000) ms = 600000;
    NetSurfJsTimer t;
    t.id = br->m_nextId++;
    t.dueMs = SDL_GetTicks() + (uint32_t)ms;
    {
        std::lock_guard<std::mutex> lock(br->m_asyncMutex);
        if (br->m_timers.size() >= 64) {
            duk_push_int(ctx, 0); return 1;
        }
        duk_dup(ctx, 0);
        std::string key = "\xff" "t" + std::to_string(t.id);
        duk_put_global_string(ctx, key.c_str());
        br->m_timers.push_back(t);
    }
    duk_push_int(ctx, t.id);
    return 1;
}

duk_ret_t NetSurfJsBridge::fnSetInterval(duk_context* ctx) {
    NetSurfJsBridge* br = engineOf(ctx);
    if (!br || !duk_is_function(ctx, 0)) return DUK_RET_TYPE_ERROR;
    double ms = duk_is_number(ctx, 1) ? duk_get_number(ctx, 1) : 0.0;
    if (ms < 50) ms = 50;
    if (ms > 600000) ms = 600000;
    NetSurfJsTimer t;
    t.id = br->m_nextId++;
    t.dueMs = SDL_GetTicks() + (uint32_t)ms;
    t.intervalMs = (uint32_t)ms;
    t.repeat = true;
    {
        std::lock_guard<std::mutex> lock(br->m_asyncMutex);
        if (br->m_timers.size() >= 64) {
            duk_push_int(ctx, 0); return 1;
        }
        duk_dup(ctx, 0);
        std::string key = "\xff" "t" + std::to_string(t.id);
        duk_put_global_string(ctx, key.c_str());
        br->m_timers.push_back(t);
    }
    duk_push_int(ctx, t.id);
    return 1;
}

duk_ret_t NetSurfJsBridge::fnClearTimeout(duk_context* ctx) {
    NetSurfJsBridge* br = engineOf(ctx);
    if (!br) return 0;
    uint32_t id = (uint32_t)duk_to_uint(ctx, 0);
    {
        std::lock_guard<std::mutex> lock(br->m_asyncMutex);
        for (auto it = br->m_timers.begin(); it != br->m_timers.end(); ++it) {
            if (it->id == id) {
                br->m_timers.erase(it);
                std::string key = "\xff" "t" + std::to_string(id);
                duk_push_global_object(ctx);
                duk_del_prop_string(ctx, -1, key.c_str());
                duk_pop(ctx);
                break;
            }
        }
    }
    return 0;
}

duk_ret_t NetSurfJsBridge::fnGetElementById(duk_context* ctx) {
    NetSurfJsBridge* br = engineOf(ctx);
    if (!br || !br->m_root) { duk_push_null(ctx); return 1; }
    std::string id = duk_safe_to_string(ctx, 0);
    RenderBox* b = findById(br->m_root, id);
    if (!b) duk_push_null(ctx); else br->pushElementProxy(b);
    return 1;
}

duk_ret_t NetSurfJsBridge::fnQuerySelector(duk_context* ctx) {
    NetSurfJsBridge* br = engineOf(ctx);
    if (!br || !br->m_root) { duk_push_null(ctx); return 1; }
    std::string sel = duk_safe_to_string(ctx, 0);
    RenderBox* b = findFirst(br->m_root, sel);
    if (!b) duk_push_null(ctx); else br->pushElementProxy(b);
    return 1;
}

duk_ret_t NetSurfJsBridge::fnGetBody(duk_context* ctx) {
    NetSurfJsBridge* br = engineOf(ctx);
    if (!br || !br->m_root) { duk_push_null(ctx); return 1; }
    br->pushElementProxy(br->m_root);
    return 1;
}

duk_ret_t NetSurfJsBridge::locationGetHref(duk_context* ctx) {
    NetSurfJsBridge* br = engineOf(ctx);
    if (!br) { duk_push_string(ctx, ""); return 1; }
    duk_push_string(ctx, br->m_pageUrl.c_str());
    return 1;
}

duk_ret_t NetSurfJsBridge::locationSetHref(duk_context* ctx) {
    NetSurfJsBridge* br = engineOf(ctx);
    if (!br) return 0;
    std::string url = duk_safe_to_string(ctx, 0);
    if (!url.empty()) {
        Logger::info("NetSurfJsBridge: location.href = " + url);
        BrowserManager::instance().openUrl(url);
    }
    return 0;
}

void NetSurfJsBridge::pushElementProxy(RenderBox* el) {
    if (!el) { duk_push_null(m_ctx); return; }
    duk_push_object(m_ctx);
    duk_push_pointer(m_ctx, el);
    duk_put_prop_string(m_ctx, -2, "\xff" "el");
    duk_push_uint(m_ctx, m_epoch);
    duk_put_prop_string(m_ctx, -2, "\xff" "ep");
    duk_push_string(m_ctx, el->tagName.c_str());
    duk_put_prop_string(m_ctx, -2, "tagName");
    duk_push_string(m_ctx, el->id.c_str());
    duk_put_prop_string(m_ctx, -2, "id");
    duk_push_string(m_ctx, el->className.c_str());
    duk_put_prop_string(m_ctx, -2, "className");
    duk_push_string(m_ctx, el->href.c_str());
    duk_put_prop_string(m_ctx, -2, "href");
    duk_push_string(m_ctx, el->inputType.c_str());
    duk_put_prop_string(m_ctx, -2, "type");
    // textContent / innerHTML / value via getter+setter
    duk_push_string(m_ctx, "textContent");
    duk_push_c_function(m_ctx, elGetText, 0);
    duk_push_c_function(m_ctx, elSetText, 1);
    duk_def_prop(m_ctx, -4,
        DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER |
        DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_FORCE);
    duk_push_string(m_ctx, "innerHTML");
    duk_push_c_function(m_ctx, elGetInnerHtml, 0);
    duk_push_c_function(m_ctx, elSetInnerHtml, 1);
    duk_def_prop(m_ctx, -4,
        DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER |
        DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_FORCE);
    duk_push_string(m_ctx, "value");
    duk_push_c_function(m_ctx, elGetValue, 0);
    duk_push_c_function(m_ctx, elSetValue, 1);
    duk_def_prop(m_ctx, -4,
        DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER |
        DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_FORCE);
    duk_push_c_function(m_ctx, elClick, 0);
    duk_put_prop_string(m_ctx, -2, "click");
    duk_push_c_function(m_ctx, elAddEventListener, 2);
    duk_put_prop_string(m_ctx, -2, "addEventListener");
    // classList
    duk_push_object(m_ctx);
    duk_push_pointer(m_ctx, el);
    duk_put_prop_string(m_ctx, -2, "\xff" "el");
    duk_push_uint(m_ctx, m_epoch);
    duk_put_prop_string(m_ctx, -2, "\xff" "ep");
    duk_push_c_function(m_ctx, classListAdd, 1);
    duk_put_prop_string(m_ctx, -2, "add");
    duk_push_c_function(m_ctx, classListRemove, 1);
    duk_put_prop_string(m_ctx, -2, "remove");
    duk_push_c_function(m_ctx, classListToggle, 1);
    duk_put_prop_string(m_ctx, -2, "toggle");
    duk_push_c_function(m_ctx, classListContains, 1);
    duk_put_prop_string(m_ctx, -2, "contains");
    duk_put_prop_string(m_ctx, -2, "classList");
    // style with display getter+setter
    duk_push_object(m_ctx);
    duk_push_pointer(m_ctx, el);
    duk_put_prop_string(m_ctx, -2, "\xff" "el");
    duk_push_uint(m_ctx, m_epoch);
    duk_put_prop_string(m_ctx, -2, "\xff" "ep");
    duk_push_string(m_ctx, "display");
    duk_push_c_function(m_ctx, styleGetDisplay, 0);
    duk_push_c_function(m_ctx, styleSetDisplay, 1);
    duk_def_prop(m_ctx, -4,
        DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER |
        DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_FORCE);
    duk_put_prop_string(m_ctx, -2, "style");
}

RenderBox* NetSurfJsBridge::checkElement(duk_context* ctx, duk_idx_t idx, uint64_t* epochOut) {
    duk_get_prop_string(ctx, idx, "\xff" "el");
    RenderBox* b = static_cast<RenderBox*>(duk_get_pointer(ctx, -1));
    duk_pop(ctx);
    duk_get_prop_string(ctx, idx, "\xff" "ep");
    uint64_t ep = (uint64_t)duk_get_uint(ctx, -1);
    duk_pop(ctx);
    if (epochOut) *epochOut = ep;
    return b;
}

static std::string boxEffectiveText(const RenderBox* b) {
    if (!b) return "";
    if (!b->lines.empty()) {
        std::string s;
        for (size_t i = 0; i < b->lines.size(); i++) {
            if (i) s += "\n";
            s += b->lines[i];
        }
        return s;
    }
    return b->textContent;
}

duk_ret_t NetSurfJsBridge::elGetText(duk_context* ctx) {
    uint64_t ep = 0;
    RenderBox* b = checkElement(ctx, 0, &ep);
    NetSurfJsBridge* br = engineOf(ctx);
    if (!b || !br || ep != br->m_epoch) { duk_push_undefined(ctx); return 1; }
    duk_push_string(ctx, boxEffectiveText(b).c_str());
    return 1;
}

duk_ret_t NetSurfJsBridge::elSetText(duk_context* ctx) {
    uint64_t ep = 0;
    RenderBox* b = checkElement(ctx, 0, &ep);
    NetSurfJsBridge* br = engineOf(ctx);
    if (!b || !br || ep != br->m_epoch) return 0;
    std::string v = duk_safe_to_string(ctx, 0);
    b->textContent = v;
    b->lines.clear();
    b->lines.push_back(v);
    b->jsEpoch++;
    br->markDirty();
    return 0;
}

duk_ret_t NetSurfJsBridge::elGetInnerHtml(duk_context* ctx) {
    return elGetText(ctx);
}

duk_ret_t NetSurfJsBridge::elSetInnerHtml(duk_context* ctx) {
    return elSetText(ctx);
}

duk_ret_t NetSurfJsBridge::elGetValue(duk_context* ctx) {
    uint64_t ep = 0;
    RenderBox* b = checkElement(ctx, 0, &ep);
    NetSurfJsBridge* br = engineOf(ctx);
    if (!b || !br || ep != br->m_epoch) { duk_push_string(ctx, ""); return 1; }
    duk_push_string(ctx, b->value.c_str());
    return 1;
}

duk_ret_t NetSurfJsBridge::elSetValue(duk_context* ctx) {
    uint64_t ep = 0;
    RenderBox* b = checkElement(ctx, 0, &ep);
    NetSurfJsBridge* br = engineOf(ctx);
    if (!b || !br || ep != br->m_epoch) return 0;
    std::string v = duk_safe_to_string(ctx, 0);
    if (b->maxLen > 0 && (int)v.size() > b->maxLen) v = v.substr(0, b->maxLen);
    b->value = v;
    b->jsEpoch++;
    br->markDirty();
    return 0;
}

duk_ret_t NetSurfJsBridge::elClick(duk_context* ctx) {
    uint64_t ep = 0;
    RenderBox* b = checkElement(ctx, 0, &ep);
    NetSurfJsBridge* br = engineOf(ctx);
    if (!b || !br || ep != br->m_epoch) return 0;
    br->fireClick(b);
    return 0;
}

duk_ret_t NetSurfJsBridge::elAddEventListener(duk_context* ctx) {
    uint64_t ep = 0;
    RenderBox* b = checkElement(ctx, 0, &ep);
    NetSurfJsBridge* br = engineOf(ctx);
    if (!b || !br || ep != br->m_epoch) return 0;
    std::string evt = toLowerStr(duk_safe_to_string(ctx, 0));
    if (!duk_is_function(ctx, 1)) return 0;
    if (evt == "click") {
        duk_dup(ctx, 1);
        void* fn = duk_get_heapptr(ctx, -1);
        b->jsClickHandlers.push_back(fn);
        duk_pop(ctx);
    } else {
        Logger::warn("NetSurfJsBridge: addEventListener('" + evt + "') unsupported");
    }
    return 0;
}

void NetSurfJsBridge::fireClick(RenderBox* thisEl) {
    if (!m_ctx || !thisEl) return;
    if (thisEl->jsClickHandlers.empty()) return;
    std::vector<void*> snap = thisEl->jsClickHandlers;
    for (void* fnPtr : snap) {
        s_hasFatalJmp = true;
        if (setjmp(s_fatalJmp) != 0) {
            s_hasFatalJmp = false;
            Logger::error("NetSurfJsBridge: fatal in click handler");
            duk_pop(m_ctx);
            continue;
        }
        // Stack: [proxy, fn] -> duk_pcall_method(ctx, 1) calls fn with `this`=proxy.
        pushElementProxy(thisEl);            // -2: proxy
        duk_push_heapptr(m_ctx, fnPtr);     // -1: fn
        duk_insert(m_ctx, -2);              // [fn, proxy]
        int rc = duk_pcall_method(m_ctx, 1);
        if (rc != 0) {
            Logger::warn(std::string("NetSurfJsBridge: click handler: ") +
                         duk_safe_to_string(m_ctx, -1));
            duk_pop(m_ctx);
        } else {
            duk_pop(m_ctx);
        }
        s_hasFatalJmp = false;
    }
}

static bool classListHas(const RenderBox* b, const std::string& cls) {
    if (cls.empty()) return false;
    std::istringstream iss(b->className);
    std::string tok;
    while (iss >> tok) if (tok == cls) return true;
    return false;
}

static void classListAddImpl(RenderBox* b, const std::string& cls) {
    if (cls.empty() || classListHas(b, cls)) return;
    if (!b->className.empty()) b->className += " ";
    b->className += cls;
}

static void classListRemoveImpl(RenderBox* b, const std::string& cls) {
    if (cls.empty() || b->className.empty()) return;
    std::istringstream iss(b->className);
    std::string tok, out;
    while (iss >> tok) {
        if (tok == cls) continue;
        if (!out.empty()) out += " ";
        out += tok;
    }
    b->className = out;
}

duk_ret_t NetSurfJsBridge::elGetClass(duk_context* ctx) {
    uint64_t ep = 0;
    RenderBox* b = checkElement(ctx, 0, &ep);
    NetSurfJsBridge* br = engineOf(ctx);
    if (!b || !br || ep != br->m_epoch) { duk_push_string(ctx, ""); return 1; }
    duk_push_string(ctx, b->className.c_str());
    return 1;
}

duk_ret_t NetSurfJsBridge::classListAdd(duk_context* ctx) {
    uint64_t ep = 0;
    RenderBox* b = checkElement(ctx, 0, &ep);
    NetSurfJsBridge* br = engineOf(ctx);
    if (!b || !br || ep != br->m_epoch) return 0;
    std::string cls = duk_safe_to_string(ctx, 0);
    classListAddImpl(b, cls);
    b->jsEpoch++;
    br->markDirty();
    return 0;
}

duk_ret_t NetSurfJsBridge::classListRemove(duk_context* ctx) {
    uint64_t ep = 0;
    RenderBox* b = checkElement(ctx, 0, &ep);
    NetSurfJsBridge* br = engineOf(ctx);
    if (!b || !br || ep != br->m_epoch) return 0;
    std::string cls = duk_safe_to_string(ctx, 0);
    classListRemoveImpl(b, cls);
    b->jsEpoch++;
    br->markDirty();
    return 0;
}

duk_ret_t NetSurfJsBridge::classListToggle(duk_context* ctx) {
    uint64_t ep = 0;
    RenderBox* b = checkElement(ctx, 0, &ep);
    NetSurfJsBridge* br = engineOf(ctx);
    if (!b || !br || ep != br->m_epoch) return 0;
    std::string cls = duk_safe_to_string(ctx, 0);
    if (classListHas(b, cls)) classListRemoveImpl(b, cls);
    else classListAddImpl(b, cls);
    b->jsEpoch++;
    br->markDirty();
    return 0;
}

duk_ret_t NetSurfJsBridge::classListContains(duk_context* ctx) {
    uint64_t ep = 0;
    RenderBox* b = checkElement(ctx, 0, &ep);
    NetSurfJsBridge* br = engineOf(ctx);
    if (!b || !br || ep != br->m_epoch) { duk_push_false(ctx); return 1; }
    std::string cls = duk_safe_to_string(ctx, 0);
    duk_push_boolean(ctx, classListHas(b, cls));
    return 1;
}

duk_ret_t NetSurfJsBridge::styleGetDisplay(duk_context* ctx) {
    uint64_t ep = 0;
    RenderBox* b = checkElement(ctx, 0, &ep);
    NetSurfJsBridge* br = engineOf(ctx);
    if (!b || !br || ep != br->m_epoch) { duk_push_string(ctx, ""); return 1; }
    if (b->jsHidden) { duk_push_string(ctx, "none"); return 1; }
    // RenderBoxType → CSS display string.
    switch (b->type) {
        case RenderBoxType::INLINE:
        case RenderBoxType::TEXT:
        case RenderBoxType::IMAGE:
            duk_push_string(ctx, "inline"); break;
        case RenderBoxType::FLEX_ROW:
        case RenderBoxType::FLEX_COLUMN:
            duk_push_string(ctx, "flex"); break;
        case RenderBoxType::GRID:
            duk_push_string(ctx, "grid"); break;
        default:
            duk_push_string(ctx, "block"); break;
    }
    return 1;
}

duk_ret_t NetSurfJsBridge::styleSetDisplay(duk_context* ctx) {
    uint64_t ep = 0;
    RenderBox* b = checkElement(ctx, 0, &ep);
    NetSurfJsBridge* br = engineOf(ctx);
    if (!b || !br || ep != br->m_epoch) return 0;
    std::string v = toLowerStr(trimStr(duk_safe_to_string(ctx, 0)));
    // v1: only "none" / non-"none" is meaningful. Hiding hides the box and
    // its subtree on the next layout; restoring requires a full re-layout
    // (we leave type unchanged — the renderer/layout consult jsHidden).
    if (v == "none") {
        b->jsHidden = true;
    } else if (v == "block" || v == "inline" || v == "flex" || v == "grid") {
        b->jsHidden = false;
        // The pre-layout display can't be changed for an already-laid-out
        // box; the bridge accepts the assignment so JS doesn't error, but
        // a full re-layout is needed to materialize the new flow.
        Logger::info("NetSurfJsBridge: display='" + v + "' set on existing box (re-layout for full effect)");
    } else {
        Logger::warn("NetSurfJsBridge: unknown display='" + v + "'");
    }
    b->jsEpoch++;
    br->markDirty();
    return 0;
}

bool NetSurfJsBridge::poll(uint32_t nowMs) {
    if (!m_ctx) return false;
    std::vector<std::pair<uint32_t, std::string>> fireList;
    {
        std::lock_guard<std::mutex> lock(m_asyncMutex);
        for (size_t i = 0; i < m_timers.size(); ) {
            if (m_timers[i].dueMs <= nowMs) {
                fireList.emplace_back(m_timers[i].id,
                    "\xff" "t" + std::to_string(m_timers[i].id));
                m_timers.erase(m_timers.begin() + i);
            } else {
                i++;
            }
        }
    }
    if (fireList.empty()) return false;
    for (const auto& p : fireList) {
        duk_get_global_string(m_ctx, p.second.c_str());
        if (duk_is_function(m_ctx, -1)) {
            s_hasFatalJmp = true;
            if (setjmp(s_fatalJmp) != 0) {
                s_hasFatalJmp = false;
                Logger::error("NetSurfJsBridge: fatal in timer");
                duk_pop(m_ctx);
                duk_push_global_object(m_ctx);
                duk_del_prop_string(m_ctx, -1, p.second.c_str());
                duk_pop(m_ctx);
                continue;
            }
            int rc = duk_pcall(m_ctx, 0);
            if (rc != 0) {
                Logger::warn(std::string("NetSurfJsBridge: timer: ") +
                             duk_safe_to_string(m_ctx, -1));
                duk_pop(m_ctx);
            } else {
                duk_pop(m_ctx);
            }
            s_hasFatalJmp = false;
        } else {
            duk_pop(m_ctx);
        }
        duk_push_global_object(m_ctx);
        duk_del_prop_string(m_ctx, -1, p.second.c_str());
        duk_pop(m_ctx);
    }
    return m_dirty;
}

}  // namespace RomCloud
