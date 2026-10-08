#include "HttpClient.h"
#include "../logging/Logger.h"
#include <curl/curl.h>
#include <sstream>
#include <unistd.h>

namespace RomCloud {

// P4: cookie jar path (tmpfs on Brick — session cookies, gone on reboot).
static const char* kCookieJarPath = "/tmp/romcloud_cookies.txt";

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

void HttpClient::clearCookies() {
    unlink(kCookieJarPath);
}

// Apply the options shared by GET and POST handles.
static void applyCommonOptions(CURL* curl, struct curl_slist* chunk, int timeoutSec) {
    (void)chunk;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 6);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    // P4: cookie engine backed by jar file — every handle in the app shares
    // the portal session (each request builds a fresh easy handle).
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, kCookieJarPath);
    curl_easy_setopt(curl, CURLOPT_COOKIEJAR, kCookieJarPath);
    // P4: ask for gzip/deflate/br — vnexpress serves gzip, ~5x smaller.
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
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

    // Preflight: emit one log line with hostname + DNS state so a Brick user
    // can see whether the failure is DNS, network, or HTTP from /var/log.
    {
        std::string host;
        size_t schemeEnd = url.find("://");
        if (schemeEnd != std::string::npos) {
            size_t hostStart = schemeEnd + 3;
            size_t hostEnd = url.find_first_of("/?#:", hostStart);
            host = url.substr(hostStart,
                              hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart);
        }
        bool resolvOk = access("/etc/resolv.conf", F_OK) == 0;
        Logger::info(std::string("HttpClient: GET ") + host
                     + " (resolv.conf=" + (resolvOk ? "ok" : "MISSING") + ")");
    }

    struct curl_slist* chunk = nullptr;
    chunk = curl_slist_append(chunk, "Sec-CH-UA-Mobile: ?1");
    chunk = curl_slist_append(chunk, "Sec-CH-UA-Platform: \"Android\"");
    // Do NOT advertise image/webp: TrimUI Brick's SDL2_image does not have libwebp.
    // Advertising webp causes CDNs (VnExpress, etc.) to transcode JPEG/PNG to WebP,
    // which then fails to decode on the device.
    bool hasAccept = false;
    for (const auto& h : headers) {
        if (h.rfind("Accept:", 0) == 0 || h.rfind("accept:", 0) == 0) {
            hasAccept = true;
            break;
        }
    }
    if (!hasAccept) {
        chunk = curl_slist_append(chunk, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/jpeg,image/png,image/*;q=0.8,*/*;q=0.7");
    }
    chunk = curl_slist_append(chunk, "Accept-Language: vi-VN,vi;q=0.9,en-US;q=0.8,en;q=0.7");
    for (const auto& h : headers) {
        chunk = curl_slist_append(chunk, h.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
    applyCommonOptions(curl, chunk, timeoutSec);
    // Verbose stderr logging is off by default — set ROMCLOUD_CURL_VERBOSE=1 in
    // env to see DNS/TLS/handshake chatter on the device console.
    if (getenv("ROMCLOUD_CURL_VERBOSE")) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    }
    if (access("/etc/ssl/certs/ca-certificates.crt", F_OK) == 0) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Linux; Android 13; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.6099.230 Mobile Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // Cap response size at 10MB to avoid OOM while allowing full news / modern pages.
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(10 * 1024 * 1024));
    Logger::info("HTTP GET start: " + url + " (timeout=" + std::to_string(timeoutSec) + "s)");

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        response.statusCode = static_cast<int>(httpCode);
        response.success = (response.statusCode >= 200 && response.statusCode < 300);
        char* effUrl = nullptr;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effUrl);
        if (effUrl) response.effectiveUrl = effUrl;
        if (!response.success) {
            Logger::warn("HTTP GET " + std::to_string(response.statusCode) +
                         " (" + url + ")");
        }
    } else {
        response.error = curl_easy_strerror(res);
        response.success = false;
        // Log the numeric CURLcode too — `curl_easy_strerror()` returns
        // just "Error" for some codes (CURLE_FAILED_INIT = 2) which makes
        // "did the device run out of sockets?" indistinguishable from
        // "did the URL have a typo?". The integer code survives any
        // sanitization and lets grep find it deterministically.
        Logger::error("HTTP GET failed (" + url + ") res=" + std::to_string(static_cast<int>(res)) +
                      " msg=" + response.error);
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
    chunk = curl_slist_append(chunk, "Sec-CH-UA-Mobile: ?1");
    chunk = curl_slist_append(chunk, "Sec-CH-UA-Platform: \"Android\"");
    chunk = curl_slist_append(chunk, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8");
    chunk = curl_slist_append(chunk, "Accept-Language: vi-VN,vi;q=0.9,en-US;q=0.8,en;q=0.7");
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
    applyCommonOptions(curl, chunk, timeoutSec);
    if (access("/etc/ssl/certs/ca-certificates.crt", F_OK) == 0) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Linux; Android 13; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.6099.230 Mobile Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // Cap response size at 10MB to avoid OOM while allowing full portal/web forms.
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(10 * 1024 * 1024));

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
