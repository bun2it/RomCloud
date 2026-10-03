// ============================================================================
// SearchInputModal — implementation (P0-6)
// ============================================================================
#include "SearchInputModal.h"
#include "UiRenderer.h"
#include "UiTheme.h"
#include <algorithm>

namespace RomCloud {

static void drawText(UiRenderer& r, const std::string& s, int x, int y,
                     SDL_Color c, TTF_Font* f, bool centered = false) {
    r.drawText(s, x, y, c, f, centered);
}

static std::string truncateFor(UiRenderer& r, const std::string& s,
                                TTF_Font* f, int maxPx) {
    return r.truncateToWidth(s, f, maxPx);
}

// ─── Title strip ────────────────────────────────────────────────────────
void SearchInputModal::renderTitle(const Config& cfg) {
    drawText(*cfg.ui, cfg.title, cfg.modalX, cfg.historyY - 4,
             UiTheme::ACCENT_CYAN, cfg.fLarge);
    if (cfg.vk && !cfg.vk->query.empty()) {
        std::string q = "\"" + cfg.vk->query + "\"";
        std::string trunc =
            truncateFor(*cfg.ui, q, cfg.fMedium, cfg.modalW - 200);
        cfg.ui->drawTextRight(trunc, cfg.modalX + cfg.modalW,
                              cfg.historyY + 4, UiTheme::TEXT_MAIN,
                              cfg.fMedium);
    }
}

// ─── History list (vertical → PILL NGANG) ──────────────────────────────
void SearchInputModal::renderHistoryList(const Config& cfg) {
    const int pillY = cfg.dividerY;
    const int pillH = 40;
    const int pillGap = 10;
    if (!cfg.history || cfg.history->empty()) {
        drawText(*cfg.ui, "Chưa có lịch sử — gõ từ khóa mới bên dưới.",
                 cfg.modalX + cfg.modalW / 2, pillY + 10,
                 UiTheme::TEXT_DIM, cfg.fSmall, true);
        return;
    }
    int n = static_cast<int>(cfg.history->size());
    // Đo width mỗi pill (truncate text quá dài về 260px)
    std::vector<int> widths;
    widths.reserve(n);
    for (int i = 0; i < n; i++) {
        std::string t =
            truncateFor(*cfg.ui, (*cfg.history)[i], cfg.fSmall, 260);
        int w = cfg.ui->textWidth(t, cfg.fSmall) + 32;
        if (w < 64) w = 64;
        widths.push_back(w);
    }
    Config& mcfg = const_cast<Config&>(cfg);
    if (mcfg.historySelected < 0) mcfg.historySelected = 0;
    if (mcfg.historySelected >= n) mcfg.historySelected = n - 1;
    if (mcfg.historyScrollOffset < 0) mcfg.historyScrollOffset = 0;
    if (mcfg.historyScrollOffset >= n) mcfg.historyScrollOffset = n - 1;
    if (mcfg.historySelected < mcfg.historyScrollOffset)
        mcfg.historyScrollOffset = mcfg.historySelected;
    // Cuộn sao cho pill chọn luôn nằm trong khung
    while (true) {
        int x = cfg.modalX;
        for (int i = mcfg.historyScrollOffset; i <= mcfg.historySelected; i++) {
            if (i > mcfg.historyScrollOffset) x += pillGap;
            x += widths[i];
        }
        if (x <= cfg.modalX + cfg.modalW ||
            mcfg.historyScrollOffset >= mcfg.historySelected)
            break;
        mcfg.historyScrollOffset++;
    }
    int cx = cfg.modalX;
    for (int i = mcfg.historyScrollOffset; i < n; i++) {
        int w = widths[i];
        if (cx + w > cfg.modalX + cfg.modalW) break;
        bool isSel = (cfg.focusMode == 0 && i == mcfg.historySelected);
        SDL_Color bg = isSel ? SDL_Color{255, 255, 255, 255}
                             : SDL_Color{28, 38, 50, 230};
        cfg.ui->drawRoundedRect(cx, pillY, w, pillH, pillH / 2, bg, true);
        if (isSel) {
            cfg.ui->drawRoundedBorder(cx, pillY, w, pillH, pillH / 2,
                                      UiTheme::FOCUS_GLOW, 2);
        }
        std::string t =
            truncateFor(*cfg.ui, (*cfg.history)[i], cfg.fSmall, 260);
        drawText(*cfg.ui, t, cx + w / 2, pillY + (pillH - 16) / 2,
                 isSel ? SDL_Color{20, 20, 20, 255} : UiTheme::TEXT_SUB,
                 cfg.fSmall, true);
        cx += w + pillGap;
    }
}

// ─── Keyboard ──────────────────────────────────────────────────────────
void SearchInputModal::renderKeyboard(const Config& cfg) {
    if (!cfg.ui || !cfg.vk) return;
    cfg.ui->drawVirtualKeyboard(*cfg.vk, cfg.modalX, cfg.keypadY,
                                86, 44, 8, 6, cfg.accent, cfg.accentEdge,
                                const_cast<const char**>(cfg.actionLabels),
                                true, true, cfg.actionStride);
}

// ─── render (top-level) ────────────────────────────────────────────────
void SearchInputModal::render(const Config& cfg) {
    if (!cfg.ui) return;
    cfg.ui->beginModalDim();
    renderTitle(cfg);
    renderHistoryList(cfg);
    cfg.ui->drawRect(cfg.modalX, cfg.dividerY, cfg.modalW, 1,
                     {20, 48, 64, 180}, true);
    renderKeyboard(cfg);
}

// ─── handleInput ───────────────────────────────────────────────────────
SearchInputModal::Result SearchInputModal::handleInput(
    const Config& cfg, bool aPressed, bool bPressed, bool startPressed,
    bool upPressed, bool downPressed, bool leftPressed, bool rightPressed,
    bool xPressed, bool yPressed, bool l1Pressed, bool r1Pressed) {
    if (!cfg.vk) return Result::Cancel;
    if (bPressed) return Result::Cancel;

    if (cfg.focusMode == 0) {
        // Focus trên hàng pill ngang → L/R chuyển pill, DOWN xuống keyboard.
        Config& mcfg = const_cast<Config&>(cfg);
        int n = cfg.history ? static_cast<int>(cfg.history->size()) : 0;
        if (n == 0) {
            mcfg.focusMode = 1;
            return Result::None;
        }
        if (leftPressed) {
            if (mcfg.historySelected > 0) mcfg.historySelected--;
            return Result::None;
        }
        if (rightPressed) {
            if (mcfg.historySelected + 1 < n) mcfg.historySelected++;
            return Result::None;
        }
        if (downPressed) {
            mcfg.focusMode = 1;
            return Result::None;
        }
        if (aPressed && cfg.allowPickHistory) {
            pickHistoryItem(mcfg, mcfg.historySelected);
            return Result::PickHistory;
        }
        if (yPressed && cfg.allowDeleteHistoryItem) {
            mcfg.history->erase(mcfg.history->begin() + mcfg.historySelected);
            if (mcfg.historySelected >= n - 1)
                mcfg.historySelected = static_cast<int>(mcfg.history->size()) - 1;
            if (mcfg.historySelected < 0) mcfg.historySelected = 0;
            return Result::None;
        }
        return Result::None;
    }

    // focusMode == 1: keyboard
    if (upPressed && cfg.history && !cfg.history->empty()) {
        // UP từ keyboard → về hàng pill (chỉ khi có lịch sử)
        Config& mcfg = const_cast<Config&>(cfg);
        mcfg.focusMode = 0;
        return Result::None;
    }
    if (aPressed) {
        VkAction va = VirtualKeyboard::pressA(
            *cfg.vk, [](const char*) {}, cfg.actionStride == 2);
        if (va == VkAction::Commit) return Result::Commit;
        return Result::None;
    }
    if (leftPressed) {
        VirtualKeyboard::move(*cfg.vk, 0, -1, cfg.actionStride == 2);
        return Result::None;
    }
    if (rightPressed) {
        VirtualKeyboard::move(*cfg.vk, 0, 1, cfg.actionStride == 2);
        return Result::None;
    }
    if (upPressed) {
        VirtualKeyboard::move(*cfg.vk, -1, 0, cfg.actionStride == 2);
        return Result::None;
    }
    if (downPressed) {
        VirtualKeyboard::move(*cfg.vk, 1, 0, cfg.actionStride == 2);
        return Result::None;
    }
    if (startPressed) {
        return cfg.vk->query.empty() ? Result::Cancel : Result::Commit;
    }
    if (xPressed) {
        VirtualKeyboard::typeSpace(*cfg.vk);
        return Result::None;
    }
    if (yPressed) {
        VirtualKeyboard::backspace(*cfg.vk);
        return Result::None;
    }
    if (l1Pressed) {
        cfg.vk->shift = !cfg.vk->shift;
        return Result::None;
    }
    if (r1Pressed) {
        cfg.vk->telexMode = !cfg.vk->telexMode;
        return Result::None;
    }
    return Result::None;
}

void SearchInputModal::pickHistoryItem(Config& cfg, int historyIdx) {
    if (!cfg.history || !cfg.vk) return;
    int n = static_cast<int>(cfg.history->size());
    if (historyIdx < 0 || historyIdx >= n) return;
    cfg.vk->query = (*cfg.history)[historyIdx];
    std::string picked = std::move((*cfg.history)[historyIdx]);
    cfg.history->erase(cfg.history->begin() + historyIdx);
    cfg.history->insert(cfg.history->begin(), std::move(picked));
    cfg.historySelected = 0;
    cfg.historyScrollOffset = 0;
    cfg.vk->row = 4;
    cfg.vk->col = cfg.actionStride == 2 ? 8 : 4;
    cfg.focusMode = 1;
}

int SearchInputModal::maxHistoryOffset(int historyCount) {
    const int rowH = 56;
    const int rowGap = 8;
    int vis = (440 - 16) / (rowH + rowGap);
    return std::max(0, historyCount - vis);
}

} // namespace RomCloud