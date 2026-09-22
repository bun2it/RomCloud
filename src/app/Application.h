#pragma once
#include <SDL2/SDL.h>
#include <string>
#include <atomic>

namespace RomCloud {

class Application {
public:
    static Application& instance();
    bool init(int argc, char* argv[]);
    void run();
    void shutdown();
    void requestExit();
    void requestRestart() { m_exitCode = 42; requestExit(); }
    int getExitCode() const { return m_exitCode; }

private:
    Application() = default;
    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    std::atomic<bool> m_running{false};
    int m_exitCode = 0;
    bool initSDL();
};

} // namespace RomCloud
