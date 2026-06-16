#pragma once

#include <stddef.h>
#include <stdint.h>

// Cryptographically-strong kernel CSPRNG backing getentropy()/getrandom().
//
// A ChaCha20 stream generator seeded from the CPU hardware RNG (RDSEED, then
// RDRAND) mixed with cycle-counter / timer / RTC jitter, with a forward-secure
// key ratchet after every request and periodic reseeding. Strong enough for
// TLS key/nonce generation; degrades gracefully (jitter-only) on CPUs with no
// hardware RNG.
void kernel_fill_entropy(void* buffer, size_t length);
