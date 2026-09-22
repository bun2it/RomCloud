#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace RomCloud {

struct HttpResponse {
    int statusCode = 0;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    std::string error;
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

private:
    HttpClient() = default;
    ~HttpClient();

    bool m_initialized = false;
};

} // namespace RomCloud
