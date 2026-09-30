#include "UIManager.h"
#include "../input/InputManager.h"
#include "../filesystem/FileSystemManager.h"
#include "../localsend/LocalSendManager.h"
#include <sys/stat.h>

namespace RomCloud {

bool UIManager::handleExplorerInput() {
    InputManager& input = InputManager::instance();
    auto& etA = expA().task();
    auto& etB = expB().task();
    if (etA.isRunning() || etB.isRunning()) {
        if (input.isButtonJustPressed(Button::B)) {
            etA.requestCancel();
            etB.requestCancel();
            m_dialogs.progress.requestCancel();
        }
        return true;
    }
    if (m_dialogs.confirm.visible) {
        if (input.isButtonJustPressed(Button::A)) {
            m_dialogs.confirm.confirm();
            expA().refresh();
            expB().refresh();
        } else if (input.isButtonJustPressed(Button::B)) {
            m_dialogs.confirm.cancel();
            expA().dialogs().confirm.cancel();
            expB().dialogs().confirm.cancel();
            m_expDeleteArmed = false;
        }
        return true;
    }
    if (expA().creatingFolder() || expA().renaming())
        return handleExplorerKeyboard();
    return handleExplorerBrowser();
}

bool UIManager::handleExplorerKeyboardFor(FileExplorer& ex) {
    InputManager& input = InputManager::instance();
    VkState& vk = ex.keyboard();
    if (input.isButtonJustPressed(Button::B)) {
        ex.cancelKeyboard();
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
        if (wantCommit) { ex.commitKeyboard(); }
        else ex.cancelKeyboard();
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
                ex.commitKeyboard();
                syncExplorerDialogs();
            } else if (act == VkAction::Cancel) {
                ex.cancelKeyboard();
            }
        }
        return true;
    }
    return true;
}

bool UIManager::handleExplorerKeyboard() {
    return handleExplorerKeyboardFor(expA());
}

bool UIManager::handleExplorerBrowser() {
    InputManager& input = InputManager::instance();
    FileExplorer& A = expA();
    FileExplorer& B = expB();
    if (input.isButtonJustPressed(Button::UP)) { A.moveSel(-1); return true; }
    if (input.isButtonJustPressed(Button::DOWN)) { A.moveSel(1); return true; }
    // A: vao thu muc / mo file (toast)
    if (input.isButtonJustPressed(Button::A)) {
        const ExplorerEntry* e = A.current();
        if (e && e->isDir) A.enter();
        else if (e) showToast(std::string("File: ") + e->name, {200, 210, 225, 255}, 1200);
        return true;
    }
    // B: len thu muc cha; o goc /mnt/SDCARD thi thoat ve MENU
    if (input.isButtonJustPressed(Button::B)) {
        const std::string cur = A.currentPath();
        if (cur == "/mnt/SDCARD" || cur == "/mnt/SDCARD/" || cur == "/" || cur.empty()) return false;
        A.goUp();
        return true;
    }
    // L1/R1: doi pane focus (Source = pane active, Target = pane kia)
    if (input.isButtonJustPressed(Button::L1) || input.isButtonJustPressed(Button::R1)) {
        m_expActive = 1 - m_expActive;
        m_expDeleteArmed = false;
        return true;
    }
    // Y: copy active -> pane kia, co Confirm dialog (A xac nhan / B huy)
    if (input.isButtonJustPressed(Button::Y)) {
        const ExplorerEntry* e = A.current();
        if (e) {
            std::string src = e->path, name = e->name;
            std::string dstDir = B.currentPath();
            if (src == dstDir + "/" + name ||
                FileSystemManager::isSubPath(src, dstDir + "/" + name)) {
                showToast("Không sao chép vào chính nó!", {239, 68, 68, 255}, 1800);
                return true;
            }
            struct stat st;
            if (stat((dstDir + "/" + name).c_str(), &st) == 0) {
                showToast("Đích đã tồn tại!", {239, 68, 68, 255}, 1800);
                return true;
            }
            ExplorerClipboard clip;
            clip.op = ExplorerClipboard::Op::COPY;
            clip.srcPath = src;
            clip.srcIsDir = e->isDir;
            m_dialogs.confirm.open("Sao chép?", {name, "-> " + dstDir},
                [this, clip, dstDir]() {
                    expB().pasteTo(dstDir, &clip);
                    syncExplorerDialogs();
                }, false);
            syncExplorerDialogs();
        }
        return true;
    }
    // X: move active -> pane kia, co Confirm dialog (A xac nhan / B huy)
    if (input.isButtonJustPressed(Button::X)) {
        const ExplorerEntry* e = A.current();
        if (e) {
            std::string src = e->path, name = e->name;
            std::string dstDir = B.currentPath();
            if (src == dstDir + "/" + name ||
                FileSystemManager::isSubPath(src, dstDir + "/" + name)) {
                showToast("Không chuyển vào chính nó!", {239, 68, 68, 255}, 1800);
                return true;
            }
            struct stat st;
            if (stat((dstDir + "/" + name).c_str(), &st) == 0) {
                showToast("Đích đã tồn tại!", {239, 68, 68, 255}, 1800);
                return true;
            }
            ExplorerClipboard clip;
            clip.op = ExplorerClipboard::Op::CUT;
            clip.srcPath = src;
            clip.srcIsDir = e->isDir;
            m_dialogs.confirm.open("Chuyển đi?", {name, "-> " + dstDir},
                [this, clip, dstDir]() {
                    expB().pasteTo(dstDir, &clip);
                    syncExplorerDialogs();
                }, false);
            syncExplorerDialogs();
        }
        return true;
    }
    // PLAY (phai) + START: tao thu muc moi o pane active, mac dinh "New Folder"
    if (input.isButtonJustPressed(Button::PLAY) ||
        input.isButtonJustPressed(Button::START)) {
        A.beginCreateFolder();
        return true;
    }
    // MENU (giua): xoa 2-step — step1 arm + mo confirm do, step2 A xac nhan / B huy
    if (input.isButtonJustPressed(Button::MENU)) {
        const ExplorerEntry* e = A.current();
        if (e && !m_dialogs.confirm.visible) {
            m_expDeleteArmed = true;
            A.askDelete([this](bool) { expA().refresh(); expB().refresh(); m_expDeleteArmed = false; });
            syncExplorerDialogs();
        }
        return true;
    }
    // SELECT (trai): thoat explorer ve MENU
    if (input.isButtonJustPressed(Button::SELECT)) return false;
    return true;
}

bool UIManager::handleFolderPickerInput() {
    InputManager& input = InputManager::instance();
    if (m_expPicker.creatingFolder() || m_expPicker.renaming()) {
        return handleExplorerKeyboardFor(m_expPicker);
    }
    if (input.isButtonJustPressed(Button::UP)) {
        m_expPicker.moveSel(-1);
        return true;
    }
    if (input.isButtonJustPressed(Button::DOWN)) {
        m_expPicker.moveSel(1);
        return true;
    }
    if (input.isButtonJustPressed(Button::LEFT) || input.isButtonJustPressed(Button::L1)) {
        m_expPicker.moveSel(-10);
        return true;
    }
    if (input.isButtonJustPressed(Button::RIGHT) || input.isButtonJustPressed(Button::R1)) {
        m_expPicker.moveSel(10);
        return true;
    }
    // A: vào thư mục con đang chọn
    if (input.isButtonJustPressed(Button::A)) {
        const ExplorerEntry* e = m_expPicker.current();
        if (e && e->isDir) {
            m_expPicker.enter();
        }
        return true;
    }
    // B: lên thư mục cha hoặc thoát
    if (input.isButtonJustPressed(Button::B)) {
        if (m_expPicker.currentPath() != "/mnt/SDCARD" && m_expPicker.currentPath() != "/" && !m_expPicker.currentPath().empty()) {
            m_expPicker.goUp();
        } else {
            if (m_currentState == UIState::LOCALSEND_INCOMING) {
                m_localSendIncomingMode = 0; // return to dialog
            } else {
                setState(UIState::LOCALSEND_HOME);
            }
        }
        return true;
    }
    // START / PLAY / X: Xác nhận thư mục hiện tại làm mục lưu
    if (input.isButtonJustPressed(Button::START) || input.isButtonJustPressed(Button::PLAY) || input.isButtonJustPressed(Button::X)) {
        std::string chosenDir = m_expPicker.currentPath();
        if (m_currentState == UIState::LOCALSEND_INCOMING) {
            m_localSendIncomingSavePath = chosenDir;
            m_localSendCurrentPrompt.savedPath = chosenDir;
            m_localSendIncomingMode = 0;
            showToast("Đã chọn mục lưu: " + chosenDir, {34, 197, 94, 255}, 1500);
        } else {
            std::string rel = chosenDir;
            const std::string root = "/mnt/SDCARD";
            if (rel.compare(0, root.size(), root) == 0) rel = rel.substr(root.size());
            if (!rel.empty() && rel.front() == '/') rel = rel.substr(1);
            if (!rel.empty() && rel.back() != '/') rel += '/';
            LocalSendManager::instance().setTargetFolder(rel);
            showToast("Đã chọn mục lưu: " + (rel.empty() ? "(Tự động)" : rel), {34, 197, 94, 255}, 2000);
            setState(UIState::LOCALSEND_HOME);
        }
        return true;
    }
    // Y: tạo thư mục mới
    if (input.isButtonJustPressed(Button::Y)) {
        m_expPicker.beginCreateFolder();
        return true;
    }
    return false;
}

} // namespace RomCloud
