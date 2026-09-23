#include "IPTVManager.h"
#include "../logging/Logger.h"
#include "../filesystem/FileSystemManager.h"
#include "../config/AppConfig.h"
#include "../input/InputManager.h"
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>

#include <SDL2/SDL.h>

namespace RomCloud {

IPTVManager& IPTVManager::instance() {
    static IPTVManager instance;
    return instance;
}

IPTVManager::IPTVManager() {
    std::string appDir = AppConfig::instance().getAppRoot();
    if (appDir.empty()) {
        appDir = "/mnt/SDCARD/Apps/RomCloud";
    }
    m_iptvDir = appDir + "/iptv";

    // Ensure directory exists
    FileSystemManager::instance().createDirectoryRecursive(m_iptvDir);

    Logger::info("IPTVManager: Initializing from " + m_iptvDir);
    loadPlaylists(m_iptvDir);

    // If no playlist exists, create default playlist
    if (m_channels.empty()) {
        Logger::info("No playlists found, creating default playlist...");
        createDefaultPlaylist(m_iptvDir + "/default.m3u");
        loadPlaylists(m_iptvDir);
    }
}

IPTVManager::~IPTVManager() {
    stop();
}

bool IPTVManager::loadPlaylists(const std::string& directory) {
    std::string dirPath = directory.empty() ? m_iptvDir : directory;
    Logger::info("Loading IPTV playlists from: " + dirPath);

    m_channels.clear();
    m_groups.clear();
    loadFavorites();

    if (!FileSystemManager::instance().directoryExists(dirPath)) {
        Logger::warn("IPTV directory not found: " + dirPath);
        return false;
    }

    DIR* dir = opendir(dirPath.c_str());
    if (!dir) {
        return false;
    }

    int loaded = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string filename = entry->d_name;
        if (filename.length() >= 4) {
            std::string ext = "";
            size_t dot = filename.rfind('.');
            if (dot != std::string::npos) {
                ext = filename.substr(dot);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            }
            if (ext == ".m3u" || ext == ".m3u8") {
                std::string path = dirPath + "/" + filename;
                if (parseM3UFile(path)) {
                    loaded++;
                }
            }
        }
    }
    closedir(dir);

    Logger::info("Loaded " + std::to_string(m_channels.size()) + " IPTV channels from " +
                 std::to_string(loaded) + " playlist(s)");

    return m_channels.size() > 0;
}

bool IPTVManager::parseM3UFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        Logger::error("Cannot open M3U file: " + filepath);
        return false;
    }

    std::string line;
    std::string currentGroup = "Việt Nam";
    std::string currentName;
    std::string currentLogo;

    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) continue;

        if (line.rfind("#EXTINF:", 0) == 0) {
            currentName = extractName(line);
            std::string group = extractGroup(line);
            if (!group.empty()) {
                currentGroup = group;
            }
            currentLogo = extractLogo(line);

            if (currentName.empty()) {
                currentName = "Kênh không tên";
            }
            continue;
        }

        if (line[0] == '#') {
            continue;
        }

        std::string url = line;
        if (!url.empty() && url.find("://") != std::string::npos) {
            IPTVChannel channel;
            channel.name = currentName;
            channel.url = url;
            channel.group = currentGroup;
            channel.logo = currentLogo;
            channel.isFavorite = isFavorite(currentName);

            m_channels.push_back(channel);

            size_t idx = m_channels.size() - 1;
            m_groups[currentGroup].push_back(idx);
        }
    }

    file.close();
    return true;
}

std::string IPTVManager::extractGroup(const std::string& line) {
    size_t pos = line.find("group-title=\"");
    if (pos != std::string::npos) {
        pos += 13;
        size_t end = line.find("\"", pos);
        if (end != std::string::npos) {
            return line.substr(pos, end - pos);
        }
    }
    return "";
}

std::string IPTVManager::extractName(const std::string& line) {
    size_t commaPos = line.rfind(',');
    if (commaPos != std::string::npos && commaPos < line.length() - 1) {
        return line.substr(commaPos + 1);
    }
    return "";
}

std::string IPTVManager::extractLogo(const std::string& line) {
    size_t pos = line.find("tvg-logo=\"");
    if (pos != std::string::npos) {
        pos += 10;
        size_t end = line.find("\"", pos);
        if (end != std::string::npos) {
            return line.substr(pos, end - pos);
        }
    }
    return "";
}

bool IPTVManager::isFavorite(const std::string& channelName) const {
    return m_favorites.find(channelName) != m_favorites.end();
}

void IPTVManager::toggleFavorite(const std::string& channelName) {
    if (channelName.empty()) return;
    auto it = m_favorites.find(channelName);
    bool nowFav = false;
    if (it != m_favorites.end()) {
        m_favorites.erase(it);
        nowFav = false;
    } else {
        m_favorites.insert(channelName);
        nowFav = true;
    }
    // Update existing channels in memory
    for (auto& chan : m_channels) {
        if (chan.name == channelName) {
            chan.isFavorite = nowFav;
        }
    }
    saveFavorites();
    Logger::info("Toggled favorite for '" + channelName + "': " + (nowFav ? "ADDED" : "REMOVED"));
}

void IPTVManager::loadFavorites() {
    m_favorites.clear();
    std::string favPath = m_iptvDir + "/favorites.txt";
    std::ifstream file(favPath);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] != '#') {
            m_favorites.insert(line);
        }
    }
    file.close();
    Logger::info("Loaded " + std::to_string(m_favorites.size()) + " favorite channels");
}

void IPTVManager::saveFavorites() {
    std::string favPath = m_iptvDir + "/favorites.txt";
    std::ofstream file(favPath);
    if (!file.is_open()) {
        Logger::error("Cannot open favorites file for writing: " + favPath);
        return;
    }
    for (const auto& fav : m_favorites) {
        file << fav << "\n";
    }
    file.close();
}

std::vector<IPTVChannel> IPTVManager::getFavoriteChannels() const {
    std::vector<IPTVChannel> result;
    for (const auto& chan : m_channels) {
        if (chan.isFavorite) {
            result.push_back(chan);
        }
    }
    return result;
}

std::vector<IPTVChannel> IPTVManager::getChannelsByGroup(const std::string& group) const {
    std::vector<IPTVChannel> result;
    auto it = m_groups.find(group);
    if (it != m_groups.end()) {
        for (size_t idx : it->second) {
            if (idx < m_channels.size()) {
                result.push_back(m_channels[idx]);
            }
        }
    }
    return result;
}

std::vector<std::string> IPTVManager::getGroups() const {
    std::vector<std::string> groups;
    for (const auto& pair : m_groups) {
        groups.push_back(pair.first);
    }
    std::sort(groups.begin(), groups.end());
    return groups;
}

std::vector<IPTVChannel> IPTVManager::search(const std::string& query) const {
    std::vector<IPTVChannel> result;
    std::string lowerQuery = query;
    std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);

    for (const auto& channel : m_channels) {
        std::string lowerName = channel.name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

        std::string lowerGroup = channel.group;
        std::transform(lowerGroup.begin(), lowerGroup.end(), lowerGroup.begin(), ::tolower);

        if (lowerName.find(lowerQuery) != std::string::npos || lowerGroup.find(lowerQuery) != std::string::npos) {
            result.push_back(channel);
        }
    }
    return result;
}

IPTVChannel* IPTVManager::getChannel(size_t index) {
    if (index < m_channels.size()) {
        return &m_channels[index];
    }
    return nullptr;
}

bool IPTVManager::playChannel(const IPTVChannel& channel) {
    stop();

    if (channel.url.empty()) {
        Logger::error("IPTV channel URL is empty");
        return false;
    }

    Logger::info("Playing IPTV channel: " + channel.name);
    Logger::info("URL: " + channel.url);

    std::string appRoot = AppConfig::instance().getAppRoot();
    if (appRoot.empty()) appRoot = "/mnt/SDCARD/Apps/RomCloud";

    std::vector<std::string> playerCandidates = {
        appRoot + "/bin/mpv",
        "/mnt/SDCARD/System/bin/mpv",
        "/mnt/SDCARD/Emus/VIDEOS/mpv.sh",
        "/usr/trimui/bin/mpv",
        "/usr/bin/mpv",
        appRoot + "/bin/ffplay",
        "/mnt/SDCARD/System/bin/ffplay",
        "/usr/bin/ffplay",
        "mpv",
        "ffplay"
    };

    std::string playerPath;
    for (const auto& candidate : playerCandidates) {
        if (candidate.front() == '/') {
            struct stat st;
            if (stat(candidate.c_str(), &st) == 0 && st.st_size > 1000 && access(candidate.c_str(), X_OK) == 0) {
                playerPath = candidate;
                break;
            }
        } else {
            playerPath = candidate;
            break;
        }
    }

    if (playerPath.empty()) {
        Logger::error("No valid media player (mpv/ffplay) found on system");
        return false;
    }

    Logger::info("Selected IPTV player: " + playerPath);

    // Prevent TrimUI screen standby while watching video
    FILE* fwake = fopen("/tmp/stay_awake", "w");
    if (fwake) {
        fputs("1\n", fwake);
        fclose(fwake);
    }

    pid_t pid = fork();
    if (pid == 0) {
        // Child process: create independent process group
        setpgid(0, 0);

        // Redirect stdout & stderr to iptv_mpv.log for diagnostics
        std::string logPath = appRoot + "/iptv_mpv.log";
        int logFd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logFd >= 0) {
            dup2(logFd, STDOUT_FILENO);
            dup2(logFd, STDERR_FILENO);
            close(logFd);
        }

        // Export library paths so mpv can find codec libraries and ALSA/DRM
        std::string libPath = appRoot + "/lib:/mnt/SDCARD/System/lib:/usr/lib:/lib";
        setenv("LD_LIBRARY_PATH", libPath.c_str(), 1);
        setenv("HOME", appRoot.c_str(), 1);

        std::string inputConf = appRoot + "/config/input.conf";

        if (playerPath.find("mpv.sh") != std::string::npos) {
            execl("/bin/sh", "sh", playerPath.c_str(), channel.url.c_str(), nullptr);
        } else if (playerPath.find("mpv") != std::string::npos) {
            std::vector<std::string> argList = {
                playerPath,
                channel.url,
                "--fullscreen",
                "--keepaspect=yes",
                "--hwdec=auto",
                "--vd-lavc-threads=4",
                "--framedrop=vo",
                "--demuxer-max-bytes=16M",
                "--demuxer-readahead-secs=5",
                "--audio-buffer=0.5",
                "--terminal=no"
            };
            if (access(inputConf.c_str(), R_OK) == 0) {
                argList.push_back("--input-conf=" + inputConf);
            }
            std::vector<char*> cArgs;
            for (auto& s : argList) {
                cArgs.push_back(const_cast<char*>(s.c_str()));
            }
            cArgs.push_back(nullptr);

            execv(playerPath.c_str(), cArgs.data());
        } else {
            // ffplay
            const char* args[] = {
                playerPath.c_str(),
                "-fs",
                "-autoexit",
                "-loglevel", "warning",
                channel.url.c_str(),
                nullptr
            };
            execvp(playerPath.c_str(), const_cast<char* const*>(args));
        }
        _exit(1);
    } else if (pid > 0) {
        m_mpvPid = pid;
        m_isPlaying = true;
        m_currentChannel = channel.name;
        Logger::info("IPTV player started with PID: " + std::to_string(pid) + " using " + playerPath);

        // Responsive loop: wait for player to exit OR user to press B/Menu/Select
        int status = 0;
        while (m_isPlaying && m_mpvPid > 0) {
            pid_t res = waitpid(m_mpvPid, &status, WNOHANG);
            if (res != 0) {
                break;
            }

            SDL_Event ev;
            bool shouldExit = false;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                    // TrimUI physical B button = SDL_CONTROLLER_BUTTON_A (or Menu / Back)
                    if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_A ||
                        ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE ||
                        ev.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
                        Logger::info("User pressed B/Menu button, stopping IPTV player");
                        shouldExit = true;
                        break;
                    }
                } else if (ev.type == SDL_KEYDOWN) {
                    if (ev.key.keysym.sym == SDLK_b || ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_q) {
                        Logger::info("User pressed key B/ESC/Q, stopping IPTV player");
                        shouldExit = true;
                        break;
                    }
                }
            }

            if (shouldExit) {
                stop();
                break;
            }

            SDL_Delay(40);
        }

        // Clean up stay_awake
        unlink("/tmp/stay_awake");

        if (WIFEXITED(status)) {
            int exitCode = WEXITSTATUS(status);
            Logger::info("IPTV player exited with code: " + std::to_string(exitCode));
            if (exitCode != 0) {
                // Read last lines of iptv_mpv.log for diagnostics
                std::ifstream logFile(appRoot + "/iptv_mpv.log");
                if (logFile.is_open()) {
                    std::string errLine;
                    int count = 0;
                    while (std::getline(logFile, errLine) && count++ < 10) {
                        if (!errLine.empty()) {
                            Logger::warn("mpv: " + errLine);
                        }
                    }
                }
            }
        } else if (WIFSIGNALED(status)) {
            Logger::info("IPTV player terminated by signal: " + std::to_string(WTERMSIG(status)));
        }

        m_mpvPid = -1;
        m_isPlaying = false;
        m_currentChannel = "";
        Logger::info("IPTV player finished, returning to RomCloud UI");

        // Flush any button events queued while player was active and reset controller state
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
        InputManager::instance().reset();

        return true;
    }

    unlink("/tmp/stay_awake");
    Logger::error("Failed to fork IPTV player");
    return false;
}

bool IPTVManager::stop() {
    unlink("/tmp/stay_awake");
    if (m_mpvPid > 0) {
        Logger::info("Stopping IPTV player (PID: " + std::to_string(m_mpvPid) + ")");
        // Send SIGTERM to process group and direct PID
        kill(-m_mpvPid, SIGTERM);
        kill(m_mpvPid, SIGTERM);

        int status = 0;
        for (int i = 0; i < 5; i++) {
            pid_t res = waitpid(m_mpvPid, &status, WNOHANG);
            if (res != 0) break;
            usleep(20000); // 20ms
        }

        // If process is still active, force kill
        if (kill(m_mpvPid, 0) == 0) {
            kill(-m_mpvPid, SIGKILL);
            kill(m_mpvPid, SIGKILL);
            waitpid(m_mpvPid, &status, WNOHANG);
        }
        m_mpvPid = -1;
    }

    m_isPlaying = false;
    m_currentChannel = "";
    return true;
}

void IPTVManager::createDefaultPlaylist(const std::string& filepath) {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        Logger::error("Cannot create default playlist: " + filepath);
        return;
    }

    file << "#EXTM3U\n";
    file << "#EXTINF:-1 tvg-id=\"test1\" tvg-name=\"Big Buck Bunny\" group-title=\"Test\",Big Buck Bunny\n";
    file << "https://test-streams.mux.dev/outcasts/index.m3u8\n";
    file << "#EXTINF:-1 tvg-id=\"test2\" tvg-name=\"Sintel\" group-title=\"Test\",Sintel Trailer\n";
    file << "https://bitdash-a.akamaihd.net/content/sintel/hls/playlist.m3u8\n";
    file << "#EXTINF:-1 tvg-id=\"test3\" tvg-name=\"Tears of Steel\" group-title=\"Test\",Tears of Steel\n";
    file << "https://bitdash-a.akamaihd.net/content/sintel/hls/playlist.m3u8\n";

    file.close();
    Logger::info("Created default playlist: " + filepath);
}

} // namespace RomCloud
