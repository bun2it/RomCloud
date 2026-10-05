#include "CameraManager.h"
#include "../config/AppConfig.h"
#include "../logging/Logger.h"

#include <fstream>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace RomCloud {

CameraManager& CameraManager::instance() {
    static CameraManager inst;
    return inst;
}

bool CameraManager::load() {
    if (!m_cams.empty()) return true;
    std::string path = AppConfig::instance().getAssetsDir() + "/traffic_cams.json";
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        Logger::warn("CameraManager: missing " + path);
        return false;
    }
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    // Format gọn: [{"n":"...","c":"...","u":"..."},...]
    size_t pos = 0;
    auto getField = [&](size_t from, size_t endPos, const char* key, std::string& out) -> size_t {
        std::string pat = std::string("\"") + key + "\"";
        size_t p = body.find(pat, from);
        if (p == std::string::npos || p >= endPos) return std::string::npos;
        size_t c0 = body.find(':', p);
        size_t q0 = body.find('"', c0);
        if (q0 == std::string::npos || q0 > endPos) return std::string::npos;
        // Bỏ qua \" escaped: tìm quote đóng thật (đếm backslash)
        size_t q = q0 + 1;
        std::string val;
        while (q < body.size()) {
            char ch = body[q];
            if (ch == '\\' && q + 1 < body.size()) {
                char nx = body[q + 1];
                if (nx == '"') val += '"';
                else if (nx == 'n') val += ' ';
                else if (nx == 'u') {
                    // \uXXXX: bỏ qua (tên camera ít dùng)
                    q += 6;
                    continue;
                } else { val += ch; val += nx; }
                q += 2;
            } else if (ch == '"') {
                break;
            } else {
                val += ch;
                q++;
            }
        }
        out = val;
        return q;
    };
    while ((pos = body.find('{', pos)) != std::string::npos) {
        size_t end = body.find('}', pos);
        if (end == std::string::npos) break;
        TrafficCam cam;
        std::string n, c, u, ytv, tmp;
        size_t q = pos;
        q = getField(q, end, "n", n);
        if (q == std::string::npos) { pos = end + 1; continue; }
        getField(q, end, "c", c);
        getField(q, end, "u", u);
        getField(q, end, "yt", ytv);
        if (!ytv.empty()) {
            cam.name = n;
            cam.code = c;
            cam.kind = 1;
            cam.vid = ytv;
            m_cams.push_back(std::move(cam));
        } else if (!u.empty()) {
            cam.name = n;
            cam.code = c;
            cam.url = u;
            m_cams.push_back(std::move(cam));
        }
        pos = end + 1;
    }
    Logger::info("CameraManager: loaded " + std::to_string(m_cams.size()) + " cameras");
    return !m_cams.empty();
}

std::string CameraManager::favPath() const {
    std::string d = AppConfig::instance().getDataDir();
    if (d.empty()) d = "/mnt/SDCARD/Apps/RomCloud/data";
    return d + "/cam_fav.txt";
}

void CameraManager::loadFavs() {
    if (m_favsLoaded) return;
    m_favsLoaded = true;
    std::ifstream f(favPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) m_favs.insert(line);
    }
}

void CameraManager::saveFavs() {
    std::ofstream f(favPath(), std::ios::trunc);
    if (!f) return;
    for (const auto& k : m_favs) f << k << "\n";
}

std::string CameraManager::snapPath(size_t i) {
    char buf[64];
    snprintf(buf, sizeof(buf), "/tmp/traffic_cam_%zu.jpg", i);
    return buf;
}

std::string CameraManager::favKey(size_t i) {
    if (i >= m_cams.size()) return "";
    const auto& c = m_cams[i];
    // Ổn định qua các lần update data: ưu tiên mã, rỗng thì tên
    return c.code.empty() ? c.name : (c.code + " " + c.name);
}

bool CameraManager::isFavorite(const std::string& key) const {
    if (key.empty()) return false;
    const_cast<CameraManager*>(this)->loadFavs();
    return m_favs.count(key) > 0;
}

void CameraManager::toggleFavorite(const std::string& key) {
    if (key.empty()) return;
    loadFavs();
    if (m_favs.count(key)) m_favs.erase(key);
    else m_favs.insert(key);
    saveFavs();
}

std::string CameraManager::fetch(size_t i) {
    if (i >= m_cams.size()) return "";
    const TrafficCam& cam = m_cams[i];
    std::string url;
    if (cam.kind == 1) {
        // YouTube live: lấy thumbnail 1 lần (không refresh liên tục)
        url = "https://i.ytimg.com/vi/" + cam.vid + "/hqdefault.jpg";
    } else {
        url = cam.url;
        url += (url.find('?') == std::string::npos ? "?" : "&");
        url += "t=" + std::to_string((long long)std::time(nullptr) * 1000);
    }
    std::string out = snapPath(i);
    if (cam.kind == 1) {
        struct stat st;
        if (stat(out.c_str(), &st) == 0 && st.st_size > 2000) return out;
    }
    std::string tmp = out + ".tmp";
    std::string cmd = "curl -4 -k -s -L -A 'Mozilla/5.0' --connect-timeout 5 --max-time 12 -o \"" +
                      tmp + "\" \"" + url + "\" >/dev/null 2>&1";
    int rc = system(cmd.c_str());
    struct stat st;
    if (rc != 0 || stat(tmp.c_str(), &st) != 0 || st.st_size < 2000) {
        unlink(tmp.c_str());
        return "";
    }
    rename(tmp.c_str(), out.c_str());
    return out;
}

} // namespace RomCloud
