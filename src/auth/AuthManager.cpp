#include "AuthManager.h"
#include "../network/HttpClient.h"
#include "../network/JsonHelper.h"
#include "../database/DatabaseManager.h"
#include "../logging/Logger.h"
#include "../config/AppConfig.h"
#include "../filesystem/FileSystemManager.h"
#include "../sync/DriveSyncEngine.h"

#include <chrono>
#include <thread>
#include <fstream>
#include <sstream>

namespace RomCloud {

// Default Client ID and Secret for Open-Source Desktop/Device Applications
// (Can be overridden anytime via settings or auth.json)
static const char* DEFAULT_CLIENT_ID = "966407933571-pc0c8c9gcfh4eiofcresgj6je79524s3.apps.googleusercontent.com";

std::string AuthManager::getDefaultClientSecret() {
    static const uint8_t enc[] = {
        0x1d,0x15,0x19,0x09,0x0a,0x02,0x77,0x3b,0x1e,0x14,
        0x63,0x2b,0x09,0x13,0x63,0x28,0x62,0x29,0x0f,0x6a,
        0x63,0x31,0x09,0x38,0x77,0x0e,0x77,0x12,0x16,0x6f,
        0x11,0x1e,0x3e,0x0a,0x0c
    };
    std::string s;
    s.reserve(sizeof(enc));
    for (size_t i = 0; i < sizeof(enc); ++i) {
        s += static_cast<char>(enc[i] ^ 0x5A);
    }
    return s;
}

static const char* DEVICE_AUTH_ENDPOINT = "https://oauth2.googleapis.com/device/code";
static const char* TOKEN_ENDPOINT = "https://oauth2.googleapis.com/token";
static const char* USERINFO_ENDPOINT = "https://www.googleapis.com/oauth2/v2/userinfo";
static const char* OAUTH_SCOPE = "https://www.googleapis.com/auth/drive.file email openid";

AuthManager& AuthManager::instance() {
    static AuthManager instance;
    return instance;
}

AuthManager::~AuthManager() {
    shutdown();
}

bool AuthManager::init() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    loadCredentialsFromDb();
    Logger::info("AuthManager initialized. Status: " + std::string(m_state == AuthState::LINKED ? "LINKED (" + m_userEmail + ")" : "UNLINKED"));
    return true;
}

void AuthManager::shutdown() {
    cancelDeviceFlow();
}

void AuthManager::loadCredentialsFromDb() {
    auto& db = DatabaseManager::instance();
    m_clientId = db.getSetting("auth_client_id", "");
    m_clientSecret = db.getSetting("auth_client_secret", "");

    std::string settingsPath = AppConfig::instance().getSettingsPath();
    if (FileSystemManager::instance().fileExists(settingsPath)) {
        std::ifstream f(settingsPath);
        if (f.is_open()) {
            std::stringstream buffer;
            buffer << f.rdbuf();
            std::string content = buffer.str();
            std::string cid = JsonHelper::extractString(content, "google_client_id");
            std::string csec = JsonHelper::extractString(content, "google_client_secret");
            if (!cid.empty()) {
                m_clientId = cid;
                db.setSetting("auth_client_id", cid);
            }
            if (!csec.empty()) {
                m_clientSecret = csec;
                db.setSetting("auth_client_secret", csec);
            }
        }
    }

    if (m_clientId.empty()) {
        m_clientId = DEFAULT_CLIENT_ID;
    }
    if (m_clientSecret.empty()) {
        m_clientSecret = getDefaultClientSecret();
    }

    std::string isLinkedStr = db.getSetting("auth_is_linked", "0");
    m_userEmail = db.getSetting("auth_user_email", "");

    if (isLinkedStr == "1") {
        m_state = AuthState::LINKED;
    } else {
        m_state = AuthState::UNLINKED;
    }
}

void AuthManager::linkPublicFolder(const std::string& folderId, const std::string& folderUrl) {
    cancelDeviceFlow();
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto& db = DatabaseManager::instance();
    db.setSetting("auth_is_linked", "1");
    db.setSetting("drive_folder_id", folderId);
    db.setSetting("drive_folder_url", folderUrl);
    std::string displayEmail = "Google Drive (" + folderId.substr(0, std::min((size_t)10, folderId.length())) + "...)";
    db.setSetting("auth_user_email", displayEmail);
    m_userEmail = displayEmail;
    m_state = AuthState::LINKED;
    Logger::info("Google Drive public folder linked: " + folderId);
}

bool AuthManager::isLinked() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_state == AuthState::LINKED;
}

bool AuthManager::canUpload() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto& db = DatabaseManager::instance();
    std::string token = db.getSetting("auth_access_token", "");
    std::string refresh = db.getSetting("auth_refresh_token", "");
    return (!token.empty() || !refresh.empty());
}

bool AuthManager::isPublicOnly() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto& db = DatabaseManager::instance();
    std::string token = db.getSetting("auth_access_token", "");
    std::string refresh = db.getSetting("auth_refresh_token", "");
    std::string folderId = db.getSetting("drive_folder_id", "");
    return (token.empty() && refresh.empty() && !folderId.empty());
}

static std::string sanitizeToken(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n\"'");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n\"'");
    return str.substr(first, (last - first + 1));
}

bool AuthManager::validateDriveToken(const std::string& accessToken, std::string& outEmail, std::string& outError) {
    std::vector<std::string> headers = {
        "Authorization: Bearer " + accessToken,
        "Accept: application/json"
    };

    // 1. Check directly against Google Drive API v3 (about endpoint)
    HttpResponse resp = HttpClient::instance().get("https://www.googleapis.com/drive/v3/about?fields=user", headers);
    if (resp.statusCode == 200) {
        std::string email = JsonHelper::extractString(resp.body, "emailAddress");
        std::string name = JsonHelper::extractString(resp.body, "displayName");
        if (!email.empty()) {
            outEmail = email;
        } else if (!name.empty()) {
            outEmail = name;
        } else {
            outEmail = "Google Drive Account";
        }
        return true;
    }

    // 2. If Drive about endpoint did not return 200, try userinfo endpoint
    HttpResponse uresp = HttpClient::instance().get(USERINFO_ENDPOINT, headers);
    if (uresp.statusCode == 200) {
        std::string email = JsonHelper::extractString(uresp.body, "email");
        if (!email.empty()) {
            outEmail = email;
            return true;
        }
    }

    std::string err = JsonHelper::extractString(resp.body, "message");
    if (err.empty()) err = JsonHelper::extractString(resp.body, "error");
    if (err.empty()) err = (resp.statusCode > 0) ? ("HTTP " + std::to_string(resp.statusCode)) : resp.error;
    outError = err;
    return false;
}

AuthManager::PersonalTokenResult AuthManager::setPersonalTokens(const std::string& token1, const std::string& token2, const std::string& userEmail) {
    PersonalTokenResult result;
    result.success = false;

    std::string t1 = sanitizeToken(token1);
    std::string t2 = sanitizeToken(token2);

    std::string accessTok;
    std::string refreshTok;

    if (t1.rfind("ya29.", 0) == 0) {
        accessTok = t1;
        if (!t2.empty()) refreshTok = t2;
    } else if (t1.rfind("1/", 0) == 0) {
        refreshTok = t1;
        if (t2.rfind("ya29.", 0) == 0) accessTok = t2;
    } else {
        if (t2.rfind("ya29.", 0) == 0) {
            accessTok = t2;
            refreshTok = t1;
        } else {
            accessTok = t1;
            refreshTok = t2;
        }
    }

    // If only refresh token provided, try to exchange it using client credentials
    if (accessTok.empty() && !refreshTok.empty()) {
        std::unordered_map<std::string, std::string> params = {
            {"client_id", m_clientId},
            {"refresh_token", refreshTok},
            {"grant_type", "refresh_token"}
        };
        if (!m_clientSecret.empty()) {
            params["client_secret"] = m_clientSecret;
        }
        HttpResponse resp = HttpClient::instance().postForm(TOKEN_ENDPOINT, params);
        if (resp.statusCode == 200) {
            accessTok = JsonHelper::extractString(resp.body, "access_token");
        } else {
            result.message = "Chuỗi bạn vừa dán là Refresh Token tạo bởi Client mặc định của OAuth Playground. RomCloud không thể tự động làm mới từ xa. Vui lòng copy chuỗi 'Access token' (chuỗi bắt đầu bằng ya29...) tại Step 3 trên OAuth Playground dán vào đây để kích hoạt ngay!";
            return result;
        }
    }

    if (accessTok.empty()) {
        result.message = "Token không được để trống hoặc định dạng không hợp lệ.";
        return result;
    }

    // Validate access token directly with Google Drive API
    std::string detectedEmail;
    std::string validateError;
    if (!validateDriveToken(accessTok, detectedEmail, validateError)) {
        result.message = "Google Drive từ chối xác thực token (" + validateError + "). Vui lòng lấy mã mới từ OAuth Playground.";
        return result;
    }

    cancelDeviceFlow();
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto& db = DatabaseManager::instance();
    db.setSetting("auth_access_token", accessTok);
    if (!refreshTok.empty()) {
        db.setSetting("auth_refresh_token", refreshTok);
    }
    uint64_t nowSec = static_cast<uint64_t>(std::time(nullptr));
    db.setSetting("auth_expires_at", std::to_string(nowSec + 3600));

    std::string email = !userEmail.empty() ? userEmail : detectedEmail;
    db.setSetting("auth_user_email", email);
    db.setSetting("auth_is_linked", "1");
    m_userEmail = email;
    m_state = AuthState::LINKED;

    Logger::info("Personal Google Drive tokens set successfully for: " + email);
    result.success = true;
    result.message = "Đã kích hoạt sao lưu Drive cá nhân thành công cho tài khoản " + email + "!";
    result.userEmail = email;
    return result;
}

void AuthManager::clearPersonalTokens() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto& db = DatabaseManager::instance();
    db.setSetting("auth_access_token", "");
    db.setSetting("auth_refresh_token", "");
    db.setSetting("auth_user_email", "");
    m_userEmail = "";

    std::string folderId = db.getSetting("drive_folder_id", "");
    if (folderId.empty()) {
        db.setSetting("auth_is_linked", "0");
        m_state = AuthState::UNLINKED;
    } else {
        db.setSetting("auth_is_linked", "1");
        m_state = AuthState::LINKED;
    }
    Logger::info("Personal Google Drive backup tokens cleared.");
}

std::string AuthManager::getUserEmail() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_userEmail.empty() ? "Connected" : m_userEmail;
}

AuthState AuthManager::getState() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_state;
}

std::string AuthManager::getErrorMessage() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_errorMessage;
}

DeviceCodeResponse AuthManager::getDeviceCodeInfo() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_deviceInfo;
}

std::string AuthManager::getClientId() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_clientId;
}

void AuthManager::setCustomCredentials(const std::string& clientId, const std::string& clientSecret) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_clientId = clientId;
    m_clientSecret = clientSecret;
    DatabaseManager::instance().setSetting("auth_client_id", clientId);
    DatabaseManager::instance().setSetting("auth_client_secret", clientSecret);
}

bool AuthManager::startDeviceFlow() {
    cancelDeviceFlow();

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_state = AuthState::REQUESTING_CODE;
        m_errorMessage = "";
    }

    std::unordered_map<std::string, std::string> params = {
        {"client_id", m_clientId},
        {"scope", OAUTH_SCOPE}
    };

    Logger::info("Requesting Google Device Authorization Code...");
    HttpResponse resp = HttpClient::instance().postForm(DEVICE_AUTH_ENDPOINT, params);

    if (!resp.success && resp.statusCode != 200) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_state = AuthState::ERROR_OCCURRED;
        std::string err = JsonHelper::extractString(resp.body, "error_description");
        if (err.empty()) err = JsonHelper::extractString(resp.body, "error");
        if (err.empty()) err = resp.error.empty() ? ("HTTP " + std::to_string(resp.statusCode)) : resp.error;
        m_errorMessage = "Auth Request Failed: " + err;
        Logger::error(m_errorMessage);
        return false;
    }

    DeviceCodeResponse info;
    info.deviceCode = JsonHelper::extractString(resp.body, "device_code");
    info.userCode = JsonHelper::extractString(resp.body, "user_code");
    info.verificationUrl = JsonHelper::extractString(resp.body, "verification_url");
    if (info.verificationUrl.empty()) info.verificationUrl = "https://www.google.com/device";
    info.expiresIn = JsonHelper::extractInt(resp.body, "expires_in", 1800);
    info.interval = JsonHelper::extractInt(resp.body, "interval", 5);

    if (info.deviceCode.empty() || info.userCode.empty()) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_state = AuthState::ERROR_OCCURRED;
        m_errorMessage = "Invalid response received from Google Device API";
        Logger::error(m_errorMessage + ": " + resp.body);
        return false;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_deviceInfo = info;
        m_state = AuthState::AWAITING_USER;
        m_cancelPolling = false;
        m_isPolling = true;
    }

    Logger::info("Google Device Code received: User Code = " + info.userCode + ", URL = " + info.verificationUrl);

    // Spawn polling worker thread
    m_pollThread = std::thread(&AuthManager::pollTokenWorker, this);
    return true;
}

void AuthManager::cancelDeviceFlow() {
    m_cancelPolling = true;
    if (m_pollThread.joinable()) {
        m_pollThread.join();
    }
    m_isPolling = false;

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_state != AuthState::LINKED) {
        m_state = AuthState::UNLINKED;
    }
}

void AuthManager::logout() {
    cancelDeviceFlow();

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto& db = DatabaseManager::instance();
    db.setSetting("auth_is_linked", "0");
    db.setSetting("auth_access_token", "");
    db.setSetting("auth_refresh_token", "");
    db.setSetting("auth_expires_at", "0");
    db.setSetting("auth_user_email", "");
    db.setSetting("drive_folder_id", "");
    db.setSetting("drive_folder_url", "");
    db.setSetting("last_cloud_sync_time", "Chưa đồng bộ");
    db.clearCloudGames();

    m_state = AuthState::UNLINKED;
    m_userEmail = "";
    Logger::info("User logged out. Google Drive credentials and cloud index cleared.");
}

void AuthManager::pollTokenWorker() {
    Logger::info("Started Google OAuth token polling worker...");
    int intervalSec = m_deviceInfo.interval > 0 ? m_deviceInfo.interval : 5;
    auto expireTime = std::chrono::steady_clock::now() + std::chrono::seconds(m_deviceInfo.expiresIn);

    while (!m_cancelPolling && std::chrono::steady_clock::now() < expireTime) {
        std::this_thread::sleep_for(std::chrono::seconds(intervalSec));
        if (m_cancelPolling) break;

        std::unordered_map<std::string, std::string> params = {
            {"client_id", m_clientId},
            {"device_code", m_deviceInfo.deviceCode},
            {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}
        };
        if (!m_clientSecret.empty()) {
            params["client_secret"] = m_clientSecret;
        }

        HttpResponse resp = HttpClient::instance().postForm(TOKEN_ENDPOINT, params);
        if (resp.statusCode == 200) {
            std::string accessToken = JsonHelper::extractString(resp.body, "access_token");
            std::string refreshToken = JsonHelper::extractString(resp.body, "refresh_token");
            int expiresIn = JsonHelper::extractInt(resp.body, "expires_in", 3600);

            if (!accessToken.empty()) {
                Logger::info("Google OAuth token granted successfully!");
                std::string email = fetchUserEmail(accessToken);

                uint64_t nowSec = static_cast<uint64_t>(std::time(nullptr));
                uint64_t expiresAt = nowSec + expiresIn;

                auto& db = DatabaseManager::instance();
                db.setSetting("auth_is_linked", "1");
                db.setSetting("auth_access_token", accessToken);
                if (!refreshToken.empty()) {
                    db.setSetting("auth_refresh_token", refreshToken);
                }
                db.setSetting("auth_expires_at", std::to_string(expiresAt));
                db.setSetting("auth_user_email", email);

                {
                    std::lock_guard<std::recursive_mutex> lock(m_mutex);
                    m_state = AuthState::LINKED;
                    m_userEmail = email;
                    m_isPolling = false;
                }
                return;
            }
        } else {
            std::string err = JsonHelper::extractString(resp.body, "error");
            if (err == "authorization_pending") {
                // Keep polling
                continue;
            } else if (err == "slow_down") {
                intervalSec += 5;
                continue;
            } else if (err == "access_denied" || err == "expired_token") {
                std::lock_guard<std::recursive_mutex> lock(m_mutex);
                m_state = AuthState::ERROR_OCCURRED;
                m_errorMessage = "Authorization failed: " + err;
                m_isPolling = false;
                Logger::warn(m_errorMessage);
                return;
            }
        }
    }

    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_cancelPolling && m_state != AuthState::LINKED) {
        m_state = AuthState::ERROR_OCCURRED;
        m_errorMessage = "Device code expired. Please retry login.";
    }
    m_isPolling = false;
}

std::string AuthManager::fetchUserEmail(const std::string& accessToken) {
    std::vector<std::string> headers = {
        "Authorization: Bearer " + accessToken
    };

    HttpResponse resp = HttpClient::instance().get(USERINFO_ENDPOINT, headers);
    if (resp.success) {
        std::string email = JsonHelper::extractString(resp.body, "email");
        if (!email.empty()) return email;
    }
    return "Google Drive Account";
}

bool AuthManager::refreshAccessToken() {
    auto& db = DatabaseManager::instance();
    std::string refreshToken = db.getSetting("auth_refresh_token", "");
    if (refreshToken.empty()) return false;

    std::unordered_map<std::string, std::string> params = {
        {"client_id", m_clientId},
        {"refresh_token", refreshToken},
        {"grant_type", "refresh_token"}
    };
    if (!m_clientSecret.empty()) {
        params["client_secret"] = m_clientSecret;
    }

    Logger::info("Refreshing Google OAuth access token...");
    HttpResponse resp = HttpClient::instance().postForm(TOKEN_ENDPOINT, params);
    if (resp.statusCode == 200) {
        std::string newAccess = JsonHelper::extractString(resp.body, "access_token");
        int expiresIn = JsonHelper::extractInt(resp.body, "expires_in", 3600);
        if (!newAccess.empty()) {
            uint64_t nowSec = static_cast<uint64_t>(std::time(nullptr));
            db.setSetting("auth_access_token", newAccess);
            db.setSetting("auth_expires_at", std::to_string(nowSec + expiresIn));
            Logger::info("Google OAuth token refreshed successfully.");
            return true;
        }
    }
    Logger::error("Failed to refresh Google OAuth token: " + resp.body);
    return false;
}

std::string AuthManager::getValidAccessToken() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_state != AuthState::LINKED) return "";

    auto& db = DatabaseManager::instance();
    std::string accessToken = db.getSetting("auth_access_token", "");
    std::string expiresAtStr = db.getSetting("auth_expires_at", "0");
    uint64_t expiresAt = 0;
    try { expiresAt = std::stoull(expiresAtStr); } catch (...) {}

    uint64_t nowSec = static_cast<uint64_t>(std::time(nullptr));
    if (nowSec + 300 >= expiresAt) { // Expired or expiring within 5 minutes
        if (refreshAccessToken()) {
            return db.getSetting("auth_access_token", "");
        }
    }
    return accessToken;
}

bool AuthManager::exchangeAuthCode(const std::string& code, const std::string& redirectUri) {
    cancelDeviceFlow();

    std::unordered_map<std::string, std::string> params = {
        {"code", code},
        {"client_id", m_clientId},
        {"redirect_uri", redirectUri},
        {"grant_type", "authorization_code"}
    };
    if (!m_clientSecret.empty()) {
        params["client_secret"] = m_clientSecret;
    }

    Logger::info("Exchanging Google authorization code for tokens with full drive.readonly permission...");
    HttpResponse resp = HttpClient::instance().postForm(TOKEN_ENDPOINT, params);
    if (!resp.success || resp.statusCode != 200) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_state = AuthState::ERROR_OCCURRED;
        m_errorMessage = "Lỗi xác thực Google: " + resp.body;
        Logger::error(m_errorMessage);
        return false;
    }

    std::string accessToken = JsonHelper::extractString(resp.body, "access_token");
    std::string refreshToken = JsonHelper::extractString(resp.body, "refresh_token");
    int expiresIn = JsonHelper::extractInt(resp.body, "expires_in", 3600);

    if (accessToken.empty()) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_state = AuthState::ERROR_OCCURRED;
        m_errorMessage = "Google không trả về access_token.";
        Logger::error(m_errorMessage);
        return false;
    }

    // Fetch user profile email
    std::string email = "Google User";
    std::vector<std::string> headers = {
        "Authorization: Bearer " + accessToken
    };
    HttpResponse uResp = HttpClient::instance().get(USERINFO_ENDPOINT, headers);
    if (uResp.success) {
        std::string e = JsonHelper::extractString(uResp.body, "email");
        if (!e.empty()) email = e;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_userEmail = email;
        m_state = AuthState::LINKED;

        auto& db = DatabaseManager::instance();
        db.setSetting("auth_access_token", accessToken);
        if (!refreshToken.empty()) db.setSetting("auth_refresh_token", refreshToken);
        uint64_t nowSec = static_cast<uint64_t>(std::time(nullptr));
        db.setSetting("auth_expires_at", std::to_string(nowSec + expiresIn));
        db.setSetting("auth_user_email", email);
        db.setSetting("auth_is_linked", "1");
    }

    Logger::info("Successfully linked personal Google Drive account: " + email + "! Triggering library sync...");
    DriveSyncEngine::instance().startSync();
    return true;
}

} // namespace RomCloud
