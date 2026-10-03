#pragma once
#include <string>
#include <thread>
#include <atomic>

namespace RomCloud {

class WebServer {
public:
    static WebServer& instance();
    bool start(int port = 8888);
    void stop();
    bool isRunning() const { return m_running; }
    int getPort() const { return m_port; }
    int cycleNextPort();
    // URL prefix of the running portal, e.g. "http://192.168.1.155:8082".
    // Empty when server is not running.
    std::string getPortalUrl() const;

private:
    WebServer() = default;
    ~WebServer();

    int m_port = 8888;
    int m_serverFd = -1;
    std::atomic<bool> m_running{false};
    std::thread m_thread;

    void serverLoop();
    void handleClient(int clientFd);
    std::string buildHtmlResponse();
    std::string buildSuccessResponse(const std::string& message);
};

} // namespace RomCloud
