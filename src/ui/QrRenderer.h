#pragma once
#include <SDL2/SDL.h>
#include <string>

namespace RomCloud {

class QrRenderer {
public:
    static void renderQrCode(
        SDL_Renderer* renderer,
        const std::string& text,
        int x,
        int y,
        int targetSize,
        SDL_Color fgColor = {0, 0, 0, 255},
        SDL_Color bgColor = {255, 255, 255, 255}
    );
};

} // namespace RomCloud
