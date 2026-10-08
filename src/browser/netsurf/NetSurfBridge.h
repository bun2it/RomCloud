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
    uint8_t display = 1;          // 0x01 = inline, 0x02 = block, 0x10 = none, 0x11 = flex
    uint8_t flexDirection = 1;    // 0x01 = row, 0x03 = column
    uint8_t alignItems = 0;
    uint8_t justifyContent = 0;
    float flexGrow = 0.0f;
    float flexShrink = 1.0f;

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
                       std::unique_ptr<NetSurfStyledNode>& outRoot);

    // Phase 3: Parse HTML, cascade CSS, and compute full 2D RenderBox layout tree
    bool layout(const std::string& html,
                const std::string& extraCss,
                int viewportWidth,
                std::unique_ptr<struct RenderBox>& outRenderTree);

    bool isInitialized() const { return m_initialized; }

private:
    NetSurfBridge() = default;
    ~NetSurfBridge();

    bool m_initialized = false;
};

} // namespace RomCloud
