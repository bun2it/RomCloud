// NanoSVG implementation TU (must be compiled exactly once).
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "SvgRaster.h"

namespace RomCloud {

bool svgRasterize(const uint8_t* data, size_t len, int targetW,
                  std::vector<uint8_t>& outRgba, int& outW, int& outH) {
    outRgba.clear();
    outW = 0;
    outH = 0;
    // Guard against null, tiny noise, or oversized SVGs (>512KB)
    if (!data || len < 16 || len > 512 * 1024 || targetW <= 0) return false;
    // NanoSVG wants a NUL-terminated copy.
    std::string xml((const char*)data, len);
    NSVGimage* img = nsvgParse(&xml[0], "px", 96.0f);
    if (!img || !img->shapes || !std::isfinite(img->width) || !std::isfinite(img->height) ||
        img->width <= 0 || img->height <= 0) {
        if (img) nsvgDelete(img);
        return false;
    }
    float scale = (float)targetW / img->width;
    if (!std::isfinite(scale) || scale <= 0) scale = 1.0f;
    int w = targetW;
    int h = (int)(img->height * scale + 0.5f);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
        nsvgDelete(img);
        return false;
    }
    outRgba.assign((size_t)w * (size_t)h * 4, 0);
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) {
        nsvgDelete(img);
        outRgba.clear();
        return false;
    }
    nsvgRasterize(rast, img, 0, 0, scale, outRgba.data(), w, h, w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);
    // Reject blank output (unparseable shapes → transparent sheet).
    size_t alpha = 0;
    for (size_t i = 3; i < outRgba.size(); i += 64) {
        if (outRgba[i] > 8) {
            alpha++;
            if (alpha >= 4) break;
        }
    }
    if (alpha < 4) {
        outRgba.clear();
        return false;
    }
    outW = w;
    outH = h;
    return true;
}

}  // namespace RomCloud
