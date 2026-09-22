/* 
 * QR Code generator library (C++)
 * 
 * Copyright (c) Project Nayuki. (MIT License)
 * https://www.nayuki.io/page/qr-code-generator-library
 */

#include "qrcodegen.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <utility>

namespace qrcodegen {

QrSegment::QrSegment(Mode md, int numCh, const std::vector<bool> &dt) :
    mode(md),
    numChars(numCh),
    data(dt) {
    if (numCh < 0)
        throw std::domain_error("Invalid value");
}

QrSegment::QrSegment(Mode md, int numCh, std::vector<bool> &&dt) :
    mode(md),
    numChars(numCh),
    data(std::move(dt)) {
    if (numCh < 0)
        throw std::domain_error("Invalid value");
}

QrSegment::Mode QrSegment::getMode() const { return mode; }
int QrSegment::getNumChars() const { return numChars; }
const std::vector<bool> &QrSegment::getData() const { return data; }

QrSegment QrSegment::makeBytes(const std::vector<uint8_t> &data) {
    std::vector<bool> bb;
    for (uint8_t b : data) {
        for (int i = 7; i >= 0; i--)
            bb.push_back(((b >> i) & 1) != 0);
    }
    return QrSegment(Mode::BYTE, static_cast<int>(data.size()), std::move(bb));
}

QrSegment QrSegment::makeBytes(const char *dataStr) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; dataStr[i] != '\0'; i++)
        bytes.push_back(static_cast<uint8_t>(dataStr[i]));
    return makeBytes(bytes);
}

// Table of Reed-Solomon Error Correction Code polynomials and capacities
static const int8_t ECC_CODEWORDS_PER_BLOCK[4][41] = {
    // Version: (0), 1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40
    {-1,  7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30}, // Low
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28}, // Medium
    {-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30}, // Quartile
    {-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30}, // High
};

static const int8_t NUM_ERROR_CORRECTION_BLOCKS[4][41] = {
    {-1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  4,  4,  4,  4,  4,  6,  6,  6,  6,  7,  8,  8,  9,  9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},
    {-1,  1,  1,  1,  2,  2,  4,  4,  4,  5,  5,  5,  8,  9,  9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},
    {-1,  1,  1,  2,  2,  4,  4,  6,  6,  8,  8,  8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},
    {-1,  1,  1,  2,  4,  4,  4,  5,  6,  8,  8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 72, 74, 77},
};

static int getNumDataCodewords(int ver, QrCode::Ecc ecl) {
    return (ver * 4 + 17) * (ver * 4 + 17) / 8
        - ECC_CODEWORDS_PER_BLOCK[static_cast<int>(ecl)][ver] * NUM_ERROR_CORRECTION_BLOCKS[static_cast<int>(ecl)][ver];
}

static uint8_t reedSolomonMultiply(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return static_cast<uint8_t>(z);
}

static std::vector<uint8_t> reedSolomonComputeDivisor(int degree) {
    std::vector<uint8_t> result(degree, 0);
    result.back() = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (size_t j = 0; j < result.size(); j++) {
            result[j] = reedSolomonMultiply(result[j], root);
            if (j + 1 < result.size())
                result[j] ^= result[j + 1];
        }
        root = reedSolomonMultiply(root, 0x02);
    }
    return result;
}

static std::vector<uint8_t> reedSolomonComputeRemainder(const std::vector<uint8_t> &data, const std::vector<uint8_t> &divisor) {
    std::vector<uint8_t> result(divisor.size(), 0);
    for (uint8_t b : data) {
        uint8_t factor = b ^ result[0];
        result.erase(result.begin());
        result.push_back(0);
        for (size_t i = 0; i < divisor.size(); i++)
            result[i] ^= reedSolomonMultiply(divisor[i], factor);
    }
    return result;
}

QrCode QrCode::encodeText(const char *text, Ecc ecc) {
    std::vector<QrSegment> segs = { QrSegment::makeBytes(text) };
    return encodeSegments(segs, ecc);
}

QrCode QrCode::encodeSegments(const std::vector<QrSegment> &segs, Ecc ecc, int minVersion, int maxVersion, int mask, bool boostEcl) {
    (void)boostEcl;
    for (int version = minVersion; version <= maxVersion; version++) {
        int dataCapacityBits = getNumDataCodewords(version, ecc) * 8;
        int usedBits = 4 + (version < 10 ? 8 : 16); // Header for byte mode
        for (const auto &seg : segs) {
            usedBits += static_cast<int>(seg.getData().size());
        }
        if (usedBits <= dataCapacityBits) {
            std::vector<bool> bb;
            bb.push_back(false); bb.push_back(true); bb.push_back(false); bb.push_back(false); // Mode::BYTE
            int ccBits = (version < 10 ? 8 : 16);
            int count = 0;
            for (const auto &seg : segs) count += seg.getNumChars();
            for (int i = ccBits - 1; i >= 0; i--) bb.push_back(((count >> i) & 1) != 0);
            for (const auto &seg : segs) {
                for (bool b : seg.getData()) bb.push_back(b);
            }
            // Terminator
            for (int i = 0; i < 4 && static_cast<int>(bb.size()) < dataCapacityBits; i++) bb.push_back(false);
            while (bb.size() % 8 != 0) bb.push_back(false);
            // Pad bytes
            for (uint8_t pad = 0xEC; static_cast<int>(bb.size()) < dataCapacityBits; pad ^= 0xEC ^ 0x11) {
                for (int i = 7; i >= 0; i--) bb.push_back(((pad >> i) & 1) != 0);
            }

            std::vector<uint8_t> dataCodewords(bb.size() / 8);
            for (size_t i = 0; i < bb.size(); i++) {
                dataCodewords[i >> 3] |= (bb[i] ? 1 : 0) << (7 - (i & 7));
            }
            return QrCode(version, ecc, dataCodewords, mask == -1 ? 0 : mask);
        }
    }
    throw std::length_error("Data too long for QR Code");
}

QrCode::QrCode(int ver, Ecc ecl, const std::vector<uint8_t> &dataCodewords, int msk) :
    version(ver),
    size(ver * 4 + 17),
    errorCorrectionLevel(ecl),
    mask(msk),
    modules(size, std::vector<bool>(size, false)),
    isFunction(size, std::vector<bool>(size, false)) {

    drawFunctionPatterns();
    drawCodewords(dataCodewords);
    applyMask(msk);
}

int QrCode::getVersion() const { return version; }
int QrCode::getSize() const { return size; }
QrCode::Ecc QrCode::getErrorCorrectionLevel() const { return errorCorrectionLevel; }
int QrCode::getMask() const { return mask; }
bool QrCode::getModule(int x, int y) const {
    if (x >= 0 && x < size && y >= 0 && y < size) return modules[y][x];
    return false;
}

void QrCode::setFunctionModule(int x, int y, bool isDark) {
    modules[y][x] = isDark;
    isFunction[y][x] = true;
}

void QrCode::drawFinderPattern(int x, int y) {
    for (int dy = -4; dy <= 4; dy++) {
        for (int dx = -4; dx <= 4; dx++) {
            int dist = std::max(std::abs(dx), std::abs(dy));
            int qx = x + dx, qy = y + dy;
            if (qx >= 0 && qx < size && qy >= 0 && qy < size) {
                setFunctionModule(qx, qy, dist != 2 && dist != 4);
            }
        }
    }
}

void QrCode::drawAlignmentPattern(int x, int y) {
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            setFunctionModule(x + dx, y + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
        }
    }
}

std::vector<int> QrCode::getAlignmentPatternPositions() const {
    if (version == 1) return {};
    int numAlign = version / 7 + 2;
    int step = (version == 32) ? 26 : (version * 4 + numAlign * 2 + 1) / (numAlign * 2 - 2) * 2;
    std::vector<int> pos(numAlign);
    pos[0] = 6;
    for (int i = numAlign - 1, p = size - 7; i >= 1; i--, p -= step) pos[i] = p;
    return pos;
}

void QrCode::drawFunctionPatterns() {
    // Timing patterns
    for (int i = 0; i < size; i++) {
        setFunctionModule(6, i, i % 2 == 0);
        setFunctionModule(i, 6, i % 2 == 0);
    }
    // Finder patterns
    drawFinderPattern(3, 3);
    drawFinderPattern(size - 4, 3);
    drawFinderPattern(3, size - 4);

    // Alignment patterns
    auto alignPos = getAlignmentPatternPositions();
    for (size_t i = 0; i < alignPos.size(); i++) {
        for (size_t j = 0; j < alignPos.size(); j++) {
            if ((i == 0 && j == 0) || (i == 0 && j == alignPos.size() - 1) || (i == alignPos.size() - 1 && j == 0)) continue;
            drawAlignmentPattern(alignPos[i], alignPos[j]);
        }
    }
    // Format bits dummy
    for (int i = 0; i <= 8; i++) {
        if (i != 6) { setFunctionModule(8, i, false); setFunctionModule(i, 8, false); }
    }
    for (int i = size - 8; i < size; i++) { setFunctionModule(8, i, false); setFunctionModule(i, 8, false); }
    setFunctionModule(8, size - 8, true);
}

void QrCode::drawCodewords(const std::vector<uint8_t> &data) {
    int numBlocks = NUM_ERROR_CORRECTION_BLOCKS[static_cast<int>(errorCorrectionLevel)][version];
    int blockEccLen = ECC_CODEWORDS_PER_BLOCK[static_cast<int>(errorCorrectionLevel)][version];
    int rawCodewords = (size * size) / 8; // approx
    (void)rawCodewords;

    auto divisor = reedSolomonComputeDivisor(blockEccLen);
    std::vector<std::vector<uint8_t>> dataBlocks(numBlocks);
    std::vector<std::vector<uint8_t>> eccBlocks(numBlocks);

    for (size_t i = 0, k = 0; i < data.size(); i++) {
        dataBlocks[k].push_back(data[i]);
        k = (k + 1) % numBlocks;
    }
    for (int i = 0; i < numBlocks; i++) {
        eccBlocks[i] = reedSolomonComputeRemainder(dataBlocks[i], divisor);
    }

    std::vector<uint8_t> allCodewords;
    size_t maxDataBlockLen = dataBlocks[0].size();
    for (size_t i = 0; i < maxDataBlockLen; i++) {
        for (int j = 0; j < numBlocks; j++) {
            if (i < dataBlocks[j].size()) allCodewords.push_back(dataBlocks[j][i]);
        }
    }
    for (int i = 0; i < blockEccLen; i++) {
        for (int j = 0; j < numBlocks; j++) {
            allCodewords.push_back(eccBlocks[j][i]);
        }
    }

    // Place bits into matrix
    size_t bitIdx = 0;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < size; vert++) {
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                bool upward = ((right + 1) & 2) == 0;
                int y = upward ? size - 1 - vert : vert;
                if (!isFunction[y][x] && bitIdx < allCodewords.size() * 8) {
                    bool bit = ((allCodewords[bitIdx >> 3] >> (7 - (bitIdx & 7))) & 1) != 0;
                    modules[y][x] = bit;
                    bitIdx++;
                }
            }
        }
    }
}

void QrCode::applyMask(int msk) {
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (isFunction[y][x]) continue;
            bool invert = false;
            switch (msk) {
                case 0: invert = (x + y) % 2 == 0; break;
                case 1: invert = y % 2 == 0; break;
                case 2: invert = x % 3 == 0; break;
                case 3: invert = (x + y) % 3 == 0; break;
                default: invert = (x + y) % 2 == 0; break;
            }
            if (invert) modules[y][x] = !modules[y][x];
        }
    }
}

} // namespace qrcodegen
