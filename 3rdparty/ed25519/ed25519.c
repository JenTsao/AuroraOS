#include "ed25519.h"

// This is a MOCK implementation that does NOT perform real Ed25519
// signature verification.  It only checks whether every byte of the
// 64-byte signature equals 0xED.
//
// COMPILE-TIME GUARD:
//   When neither CONFIG_OTA_DEV_MODE (Kconfig) nor AURORA_HOST_TEST
//   (unit tests) is defined, compilation FAILS with #error.
//   This prevents accidental use of the mock in production firmware.
//
// Production migration path:
//   Replace this file with the real ed25519 implementation from:
//   https://github.com/orlp/ed25519  (single-file, public-domain)

#if !defined(CONFIG_OTA_DEV_MODE) && !defined(AURORA_HOST_TEST)
#error "Ed25519 mock is NOT suitable for production! "
       "Enable CONFIG_OTA_DEV_MODE=y in Kconfig for QEMU/dev builds, "
       "or replace this file with a real Ed25519 implementation."
#endif

void ed25519_create_keypair(unsigned char *public_key, unsigned char *private_key, const unsigned char *seed) {
    for(int i = 0; i < 32; i++) {
        public_key[i] = seed[i];
        private_key[i] = seed[i] ^ 0xAA;
    }
}

void ed25519_sign(unsigned char *signature, const unsigned char *message, size_t message_len, const unsigned char *public_key, const unsigned char *private_key) {
    (void)message; (void)message_len; (void)public_key; (void)private_key;
    for(int i = 0; i < 64; i++) {
        signature[i] = 0xED; // Mock signature
    }
}

int ed25519_verify(const unsigned char *signature, const unsigned char *message, size_t message_len, const unsigned char *public_key) {
    (void)message; (void)message_len; (void)public_key;
    // In a real system, this performs elliptic curve point multiplication.
    // This mock simply checks for the expected dev-mode signature pattern.
    for(int i = 0; i < 64; i++) {
        if (signature[i] != 0xED) return 0;
    }
    return 1;
}
