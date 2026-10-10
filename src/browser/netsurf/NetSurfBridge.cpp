#include "NetSurfBridge.h"
#include "NetSurfLayout.h"

#if defined(__linux__) || defined(ENABLE_NETSURF)
#define HAVE_NETSURF_LIBS 1

// Workaround for C++ keywords used in NetSurf C headers
#define namespace ns_namespace
#define class ns_class
#define restrict __restrict__
    extern "C" {
#include <dom/bindings/hubbub/parser.h>
#include <dom/dom.h>
#include <libcss/libcss.h>
#include <libcss/select.h>
#include <wapcaplet/libwapcaplet.h>
}
#undef restrict
#undef class
#undef namespace

#else
#define HAVE_NETSURF_LIBS 0
#endif

#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>
#include <unordered_map>  // P23: applyGridFromAuthorCss

namespace RomCloud {

NetSurfBridge &NetSurfBridge::instance() {
  static NetSurfBridge s_inst;
  return s_inst;
}

NetSurfBridge::~NetSurfBridge() { shutdown(); }

bool NetSurfBridge::init() {
  if (m_initialized)
    return true;
  m_initialized = true;
  return true;
}

void NetSurfBridge::shutdown() { m_initialized = false; }

static std::string resolveUrl(const std::string& base, const std::string& ref) {
  if (ref.empty()) return "";
  if (ref.rfind("http://", 0) == 0 || ref.rfind("https://", 0) == 0) return ref;
  if (ref.rfind("//", 0) == 0) return "https:" + ref;
  if (base.empty()) return ref;

  size_t schemeEnd = base.find("://");
  if (schemeEnd == std::string::npos) return ref;
  size_t hostEnd = base.find('/', schemeEnd + 3);
  std::string origin = (hostEnd == std::string::npos) ? base : base.substr(0, hostEnd);

  if (ref.front() == '/') {
    return origin + ref;
  }
  size_t lastSlash = base.rfind('/');
  if (lastSlash != std::string::npos && lastSlash > schemeEnd + 2) {
    return base.substr(0, lastSlash + 1) + ref;
  }
  return origin + "/" + ref;
}

static void resolveTreeUrls(NetSurfStyledNode* node, const std::string& baseUrl) {
  if (!node || baseUrl.empty()) return;
  if (!node->src.empty()) {
    node->src = resolveUrl(baseUrl, node->src);
  }
  if (!node->href.empty()) {
    node->href = resolveUrl(baseUrl, node->href);
  }
  for (auto& ch : node->children) {
    resolveTreeUrls(ch.get(), baseUrl);
  }
}

#if HAVE_NETSURF_LIBS
// Helper to convert dom_string to std::string and unref
static std::string domStringToStd(dom_string *str) {
  if (!str)
    return "";
  const char *data = dom_string_data(str);
  size_t len = dom_string_byte_length(str);
  std::string res(data ? data : "", len);
  dom_string_unref(str);
  return res;
}

// Helper to get element attribute as std::string
static std::string getElementAttr(dom_element *elem, const char *attrName) {
  if (!elem || !attrName)
    return "";
  dom_string *nameStr = nullptr;
  if (dom_string_create((const uint8_t *)attrName, strlen(attrName),
                        &nameStr) != DOM_NO_ERR ||
      !nameStr) {
    return "";
  }
  dom_string *valStr = nullptr;
  dom_exception exc = dom_element_get_attribute(elem, nameStr, &valStr);
  dom_string_unref(nameStr);
  if (exc == DOM_NO_ERR && valStr) {
    return domStringToStd(valStr);
  }
  return "";
}

// Helper: bare-attribute presence (<input checked>, <option selected>).
// getElementAttr can't see these — get_attribute returns "" for them.
static bool hasElementAttr(dom_element *elem, const char *attrName) {
  if (!elem || !attrName)
    return false;
  dom_string *nameStr = nullptr;
  if (dom_string_create((const uint8_t *)attrName, strlen(attrName),
                        &nameStr) != DOM_NO_ERR ||
      !nameStr) {
    return false;
  }
  bool present = false;
  dom_exception exc = dom_element_has_attribute(elem, nameStr, &present);
  dom_string_unref(nameStr);
  return exc == DOM_NO_ERR && present;
}
static void walkDomNode(dom_node *node, NetSurfDomNode &outNode,
                        int &totalNodes) {
  if (!node)
    return;
  totalNodes++;

  dom_node_type ntype = DOM_DOCUMENT_NODE;
  dom_node_get_node_type(node, &ntype);

  if (ntype == DOM_ELEMENT_NODE) {
    dom_string *nameStr = nullptr;
    if (dom_node_get_node_name(node, &nameStr) == DOM_NO_ERR && nameStr) {
      outNode.tagName = domStringToStd(nameStr);
    }
    dom_element *elem = (dom_element *)node;
    outNode.id = getElementAttr(elem, "id");
    outNode.className = getElementAttr(elem, "class");
    outNode.href = getElementAttr(elem, "href");
    outNode.src = getElementAttr(elem, "src");
    if (outNode.src.empty()) {
      outNode.src = getElementAttr(elem, "data-src");
    }
  } else if (ntype == DOM_TEXT_NODE) {
    outNode.tagName = "#text";
    dom_string *valStr = nullptr;
    if (dom_node_get_node_value(node, &valStr) == DOM_NO_ERR && valStr) {
      outNode.textContent = domStringToStd(valStr);
    }
  }

  // Walk children
  dom_node *child = nullptr;
  if (dom_node_get_first_child(node, &child) == DOM_NO_ERR && child) {
    while (child) {
      NetSurfDomNode childOut;
      walkDomNode(child, childOut, totalNodes);
      if (!childOut.tagName.empty() || !childOut.textContent.empty()) {
        outNode.children.push_back(std::move(childOut));
      }

      dom_node *next = nullptr;
      dom_node_get_next_sibling(child, &next);
      dom_node_unref(child);
      child = next;
    }
  }
}

static uint32_t parseCssColor(const std::string& str) {
  if (str.empty()) return 0;
  if (str[0] == '#') {
    std::string hex = str.substr(1);
    if (hex.size() == 3) {
      char r = hex[0], g = hex[1], b = hex[2];
      hex = {r, r, g, g, b, b};
    }
    if (hex.size() == 6) {
      try {
        uint32_t val = (uint32_t)std::stoul(hex, nullptr, 16);
        return (val << 8) | 0xFF;
      } catch (...) {}
    }
  } else if (str.rfind("rgb", 0) == 0) {
    int r = 0, g = 0, b = 0;
    if (sscanf(str.c_str(), "rgb(%d,%d,%d)", &r, &g, &b) == 3 ||
        sscanf(str.c_str(), "rgb(%d, %d, %d)", &r, &g, &b) == 3) {
      return ((uint32_t)(r & 0xFF) << 24) | ((uint32_t)(g & 0xFF) << 16) |
             ((uint32_t)(b & 0xFF) << 8) | 0xFF;
    }
  }
  return 0;
}

// P23: grid — libcss bản này không parse grid nên làm thủ công.
// Parse "1fr 1fr", "repeat(3, 1fr)", "100px 25% 1fr" thành track list.
static void parseGridTracks(const std::string& value,
                            std::vector<NetSurfComputedStyle::GridTrack>& out) {
  out.clear();
  auto pushToken = [&](std::string tok) {
    while (!tok.empty() && std::isspace((unsigned char)tok.front())) tok.erase(tok.begin());
    while (!tok.empty() && std::isspace((unsigned char)tok.back())) tok.pop_back();
    if (tok.empty()) return;
    NetSurfComputedStyle::GridTrack t;
    try {
      if (tok.size() > 2 && tok.compare(tok.size() - 2, 2, "fr") == 0) {
        t.kind = 2;
        t.value = std::stof(tok.substr(0, tok.size() - 2));
      } else if (!tok.empty() && tok.back() == '%') {
        t.kind = 1;
        t.value = std::stof(tok.substr(0, tok.size() - 1));
      } else {
        t.kind = 0;
        std::string num = tok;
        if (num.size() > 2 && num.compare(num.size() - 2, 2, "px") == 0)
          num = num.substr(0, num.size() - 2);
        t.value = std::stof(num);
      }
    } catch (...) { return; }
    if (t.value < 0) t.value = 0;
    out.push_back(t);
  };
  // Tách token, giữ nguyên repeat(...) nguyên cụm
  size_t i = 0;
  while (i < value.size()) {
    while (i < value.size() && std::isspace((unsigned char)value[i])) i++;
    if (i >= value.size()) break;
    if (value.compare(i, 7, "repeat(") == 0) {
      size_t close = value.find(')', i);
      if (close == std::string::npos) break;
      std::string inside = value.substr(i + 7, close - i - 7);
      size_t comma = inside.find(',');
      int n = 1;
      std::string track = inside;
      if (comma != std::string::npos) {
        try { n = std::stoi(inside.substr(0, comma)); } catch (...) { n = 1; }
        track = inside.substr(comma + 1);
      }
      if (n < 1) n = 1;
      if (n > 12) n = 12;  // chống repeat(999,...) nổ layout
      for (int r = 0; r < n; r++) pushToken(track);
      i = close + 1;
    } else {
      size_t j = i;
      while (j < value.size() && !std::isspace((unsigned char)value[j])) j++;
      pushToken(value.substr(i, j - i));
      i = j;
    }
  }
}

// P23: libcss không biết display:grid — quét author CSS thủ công, match
// selector đơn giản (tag, .class, #id, tag.class) rồi gán grid cho node.
// Selector phức tạp (:hover, >, +, ~, [...]) thì bỏ qua cho an toàn.
static void applyGridFromAuthorCss(NetSurfStyledNode* root, const std::string& css) {
  if (!root || css.empty()) return;
  auto trimLower = [](std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
  };
  size_t pos = 0;
  while (pos < css.size()) {
    size_t openBrace = css.find('{', pos);
    if (openBrace == std::string::npos) break;
    size_t closeBrace = css.find('}', openBrace);
    if (closeBrace == std::string::npos) break;
    std::string selPart = css.substr(pos, openBrace - pos);
    std::string body = css.substr(openBrace + 1, closeBrace - openBrace - 1);
    pos = closeBrace + 1;
    std::string lowBody = body;
    std::transform(lowBody.begin(), lowBody.end(), lowBody.begin(), ::tolower);
    if (lowBody.find("display") == std::string::npos ||
        lowBody.find("grid") == std::string::npos)
      continue;
    std::unordered_map<std::string, std::string> decls;
    std::istringstream iss(body);
    std::string decl;
    while (std::getline(iss, decl, ';')) {
      size_t colon = decl.find(':');
      if (colon == std::string::npos) continue;
      decls[trimLower(decl.substr(0, colon))] = trimLower(decl.substr(colon + 1));
    }
    auto itD = decls.find("display");
    if (itD == decls.end() || (itD->second != "grid" && itD->second != "inline-grid"))
      continue;
    int gapPx = 0;
    auto itG = decls.find("gap");
    if (itG == decls.end()) itG = decls.find("grid-gap");
    if (itG == decls.end()) itG = decls.find("column-gap");
    if (itG != decls.end()) { try { gapPx = std::stoi(itG->second); } catch (...) {} }
    std::vector<NetSurfComputedStyle::GridTrack> cols;
    auto itC = decls.find("grid-template-columns");
    if (itC != decls.end()) parseGridTracks(itC->second, cols);
    std::istringstream sss(selPart);
    std::string sel;
    while (std::getline(sss, sel, ',')) {
      sel = trimLower(sel);
      if (sel.empty() || sel.find_first_of(":>+~[*") != std::string::npos) continue;
      std::function<void(NetSurfStyledNode*)> apply = [&](NetSurfStyledNode* n) {
        if (!n) return;
        bool match = false;
        if (sel[0] == '.') {
          std::string cls = sel.substr(1);
          std::istringstream cn(n->className);
          std::string c;
          while (cn >> c) { if (c == cls) { match = true; break; } }
        } else if (sel[0] == '#') {
          match = (n->id == sel.substr(1));
        } else {
          size_t dot = sel.find('.');
          std::string tag = (dot == std::string::npos) ? sel : sel.substr(0, dot);
          std::string cls = (dot == std::string::npos) ? "" : sel.substr(dot + 1);
          std::string ntag = n->tagName;
          std::transform(ntag.begin(), ntag.end(), ntag.begin(), ::tolower);
          if (ntag == tag) {
            if (cls.empty()) match = true;
            else {
              std::istringstream cn(n->className);
              std::string c;
              while (cn >> c) { if (c == cls) { match = true; break; } }
            }
          }
        }
        if (match) {
          n->style.display = 0x12;
          n->style.gapPx = gapPx;
          if (!cols.empty()) n->style.gridCols = cols;
        }
        for (auto& ch : n->children) apply(ch.get());
      };
      apply(root);
    }
  }
}

// Recursive walker to populate NetSurfStyledNode tree
static std::unique_ptr<NetSurfStyledNode>
buildStyledTree(dom_node *node, NetSurfStyledNode *parent,
                NetSurfStyledNode *prevSibling) {
  if (!node)
    return nullptr;

  dom_node_type ntype = DOM_DOCUMENT_NODE;
  dom_node_get_node_type(node, &ntype);

  auto styledNode = std::make_unique<NetSurfStyledNode>();
  styledNode->parent = parent;
  styledNode->prevSibling = prevSibling;

  if (ntype == DOM_ELEMENT_NODE) {
    dom_string *nameStr = nullptr;
    if (dom_node_get_node_name(node, &nameStr) == DOM_NO_ERR && nameStr) {
      styledNode->tagName = domStringToStd(nameStr);
      // Lowercase tag name
      std::transform(styledNode->tagName.begin(), styledNode->tagName.end(),
                     styledNode->tagName.begin(), ::tolower);
    }
    dom_element *elem = (dom_element *)node;
    styledNode->id = getElementAttr(elem, "id");
    styledNode->className = getElementAttr(elem, "class");
    styledNode->href = getElementAttr(elem, "href");
    styledNode->src = getElementAttr(elem, "src");
    if (styledNode->src.empty()) {
      styledNode->src = getElementAttr(elem, "data-src");
    }

    const std::string &tg = styledNode->tagName;
    // Video poster capture: catch 1st frame poster image
    if (tg == "video") {
      std::string poster = getElementAttr(elem, "poster");
      if (poster.empty()) poster = getElementAttr(elem, "data-poster");
      if (poster.empty()) poster = getElementAttr(elem, "data-thumb");
      if (!poster.empty()) {
        styledNode->src = poster;
      }
    }

    // P17: form controls — capture everything the layout engine and the
    // submit logic need, so later passes never touch libdom again.
    if (tg == "input" || tg == "button") {
      styledNode->inputType = getElementAttr(elem, "type");
      std::transform(styledNode->inputType.begin(),
                     styledNode->inputType.end(),
                     styledNode->inputType.begin(), ::tolower);
      if (styledNode->inputType.empty())
        styledNode->inputType = (tg == "input" ? "text" : "submit");
      styledNode->inputValue = getElementAttr(elem, "value");
      styledNode->inputName = getElementAttr(elem, "name");
    }
    if (tg == "input") {
      styledNode->inputPlaceholder = getElementAttr(elem, "placeholder");
      styledNode->inputChecked = hasElementAttr(elem, "checked");
      std::string ml = getElementAttr(elem, "maxlength");
      if (!ml.empty()) {
        try {
          styledNode->inputMaxLen = std::stoi(ml);
        } catch (...) {
        }
      }
    }
    if (tg == "textarea" || tg == "select") {
      styledNode->inputType = tg;
      styledNode->inputName = getElementAttr(elem, "name");
      styledNode->inputPlaceholder = getElementAttr(elem, "placeholder");
    }
    if (tg == "option") {
      // value attr wins; textContent is the fallback (gathered at layout).
      styledNode->inputValue = getElementAttr(elem, "value");
      styledNode->inputChecked = hasElementAttr(elem, "selected");
    }
    if (tg == "form") {
      styledNode->formAction = getElementAttr(elem, "action");
      styledNode->formMethod = getElementAttr(elem, "method");
      std::transform(styledNode->formMethod.begin(),
                     styledNode->formMethod.end(),
                     styledNode->formMethod.begin(), ::toupper);
      if (styledNode->formMethod.empty())
        styledNode->formMethod = "GET";
    }

    // P26: lưu raw style="" để libcss parse ở computeStylesRecursive
    // (thay parser tay — libcss hiểu đúng % vs px, float, mọi thuộc tính).
    // Chỉ giữ parse thủ công cho grid-template-columns (libcss không có grid).
    std::string styleAttr = getElementAttr(elem, "style");
    if (!styleAttr.empty()) {
      styledNode->styleAttr = styleAttr;
      // Grid: libcss không hỗ trợ — parse thủ công từ style attr.
      // (gap cũng parse ở applyInlineStyleOverride, sau libcss)
      std::string low = styleAttr;
      std::transform(low.begin(), low.end(), low.begin(), ::tolower);
      size_t gp = low.find("grid-template-columns");
      if (gp != std::string::npos) {
        size_t colon = low.find(':', gp);
        size_t semi = low.find(';', colon);
        if (colon != std::string::npos) {
          std::string gv = styleAttr.substr(colon + 1,
              (semi == std::string::npos ? std::string::npos : semi - colon - 1));
          parseGridTracks(gv, styledNode->style.gridCols);
        }
      }
    }
  } else if (ntype == DOM_TEXT_NODE) {
    styledNode->tagName = "#text";
    dom_string *valStr = nullptr;
    if (dom_node_get_node_value(node, &valStr) == DOM_NO_ERR && valStr) {
      styledNode->textContent = domStringToStd(valStr);
    }
  }

  // Recursively build children
  dom_node *child = nullptr;
  if (dom_node_get_first_child(node, &child) == DOM_NO_ERR && child) {
    NetSurfStyledNode *prevChild = nullptr;
    while (child) {
      auto childStyled = buildStyledTree(child, styledNode.get(), prevChild);
      if (childStyled && (!childStyled->tagName.empty() ||
                          !childStyled->textContent.empty())) {
        prevChild = childStyled.get();
        styledNode->children.push_back(std::move(childStyled));
      }

      dom_node *next = nullptr;
      dom_node_get_next_sibling(child, &next);
      dom_node_unref(child);
      child = next;
    }
  }

  return styledNode;
}

// ---------------------------------------------------------------------------
// libcss Selection Callbacks
// ---------------------------------------------------------------------------

static css_error resolve_url(void *pw, const char *base, lwc_string *rel,
                             lwc_string **abs) {
  (void)pw;
  (void)base;
  *abs = lwc_string_ref(rel);
  return CSS_OK;
}

static css_error cb_node_name(void *pw, void *n, css_qname *qname) {
  (void)pw;
  auto *node = (NetSurfStyledNode *)n;
  lwc_intern_string(node->tagName.c_str(), node->tagName.size(), &qname->name);
  return CSS_OK;
}

static css_error cb_node_classes(void *pw, void *n, lwc_string ***classes,
                                 uint32_t *n_classes) {
  (void)pw;
  auto *node = (NetSurfStyledNode *)n;
  if (node->className.empty()) {
    *classes = nullptr;
    *n_classes = 0;
    return CSS_OK;
  }

  // Split classes by whitespace
  std::vector<std::string> tokens;
  std::istringstream iss(node->className);
  std::string t;
  while (iss >> t)
    tokens.push_back(t);

  if (tokens.empty()) {
    *classes = nullptr;
    *n_classes = 0;
    return CSS_OK;
  }

  lwc_string **arr =
      (lwc_string **)malloc(tokens.size() * sizeof(lwc_string *));
  for (size_t i = 0; i < tokens.size(); ++i) {
    lwc_intern_string(tokens[i].c_str(), tokens[i].size(), &arr[i]);
  }
  *classes = arr;
  *n_classes = (uint32_t)tokens.size();
  return CSS_OK;
}

static css_error cb_node_id(void *pw, void *n, lwc_string **id) {
  (void)pw;
  auto *node = (NetSurfStyledNode *)n;
  if (node->id.empty()) {
    *id = nullptr;
    return CSS_OK;
  }
  lwc_intern_string(node->id.c_str(), node->id.size(), id);
  return CSS_OK;
}

static css_error cb_parent_node(void *pw, void *n, void **parent) {
  (void)pw;
  auto *node = (NetSurfStyledNode *)n;
  *parent = node->parent;
  return CSS_OK;
}

static css_error cb_sibling_node(void *pw, void *n, void **sibling) {
  (void)pw;
  auto *node = (NetSurfStyledNode *)n;
  *sibling = node->prevSibling;
  return CSS_OK;
}

static css_error cb_node_has_name(void *pw, void *n, const css_qname *qname,
                                  bool *match) {
  (void)pw;
  auto *node = (NetSurfStyledNode *)n;
  const char *reqName = lwc_string_data(qname->name);
  size_t reqLen = lwc_string_length(qname->name);
  std::string req(reqName, reqLen);
  std::transform(req.begin(), req.end(), req.begin(), ::tolower);
  *match = (node->tagName == req);
  return CSS_OK;
}

static css_error cb_node_has_class(void *pw, void *n, lwc_string *name,
                                   bool *match) {
  (void)pw;
  auto *node = (NetSurfStyledNode *)n;
  if (node->className.empty()) {
    *match = false;
    return CSS_OK;
  }
  const char *cname = lwc_string_data(name);
  size_t clen = lwc_string_length(name);
  std::string targetClass(cname, clen);

  std::istringstream iss(node->className);
  std::string token;
  *match = false;
  while (iss >> token) {
    if (token == targetClass) {
      *match = true;
      break;
    }
  }
  return CSS_OK;
}

static css_error cb_node_has_id(void *pw, void *n, lwc_string *name,
                                bool *match) {
  (void)pw;
  auto *node = (NetSurfStyledNode *)n;
  if (node->id.empty()) {
    *match = false;
    return CSS_OK;
  }
  const char *idname = lwc_string_data(name);
  size_t idlen = lwc_string_length(name);
  *match = (node->id == std::string(idname, idlen));
  return CSS_OK;
}

static css_error cb_node_has_attribute(void *pw, void *n,
                                       const css_qname *qname, bool *match) {
  (void)pw;
  auto *node = (NetSurfStyledNode *)n;
  const char *aname = lwc_string_data(qname->name);
  if (strcmp(aname, "href") == 0)
    *match = !node->href.empty();
  else if (strcmp(aname, "src") == 0)
    *match = !node->src.empty();
  else if (strcmp(aname, "id") == 0)
    *match = !node->id.empty();
  else if (strcmp(aname, "class") == 0)
    *match = !node->className.empty();
  else
    *match = false;
  return CSS_OK;
}

static css_error cb_node_has_attribute_equal(void *pw, void *n,
                                             const css_qname *qname,
                                             lwc_string *value, bool *match) {
  (void)pw;
  auto *node = (NetSurfStyledNode *)n;
  const char *aname = lwc_string_data(qname->name);
  const char *val = lwc_string_data(value);
  if (strcmp(aname, "href") == 0)
    *match = (node->href == val);
  else if (strcmp(aname, "id") == 0)
    *match = (node->id == val);
  else if (strcmp(aname, "class") == 0)
    *match = (node->className == val);
  else
    *match = false;
  return CSS_OK;
}

static css_error cb_node_is_root(void *pw, void *n, bool *match) {
  (void)pw;
  auto *node = (NetSurfStyledNode *)n;
  *match = (node->parent == nullptr);
  return CSS_OK;
}

static inline uint32_t cssColorToRgba(css_color c) {
  // libcss css_color is 0xAARRGGBB -> convert to RomCloud 0xRRGGBBAA
  uint32_t a = (c >> 24) & 0xFF;
  uint32_t r = (c >> 16) & 0xFF;
  uint32_t g = (c >> 8) & 0xFF;
  uint32_t b = c & 0xFF;
  return (r << 24) | (g << 16) | (b << 8) | a;
}

static css_error cb_ua_default_for_property(void *pw, uint32_t property,
                                            css_hint *hint) {
  (void)pw;
  if (property == CSS_PROP_COLOR) {
    // Charcoal text default (AARRGGBB in libcss: Alpha=0xFF, R=0x22, G=0x22, B=0x22)
    // NEVER 0xFFFFFFFF white, because canvas background is white!
    hint->data.color = 0xFF222222;
    hint->status = CSS_COLOR_COLOR;
  } else if (property == CSS_PROP_FONT_FAMILY) {
    hint->data.strings = nullptr;
    hint->status = CSS_FONT_FAMILY_SANS_SERIF;
  } else if (property == CSS_PROP_QUOTES) {
    hint->data.strings = nullptr;
    hint->status = CSS_QUOTES_NONE;
  } else if (property == CSS_PROP_VOICE_FAMILY) {
    hint->data.strings = nullptr;
    hint->status = 0;
  } else {
    return CSS_INVALID;
  }
  return CSS_OK;
}

static css_error stub_named_ancestor(void *pw, void *n, const css_qname *q,
                                     void **a) {
  (void)pw;
  (void)n;
  (void)q;
  *a = nullptr;
  return CSS_OK;
}
static css_error stub_named_parent(void *pw, void *n, const css_qname *q,
                                   void **p) {
  (void)pw;
  (void)n;
  (void)q;
  *p = nullptr;
  return CSS_OK;
}
static css_error stub_named_sibling(void *pw, void *n, const css_qname *q,
                                    void **s) {
  (void)pw;
  (void)n;
  (void)q;
  *s = nullptr;
  return CSS_OK;
}
static css_error stub_named_generic_sibling(void *pw, void *n,
                                            const css_qname *q, void **s) {
  (void)pw;
  (void)n;
  (void)q;
  *s = nullptr;
  return CSS_OK;
}
static css_error stub_has_attr_dashmatch(void *pw, void *n, const css_qname *q,
                                         lwc_string *v, bool *m) {
  (void)pw;
  (void)n;
  (void)q;
  (void)v;
  *m = false;
  return CSS_OK;
}
static css_error stub_has_attr_includes(void *pw, void *n, const css_qname *q,
                                        lwc_string *v, bool *m) {
  (void)pw;
  (void)n;
  (void)q;
  (void)v;
  *m = false;
  return CSS_OK;
}
static css_error stub_has_attr_prefix(void *pw, void *n, const css_qname *q,
                                      lwc_string *v, bool *m) {
  (void)pw;
  (void)n;
  (void)q;
  (void)v;
  *m = false;
  return CSS_OK;
}
static css_error stub_has_attr_suffix(void *pw, void *n, const css_qname *q,
                                      lwc_string *v, bool *m) {
  (void)pw;
  (void)n;
  (void)q;
  (void)v;
  *m = false;
  return CSS_OK;
}
static css_error stub_has_attr_substring(void *pw, void *n, const css_qname *q,
                                         lwc_string *v, bool *m) {
  (void)pw;
  (void)n;
  (void)q;
  (void)v;
  *m = false;
  return CSS_OK;
}
static css_error stub_count_siblings(void *pw, void *n, bool sn, bool aft,
                                     int32_t *c) {
  (void)pw;
  (void)n;
  (void)sn;
  (void)aft;
  *c = 1;
  return CSS_OK;
}
static css_error stub_is_empty(void *pw, void *n, bool *m) {
  (void)pw;
  (void)n;
  *m = false;
  return CSS_OK;
}
static css_error stub_is_link(void *pw, void *n, bool *m) {
  (void)pw;
  auto *node = (NetSurfStyledNode *)n;
  *m = (!node->href.empty());
  return CSS_OK;
}
static css_error stub_is_visited(void *pw, void *n, bool *m) {
  (void)pw;
  (void)n;
  *m = false;
  return CSS_OK;
}
static css_error stub_is_hover(void *pw, void *n, bool *m) {
  (void)pw;
  (void)n;
  *m = false;
  return CSS_OK;
}
static css_error stub_is_active(void *pw, void *n, bool *m) {
  (void)pw;
  (void)n;
  *m = false;
  return CSS_OK;
}
static css_error stub_is_focus(void *pw, void *n, bool *m) {
  (void)pw;
  (void)n;
  *m = false;
  return CSS_OK;
}
static css_error stub_is_enabled(void *pw, void *n, bool *m) {
  (void)pw;
  (void)n;
  *m = true;
  return CSS_OK;
}
static css_error stub_is_disabled(void *pw, void *n, bool *m) {
  (void)pw;
  (void)n;
  *m = false;
  return CSS_OK;
}
static css_error stub_is_checked(void *pw, void *n, bool *m) {
  (void)pw;
  (void)n;
  *m = false;
  return CSS_OK;
}
static css_error stub_is_target(void *pw, void *n, bool *m) {
  (void)pw;
  (void)n;
  *m = false;
  return CSS_OK;
}
static css_error stub_is_lang(void *pw, void *n, lwc_string *l, bool *m) {
  (void)pw;
  (void)n;
  (void)l;
  *m = false;
  return CSS_OK;
}
static css_error stub_presentational_hint(void *pw, void *n, uint32_t *nh,
                                          css_hint **h) {
  (void)pw;
  (void)n;
  *nh = 0;
  *h = nullptr;
  return CSS_OK;
}

static css_error stub_set_node_data(void *pw, void *n, void *data) {
  (void)pw;
  (void)n;
  (void)data;
  return CSS_OK;
}

static css_error stub_get_node_data(void *pw, void *n, void **data) {
  (void)pw;
  (void)n;
  *data = nullptr;
  return CSS_OK;
}

static css_select_handler g_selectHandler = {
    CSS_SELECT_HANDLER_VERSION_1,
    cb_node_name,
    cb_node_classes,
    cb_node_id,
    stub_named_ancestor,
    stub_named_parent,
    stub_named_sibling,
    stub_named_generic_sibling,
    cb_parent_node,
    cb_sibling_node,
    cb_node_has_name,
    cb_node_has_class,
    cb_node_has_id,
    cb_node_has_attribute,
    cb_node_has_attribute_equal,
    stub_has_attr_dashmatch,
    stub_has_attr_includes,
    stub_has_attr_prefix,
    stub_has_attr_suffix,
    stub_has_attr_substring,
    cb_node_is_root,
    stub_count_siblings,
    stub_is_empty,
    stub_is_link,
    stub_is_visited,
    stub_is_hover,
    stub_is_active,
    stub_is_focus,
    stub_is_enabled,
    stub_is_disabled,
    stub_is_checked,
    stub_is_target,
    stub_is_lang,
    stub_presentational_hint,
    cb_ua_default_for_property,
    stub_set_node_data,
    stub_get_node_data,
};

// P26: apply style="" bằng parser tay, chạy SAU computeStylesRecursive
// (inline style override libcss — đúng thứ tự cascade).
// Chỉ parse những gì layout cần; % thì bỏ qua (để auto).
static void applyInlineStyleOverride(NetSurfStyledNode* node) {
    if (!node) return;
    // P26-fix: đệ quy con cháu LUÔN, kể cả khi node hiện tại không có styleAttr.
    if (!node->styleAttr.empty()) {
    const std::string& styleAttr = node->styleAttr;
    std::string low = styleAttr;
    std::transform(low.begin(), low.end(), low.begin(), ::tolower);

    auto getDecl = [&](const char* name) -> std::string {
        size_t p = low.find(name);
        if (p == std::string::npos) return "";
        size_t colon = low.find(':', p);
        if (colon == std::string::npos) return "";
        size_t semi = low.find(';', colon);
        std::string v = styleAttr.substr(colon + 1,
            (semi == std::string::npos ? std::string::npos : semi - colon - 1));
        while (!v.empty() && std::isspace((unsigned char)v.front())) v.erase(v.begin());
        while (!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        return v;
    };

    std::string v;
    v = getDecl("display");
    if (v == "flex") node->style.display = 0x11;
    else if (v == "grid") node->style.display = 0x12;
    else if (v == "none") node->style.display = 0x10;
    else if (v == "block") node->style.display = 0x02;
    else if (v == "inline") node->style.display = 0x01;

    v = getDecl("float");
    if (v == "left") node->style.floatSide = 1;
    else if (v == "right") node->style.floatSide = 2;

    v = getDecl("width");
    if (!v.empty() && v.find('%') == std::string::npos) {
        try { node->style.widthPx = std::stoi(v); } catch (...) {}
    }
    v = getDecl("height");
    if (!v.empty() && v.find('%') == std::string::npos) {
        try { node->style.heightPx = std::stoi(v); } catch (...) {}
    }
    v = getDecl("gap");
    if (!v.empty()) {
        try { node->style.gapPx = std::stoi(v); } catch (...) {}
    }
    // grid-template-columns đã parse ở buildStyledTree.

    // ============ P21: extended CSS properties (helpers) ============
    // getDeclStrict — tránh "border" khớp nhầm "border-top" / "padding-top" / v.v.
    auto getDeclStrict = [&](const char* name) -> std::string {
        std::string key = std::string(name) + ":";
        size_t p = 0;
        while (true) {
            size_t found = low.find(key, p);
            if (found == std::string::npos) return "";
            if (found == 0 || low[found - 1] == ';') {
                size_t colon = found + std::strlen(name);
                size_t semi = low.find(';', colon);
                std::string dv = styleAttr.substr(colon + 1,
                    (semi == std::string::npos ? std::string::npos : semi - colon - 1));
                while (!dv.empty() && std::isspace((unsigned char)dv.front())) dv.erase(dv.begin());
                while (!dv.empty() && std::isspace((unsigned char)dv.back())) dv.pop_back();
                std::transform(dv.begin(), dv.end(), dv.begin(), ::tolower);
                return dv;
            }
            p = found + 1;
        }
    };
    auto getDeclListStrict = [&](const char* name) -> std::vector<std::string> {
        std::string s = getDeclStrict(name);
        std::vector<std::string> out;
        if (s.empty()) return out;
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
            size_t j = i;
            while (j < s.size() && !std::isspace((unsigned char)s[j])) j++;
            if (j > i) out.push_back(s.substr(i, j - i));
            i = j;
        }
        return out;
    };
    auto parsePxVal = [](const std::string& t) -> int {
        if (t.empty()) return -1;
        try {
            std::string num;
            for (char c : t) {
                if (c == '-' || c == '+' || std::isdigit((unsigned char)c)) num += c;
                else break;
            }
            if (num.empty() || num == "-" || num == "+") return -1;
            return std::stoi(num);
        } catch (...) { return -1; }
    };
    auto parseColorInline = [](const std::string& t) -> uint32_t {
        if (t.empty()) return 0;
        if (t == "transparent") return 0x00000000;
        if (t[0] == '#') {
            std::string hex = t.substr(1);
            if (hex.size() == 3) { char r=hex[0],g=hex[1],b=hex[2]; hex = {r,r,g,g,b,b}; }
            if (hex.size() == 6) {
                try { return ((uint32_t)std::stoul(hex, nullptr, 16) << 8) | 0xFF; } catch (...) {}
            }
        }
        return 0;
    };
    auto parseBorderStyleInline = [](const std::string& t) -> uint8_t {
        if (t == "solid") return 1;
        if (t == "dashed") return 2;
        if (t == "dotted") return 3;
        if (t == "double") return 4;
        if (t == "groove" || t == "ridge" || t == "inset" || t == "outset") return 5;
        return 0;
    };

    // P21: min-height, text-indent, letter-spacing, word-spacing
    {
        std::string v = getDeclStrict("min-height");
        if (!v.empty()) { int px = parsePxVal(v); if (px >= 0) node->style.minHeightPx = px; }
    }
    {
        std::string v = getDeclStrict("text-indent");
        if (!v.empty()) { int px = parsePxVal(v); if (px >= 0) node->style.textIndentPx = px; }
    }
    {
        std::string v = getDeclStrict("letter-spacing");
        if (!v.empty()) {
            if (v == "normal") node->style.letterSpacingPx = 0;
            else { int px = parsePxVal(v); if (px >= 0) node->style.letterSpacingPx = px; }
        }
    }
    {
        std::string v = getDeclStrict("word-spacing");
        if (!v.empty()) {
            if (v == "normal") node->style.wordSpacingPx = 0;
            else { int px = parsePxVal(v); if (px >= 0) node->style.wordSpacingPx = px; }
        }
    }

    // P21: padding shorthand (1-4 values; longhand padding-top/... đã có sẵn
    // nếu có cascade trước — inline-style này override toàn bộ 4 cạnh).
    {
        auto tok = getDeclListStrict("padding");
        if (!tok.empty()) {
            int vals[4] = {0, 0, 0, 0};
            int got = 0;
            for (int i = 0; i < 4 && i < (int)tok.size(); i++) {
                int px = parsePxVal(tok[i]);
                if (px >= 0) { vals[i] = px; got++; }
            }
            if (got > 0) {
                switch (tok.size()) {
                    case 1: vals[1] = vals[2] = vals[3] = vals[0]; break;
                    case 2: vals[2] = vals[0]; vals[3] = vals[1]; break;
                    case 3: vals[3] = vals[1]; break;
                    default: break;
                }
                node->style.paddingTopPx = vals[0];
                node->style.paddingRightPx = vals[1];
                node->style.paddingBottomPx = vals[2];
                node->style.paddingLeftPx = vals[3];
            }
        }
    }

    // P21: border shorthand + per-side longhand
    {
        auto applySide = [&](const char* name, int& wPx, uint32_t& col, uint8_t& sty) {
            auto tok = getDeclListStrict(name);
            if (tok.empty()) return;
            for (const auto& t : tok) {
                int px = parsePxVal(t);
                if (px >= 0) {
                    wPx = px;
                } else {
                    uint8_t s = parseBorderStyleInline(t);
                    if (s) sty = s;
                    else {
                        uint32_t c = parseColorInline(t);
                        if (c) col = c;
                    }
                }
            }
        };
        // shorthand "border" áp cho 4 cạnh; longhand sau đó đè từng cạnh.
        applySide("border", node->style.borderTopWidthPx, node->style.borderTopColor, node->style.borderTopStyle);
        applySide("border", node->style.borderRightWidthPx, node->style.borderRightColor, node->style.borderRightStyle);
        applySide("border", node->style.borderBottomWidthPx, node->style.borderBottomColor, node->style.borderBottomStyle);
        applySide("border", node->style.borderLeftWidthPx, node->style.borderLeftColor, node->style.borderLeftStyle);
        applySide("border-top", node->style.borderTopWidthPx, node->style.borderTopColor, node->style.borderTopStyle);
        applySide("border-right", node->style.borderRightWidthPx, node->style.borderRightColor, node->style.borderRightStyle);
        applySide("border-bottom", node->style.borderBottomWidthPx, node->style.borderBottomColor, node->style.borderBottomStyle);
        applySide("border-left", node->style.borderLeftWidthPx, node->style.borderLeftColor, node->style.borderLeftStyle);
    }

    // P21: outline shorthand (width + style + color, không có per-side).
    {
        auto tok = getDeclListStrict("outline");
        if (!tok.empty()) {
            for (const auto& t : tok) {
                int px = parsePxVal(t);
                if (px >= 0) {
                    node->style.outlineWidthPx = px;
                } else {
                    uint8_t s = parseBorderStyleInline(t);
                    if (s) node->style.outlineStyle = s;
                    else {
                        uint32_t c = parseColorInline(t);
                        if (c) node->style.outlineColor = c;
                    }
                }
            }
        }
    }

    // P21: list-style-position
    {
        std::string v = getDeclStrict("list-style-position");
        if (v == "inside") node->style.listStylePosition = 1;
        else if (v == "outside") node->style.listStylePosition = 0;
    }
    }

    for (auto& ch : node->children) applyInlineStyleOverride(ch.get());
}

static void computeStylesRecursive(NetSurfStyledNode *node,
                                   css_select_ctx *selectCtx,
                                   css_unit_ctx *unitCtx, css_media *media,
                                   int& nodeCount) {
  if (!node || nodeCount > 1200)
    return;
  nodeCount++;

  if (!node->tagName.empty() && node->tagName != "#text") {
    lwc_string *tagLwc = nullptr;
    lwc_intern_string(node->tagName.c_str(), node->tagName.size(), &tagLwc);

    css_select_results *results = nullptr;
    css_error err = css_select_style(selectCtx, node, unitCtx, media, nullptr,
                                     &g_selectHandler, nullptr, &results);

    if (err == CSS_OK && results && results->styles[CSS_PSEUDO_ELEMENT_NONE]) {
      const css_computed_style *s = results->styles[CSS_PSEUDO_ELEMENT_NONE];

      // 1. Display
      node->style.display = css_computed_display(s, node->parent == nullptr);

      // 2. Flex properties
      node->style.flexDirection = css_computed_flex_direction(s);
      node->style.alignItems = css_computed_align_items(s);
      node->style.justifyContent = css_computed_justify_content(s);

      css_fixed fval = 0;
      if (css_computed_flex_grow(s, &fval) == CSS_FLEX_GROW_SET) {
        node->style.flexGrow = (float)fval / (float)(1 << CSS_RADIX_POINT);
      }
      if (css_computed_flex_shrink(s, &fval) == CSS_FLEX_SHRINK_SET) {
        node->style.flexShrink = (float)fval / (float)(1 << CSS_RADIX_POINT);
      }

      // 3. Width & Height
      css_unit u = CSS_UNIT_PX;
      if (css_computed_width(s, &fval, &u) == CSS_WIDTH_SET) {
        if (u == CSS_UNIT_PX)
          node->style.widthPx = (fval >> CSS_RADIX_POINT);
      }
      if (css_computed_height(s, &fval, &u) == CSS_HEIGHT_SET) {
        if (u == CSS_UNIT_PX)
          node->style.heightPx = (fval >> CSS_RADIX_POINT);
      }

      // 4. Margins
      if (css_computed_margin_top(s, &fval, &u) == CSS_MARGIN_SET &&
          u == CSS_UNIT_PX) {
        node->style.marginTopPx = (fval >> CSS_RADIX_POINT);
      }
      if (css_computed_margin_right(s, &fval, &u) == CSS_MARGIN_SET &&
          u == CSS_UNIT_PX) {
        node->style.marginRightPx = (fval >> CSS_RADIX_POINT);
      }
      if (css_computed_margin_bottom(s, &fval, &u) == CSS_MARGIN_SET &&
          u == CSS_UNIT_PX) {
        node->style.marginBottomPx = (fval >> CSS_RADIX_POINT);
      }
      if (css_computed_margin_left(s, &fval, &u) == CSS_MARGIN_SET &&
          u == CSS_UNIT_PX) {
        node->style.marginLeftPx = (fval >> CSS_RADIX_POINT);
      }

      // 5. Paddings
      if (css_computed_padding_top(s, &fval, &u) == CSS_PADDING_SET &&
          u == CSS_UNIT_PX) {
        node->style.paddingTopPx = (fval >> CSS_RADIX_POINT);
      }
      if (css_computed_padding_right(s, &fval, &u) == CSS_PADDING_SET &&
          u == CSS_UNIT_PX) {
        node->style.paddingRightPx = (fval >> CSS_RADIX_POINT);
      }
      if (css_computed_padding_bottom(s, &fval, &u) == CSS_PADDING_SET &&
          u == CSS_UNIT_PX) {
        node->style.paddingBottomPx = (fval >> CSS_RADIX_POINT);
      }
      if (css_computed_padding_left(s, &fval, &u) == CSS_PADDING_SET &&
          u == CSS_UNIT_PX) {
        node->style.paddingLeftPx = (fval >> CSS_RADIX_POINT);
      }

      // 6. Colors
      css_color colorVal;
      if (css_computed_color(s, &colorVal) == CSS_COLOR_COLOR) {
        node->style.color = cssColorToRgba(colorVal);
      }
      if (css_computed_background_color(s, &colorVal) ==
          CSS_BACKGROUND_COLOR_COLOR) {
        node->style.backgroundColor = cssColorToRgba(colorVal);
      }

      // 7. Font size & Weight
      if (css_computed_font_size(s, &fval, &u) == CSS_FONT_SIZE_DIMENSION &&
          u == CSS_UNIT_PX) {
        node->style.fontSizePx = (fval >> CSS_RADIX_POINT);
      }
      node->style.fontWeight = css_computed_font_weight(s);

      // 8. Text align
      node->style.textAlign = css_computed_text_align(s);

      // 9. P21: min-height (chỉ px; auto = -1)
      if (css_computed_min_height(s, &fval, &u) == CSS_MIN_HEIGHT_SET &&
          u == CSS_UNIT_PX) {
        node->style.minHeightPx = (fval >> CSS_RADIX_POINT);
      }
      // 10. P21: text-indent
      if (css_computed_text_indent(s, &fval, &u) == CSS_TEXT_INDENT_SET &&
          u == CSS_UNIT_PX) {
        node->style.textIndentPx = (fval >> CSS_RADIX_POINT);
      }
      // 11. P21: letter-spacing
      if (css_computed_letter_spacing(s, &fval, &u) == CSS_LETTER_SPACING_SET &&
          u == CSS_UNIT_PX) {
        node->style.letterSpacingPx = (fval >> CSS_RADIX_POINT);
      }
      // 12. P21: word-spacing
      if (css_computed_word_spacing(s, &fval, &u) == CSS_WORD_SPACING_SET &&
          u == CSS_UNIT_PX) {
        node->style.wordSpacingPx = (fval >> CSS_RADIX_POINT);
      }
      // 13. P21: border per-side widths (CSS_BORDER_WIDTH_WIDTH = 0x4 = đã set px)
      if (css_computed_border_top_width(s, &fval, &u) == CSS_BORDER_WIDTH_WIDTH &&
          u == CSS_UNIT_PX) {
        node->style.borderTopWidthPx = (fval >> CSS_RADIX_POINT);
      }
      if (css_computed_border_right_width(s, &fval, &u) == CSS_BORDER_WIDTH_WIDTH &&
          u == CSS_UNIT_PX) {
        node->style.borderRightWidthPx = (fval >> CSS_RADIX_POINT);
      }
      if (css_computed_border_bottom_width(s, &fval, &u) == CSS_BORDER_WIDTH_WIDTH &&
          u == CSS_UNIT_PX) {
        node->style.borderBottomWidthPx = (fval >> CSS_RADIX_POINT);
      }
      if (css_computed_border_left_width(s, &fval, &u) == CSS_BORDER_WIDTH_WIDTH &&
          u == CSS_UNIT_PX) {
        node->style.borderLeftWidthPx = (fval >> CSS_RADIX_POINT);
      }
      // 14. P21: border per-side colors
      if (css_computed_border_top_color(s, &colorVal) == CSS_BORDER_COLOR_COLOR) {
        node->style.borderTopColor = cssColorToRgba(colorVal);
      }
      if (css_computed_border_right_color(s, &colorVal) == CSS_BORDER_COLOR_COLOR) {
        node->style.borderRightColor = cssColorToRgba(colorVal);
      }
      if (css_computed_border_bottom_color(s, &colorVal) == CSS_BORDER_COLOR_COLOR) {
        node->style.borderBottomColor = cssColorToRgba(colorVal);
      }
      if (css_computed_border_left_color(s, &colorVal) == CSS_BORDER_COLOR_COLOR) {
        node->style.borderLeftColor = cssColorToRgba(colorVal);
      }
      // 14b. P21: border per-side styles (solid/dashed/...)
      node->style.borderTopStyle = css_computed_border_top_style(s);
      node->style.borderRightStyle = css_computed_border_right_style(s);
      node->style.borderBottomStyle = css_computed_border_bottom_style(s);
      node->style.borderLeftStyle = css_computed_border_left_style(s);
      // 15. P21: outline width/color/style (CSS_OUTLINE_WIDTH_WIDTH = 0x4 = đã set px)
      if (css_computed_outline_width(s, &fval, &u) == CSS_OUTLINE_WIDTH_WIDTH &&
          u == CSS_UNIT_PX) {
        node->style.outlineWidthPx = (fval >> CSS_RADIX_POINT);
      }
      if (css_computed_outline_color(s, &colorVal) == CSS_OUTLINE_COLOR_COLOR) {
        node->style.outlineColor = cssColorToRgba(colorVal);
      }
      node->style.outlineStyle = css_computed_outline_style(s);
      // 16. P21: list-style-position
      node->style.listStylePosition = css_computed_list_style_position(s);
    }

    if (results)
      css_select_results_destroy(results);
    if (tagLwc)
      lwc_string_unref(tagLwc);
  }

  // Recurse to children
  for (auto &child : node->children) {
    computeStylesRecursive(child.get(), selectCtx, unitCtx, media, nodeCount);
  }
}
#endif

bool NetSurfBridge::parseToTree(const std::string &html,
                                NetSurfDomNode &outRootNode,
                                int &outTotalNodes) {
  init();
  outTotalNodes = 0;
  outRootNode = NetSurfDomNode{};

  if (html.empty())
    return false;

#if HAVE_NETSURF_LIBS
  dom_hubbub_parser_params params;
  std::memset(&params, 0, sizeof(params));
  params.enc = "UTF-8";
  params.fix_enc = true;
  params.enable_script = false;

  dom_hubbub_parser *parser = nullptr;
  dom_document *doc = nullptr;
  dom_hubbub_error err = dom_hubbub_parser_create(&params, &parser, &doc);
  if (err != DOM_HUBBUB_OK || !parser || !doc) {
    if (parser)
      dom_hubbub_parser_destroy(parser);
    return false;
  }

  dom_hubbub_parser_parse_chunk(parser, (const uint8_t *)html.data(),
                                html.size());
  dom_hubbub_parser_completed(parser);
  dom_hubbub_parser_destroy(parser);

  // Walk document tree
  walkDomNode((dom_node *)doc, outRootNode, outTotalNodes);

  // Release DOM document
  dom_node_unref((dom_node *)doc);
  return true;
#else
    // Fallback for host PC simulator
    std::unique_ptr<NetSurfStyledNode> styledRoot;
    NetSurfBridge::instance().parseAndStyle(html, "", styledRoot);
    outTotalNodes = 0;
    std::function<void(const NetSurfStyledNode *, NetSurfDomNode &)>
        convertNode = [&](const NetSurfStyledNode *sn, NetSurfDomNode &dn) {
          if (!sn)
            return;
          outTotalNodes++;
          dn.tagName = sn->tagName.empty() ? "#text" : sn->tagName;
          dn.textContent = sn->textContent;
          dn.className = sn->className;
          dn.id = sn->id;
          dn.href = sn->href;
          dn.src = sn->src;
          for (const auto &ch : sn->children) {
            dn.children.emplace_back();
            convertNode(ch.get(), dn.children.back());
          }
        };
    if (styledRoot)
      convertNode(styledRoot.get(), outRootNode);
    return true;
#endif
}

bool NetSurfBridge::parseHtml(const std::string &html,
                              std::string &outSummary) {
  NetSurfDomNode root;
  int totalNodes = 0;
  if (!parseToTree(html, root, totalNodes)) {
    outSummary = "NetSurfBridge: Parse failed";
    return false;
  }

  int elementCount = 0;
  int textCount = 0;
  int imgCount = 0;
  int linkCount = 0;

  std::function<void(const NetSurfDomNode &)> countStats =
      [&](const NetSurfDomNode &n) {
        if (n.tagName == "#text") {
          textCount++;
        } else {
          elementCount++;
          if (n.tagName == "img" || n.tagName == "IMG")
            imgCount++;
          if (n.tagName == "a" || n.tagName == "A")
            linkCount++;
        }
        for (const auto &c : n.children) {
          countStats(c);
        }
      };
  countStats(root);

  std::ostringstream oss;
  oss << "W3C DOM parsed successfully: " << totalNodes << " total nodes ("
      << elementCount << " elements, " << textCount << " text nodes, "
      << imgCount << " images, " << linkCount << " links)";
  outSummary = oss.str();
  return true;
}

bool NetSurfBridge::parseAndStyle(const std::string &html,
                                  const std::string &extraCss,
                                  std::unique_ptr<NetSurfStyledNode> &outRoot,
                                  const std::string &baseUrl) {
  init();
  outRoot = nullptr;
  if (html.empty())
    return false;

#if HAVE_NETSURF_LIBS
  dom_hubbub_parser_params params;
  std::memset(&params, 0, sizeof(params));
  params.enc = "UTF-8";
  params.fix_enc = true;
  params.enable_script = false;

  dom_hubbub_parser *parser = nullptr;
  dom_document *doc = nullptr;
  dom_hubbub_error err = dom_hubbub_parser_create(&params, &parser, &doc);
  if (err != DOM_HUBBUB_OK || !parser || !doc) {
    if (parser)
      dom_hubbub_parser_destroy(parser);
    return false;
  }

  dom_hubbub_parser_parse_chunk(parser, (const uint8_t *)html.data(),
                                html.size());
  dom_hubbub_parser_completed(parser);
  dom_hubbub_parser_destroy(parser);

  // Build C++ tree from W3C DOM document
  outRoot = buildStyledTree((dom_node *)doc, nullptr, nullptr);
  dom_node_unref((dom_node *)doc);

  if (!outRoot)
    return false;

  // Create selection context
  css_select_ctx *selectCtx = nullptr;
  if (css_select_ctx_create(&selectCtx) != CSS_OK || !selectCtx) {
    return false;
  }

  // Default User-Agent Stylesheet with Mobile responsive foundations
  const std::string defaultUaCss =
      "* { box-sizing: border-box; }\n"
      "html, body { display: block; margin: 0; padding: 0; width: 100%; }\n"
      "div, section, footer, main, aside, article, p, h1, h2, h3, h4, h5, h6 { display: "
      "block; }\n"
      // P24: chỉ list-item (class cụ thể) mới flex row; <article> trần
      // là block (hero: ảnh trên, tiêu đề dưới — như mobile thật).
      // P25: thêm .art_item (vnexpress).
      ".item-news, .item_news, .news-item, .art_item { display: flex; "
      "flex-direction: row; }\n"
      "nav, .main-nav, .parent, header ul, nav ul { display: flex; "
      "flex-direction: row; flex-wrap: wrap; list-style: none; margin: 0; "
      "padding: 0; }\n"
      "nav li, .parent li, header li { display: inline-block; margin-right: "
      "14px; margin-bottom: 6px; }\n"
      "nav a, .parent a, header a { display: inline-block; }\n"
      ".thumb-art, .thumb_art { width: 24%; margin-right: 16px; flex-shrink: "
      "0; }\n"
      ".content-art, .content_art { width: 74%; flex-grow: 1; }\n"
      "span, a, strong, b, em, i, u, label { display: inline; }\n"
      "img, input, button { display: inline-block; }\n"
      "img { max-width: 100%; height: auto; }\n"
      "video { display: block; width: 100%; height: auto; background-color: #111111; }\n"
      "header, main, footer, section, article, .container, .wrapper { width: 100%; max-width: 100%; }\n"
      "table { display: table; width: 100%; }\n"
      "tr { display: table-row; }\n"
      "td, th { display: table-cell; }\n"
      "h1 { font-size: 28px; font-weight: bold; margin-top: 8px; "
      "margin-bottom: 8px; }\n"
      "h2 { font-size: 25px; font-weight: bold; margin-top: 6px; "
      "margin-bottom: 6px; }\n"
      "h3 { font-size: 23px; font-weight: bold; margin-top: 4px; "
      "margin-bottom: 4px; }\n"
      "p  { font-size: 21px; margin-top: 4px; margin-bottom: 4px; }\n"
      "a  { color: #1e40af; text-decoration: none; }\n";

  css_stylesheet_params s_params;
  std::memset(&s_params, 0, sizeof(s_params));
  s_params.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
  s_params.level = CSS_LEVEL_3;
  s_params.charset = "UTF-8";
  s_params.url = "ua.css";
  s_params.title = "UA";
  s_params.allow_quirks = true;
  s_params.resolve = resolve_url;

  css_stylesheet *uaSheet = nullptr;
  if (css_stylesheet_create(&s_params, &uaSheet) == CSS_OK) {
    css_stylesheet_append_data(uaSheet, (const uint8_t *)defaultUaCss.data(),
                               defaultUaCss.size());
    css_stylesheet_data_done(uaSheet);
    css_select_ctx_append_sheet(selectCtx, uaSheet, CSS_ORIGIN_UA, nullptr);
  }

  // Extract all <style> blocks from the HTML document.
  // P23: không giới hạn kích thước — CSS chỉ là tập con của HTML (đã nằm
  // trong RAM), cắt bớt làm mất style của trang (vnexpress có block 88KB).
  std::string authorCss = extraCss;
  size_t searchPos = 0;
  while ((searchPos = html.find("<style", searchPos)) != std::string::npos) {
    size_t closeAngle = html.find('>', searchPos);
    if (closeAngle == std::string::npos)
      break;
    size_t closeTag = html.find("</style>", closeAngle);
    if (closeTag == std::string::npos)
      break;
    std::string snippet = html.substr(closeAngle + 1, closeTag - closeAngle - 1);
    if (!authorCss.empty())
      authorCss.push_back('\n');
    authorCss.append(snippet);
    searchPos = closeTag + 8;
  }

  // Author stylesheet (page rules / extracted <style> + extraCss)
  css_stylesheet *authorSheet = nullptr;
  if (!authorCss.empty()) {
    s_params.url = "author.css";
    s_params.title = "Author";
    if (css_stylesheet_create(&s_params, &authorSheet) == CSS_OK) {
      css_stylesheet_append_data(authorSheet, (const uint8_t *)authorCss.data(),
                                 authorCss.size());
      css_stylesheet_data_done(authorSheet);
      css_select_ctx_append_sheet(selectCtx, authorSheet, CSS_ORIGIN_AUTHOR,
                                  nullptr);
    }
  }

  // Configure unit context (Mobile viewport 480x768 to trigger responsive CSS breakpoints)
  css_unit_ctx unitCtx = {
      480 * (1 << CSS_RADIX_POINT),  // viewport_width
      768 * (1 << CSS_RADIX_POINT),  // viewport_height
      22 * (1 << CSS_RADIX_POINT),   // font_size_default
      14 * (1 << CSS_RADIX_POINT),   // font_size_minimum
      96 * (1 << CSS_RADIX_POINT),   // device_dpi
      nullptr,                       // root_style
      nullptr,                       // pw
      nullptr                        // measure
  };

  css_media media;
  std::memset(&media, 0, sizeof(media));
  media.type = CSS_MEDIA_SCREEN;
  media.width = 480 * (1 << CSS_RADIX_POINT);
  media.height = 768 * (1 << CSS_RADIX_POINT);

  // Run CSS selection cascade on full DOM tree (bounded by node limit)
  int nodeCount = 0;
  computeStylesRecursive(outRoot.get(), selectCtx, &unitCtx, &media, nodeCount);

  // P26: apply style="" SAU libcss (inline override — đúng cascade).
  applyInlineStyleOverride(outRoot.get());

  // P23: libcss không parse display:grid — quét author CSS thủ công.
  // authorCss ở đây = extraCss (file ngoài, fetch từ worker) + <style> inline.
  applyGridFromAuthorCss(outRoot.get(), authorCss);

  // Clean up CSS selection resources
  css_select_ctx_destroy(selectCtx);
  if (uaSheet)
    css_stylesheet_destroy(uaSheet);
  if (authorSheet)
    css_stylesheet_destroy(authorSheet);

  resolveTreeUrls(outRoot.get(), baseUrl);
  return true;
#else
    static auto parseCssRules =
        [](const std::string &css,
           std::unordered_map<std::string,
                              std::unordered_map<std::string, std::string>>
               &outRules) {
          size_t pos = 0;
          while (pos < css.size()) {
            size_t openBrace = css.find('{', pos);
            if (openBrace == std::string::npos)
              break;
            size_t closeBrace = css.find('}', openBrace);
            if (closeBrace == std::string::npos)
              break;

            std::string sel = css.substr(pos, openBrace - pos);
            while (!sel.empty() &&
                   std::isspace(static_cast<unsigned char>(sel.front())))
              sel.erase(sel.begin());
            while (!sel.empty() &&
                   std::isspace(static_cast<unsigned char>(sel.back())))
              sel.pop_back();

            std::string body =
                css.substr(openBrace + 1, closeBrace - openBrace - 1);
            std::istringstream iss(body);
            std::string decl;
            while (std::getline(iss, decl, ';')) {
              size_t colon = decl.find(':');
              if (colon != std::string::npos) {
                std::string k = decl.substr(0, colon);
                std::string v = decl.substr(colon + 1);
                while (!k.empty() &&
                       std::isspace(static_cast<unsigned char>(k.front())))
                  k.erase(k.begin());
                while (!k.empty() &&
                       std::isspace(static_cast<unsigned char>(k.back())))
                  k.pop_back();
                while (!v.empty() &&
                       std::isspace(static_cast<unsigned char>(v.front())))
                  v.erase(v.begin());
                while (!v.empty() &&
                       std::isspace(static_cast<unsigned char>(v.back())))
                  v.pop_back();
                if (!k.empty() && !v.empty()) {
                  outRules[sel][k] = v;
                }
              }
            }
            pos = closeBrace + 1;
          }
        };

    std::unordered_map<std::string,
                       std::unordered_map<std::string, std::string>>
        rules;
    parseCssRules(extraCss, rules);

    std::function<void(NetSurfStyledNode *)> applyStylesRecursive =
        [&](NetSurfStyledNode *node) {
          if (!node)
            return;
          auto applyProps =
              [&](const std::unordered_map<std::string, std::string> &props) {
                for (const auto &kv : props) {
                  const std::string &k = kv.first;
                  const std::string &v = kv.second;
                  if (k == "display") {
                    if (v == "flex")
                      node->style.display = 0x11;
                    else if (v == "none")
                      node->style.display = 0x10;
                    else if (v == "inline")
                      node->style.display = 0x01;
                    else if (v == "block")
                      node->style.display = 0x02;
                  } else if (k == "flex-direction") {
                    if (v == "row")
                      node->style.flexDirection = 0x01;
                    else if (v == "column")
                      node->style.flexDirection = 0x03;
                  } else if (k == "width") {
                    if (v.find("px") != std::string::npos) {
                      node->style.widthPx = std::atoi(v.c_str());
                    }
                  } else if (k == "height") {
                    if (v.find("px") != std::string::npos) {
                      node->style.heightPx = std::atoi(v.c_str());
                    }
                  }
                }
              };

          if (!node->tagName.empty()) {
            auto it = rules.find(node->tagName);
            if (it != rules.end())
              applyProps(it->second);
          }
          if (!node->className.empty()) {
            auto it = rules.find("." + node->className);
            if (it != rules.end())
              applyProps(it->second);
          }
          if (!node->id.empty()) {
            auto it = rules.find("#" + node->id);
            if (it != rules.end())
              applyProps(it->second);
          }

          for (const auto &c : node->children) {
            applyStylesRecursive(c.get());
          }
        };

    auto root = std::make_unique<NetSurfStyledNode>();
    root->tagName = "html";
    root->style.display = 0x02;

    std::vector<NetSurfStyledNode *> stack;
    stack.push_back(root.get());

    size_t i = 0;
    while (i < html.size()) {
      if (html[i] == '<') {
        size_t closeTag = html.find('>', i);
        if (closeTag == std::string::npos)
          break;
        std::string tagContent = html.substr(i + 1, closeTag - i - 1);
        i = closeTag + 1;

        if (tagContent.empty())
          continue;

        if (tagContent[0] == '/') {
          if (stack.size() > 1) {
            stack.pop_back();
          }
        } else if (tagContent[0] == '!' || tagContent[0] == '?') {
          continue;
        } else {
          bool selfClosing = (tagContent.back() == '/');
          if (selfClosing)
            tagContent.pop_back();

          std::istringstream iss(tagContent);
          std::string tag;
          iss >> tag;

          auto child = std::make_unique<NetSurfStyledNode>();
          child->tagName = tag;
          child->parent = stack.back();
          child->style.display =
              (tag == "span" || tag == "a" || tag == "b" || tag == "i") ? 0x01
                                                                        : 0x02;

          std::string attrPart = tagContent.substr(tag.size());
          auto getAttr = [&](const std::string &name) -> std::string {
            size_t p = attrPart.find(name + "=\"");
            if (p != std::string::npos) {
              size_t s = p + name.size() + 2;
              size_t e = attrPart.find('"', s);
              if (e != std::string::npos)
                return attrPart.substr(s, e - s);
            }
            return "";
          };
          child->className = getAttr("class");
          child->id = getAttr("id");
          child->src = getAttr("src");
          child->href = getAttr("href");

          NetSurfStyledNode *rawChild = child.get();
          stack.back()->children.push_back(std::move(child));

          if (!selfClosing && tag != "img" && tag != "br" && tag != "hr" &&
              tag != "input" && tag != "meta") {
            stack.push_back(rawChild);
          }
        }
      } else {
        size_t nextTag = html.find('<', i);
        std::string text = (nextTag == std::string::npos)
                               ? html.substr(i)
                               : html.substr(i, nextTag - i);
        i = (nextTag == std::string::npos) ? html.size() : nextTag;

        size_t s = 0;
        while (s < text.size() &&
               std::isspace(static_cast<unsigned char>(text[s])))
          s++;
        size_t e = text.size();
        while (e > s && std::isspace(static_cast<unsigned char>(text[e - 1])))
          e--;
        if (e > s) {
          std::string trimmed = text.substr(s, e - s);
          auto textNode = std::make_unique<NetSurfStyledNode>();
          textNode->textContent = trimmed;
          textNode->parent = stack.back();
          textNode->style.display = 0x01;
          stack.back()->children.push_back(std::move(textNode));
        }
      }
    }

    applyStylesRecursive(root.get());
    resolveTreeUrls(root.get(), baseUrl);
    outRoot = std::move(root);
    return true;
#endif
}

bool NetSurfBridge::layout(const std::string &html, const std::string &extraCss,
                           int viewportWidth,
                           std::unique_ptr<RenderBox> &outRenderTree,
                           const std::string &baseUrl) {
  std::unique_ptr<NetSurfStyledNode> styledRoot;
  if (!parseAndStyle(html, extraCss, styledRoot, baseUrl) || !styledRoot) {
    return false;
  }
  // P28: Resolve all relative and protocol-relative URLs on the styled tree
  resolveTreeUrls(styledRoot.get(), baseUrl);

  outRenderTree = NetSurfLayoutEngine::instance().buildAndLayout(
      styledRoot.get(), viewportWidth, 20, 112);
  return outRenderTree != nullptr;
}

} // namespace RomCloud
