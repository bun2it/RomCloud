#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <mutex>
#include <curl/curl.h>

namespace RomCloud {

struct HttpResponse {
    int statusCode = 0;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    std::string error;
    std::string effectiveUrl;
    bool success = false;
};

class HttpClient {
public:
    static HttpClient& instance();
    bool init();
    void shutdown();

    HttpResponse get(const std::string& url, const std::vector<std::string>& headers = {}, int timeoutSec = 15);
    HttpResponse post(const std::string& url, const std::string& postData, const std::vector<std::string>& headers = {}, int timeoutSec = 15);
    HttpResponse postForm(const std::string& url, const std::unordered_map<std::string, std::string>& formData, const std::vector<std::string>& headers = {}, int timeoutSec = 15);

    std::string urlEncode(const std::string& value);

    // P4 (vnexpress plan): cookie jar shared by all handles (portal logins
    // are multi-step and stateless handles would drop session cookies).
    // Backed by a file so cookies survive across easy handles.
    // P23: path is configurable so cookies survive across reboots (defaults
    // to /tmp/romcloud_cookies.txt for backwards-compat with LITE tests).
    void clearCookies();
    void setCookieJarPath(const std::string& path);
    const std::string& cookieJarPath() const { return m_cookieJarPath; }

private:
    HttpClient() = default;
    ~HttpClient();

    bool m_initialized = false;
    std::string m_cookieJarPath = "/tmp/romcloud_cookies.txt";
    // Tái dùng 1 easy handle cho mọi GET (giữ keep-alive + DNS cache +
    // TLS session): scrape hàng chục request liên tiếp nhanh gấp nhiều lần
    // so với init/cleanup mỗi lần. Có mutex vì GET gọi từ nhiều thread.
    // POST giữ handle riêng (tránh rò CURLOPT_POST/POSTFIELDS).
    CURL* m_getCurl = nullptr;
    std::mutex m_getMutex;
};

} // namespace RomCloud
