#include "QrRenderer.h"
#include "qrcodegen.hpp"
#include <algorithm>

namespace RomCloud {

void QrRenderer::renderQrCode(
    SDL_Renderer* renderer,
    const std::string& text,
    int x,
    int y,
    int targetSize,
    SDL_Color fgColor,
    SDL_Color bgColor
) {
    if (!renderer || text.empty()) return;

    try {
        // Use LOW error correction for digital screens to maximize module pixel size
        qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::LOW);
        int qrSize = qr.getSize();
        if (qrSize <= 0) return;

        // ISO/IEC 18004 standard requires quiet zone of at least 4 modules
        int quietZone = 4;
        int totalModules = qrSize + 2 * quietZone;
        int modulePixelSize = std::max(2, targetSize / totalModules);
        int actualDrawSize = totalModules * modulePixelSize;
        int offsetX = x + (targetSize - actualDrawSize) / 2;
        int offsetY = y + (targetSize - actualDrawSize) / 2;

        // Draw background with extra 12px margin
        SDL_SetRenderDrawColor(renderer, bgColor.r, bgColor.g, bgColor.b, 255);
        SDL_Rect bgRect = {offsetX - 12, offsetY - 12, actualDrawSize + 24, actualDrawSize + 24};
        SDL_RenderFillRect(renderer, &bgRect);

        // Draw pure modules
        SDL_SetRenderDrawColor(renderer, fgColor.r, fgColor.g, fgColor.b, 255);
        for (int qy = 0; qy < qrSize; qy++) {
            for (int qx = 0; qx < qrSize; qx++) {
                if (qr.getModule(qx, qy)) {
                    SDL_Rect modRect = {
                        offsetX + (qx + quietZone) * modulePixelSize,
                        offsetY + (qy + quietZone) * modulePixelSize,
                        modulePixelSize,
                        modulePixelSize
                    };
                    SDL_RenderFillRect(renderer, &modRect);
                }
            }
        }
    } catch (...) {
        // Fallback placeholder box
        SDL_SetRenderDrawColor(renderer, bgColor.r, bgColor.g, bgColor.b, bgColor.a);
        SDL_Rect bgRect = {x, y, targetSize, targetSize};
        SDL_RenderFillRect(renderer, &bgRect);
    }
}

} // namespace RomCloud
