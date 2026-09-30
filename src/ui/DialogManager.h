#pragma once
// RomCloud DialogManager — state logic cho toast/confirm/progress, khong SDL.
// UIManager giu render hien tai; dan chuyen render ve day sau khi UiRenderer xong.
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace RomCloud {

struct ToastState {
    std::string message;
    uint32_t colorPacked = 0x00C853FF; // rgba
    uint32_t durationMs = 1200;
    int64_t expiryMs = 0;
    static int64_t nowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }
    void show(const std::string& msg, uint32_t color = 0x00C853FF, uint32_t ms = 1200) {
        message = msg; colorPacked = color; durationMs = ms;
        expiryMs = nowMs() + ms;
    }
    bool visible() const { return !message.empty() && nowMs() < expiryMs; }
    void clear() { message.clear(); expiryMs = 0; }
};

struct ConfirmDialog {
    bool visible = false;
    std::string title;
    std::vector<std::string> lines;
    std::string okLabel = "[A] Xoa";
    std::string cancelLabel = "[B] Huy";
    bool danger = true;
    std::function<void()> onOk;
    void open(const std::string& t, std::vector<std::string> b, std::function<void()> ok, bool d = true) {
        title = t; lines = std::move(b); onOk = std::move(ok); danger = d; visible = true;
    }
    bool confirm() {
        if (!visible) return false;
        visible = false;
        if (onOk) onOk();
        return true;
    }
    void cancel() { visible = false; onOk = nullptr; }
};

struct ProgressDialog {
    bool visible = false;
    std::string title;
    std::string detail;
    uint64_t done = 0, total = 1;
    bool cancellable = true;
    bool cancelRequested = false;
    void open(const std::string& t, bool c = true) {
        title = t; detail.clear(); done = 0; total = 1; cancellable = c;
        cancelRequested = false; visible = true;
    }
    void update(uint64_t d, uint64_t t) { done = d; total = t > 0 ? t : 1; }
    double fraction() const { return total == 0 ? 0.0 : (double)done / (double)total; }
    void requestCancel() { if (cancellable) cancelRequested = true; }
    void close() { visible = false; detail.clear(); }
};

class DialogManager {
public:
    ToastState toast;
    ConfirmDialog confirm;
    ProgressDialog progress;
    void toastMsg(const std::string& m, uint32_t c = 0x00C853FF, uint32_t ms = 1200) {
        toast.show(m, c, ms);
    }
};

} // namespace RomCloud
