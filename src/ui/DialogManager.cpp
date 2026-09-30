#include "DialogManager.h"
#include "UiRenderer.h"
#include "UiStrings.h"
#include "UiTheme.h"
#include "../filesystem/FileSystemManager.h"
#include "../sync/DriveSyncEngine.h"
#include "../download/DownloadManager.h"
#include "../sync/UploadManager.h"
#include "../localsend/LocalSendManager.h"
#include <algorithm>
#include <cstdio>

namespace RomCloud {

static void drawBar(UiRenderer& ui, int x, int y, int w, int h, double frac,
                    SDL_Color fill, bool rounded, SDL_Color bg = {35, 42, 54, 255}) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int radius = rounded ? UiTheme::RADIUS_ROW : 0;
    if (rounded) ui.drawRoundedRect(x, y, w, h, radius, bg, true);
    else ui.drawRect(x, y, w, h, bg, true);
    int fw = (int)(w * frac);
    if (fw <= 0) return;
    if (rounded) ui.drawRoundedRect(x, y, fw, h, radius, fill, true);
    else ui.drawRect(x, y, fw, h, fill, true);
}

void DialogManager::renderToast(UiRenderer& ui, TTF_Font* fSmall) {
    if (!toast.visible()) return;
    uint32_t packed = toast.colorPacked;
    SDL_Color border = {(uint8_t)(packed >> 24), (uint8_t)(packed >> 16), (uint8_t)(packed >> 8), (uint8_t)packed};
    int toastW = 620, toastH = 48;
    int toastX = (1024 - toastW) / 2, toastY = 645;
    ui.drawRoundedRect(toastX, toastY, toastW, toastH, 24, {20, 24, 32, 245}, true);
    ui.drawRoundedBorder(toastX, toastY, toastW, toastH, 24, border, 2);
    ui.drawInlineHintsCentered(toast.message, 512, toastY + 14, {255, 255, 255, 255}, fSmall, 24);
}

void DialogManager::renderProgress(UiRenderer& ui, TTF_Font* fSmall, TTF_Font* fLarge) {
    if (!progress.visible) return;
    ui.beginModalDim();
    int dlgW = 620, dlgH = 220;
    int dlgX = (1024 - dlgW) / 2, dlgY = (768 - dlgH) / 2;
    ui.drawRoundedRect(dlgX, dlgY, dlgW, dlgH, UiTheme::RADIUS_MODAL, {24, 28, 38, 255}, true);
    ui.drawRect(dlgX, dlgY, dlgW, 48, {18, 55, 95, 255}, true);
    ui.drawText(progress.title, dlgX + dlgW / 2, dlgY + 14, {255, 255, 255, 255}, fLarge, true);
    if (!progress.detail.empty()) {
        ui.drawText(progress.detail, dlgX + dlgW / 2, dlgY + 80, {0, 180, 216, 255}, fSmall, true);
    }
    drawBar(ui, dlgX + 60, dlgY + 120, dlgW - 120, 18, progress.fraction(), {0, 180, 216, 255}, true);
    char pctBuf[32];
    std::snprintf(pctBuf, sizeof(pctBuf), "%.0f%%", progress.fraction() * 100.0);
    ui.drawText(pctBuf, dlgX + dlgW / 2, dlgY + 148, {255, 255, 255, 255}, fSmall, true);
}
void DialogManager::renderConfirm(UiRenderer& ui, TTF_Font* fSmall, TTF_Font* fMedium, TTF_Font* fLarge) {
    if (!confirm.visible) return;
    ui.beginModalDim();
    int dlgW = 680;
    int dlgH = 200 + static_cast<int>(confirm.lines.size()) * 34;
    if (dlgH < 300) dlgH = 300;
    if (dlgH > 460) dlgH = 460;
    int dlgX = (1024 - dlgW) / 2, dlgY = (768 - dlgH) / 2;
    SDL_Color titleBg = confirm.danger ? SDL_Color{185, 28, 28, 255} : SDL_Color{18, 55, 95, 255};
    ui.drawRoundedRect(dlgX, dlgY, dlgW, dlgH, UiTheme::RADIUS_MODAL, {24, 28, 38, 255}, true);
    ui.drawRect(dlgX, dlgY, dlgW, 48, titleBg, true);
    ui.drawText(confirm.title, dlgX + dlgW / 2, dlgY + 14, {255, 255, 255, 255}, fLarge, true);
    int y = dlgY + 80;
    for (size_t i = 0; i < confirm.lines.size(); ++i) {
        const std::string& ln = confirm.lines[i];
        SDL_Color c = {220, 225, 235, 255};
        TTF_Font* f = fSmall;
        bool centered = true;
        if (i == 0) { c = {255, 255, 255, 255}; f = fMedium; }
        else if (ln.rfind("- ", 0) == 0) { centered = false; }
        else if (ln.rfind("File:", 0) == 0) { c = {0, 180, 216, 255}; }
        if (centered) ui.drawText(ln, dlgX + dlgW / 2, y, c, f, true);
        else ui.drawText(ln, dlgX + 60, y, {200, 210, 225, 255}, f, false);
        y += (i == 0 ? 38 : 30);
        if (y > dlgY + dlgH - 90) break;
    }
    ui.drawBadge(dlgX + 60, dlgY + dlgH - 70, 240, 50, confirm.okLabel, {185, 28, 28, 255}, {255, 255, 255, 255});
    ui.drawBadge(dlgX + dlgW - 300, dlgY + dlgH - 70, 240, 50, confirm.cancelLabel, {55, 65, 81, 255}, {255, 255, 255, 255});
}
void DialogManager::renderSyncOverlay(UiRenderer& ui, TTF_Font* fSmall, TTF_Font* fMedium, TTF_Font* fLarge) {
    if (!DriveSyncEngine::instance().isSyncing()) return;
    ui.beginModalDim();
    int boxW = 700, boxH = 380;
    int boxX = (1024 - boxW) / 2, boxY = (768 - boxH) / 2;
    ui.drawRect(boxX, boxY, boxW, boxH, {22, 27, 36, 255}, true);
    ui.drawBorder(boxX, boxY, boxW, boxH, {0, 180, 216, 255}, 3);
    ui.drawRect(boxX, boxY, boxW, 55, {18, 55, 95, 255}, true);
    ui.drawText(UiStrings::HEADER_SYNC, boxX + boxW / 2, boxY + 16, {0, 180, 216, 255}, fLarge, true);
    auto prog = DriveSyncEngine::instance().getProgress();
    int contentY = boxY + 85;
    if (prog.status == SyncStatus::CONNECTING) {
        ui.drawText(UiStrings::SYNC_API_CONNECTING, boxX + boxW / 2, contentY, {255, 255, 255, 255}, fMedium, true);
        ui.drawText(UiStrings::SYNC_AUTH_TOKEN, boxX + boxW / 2, contentY + 35, {150, 165, 180, 255}, fSmall, true);
    } else if (prog.status == SyncStatus::DISCOVERING_FOLDERS) {
        ui.drawText(UiStrings::SYNC_SCANNING_GAMES, boxX + boxW / 2, contentY, {255, 255, 255, 255}, fMedium, true);
        ui.drawText(UiStrings::SYNC_SEARCH_FOLDERS, boxX + boxW / 2, contentY + 35, {150, 165, 180, 255}, fSmall, true);
    } else if (prog.status == SyncStatus::SYNCING_FILES) {
        std::string plat = "Dang quet: " + prog.currentPlatform;
        ui.drawText(plat, boxX + boxW / 2, contentY, {255, 255, 255, 255}, fMedium, true);
        std::string ps = "He may " + std::to_string(prog.currentSystemIndex) + " / " + std::to_string(prog.totalSystems);
        ui.drawText(ps, boxX + boxW / 2, contentY + 35, {0, 180, 216, 255}, fSmall, true);
        float pct = prog.totalSystems > 0 ? (float)prog.currentSystemIndex / (float)prog.totalSystems : 0.0f;
        drawBar(ui, boxX + 80, contentY + 70, 540, 16, pct, {0, 180, 216, 255}, false);
        std::string stats = "Da luu: " + std::to_string(prog.cloudGamesFound) + " (Moi: " +
            std::to_string(prog.newGamesIndexed) + ", CN: " + std::to_string(prog.updatedGames) + ")";
        ui.drawText(stats, boxX + boxW / 2, contentY + 110, {34, 197, 94, 255}, fSmall, true);
    }
    ui.drawBadge(boxX + (boxW - 220) / 2, boxY + 295, 220, 50, UiStrings::SYNC_CANCEL_BTN, {55, 65, 81, 255}, {255, 255, 255, 255});
}
void DialogManager::renderDownloadOverlay(UiRenderer& ui, TTF_Font* fSmall, TTF_Font* fLarge) {
    if (!DownloadManager::instance().isDownloading()) return;
    ui.beginModalDim();
    int boxW = 720, boxH = 390;
    int boxX = (1024 - boxW) / 2, boxY = (768 - boxH) / 2;
    ui.drawRect(boxX, boxY, boxW, boxH, {22, 27, 36, 255}, true);
    ui.drawBorder(boxX, boxY, boxW, boxH, {0, 180, 216, 255}, 3);
    ui.drawRect(boxX, boxY, boxW, 55, {18, 55, 95, 255}, true);
    ui.drawText(UiStrings::HEADER_DOWNLOAD, boxX + boxW / 2, boxY + 16, {0, 180, 216, 255}, fLarge, true);
    auto prog = DownloadManager::instance().getProgress();
    int contentY = boxY + 80;
    std::string title = prog.gameTitle;
    if (title.length() > 32) title = title.substr(0, 29) + "...";
    ui.drawText(title, boxX + boxW / 2, contentY, {255, 255, 255, 255}, fLarge, true);
    ui.drawText("He may: " + prog.systemCode + " | File: " + prog.filename,
                boxX + boxW / 2, contentY + 38, {0, 180, 216, 255}, fSmall, true);
    std::string statusMsg = UiStrings::DL_FROM_DRIVE;
    if (prog.state == DownloadState::INITIALIZING) statusMsg = UiStrings::DL_CONNECTING;
    else if (prog.state == DownloadState::VERIFYING) statusMsg = UiStrings::DL_CHECKING_FILE;
    ui.drawText(statusMsg, boxX + boxW / 2, contentY + 75, {245, 158, 11, 255}, fSmall, true);
    float pct = std::max(0.0, std::min(100.0, prog.progressPct));
    drawBar(ui, boxX + 80, contentY + 105, 560, 18, pct / 100.0, {34, 197, 94, 255}, false);
    std::string info = FileSystemManager::instance().formatBytes(prog.bytesDownloaded) +
        " / " + FileSystemManager::instance().formatBytes(prog.totalBytes);
    char pctBuf[32];
    std::snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", pct);
    info += " (" + std::string(pctBuf) + ")";
    ui.drawText(info, boxX + boxW / 2, contentY + 133, {255, 255, 255, 255}, fSmall, true);
    int remaining = DownloadManager::instance().queueSize();
    if (remaining > 0) {
        ui.drawText("Con " + std::to_string(remaining) + " game trong hang cho.",
                    boxX + boxW / 2, contentY + 161, {168, 85, 247, 255}, fSmall, true);
    }
    ui.drawBadge(boxX + (boxW - 220) / 2, boxY + 320, 220, 50, UiStrings::DL_CANCEL_BTN, {55, 65, 81, 255}, {255, 255, 255, 255});
}
void DialogManager::renderUploadOverlay(UiRenderer& ui, TTF_Font* fSmall) {
    if (!UploadManager::instance().isUploading()) return;
    auto prog = UploadManager::instance().getProgress();
    if (prog.state != UploadState::UPLOADING) return;
    int bannerW = 500, bannerH = 40;
    int bannerX = (1024 - bannerW) / 2, bannerY = 720;
    ui.drawRoundedRect(bannerX, bannerY, bannerW, bannerH, UiTheme::RADIUS_CARD, {30, 20, 45, 255}, true);
    ui.drawRoundedBorder(bannerX, bannerY, bannerW, bannerH, UiTheme::RADIUS_CARD, {168, 85, 247, 255}, 1);
    char pctBuf[16];
    std::snprintf(pctBuf, sizeof(pctBuf), "%.0f%%", prog.progressPct);
    std::string text = std::string(UiStrings::REVERSE_SYNC_UPLOADING) + prog.gameTitle + " " + pctBuf;
    if (text.length() > 55) text = text.substr(0, 52) + "...";
    ui.drawText(text, bannerX + bannerW / 2, bannerY + 10, {200, 210, 225, 255}, fSmall, true);
}

void DialogManager::renderLsRow(UiRenderer& ui, TTF_Font* fSmall, TTF_Font* fMedium,
                                bool isSend, int idx, int x, int y, int w, bool sel) {
    const int rowH = 88;
    if (sel) {
        ui.drawFocusRow(x, y, w, rowH);
    } else {
        ui.drawRoundedRect(x, y, w, rowH, UiTheme::RADIUS_ROW, UiTheme::ROW_BG, true);
        ui.drawRoundedBorder(x, y, w, rowH, UiTheme::RADIUS_ROW, UiTheme::CARD_BORDER, 1);
    }

    std::string name, pathVal, peer, status;
    uint64_t done = 0, tot = 0;
    uint32_t bps = 0;
    SDL_Color statusC = {148, 163, 184, 255};
    bool isDone = false;
    bool isFailed = false;

    if (isSend) {
        auto v = LocalSendManager::instance().sendProgresses();
        if (idx < 0 || idx >= (int)v.size()) return;
        const auto& s = v[(size_t)idx];
        name = s.fileName; pathVal = s.absPath;
        peer = s.toAlias.empty() ? s.toIp : s.toAlias;
        done = s.sentBytes; tot = s.totalBytes; bps = s.bytesPerSec;
        if (s.state == LsSendProgress::UPLOADING) {
            status = "Đang gửi..."; statusC = {0, 180, 216, 255};
        } else if (s.state == LsSendProgress::DONE) {
            status = "Hoàn thành"; statusC = {34, 197, 94, 255}; isDone = true;
        } else if (s.state == LsSendProgress::FAILED) {
            status = "Thất bại"; statusC = {239, 68, 68, 255}; isFailed = true;
        } else if (s.state == LsSendProgress::NEGOTIATING) {
            status = "Đang đàm phán..."; statusC = {250, 204, 21, 255};
        } else {
            status = "Chờ gửi..."; statusC = {148, 163, 184, 255};
        }
        ui.drawBadge(x + 16, y + (rowH - 34) / 2, 72, 34, "GỬI", {30, 58, 138, 255}, {255, 255, 255, 255});
    } else {
        auto v = LocalSendManager::instance().receiveProgresses();
        if (idx < 0 || idx >= (int)v.size()) return;
        const auto& r = v[(size_t)idx];
        name = r.file.fileName; pathVal = r.savedPath;
        peer = r.fromAlias.empty() ? r.fromIp : r.fromAlias;
        done = r.receivedBytes; tot = r.file.size; bps = r.bytesPerSec;
        if (r.state == LsUploadRequest::RECEIVING) {
            status = "Đang nhận..."; statusC = {0, 180, 216, 255};
        } else if (r.state == LsUploadRequest::DONE) {
            status = "Hoàn thành"; statusC = {34, 197, 94, 255}; isDone = true;
        } else if (r.state == LsUploadRequest::FAILED || r.state == LsUploadRequest::REJECTED) {
            status = (r.state == LsUploadRequest::REJECTED) ? "Bị từ chối" : "Thất bại";
            statusC = {239, 68, 68, 255}; isFailed = true;
        } else if (r.state == LsUploadRequest::PENDING) {
            status = "Chờ duyệt"; statusC = {250, 204, 21, 255};
        } else {
            status = "Đã chấp nhận"; statusC = {34, 197, 94, 255};
        }
        ui.drawBadge(x + 16, y + (rowH - 34) / 2, 72, 34, "NHẬN", {22, 101, 52, 255}, {255, 255, 255, 255});
    }

    int contentX = x + 102;
    int contentW = w - 120;

    // Line 1: Title and Peer
    std::string peerDisp = (isSend ? "Đích: " : "Nguồn: ") + peer;
    int peerW = ui.textWidth(peerDisp, fSmall);
    ui.drawTextRight(peerDisp, x + w - 18, y + 10, {148, 163, 184, 255}, fSmall);

    TTF_Font* titleFont = fMedium ? fMedium : fSmall;
    int maxTitleW = contentW - peerW - 24;
    if (maxTitleW < 220) maxTitleW = 220;
    std::string dispTitle = ui.truncateToWidth(name, titleFont, maxTitleW);
    ui.drawText(dispTitle, contentX, y + 8, {255, 255, 255, 255}, titleFont, false);

    // Line 2: Rounded Progress bar
    int barY = y + 42;
    int barH = 6;
    double frac = (tot == 0) ? 0.0 : (double)done / (double)tot;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;

    ui.drawRoundedRect(contentX, barY, contentW, barH, 3, {35, 45, 60, 255}, true);
    SDL_Color barCol = {0, 180, 216, 255};
    if (isDone) barCol = {34, 197, 94, 255};
    else if (isFailed) barCol = {239, 68, 68, 255};
    int fillW = (int)(contentW * frac);
    if (fillW > 0) {
        ui.drawRoundedRect(contentX, barY, std::max(fillW, 6), barH, 3, barCol, true);
    }

    // Line 3: Info & Status
    char infoBuf[160];
    snprintf(infoBuf, sizeof(infoBuf), "%d%%  •  %s / %s  •  %s",
             (int)(frac * 100 + 0.5), LsUtil::humanSize(done).c_str(),
             LsUtil::humanSize(tot).c_str(), LsUtil::humanSpeed(bps).c_str());
    ui.drawText(infoBuf, contentX, y + 56, {180, 190, 205, 255}, fSmall, false);
    ui.drawTextRight(status, x + w - 18, y + 56, statusC, fSmall);
}

void DialogManager::renderLocalSendProgress(UiRenderer& ui, TTF_Font* fSmall, TTF_Font* fMedium,
                                            TTF_Font* fLarge, TTF_Font* fTitle,
                                            int& sel, int& scroll) {
    ui.drawAppBackground();

    // Header 0..64 borderless
    ui.drawRect(0, 0, 1024, 64, {15, 23, 42, 255}, true);
    ui.drawRect(0, 64, 1024, 1, {30, 41, 59, 255}, true);
    ui.drawGridIcon("FILES.png", 16, 14, 36, 36);
    ui.drawText("TRUYỀN FILE", 62, 16, {255, 255, 255, 255}, fLarge ? fLarge : fTitle, false);

    auto sends = LocalSendManager::instance().sendProgresses();
    auto recvs = LocalSendManager::instance().receiveProgresses();
    int ns = (int)sends.size(), nr = (int)recvs.size();
    int total = ns + nr;

    char sub[128];
    snprintf(sub, sizeof(sub), "Gửi: %d  |  Nhận: %d", ns, nr);
    ui.drawTextRight(sub, 1024 - 24, 22, {0, 180, 216, 255}, fMedium ? fMedium : fSmall);

    // List area: 24..1000 (w=976), listY=74, listH=630
    const int px = 24, listY = 74, paneW = 976;
    const int rowH = 88, rowGap = 10;
    const int visibleRows = 6;

    if (total <= 0) {
        ui.drawText("Chưa có tác vụ truyền file nào.", px + paneW / 2, listY + 220,
                    {148, 163, 184, 255}, fLarge ? fLarge : fMedium, true);
        ui.drawText("Chọn ROM để gửi, hoặc chờ thiết bị khác gửi file đến.",
                    px + paneW / 2, listY + 265, {100, 116, 139, 255}, fSmall, true);
    } else {
        if (sel >= total) sel = total - 1;
        if (sel < 0) sel = 0;
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + visibleRows) scroll = sel - visibleRows + 1;

        for (int i = 0; i < visibleRows && scroll + i < total; ++i) {
            int idx = scroll + i;
            int y = listY + i * (rowH + rowGap);
            bool isS = idx < ns;
            renderLsRow(ui, fSmall, fMedium, isS, isS ? idx : idx - ns, px, y, paneW, idx == sel);
        }
    }

    // Footer 715..768 borderless
    ui.drawRect(0, 715, 1024, 53, {18, 22, 30, 255}, true);
    ui.drawRect(0, 715, 1024, 1, {40, 48, 62, 255}, true);
    ui.drawAppFooter({{UiTheme::PadBtn::DPAD, "Chọn tác vụ"},
                      {UiTheme::PadBtn::B, "Quay lại"},
                      {UiTheme::PadBtn::Y, "Xóa đã xong/lỗi"}});
}

void DialogManager::renderGlobalOverlays(UiRenderer& ui, TTF_Font* fSmall, TTF_Font* fMedium, TTF_Font* fLarge) {
    renderToast(ui, fSmall);
    renderProgress(ui, fSmall, fLarge);
    renderSyncOverlay(ui, fSmall, fMedium, fLarge);
    renderUploadOverlay(ui, fSmall);
}
} // namespace RomCloud
