#pragma once
// SVG raster via vendored NanoSVG (single-header, Zlib license).
// Covers logos/icons (flat paths, viewBox scaling). No external deps;
// compiled as C++ (headers carry extern "C" guards).
#include <cstdint>
#include <string>
#include <vector>

namespace RomCloud {

// Rasterize SVG bytes to RGBA. Target width caps output (aspect kept);
// outW/outH receive actual dims. Returns false when unparseable.
bool svgRasterize(const uint8_t* data, size_t len, int targetW,
                  std::vector<uint8_t>& outRgba, int& outW, int& outH);

}  // namespace RomCloud
