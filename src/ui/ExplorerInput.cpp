#include "UIManager.h"
#include "../input/InputManager.h"

namespace RomCloud {

bool UIManager::handleExplorerInput() {
    InputManager& input = InputManager::instance();
    auto& et = m_explorer.task();
    if (et.isRunning()) {
        if (input.isButtonJustPressed(Button::B)) {
            et.requestCancel();
            m_dialogs.progress.requestCancel();
        }
        return true;
    }
    if (m_dialogs.confirm.visible) {
        if (input.isButtonJustPressed(Button::A)) {
            m_dialogs.confirm.confirm();
            m_explorer.refresh();
        } else if (input.isButtonJustPressed(Button::B) ||
                   input.isButtonJustPressed(Button::X)) {
            m_dialogs.confirm.cancel();
            m_explorer.dialogs().confirm.cancel();
        }
        return true;
    }
    if (m_explorer.creatingFolder() || m_explorer.renaming())
        return handleExplorerKeyboard();
    return handleExplorerBrowser();
}

bool UIManager::handleExplorerKeyboard() {
    InputManager& input = InputManager::instance();
    VkState& vk = m_explorer.keyboard();
    if (input.isButtonJustPressed(Button::B)) {
        if (!vk.query.empty()) { VirtualKeyboard::backspace(vk); return true; }
        m_explorer.cancelKeyboard();
        return true;
    }
    if (input.isButtonJustPressed(Button::UP)) { VirtualKeyboard::move(vk, -1, 0); return true; }
    if (input.isButtonJustPressed(Button::DOWN)) { VirtualKeyboard::move(vk, 1, 0); return true; }
    if (input.isButtonJustPressed(Button::LEFT)) { VirtualKeyboard::move(vk, 0, -1); return true; }
    if (input.isButtonJustPressed(Button::RIGHT)) { VirtualKeyboard::move(vk, 0, 1); return true; }
    if (input.isButtonJustPressed(Button::X)) { VirtualKeyboard::backspace(vk); return true; }
    if (input.isButtonJustPressed(Button::Y)) { vk.shift = !vk.shift; return true; }
    if (input.isButtonJustPressed(Button::R1)) { vk.telexMode = !vk.telexMode; return true; }
    if (input.isButtonJustPressed(Button::START) || input.isButtonJustPressed(Button::SELECT)) {
        bool wantCommit = input.isButtonJustPressed(Button::START);
        if (wantCommit) { m_explorer.commitKeyboard(); }
        else m_explorer.cancelKeyboard();
        syncExplorerDialogs();
        return true;
    }
    if (input.isButtonJustPressed(Button::A)) {
        if (vk.row < 4) {
            char ch = VirtualKeyboard::charAt(vk);
            if (ch) VirtualKeyboard::typeChar(vk, ch);
        } else {
            VkAction act = VirtualKeyboard::pressA(vk, [](const char*) {});
            if (act == VkAction::Commit) {
                m_explorer.commitKeyboard();
                syncExplorerDialogs();
            } else if (act == VkAction::Cancel) {
                m_explorer.cancelKeyboard();
            }
        }
        return true;
    }
    return true;
}

bool UIManager::handleExplorerBrowser() {
    InputManager& input = InputManager::instance();
    if (input.isButtonJustPressed(Button::UP)) { m_explorer.moveSel(-1); return true; }
    if (input.isButtonJustPressed(Button::DOWN)) { m_explorer.moveSel(1); return true; }
    if (input.isButtonJustPressed(Button::A)) {
        const ExplorerEntry* e = m_explorer.current();
        if (e && e->isDir) m_explorer.enter();
        else if (e) showToast(std::string("File: ") + e->name, {200, 210, 225, 255}, 1200);
        return true;
    }
    if (input.isButtonJustPressed(Button::B)) {
        if (!m_explorer.goUp()) return false;
        return true;
    }
    if (input.isButtonJustPressed(Button::X)) { m_explorer.markCut(); syncExplorerDialogs(); return true; }
    if (input.isButtonJustPressed(Button::Y)) { m_explorer.markCopy(); syncExplorerDialogs(); return true; }
    if (input.isButtonJustPressed(Button::R1)) { m_explorer.paste(); syncExplorerDialogs(); return true; }
    if (input.isButtonJustPressed(Button::START)) {
        if (m_explorer.clipboard().active()) m_explorer.paste();
        else m_explorer.beginRename();
        syncExplorerDialogs();
        return true;
    }
    if (input.isButtonJustPressed(Button::L1)) { m_explorer.beginCreateFolder(); return true; }
    if (input.isButtonJustPressed(Button::LEFT)) {
        m_explorer.askDelete([this](bool) { m_explorer.refresh(); });
        syncExplorerDialogs();
        return true;
    }
    if (input.isButtonJustPressed(Button::RIGHT)) {
        const ExplorerEntry* e = m_explorer.current();
        if (e) showToast(FileExplorer::propertiesOf(*e), {200, 210, 225, 255}, 2500);
        return true;
    }
    if (input.isButtonJustPressed(Button::SELECT)) {
        if (m_explorer.clipboard().active()) m_explorer.cancelClipboard();
        else m_explorer.setHideJunk(!m_explorer.hideJunk());
        syncExplorerDialogs();
        return true;
    }
    return true;
}

} // namespace RomCloud
