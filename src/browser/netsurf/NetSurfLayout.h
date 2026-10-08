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
    int measureText(const std::string& text, int fontSizePx) const;

    // Word-wrap text into lines according to maxWidth
    std::vector<std::string> wrapText(const std::string& text, int maxWidth, int fontSizePx) const;

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
