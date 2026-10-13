#include "UIManager.h"
#include "../archive/ArchiveEngine.h"
#include "../config/AppConfig.h"
#include "../input/InputManager.h"
#include "../filesystem/FileSystemManager.h"
#include "../localsend/LocalSendManager.h"
#include "../rom/EmuLauncher.h"
#include "../rom/RomOrganizer.h"
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
    if (input.isButtonRepeat(Button::X)) { VirtualKeyboard::typeSpace(vk); return true; }
    if (input.isButtonRepeat(Button::Y)) { VirtualKeyboard::backspace(vk); return true; }
    if (input.isButtonJustPressed(Button::L1)) { vk.shift = !vk.shift; return true; }
    if (input.isButtonJustPressed(Button::R1)) {
        VirtualKeyboard::toggleSymbol(vk, [this](const char* msg) {
            showToast(msg, {0, 180, 255, 255}, 1200);
        });
        return true;
    }
    if (input.isButtonJustPressed(Button::SELECT)) {
        VirtualKeyboard::toggleTelex(vk, [this](const char* msg) {
            showToast(msg, {0, 180, 255, 255}, 1200);
        });
        return true;
    }
    if (input.isButtonJustPressed(Button::START)) {
        ex.commitKeyboard();
        syncExplorerDialogs();
        return true;
    }
    if (input.isButtonRepeat(Button::A)) {
        if (vk.row < 4) {
            char ch = VirtualKeyboard::charAt(vk);
            if (ch) VirtualKeyboard::typeChar(vk, ch);
        } else {
            VkAction act = VirtualKeyboard::pressA(vk, [this](const char* msg) {
                showToast(msg, {0, 180, 255, 255}, 1200);
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
    // B: lên thư mục cha; ở gốc /mnt/SDCARD thì thoát về MENU
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
    // Y: copy active -> pane kia, có Confirm dialog (A xác nhận / B hủy)
    if (input.isButtonJustPressed(Button::Y)) {
        expCopyCurrent();
        return true;
    }
    // X: move active -> pane kia, có Confirm dialog (A xác nhận / B hủy)
    if (input.isButtonJustPressed(Button::X)) {
        expMoveCurrent();
        return true;
    }
    // START (phải): tạo thư mục mới ở pane active, mặc định "New Folder"
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
    // SELECT (giữa) / B: thoát explorer về MENU
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

// Action dùng chung Y/X/MENU và popup menu

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

// Tab Thẻ nhớ (menu A -> Chuyển hệ giả lập): mở picker danh sách console
// (tên + icon), path đích preset Roms/<romDir>. A chọn, B đóng.
void UIManager::openSysPicker() {
    if (m_libraryTab != LibraryTab::SDCARD || m_multiSelectMode) return;
    if (m_selectedGameIndex < 0 ||
        m_selectedGameIndex >= static_cast<int>(m_cachedGames.size()))
        return;
    const auto& g = m_cachedGames[m_selectedGameIndex];
    if (g.localState != GameState::LOCAL ||
        !FileSystemManager::instance().fileExists(g.localPath)) {
        showToast("Chỉ dời được ROM đã có trên thẻ.", {245, 158, 11, 255}, 2000);
        return;
    }
    if (RomOrganizer::instance().isBusy()) {
        showToast("Đang sắp xếp, vui lòng chờ...", {245, 158, 11, 255}, 2000);
        return;
    }
    m_sysPickerSystems.clear();
    for (const auto& s : DatabaseManager::instance().getSystems(false)) {
        if (s.id == m_activeSystem.id) continue; // bỏ hệ hiện tại
        m_sysPickerSystems.push_back(s);
    }
    if (m_sysPickerSystems.empty()) {
        showToast("Không còn hệ máy nào để chuyển.", {245, 158, 11, 255}, 2000);
        return;
    }
    m_movePendingGameId = g.id;
    m_sysPickerSel = 0;
    m_sysPickerScroll = 0;
    m_sysPickerOpen = true;
}

bool UIManager::handleSysPickerInput() {
    InputManager& input = InputManager::instance();
    int n = (int)m_sysPickerSystems.size();
    if (input.isButtonJustPressed(Button::B)) {
        m_sysPickerOpen = false;
        return true;
    }
    if (input.isButtonJustPressed(Button::UP)) {
        if (m_sysPickerSel > 0) m_sysPickerSel--;
        else m_sysPickerSel = std::max(0, n - 1);
        return true;
    }
    if (input.isButtonJustPressed(Button::DOWN)) {
        m_sysPickerSel = (n == 0) ? 0 : (m_sysPickerSel + 1) % n;
        return true;
    }
    if (input.isButtonJustPressed(Button::A)) {
        if (m_sysPickerSel < 0 || m_sysPickerSel >= n) return true;
        const SystemRecord& target = m_sysPickerSystems[m_sysPickerSel];
        m_moveTargetSystem = target;
        m_moveTargetDir =
            AppConfig::instance().getRomsDir() + "/" + target.romDir;
        m_sysPickerOpen = false;
        setState(UIState::CONFIRM_MOVE_SYSTEM);
        return true;
    }
    return true; // picker mở thì nuốt các nút còn lại
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

// Popup menu game tab Thẻ nhớ (A trên ROM): Chơi / Chuyển hệ giả lập /
// Kiểm tra ROM / Xóa ROM. A thực thi, B đóng.
void UIManager::openGameMenu() {
    if (m_selectedGameIndex < 0 ||
        m_selectedGameIndex >= static_cast<int>(m_cachedGames.size()))
        return;
    const auto& g = m_cachedGames[m_selectedGameIndex];
    if (g.localState != GameState::LOCAL ||
        !FileSystemManager::instance().fileExists(g.localPath))
        return;
    m_gameMenuGameId = g.id;
    m_gameMenuItems = {"Chơi", "Chuyển hệ giả lập", "Kiểm tra ROM",
                       "Chi tiết file", "Xóa ROM"};
    m_gameMenuSel = 0;
    m_gameMenuOpen = true;
}

bool UIManager::handleGameMenuInput() {
    InputManager& input = InputManager::instance();
    if (input.isButtonJustPressed(Button::B)) {
        m_gameMenuOpen = false;
        return true;
    }
    if (input.isButtonJustPressed(Button::UP)) {
        if (m_gameMenuSel > 0) m_gameMenuSel--;
        else m_gameMenuSel = (int)m_gameMenuItems.size() - 1;
        return true;
    }
    if (input.isButtonJustPressed(Button::DOWN)) {
        m_gameMenuSel = m_gameMenuItems.empty()
            ? 0 : (m_gameMenuSel + 1) % (int)m_gameMenuItems.size();
        return true;
    }
    if (input.isButtonJustPressed(Button::A)) {
        runGameMenuAction(m_gameMenuSel);
        return true;
    }
    return true; // menu mở thì nuốt các nút còn lại
}

static int menuGameIndex(const std::vector<GameRecord>& games, int64_t id) {
    for (int i = 0; i < (int)games.size(); ++i)
        if (games[i].id == id) return i;
    return -1;
}

void UIManager::runGameMenuAction(int idx) {
    if (idx < 0 || idx >= (int)m_gameMenuItems.size()) return;
    const std::string& act = m_gameMenuItems[idx];
    m_gameMenuOpen = false;
    int gi = menuGameIndex(m_cachedGames, m_gameMenuGameId);
    if (gi < 0) return;
    m_selectedGameIndex = gi;
    if (act == "Chơi") playMenuGameSelection();
    else if (act == "Chuyển hệ giả lập") openSysPicker();
    else if (act == "Kiểm tra ROM") checkSelectedGameRom();
    else if (act == "Chi tiết file") auditSelectedGameRom();
    else if (act == "Xóa ROM") {
        const auto& g = m_cachedGames[m_selectedGameIndex];
        if (g.localState == GameState::LOCAL) setState(UIState::CONFIRM_DELETE);
    }
}

void UIManager::playMenuGameSelection() {
    if (m_selectedGameIndex < 0 ||
        m_selectedGameIndex >= static_cast<int>(m_cachedGames.size()))
        return;
    const auto& g = m_cachedGames[m_selectedGameIndex];
    if (g.localState == GameState::LOCAL &&
        FileSystemManager::instance().fileExists(g.localPath)) {
        if (EmuLauncher::instance().play(m_activeSystem.code, g.localPath,
                                         g.title)) {
            setState(UIState::GAME_PLAYING);
        } else {
            showToast("Không có emulator cho hệ " + m_activeSystem.code,
                      {239, 68, 68, 255}, 2500);
        }
    } else {
        showToast("Game chưa có trên thẻ nhớ.", {245, 158, 11, 255}, 2500);
    }
}

// Kiểm tra 1 ROM có đúng hệ hiện tại không (extension/header/MD5), chạy nền.
// Xong thì poll ở GAME_LIST: đúng -> toast, lạc -> đổ vào dialog Chuyển hệ.
void UIManager::checkSelectedGameRom() {
    if (m_selectedGameIndex < 0 ||
        m_selectedGameIndex >= static_cast<int>(m_cachedGames.size()))
        return;
    GameRecord g = m_cachedGames[m_selectedGameIndex];
    if (g.localState != GameState::LOCAL ||
        !FileSystemManager::instance().fileExists(g.localPath)) {
        showToast("Chỉ kiểm tra được ROM đã có trên thẻ.",
                  {245, 158, 11, 255}, 2000);
        return;
    }
    std::string curCode = m_activeSystem.code;
    showToast("Đang kiểm tra " + g.title + "...", {0, 180, 216, 255}, 60000);
    std::thread([this, g, curCode]() {
        RomDetectionResult res;
        bool ok = RomDetector::instance().isFileInCorrectSystem(
            g.localPath, curCode, res);
        std::lock_guard<std::mutex> lock(m_gameCheckMutex);
        m_gameCheck.ready = true;
        m_gameCheck.misplaced = !ok && res.detected;
        m_gameCheck.res = res;
        m_gameCheck.gameId = g.id;
        m_gameCheck.srcPath = g.localPath;
        m_gameCheck.title = g.title;
        m_gameCheck.curCode = curCode;
    }).detach();
}

void UIManager::consumeGameCheckResult() {
    GameCheckResult r;
    {
        std::lock_guard<std::mutex> lock(m_gameCheckMutex);
        if (!m_gameCheck.ready) return;
        r = m_gameCheck;
        m_gameCheck.ready = false;
    }
    if (!r.misplaced) {
        if (r.res.detected) {
            showToast("ROM đúng hệ " + m_activeSystem.name +
                          " (" + r.res.confidence + ").",
                      {34, 197, 94, 255}, 4000);
        } else {
            showToast("Không nhận diện được hệ máy, giữ nguyên.",
                      {245, 158, 11, 255}, 3000);
        }
        return;
    }
    // ROM lạc: tìm hệ đích theo mã detect rồi đổ vào dialog Chuyển hệ.
    auto systems = DatabaseManager::instance().getSystems(false);
    const SystemRecord* tgt = nullptr;
    for (const auto& s : systems) {
        if (s.code == r.res.systemCode) {
            tgt = &s;
            break;
        }
    }
    if (!tgt) {
        showToast("ROM này thuộc hệ " + r.res.systemName +
                      " (chưa có trên máy).",
                  {245, 158, 11, 255}, 3500);
        return;
    }
    if (tgt->id == m_activeSystem.id) {
        showToast("ROM đúng hệ " + tgt->name + ".", {34, 197, 94, 255}, 2500);
        return;
    }
    m_movePendingGameId = r.gameId;
    m_moveTargetSystem = *tgt;
    m_moveTargetDir =
        AppConfig::instance().getRomsDir() + "/" + tgt->romDir;
    setState(UIState::CONFIRM_MOVE_SYSTEM);
}

// Đọc ruột set arcade (thiếu/sai/thừa file từng mục), hiện báo cáo ở
// Text Viewer (đọc central directory, không giải nén nên chạy ngay).
void UIManager::auditSelectedGameRom() {
    if (m_selectedGameIndex < 0 ||
        m_selectedGameIndex >= static_cast<int>(m_cachedGames.size()))
        return;
    GameRecord g = m_cachedGames[m_selectedGameIndex];
    if (g.localState != GameState::LOCAL ||
        !FileSystemManager::instance().fileExists(g.localPath)) {
        showToast("Chỉ xem được ROM đã có trên thẻ.",
                  {245, 158, 11, 255}, 2000);
        return;
    }
    RomDetector::RomAuditResult ar =
        RomDetector::instance().auditArchive(g.localPath);
    m_textViewLines.clear();
    auto pushLine = [this](const std::string& ln) {
        auto w = wrapAboutText(ln, m_fontSmall, 1024 - 48 - 56);
        for (const auto& l : w) {
            if (m_textViewLines.size() >= 5000) break;
            m_textViewLines.push_back(l);
        }
    };
    pushLine("ROM: " + g.filename);
    if (!ar.knownSet) {
        pushLine("Không phải set arcade đã biết (không có trong DAT).");
        pushLine("Dung lượng: " +
                 FileSystemManager::instance().formatBytes(g.sizeBytes));
    } else {
        pushLine("Set: " + ar.setName + " (" +
                 std::to_string(ar.entries.size()) + " mục)");
        pushLine("");
        int nOk = 0, nMiss = 0, nBad = 0, nExtra = 0;
        for (const auto& en : ar.entries) {
            if (en.status == 0) {
                ++nOk;
                pushLine("[OK] " + en.name);
            } else if (en.status == 1) {
                ++nMiss;
                pushLine("[THIẾU] " + en.name + " (muốn " + en.expectedCrc +
                         ")");
            } else if (en.status == 2) {
                ++nBad;
                pushLine("[SAI CRC] " + en.name + " (có " + en.actualCrc +
                         ", muốn " + en.expectedCrc + ")");
            } else {
                ++nExtra;
                pushLine("[THỪA] " + en.name);
            }
        }
        pushLine("");
        pushLine("Tổng: " + std::to_string(nOk) + " đúng, " +
                 std::to_string(nMiss) + " thiếu, " + std::to_string(nBad) +
                 " sai, " + std::to_string(nExtra) + " thừa.");
        if (ar.verdict == RomDetector::ArcadeCompat::FBNEO_OK)
            pushLine("Kết luận: chuẩn FBNeo (core mới ăn được).");
        else if (ar.verdict == RomDetector::ArcadeCompat::MAME_ONLY)
            pushLine("Kết luận: dump kiểu MAME (chỉ core MAME ăn).");
        else
            pushLine("Kết luận: không đủ chứng cứ.");
    }
    m_textViewPath = g.filename;
    m_textViewScroll = 0;
    m_textViewReturn = UIState::GAME_LIST;
    setState(UIState::TEXT_VIEWER);
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
    m_textViewReturn = UIState::FILE_EXPLORER;
    setState(UIState::TEXT_VIEWER);
}

bool UIManager::handleTextViewerInput() {
    InputManager& input = InputManager::instance();
    if (input.isButtonJustPressed(Button::B)) {
        setState(m_textViewReturn);
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
