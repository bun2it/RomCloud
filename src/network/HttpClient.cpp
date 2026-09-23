#include "HttpClient.h"
#include "../logging/Logger.h"
#include <curl/curl.h>
#include <sstream>
#include <unistd.h>

namespace RomCloud {

static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::string* str = static_cast<std::string*>(userp);
    if (str) {
        str->append(static_cast<char*>(contents), totalSize);
    }
    return totalSize;
}

static size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t totalSize = size * nitems;
    auto* headers = static_cast<std::unordered_map<std::string, std::string>*>(userdata);
    if (headers) {
        std::string line(buffer, totalSize);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(0, 1);
            while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' ')) val.pop_back();
            (*headers)[key] = val;
        }
    }
    return totalSize;
}

HttpClient& HttpClient::instance() {
    static HttpClient instance;
    return instance;
}

HttpClient::~HttpClient() {
    shutdown();
}

bool HttpClient::init() {
    if (m_initialized) return true;
    CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (res != CURLE_OK) {
        Logger::error(std::string("curl_global_init failed: ") + curl_easy_strerror(res));
        return false;
    }
    m_initialized = true;
    Logger::info("HttpClient initialized with libcurl.");
    return true;
}

void HttpClient::shutdown() {
    if (m_initialized) {
        curl_global_cleanup();
        m_initialized = false;
    }
}

std::string HttpClient::urlEncode(const std::string& value) {
    if (!m_initialized) init();
    CURL* curl = curl_easy_init();
    if (!curl) return value;

    char* output = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.length()));
    std::string result = output ? output : "";
    if (output) curl_free(output);
    curl_easy_cleanup(curl);
    return result;
}

HttpResponse HttpClient::get(const std::string& url, const std::vector<std::string>& headers, int timeoutSec) {
    if (!m_initialized) init();

    HttpResponse response;
    CURL* curl = curl_easy_init();
    if (!curl) {
        response.error = "Failed to initialize CURL handle";
        return response;
    }

    struct curl_slist* chunk = nullptr;
    for (const auto& h : headers) {
        chunk = curl_slist_append(chunk, h.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (access("/etc/ssl/certs/ca-certificates.crt", F_OK) == 0) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "RomCloud-TrimUI-BrickPro/1.0");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        response.statusCode = static_cast<int>(httpCode);
        response.success = (response.statusCode >= 200 && response.statusCode < 300);
    } else {
        response.error = curl_easy_strerror(res);
        response.success = false;
        Logger::error("HTTP GET failed (" + url + "): " + response.error);
    }

    if (chunk) curl_slist_free_all(chunk);
    curl_easy_cleanup(curl);
    return response;
}

HttpResponse HttpClient::post(const std::string& url, const std::string& postData, const std::vector<std::string>& headers, int timeoutSec) {
    if (!m_initialized) init();

    HttpResponse response;
    CURL* curl = curl_easy_init();
    if (!curl) {
        response.error = "Failed to initialize CURL handle";
        return response;
    }

    struct curl_slist* chunk = nullptr;
    for (const auto& h : headers) {
        chunk = curl_slist_append(chunk, h.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, postData.length());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (access("/etc/ssl/certs/ca-certificates.crt", F_OK) == 0) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "RomCloud-TrimUI-BrickPro/1.0");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        response.statusCode = static_cast<int>(httpCode);
        response.success = (response.statusCode >= 200 && response.statusCode < 300);
    } else {
        response.error = curl_easy_strerror(res);
        response.success = false;
        Logger::error("HTTP POST failed (" + url + "): " + response.error);
    }

    if (chunk) curl_slist_free_all(chunk);
    curl_easy_cleanup(curl);
    return response;
}

HttpResponse HttpClient::postForm(const std::string& url, const std::unordered_map<std::string, std::string>& formData, const std::vector<std::string>& headers, int timeoutSec) {
    std::stringstream ss;
    bool first = true;
    for (const auto& pair : formData) {
        if (!first) ss << "&";
        first = false;
        ss << urlEncode(pair.first) << "=" << urlEncode(pair.second);
    }

    std::vector<std::string> reqHeaders = headers;
    reqHeaders.push_back("Content-Type: application/x-www-form-urlencoded");

    return post(url, ss.str(), reqHeaders, timeoutSec);
}

} // namespace RomCloud
