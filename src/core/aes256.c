/*
 * aes256.c - minimal AES-256 block encryption (FIPS-197).
 *
 * The S-box is computed once at start-up from its mathematical definition
 * (multiplicative inverse in GF(2^8) followed by the affine transform), so
 * there is no hand-typed table to get wrong. This code is a fallback for labs
 * without OpenSSL; it is correct but not hardened against timing attacks.
 */
#include <pthread.h>
#include <string.h>

#include "aes256.h"

static uint8_t sbox[256];
static pthread_once_t sbox_once = PTHREAD_ONCE_INIT;

static uint8_t xtime(uint8_t x)
{
    return (uint8_t)((x << 1) ^ ((x & 0x80u) ? 0x1bu : 0x00u));
}

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    uint8_t product = 0;

    while (b != 0) {
        if (b & 1u) {
            product ^= a;
        }
        a = xtime(a);
        b >>= 1;
    }
    return product;
}

static uint8_t rotl8(uint8_t x, unsigned n)
{
    return (uint8_t)((x << n) | (x >> (8 - n)));
}

static void sbox_build(void)
{
    for (unsigned x = 0; x < 256; x++) {
        /* Inverse is x^254; 0 maps to 0. */
        uint8_t inv = 0;
        if (x != 0) {
            uint8_t power = (uint8_t)x;
            inv = 1;
            for (unsigned e = 254; e != 0; e >>= 1) {
                if (e & 1u) {
                    inv = gf_mul(inv, power);
                }
                power = gf_mul(power, power);
            }
        }
        sbox[x] = (uint8_t)(inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^ rotl8(inv, 3) ^
                            rotl8(inv, 4) ^ 0x63u);
    }
}

void aes256_init(aes256_ctx_t *ctx, const uint8_t key[32])
{
    uint8_t *w = ctx->round_key;
    uint8_t rcon = 0x01;

    pthread_once(&sbox_once, sbox_build);

    memcpy(w, key, 32);
    for (unsigned i = 8; i < 4 * (AES256_ROUNDS + 1); i++) {
        uint8_t t[4];
        memcpy(t, &w[(i - 1) * 4], 4);

        if (i % 8 == 0) {
            /* RotWord, SubWord, Rcon */
            uint8_t first = t[0];
            t[0] = (uint8_t)(sbox[t[1]] ^ rcon);
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[first];
            rcon = xtime(rcon);
        } else if (i % 8 == 4) {
            for (unsigned k = 0; k < 4; k++) {
                t[k] = sbox[t[k]];
            }
        }
        for (unsigned k = 0; k < 4; k++) {
            w[i * 4 + k] = (uint8_t)(w[(i - 8) * 4 + k] ^ t[k]);
        }
    }
}

static void add_round_key(uint8_t s[16], const uint8_t *rk)
{
    for (unsigned i = 0; i < 16; i++) {
        s[i] ^= rk[i];
    }
}

/* Combined SubBytes + ShiftRows. State is column-major: s[row + 4 * col]. */
static void sub_shift(uint8_t s[16])
{
    uint8_t t[16];

    for (unsigned c = 0; c < 4; c++) {
        for (unsigned r = 0; r < 4; r++) {
            t[r + 4 * c] = sbox[s[r + 4 * ((c + r) % 4)]];
        }
    }
    memcpy(s, t, 16);
}

static void mix_columns(uint8_t s[16])
{
    for (unsigned c = 0; c < 4; c++) {
        uint8_t *col = &s[4 * c];
        uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);

        col[0] = (uint8_t)(a0 ^ all ^ xtime((uint8_t)(a0 ^ a1)));
        col[1] = (uint8_t)(a1 ^ all ^ xtime((uint8_t)(a1 ^ a2)));
        col[2] = (uint8_t)(a2 ^ all ^ xtime((uint8_t)(a2 ^ a3)));
        col[3] = (uint8_t)(a3 ^ all ^ xtime((uint8_t)(a3 ^ a0)));
    }
}

void aes256_encrypt_block(const aes256_ctx_t *ctx, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];

    memcpy(s, in, 16);
    add_round_key(s, ctx->round_key);
    for (unsigned round = 1; round < AES256_ROUNDS; round++) {
        sub_shift(s);
        mix_columns(s);
        add_round_key(s, ctx->round_key + 16 * round);
    }
    sub_shift(s);
    add_round_key(s, ctx->round_key + 16 * AES256_ROUNDS);
    memcpy(out, s, 16);
}
