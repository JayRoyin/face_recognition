#pragma once

#ifndef FACE_RECOGNITION_CORE_SHA256_HPP
#define FACE_RECOGNITION_CORE_SHA256_HPP

/**
 * Compact SHA-256 (FIPS 180-4), header-only.
 *
 * Used to fingerprint an enrolment image so that importing the same photo
 * twice is detectable: a file name can be renamed, an EXIF tag can be
 * rewritten, but the pixel payload of the very same photo is byte-identical.
 * No dependency on OpenSSL, which is not in the build's dependency set.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace face_recognition {
namespace sha256_detail {

inline std::uint32_t ror(std::uint32_t x, std::uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

inline std::uint32_t ch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    return (x & y) ^ (~x & z);
}

inline std::uint32_t maj(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

constexpr std::uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

struct State {
    std::uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                          0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
};

inline void compress(State& s, const std::uint8_t block[64]) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               (static_cast<std::uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3];
    std::uint32_t e = s.h[4], f = s.h[5], g = s.h[6], hh = s.h[7];

    for (int i = 0; i < 64; ++i) {
        const std::uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ch(e, f, g) +
                                 kK[i] + w[i];
        const std::uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + maj(a, b, c);
        hh = g; g = f; f = e;
        e = d + t1;
        d = c; c = b; b = a;
        a = t1 + t2;
    }

    s.h[0] += a; s.h[1] += b; s.h[2] += c; s.h[3] += d;
    s.h[4] += e; s.h[5] += f; s.h[6] += g; s.h[7] += hh;
}

}  // namespace sha256_detail

/** 64-character lower-case hex digest. */
inline std::string sha256Hex(const std::uint8_t* data, std::size_t len) {
    using namespace sha256_detail;

    State s;
    std::size_t i = 0;
    std::uint8_t block[64];

    while (i + 64 <= len) {
        std::memcpy(block, data + i, 64);
        compress(s, block);
        i += 64;
    }

    // Padding: 0x80, zeros, then the 64-bit big-endian bit length.
    std::uint8_t tail[128];
    std::size_t rest = len - i;
    std::memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    std::size_t tail_len = (rest < 56) ? 64 : 128;
    for (std::size_t p = rest + 1; p < tail_len - 8; ++p) tail[p] = 0;

    const std::uint64_t bits = static_cast<std::uint64_t>(len) * 8u;
    for (int b = 0; b < 8; ++b) {
        tail[tail_len - 1 - b] = static_cast<std::uint8_t>((bits >> (8 * b)) & 0xFF);
    }
    for (std::size_t p = 0; p < tail_len; p += 64) {
        std::memcpy(block, tail + p, 64);
        compress(s, block);
    }

    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int j = 0; j < 8; ++j) {
        for (int b = 3; b >= 0; --b) {
            const std::uint8_t byte = static_cast<std::uint8_t>((s.h[j] >> (8 * b)) & 0xFF);
            out.push_back(kHex[(byte >> 4) & 0xF]);
            out.push_back(kHex[byte & 0xF]);
        }
    }
    return out;
}

inline std::string sha256Hex(const std::string& data) {
    return sha256Hex(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

}  // namespace face_recognition

#endif  // FACE_RECOGNITION_CORE_SHA256_HPP
