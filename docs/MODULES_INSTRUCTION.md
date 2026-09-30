# RomCloud — Huong dan Module da boc tach (cho Agent)

> Muc dich: agent moi doc 1 file nay la hieu cau truc module, ham nao goi ham nao, khong phai mo UIManager.cpp 6676 dong.
> Stack: C++17, SDL2/SDL_ttf, TrimUI Brick 1024x768, build `bash ./build.sh` (zig c++ aarch64) / PC `bash ./build_pc.sh`.
> Namespace: `RomCloud::`. Tokens: `src/ui/UiTheme.h`. Chuoi UI: `src/ui/UiStrings.h` (giu nguyen).

## 0. Ban do tong quan

```
UIManager (dieu phoi state + input + goi module)
 +- UiRenderer      primitive ve SDL2
 +- DialogManager   toast/confirm/progress/sync/download/upload/localsend
 +- VirtualKeyboard logic phim ao QWERTY + Telex (khong SDL)
 +- FileExplorer    duyet thu muc + clipboard + CRUD (khong SDL)
 +- FileListView    helper layout list Master-Detail (static)
 +- MpvPlayer       fork mpv + IPC duy nhat (singleton)
 +- ImageCache      LRU cache SDL_Texture* dung chung
 +- BackgroundTask  worker thread chuan
 +- FileOps         = FileSystemManager mo rong
TelexHelper - dung noi bo boi VirtualKeyboard
UiTheme - Single Source of Truth mau/geometry/footer
```

Quy tac:
1. Module moi KHONG include UIManager. UIManager goi module.
2. Module logic (VirtualKeyboard/FileExplorer/BackgroundTask/ImageCache/FileListView) KHONG cham SDL_Renderer — tru UiRenderer/DialogManager/ImageCache.
3. Thread chi nam o BackgroundTask. FileSystemManager thuan filesystem (nhan TaskProgress* de bao tien trinh).
4. PID mpv duy nhat do MpvPlayer giu. Cam mirror m_mpvPid/waitpid/access sock o noi khac.

## 1. UiRenderer - src/ui/UiRenderer.h/.cpp (P0-1)

Vai tro: moi primitive ve SDL2. Tach ~26 ham draw* tu UIManager, giu nguyen chu ky.

Bind 1 lan:
```cpp
UiRenderer m_ui;
m_ui.bind(renderer, fSmall, fMedium, fLarge, AppConfig::getAssetsDir());
m_ui.unbind(); // khi thoat
```

Nhom ham:
- Co ban: drawText, drawRect, drawBorder, drawRoundedRect, drawRoundedBorder, drawBadge, drawIcon, drawPlayerIcon, drawButtonIcon, drawGridIcon
- Do chu: textHeight, textWidth, truncateToWidth, pillWidth
- Widget: drawPill, drawButton(label,focused,danger), drawRow, textYCentered, drawRowMainSub, drawTextRight, drawFooterHint, drawFooterHintsCentered, drawBadgeDual, drawInlineHintsCentered
- Theme: drawAppBackground, drawCard, drawFocusRow, drawAppHeader(title,sub), drawPadIcon(PadBtn,x,y,size), drawAppFooter(hints), beginModalDim
- Phim ao: drawVirtualKeyboard(vk,x,y,cellW,cellH,gapX,gapY,accent,accentEdge,actionLabels[5],withIcons,rounded,actionStride)
- Anh: images(), getImage(key,&w,&h), getOrLoadImage(key,path), clearImages(), clearTextCache()

actionStride=1 → col 0..4 (Explorer/LocalSend). =2 → col 0..9 kieu YT/TT cu (lay col/2).
Tinh trang: UIManager giu 26 wrapper draw* goi m_ui.draw* — KHONG xoa. Code moi goi thang m_ui.

## 2. VirtualKeyboard - src/ui/VirtualKeyboard.h (header-only, P0-2)

Vai tro: logic phim ao QWERTY dung chung, khong SDL. Gom 5+ bo m_*KbRow/Col/Shift.

```cpp
struct VkState { string query; int row=0 /*0..4*/; int col=0; bool shift=false, telexMode=true, inResults=false; size_t maxLen=60; int charset=0; /*0=Explorer,1=Media*/ };
enum class VkAction { None, Commit, Cancel, Backspace };
VirtualKeyboard::reset(s, telexDefault=true);
char ch = VirtualKeyboard::charAt(s);
VirtualKeyboard::typeChar(s,ch); // co Telex, false neu qua maxLen
VirtualKeyboard::typeSpace(s);
VirtualKeyboard::backspace(s);
bool h = VirtualKeyboard::move(s,dRow,dCol,stride2=false);
VkAction a = VirtualKeyboard::pressA(s,toastFn,stride2=false);
VkAction a = VirtualKeyboard::pressB(s); // con chu→Backspace, het chu→Cancel
VkAction a = VirtualKeyboard::pressAction(idx,s,toastFn); // idx 0..4
```

Mapping: UP/DOWN/LEFT/RIGHT di chuyen | A chon | B xoa/thoat | X space | Y xoa | L1 Shift | R1 Telex | START OK.
Layout: 4 hang phim 10 cot (lowerRows/upperRows + mediaLowerRows/mediaUpperRows legacy) + 1 hang action 5 o Shift/Space/Xoa/Xong/Huy.
Render: goi UiRenderer::drawVirtualKeyboard. Tich hop TelexHelper::processTelex.

## 3. DialogManager - src/ui/DialogManager.h/.cpp (P0-3)

Vai tro: state + render 6 overlay gom tu UIManager. UIManager chi giu wrapper 1 dong.

```cpp
ToastState { message,colorPacked,durationMs,expiryMs; show(msg,color,ms); visible(); clear(); }
ConfirmDialog { visible,title,lines,okLabel,cancelLabel,danger,onOk; open(t,body,ok,danger); confirm(); cancel(); }
ProgressDialog { visible,title,detail,done,total,cancellable,cancelRequested; open(t,c); update(d,t); fraction(); requestCancel(); close(); }
class DialogManager { ToastState toast; ConfirmDialog confirm; ProgressDialog progress;
  void toastMsg(m,c,ms);
  void renderToast(ui,fSmall);
  void renderConfirm(ui,fSmall,fMedium,fLarge);
  void renderProgress(ui,fSmall,fLarge);
  void renderSyncOverlay(ui,fSmall,fMedium,fLarge);
  void renderDownloadOverlay(ui,fSmall,fLarge);
  void renderUploadOverlay(ui,fSmall);
  void renderLsRow(ui,fSmall,isSend,idx,x,y,w,sel);
  void renderLocalSendProgress(ui,fSmall,fMedium,fLarge,fTitle,sel,scroll);
  void renderGlobalOverlays(ui,fSmall,fMedium,fLarge); // goi moi frame
};
```

Cach dung: toastMsg("Da xoa"); confirm.open("Xoa?",body,onOk); progress.open("Copy...",true); progress.update(done,total);

## 4. BackgroundTask - src/common/BackgroundTask.h (header-only, P1-1)

Worker thread chuan. UI poll moi frame de ve progress, khong block.

```cpp
struct TaskProgress { atomic<uint64_t> done,total; atomic<bool> cancel,finished,running; string error; };
class BackgroundTask {
  template<typename Fn> void run(Fn&& fn); // fn(TaskProgress&)
  TaskProgress& progress();
  void requestCancel(); bool isRunning(); bool isFinished(); double fraction();
  void wait();
};
```

Pattern:
```cpp
BackgroundTask task;
task.run([](TaskProgress& p){ for(...) { if(p.cancel) break; p.done++; p.total=N; } });
// moi frame: ve task.fraction(); nut B → task.requestCancel();
```

Dung cho: FileExplorer copy/paste, Download, OTA, DriveSync, thumbnail.

## 5. FileOps = FileSystemManager mo rong - src/filesystem/FileSystemManager.h/.cpp (P1-2)

Thuan filesystem, khong UI/thread.

```cpp
bool renamePath(from,to);
bool removeRecursive(path, TaskProgress* prog=nullptr);
bool copyRecursive(src,dst, TaskProgress* prog=nullptr); // false neu dst trong src
DirStats getDirStats(path); // {bytes,files,dirs}
int64_t getModTime(path);
static bool isSubPath(parent,child);
```

prog==nullptr → chay dong bo. Co prog → cap nhat done/total, kiem tra cancel.

## 6. FileListView - src/ui/FileListView.h (header-only, P2-1 phu)

Helper list 2 cot Master-Detail, khong giu state.

```cpp
struct FileListRow { string title,sub; bool isDir; string badge; /*CUT/COPY*/ };
class FileListView {
  static int calcScroll(selected,visibleRows,prevScroll);
  static int calcVisibleRows(listH,rowH);
  static string shorten(s,maxChars=26);
  struct Layout { leftX=24,leftY=128,leftW=624,leftH=574; rightX=664,rightW=336,rightY=128,rightH=574; rowH=56; };
  static Layout defaultLayout();
};
```

Dung cho FileExplorer + LocalSend picker.

## 7. FileExplorer - src/fileexplorer/FileExplorer.h (header-only, P2-1 chinh)

Logic duyet thu muc + clipboard 2 buoc, khong SDL. UIManager chi goi enter()/refresh() roi tu ve.

```cpp
struct ExplorerEntry { string name,path; bool isDir; uint64_t sizeBytes; int64_t mtime; };
struct ExplorerClipboard { enum class Op{NONE,COPY,CUT}; Op op; string srcPath; bool srcIsDir; void clear(); bool active(); };
class FileExplorer {
  currentPath(); entries(); selected(); clipboard(); dialogs(); task(); keyboard();
  creatingFolder(); renaming(); hideJunk(); romOnly();
  bool open(path); bool refresh(); void moveSel(d);
  const ExplorerEntry* current();
  bool enter(); bool goUp();
  void beginCreateFolder(); void beginRename();
  bool commitCreateOrRename(outErr);
  void cutCurrent(); void copyCurrent(); void cancelClipboard();
  bool pasteHere(...); // chan isSubPath, chay BackgroundTask + copyRecursive
  void requestDeleteCurrent(onDone); // confirm → task.run(removeRecursive)
  static string propertiesOf(e);
  string suggestNewFolderName();
  static bool isJunk(name); static bool isRomOrMedia(name);
};
```

Clipboard: X=Cut, Y=Copy, R1/START=Paste, SELECT=huy. Filter an .DS_Store/Thumbs.db/._*, whitelist rom/media. Mapping: UP/DOWN chon, A vao folder, B lui, L1 menu (New/Rename/Delete/Properties/Filter).

## 8. MpvPlayer - src/media/MpvPlayer.h/.cpp (singleton, P1-3)

Fork mpv + IPC dung chung cho IPTV/YouTube/TikTok. Manager chi dua URL + mapping nut.

```cpp
class MpvPlayer {
  static MpvPlayer& instance();
  bool play(url, extraArgs={}, sockPath="/tmp/mpv_iptv.sock", logFile="");
  bool isPlaying(); pid_t pid(); const string& sockPath();
  bool sendCmd(json, response*=nullptr, sockOverride="");
  bool showText(text,ms,sockOverride="");
  bool seekRelative(seconds,sockOverride="");
  bool cyclePause(sockOverride="");
  bool showOverlayIcon(appRoot,iconName,durationMs,sockOverride="");
  bool pollExited(); bool waitForSocket(timeoutMs=2500); bool stop();
  static string findBinary(appRoot,sdRoot);
  static string resolveOsdFont(appRoot);
};
```

Quy tac: PID duy nhat o day. IPTVManager::sendMpvIpcCommand uy quyen sendCmd.

## 9. ImageCache - src/ui/ImageCache.h (header-only, P2-2)

1 LRU cache cho moi SDL_Texture*. Gom grid/system/button cache, ytThumbnails, CoverManager.

```cpp
class ImageCache {
  explicit ImageCache(maxItems=128);
  SDL_Texture* get(key,&w,&h);
  bool contains(key);
  void put(key,tex,w,h); // tu destroy cu, tu trim
  size_t retainOnly(keepSet);
  void clear(); void setMax(n); size_t size();
};
```

UiRenderer giu ImageCache m_images{256} + wrapper getImage/getOrLoadImage/clearImages.

## 10. Phu tro giu nguyen

- TelexHelper (src/ui/TelexHelper.h): processTelex, popUtf8, splitUtf8, bang dau. Chi goi qua VirtualKeyboard.
- UiTheme (src/ui/UiTheme.h): token mau, APP_W=1024/APP_H=768, FOOTER_Y/H, Layout A 65/35, enum PadBtn, struct FooterHint.

## 11. Checklist khi them man hinh moi

1. Ve → m_ui.draw*, mau/geometry tu UiTheme::, khong hardcode.
2. Nhap lieu → 1 VkState + VirtualKeyboard::*, ve bang drawVirtualKeyboard.
3. Popup → DialogManager, overlay global goi renderGlobalOverlays moi frame.
4. Tac vu nang → BackgroundTask::run, poll fraction(), nut huy → requestCancel().
5. File → FileSystemManager (FileOps), list → FileListView::calcScroll/defaultLayout.
6. Phat video → MpvPlayer::instance().play/sendCmd, khong fork/waitpid/socket tay.
7. Anh → UiRenderer::images()/ImageCache, khong tao map cache moi.
8. Xong: them .cpp moi vao build.sh, chay bash ./build.sh, kiem tra ls -lh bin/RomCloud.
