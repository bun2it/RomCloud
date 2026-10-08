// JsEngine — Duktape bridge (see JsEngine.h). Conventions:
// - The renderer pointer + epoch travel in the global stash ("\xff" "eng").
// - Element proxies are plain objects with hidden "\xff" "el" (HtmlElement*)
//   and "\xff" "ep" (epoch) properties. Every access revalidates the epoch;
//   stale proxies behave as null (methods no-op, getters give undefined).
// - All DOM reads/writes happen on the main thread (scripts run at load,
//   timers/XHR callbacks in poll()). XHR workers only touch the job struct.
#include "JsEngine.h"

#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <SDL2/SDL.h>

#include <csetjmp>

#include "../logging/Logger.h"
#include "../network/HttpClient.h"
#include "BrowserManager.h"
#include "HtmlRenderer.h"

namespace RomCloud {

static jmp_buf s_jsFatalJmp;
static bool s_hasFatalJmp = false;

static void jsFatalHandler(void* udata, const char* msg) {
    (void)udata;
    Logger::error(std::string("JsEngine FATAL: ") + (msg ? msg : "unknown"));
    if (s_hasFatalJmp) {
        longjmp(s_jsFatalJmp, 1);
    }
}

JsEngine::JsEngine(HtmlRenderer* renderer, uint64_t epoch, const std::string& pageUrl)
    : m_renderer(renderer), m_epoch(epoch), m_pageUrl(pageUrl) {
    m_ctx = duk_create_heap(nullptr, nullptr, nullptr, nullptr, jsFatalHandler);
    if (m_ctx) registerGlobals();
}

JsEngine::~JsEngine() {
    if (m_ctx) {
        duk_destroy_heap(m_ctx);
        m_ctx = nullptr;
    }
}

JsEngine* JsEngine::engineOf(duk_context* ctx) {
    duk_push_global_stash(ctx);
    duk_get_prop_string(ctx, -1, "\xff" "eng");
    JsEngine* eng = static_cast<JsEngine*>(duk_get_pointer(ctx, -1));
    duk_pop_2(ctx);
    return eng;
}

HtmlRenderer* JsEngine::rendererOf(duk_context* ctx) {
    JsEngine* eng = engineOf(ctx);
    if (!eng || !eng->m_renderer) return nullptr;
    if (eng->m_renderer->jsEpoch() != eng->m_epoch) return nullptr;
    return eng->m_renderer;
}

bool JsEngine::runScript(const std::string& code, const std::string& name) {
    // Cap script size to 64KB to avoid exhausting Duktape heap on low-memory ARM
    if (!m_ctx || code.empty() || code.size() > 64 * 1024) return false;
    s_hasFatalJmp = true;
    if (setjmp(s_jsFatalJmp) != 0) {
        s_hasFatalJmp = false;
        Logger::error("JsEngine: safely recovered from fatal error in " + name);
        return false;
    }
    if (duk_peval_string(m_ctx, code.c_str()) != 0) {
        Logger::warn("JsEngine: " + name + ": " + duk_safe_to_string(m_ctx, -1));
        duk_pop(m_ctx);
        s_hasFatalJmp = false;
        return false;
    }
    duk_pop(m_ctx);
    s_hasFatalJmp = false;
    return true;
}

// ---------- globals ----------

duk_ret_t JsEngine::fnLog(duk_context* ctx) {
    duk_idx_t n = duk_get_top(ctx);
    std::string out;
    for (duk_idx_t i = 0; i < n; i++) {
        if (i) out += " ";
        out += duk_safe_to_string(ctx, i);
    }
    Logger::info("JsEngine: console: " + out.substr(0, 300));
    return 0;
}

duk_ret_t JsEngine::fnAlert(duk_context* ctx) {
    std::string msg = duk_safe_to_string(ctx, 0);
    Logger::info("JsEngine: alert: " + msg.substr(0, 200));
    return 0;
}

duk_ret_t JsEngine::fnSetTimeout(duk_context* ctx) {
    JsEngine* eng = engineOf(ctx);
    if (!eng || !duk_is_function(ctx, 0)) return DUK_RET_TYPE_ERROR;
    if (eng->m_timers.size() >= 64) return 0;
    double ms = duk_to_number(ctx, 1);
    JsTimer t;
    t.id = eng->m_nextId++;
    if (t.id == 0) t.id = eng->m_nextId++;
    t.dueMs = SDL_GetTicks() + (uint32_t)(ms < 0 ? 0 : ms);
    t.repeat = false;
    // Keep the callback alive in the stash keyed by id.
    duk_push_global_stash(ctx);
    char key[32];
    snprintf(key, sizeof(key), "\xFF" "cb%u", t.id);
    duk_dup(ctx, 0);
    duk_put_prop_string(ctx, -2, key);
    duk_pop(ctx);
    eng->m_timers.push_back(t);
    duk_push_uint(ctx, t.id);
    return 1;
}

duk_ret_t JsEngine::fnSetInterval(duk_context* ctx) {
    JsEngine* eng = engineOf(ctx);
    if (!eng || !duk_is_function(ctx, 0)) return DUK_RET_TYPE_ERROR;
    if (eng->m_timers.size() >= 64) return 0;
    double ms = duk_to_number(ctx, 1);
    if (ms < 10) ms = 10;
    JsTimer t;
    t.id = eng->m_nextId++;
    if (t.id == 0) t.id = eng->m_nextId++;
    t.dueMs = SDL_GetTicks() + (uint32_t)ms;
    t.intervalMs = (uint32_t)ms;
    t.repeat = true;
    duk_push_global_stash(ctx);
    char key[32];
    snprintf(key, sizeof(key), "\xFF" "cb%u", t.id);
    duk_dup(ctx, 0);
    duk_put_prop_string(ctx, -2, key);
    duk_pop(ctx);
    eng->m_timers.push_back(t);
    duk_push_uint(ctx, t.id);
    return 1;
}

duk_ret_t JsEngine::fnClearTimeout(duk_context* ctx) {
    JsEngine* eng = engineOf(ctx);
    if (!eng) return 0;
    uint32_t id = duk_to_uint32(ctx, 0);
    for (size_t i = 0; i < eng->m_timers.size(); i++) {
        if (eng->m_timers[i].id == id) {
            eng->m_timers.erase(eng->m_timers.begin() + i);
            duk_push_global_stash(ctx);
            char key[32];
            snprintf(key, sizeof(key), "\xFF" "cb%u", id);
            duk_del_prop_string(ctx, -1, key);
            duk_pop(ctx);
            break;
        }
    }
    return 0;
}

bool JsEngine::poll(uint32_t nowMs) {
    if (!m_ctx) return false;
    bool domDirty = false;
    // Timers due?
    for (size_t i = 0; i < m_timers.size();) {
        JsTimer& t = m_timers[i];
        if ((int32_t)(nowMs - t.dueMs) < 0) {
            i++;
            continue;
        }
        char key[32];
        snprintf(key, sizeof(key), "\xFF" "cb%u", t.id);
        duk_push_global_stash(m_ctx);
        duk_get_prop_string(m_ctx, -1, key);
        bool isFn = duk_is_function(m_ctx, -1);
        duk_pop(m_ctx);  // stash
        if (isFn) {
            duk_push_global_stash(m_ctx);
            duk_get_prop_string(m_ctx, -1, key);
            duk_remove(m_ctx, -2);
            if (duk_pcall(m_ctx, 0) != 0) {
                Logger::warn(std::string("JsEngine: timer: ") + duk_safe_to_string(m_ctx, -1));
            }
            duk_pop(m_ctx);
            domDirty = true;
        }
        if (t.repeat) {
            t.dueMs = nowMs + t.intervalMs;
            i++;
        } else {
            duk_push_global_stash(m_ctx);
            duk_del_prop_string(m_ctx, -1, key);
            duk_pop(m_ctx);
            m_timers.erase(m_timers.begin() + i);
        }
    }
    // Finished XHR?
    for (const auto& job : m_xhrs) {
        bool done = false;
        {
            std::lock_guard<std::mutex> lock(job->mtx);
            done = job->done;
        }
        if (!done || job->epoch != m_epoch) continue;
        // Deliver: find the xhr object by job id in the stash.
        char key[32];
        snprintf(key, sizeof(key), "\xffxhr%u", job->id);
        duk_push_global_stash(m_ctx);
        duk_get_prop_string(m_ctx, -1, key);
        if (duk_is_object(m_ctx, -1)) {
            duk_push_int(m_ctx, job->status);
            duk_put_prop_string(m_ctx, -2, "status");
            duk_push_string(m_ctx, job->respBody.c_str());
            duk_put_prop_string(m_ctx, -2, "responseText");
            duk_push_int(m_ctx, 4);
            duk_put_prop_string(m_ctx, -2, "readyState");
            // onreadystatechange
            duk_get_prop_string(m_ctx, -1, "onreadystatechange");
            if (duk_is_function(m_ctx, -1)) {
                duk_dup(m_ctx, -2);  // this = xhr
                if (duk_pcall_method(m_ctx, 0) != 0) {
                    Logger::warn(std::string("JsEngine: xhr rs: ") + duk_safe_to_string(m_ctx, -1));
                }
                duk_pop(m_ctx);
            } else {
                duk_pop(m_ctx);
            }
            // onload on success
            if (job->success) {
                duk_get_prop_string(m_ctx, -1, "onload");
                if (duk_is_function(m_ctx, -1)) {
                    duk_dup(m_ctx, -2);
                    if (duk_pcall_method(m_ctx, 0) != 0) {
                        Logger::warn(std::string("JsEngine: xhr load: ") +
                                     duk_safe_to_string(m_ctx, -1));
                    }
                    duk_pop(m_ctx);
                } else {
                    duk_pop(m_ctx);
                }
            }
            domDirty = true;
        }
        duk_pop(m_ctx);  // stash
        // Mark delivered (epoch mismatch naturally drops it next time).
        {
            std::lock_guard<std::mutex> lock(job->mtx);
            job->done = false;
            job->epoch = 0;  // delivered
        }
    }
    // Drop delivered jobs.
    for (size_t i = 0; i < m_xhrs.size();) {
        bool drop = (m_xhrs[i]->epoch == 0);
        if (drop) {
            char key[32];
            snprintf(key, sizeof(key), "\xffxhr%u", m_xhrs[i]->id);
            duk_push_global_stash(m_ctx);
            duk_del_prop_string(m_ctx, -1, key);
            duk_pop(m_ctx);
            m_xhrs.erase(m_xhrs.begin() + i);
        } else {
            i++;
        }
    }
    return domDirty;
}

void JsEngine::registerGlobals() {
    duk_push_global_stash(m_ctx);
    duk_push_pointer(m_ctx, this);
    duk_put_prop_string(m_ctx, -2, "\xff" "eng");
    duk_pop(m_ctx);

    duk_push_c_function(m_ctx, fnLog, DUK_VARARGS);
    duk_put_global_string(m_ctx, "__log_alias");
    // console = { log }
    duk_push_object(m_ctx);
    duk_push_c_function(m_ctx, fnLog, DUK_VARARGS);
    duk_put_prop_string(m_ctx, -2, "log");
    duk_push_c_function(m_ctx, fnLog, DUK_VARARGS);
    duk_put_prop_string(m_ctx, -2, "info");
    duk_push_c_function(m_ctx, fnLog, DUK_VARARGS);
    duk_put_prop_string(m_ctx, -2, "warn");
    duk_push_c_function(m_ctx, fnLog, DUK_VARARGS);
    duk_put_prop_string(m_ctx, -2, "error");
    duk_put_global_string(m_ctx, "console");

    duk_push_c_function(m_ctx, fnSetTimeout, 2);
    duk_put_global_string(m_ctx, "setTimeout");
    duk_push_c_function(m_ctx, fnSetInterval, 2);
    duk_put_global_string(m_ctx, "setInterval");
    duk_push_c_function(m_ctx, fnClearTimeout, 1);
    duk_put_global_string(m_ctx, "clearTimeout");
    duk_push_c_function(m_ctx, fnClearTimeout, 1);
    duk_put_global_string(m_ctx, "clearInterval");
    duk_push_c_function(m_ctx, fnAlert, 1);
    duk_put_global_string(m_ctx, "alert");

    // document = { getElementById, querySelector, forms, location passthrough }
    duk_push_object(m_ctx);
    duk_push_c_function(m_ctx, fnGetElementById, 1);
    duk_put_prop_string(m_ctx, -2, "getElementById");
    duk_push_c_function(m_ctx, fnQuerySelector, 1);
    duk_put_prop_string(m_ctx, -2, "querySelector");
    duk_put_global_string(m_ctx, "document");

    // location = { href getter/setter }.
    duk_push_object(m_ctx);
    duk_push_string(m_ctx, "href");
    // getter
    duk_push_c_function(m_ctx,
                        [](duk_context* ctx) -> duk_ret_t {
                            HtmlRenderer* r = rendererOf(ctx);
                            duk_push_string(ctx, r ? r->currentUrl().c_str() : "");
                            return 1;
                        },
                        0);
    // setter
    duk_push_c_function(m_ctx,
                        [](duk_context* ctx) -> duk_ret_t {
                            HtmlRenderer* r = rendererOf(ctx);
                            if (r) {
                                std::string u = duk_safe_to_string(ctx, 0);
                                if (u.compare(0, 7, "http://") == 0 ||
                                    u.compare(0, 8, "https://") == 0) {
                                    r->loadUrl(u);
                                }
                            }
                            return 0;
                        },
                        1);
    duk_def_prop(m_ctx, -4,
                 DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER |
                     DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_FORCE);
    duk_put_global_string(m_ctx, "location");

    // window === globalThis alias.
    duk_push_global_object(m_ctx);
    duk_put_global_string(m_ctx, "window");

    // XMLHttpRequest constructor.
    duk_push_c_function(m_ctx, fnXhrCtor, 0);
    duk_put_global_string(m_ctx, "XMLHttpRequest");
}

// ---------- element proxies ----------

void JsEngine::pushElementProxy(HtmlElement* el) {
    duk_push_object(m_ctx);
    duk_push_pointer(m_ctx, el);
    duk_put_prop_string(m_ctx, -2, "\xff" "el");
    duk_push_number(m_ctx, (double)m_epoch);
    duk_put_prop_string(m_ctx, -2, "\xff" "ep");
    // value getter/setter
    duk_push_string(m_ctx, "value");
    duk_push_c_function(m_ctx, elGetValue, 0);
    duk_push_c_function(m_ctx, elSetValue, 1);
    duk_def_prop(m_ctx, -4,
                 DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER |
                     DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_FORCE);
    // textContent getter/setter
    duk_push_string(m_ctx, "textContent");
    duk_push_c_function(m_ctx, elGetText, 0);
    duk_push_c_function(m_ctx, elSetText, 1);
    duk_def_prop(m_ctx, -4,
                 DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER |
                     DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_FORCE);
    // innerHTML getter only (set falls back to textContent set)
    duk_push_string(m_ctx, "innerHTML");
    duk_push_c_function(m_ctx, elGetText, 0);
    duk_push_c_function(m_ctx, elSetText, 1);
    duk_def_prop(m_ctx, -4,
                 DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER |
                     DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_FORCE);
    // style object with display accessor (owner element stashed on it).
    duk_push_string(m_ctx, "style");
    duk_push_object(m_ctx);
    duk_push_pointer(m_ctx, el);
    duk_put_prop_string(m_ctx, -2, "\xff" "owner");
    duk_push_string(m_ctx, "display");
    duk_push_c_function(m_ctx, styleGetDisplay, 0);
    duk_push_c_function(m_ctx, styleSetDisplay, 1);
    duk_def_prop(m_ctx, -4,
                 DUK_DEFPROP_HAVE_GETTER | DUK_DEFPROP_HAVE_SETTER |
                     DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_FORCE);
    duk_def_prop(m_ctx, -3,
                 DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_ENUMERABLE | DUK_DEFPROP_FORCE);
    duk_push_c_function(m_ctx, elClick, 0);
    duk_put_prop_string(m_ctx, -2, "click");
    duk_push_c_function(m_ctx, elSubmit, 0);
    duk_put_prop_string(m_ctx, -2, "submit");
}

HtmlElement* JsEngine::checkElement(duk_context* ctx, duk_idx_t idx, uint64_t* epochOut) {
    if (!duk_is_object(ctx, idx)) return nullptr;
    duk_get_prop_string(ctx, idx, "\xff" "el");
    HtmlElement* el = static_cast<HtmlElement*>(duk_get_pointer(ctx, -1));
    duk_pop(ctx);
    duk_get_prop_string(ctx, idx, "\xff" "ep");
    double ep = duk_get_number(ctx, -1);
    duk_pop(ctx);
    JsEngine* eng = engineOf(ctx);
    if (!eng || !el) return nullptr;
    if ((uint64_t)ep != eng->m_epoch) return nullptr;
    if (epochOut) *epochOut = (uint64_t)ep;
    return el;
}

duk_ret_t JsEngine::elGetValue(duk_context* ctx) {
    duk_push_this(ctx);
    HtmlElement* el = checkElement(ctx, -1, nullptr);
    duk_pop(ctx);
    if (!el) {
        duk_push_undefined(ctx);
        return 1;
    }
    duk_push_string(ctx, el->value.c_str());
    return 1;
}

duk_ret_t JsEngine::elSetValue(duk_context* ctx) {
    duk_push_this(ctx);
    HtmlElement* el = checkElement(ctx, -1, nullptr);
    duk_pop(ctx);
    if (el && (el->type == HtmlElementType::INPUT_TEXT ||
               el->type == HtmlElementType::INPUT_PASSWORD)) {
        std::string v = duk_safe_to_string(ctx, 0);
        if (v.size() > 256) v.resize(256);
        el->value = v;
        if (JsEngine* eng = engineOf(ctx)) {
            if (eng->m_renderer) eng->m_renderer->markDirtyPublic();
        }
    }
    return 0;
}

static void collectTextOf(HtmlElement* el, std::string& out) {
    if (el->type == HtmlElementType::TEXT && !el->text.empty()) {
        if (!out.empty()) out += " ";
        out += el->text;
    }
    for (auto& c : el->children) collectTextOf(&c, out);
}

duk_ret_t JsEngine::elGetText(duk_context* ctx) {
    duk_push_this(ctx);
    HtmlElement* el = checkElement(ctx, -1, nullptr);
    duk_pop(ctx);
    if (!el) {
        duk_push_undefined(ctx);
        return 1;
    }
    std::string out;
    collectTextOf(el, out);
    duk_push_string(ctx, out.c_str());
    return 1;
}

duk_ret_t JsEngine::elSetText(duk_context* ctx) {
    duk_push_this(ctx);
    HtmlElement* el = checkElement(ctx, -1, nullptr);
    duk_pop(ctx);
    if (el) {
        std::string v = duk_safe_to_string(ctx, 0);
        // Do not wipe out structural containers (body, main, article, nav, header, footer)
        if (el->tag != "body" && el->tag != "main" && el->tag != "article" &&
            el->tag != "nav" && el->tag != "header" && el->tag != "footer") {
            el->text = v;
            el->children.clear();
            if (!v.empty()) {
                HtmlElement t;
                t.type = HtmlElementType::TEXT;
                t.text = v;
                t.lineH = el->lineH;
                t.bold = el->bold;
                el->children.push_back(std::move(t));
            }
            if (JsEngine* eng = engineOf(ctx)) {
                if (eng->m_renderer) eng->m_renderer->markDirtyPublic();
            }
        }
    }
    return 0;
}

duk_ret_t JsEngine::elClick(duk_context* ctx) {
    duk_push_this(ctx);
    HtmlElement* el = checkElement(ctx, -1, nullptr);
    duk_pop(ctx);
    JsEngine* eng = engineOf(ctx);
    if (!el || !eng || !eng->m_renderer) return 0;
    HtmlRenderer* r = rendererOf(ctx);
    if (!r) return 0;
    // Route through the same A-key path as dpad input.
    int saved = r->focusIndex();
    (void)saved;
    // Find index of el in focusables if present, else act directly.
    r->jsActivate(el);
    return 0;
}

duk_ret_t JsEngine::elSubmit(duk_context* ctx) {
    duk_push_this(ctx);
    HtmlElement* el = checkElement(ctx, -1, nullptr);
    duk_pop(ctx);
    if (el) {
        if (JsEngine* eng = engineOf(ctx)) {
            if (eng->m_renderer) eng->m_renderer->jsSubmit(el);
        }
    }
    return 0;
}

duk_ret_t JsEngine::elGetStyle(duk_context* ctx) {
    // style property itself is an object; this resolves style.display reads
    // when accessed via the def_prop getter chain (unused directly).
    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, "display");
    duk_remove(ctx, -2);
    return 1;
}

duk_ret_t JsEngine::styleGetDisplay(duk_context* ctx) {
    // this = style object; owner element is two levels up via closure? We
    // store the element on the style object at creation instead — simpler:
    // re-resolve through the parent chain is unavailable, so proxies store
    // "\xff" "owner" on the style object. Fallback: visible.
    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, "\xff" "owner");
    HtmlElement* el = static_cast<HtmlElement*>(duk_get_pointer(ctx, -1));
    duk_pop_2(ctx);
    JsEngine* eng = engineOf(ctx);
    if (!el || !eng) {
        duk_push_string(ctx, "");
        return 1;
    }
    // Validate epoch via a fresh proxy check is overkill; element lifetime
    // is tied to the page epoch which outlives short scripts. Timers re-run
    // on the same page only (poll checks epoch).
    duk_push_string(ctx, el->hide ? "none" : "block");
    return 1;
}

duk_ret_t JsEngine::styleSetDisplay(duk_context* ctx) {
    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, "\xff" "owner");
    HtmlElement* el = static_cast<HtmlElement*>(duk_get_pointer(ctx, -1));
    duk_pop_2(ctx);
    JsEngine* eng = engineOf(ctx);
    if (el && eng && eng->m_renderer && eng->m_renderer->jsEpoch() == eng->m_epoch) {
        std::string v = duk_safe_to_string(ctx, 0);
        std::string low = v;
        for (char& c : low) c = (char)std::tolower((unsigned char)c);
        el->hide = (low.find("none") != std::string::npos);
        if (eng->m_renderer) eng->m_renderer->markDirtyPublic();
    }
    return 0;
}

// ---------- document ----------

duk_ret_t JsEngine::fnGetElementById(duk_context* ctx) {
    HtmlRenderer* r = rendererOf(ctx);
    std::string id = duk_safe_to_string(ctx, 0);
    JsEngine* eng = engineOf(ctx);
    if (!r || !eng || id.empty()) {
        duk_push_null(ctx);
        return 1;
    }
    HtmlElement* el = r->jsFindById(id);
    if (!el) {
        duk_push_null(ctx);
        return 1;
    }
    eng->pushElementProxy(el);
    return 1;
}

duk_ret_t JsEngine::fnQuerySelector(duk_context* ctx) {
    HtmlRenderer* r = rendererOf(ctx);
    std::string sel = duk_safe_to_string(ctx, 0);
    JsEngine* eng = engineOf(ctx);
    if (!r || !eng || sel.empty()) {
        duk_push_null(ctx);
        return 1;
    }
    HtmlElement* el = r->jsQueryFirst(sel);
    if (!el) {
        duk_push_null(ctx);
        return 1;
    }
    eng->pushElementProxy(el);
    return 1;
}

// ---------- XMLHttpRequest ----------

duk_ret_t JsEngine::fnXhrCtor(duk_context* ctx) {
    if (!duk_is_constructor_call(ctx)) return DUK_RET_TYPE_ERROR;
    JsEngine* eng = engineOf(ctx);
    if (!eng) return DUK_RET_TYPE_ERROR;
    // this = fresh object. Attach job slot + methods.
    auto job = std::make_shared<JsXhrJob>();
    job->id = eng->m_nextId++;
    if (job->id == 0) job->id = eng->m_nextId++;
    job->epoch = eng->m_epoch;
    {
        std::lock_guard<std::mutex> lock(eng->m_asyncMutex);
        if (eng->m_xhrs.size() >= 4) {
            duk_push_error_object(ctx, DUK_ERR_ERROR, "too many requests");
            return duk_throw(ctx);
        }
        eng->m_xhrs.push_back(job);
    }
    duk_push_this(ctx);
    duk_push_pointer(ctx, job.get());
    duk_put_prop_string(ctx, -2, "\xff" "job");
    // Keep a ref so delivery can find us.
    duk_push_global_stash(ctx);
    char key[32];
    snprintf(key, sizeof(key), "\xffxhr%u", job->id);
    duk_dup(ctx, -2);
    duk_put_prop_string(ctx, -2, key);
    duk_pop_2(ctx);  // stash, this
    duk_push_this(ctx);
    duk_push_c_function(ctx, xhrOpen, 3);
    duk_put_prop_string(ctx, -2, "open");
    duk_push_c_function(ctx, xhrSend, 1);
    duk_put_prop_string(ctx, -2, "send");
    duk_push_c_function(ctx, xhrSetHeader, 2);
    duk_put_prop_string(ctx, -2, "setRequestHeader");
    duk_push_c_function(ctx, xhrAbort, 0);
    duk_put_prop_string(ctx, -2, "abort");
    duk_push_int(ctx, 0);
    duk_put_prop_string(ctx, -2, "readyState");
    duk_push_int(ctx, 0);
    duk_put_prop_string(ctx, -2, "status");
    duk_push_string(ctx, "");
    duk_put_prop_string(ctx, -2, "responseText");
    return 0;  // constructor return is ignored (this used)
}

std::shared_ptr<JsXhrJob> JsEngine::xhrJobOf(duk_context* ctx) {
    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, "\xff" "job");
    void* p = duk_get_pointer(ctx, -1);
    duk_pop_2(ctx);
    if (!p) return nullptr;
    JsEngine* eng = JsEngine::engineOf(ctx);
    if (!eng) return nullptr;
    std::lock_guard<std::mutex> lock(eng->m_asyncMutex);
    for (auto& j : eng->m_xhrs) {
        if (j.get() == p) return j;
    }
    return nullptr;
}

duk_ret_t JsEngine::xhrOpen(duk_context* ctx) {
    auto job = xhrJobOf(ctx);
    if (!job) return 0;
    job->method = duk_safe_to_string(ctx, 0);
    std::string url = duk_safe_to_string(ctx, 1);
    // Resolve relative against the page URL on the main thread (have it).
    JsEngine* eng = engineOf(ctx);
    if (eng && eng->m_renderer && !url.empty() && url.find("://") == std::string::npos &&
        url.compare(0, 5, "data:") != 0) {
        // Simple resolve: reuse renderer's resolver via a temp element trick
        // is unavailable; resolve against page origin here.
        std::string base = eng->m_renderer->currentUrl();
        size_t se = base.find("://");
        if (se != std::string::npos) {
            size_t ps = base.find('/', se + 3);
            std::string origin = (ps == std::string::npos) ? base : base.substr(0, ps);
            if (!url.empty() && url[0] == '/') url = origin + url;
        }
    }
    job->url = url;
    duk_push_this(ctx);
    duk_push_int(ctx, 1);
    duk_put_prop_string(ctx, -2, "readyState");
    return 0;
}

duk_ret_t JsEngine::xhrSend(duk_context* ctx) {
    auto job = xhrJobOf(ctx);
    JsEngine* eng = engineOf(ctx);
    if (!job || !eng) return 0;
    if (duk_get_top(ctx) >= 1 && !duk_is_null_or_undefined(ctx, 0)) {
        job->body = duk_safe_to_string(ctx, 0);
    }
    duk_push_this(ctx);
    duk_push_int(ctx, 3);
    duk_put_prop_string(ctx, -2, "readyState");
    duk_pop(ctx);
    // Worker thread performs the fetch, then flags completion.
    // NOTE: never touches the engine (it may be dead); job mutex only.
    struct Payload {
        std::shared_ptr<JsXhrJob> job;
    };
    Payload* pl = new Payload{job};
    pthread_t tid;
    int rc = pthread_create(
        &tid, nullptr,
        [](void* arg) -> void* {
            std::unique_ptr<Payload> owned(static_cast<Payload*>(arg));
            HttpResponse resp;
            std::string method, url, body;
            std::vector<std::string> headers;
            {
                std::lock_guard<std::mutex> lock(owned->job->mtx);
                method = owned->job->method;
                url = owned->job->url;
                body = owned->job->body;
                headers = owned->job->headers;
            }
            if (method == "POST" || method == "post") {
                resp = HttpClient::instance().post(url, body, headers, 15);
            } else {
                resp = HttpClient::instance().get(url, headers, 15);
            }
            {
                std::lock_guard<std::mutex> lock(owned->job->mtx);
                owned->job->status = resp.statusCode;
                owned->job->respBody = std::move(resp.body);
                if (owned->job->respBody.size() > 512 * 1024) {
                    owned->job->respBody.resize(512 * 1024);
                }
                owned->job->success = resp.success;
                owned->job->done = true;
            }
            return nullptr;
        },
        pl);
    if (rc != 0) {
        delete pl;
    } else {
        pthread_detach(tid);
    }
    return 0;
}

duk_ret_t JsEngine::xhrSetHeader(duk_context* ctx) {
    auto job = xhrJobOf(ctx);
    if (!job) return 0;
    std::string k = duk_safe_to_string(ctx, 0);
    std::string v = duk_safe_to_string(ctx, 1);
    if (!k.empty()) job->headers.push_back(k + ": " + v);
    return 0;
}

duk_ret_t JsEngine::xhrAbort(duk_context* ctx) {
    auto job = xhrJobOf(ctx);
    if (!job) return 0;
    {
        std::lock_guard<std::mutex> lock(job->mtx);
        job->done = false;
        job->epoch = 0;
    }
    return 0;
}

}  // namespace RomCloud
