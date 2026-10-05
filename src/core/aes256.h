/*
 * aes256.h - minimal AES-256 block encryption (FIPS-197), used only by the
 * bundled cipher backend. CTR mode needs the forward direction only.
 */
#ifndef ENC_AES256_H
#define ENC_AES256_H

#include <stdint.h>

#define AES256_ROUNDS 14

typedef struct {
    uint8_t round_key[(AES256_ROUNDS + 1) * 16];
} aes256_ctx_t;

void aes256_init(aes256_ctx_t *ctx, const uint8_t key[32]);
void aes256_encrypt_block(const aes256_ctx_t *ctx, const uint8_t in[16], uint8_t out[16]);

#endif /* ENC_AES256_H */
