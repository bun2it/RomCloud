#include "UpdateManager.h"
#include "../config/AppConfig.h"
#include "../filesystem/FileSystemManager.h"
#include "../logging/Logger.h"
#include "../network/HttpClient.h"
#include "../network/JsonHelper.h"

#include <curl/curl.h>
#include <SDL2/SDL.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <fstream>
#include <algorithm>

namespace RomCloud {

UpdateManager &UpdateManager::instance() {
  static UpdateManager instance;
  return instance;
}

UpdateManager::~UpdateManager() { shutdown(); }

bool UpdateManager::init() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_progress.state = UpdateState::IDLE;
  m_hasUpdate = false;
  Logger::info("UpdateManager initialized. Current version: v" + std::string(APP_VERSION));

  // Auto-detect OS and log it
  std::string osType = AppConfig::instance().getOSName();
  Logger::info("Detected OS: " + osType);

  return true;
}

void UpdateManager::shutdown() { cancelUpdate(); }

// ============================================================================
// VERSION COMPARISON
// ============================================================================

bool UpdateManager::isVersionNewer(const std::string &remote,
                                   const std::string &current) {
  std::string r = remote;
  std::string c = current;
  if (!r.empty() && (r.front() == 'v' || r.front() == 'V'))
    r.erase(0, 1);
  if (!c.empty() && (c.front() == 'v' || c.front() == 'V'))
    c.erase(0, 1);

  auto parseParts = [](const std::string &str) {
    std::vector<int> parts;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, '.')) {
      try {
        parts.push_back(std::stoi(item));
      } catch (...) {
        parts.push_back(0);
      }
    }
    while (parts.size() < 3)
      parts.push_back(0);
    return parts;
  };

  auto rParts = parseParts(r);
  auto cParts = parseParts(c);

  for (size_t i = 0; i < 3; ++i) {
    if (rParts[i] > cParts[i])
      return true;
    if (rParts[i] < cParts[i])
      return false;
  }
  return false;
}

// ============================================================================
// CHECK FOR UPDATES
// ============================================================================

bool UpdateManager::checkForUpdatesSync(UpdateInfo &outInfo) {
  Logger::info("Checking for OTA updates...");

  // Get OS type for OS-specific bundles and targeting
  OSType currentOSType = AppConfig::instance().getOSType();
  if (currentOSType == OSType::AUTO) {
    currentOSType = AppConfig::instance().detectOSType();
    AppConfig::instance().setOSType(currentOSType);
  }
  std::string osType = OSTypeToString(currentOSType);

  // Standard canonical OS key: STOCK_PS, NEXTUI, SPRUCE_OS
  std::string osKey = "STOCK_PS";
  switch (currentOSType) {
    case OSType::STOCK_PS:  osKey = "STOCK_PS"; break;
    case OSType::NEXTUI:    osKey = "NEXTUI"; break;
    case OSType::SPRUCE_OS: osKey = "SPRUCE_OS"; break;
    default:                osKey = "STOCK_PS"; break;
  }

  std::vector<std::string> headers = {
      "User-Agent: RomCloud-OTA/1.0",
      "Accept: application/vnd.github.v3+json"};

  // 1. Try version.json manifest
  std::string manifestUrl = std::string(VERSION_MANIFEST_URL) +
                            "?t=" + std::to_string(std::time(nullptr));
  std::vector<std::string> manifestHeaders = {
      "User-Agent: RomCloud-OTA/1.0",
      "Cache-Control: no-cache, no-store, must-revalidate", "Pragma: no-cache"};
  HttpResponse mResp = HttpClient::instance().get(manifestUrl, manifestHeaders);

  std::string remoteVer = "";
  std::string targetOs = "ALL";
  std::string iconUrl = "";
  std::string changelog = "";
  std::string relDate = "";
  std::string binUrl = "";
  std::string bundleUrl = "";
  std::string osBundleUrl = "";

  if (mResp.success && !mResp.body.empty() && mResp.statusCode == 200) {
    // 1. Check target_os
    targetOs = JsonHelper::extractString(mResp.body, "target_os");
    if (targetOs.empty()) {
      targetOs = "ALL";
    }

    // Verify whether this update is intended for current OS
    bool osMatches = false;
    if (targetOs == "ALL" || targetOs == "all" || targetOs == "*") {
      osMatches = true;
    } else {
      if (targetOs.find(osKey) != std::string::npos ||
          targetOs.find(osType) != std::string::npos) {
        osMatches = true;
      } else {
        std::string lowerTarget = targetOs;
        std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(), ::tolower);
        std::string lowerKey = osKey;
        std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);
        std::string lowerType = osType;
        std::transform(lowerType.begin(), lowerType.end(), lowerType.begin(), ::tolower);
        if (lowerTarget.find(lowerKey) != std::string::npos || lowerTarget.find(lowerType) != std::string::npos) {
          osMatches = true;
        }
      }
    }

    if (!osMatches) {
      Logger::info("OTA update is targeted for [" + targetOs + "] but device is [" + osType + "/" + osKey + "]. Skipping OTA notification.");
      std::lock_guard<std::mutex> lock(m_mutex);
      m_hasUpdate = false;
      m_progress.state = UpdateState::UP_TO_DATE;
      return false;
    }

    // 2. Version determination: check OS-specific override first, then general version
    std::string osVer = JsonHelper::extractString(mResp.body, osKey + "_version");
    if (osVer.empty()) {
      osVer = JsonHelper::extractString(mResp.body, osKey);
    }
    if (!osVer.empty()) {
      remoteVer = osVer;
    } else {
      remoteVer = JsonHelper::extractString(mResp.body, "version");
    }

    iconUrl = JsonHelper::extractString(mResp.body, "icon_url");

    binUrl = JsonHelper::extractString(mResp.body, "binary_url");
    if (binUrl.empty())
      binUrl = JsonHelper::extractString(mResp.body, "download_url");
    bundleUrl = JsonHelper::extractString(mResp.body, "bundle_url");
    osBundleUrl = JsonHelper::extractString(mResp.body, "os_bundle_url");

    // Check for OS-specific bundle (check osKey first e.g. SPRUCE_OS_bundle_url, then osType)
    if (osBundleUrl.empty()) {
      osBundleUrl = JsonHelper::extractString(mResp.body, osKey + "_bundle_url");
    }
    if (osBundleUrl.empty()) {
      osBundleUrl = JsonHelper::extractString(mResp.body, osType + "_bundle_url");
    }

    // Changelog: OS-specific changelog first, then general changelog
    std::string osChangelog = JsonHelper::extractString(mResp.body, osKey + "_changelog");
    if (!osChangelog.empty()) {
      changelog = osChangelog;
    } else {
      changelog = JsonHelper::extractString(mResp.body, "changelog");
    }
    relDate = JsonHelper::extractString(mResp.body, "release_date");
  }

  // 2. Fallback to GitHub Releases API if manifest was empty
  if (remoteVer.empty()) {
    Logger::info("Checking GitHub Releases API as fallback...");
    std::string apiEndpoint = "https://api.github.com/repos/" +
                              std::string(GITHUB_REPO) + "/releases/latest";
    HttpResponse resp = HttpClient::instance().get(apiEndpoint, headers);
    if (resp.success && !resp.body.empty() && resp.statusCode == 200) {
      std::string tag = JsonHelper::extractString(resp.body, "tag_name");
      if (!tag.empty()) {
        remoteVer = tag;
        if (remoteVer.front() == 'v' || remoteVer.front() == 'V') {
          remoteVer.erase(0, 1);
        }
        changelog = JsonHelper::extractString(resp.body, "body");
        relDate = JsonHelper::extractString(resp.body, "published_at");
        if (relDate.length() >= 10)
          relDate = relDate.substr(0, 10);

        // Get download URLs from release assets
        auto assets = JsonHelper::extractArrayObjects(resp.body, "assets");
        for (const auto &asset : assets) {
          std::string name = JsonHelper::extractString(asset, "name");
          std::string url = JsonHelper::extractString(asset, "browser_download_url");

          if (name == "RomCloud" || name == "RomCloud.bin") {
            binUrl = url;
          } else if (name == "icon.png" || name == "APP.png") {
            iconUrl = url;
          } else if (name == "mpv_bundle.zip" || name == "mpv_bundle-" + osType + ".zip") {
            bundleUrl = url;
          } else if (name.find("_bundle.zip") != std::string::npos) {
            // Check OS-specific bundle
            std::string lowerOsKey = osKey;
            std::transform(lowerOsKey.begin(), lowerOsKey.end(), lowerOsKey.begin(), ::tolower);
            std::string lowerOsType = osType;
            std::transform(lowerOsType.begin(), lowerOsType.end(), lowerOsType.begin(), ::tolower);
            if (name.find(lowerOsKey) != std::string::npos || name.find(lowerOsType) != std::string::npos) {
              osBundleUrl = url;
            }
          }
        }
        if (binUrl.empty()) {
          binUrl = "https://github.com/" + std::string(GITHUB_REPO) +
                   "/releases/download/v" + remoteVer + "/RomCloud";
        }
      }
    }
  }

  if (remoteVer.empty()) {
    Logger::warn("OTA check failed to obtain remote version.");
    return false;
  }

  // Build default URLs if not found
  if (binUrl.empty()) {
    binUrl = "https://github.com/" + std::string(GITHUB_REPO) +
             "/releases/download/v" + remoteVer + "/RomCloud";
  }
  if (bundleUrl.empty()) {
    bundleUrl = "https://github.com/" + std::string(GITHUB_REPO) +
                "/releases/download/v" + remoteVer + "/mpv_bundle.zip";
  }
  if (iconUrl.empty()) {
    iconUrl = "https://raw.githubusercontent.com/" + std::string(GITHUB_REPO) + "/main/icon.png";
  }

  // OS-specific bundle URL
  if (osBundleUrl.empty()) {
    osBundleUrl = "https://github.com/" + std::string(GITHUB_REPO) +
                  "/releases/download/v" + remoteVer + "/bundle-" + osKey + ".zip";
  }

  outInfo.remoteVersion = remoteVer;
  outInfo.downloadUrl = binUrl;
  outInfo.iconUrl = iconUrl;
  outInfo.bundleUrl = bundleUrl;
  outInfo.osBundleUrl = osBundleUrl;
  outInfo.changelog = changelog;
  outInfo.releaseDate = relDate;
  outInfo.osType = osType;
  outInfo.targetOs = targetOs;

  bool newer = isVersionNewer(remoteVer, APP_VERSION);
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_latestInfo = outInfo;
    m_hasUpdate = newer;
    m_progress.newVersion = remoteVer;
    m_progress.state =
        newer ? UpdateState::UPDATE_AVAILABLE : UpdateState::UP_TO_DATE;
  }

  if (newer) {
    Logger::info("New OTA update available: v" + remoteVer + " (Current: v" +
                 std::string(APP_VERSION) + ") for OS: " + osType);
  } else {
    Logger::info("RomCloud is up to date (v" + std::string(APP_VERSION) +
                 ") on " + osType);
  }

  return newer;
}

void UpdateManager::checkForUpdatesAsync(
    std::function<void(bool hasUpdate, const UpdateInfo &info)> callback) {
  std::thread([this, callback]() {
    UpdateInfo info;
    bool hasUpdate = checkForUpdatesSync(info);
    if (callback) {
      callback(hasUpdate, info);
    }
  }).detach();
}

// ============================================================================
// DEPENDENCY CHECKING
// ============================================================================

std::vector<DependencyInfo> UpdateManager::getMissingDependencies() {
  std::vector<DependencyInfo> missing;

  std::string appRoot = AppConfig::instance().getAppRoot();
  std::string binDir = appRoot + "/bin";
  std::string libDir = appRoot + "/lib";

  // Create directories if needed
  mkdir(binDir.c_str(), 0755);
  mkdir(libDir.c_str(), 0755);

  // Check mpv binary
  DependencyInfo mpv = {"mpv", binDir + "/mpv", "", true};
  if (access(mpv.path.c_str(), X_OK) != 0) {
    missing.push_back(mpv);
    Logger::info("Dependency missing: mpv at " + mpv.path);
  }

  // Check critical libraries
  const char* libs[] = {
    "libavcodec.so.58",
    "libavformat.so.58",
    "libavutil.so.56",
    "libswscale.so.5",
    "libswresample.so.3"
  };

  for (const char* lib : libs) {
    DependencyInfo dep = {lib, libDir + "/" + lib, "", true};
    if (access(dep.path.c_str(), R_OK) != 0) {
      missing.push_back(dep);
      Logger::info("Dependency missing: " + std::string(lib));
    }
  }

  // Check YouTube backend binary (yt-dlp)
  std::string ytdlPath = binDir + "/yt-dlp";
  std::string ytdlGlibcPath = binDir + "/yt-dlp-glibc";
  if (access(ytdlPath.c_str(), X_OK) != 0 && access(ytdlGlibcPath.c_str(), X_OK) != 0) {
    DependencyInfo ytdl = {"yt-dlp", ytdlPath, "", true};
    missing.push_back(ytdl);
    Logger::info("Dependency missing: yt-dlp at " + ytdlPath);
  }

  // Check YouTube search script
  std::string ytScript = appRoot + "/scripts/youtube_search.sh";
  if (access(ytScript.c_str(), X_OK) != 0) {
    DependencyInfo scriptDep = {"youtube_search.sh", ytScript, "", true};
    missing.push_back(scriptDep);
    Logger::info("Dependency missing: youtube_search.sh at " + ytScript);
  }

  // Check YouTube app icon
  std::string ytIcon = appRoot + "/assets/apps_icons/YOUTUBE.png";
  if (access(ytIcon.c_str(), R_OK) != 0) {
    DependencyInfo iconDep = {"YOUTUBE.png", ytIcon, "", true};
    missing.push_back(iconDep);
    Logger::info("Dependency missing: YOUTUBE.png at " + ytIcon);
  }

  return missing;
}

bool UpdateManager::checkAndInstallDependencies() {
  auto missing = getMissingDependencies();
  if (missing.empty()) {
    Logger::info("All dependencies satisfied.");
    return true;
  }

  Logger::info("Missing " + std::to_string(missing.size()) + " dependencies, will install...");

  std::string appRoot = AppConfig::instance().getAppRoot();
  std::string bundleUrl = "https://github.com/" + std::string(GITHUB_REPO) +
                          "/releases/download/v" + std::string(APP_VERSION) + "/mpv_bundle.zip";

  std::string bundlePath = appRoot + "/mpv_bundle.zip";

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress.state = UpdateState::DOWNLOADING_DEPS;
    m_progress.currentStep = "Downloading media bundle...";
  }

  // Download bundle
  if (!downloadFile(bundleUrl, bundlePath, nullptr, true)) {
    Logger::error("Failed to download media bundle from: " + bundleUrl);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress.state = UpdateState::FAILED;
    m_progress.errorMessage = "Cannot download media bundle";
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress.state = UpdateState::INSTALLING_DEPS;
    m_progress.currentStep = "Installing media bundle...";
  }

  // Install bundle
  if (!installMpvsBundle(bundlePath)) {
    Logger::error("Failed to install media bundle");
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress.state = UpdateState::FAILED;
    m_progress.errorMessage = "Cannot install media bundle";
    return false;
  }

  // Clean up
  unlink(bundlePath.c_str());

  Logger::info("Dependencies installed successfully!");
  return true;
}

// ============================================================================
// DOWNLOAD HELPERS
// ============================================================================

bool UpdateManager::downloadFile(const std::string& url, const std::string& destPath, uint64_t* outSize, bool trackProgress) {
  CURL* curl = curl_easy_init();
  if (!curl) return false;

  FILE* fp = fopen(destPath.c_str(), "wb");
  if (!fp) {
    curl_easy_cleanup(curl);
    return false;
  }

  if (trackProgress) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lastXferTime = 0;
    m_lastXferBytes = 0;
    m_progress.speedKBps = 0.0;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "RomCloud-OTA/2.0");

  if (trackProgress) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
  } else {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
  }

  CURLcode res = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  curl_easy_cleanup(curl);
  fclose(fp);

  if (m_cancelRequested) {
    unlink(destPath.c_str());
    return false;
  }

  if (res != CURLE_OK || httpCode < 200 || httpCode >= 300) {
    unlink(destPath.c_str());
    return false;
  }

  if (outSize) {
    struct stat st;
    if (stat(destPath.c_str(), &st) == 0) {
      *outSize = st.st_size;
    }
  }

  return true;
}

bool UpdateManager::installMpvsBundle(const std::string& zipPath) {
  std::string appRoot = AppConfig::instance().getAppRoot();
  std::string binDir = appRoot + "/bin";
  std::string libDir = appRoot + "/lib";

  // Ensure directories exist
  mkdir(binDir.c_str(), 0755);
  mkdir(libDir.c_str(), 0755);

  // Extract with unzip
  std::string cmd = "cd '" + appRoot + "' && unzip -o '" + zipPath + "' 2>/dev/null";
  int ret = system(cmd.c_str());

  // Also try busybox unzip
  if (ret != 0) {
    cmd = "cd '" + appRoot + "' && busybox unzip -o '" + zipPath + "' 2>/dev/null";
    system(cmd.c_str());
  }

  // Make executable
  std::string mpvPath = binDir + "/mpv";
  chmod(mpvPath.c_str(), 0755);
  std::string ytdlPath = binDir + "/yt-dlp";
  chmod(ytdlPath.c_str(), 0755);
  std::string ytdlGlibcPath = binDir + "/yt-dlp-glibc";
  chmod(ytdlGlibcPath.c_str(), 0755);
  system(("chmod +x '" + appRoot + "/scripts/'*.sh 2>/dev/null").c_str());

  sync();
  return true;
}

bool UpdateManager::installOsBundle(const std::string& zipPath, const std::string& osType) {
  std::string appRoot = AppConfig::instance().getAppRoot();

  // Extract OS-specific bundle
  std::string cmd = "cd '" + appRoot + "' && unzip -o '" + zipPath + "' 2>/dev/null";
  system(cmd.c_str());

  // Apply OS-specific patches if needed
  if (osType == "SpruceOS") {
    // SpruceOS may need special configuration
    Logger::info("Applying SpruceOS patches...");
  } else if (osType == "NextUI") {
    // NextUI specific setup
    Logger::info("Applying NextUI patches...");
  }

  sync();
  return true;
}

// ============================================================================
// PROGRESS TRACKING
// ============================================================================

UpdateProgress UpdateManager::getProgress() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_progress;
}

UpdateInfo UpdateManager::getLatestInfo() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_latestInfo;
}

int UpdateManager::xferCallback(void *clientp, int64_t dltotal, int64_t dlnow,
                                int64_t ultotal, int64_t ulnow) {
  (void)ultotal;
  (void)ulnow;
  auto *self = static_cast<UpdateManager *>(clientp);
  if (!self)
    return 0;
  if (self->m_cancelRequested)
    return 1;

  if (dlnow >= 0) {
    uint32_t now = SDL_GetTicks();
    std::lock_guard<std::mutex> lock(self->m_mutex);
    self->m_progress.bytesDownloaded = static_cast<uint64_t>(dlnow);
    if (dltotal > 0) {
      self->m_progress.totalBytes = static_cast<uint64_t>(dltotal);
      self->m_progress.progressPct =
          (static_cast<double>(dlnow) / static_cast<double>(dltotal)) * 100.0;
    }

    if (self->m_lastXferTime == 0) {
      self->m_lastXferTime = now;
      self->m_lastXferBytes = dlnow;
    } else if (now > self->m_lastXferTime + 300) {
      uint32_t elapsedMs = now - self->m_lastXferTime;
      int64_t bytesDiff = dlnow - self->m_lastXferBytes;
      if (bytesDiff >= 0 && elapsedMs > 0) {
        self->m_progress.speedKBps = (static_cast<double>(bytesDiff) / 1024.0) / (static_cast<double>(elapsedMs) / 1000.0);
      }
      self->m_lastXferTime = now;
      self->m_lastXferBytes = dlnow;
    }
  }
  return 0;
}

// ============================================================================
// UPDATE START
// ============================================================================

bool UpdateManager::startUpdate(const UpdateInfo &info) {
  if (m_isRunning) {
    Logger::warn("An OTA update is already in progress.");
    return false;
  }

  m_cancelRequested = false;
  m_isRunning = true;

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress = UpdateProgress();
    m_progress.state = UpdateState::DOWNLOADING;
    m_progress.newVersion = info.remoteVersion;
    m_progress.currentStep = "Downloading RomCloud v" + info.remoteVersion + "...";
  }

  if (m_workerThread.joinable()) {
    m_workerThread.join();
  }
  m_workerThread = std::thread(&UpdateManager::runDownloadWorker, this, info);
  return true;
}

void UpdateManager::cancelUpdate() {
  m_cancelRequested = true;
  if (m_workerThread.joinable()) {
    m_workerThread.join();
  }
  m_isRunning = false;
}

// ============================================================================
// DOWNLOAD WORKER
// ============================================================================

void UpdateManager::runDownloadWorker(UpdateInfo info) {
  Logger::info("Starting OTA update: v" + info.remoteVersion + " for " + info.osType);

  std::string binDir = AppConfig::instance().getBinDir();
  std::string newBinPath = binDir + "/RomCloud.new";
  std::string finalBinPath = binDir + "/RomCloud";

  // 1. Download main app binary
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress.state = UpdateState::DOWNLOADING;
    m_progress.currentStep = "Đang tải bản cập nhật RomCloud...";
    m_progress.bytesDownloaded = 0;
    m_progress.totalBytes = info.sizeBytes;
    m_progress.progressPct = 0.0;
    m_progress.speedKBps = 0.0;
  }

  uint64_t downloadedSize = 0;
  if (!downloadFile(info.downloadUrl, newBinPath, &downloadedSize, true)) {
    if (m_cancelRequested) {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_progress.state = UpdateState::IDLE;
      m_isRunning = false;
      return;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress.state = UpdateState::FAILED;
    m_progress.errorMessage = "Tải tập tin RomCloud thất bại. Kiểm tra kết nối mạng!";
    Logger::error(m_progress.errorMessage);
    m_isRunning = false;
    return;
  }

  // Verify binary size
  if (downloadedSize < 1000000) {
    unlink(newBinPath.c_str());
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress.state = UpdateState::FAILED;
    m_progress.errorMessage = "Tập tin tải về quá nhỏ hoặc không hợp lệ.";
    m_isRunning = false;
    return;
  }

  chmod(newBinPath.c_str(), 0755);
  sync();

  // 2. Replace binary
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress.state = UpdateState::INSTALLING;
    m_progress.currentStep = "Đang cài đặt và thay thế file thực thi RomCloud...";
    m_progress.progressPct = 100.0;
    m_progress.speedKBps = 0.0;
  }

  bool replaced = false;
  std::string oldBinPath = binDir + "/RomCloud.old";
  unlink(oldBinPath.c_str());

  if (rename(finalBinPath.c_str(), oldBinPath.c_str()) == 0) {
    if (rename(newBinPath.c_str(), finalBinPath.c_str()) == 0) {
      chmod(finalBinPath.c_str(), 0755);
      unlink(oldBinPath.c_str());
      replaced = true;
      Logger::info("Binary replaced successfully");
    } else {
      rename(oldBinPath.c_str(), finalBinPath.c_str());
    }
  }

  if (!replaced) {
    // FAT32 workaround - create install script
    std::string scriptPath = binDir + "/ota_install.sh";
    FILE* script = fopen(scriptPath.c_str(), "w");
    if (script) {
      fprintf(script, "#!/bin/sh\n");
      fprintf(script, "mv -f '%s' '%s' 2>/dev/null; ", newBinPath.c_str(), finalBinPath.c_str());
      fprintf(script, "chmod +x '%s'; ", finalBinPath.c_str());
      fprintf(script, "rm -f '%s'\n", scriptPath.c_str());
      fclose(script);
      chmod(scriptPath.c_str(), 0755);
    }
    Logger::info("Binary replacement deferred to next boot");
  }

  sync();

  // 3. Download and update official app icon
  if (!info.iconUrl.empty()) {
    std::string appRoot = AppConfig::instance().getAppRoot();
    std::string newIconPath = appRoot + "/icon.png.new";
    std::string finalIconPath = appRoot + "/icon.png";
    std::string appIconPath = appRoot + "/assets/apps_icons/APP.png";
    std::string assetsIconPath = appRoot + "/assets/icon.png";

    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_progress.state = UpdateState::INSTALLING;
      m_progress.currentStep = "Đang cập nhật biểu tượng ứng dụng...";
    }

    uint64_t iconSize = 0;
    if (downloadFile(info.iconUrl, newIconPath, &iconSize, false) && iconSize > 1000) {
      chmod(newIconPath.c_str(), 0644);
      unlink(finalIconPath.c_str());
      if (rename(newIconPath.c_str(), finalIconPath.c_str()) == 0) {
        std::ifstream src(finalIconPath, std::ios::binary);
        if (src) {
          std::ofstream dst1(appIconPath, std::ios::binary | std::ios::trunc);
          if (dst1) dst1 << src.rdbuf();
          src.clear();
          src.seekg(0, std::ios::beg);
          std::ofstream dst2(assetsIconPath, std::ios::binary | std::ios::trunc);
          if (dst2) dst2 << src.rdbuf();
        }
        Logger::info("App icon updated successfully via OTA (" + std::to_string(iconSize) + " bytes)");
      } else {
        // FAT32 deferred: keep newIconPath so launch.sh can copy it
        Logger::info("Icon rename deferred to launch.sh");
      }
    } else {
      unlink(newIconPath.c_str());
      Logger::warn("Failed to download or verify app icon");
    }
  }

  // 4. Check and install dependencies (mpv, codecs)
  if (!downloadAndInstallDependencies(info)) {
    Logger::warn("Some dependencies may be missing - app may not work fully");
  }

  // 5. Check and install OS-specific bundle if available
  if (!info.osBundleUrl.empty()) {
    std::string appRoot = AppConfig::instance().getAppRoot();
    std::string osBundlePath = appRoot + "/os_bundle.zip";

    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_progress.state = UpdateState::DOWNLOADING_DEPS;
      m_progress.currentStep = "Đang tải gói cấu hình " + info.osType + "...";
      m_progress.bytesDownloaded = 0;
      m_progress.totalBytes = 0;
      m_progress.progressPct = 0.0;
      m_progress.speedKBps = 0.0;
    }

    if (downloadFile(info.osBundleUrl, osBundlePath, nullptr, true)) {
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_progress.state = UpdateState::INSTALLING_DEPS;
        m_progress.currentStep = "Đang cài đặt gói tối ưu " + info.osType + "...";
        m_progress.progressPct = 100.0;
        m_progress.speedKBps = 0.0;
      }
      installOsBundle(osBundlePath, info.osType);
      unlink(osBundlePath.c_str());
    }
  }

  Logger::info("OTA update completed!");

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress.state = UpdateState::COMPLETED;
    m_progress.progressPct = 100.0;
    m_progress.currentStep = "Cập nhật thành công! Vui lòng khởi động lại.";
  }

  m_isRunning = false;
}

bool UpdateManager::downloadAndInstallDependencies(const UpdateInfo& info) {
  auto missing = getMissingDependencies();
  if (missing.empty()) {
    return true;
  }

  Logger::info("Installing " + std::to_string(missing.size()) + " missing dependencies...");

  std::string appRoot = AppConfig::instance().getAppRoot();
  std::string bundleUrl = info.bundleUrl;
  if (bundleUrl.empty()) {
    bundleUrl = "https://github.com/" + std::string(GITHUB_REPO) +
                "/releases/download/v" + info.remoteVersion + "/mpv_bundle.zip";
  }

  std::string bundlePath = appRoot + "/mpv_bundle.zip";

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress.state = UpdateState::DOWNLOADING_DEPS;
    m_progress.currentStep = "Đang tải gói hỗ trợ phát video (mpv, codecs)...";
    m_progress.bytesDownloaded = 0;
    m_progress.totalBytes = 0;
    m_progress.progressPct = 0.0;
    m_progress.speedKBps = 0.0;
  }

  if (!downloadFile(bundleUrl, bundlePath, nullptr, true)) {
    Logger::warn("Failed to download media bundle from " + bundleUrl + ", trying fallback v2.1.0...");
    std::string fallbackUrl = "https://github.com/" + std::string(GITHUB_REPO) +
                              "/releases/download/v2.1.0/mpv_bundle.zip";
    if (!downloadFile(fallbackUrl, bundlePath, nullptr, true)) {
      Logger::error("Failed to download media bundle from all sources");
      return false;
    }
  }

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_progress.state = UpdateState::INSTALLING_DEPS;
    m_progress.currentStep = "Đang giải nén và thiết lập trình phát video...";
    m_progress.progressPct = 100.0;
    m_progress.speedKBps = 0.0;
  }

  bool success = installMpvsBundle(bundlePath);
  unlink(bundlePath.c_str());

  return success;
}

} // namespace RomCloud
