#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

namespace RomCloud {

enum class AuthState {
    UNLINKED,
    REQUESTING_CODE,
    AWAITING_USER,
    LINKED,
    REFRESHING,
    ERROR_OCCURRED
};

struct DeviceCodeResponse {
    std::string deviceCode;
    std::string userCode;
    std::string verificationUrl;
    int expiresIn = 1800;
    int interval = 5;
};

class AuthManager {
public:
    static AuthManager& instance();
    bool init();
    void shutdown();

    bool isLinked() const;
    std::string getUserEmail() const;
    AuthState getState() const;
    std::string getErrorMessage() const;

    // Start Device Authorization flow
    bool startDeviceFlow();
    void cancelDeviceFlow();
    void logout();
    void linkPublicFolder(const std::string& folderId, const std::string& folderUrl);
    bool exchangeAuthCode(const std::string& code, const std::string& redirectUri);

    // Personal token and upload permission check
    bool canUpload() const;
    bool isPublicOnly() const;
    void setPersonalTokens(const std::string& accessToken, const std::string& refreshToken = "", const std::string& userEmail = "");

    // Get current device flow prompt data
    DeviceCodeResponse getDeviceCodeInfo() const;

    // Returns a valid access token, auto-refreshing if expired
    std::string getValidAccessToken();

    // Custom Client ID / Secret configuration
    void setCustomCredentials(const std::string& clientId, const std::string& clientSecret);
    std::string getClientId() const;
    static std::string getDefaultClientSecret();

private:
    AuthManager() = default;
    ~AuthManager();

    mutable std::recursive_mutex m_mutex;
    AuthState m_state = AuthState::UNLINKED;
    std::string m_errorMessage;
    DeviceCodeResponse m_deviceInfo;
    std::string m_userEmail;

    std::string m_clientId;
    std::string m_clientSecret;

    std::thread m_pollThread;
    std::atomic<bool> m_cancelPolling{false};
    std::atomic<bool> m_isPolling{false};

    void loadCredentialsFromDb();
    void pollTokenWorker();
    bool refreshAccessToken();
    std::string fetchUserEmail(const std::string& accessToken);
};

} // namespace RomCloud
