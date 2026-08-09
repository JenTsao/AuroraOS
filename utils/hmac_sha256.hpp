// =============================================================================
// utils/hmac_sha256.hpp — Self-contained HMAC-SHA256 for bare-metal
//
// No external dependencies. ~2 KB Flash footprint.
// Implements FIPS 180-4 SHA-256 + RFC 2104 HMAC.
//
// Usage:
//   HmacSha256 ctx;
//   ctx.init(key, key_len);
//   ctx.update(data, data_len);
//   uint8_t digest[32];
//   ctx.finish(digest);
//
// For a one-shot call:
//   HmacSha256::compute(key, key_len, msg, msg_len, digest);
// =============================================================================

#ifndef AURORA_HMAC_SHA256_HPP
#define AURORA_HMAC_SHA256_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>  // memcpy, memset

// ─────────────────────────────────────────────────────────────────────────────
// SHA-256 Core (FIPS 180-4)
// ─────────────────────────────────────────────────────────────────────────────
class Sha256 {
public:
    static constexpr size_t DIGEST_SIZE  = 32;
    static constexpr size_t BLOCK_SIZE   = 64;

    void init() noexcept {
        state_[0] = 0x6a09e667u;
        state_[1] = 0xbb67ae85u;
        state_[2] = 0x3c6ef372u;
        state_[3] = 0xa54ff53au;
        state_[4] = 0x510e527fu;
        state_[5] = 0x9b05688cu;
        state_[6] = 0x1f83d9abu;
        state_[7] = 0x5be0cd19u;
        bit_count_ = 0;
        buf_used_  = 0;
    }

    void update(const uint8_t* data, size_t len) noexcept {
        bit_count_ += static_cast<uint64_t>(len) * 8u;
        while (len > 0) {
            const size_t space = BLOCK_SIZE - buf_used_;
            const size_t copy  = len < space ? len : space;
            memcpy(buf_ + buf_used_, data, copy);
            buf_used_ += copy;
            data      += copy;
            len       -= copy;
            if (buf_used_ == BLOCK_SIZE) {
                process_block(buf_);
                buf_used_ = 0;
            }
        }
    }

    void finish(uint8_t digest[DIGEST_SIZE]) noexcept {
        // Padding
        buf_[buf_used_++] = 0x80u;
        if (buf_used_ > 56u) {
            // Need extra block
            memset(buf_ + buf_used_, 0, BLOCK_SIZE - buf_used_);
            process_block(buf_);
            buf_used_ = 0;
        }
        memset(buf_ + buf_used_, 0, 56u - buf_used_);
        // Length field (big-endian, 64-bit)
        for (int i = 7; i >= 0; i--) {
            buf_[56u + (7u - static_cast<unsigned>(i))] =
                static_cast<uint8_t>(bit_count_ >> (i * 8));
        }
        process_block(buf_);

        // Output (big-endian)
        for (int i = 0; i < 8; i++) {
            digest[i*4+0] = static_cast<uint8_t>(state_[i] >> 24);
            digest[i*4+1] = static_cast<uint8_t>(state_[i] >> 16);
            digest[i*4+2] = static_cast<uint8_t>(state_[i] >>  8);
            digest[i*4+3] = static_cast<uint8_t>(state_[i]      );
        }
        // Zeroize
        memset(state_, 0, sizeof(state_));
        memset(buf_,   0, sizeof(buf_));
        bit_count_ = 0;
        buf_used_  = 0;
    }

private:
    uint32_t state_[8]{};
    uint8_t  buf_[BLOCK_SIZE]{};
    uint64_t bit_count_ = 0;
    size_t   buf_used_  = 0;

    static inline constexpr uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };

    static constexpr uint32_t rotr(uint32_t x, int n) noexcept {
        return (x >> n) | (x << (32 - n));
    }
    static constexpr uint32_t ch (uint32_t x, uint32_t y, uint32_t z) noexcept {
        return (x & y) ^ (~x & z);
    }
    static constexpr uint32_t maj(uint32_t x, uint32_t y, uint32_t z) noexcept {
        return (x & y) ^ (x & z) ^ (y & z);
    }
    static constexpr uint32_t sig0(uint32_t x) noexcept {
        return rotr(x,  2) ^ rotr(x, 13) ^ rotr(x, 22);
    }
    static constexpr uint32_t sig1(uint32_t x) noexcept {
        return rotr(x,  6) ^ rotr(x, 11) ^ rotr(x, 25);
    }
    static constexpr uint32_t gam0(uint32_t x) noexcept {
        return rotr(x,  7) ^ rotr(x, 18) ^ (x >>  3);
    }
    static constexpr uint32_t gam1(uint32_t x) noexcept {
        return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
    }

    void process_block(const uint8_t blk[BLOCK_SIZE]) noexcept {
        uint32_t W[64];
        // Prepare message schedule
        for (int i = 0; i < 16; i++) {
            W[i] = (static_cast<uint32_t>(blk[i*4])   << 24)
                 | (static_cast<uint32_t>(blk[i*4+1]) << 16)
                 | (static_cast<uint32_t>(blk[i*4+2]) <<  8)
                 | (static_cast<uint32_t>(blk[i*4+3])      );
        }
        for (int i = 16; i < 64; i++) {
            W[i] = gam1(W[i-2]) + W[i-7] + gam0(W[i-15]) + W[i-16];
        }
        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

        for (int i = 0; i < 64; i++) {
            const uint32_t T1 = h + sig1(e) + ch(e,f,g) + K[i] + W[i];
            const uint32_t T2 = sig0(a) + maj(a,b,c);
            h = g; g = f; f = e; e = d + T1;
            d = c; c = b; b = a; a = T1 + T2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;

        // Zeroize W on stack
        memset(W, 0, sizeof(W));
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// HMAC-SHA256 (RFC 2104)
// ─────────────────────────────────────────────────────────────────────────────
class HmacSha256 {
public:
    static constexpr size_t DIGEST_SIZE = Sha256::DIGEST_SIZE;  // 32
    static constexpr size_t BLOCK_SIZE  = Sha256::BLOCK_SIZE;   // 64

    // Prepare HMAC context with the given key.
    void init(const uint8_t* key, size_t key_len) noexcept {
        uint8_t k_pad[BLOCK_SIZE]{};

        if (key_len > BLOCK_SIZE) {
            // If key is longer than block size, hash it first
            Sha256 kh;
            kh.init();
            kh.update(key, key_len);
            kh.finish(k_pad);
        } else {
            memcpy(k_pad, key, key_len);
        }

        // Derive inner and outer padding
        uint8_t i_pad[BLOCK_SIZE];
        uint8_t o_pad[BLOCK_SIZE];
        for (size_t i = 0; i < BLOCK_SIZE; i++) {
            i_pad[i] = k_pad[i] ^ 0x36u;
            o_pad[i] = k_pad[i] ^ 0x5Cu;
        }
        memcpy(o_pad_, o_pad, BLOCK_SIZE);

        // Start inner hash: H(i_pad || message)
        inner_.init();
        inner_.update(i_pad, BLOCK_SIZE);

        // Zeroize sensitive stack data
        memset(k_pad, 0, BLOCK_SIZE);
        memset(i_pad, 0, BLOCK_SIZE);
        memset(o_pad, 0, BLOCK_SIZE);
    }

    void update(const uint8_t* data, size_t len) noexcept {
        inner_.update(data, len);
    }

    void finish(uint8_t digest[DIGEST_SIZE]) noexcept {
        // inner_digest = H(i_pad || message)
        uint8_t inner_digest[DIGEST_SIZE]{};
        inner_.finish(inner_digest);

        // outer = H(o_pad || inner_digest)
        Sha256 outer;
        outer.init();
        outer.update(o_pad_, BLOCK_SIZE);
        outer.update(inner_digest, DIGEST_SIZE);
        outer.finish(digest);

        // Zeroize
        memset(inner_digest, 0, DIGEST_SIZE);
        memset(o_pad_,       0, BLOCK_SIZE);
    }

    // ── One-shot convenience function ────────────────────────────────────────
    static void compute(const uint8_t* key,  size_t key_len,
                        const uint8_t* msg,  size_t msg_len,
                        uint8_t        digest[DIGEST_SIZE]) noexcept {
        HmacSha256 ctx;
        ctx.init(key, key_len);
        ctx.update(msg, msg_len);
        ctx.finish(digest);
    }

    // ── Constant-time comparison (prevent timing side-channel) ───────────────
    static bool constant_time_eq(const uint8_t* a,
                                  const uint8_t* b,
                                  size_t len) noexcept {
        uint8_t diff = 0;
        for (size_t i = 0; i < len; i++) diff |= a[i] ^ b[i];
        return diff == 0;
    }

private:
    Sha256  inner_{};
    uint8_t o_pad_[BLOCK_SIZE]{};
};

// ─────────────────────────────────────────────────────────────────────────────
// Simple CRC32 (ISO 3309 polynomial) — used for MPU sandbox descriptor
// ─────────────────────────────────────────────────────────────────────────────
namespace Crc32 {
    inline uint32_t compute(const uint8_t* data, size_t len) noexcept {
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < len; i++) {
            crc ^= static_cast<uint32_t>(data[i]);
            for (int j = 0; j < 8; j++) {
                crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
            }
        }
        return crc ^ 0xFFFFFFFFu;
    }

    inline uint32_t of_word(uint32_t w) noexcept {
        return compute(reinterpret_cast<const uint8_t*>(&w), sizeof(w));
    }

    inline uint32_t of_uintptr(uintptr_t v) noexcept {
        return compute(reinterpret_cast<const uint8_t*>(&v), sizeof(v));
    }
}  // namespace Crc32

#endif  // AURORA_HMAC_SHA256_HPP
