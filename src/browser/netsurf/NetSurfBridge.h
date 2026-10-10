#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>

namespace RomCloud {

// Simplified node info representation for inspecting/testing W3C DOM tree
struct NetSurfDomNode {
    std::string tagName;
    std::string textContent;
    std::string id;
    std::string className;
    std::string href;
    std::string src;
    std::vector<NetSurfDomNode> children;
};

// Computed style properties resolved by libcss Selection Cascade
struct NetSurfComputedStyle {
    uint8_t display = 1;          // 0x01 = inline, 0x02 = block, 0x10 = none,
                                  // 0x11 = flex, 0x12 = grid
    uint8_t flexDirection = 1;    // 0x01 = row, 0x03 = column
    uint8_t floatSide = 0;        // P25: 0 = none, 1 = left, 2 = right
    uint8_t alignItems = 0;
    uint8_t justifyContent = 0;
    float flexGrow = 0.0f;
    float flexShrink = 1.0f;
    int gapPx = 0;                // gap (flex) / grid-gap (grid), px

    // Grid template columns — libcss này không có grid nên parse thủ công.
    struct GridTrack {
        uint8_t kind = 2;         // 0 = px, 1 = percent, 2 = fr
        float value = 1.0f;
    };
    std::vector<GridTrack> gridCols;  // rỗng = tự chia đều theo số item

    int widthPx = -1;             // -1 = auto
    int heightPx = -1;            // -1 = auto
    int marginTopPx = 0;
    int marginRightPx = 0;
    int marginBottomPx = 0;
    int marginLeftPx = 0;
    int paddingTopPx = 0;
    int paddingRightPx = 0;
    int paddingBottomPx = 0;
    int paddingLeftPx = 0;

    uint32_t color = 0x222222FF;          // RGBA (default dark text)
    uint32_t backgroundColor = 0x00000000;// RGBA (transparent)
    uint32_t borderColor = 0x00000000;    // RGBA
    int borderWidthPx = 0;
    int fontSizePx = 24;
    int fontWeight = 400;
    uint8_t textAlign = 0;        // 0 = left, 1 = right, 2 = center

    // P21: extended CSS properties (libcss cascade + inline-style override).
    int minHeightPx = -1;          // -1 = auto
    int textIndentPx = 0;          // px
    int letterSpacingPx = 0;       // px
    int wordSpacingPx = 0;         // px
    int borderTopWidthPx = 0;
    int borderRightWidthPx = 0;
    int borderBottomWidthPx = 0;
    int borderLeftWidthPx = 0;
    uint8_t borderTopStyle = 0;
    uint8_t borderRightStyle = 0;
    uint8_t borderBottomStyle = 0;
    uint8_t borderLeftStyle = 0;
    uint32_t borderTopColor = 0x00000000;
    uint32_t borderRightColor = 0x00000000;
    uint32_t borderBottomColor = 0x00000000;
    uint32_t borderLeftColor = 0x00000000;
    int outlineWidthPx = 0;
    uint32_t outlineColor = 0x00000000;
    uint8_t outlineStyle = 0;      // 0=none, 1=solid, 2=dashed, 3=dotted, 4=double
    uint8_t listStylePosition = 0; // 0=outside, 1=inside
};

struct NetSurfStyledNode {
    std::string tagName;
    std::string textContent;
    std::string id;
    std::string className;
    std::string href;
    std::string src;

    // P17: form control attributes (captured in buildStyledTree).
    std::string inputType;   // <input>/<button> type, lowercased ("text" default)
    std::string inputValue;  // value attr (input/button)
    std::string inputName;   // name attr (input/textarea/select/button)
    std::string inputPlaceholder;
    bool inputChecked = false;  // bare `checked` / `selected`
    int inputMaxLen = 0;        // maxlength (0 = none)
    std::string formAction;  // <form> action
    std::string formMethod;  // <form> method, uppercased ("GET" default)

    NetSurfComputedStyle style;

    // P26: raw style="" attr (libcss parse ở computeStylesRecursive, thay parser tay).
    std::string styleAttr;

    NetSurfStyledNode* parent = nullptr;
    NetSurfStyledNode* prevSibling = nullptr;
    std::vector<std::unique_ptr<NetSurfStyledNode>> children;
};

class NetSurfBridge {
public:
    static NetSurfBridge& instance();

    // Initialize core subsystems (wapcaplet, libcss, etc.)
    bool init();
    void shutdown();

    // Parse HTML string using libhubbub + libdom into W3C DOM
    // Returns true on success, and outputs a statistical summary of the parsed DOM
    bool parseHtml(const std::string& html, std::string& outSummary);

    // Parse HTML and dump simplified node tree (useful for verification and inspection)
    bool parseToTree(const std::string& html, NetSurfDomNode& outRootNode, int& outTotalNodes);

    // Phase 2: Parse HTML and compute W3C CSS cascaded styles for all elements
    // extraCss: Additional stylesheet text (e.g. from <style> tags or external css)
    bool parseAndStyle(const std::string& html,
                       const std::string& extraCss,
                       std::unique_ptr<NetSurfStyledNode>& outRoot,
                       const std::string& baseUrl = "");

    // Phase 3: Parse HTML, cascade CSS, and compute full 2D RenderBox layout tree
    bool layout(const std::string& html,
                const std::string& extraCss,
                int viewportWidth,
                std::unique_ptr<struct RenderBox>& outRenderTree,
                const std::string& baseUrl = "");

    bool isInitialized() const { return m_initialized; }

private:
    NetSurfBridge() = default;
    ~NetSurfBridge();

    bool m_initialized = false;
};

} // namespace RomCloud
