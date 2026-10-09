/*
 * cipher_openssl.c - AES-256-CTR through the OpenSSL EVP interface.
 *
 * Each call uses its own EVP_CIPHER_CTX, so worker threads never share
 * cipher state. The key and IV are only read.
 */
#include <errno.h>
#include <limits.h>

#include <openssl/evp.h>

#include "cipher.h"

const char *cipher_backend_name(void)
{
    return "openssl";
}

int cipher_ctr(const uint8_t key[ENC_KEY_LEN], const uint8_t iv[ENC_IV_LEN],
               uint64_t byte_offset, const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t counter[ENC_IV_LEN];
    EVP_CIPHER_CTX *ctx;
    int outl;
    int err = 0;

    if (byte_offset % ENC_CIPHER_BLOCK != 0) {
        return EINVAL;
    }
    cipher_counter_at(iv, byte_offset / ENC_CIPHER_BLOCK, counter);

    ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL) {
        return ENOMEM;
    }
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), NULL, key, counter) != 1) {
        err = EIO;
        goto done;
    }

    /* EVP_EncryptUpdate takes an int length, so feed large inputs in pieces. */
    while (len > 0) {
        int piece = len > (size_t)(INT_MAX / 2) ? INT_MAX / 2 : (int)len;
        if (EVP_EncryptUpdate(ctx, out, &outl, in, piece) != 1 || outl != piece) {
            err = EIO;
            goto done;
        }
        in += piece;
        out += piece;
        len -= (size_t)piece;
    }
    if (EVP_EncryptFinal_ex(ctx, out, &outl) != 1) {
        err = EIO;
    }

done:
    EVP_CIPHER_CTX_free(ctx);
    return err;
}
