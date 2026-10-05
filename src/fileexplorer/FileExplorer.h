#pragma once
// RomCloud FileExplorer — logic duyet thu muc + clipboard 2 buoc, khong SDL.
// UIManager chi goi enter()/handleButton()/refresh() va tu ve UI.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include "../common/BackgroundTask.h"
#include "../archive/ArchiveEngine.h"
#include "../config/AppConfig.h"
#include "../filesystem/FileSystemManager.h"
#include "../ui/DialogManager.h"
#include "../ui/VirtualKeyboard.h"

namespace RomCloud {

struct ExplorerEntry {
  std::string name;
  std::string path;
  bool isDir = false;
  uint64_t sizeBytes = 0;
  int64_t mtime = 0;
};

struct ExplorerClipboard {
  enum class Op { NONE, COPY, CUT };
  Op op = Op::NONE;
  std::string srcPath;
  bool srcIsDir = false;
  void clear() {
    op = Op::NONE;
    srcPath.clear();
    srcIsDir = false;
  }
  bool active() const { return op != Op::NONE && !srcPath.empty(); }
};

class FileExplorer {
public:
  FileExplorer() = default;

  const std::string &currentPath() const { return m_path; }
  const std::vector<ExplorerEntry> &entries() const { return m_entries; }
  int selected() const { return m_selected; }
  const ExplorerClipboard &clipboard() const { return m_clip; }
  DialogManager &dialogs() { return m_dialogs; }
  BackgroundTask &task() { return m_task; }
  VkState &keyboard() { return m_kb; }
  bool creatingFolder() const { return m_creating; }
  bool renaming() const { return m_renaming; }
  bool hideJunk() const { return m_hideJunk; }
  bool romOnly() const { return m_romOnly; }
  bool dirOnly() const { return m_dirOnly; }
  void setHideJunk(bool v) {
    m_hideJunk = v;
    refresh();
  }
  void setRomOnly(bool v) {
    m_romOnly = v;
    refresh();
  }
  void setDirOnly(bool v) {
    m_dirOnly = v;
    refresh();
  }

  bool open(const std::string &path) {
    m_path = normalize(path);
    m_selected = 0;
    return refresh();
  }

  bool refresh() {
    m_entries.clear();
    auto raw = FileSystemManager::instance().listDirectory(m_path, m_dirOnly);
    for (const auto &e : raw) {
      if (m_hideJunk && isJunk(e.name))
        continue;
      if (m_dirOnly && !e.isDirectory)
        continue;
      if (m_romOnly && !e.isDirectory && !isRomOrMedia(e.name))
        continue;
      ExplorerEntry out;
      out.name = e.name;
      out.path = e.path;
      out.isDir = e.isDirectory;
      out.sizeBytes = e.isDirectory ? 0 : e.sizeBytes;
      if (!e.isDirectory)
        out.mtime = FileSystemManager::instance().getModTime(e.path);
      m_entries.push_back(std::move(out));
    }
    if (m_selected >= (int)m_entries.size())
      m_selected = (int)m_entries.size() - 1;
    if (m_selected < 0)
      m_selected = 0;
    return true;
  }

  void moveSel(int d) {
    if (m_entries.empty()) {
      m_selected = 0;
      return;
    }
    m_selected += d;
    if (m_selected < 0)
      m_selected = 0;
    if (m_selected >= (int)m_entries.size())
      m_selected = (int)m_entries.size() - 1;
  }

  const ExplorerEntry *current() const {
    if (m_selected < 0 || m_selected >= (int)m_entries.size())
      return nullptr;
    return &m_entries[(size_t)m_selected];
  }

  bool enter() {
    const ExplorerEntry *e = current();
    if (!e || !e->isDir)
      return false;
    m_path = e->path;
    m_selected = 0;
    return refresh();
  }

  bool goUp() {
    if (m_path.empty() || m_path == "/")
      return false;
    size_t p = m_path.find_last_of('/');
    m_path = (p == std::string::npos || p == 0) ? "/" : m_path.substr(0, p);
    m_selected = 0;
    return refresh();
  }

  void markCut() {
    const ExplorerEntry *e = current();
    if (!e)
      return;
    m_clip.op = ExplorerClipboard::Op::CUT;
    m_clip.srcPath = e->path;
    m_clip.srcIsDir = e->isDir;
    m_dialogs.toastMsg(std::string("CUT: ") + e->name);
  }
  void markCopy() {
    const ExplorerEntry *e = current();
    if (!e)
      return;
    m_clip.op = ExplorerClipboard::Op::COPY;
    m_clip.srcPath = e->path;
    m_clip.srcIsDir = e->isDir;
    m_dialogs.toastMsg(std::string("COPY: ") + e->name);
  }
  void cancelClipboard() {
    m_clip.clear();
    m_dialogs.toastMsg("Đã hủy clipboard");
  }

  bool paste() { return pasteTo(m_path, nullptr); }
  bool pasteTo(const std::string &destDir, const ExplorerClipboard *extClip) {
    const ExplorerClipboard &clip = extClip ? *extClip : m_clip;
    if (!clip.active())
      return false;
    std::string base = basenameOf(clip.srcPath);
    if (base.empty())
      return false;
    std::string dd = destDir.empty() ? m_path : destDir;
    while (dd.size() > 1 && dd.back() == '/')
      dd.pop_back();
    std::string dst = dd + "/" + base;
    if (dst == clip.srcPath)
      return false;
    if (FileSystemManager::isSubPath(clip.srcPath, dst)) {
      m_dialogs.toastMsg("Không dán vào chính nó!");
      return false;
    }
    struct stat st;
    if (stat(dst.c_str(), &st) == 0) {
      m_dialogs.toastMsg("Đích đã tồn tại!");
      return false;
    }
    bool isCut = (clip.op == ExplorerClipboard::Op::CUT);
    std::string src = clip.srcPath;
    DirStats s = FileSystemManager::instance().getDirStats(src);
    uint64_t total = s.bytes > 0 ? s.bytes : (uint64_t)(s.files + s.dirs + 1);
    m_task.run([src, dst, isCut, total](TaskProgress &prog) {
      prog.total = total;
      prog.done = 0;
      bool ok;
      if (isCut) {
        if (rename(src.c_str(), dst.c_str()) == 0) {
          ok = true;
          prog.done.store(prog.total.load());
        } else {
          ok = FileSystemManager::instance().copyRecursive(src, dst, &prog);
          if (ok && !prog.cancel)
            FileSystemManager::instance().removeRecursive(src, nullptr);
        }
      } else {
        ok = FileSystemManager::instance().copyRecursive(src, dst, &prog);
      }
      if (prog.cancel)
        prog.error = "Cancelled";
      else if (!ok)
        prog.error = "Dán thất bại";
      prog.finished = true;
    });
    return true;
  }

  void beginCreateFolder() {
    VirtualKeyboard::reset(m_kb, false);
    m_kb.query = suggestNewFolderName();
    m_creating = true;
    m_renaming = false;
  }
  void beginRename() {
    const ExplorerEntry *e = current();
    if (!e)
      return;
    VirtualKeyboard::reset(m_kb, false);
    m_kb.query = e->name;
    m_creating = false;
    m_renaming = true;
  }
  void cancelKeyboard() { m_creating = m_renaming = false; }

  bool commitKeyboard() {
    std::string name = trim(m_kb.query);
    if (name.empty() || name.find('/') != std::string::npos) {
      m_dialogs.toastMsg("Tên không hợp lệ!");
      return false;
    }
    if (m_creating) {
      std::string dst = m_path + "/" + name;
      struct stat st;
      if (stat(dst.c_str(), &st) == 0) {
        m_dialogs.toastMsg("Đã tồn tại!");
        return false;
      }
      if (mkDir(dst) != 0) {
        m_dialogs.toastMsg("Tạo thư mục thất bại!");
        return false;
      }
      m_creating = false;
      refresh();
      return true;
    }
    if (m_renaming) {
      const ExplorerEntry *e = current();
      if (!e) {
        m_renaming = false;
        return false;
      }
      std::string dst = m_path + "/" + name;
      if (!FileSystemManager::instance().renamePath(e->path, dst)) {
        m_dialogs.toastMsg("Đổi tên thất bại (trùng tên?)!");
        return false;
      }
      m_renaming = false;
      refresh();
      return true;
    }
    return false;
  }

  // Bung file .zip/.rar/.7z đang chọn vào chính thư mục đang đứng.
  // Chạy nền qua m_task → progress dialog + hủy + toast có sẵn.
  bool extractArchiveHere() {
    return extractArchiveTo(m_path, "");
  }

  // Bung archive (mặc định = file đang chọn) vào dstDir.
  bool extractArchiveTo(const std::string& dstDir, const std::string& archiveOverride = "") {
    const ExplorerEntry *e = current();
    std::string archive = archiveOverride.empty() ? (e ? e->path : "") : archiveOverride;
    if (archive.empty() || !ArchiveEngine::isArchive(archive))
      return false;
    if (archiveOverride.empty() && (!e || e->isDir))
      return false;
    struct stat st;
    if (stat(archive.c_str(), &st) != 0)
      return false;
    std::string dst = dstDir;
    std::string appRoot = AppConfig::instance().getAppRoot();
    ArchiveEngine::Tool tool = ArchiveEngine::findTool(appRoot, archive);
    if (!tool.ok) {
      m_dialogs.toastMsg(ArchiveEngine::missingToolError());
      return false;
    }
    m_task.run([tool, archive, dst](TaskProgress &prog) {
      bool ok = ArchiveEngine::extract(tool.bin, tool.use7z, archive, dst, prog);
      if (prog.cancel)
        prog.error = "Cancelled";
      else if (!ok && prog.error.empty())
        prog.error = "Bung thất bại";
    });
    return true;
  }

  void askDelete(std::function<void(bool)> onDone = nullptr) {
    const ExplorerEntry *e = current();
    if (!e)
      return;
    std::string target = e->path;
    std::string fname = e->name;
    std::string sizeLine = e->isDir ? "" : FileSystemManager::instance().formatBytes(e->sizeBytes);
    std::vector<std::string> step1 = {fname};
    if (!sizeLine.empty()) step1.push_back(sizeLine);
    step1.push_back("Nhấn A để tiếp tục, B để hủy");
    m_dialogs.confirm.open("Xóa? (1/2)", step1, [this, target, fname, sizeLine, onDone]() {
      std::vector<std::string> step2 = {fname};
      if (!sizeLine.empty()) step2.push_back(sizeLine);
      step2.push_back("Xóa hẳn, không khôi phục!");
      step2.push_back("Nhấn A để xóa, B để hủy");
      m_dialogs.confirm.open("Chắc chắn xóa? (2/2)", step2, [this, target, onDone]() {
        m_task.run([target](TaskProgress &prog) {
          bool ok = FileSystemManager::instance().removeRecursive(target, &prog);
          if (!ok && prog.error.empty())
            prog.error = "Xóa thất bại";
          prog.finished = true;
        });
        if (onDone)
          onDone(true);
        refresh();
      });
    });
  }

  static std::string propertiesOf(const ExplorerEntry &e) {
    std::string out = e.name + "\n";
    if (e.isDir) {
      DirStats s = FileSystemManager::instance().getDirStats(e.path);
      out += std::to_string(s.files) + " file / " + std::to_string(s.dirs) +
             " folder\n";
      out += FileSystemManager::instance().formatBytes(s.bytes);
    } else {
      out += FileSystemManager::instance().formatBytes(e.sizeBytes);
    }
    return out;
  }

  std::string suggestNewFolderName() const {
    for (int i = 1;; ++i) {
      std::string n =
          (i == 1) ? "Thư mục mới" : "Thư mục mới " + std::to_string(i);
      struct stat st;
      if (stat((m_path + "/" + n).c_str(), &st) != 0)
        return n;
    }
  }

  static bool isJunk(const std::string &name) {
    if (name == ".DS_Store" || name == "Thumbs.db")
      return true;
    if (name.size() > 2 && name[0] == '.' && name[1] == '_')
      return true;
    return false;
  }

  static bool isRomOrMedia(const std::string &name) {
    size_t p = name.find_last_of('.');
    if (p == std::string::npos)
      return false;
    std::string ext = name.substr(p + 1);
    for (auto &c : ext)
      c = (char)tolower((unsigned char)c);
    static const char *k[] = {"gba", "gbc", "gb",  "nes",  "snes", "smc",
                              "sfc", "md",  "zip", "7z",   "mp4",  "mkv",
                              "mp3", "png", "jpg", nullptr};
    for (int i = 0; k[i]; ++i) {
      if (ext == k[i])
        return true;
    }
    return false;
  }

private:
  static std::string normalize(const std::string &p) {
    if (p.empty())
      return "/";
    std::string o = p;
    while (o.size() > 1 && o.back() == '/')
      o.pop_back();
    return o;
  }
  static std::string basenameOf(const std::string &p) {
    size_t i = p.find_last_of('/');
    return (i == std::string::npos) ? p : p.substr(i + 1);
  }
  static int mkDir(const std::string &p) {
#if defined(_WIN32)
    return mkdir(p.c_str());
#else
    return mkdir(p.c_str(), 0755);
#endif
  }
  static std::string trim(const std::string &s) {
    size_t a = 0;
    while (a < s.size() && isspace((unsigned char)s[a]))
      ++a;
    size_t b = s.size();
    while (b > a && isspace((unsigned char)s[b - 1]))
      --b;
    return s.substr(a, b - a);
  }

  std::string m_path = "/";
  std::vector<ExplorerEntry> m_entries;
  int m_selected = 0;
  ExplorerClipboard m_clip;
  DialogManager m_dialogs;
  BackgroundTask m_task;
  VkState m_kb;
  bool m_creating = false;
  bool m_renaming = false;
  bool m_hideJunk = true;
  bool m_romOnly = false;
  bool m_dirOnly = false;
};

} // namespace RomCloud
