#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <cstdint>
#include <cstring>

namespace RomCloud {

class HashHelper {
public:
    // Computes MD5 checksum of a file on disk (matches Google Drive's md5Checksum)
    static std::string computeFileMd5(const std::string& filePath) {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) return "";

        MD5Context ctx;
        md5Init(&ctx);

        std::vector<char> buffer(65536); // 64 KB buffer
        while (file.good()) {
            file.read(buffer.data(), buffer.size());
            std::streamsize bytesRead = file.gcount();
            if (bytesRead > 0) {
                md5Update(&ctx, reinterpret_cast<const uint8_t*>(buffer.data()), static_cast<size_t>(bytesRead));
            }
        }

        uint8_t digest[16];
        md5Finalize(&ctx, digest);

        std::stringstream ss;
        for (int i = 0; i < 16; ++i) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
        }
        return ss.str();
    }

private:
    struct MD5Context {
        uint32_t state[4];
        uint32_t count[2];
        uint8_t buffer[64];
    };

    static inline uint32_t F(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); }
    static inline uint32_t G(uint32_t x, uint32_t y, uint32_t z) { return (x & z) | (y & ~z); }
    static inline uint32_t H(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
    static inline uint32_t I(uint32_t x, uint32_t y, uint32_t z) { return y ^ (x | ~z); }
    static inline uint32_t ROTLEFT(uint32_t a, uint32_t s) { return (a << s) | (a >> (32 - s)); }

    static void md5Transform(uint32_t state[4], const uint8_t block[64]) {
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], x[16];
        for (int i = 0, j = 0; i < 16; ++i, j += 4) {
            x[i] = static_cast<uint32_t>(block[j]) |
                   (static_cast<uint32_t>(block[j + 1]) << 8) |
                   (static_cast<uint32_t>(block[j + 2]) << 16) |
                   (static_cast<uint32_t>(block[j + 3]) << 24);
        }

        #define FF(a, b, c, d, x, s, ac) { a = ROTLEFT(a + F(b, c, d) + x + ac, s) + b; }
        #define GG(a, b, c, d, x, s, ac) { a = ROTLEFT(a + G(b, c, d) + x + ac, s) + b; }
        #define HH(a, b, c, d, x, s, ac) { a = ROTLEFT(a + H(b, c, d) + x + ac, s) + b; }
        #define II(a, b, c, d, x, s, ac) { a = ROTLEFT(a + I(b, c, d) + x + ac, s) + b; }

        // Round 1
        FF(a, b, c, d, x[ 0],  7, 0xd76aa478); FF(d, a, b, c, x[ 1], 12, 0xe8c7b756);
        FF(c, d, a, b, x[ 2], 17, 0x242070db); FF(b, c, d, a, x[ 3], 22, 0xc1bdceee);
        FF(a, b, c, d, x[ 4],  7, 0xf57c0faf); FF(d, a, b, c, x[ 5], 12, 0x4787c62a);
        FF(c, d, a, b, x[ 6], 17, 0xa8304613); FF(b, c, d, a, x[ 7], 22, 0xfd469501);
        FF(a, b, c, d, x[ 8],  7, 0x698098d8); FF(d, a, b, c, x[ 9], 12, 0x8b44f7af);
        FF(c, d, a, b, x[10], 17, 0xffff5bb1); FF(b, c, d, a, x[11], 22, 0x895cd7be);
        FF(a, b, c, d, x[12],  7, 0x6b901122); FF(d, a, b, c, x[13], 12, 0xfd987193);
        FF(c, d, a, b, x[14], 17, 0xa679438e); FF(b, c, d, a, x[15], 22, 0x49b40821);

        // Round 2
        GG(a, b, c, d, x[ 1],  5, 0xf61e2562); GG(d, a, b, c, x[ 6],  9, 0xc040b340);
        GG(c, d, a, b, x[11], 14, 0x265e5a51); GG(b, c, d, a, x[ 0], 20, 0xe9b6c7aa);
        GG(a, b, c, d, x[ 5],  5, 0xd62f105d); GG(d, a, b, c, x[10],  9, 0x02441453);
        GG(c, d, a, b, x[15], 14, 0xd8a1e681); GG(b, c, d, a, x[ 4], 20, 0xe7d3fbc8);
        GG(a, b, c, d, x[ 9],  5, 0x21e1cde6); GG(d, a, b, c, x[14],  9, 0xc33707d6);
        GG(c, d, a, b, x[ 3], 14, 0xf4d50d87); GG(b, c, d, a, x[ 8], 20, 0x455a14ed);
        GG(a, b, c, d, x[13],  5, 0xa9e3e905); GG(d, a, b, c, x[ 2],  9, 0xfcefa3f8);
        GG(c, d, a, b, x[ 7], 14, 0x676f02d9); GG(b, c, d, a, x[12], 20, 0x8d2a4c8a);

        // Round 3
        HH(a, b, c, d, x[ 5],  4, 0xfffa3942); HH(d, a, b, c, x[ 8], 11, 0x8771f681);
        HH(c, d, a, b, x[11], 16, 0x6d9d6122); HH(b, c, d, a, x[14], 23, 0xfde5380c);
        HH(a, b, c, d, x[ 1],  4, 0xa4beea44); HH(d, a, b, c, x[ 4], 11, 0x4bdecfa9);
        HH(c, d, a, b, x[ 7], 16, 0xf6bb4b60); HH(b, c, d, a, x[10], 23, 0xbebfbc70);
        HH(a, b, c, d, x[13],  4, 0x289b7ec6); HH(d, a, b, c, x[ 0], 11, 0xeaa127fa);
        HH(c, d, a, b, x[ 3], 16, 0xd4ef3085); HH(b, c, d, a, x[ 6], 23, 0x04881d05);
        HH(a, b, c, d, x[ 9],  4, 0xd9d4d039); HH(d, a, b, c, x[12], 11, 0xe6db99e5);
        HH(c, d, a, b, x[15], 16, 0x1fa27cf8); HH(b, c, d, a, x[ 2], 23, 0xc4ac5665);

        // Round 4
        II(a, b, c, d, x[ 0],  6, 0xf4292244); II(d, a, b, c, x[ 7], 10, 0x432aff97);
        II(c, d, a, b, x[14], 15, 0xab9423a7); II(b, c, d, a, x[ 5], 21, 0xfc93a039);
        II(a, b, c, d, x[12],  6, 0x655b59c3); II(d, a, b, c, x[ 3], 10, 0x8f0ccc92);
        II(c, d, a, b, x[10], 15, 0xffeff47d); II(b, c, d, a, x[ 1], 21, 0x85845dd1);
        II(a, b, c, d, x[ 8],  6, 0x6fa87e4f); II(d, a, b, c, x[15], 10, 0xfe2ce6e0);
        II(c, d, a, b, x[ 6], 15, 0xa3014314); II(b, c, d, a, x[13], 21, 0x4e0811a1);
        II(a, b, c, d, x[ 4],  6, 0xf7537e82); II(d, a, b, c, x[11], 10, 0xbd3af235);
        II(c, d, a, b, x[ 2], 15, 0x2ad7d2bb); II(b, c, d, a, x[ 9], 21, 0xeb86d391);

        #undef FF
        #undef GG
        #undef HH
        #undef II

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
    }

    static void md5Init(MD5Context* ctx) {
        ctx->count[0] = ctx->count[1] = 0;
        ctx->state[0] = 0x67452301;
        ctx->state[1] = 0xefcdab89;
        ctx->state[2] = 0x98badcfe;
        ctx->state[3] = 0x10325476;
    }

    static void md5Update(MD5Context* ctx, const uint8_t* input, size_t inputLen) {
        size_t index = (ctx->count[0] >> 3) & 0x3F;
        if ((ctx->count[0] += (inputLen << 3)) < (inputLen << 3)) {
            ctx->count[1]++;
        }
        ctx->count[1] += (inputLen >> 29);

        size_t partLen = 64 - index;
        size_t i = 0;
        if (inputLen >= partLen) {
            std::memcpy(&ctx->buffer[index], input, partLen);
            md5Transform(ctx->state, ctx->buffer);
            for (i = partLen; i + 63 < inputLen; i += 64) {
                md5Transform(ctx->state, &input[i]);
            }
            index = 0;
        }
        std::memcpy(&ctx->buffer[index], &input[i], inputLen - i);
    }

    static void md5Finalize(MD5Context* ctx, uint8_t digest[16]) {
        uint8_t bits[8];
        for (int i = 0; i < 8; ++i) {
            bits[i] = static_cast<uint8_t>((ctx->count[i >= 4 ? 1 : 0] >> ((i % 4) * 8)) & 0xFF);
        }

        size_t index = (ctx->count[0] >> 3) & 0x3F;
        size_t padLen = (index < 56) ? (56 - index) : (120 - index);
        static const uint8_t PADDING[64] = { 0x80 };
        md5Update(ctx, PADDING, padLen);
        md5Update(ctx, bits, 8);

        for (int i = 0; i < 4; ++i) {
            digest[i * 4]     = static_cast<uint8_t>((ctx->state[i]      ) & 0xFF);
            digest[i * 4 + 1] = static_cast<uint8_t>((ctx->state[i] >>  8) & 0xFF);
            digest[i * 4 + 2] = static_cast<uint8_t>((ctx->state[i] >> 16) & 0xFF);
            digest[i * 4 + 3] = static_cast<uint8_t>((ctx->state[i] >> 24) & 0xFF);
        }
    }
};

} // namespace RomCloud
