#include <common/krandom.hpp>
#include <common/ports.hpp>
#include <interrupts/timer.hpp>
#include <time/time.hpp>
#include <cpuid.h>

// Cryptographically-strong kernel CSPRNG.
//
// Design:
//   - Seed material is gathered from the CPU hardware RNG (RDSEED preferred,
//     RDRAND fallback) when available, mixed with TSC / millisecond timer /
//     RTC unix time so we still get a non-repeating stream on CPUs without a
//     hardware RNG.
//   - The seed (key + nonce) drives a ChaCha20 stream cipher used as the
//     output generator. After producing output we "ratchet" the key forward
//     with fresh keystream so past output cannot be recovered from the current
//     state (forward secrecy), and we periodically fold in fresh hardware
//     entropy so the generator self-heals.
//
// This is the entropy source behind getentropy()/getrandom(), which mbedTLS
// (and therefore TLS key/nonce generation) depends on.

namespace {

// ---- CPU hardware RNG detection ------------------------------------------

bool g_probed = false;
bool g_has_rdrand = false;
bool g_has_rdseed = false;

void probe_hw_rng() {
    if (g_probed) return;
    g_probed = true;

    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        g_has_rdrand = (ecx & (1u << 30)) != 0;  // CPUID.01H:ECX.RDRAND[bit 30]
    }
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        g_has_rdseed = (ebx & (1u << 18)) != 0;  // CPUID.07H:EBX.RDSEED[bit 18]
    }
}

// RDRAND/RDSEED can transiently fail (CF=0); retry a bounded number of times.
bool rdrand64(uint64_t& out) {
    for (int i = 0; i < 32; ++i) {
        uint64_t v = 0;
        unsigned char ok = 0;
        asm volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok)::"cc");
        if (ok) { out = v; return true; }
    }
    return false;
}

bool rdseed64(uint64_t& out) {
    for (int i = 0; i < 64; ++i) {
        uint64_t v = 0;
        unsigned char ok = 0;
        asm volatile("rdseed %0; setc %1" : "=r"(v), "=qm"(ok)::"cc");
        if (ok) { out = v; return true; }
        asm volatile("pause");
    }
    return false;
}

// Best hardware entropy word available, or false if none.
bool hw_entropy64(uint64_t& out) {
    probe_hw_rng();
    if (g_has_rdseed && rdseed64(out)) return true;
    if (g_has_rdrand && rdrand64(out)) return true;
    return false;
}

// Non-hardware fallback word: cycle counter + timers folded through a
// SplitMix64 step. Not cryptographic on its own but adds unpredictable timing
// jitter to the pool when no HW RNG exists.
uint64_t splitmix64(uint64_t& s) {
    s += 0x9E3779B97F4A7C15ULL;
    uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

uint64_t g_jitter_state = 0x9E3779B97F4A7C15ULL;

uint64_t jitter_entropy64() {
    const uint64_t tsc = rdtsc();
    const uint64_t ms = Timer::get().getMilliseconds();
    const uint64_t unixt = time_get_unix();
    g_jitter_state ^= tsc;
    g_jitter_state = (g_jitter_state << 13) | (g_jitter_state >> 51);
    g_jitter_state ^= ms * 0x100000001B3ULL;
    g_jitter_state ^= unixt * 0xD6E8FEB86659FD93ULL;
    return splitmix64(g_jitter_state) ^ rdtsc();
}

// Gather a 64-bit entropy word: prefer hardware, always XOR in jitter so the
// stream is unique even on QEMU CPUs that expose a deterministic RDRAND.
uint64_t gather64() {
    uint64_t hw = 0;
    uint64_t e = jitter_entropy64();
    if (hw_entropy64(hw)) e ^= hw;
    return e;
}

// ---- ChaCha20 ------------------------------------------------------------

inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

#define QR(a, b, c, d)                 \
    a += b; d ^= a; d = rotl32(d, 16); \
    c += d; b ^= c; b = rotl32(b, 12); \
    a += b; d ^= a; d = rotl32(d, 8);  \
    c += d; b ^= c; b = rotl32(b, 7);

void chacha20_block(const uint32_t in[16], uint32_t out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = in[i];
    for (int i = 0; i < 10; ++i) {  // 20 rounds = 10 double-rounds
        QR(out[0], out[4], out[8], out[12]);
        QR(out[1], out[5], out[9], out[13]);
        QR(out[2], out[6], out[10], out[14]);
        QR(out[3], out[7], out[11], out[15]);
        QR(out[0], out[5], out[10], out[15]);
        QR(out[1], out[6], out[11], out[12]);
        QR(out[2], out[7], out[8], out[13]);
        QR(out[3], out[4], out[9], out[14]);
    }
    for (int i = 0; i < 16; ++i) out[i] += in[i];
}

#undef QR

// CSPRNG state: the ChaCha20 input block.
//   words 0..3   constant ("expand 32-byte k")
//   words 4..11  256-bit key
//   word  12     block counter
//   words 13..15 96-bit nonce
struct ChaChaRng {
    uint32_t state[16];
    bool seeded;
    uint64_t bytes_since_reseed;
};

ChaChaRng g_rng = {};

void put64(uint32_t* dst, uint64_t v) {
    dst[0] = static_cast<uint32_t>(v & 0xFFFFFFFFu);
    dst[1] = static_cast<uint32_t>(v >> 32);
}

void reseed() {
    // Constant: "expand 32-byte k"
    g_rng.state[0] = 0x61707865;
    g_rng.state[1] = 0x3320646e;
    g_rng.state[2] = 0x79622d32;
    g_rng.state[3] = 0x6b206574;

    // 256-bit key from 4 entropy words; if already seeded, mix into the
    // existing key rather than replacing (self-healing without discarding
    // accumulated state).
    for (int i = 0; i < 4; ++i) {
        uint64_t w = gather64();
        if (g_rng.seeded) {
            g_rng.state[4 + i * 2] ^= static_cast<uint32_t>(w & 0xFFFFFFFFu);
            g_rng.state[5 + i * 2] ^= static_cast<uint32_t>(w >> 32);
        } else {
            put64(&g_rng.state[4 + i * 2], w);
        }
    }

    g_rng.state[12] = 0;  // counter
    uint64_t nonce = gather64();
    g_rng.state[13] = static_cast<uint32_t>(nonce & 0xFFFFFFFFu);
    g_rng.state[14] = static_cast<uint32_t>(nonce >> 32);
    g_rng.state[15] ^= static_cast<uint32_t>(gather64() & 0xFFFFFFFFu);

    g_rng.seeded = true;
    g_rng.bytes_since_reseed = 0;
}

// Reseed at least this often so the generator recovers from any state
// compromise and folds in fresh hardware entropy.
constexpr uint64_t kReseedInterval = 1u << 20;  // 1 MiB

void rng_generate(uint8_t* out, size_t length) {
    if (!g_rng.seeded || g_rng.bytes_since_reseed >= kReseedInterval) {
        reseed();
    }

    uint32_t block[16];
    size_t produced = 0;
    while (produced < length) {
        chacha20_block(g_rng.state, block);

        // advance counter (and nonce on wrap) so each block differs
        if (++g_rng.state[12] == 0) {
            ++g_rng.state[13];
        }

        const size_t take = (length - produced) < 64 ? (length - produced) : 64;
        const uint8_t* kb = reinterpret_cast<const uint8_t*>(block);
        for (size_t i = 0; i < take; ++i) {
            out[produced + i] = kb[i];
        }
        produced += take;
    }

    g_rng.bytes_since_reseed += length;

    // Ratchet: overwrite the key with fresh keystream so the bytes we just
    // returned cannot be reproduced from the current state (forward secrecy).
    chacha20_block(g_rng.state, block);
    if (++g_rng.state[12] == 0) ++g_rng.state[13];
    for (int i = 0; i < 8; ++i) {
        g_rng.state[4 + i] = block[i];
    }
}

}  // namespace

void kernel_fill_entropy(void* buffer, size_t length) {
    if (length == 0) return;
    rng_generate(static_cast<uint8_t*>(buffer), length);
}
