#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>
#include "NetSurfBridge.h"

namespace RomCloud {

enum class RenderBoxType {
    BLOCK,
    INLINE,
    FLEX_ROW,
    FLEX_COLUMN,
    GRID,  // P23
    TEXT,
    IMAGE,
    LINK,
    BUTTON,
    INPUT,
    CONTAINER
};

struct RenderBox {
    RenderBoxType type = RenderBoxType::BLOCK;
    std::string tagName;
    std::string textContent;
    std::vector<std::string> lines; // Bẻ dòng theo width
    std::string className;
    std::string id;
    std::string href;
    std::string src;

    // Tọa độ và kích thước trong layout space (pixel)
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    // Box model
    int marginTop = 0;
    int marginRight = 0;
    int marginBottom = 0;
    int marginLeft = 0;
    int paddingTop = 0;
    int paddingRight = 0;
    int paddingBottom = 0;
    int paddingLeft = 0;

    // Styling
    uint32_t color = 0x222222FF;
    uint32_t backgroundColor = 0x00000000;
    uint32_t borderColor = 0x00000000;
    int borderWidth = 0;
    int fontSize = 24;
    int fontWeight = 400;
    int lineHeight = 32;
    uint8_t textAlign = 0;

    // P21: extended CSS properties (mirrored from NetSurfComputedStyle).
    int minHeight = 0;
    int textIndent = 0;
    int letterSpacing = 0;
    int wordSpacing = 0;
    int borderTopWidth = 0;
    int borderRightWidth = 0;
    int borderBottomWidth = 0;
    int borderLeftWidth = 0;
    uint8_t borderTopStyle = 0;
    uint8_t borderRightStyle = 0;
    uint8_t borderBottomStyle = 0;
    uint8_t borderLeftStyle = 0;
    uint32_t borderTopColor = 0x00000000;
    uint32_t borderRightColor = 0x00000000;
    uint32_t borderBottomColor = 0x00000000;
    uint32_t borderLeftColor = 0x00000000;
    int outlineWidth = 0;
    uint32_t outlineColor = 0x00000000;
    uint8_t outlineStyle = 0;
    uint8_t listStylePosition = 0;

    // Interactive
    bool isFocusable = false;
    bool isFocused = false;
    // Inline formatting: text runs, spans, links, etc. flow horizontally
    // with wrapping inside a block (not stacked vertically).
    bool isInline = false;

    // P17: form controls (copied from NetSurfStyledNode in createBoxTree).
    std::string inputType;   // "text","password","checkbox","submit","button","select","textarea"...
    std::string value;       // current value (editable for text inputs)
    std::string inputName;
    std::string placeholder;
    bool checked = false;
    bool editing = false;    // VK edit session active on this box
    int maxLen = 0;
    std::vector<std::string> options;  // <select> option labels
    int selected = 0;
    std::string formAction;  // enclosing <form> action (on the form's own box)
    std::string formMethod;  // enclosing <form> method (on the form's own box)
    bool skipChildren = false;  // P17: input/textarea/select fold children in
    bool phantom = false;  // P17: hidden input — value submits, no layout/render

    // P23: flex/grid (copied from NetSurfComputedStyle in createBoxTree).
    int gapPx = 0;  // gap (flex) / grid-gap (grid)
    std::vector<NetSurfComputedStyle::GridTrack> gridCols;  // grid-template-columns
    // justify-content / align-items cho flex (lưu raw enum libcss để layout dùng)
    uint8_t justifyContent = 0;
    uint8_t alignItems = 0;
    float flexGrow = 0.0f;
    uint8_t floatSide = 0;  // P25: 0 = none, 1 = left, 2 = right

    // P22: per-box JavaScript event handler storage. The bridge stores Duktape
    // function refs (as void* opaque handles) keyed by event name. A click
    // via JS or the renderer drains click handlers. setText / setClassList /
    // setStyle also bump jsEpoch so the layout re-derives from the latest
    // content without stale caches.
    std::vector<void*> jsClickHandlers;  // duk_context_function refs (click only)
    uint64_t jsEpoch = 0;                // bumped on any JS mutation to this box
    bool jsHidden = false;               // element.style.display = 'none' from JS
    bool jsChecked = false;              // checkbox/radio state from JS (overrides attr)

    // Hierarchy
    RenderBox* parent = nullptr;
    std::vector<std::unique_ptr<RenderBox>> children;
};

class NetSurfLayoutEngine {
public:
    static NetSurfLayoutEngine& instance();

    // Callback to measure exact text width if available (e.g. via TTF_SizeUTF8)
    using TextMeasureCallback = std::function<int(const std::string&, int)>;
    void setTextMeasurer(TextMeasureCallback measurer) { m_textMeasurer = measurer; }

    // Estimate text width in pixels (UTF-8 safe)
    int measureText(const std::string& text, int fontSizePx,
                    int letterSpacing = 0, int wordSpacing = 0) const;

    // Word-wrap text into lines according to maxWidth
    std::vector<std::string> wrapText(const std::string& text, int maxWidth, int fontSizePx,
                                      int letterSpacing = 0, int wordSpacing = 0) const;

    // Construct RenderBox tree from NetSurfStyledNode and compute 2D layout geometry
    std::unique_ptr<RenderBox> buildAndLayout(const NetSurfStyledNode* styledRoot,
                                              int viewportWidth = 984,
                                              int startX = 20,
                                              int startY = 112);

private:
    NetSurfLayoutEngine() = default;

    // Build un-positioned RenderBox tree from Styled DOM
    std::unique_ptr<RenderBox> createBoxTree(const NetSurfStyledNode* node, RenderBox* parentBox);

    // Layout computation passes
    void layoutBox(RenderBox* box, int availableWidth, int& currentY, int currentX);
    void layoutBlockBox(RenderBox* box, int availableWidth, int& currentY, int currentX);
    void layoutFlexRowBox(RenderBox* box, int availableWidth, int& currentY, int currentX);
    void layoutFlexColumnBox(RenderBox* box, int availableWidth, int& currentY, int currentX);
    void layoutGridBox(RenderBox* box, int availableWidth, int& currentY, int currentX);  // P23
    void layoutTextBox(RenderBox* box, int availableWidth, int& currentY, int currentX);
    void layoutImageBox(RenderBox* box, int availableWidth, int& currentY, int currentX);
    void layoutInlineWrapBox(RenderBox* box, int availableWidth, int& currentY, int currentX);
    // Inline formatting: lay out children[begin..end) (all isInline) in a
    // horizontal flow with word wrapping. flowY advances past the run.
    void layoutInlineRun(RenderBox* parent, size_t begin, size_t end,
                         int innerW, int childX, int& flowY);

    TextMeasureCallback m_textMeasurer = nullptr;
};

} // namespace RomCloud
