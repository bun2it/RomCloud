#include "UIManager.h"
#include "../archive/ArchiveEngine.h"
#include "../input/InputManager.h"
#include "../filesystem/FileSystemManager.h"
#include "../localsend/LocalSendManager.h"
#include <fstream>
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
    if (input.isButtonJustPressed(Button::X)) { VirtualKeyboard::typeSpace(vk); return true; }
    if (input.isButtonJustPressed(Button::Y)) { VirtualKeyboard::backspace(vk); return true; }
    if (input.isButtonJustPressed(Button::L1)) { vk.shift = !vk.shift; return true; }
    if (input.isButtonJustPressed(Button::R1)) {
        vk.telexMode = !vk.telexMode;
        showToast(vk.telexMode ? "Chế độ: TELEX" : "Chế độ: TIẾNG ANH (US)",
                  {0, 180, 255, 255}, 1200);
        return true;
    }
    if (input.isButtonJustPressed(Button::START)) {
        ex.commitKeyboard();
        syncExplorerDialogs();
        return true;
    }
    if (input.isButtonJustPressed(Button::A)) {
        if (vk.row < 4) {
            char ch = VirtualKeyboard::charAt(vk);
            if (ch) VirtualKeyboard::typeChar(vk, ch);
        } else {
            VkAction act = VirtualKeyboard::pressA(vk, [this, &vk](const char*) {
                showToast(vk.telexMode ? "Chế độ: TELEX" : "Chế độ: TIẾNG ANH (US)",
                          {0, 180, 255, 255}, 1200);
            }, false);
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
    // Popup menu đang mở thì route input vào menu trước
    if (m_expMenuOpen) return handleExpMenuInput();
    FileExplorer& A = expA();
    FileExplorer& B = expB();
    (void)B;
    if (input.isButtonJustPressed(Button::UP)) { A.moveSel(-1); return true; }
    if (input.isButtonJustPressed(Button::DOWN)) { A.moveSel(1); return true; }
    // A: vao thu muc / popup menu tren file
    if (input.isButtonJustPressed(Button::A)) {
        const ExplorerEntry* e = A.current();
        if (e && e->isDir) A.enter();
        else if (e) openExpMenu();
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
        expCopyCurrent();
        return true;
    }
    // X: move active -> pane kia, co Confirm dialog (A xac nhan / B huy)
    if (input.isButtonJustPressed(Button::X)) {
        expMoveCurrent();
        return true;
    }
    // START (phai): tao thu muc moi o pane active, mac dinh "New Folder"
    if (input.isButtonJustPressed(Button::PLAY) ||
        input.isButtonJustPressed(Button::START)) {
        A.beginCreateFolder();
        return true;
    }
    // MENU (trai): xoa 2-step — step1 arm + mo confirm do, step2 A xac nhan / B huy
    if (input.isButtonJustPressed(Button::MENU)) {
        expDeleteCurrent();
        return true;
    }
    // SELECT (giua) / B: thoat explorer ve MENU
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
                if (m_currentState == UIState::DEST_PICKER) {
                    m_pickerPendingOp = 0;
                    m_pickerPendingSrc.clear();
                }
                if (!goBack())
                    setState(UIState::LOCALSEND_HOME);
            }
        }
        return true;
    }
    // START / PLAY / X: Xác nhận thư mục hiện tại (picker đích explorer / mục lưu)
    if (input.isButtonJustPressed(Button::START) || input.isButtonJustPressed(Button::PLAY) || input.isButtonJustPressed(Button::X)) {
        std::string chosenDir = m_expPicker.currentPath();
        if (m_pickerPendingOp != 0 && !m_pickerPendingSrc.empty()) {
            // Mọi thao tác explorer đều cần xác nhận trước khi chạy
            std::string src = m_pickerPendingSrc;
            int op = m_pickerPendingOp;
            std::string name = src.substr(src.find_last_of('/') + 1);
            std::string title = (op == 1) ? "Sao chép?" : (op == 2) ? "Chuyển đi?" : "Bung tới đây?";
            m_dialogs.confirm.open(title, {name, "-> " + chosenDir},
                [this, op, src, chosenDir]() {
                    if (op == 3) {
                        expA().extractArchiveTo(chosenDir, src);
                    } else {
                        ExplorerClipboard clip;
                        clip.op = (op == 1) ? ExplorerClipboard::Op::COPY
                                            : ExplorerClipboard::Op::CUT;
                        clip.srcPath = src;
                        struct stat st;
                        clip.srcIsDir = (stat(src.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
                        expA().pasteTo(chosenDir, &clip);
                    }
                    syncExplorerDialogs();
                }, false);
            m_pickerPendingOp = 0;
            m_pickerPendingSrc.clear();
            setState(UIState::FILE_EXPLORER);
            syncExplorerDialogs();
            return true;
        }
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

// --- Action dùng chung Y/X/MENU và popup menu ---

bool UIManager::handleExpMenuInput() {
    InputManager& input = InputManager::instance();
    if (input.isButtonJustPressed(Button::B)) {
        m_expMenuOpen = false;
        return true;
    }
    if (input.isButtonJustPressed(Button::UP)) {
        if (m_expMenuSel > 0) m_expMenuSel--;
        else m_expMenuSel = (int)m_expMenuItems.size() - 1;
        return true;
    }
    if (input.isButtonJustPressed(Button::DOWN)) {
        m_expMenuSel = (int)m_expMenuItems.size() == 0
            ? 0 : (m_expMenuSel + 1) % (int)m_expMenuItems.size();
        return true;
    }
    if (input.isButtonJustPressed(Button::A)) {
        runExpMenuAction(m_expMenuSel);
        return true;
    }
    return true; // menu mở thì nuốt các nút còn lại
}

// Mở picker chọn thư mục đích cho Sao chép / Chuyển đi / Bung tới...
void UIManager::openDestPicker(int op, const std::string& title) {
    const ExplorerEntry* e = expA().current();
    if (!e) return;
    // Không copy/move vào chính nó: kiểm tra ở bước xác nhận (đích đã biết)
    m_pickerPendingOp = op;
    m_pickerPendingSrc = e->path;
    m_pickerPendingIsDir = e->isDir;
    m_expPickerTitle = title;
    m_expPicker.setDirOnly(true);
    std::string start = expA().currentPath();
    if (start.empty()) start = "/mnt/SDCARD";
    m_expPicker.open(start);
    m_expPickerScroll = 0;
    m_expMenuOpen = false;
    setState(UIState::DEST_PICKER);
}

void UIManager::expCopyCurrent() {
    if (!expA().current()) return;
    openDestPicker(1, "CHỌN THƯ MỤC SAO CHÉP TỚI");
}

void UIManager::expMoveCurrent() {
    if (!expA().current()) return;
    openDestPicker(2, "CHỌN THƯ MỤC CHUYỂN TỚI");
}

void UIManager::expDeleteCurrent() {
    FileExplorer& A = expA();
    const ExplorerEntry* e = A.current();
    if (e && !m_dialogs.confirm.visible) {
        m_expDeleteArmed = true;
        A.askDelete([this](bool) { expA().refresh(); expB().refresh(); m_expDeleteArmed = false; });
        syncExplorerDialogs();
    }
}

void UIManager::openExpMenu() {
    const ExplorerEntry* e = expA().current();
    if (!e || e->isDir) return;
    m_expMenuFile = e->path;
    m_expMenuItems = {"Xem", "Sao chép", "Chuyển đi", "Xóa"};
    if (!isTextFile(e->path))
        m_expMenuItems.erase(m_expMenuItems.begin()); // file nhị phân: bỏ Xem
    if (ArchiveEngine::isArchive(e->path)) {
        m_expMenuItems.push_back("Bung tại đây");
        m_expMenuItems.push_back("Bung tới thư mục...");
    }
    m_expMenuSel = 0;
    m_expMenuOpen = true;
}

void UIManager::runExpMenuAction(int idx) {
    if (idx < 0 || idx >= (int)m_expMenuItems.size()) return;
    const std::string& act = m_expMenuItems[idx];
    m_expMenuOpen = false;
    if (act == "Xem") openTextViewer(m_expMenuFile);
    else if (act == "Sao chép") expCopyCurrent();
    else if (act == "Chuyển đi") expMoveCurrent();
    else if (act == "Xóa") expDeleteCurrent();
    else if (act == "Bung tại đây") {
        // Mọi thao tác đều cần xác nhận
        std::string archive = m_expMenuFile;
        std::string name = archive.substr(archive.find_last_of('/') + 1);
        m_dialogs.confirm.open("Bung tại đây?", {name, "-> thư mục hiện tại"},
            [this, archive]() {
                expA().extractArchiveTo(expA().currentPath(), archive);
                syncExplorerDialogs();
            }, false);
        syncExplorerDialogs();
    } else if (act == "Bung tới thư mục...") {
        if (!expA().current()) return;
        m_pickerPendingOp = 3;
        m_pickerPendingSrc = m_expMenuFile;
        m_pickerPendingIsDir = false;
        m_expPickerTitle = "CHỌN THƯ MỤC BUNG FILE";
        m_expPicker.setDirOnly(true);
        std::string start = expA().currentPath();
        if (start.empty()) start = "/mnt/SDCARD";
        m_expPicker.open(start);
        m_expPickerScroll = 0;
        setState(UIState::DEST_PICKER);
    }
}

bool UIManager::isTextFile(const std::string& path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    static const char* kExts[] = {".txt", ".log", ".md",  ".json", ".conf",
                                  ".cfg", ".ini", ".sh",  ".py",   ".m3u",
                                  ".m3u8", ".xml", ".lst", ".nfo", nullptr};
    for (int i = 0; kExts[i]; ++i)
        if (ext == kExts[i]) return true;
    return false;
}

void UIManager::openTextViewer(const std::string& path) {
    m_textViewPath = path;
    m_textViewLines.clear();
    m_textViewScroll = 0;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        showToast("Không mở được file!", {239, 68, 68, 255}, 2000);
        return;
    }
    // Giới hạn 256KB đầu (đủ log/config/playlist, không kẹt RAM)
    constexpr size_t kMaxBytes = 256 * 1024;
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    if (content.size() > kMaxBytes)
        content.resize(kMaxBytes);
    // Bỏ ký tự NUL (file lẫn nhị phân) để TTF không cắt chuỗi
    for (char& c : content)
        if (c == '\0') c = ' ';
    // Tách đoạn theo \n, wrap từng đoạn (UTF-8 safe), tối đa 5000 dòng
    constexpr size_t kMaxLines = 5000;
    std::string cur;
    auto flushPara = [&]() {
        if (m_textViewLines.size() >= kMaxLines) return;
        if (cur.empty()) {
            if (m_textViewLines.size() + 1 < kMaxLines)
                m_textViewLines.push_back("");
            return;
        }
        auto w = wrapAboutText(cur, m_fontSmall, 1024 - 48 - 56);
        for (const auto& l : w) {
            if (m_textViewLines.size() >= kMaxLines) break;
            m_textViewLines.push_back(l);
        }
    };
    for (char c : content) {
        if (c == '\r') continue;
        if (c == '\n') { flushPara(); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) flushPara();
    if (m_textViewLines.empty()) m_textViewLines.push_back("(File trống)");
    setState(UIState::TEXT_VIEWER);
}

bool UIManager::handleTextViewerInput() {
    InputManager& input = InputManager::instance();
    if (input.isButtonJustPressed(Button::B)) {
        setState(UIState::FILE_EXPLORER);
        return true;
    }
    if (input.isButtonJustPressed(Button::UP)) {
        if (m_textViewScroll > 0) m_textViewScroll--;
        return true;
    }
    if (input.isButtonJustPressed(Button::DOWN)) {
        m_textViewScroll++;
        return true;
    }
    if (input.isButtonJustPressed(Button::L1)) {
        m_textViewScroll = std::max(0, m_textViewScroll - 10);
        return true;
    }
    if (input.isButtonJustPressed(Button::R1)) {
        m_textViewScroll += 10;
        return true;
    }
    return true;
}

} // namespace RomCloud
