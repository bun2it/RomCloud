#include "NetSurfLayout.h"
#include "../../logging/Logger.h"
#include <algorithm>
#include <cctype>

namespace RomCloud {

NetSurfLayoutEngine& NetSurfLayoutEngine::instance() {
    static NetSurfLayoutEngine inst;
    return inst;
}

int NetSurfLayoutEngine::measureText(const std::string& text, int fontSizePx) const {
    if (text.empty() || fontSizePx <= 0) return 0;
    if (m_textMeasurer) {
        return m_textMeasurer(text, fontSizePx);
    }

    // High-accuracy UTF-8 character length estimator
    int totalPx = 0;
    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == ' ') {
            totalPx += fontSizePx * 28 / 100; // Space width ~0.28em
            i++;
        } else if (c < 0x80) {
            // ASCII character
            if (c >= 'A' && c <= 'Z') {
                totalPx += fontSizePx * 65 / 100; // Capital letters ~0.65em
            } else if (c == 'i' || c == 'l' || c == 'j' || c == '!' || c == '.' || c == ':') {
                totalPx += fontSizePx * 30 / 100; // Narrow letters ~0.30em
            } else if (c == 'w' || c == 'm') {
                totalPx += fontSizePx * 78 / 100; // Wide letters ~0.78em
            } else {
                totalPx += fontSizePx * 52 / 100; // Standard lowercase ~0.52em
            }
            i++;
        } else {
            // Multi-byte UTF-8 character (e.g. Vietnamese diacritics)
            size_t n = 1;
            if ((c & 0xE0) == 0xC0) n = 2;
            else if ((c & 0xF0) == 0xE0) n = 3;
            else if ((c & 0xF8) == 0xF0) n = 4;
            totalPx += fontSizePx * 58 / 100; // Diacritic character ~0.58em
            i += n;
        }
    }
    return totalPx;
}

std::vector<std::string> NetSurfLayoutEngine::wrapText(const std::string& text, int maxWidth, int fontSizePx) const {
    std::vector<std::string> lines;
    if (text.empty() || maxWidth <= 0) return lines;

    // Tokenize into words
    std::vector<std::string> words;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) i++;
        if (i >= text.size()) break;
        size_t j = i;
        while (j < text.size() && !std::isspace(static_cast<unsigned char>(text[j]))) j++;
        words.push_back(text.substr(i, j - i));
        i = j;
    }

    auto hardBreak = [&](const std::string& w) {
        size_t pos = 0;
        std::string cur;
        while (pos < w.size()) {
            unsigned char c = static_cast<unsigned char>(w[pos]);
            size_t n = 1;
            if ((c & 0xE0) == 0xC0) n = 2;
            else if ((c & 0xF0) == 0xE0) n = 3;
            else if ((c & 0xF8) == 0xF0) n = 4;
            if (pos + n > w.size()) n = w.size() - pos;

            std::string trial = cur + w.substr(pos, n);
            if (measureText(trial, fontSizePx) <= maxWidth || cur.empty()) {
                cur = trial;
                pos += n;
            } else {
                lines.push_back(cur);
                cur.clear();
            }
        }
        if (!cur.empty()) lines.push_back(cur);
    };

    std::string currentLine;
    for (const auto& w : words) {
        if (measureText(w, fontSizePx) > maxWidth) {
            if (!currentLine.empty()) {
                lines.push_back(currentLine);
                currentLine.clear();
            }
            hardBreak(w);
            continue;
        }

        std::string trial = currentLine.empty() ? w : currentLine + " " + w;
        if (measureText(trial, fontSizePx) <= maxWidth) {
            currentLine = trial;
        } else {
            if (!currentLine.empty()) lines.push_back(currentLine);
            currentLine = w;
        }
    }
    if (!currentLine.empty()) lines.push_back(currentLine);

    return lines;
}

std::unique_ptr<RenderBox> NetSurfLayoutEngine::createBoxTree(const NetSurfStyledNode* node, RenderBox* parentBox) {
    if (!node) return nullptr;

    // Filter out display:none (0x10)
    if (node->style.display == 0x10) return nullptr;

    // Filter out non-rendered structural / meta tags, or hidden elements
    const std::string& tag = node->tagName;
    if (tag == "head" || tag == "script" || tag == "style" ||
        tag == "meta" || tag == "link" || tag == "title" || tag == "noscript") {
        return nullptr;
    }
    if (node->style.display == 0x10) { // CSS_DISPLAY_NONE: do NOT render hidden modals/popups
        return nullptr;
    }

    auto box = std::make_unique<RenderBox>();
    box->tagName = node->tagName;
    box->textContent = node->textContent;
    box->className = node->className;
    box->id = node->id;
    box->href = node->href;
    box->src = node->src;
    box->parent = parentBox;
    // P17: form control state.
    box->inputType = node->inputType;
    box->value = node->inputValue;
    box->inputName = node->inputName;
    box->placeholder = node->inputPlaceholder;
    box->checked = node->inputChecked;
    box->maxLen = node->inputMaxLen;
    box->formAction = node->formAction;
    box->formMethod = node->formMethod;

    // Box model styling
    box->marginTop = node->style.marginTopPx;
    box->marginRight = node->style.marginRightPx;
    box->marginBottom = node->style.marginBottomPx;
    box->marginLeft = node->style.marginLeftPx;
    box->paddingTop = node->style.paddingTopPx;
    box->paddingRight = node->style.paddingRightPx;
    box->paddingBottom = node->style.paddingBottomPx;
    box->paddingLeft = node->style.paddingLeftPx;

    // Font size determination & uniform typography
    int fs = node->style.fontSizePx;
    if (fs <= 0) {
        fs = 22; // Standard readable body font
    }

    if (tag == "h1") {
        fs = 30;
    } else if (tag == "h2") {
        fs = 26;
    } else if (tag == "h3" || node->className.find("title") != std::string::npos) {
        fs = 24;
    } else if (tag == "h4") {
        fs = 22;
    }

    // Children inside headings or titles MUST inherit parent's font size
    if (parentBox) {
        bool parentIsHeading = (parentBox->tagName == "h1" || parentBox->tagName == "h2" ||
                                parentBox->tagName == "h3" || parentBox->tagName == "h4" ||
                                parentBox->className.find("title") != std::string::npos);
        if (parentIsHeading) {
            fs = parentBox->fontSize;
        } else if (node->style.fontSizePx <= 0) {
            fs = parentBox->fontSize;
        }

        if (box->color == 0xFFFFFFFF || (box->color & 0xFF) == 0) {
            box->color = parentBox->color;
        }
    }

    // Default text color: dark charcoal (#222222)
    if (box->color == 0xFFFFFFFF || (box->color & 0xFF) == 0) {
        box->color = 0x222222FF;
    }

    // Clamp font size strictly between 18px (metadata) and 32px (h1)
    box->fontSize = std::max(18, std::min(32, fs));
    box->fontWeight = node->style.fontWeight;
    box->lineHeight = (int)(box->fontSize * 1.35f);
    box->backgroundColor = node->style.backgroundColor;
    box->borderColor = node->style.borderColor;
    box->borderWidth = node->style.borderWidthPx;
    box->width = node->style.widthPx;
    box->height = node->style.heightPx;
    box->textAlign = node->style.textAlign;

    // P17: hidden inputs are phantoms — their value still submits, but they
    // take no layout space, never render, and are never focusable.
    if (tag == "input" && node->inputType == "hidden") {
        box->type = RenderBoxType::INPUT;
        box->phantom = true;
        box->skipChildren = true;
        return box;
    }

    // Check if element has a valid hyperlink (href not empty)
    bool hasHref = !node->href.empty();
    if (!hasHref && parentBox && !parentBox->href.empty()) {
        box->href = parentBox->href;
        hasHref = true;
    }

    // Determine RenderBox type and focusability (Requirement 3:
    // Only hyperlinks with href and input fields are focusable;
    // Plain text and non-hyperlink images are NEVER focusable)
    bool isFlex = (node->style.display == 0x11 || node->style.display == 0x12);
    bool isArticle = (tag == "article" || node->className.find("item-news") != std::string::npos ||
                      node->className.find("item_news") != std::string::npos ||
                      node->className.find("news-item") != std::string::npos ||
                      node->className.find("box-category") != std::string::npos);

    bool hasChildImage = false;
    bool hasChildText = false;
    for (const auto& ch : node->children) {
        if (ch->tagName == "img" || ch->tagName == "picture" ||
            ch->className.find("thumb") != std::string::npos) {
            hasChildImage = true;
        }
        if (ch->tagName == "h1" || ch->tagName == "h2" || ch->tagName == "h3" || ch->tagName == "h4" ||
            ch->tagName == "p" || ch->className.find("title") != std::string::npos ||
            ch->className.find("content") != std::string::npos) {
            hasChildText = true;
        }
    }
    bool isNewsCard = (hasChildImage && hasChildText);

    if (isFlex) {
        if (node->style.flexDirection == 0x02) {
            box->type = RenderBoxType::FLEX_COLUMN;
        } else {
            box->type = RenderBoxType::FLEX_ROW;
        }
    } else if (isArticle || isNewsCard) {
        box->type = RenderBoxType::FLEX_ROW;
    } else if (tag == "nav" || tag == "ul" || tag == "ol" ||
               node->className.find("nav") != std::string::npos ||
               node->className.find("menu") != std::string::npos ||
               node->className.find("parent") != std::string::npos) {
        box->type = RenderBoxType::CONTAINER;
    } else if (tag == "img" || tag == "video") {
        box->type = RenderBoxType::IMAGE;
        box->isFocusable = hasHref || (tag == "video");
        if (tag == "video") {
            box->skipChildren = true;
            if (box->width <= 0) box->width = 720;
        }
    } else if (tag == "a" || hasHref) {
        box->type = RenderBoxType::LINK;
        box->isFocusable = hasHref;
        if (hasHref) {
            box->color = 0x1E40AFFF; // Dark blue for links
        }
    } else if (tag == "button") {
        box->type = RenderBoxType::BUTTON;
        box->isFocusable = true;
    } else if (tag == "input" || tag == "textarea" || tag == "select") {
        box->type = RenderBoxType::INPUT;
        box->isFocusable = true;
        if (tag == "textarea") {
            // Fold child text into value; don't recurse (would duplicate).
            std::string t;
            std::function<void(const NetSurfStyledNode*)> gather =
                [&](const NetSurfStyledNode* n) {
                    if (!n) return;
                    if (n->tagName == "#text") t += n->textContent;
                    for (const auto& c : n->children) gather(c.get());
                };
            for (const auto& ch : node->children) gather(ch.get());
            box->value = t;
        } else if (tag == "select") {
            // Gather <option> labels; value attr wins, text is fallback.
            for (const auto& ch : node->children) {
                if (ch->tagName != "option") continue;
                std::string label;
                std::function<void(const NetSurfStyledNode*)> gather =
                    [&](const NetSurfStyledNode* n) {
                        if (!n) return;
                        if (n->tagName == "#text") label += n->textContent;
                        for (const auto& c : n->children) gather(c.get());
                    };
                gather(ch.get());
                // trim
                size_t a = label.find_first_not_of(" \t\r\n");
                size_t b = label.find_last_not_of(" \t\r\n");
                label = (a == std::string::npos) ? "" : label.substr(a, b - a + 1);
                box->options.push_back(label);
                if (ch->inputChecked) box->selected = (int)box->options.size() - 1;
            }
            if (!box->options.empty())
                box->value = box->options[(size_t)std::max(0, box->selected)];
        }
        if (node->inputType == "checkbox" || node->inputType == "radio") {
            box->width = 40;   // explicit: don't stretch full width
            box->height = 40;
        } else if (box->height <= 0) {
            box->height = 44;  // tappable field height (was 0 -> invisible)
        }
        box->skipChildren = true;
    } else if (tag == "#text" || (tag.empty() && !node->textContent.empty())) {
        box->type = RenderBoxType::TEXT;
        box->isFocusable = false;
        box->isInline = true;  // text runs always flow inline
    } else {
        box->type = RenderBoxType::BLOCK;
        box->isFocusable = false;
    }

    // Inline formatting flag: spans, links, text-level tags flow horizontally
    // inside their block (fixes "W/ikipedi/a" vertical stacking). Only trust
    // known inline tags (the style.display default is 0x01=inline, so an
    // uncomputed <div> must NOT become inline); explicitly block/flex/none
    // always wins.
    if (!box->isInline) {
        uint8_t disp = node->style.display;
        bool isBlockish = (disp == 0x02 || disp == 0x10 || disp == 0x11 || disp == 0x12);
        if (!isBlockish) {
            if (tag == "span" || tag == "a" || tag == "b" ||
                tag == "i" || tag == "em" || tag == "strong" ||
                tag == "label" || tag == "small" || tag == "code" ||
                tag == "u" || tag == "sub" || tag == "sup" ||
                tag == "abbr" || tag == "cite" || tag == "q") {
                box->isInline = true;
            }
        }
    }

    // Recursively process children (P17: inputs fold theirs into value)
    if (!box->skipChildren) {
        for (const auto& childNode : node->children) {
            auto childBox = createBoxTree(childNode.get(), box.get());
            if (childBox) {
                box->children.push_back(std::move(childBox));
            }
        }
    }

    return box;
}

std::unique_ptr<RenderBox> NetSurfLayoutEngine::buildAndLayout(const NetSurfStyledNode* styledRoot,
                                                              int viewportWidth,
                                                              int startX,
                                                              int startY) {
    if (!styledRoot) return nullptr;

    auto rootBox = createBoxTree(styledRoot, nullptr);
    if (!rootBox) return nullptr;

    int currentY = startY >= 112 ? startY : 112;
    layoutBox(rootBox.get(), viewportWidth, currentY, startX);

    return rootBox;
}

void NetSurfLayoutEngine::layoutBox(RenderBox* box, int availableWidth, int& currentY, int currentX) {
    if (!box || box->phantom) return;

    switch (box->type) {
        case RenderBoxType::FLEX_ROW:
            layoutFlexRowBox(box, availableWidth, currentY, currentX);
            break;
        case RenderBoxType::FLEX_COLUMN:
            layoutFlexColumnBox(box, availableWidth, currentY, currentX);
            break;
        case RenderBoxType::CONTAINER:
            layoutInlineWrapBox(box, availableWidth, currentY, currentX);
            break;
        case RenderBoxType::TEXT:
            layoutTextBox(box, availableWidth, currentY, currentX);
            break;
        case RenderBoxType::IMAGE:
            layoutImageBox(box, availableWidth, currentY, currentX);
            break;
        case RenderBoxType::BLOCK:
        case RenderBoxType::INLINE:
        case RenderBoxType::LINK:
        case RenderBoxType::BUTTON:
        case RenderBoxType::INPUT:
        default:
            layoutBlockBox(box, availableWidth, currentY, currentX);
            break;
    }
}

void NetSurfLayoutEngine::layoutFlexRowBox(RenderBox* box, int availableWidth, int& currentY, int currentX) {
    box->x = currentX + box->marginLeft;
    box->y = currentY + box->marginTop;

    int availW = availableWidth - box->marginLeft - box->marginRight;
    box->width = availW;

    int innerW = availW - box->paddingLeft - box->paddingRight;
    const int gap = 16; // 16px standard flex gap

    // Separate direct children into fixed-width and flexible items
    std::vector<RenderBox*> rowItems;
    for (const auto& ch : box->children) {
        rowItems.push_back(ch.get());
    }

    if (rowItems.empty()) {
        box->height = box->paddingTop + box->paddingBottom;
        currentY = box->y + box->height + box->marginBottom;
        return;
    }

    int totalGaps = (int)(rowItems.size() - 1) * gap;
    int remainingW = innerW - totalGaps;
    int flexGrowCount = 0;

    auto hasImage = [](const RenderBox* b) {
        if (!b) return false;
        if (b->type == RenderBoxType::IMAGE) return true;
        if (b->className.find("thumb") != std::string::npos) return true;
        for (const auto& ch : b->children) {
            if (ch->type == RenderBoxType::IMAGE) return true;
            if (ch->className.find("thumb") != std::string::npos) return true;
        }
        return false;
    };

    // First pass: Allocate explicit or estimated widths
    for (auto* item : rowItems) {
        if (hasImage(item)) {
            // News thumbnail column width: ~24-25% (~226px on 944px inner width)
            item->width = (int)(innerW * 0.24f);
            remainingW -= item->width;
        } else if (item->width > 0) {
            // Constrain fixed width to max 50% of row if multiple items exist
            if (rowItems.size() > 1 && item->width > (int)(innerW * 0.50f)) {
                item->width = (int)(innerW * 0.40f);
            }
            remainingW -= item->width;
        } else {
            flexGrowCount++;
        }
    }

    // Allocate remaining width to flex-grow items (~74-75% for text column)
    if (flexGrowCount > 0) {
        int growW = std::max(remainingW / flexGrowCount, 120);
        for (auto* item : rowItems) {
            if (item->width <= 0 && !hasImage(item)) {
                item->width = growW;
            }
        }
    }

    // Second pass: Position flex row items side by side
    int itemX = box->x + box->paddingLeft;
    int itemY = box->y + box->paddingTop;
    int maxHeight = 0;

    for (auto* item : rowItems) {
        int colY = itemY;
        layoutBox(item, item->width, colY, itemX);

        int colH = (colY - itemY);
        maxHeight = std::max(maxHeight, colH);

        itemX += item->width + gap;
    }

    box->height = maxHeight + box->paddingTop + box->paddingBottom;
    currentY = box->y + box->height + box->marginBottom;
}

void NetSurfLayoutEngine::layoutFlexColumnBox(RenderBox* box, int availableWidth, int& currentY, int currentX) {
    layoutBlockBox(box, availableWidth, currentY, currentX);
}

void NetSurfLayoutEngine::layoutBlockBox(RenderBox* box, int availableWidth, int& currentY, int currentX) {
    box->x = currentX + box->marginLeft;
    box->y = currentY + box->marginTop;

    int availW = availableWidth - box->marginLeft - box->marginRight;
    box->width = (box->width > 0 && box->width < availW) ? box->width : availW;

    int innerW = box->width - box->paddingLeft - box->paddingRight;
    int childX = box->x + box->paddingLeft;
    int flowY = box->y + box->paddingTop;

    // Legacy: a block carrying its own text (e.g. #text before the TEXT
    // split) draws it as wrapped lines at the top.
    if (!box->textContent.empty() && box->children.empty()) {
        box->lines = wrapText(box->textContent, innerW, box->fontSize);
        int textH = (int)box->lines.size() * box->lineHeight;
        flowY += textH;
    }

    // Children: consecutive inline runs flow horizontally (layoutInlineRun);
    // block children break the flow and stack vertically.
    size_t i = 0;
    while (i < box->children.size()) {
        if (box->children[i]->isInline) {
            size_t j = i;
            while (j < box->children.size() && box->children[j]->isInline) j++;
            layoutInlineRun(box, i, j, innerW, childX, flowY);
            i = j;
        } else {
            int cy = flowY;
            layoutBox(box->children[i].get(), innerW, cy, childX);
            flowY = cy;
            i++;
        }
    }

    int measuredH = flowY - (box->y + box->paddingTop);
    box->height = std::max(measuredH + box->paddingTop + box->paddingBottom,
                           box->height > 0 ? box->height : 0);

    currentY = box->y + box->height + box->marginBottom;
}

// First text descendant (for inline elements wrapping styled children,
// e.g. <a><b>label</b></a>). Returns empty when there is no text.
static std::string firstInlineText(const RenderBox* b) {
    if (!b) return "";
    if (!b->textContent.empty()) return b->textContent;
    for (const auto& ch : b->children) {
        std::string t = firstInlineText(ch.get());
        if (!t.empty()) return t;
    }
    return "";
}

void NetSurfLayoutEngine::layoutInlineRun(RenderBox* parent, size_t begin, size_t end,
                                          int innerW, int childX, int& flowY) {
    // No extra hGap: textContent already carries its spaces (measureText
    // counts them); adding more would double the word spacing.
    const int vGap = 8;
    int lineX = childX;   // horizontal cursor on the current line
    int lineY = flowY;    // top of the current line
    int lineH = 0;        // tallest item on the current line

    auto newLine = [&]() {
        if (lineH > 0) {
            lineY += lineH + vGap;
            lineX = childX;
            lineH = 0;
        }
    };

    for (size_t k = begin; k < end; k++) {
        RenderBox* ib = parent->children[k].get();
        std::string text = firstInlineText(ib);
        int fs = ib->fontSize > 0 ? ib->fontSize : 22;
        int lh = ib->lineHeight > 0 ? ib->lineHeight : 32;

        if (text.empty()) {
            // Inline with no text (e.g. <a><img></a>): treat as a block so
            // the image still lays out instead of vanishing.
            newLine();
            int cy = lineY;
            layoutBox(ib, innerW, cy, childX);
            lineY = cy;
            lineX = childX;
            lineH = 0;
            continue;
        }

        // Does the whole fragment fit on the current line?
        int fragW = measureText(text, fs);
        if (lineX + fragW > childX + innerW && lineX > childX) {
            newLine();
        }
        // Re-measure against the (possibly new) line start.
        int availFirst = childX + innerW - lineX;
        std::vector<std::string> lines;
        if (measureText(text, fs) <= availFirst) {
            lines = { text };
        } else {
            // Fragment needs wrapping: start it at a fresh line for
            // predictable line breaks.
            newLine();
            lines = wrapText(text, innerW, fs);
        }

        if (lines.size() == 1) {
            int w = measureText(lines[0], fs);
            ib->x = lineX;
            ib->y = lineY;
            ib->width = w;
            ib->height = lh;
            ib->lines = lines;
            lineX += w;
            lineH = std::max(lineH, lh);
        } else {
            // Multi-line fragment: full-width block, flow continues below.
            ib->x = childX;
            ib->y = lineY;
            ib->width = innerW;
            ib->height = (int)lines.size() * lh;
            ib->lines = lines;
            lineY += ib->height + vGap;
            lineX = childX;
            lineH = 0;
        }
    }

    flowY = (lineH > 0) ? (lineY + lineH) : lineY;
}

void NetSurfLayoutEngine::layoutTextBox(RenderBox* box, int availableWidth, int& currentY, int currentX) {
    box->x = currentX + box->marginLeft;
    box->y = currentY + box->marginTop;
    box->width = availableWidth - box->marginLeft - box->marginRight;

    box->lines = wrapText(box->textContent, box->width, box->fontSize);
    box->height = (int)box->lines.size() * box->lineHeight;

    currentY = box->y + box->height + box->marginBottom;
}

void NetSurfLayoutEngine::layoutImageBox(RenderBox* box, int availableWidth, int& currentY, int currentX) {
    box->x = currentX + box->marginLeft;
    box->y = currentY + box->marginTop;

    int availW = availableWidth - box->marginLeft - box->marginRight;
    if (box->width <= 0) {
        box->width = std::min(availW, 720);
    } else if (box->width > availW) {
        box->width = availW;
    }

    if (box->height <= 0) {
        // Standard 16:9 news thumbnail aspect ratio
        box->height = box->width * 9 / 16;
    }

    currentY = box->y + box->height + box->marginBottom;
}

void NetSurfLayoutEngine::layoutInlineWrapBox(RenderBox* box, int availableWidth, int& currentY, int currentX) {
    box->x = currentX + box->marginLeft;
    box->y = currentY + box->marginTop;

    int availW = availableWidth - box->marginLeft - box->marginRight;
    box->width = availW;

    int innerW = availW - box->paddingLeft - box->paddingRight;
    int cursorX = box->x + box->paddingLeft;
    int cursorY = box->y + box->paddingTop;
    int lineHeight = 34;
    int rowMaxH = lineHeight;

    const int hGap = 14;
    const int vGap = 8;

    for (const auto& child : box->children) {
        // Measure child width
        int childW = 0;
        if (!child->textContent.empty()) {
            childW = measureText(child->textContent, child->fontSize > 0 ? child->fontSize : 22) + 12;
        } else if (!child->children.empty()) {
            std::string subTxt;
            for (const auto& sub : child->children) {
                if (!sub->textContent.empty()) subTxt = sub->textContent;
                else for (const auto& sub2 : sub->children) {
                    if (!sub2->textContent.empty()) subTxt = sub2->textContent;
                }
            }
            if (!subTxt.empty()) {
                childW = measureText(subTxt, child->fontSize > 0 ? child->fontSize : 22) + 16;
            } else {
                childW = 80;
            }
        } else {
            childW = 60;
        }

        int childH = child->lineHeight > 0 ? child->lineHeight : 32;

        // Wrap to next line if overflowing
        if (cursorX + childW > box->x + box->paddingLeft + innerW && cursorX > box->x + box->paddingLeft) {
            cursorX = box->x + box->paddingLeft;
            cursorY += rowMaxH + vGap;
            rowMaxH = childH;
        }

        child->x = cursorX;
        child->y = cursorY;
        child->width = childW;
        child->height = childH;
        // Direct text child: give it a single line so it actually draws
        // (previously only sub-children got lines -> invisible text).
        if (child->lines.empty() && !child->textContent.empty() &&
            child->children.empty()) {
            child->lines = { child->textContent };
        }

        // Propagate position to sub-children
        int innerCursorX = child->x;
        for (const auto& sub : child->children) {
            sub->x = innerCursorX;
            sub->y = cursorY;
            sub->width = childW;
            sub->height = childH;
            if (sub->lines.empty() && !sub->textContent.empty()) {
                sub->lines = { sub->textContent };
            }
            for (const auto& sub2 : sub->children) {
                sub2->x = innerCursorX;
                sub2->y = cursorY;
                sub2->width = childW;
                sub2->height = childH;
                if (sub2->lines.empty() && !sub2->textContent.empty()) {
                    sub2->lines = { sub2->textContent };
                }
            }
        }

        rowMaxH = std::max(rowMaxH, childH);
        cursorX += childW + hGap;
    }

    box->height = (cursorY + rowMaxH) - box->y + box->paddingBottom;
    currentY = box->y + box->height + box->marginBottom;
}

} // namespace RomCloud
