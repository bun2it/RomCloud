#include "UIManager.h"
#include <cstdio>

namespace RomCloud {

void UIManager::syncExplorerDialogs() {
    FileExplorer* exps[2] = {&m_expL, &m_expR};
    for (int p = 0; p < 2; ++p) {
        auto& ed = exps[p]->dialogs();
        if (ed.toast.visible()) {
            m_dialogs.toast = ed.toast;
            ed.toast.clear();
        }
        if (ed.confirm.visible && !m_dialogs.confirm.visible) {
            FileExplorer* owner = exps[p];
            m_dialogs.confirm.open(ed.confirm.title, ed.confirm.lines, [owner]() {
                owner->dialogs().confirm.confirm();
            }, ed.confirm.danger);
        }
    }
    const std::string kTitle = "Đang xử lý file...";
    bool running = m_expL.task().isRunning() || m_expR.task().isRunning();
    if (running) {
        if (!m_dialogs.progress.visible) m_dialogs.progress.open(kTitle, true);
        const auto& tp = (m_expL.task().isRunning() ? m_expL.task() : m_expR.task()).progress();
        uint64_t tot = tp.total.load() == 0 ? 1 : tp.total.load();
        m_dialogs.progress.update(tp.done.load(), tot);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%llu / %llu",
                      (unsigned long long)tp.done.load(), (unsigned long long)tot);
        m_dialogs.progress.detail = buf;
        if (m_dialogs.progress.cancelRequested) {
            m_expL.task().requestCancel();
            m_expR.task().requestCancel();
        }
    } else if (m_dialogs.progress.visible && m_dialogs.progress.title == kTitle) {
        std::string err = !m_expL.task().progress().error.empty()
            ? m_expL.task().progress().error : m_expR.task().progress().error;
        m_dialogs.progress.close();
        m_expL.refresh();
        m_expR.refresh();
        m_expDeleteArmed = false;
        if (!err.empty() && err != "Cancelled")
            showToast(err, {239, 68, 68, 255}, 2000);
        else if (err == "Cancelled")
            showToast("Đã hủy", {245, 158, 11, 255}, 1500);
        else
            showToast("Xong!", {34, 197, 94, 255}, 1500);
    }
}

} // namespace RomCloud
