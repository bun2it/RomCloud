#pragma once
#include <string>
#include <thread>
#include <atomic>

namespace RomCloud {

class WebServer {
public:
    static WebServer& instance();
    bool start(int port = 8080);
    void stop();
    bool isRunning() const { return m_running; }
    int getPort() const { return m_port; }

private:
    WebServer() = default;
    ~WebServer();

    int m_port = 8080;
    int m_serverFd = -1;
    std::atomic<bool> m_running{false};
    std::thread m_thread;

    void serverLoop();
    void handleClient(int clientFd);
    std::string buildHtmlResponse();
    std::string buildSuccessResponse(const std::string& message);
};

} // namespace RomCloud
