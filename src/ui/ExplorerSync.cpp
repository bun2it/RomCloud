#include "UIManager.h"
#include <cstdio>

namespace RomCloud {

void UIManager::syncExplorerDialogs() {
    auto& ed = m_explorer.dialogs();
    auto& et = m_explorer.task();
    if (ed.toast.visible()) {
        m_dialogs.toast = ed.toast;
        ed.toast.clear();
    }
    if (ed.confirm.visible && !m_dialogs.confirm.visible) {
        m_dialogs.confirm.open(ed.confirm.title, ed.confirm.lines, [this]() {
            m_explorer.dialogs().confirm.confirm();
        }, ed.confirm.danger);
    }
    const std::string kTitle = "Dang xu ly file...";
    if (et.isRunning()) {
        if (!m_dialogs.progress.visible) m_dialogs.progress.open(kTitle, true);
        const auto& tp = et.progress();
        uint64_t tot = tp.total.load() == 0 ? 1 : tp.total.load();
        m_dialogs.progress.update(tp.done.load(), tot);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%llu / %llu",
                      (unsigned long long)tp.done.load(), (unsigned long long)tot);
        m_dialogs.progress.detail = buf;
        if (m_dialogs.progress.cancelRequested) et.requestCancel();
    } else if (m_dialogs.progress.visible && m_dialogs.progress.title == kTitle) {
        std::string err = et.progress().error;
        m_dialogs.progress.close();
        m_explorer.refresh();
        if (!err.empty() && err != "Cancelled")
            showToast(err, {239, 68, 68, 255}, 2000);
        else if (err == "Cancelled")
            showToast("Da huy", {245, 158, 11, 255}, 1500);
        else
            showToast("Xong", {34, 197, 94, 255}, 1500);
    }
}

} // namespace RomCloud
