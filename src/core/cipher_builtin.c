/*
 * cipher_builtin.c - AES-256-CTR on top of the bundled aes256.c.
 * Selected with: make CRYPTO=builtin
 */
#include <errno.h>

#include "aes256.h"
#include "cipher.h"

const char *cipher_backend_name(void)
{
    return "builtin";
}

static void counter_increment(uint8_t counter[ENC_IV_LEN])
{
    for (int i = ENC_IV_LEN - 1; i >= 0; i--) {
        if (++counter[i] != 0) {
            break;
        }
    }
}

int cipher_ctr(const uint8_t key[ENC_KEY_LEN], const uint8_t iv[ENC_IV_LEN],
               uint64_t byte_offset, const uint8_t *in, uint8_t *out, size_t len)
{
    aes256_ctx_t ctx;
    uint8_t counter[ENC_IV_LEN];
    uint8_t keystream[ENC_CIPHER_BLOCK];

    if (byte_offset % ENC_CIPHER_BLOCK != 0) {
        return EINVAL;
    }
    aes256_init(&ctx, key);
    cipher_counter_at(iv, byte_offset / ENC_CIPHER_BLOCK, counter);

    while (len > 0) {
        size_t n = len < ENC_CIPHER_BLOCK ? len : ENC_CIPHER_BLOCK;

        aes256_encrypt_block(&ctx, counter, keystream);
        for (size_t i = 0; i < n; i++) {
            out[i] = (uint8_t)(in[i] ^ keystream[i]);
        }
        counter_increment(counter);
        in += n;
        out += n;
        len -= n;
    }
    return 0;
}
