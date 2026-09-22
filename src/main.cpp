#include "app/Application.h"
#include <iostream>

int main(int argc, char* argv[]) {
    auto& app = RomCloud::Application::instance();
    if (!app.init(argc, argv)) {
        std::cerr << "[FATAL] Failed to initialize RomCloud application." << std::endl;
        return 1;
    }
    app.run();
    app.shutdown();
    return app.getExitCode();
}
