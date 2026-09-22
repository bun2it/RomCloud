/* 
 * QR Code generator library (C++)
 * 
 * Copyright (c) Project Nayuki. (MIT License)
 * https://www.nayuki.io/page/qr-code-generator-library
 */

#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace qrcodegen {

class QrSegment final {
public:
    enum class Mode {
        NUMERIC, ALPHANUMERIC, BYTE, KANJI, ECI
    };

    static QrSegment makeBytes(const std::vector<uint8_t> &data);
    static QrSegment makeBytes(const char *dataStr);
    static QrSegment makeAlphanumeric(const char *text);
    static QrSegment makeNumeric(const char *digits);

    QrSegment(Mode md, int numCh, const std::vector<bool> &dt);
    QrSegment(Mode md, int numCh, std::vector<bool> &&dt);

    Mode getMode() const;
    int getNumChars() const;
    const std::vector<bool> &getData() const;

private:
    Mode mode;
    int numChars;
    std::vector<bool> data;
};

class QrCode final {
public:
    enum class Ecc {
        LOW = 0, MEDIUM, QUARTILE, HIGH
    };

    static QrCode encodeText(const char *text, Ecc ecc);
    static QrCode encodeBinary(const std::vector<uint8_t> &data, Ecc ecc);
    static QrCode encodeSegments(const std::vector<QrSegment> &segs, Ecc ecc,
        int minVersion = 1, int maxVersion = 40, int mask = -1, bool boostEcl = true);

    QrCode(int ver, Ecc ecl, const std::vector<uint8_t> &dataCodewords, int msk);

    int getVersion() const;
    int getSize() const;
    Ecc getErrorCorrectionLevel() const;
    int getMask() const;
    bool getModule(int x, int y) const;

private:
    int version;
    int size;
    Ecc errorCorrectionLevel;
    int mask;
    std::vector<std::vector<bool>> modules;
    std::vector<std::vector<bool>> isFunction;

    void drawFunctionPatterns();
    void drawFinderPattern(int x, int y);
    void drawAlignmentPattern(int x, int y);
    void setFunctionModule(int x, int y, bool isDark);
    void drawFormatBits(int msk);
    void drawVersion();
    void drawCodewords(const std::vector<uint8_t> &data);
    void applyMask(int msk);
    int getPenaltyScore() const;
    std::vector<int> getAlignmentPatternPositions() const;

    static const int MIN_VERSION = 1;
    static const int MAX_VERSION = 40;
};

} // namespace qrcodegen
