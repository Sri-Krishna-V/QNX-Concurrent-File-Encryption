/*
 * cipher.h - AES-256-CTR encryption that can start at any block-aligned offset.
 *
 * Two backends implement this interface; the Makefile links exactly one:
 *   cipher_openssl.c  - OpenSSL libcrypto (default)
 *   cipher_builtin.c  - bundled AES-256 (make CRYPTO=builtin)
 *
 * In CTR mode decryption is the same operation as encryption.
 */
#ifndef ENC_CIPHER_H
#define ENC_CIPHER_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

/*
 * Encrypt (or decrypt) len bytes that sit at byte_offset inside a Data Block.
 * byte_offset must be a multiple of ENC_CIPHER_BLOCK. Safe to call from many
 * threads at once. Returns 0 on success or an errno value.
 */
int cipher_ctr(const uint8_t key[ENC_KEY_LEN], const uint8_t iv[ENC_IV_LEN],
               uint64_t byte_offset, const uint8_t *in, uint8_t *out, size_t len);

/* Name of the linked backend, for log output. */
const char *cipher_backend_name(void);

/* Check the linked backend against the NIST SP 800-38A F.5.5 vectors. Returns 0 if correct. */
int cipher_self_test(void);

/* Counter block for the given block index: iv + block_index as a 128-bit big-endian add. */
void cipher_counter_at(const uint8_t iv[ENC_IV_LEN], uint64_t block_index,
                       uint8_t counter[ENC_IV_LEN]);

#endif /* ENC_CIPHER_H */
