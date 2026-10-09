/*
 * test_cipher.c - checks the linked cipher backend.
 *
 *   test_cipher            run the checks
 *   test_cipher -d FILE    also write a fixed 1 MB test encryption to FILE, so
 *                          two backends can be compared byte for byte
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cipher.h"

static int failures;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            failures++;                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                   \
            fputc('\n', stderr);                            \
        }                                                   \
    } while (0)

static void fill_pattern(uint8_t *buf, size_t len, uint32_t seed)
{
    for (size_t i = 0; i < len; i++) {
        seed = seed * 1103515245u + 12345u;
        buf[i] = (uint8_t)(seed >> 16);
    }
}

static void test_counter_arithmetic(void)
{
    uint8_t iv[ENC_IV_LEN], ctr[ENC_IV_LEN], expect[ENC_IV_LEN];

    memset(iv, 0xff, sizeof iv);
    cipher_counter_at(iv, 1, ctr);
    memset(expect, 0, sizeof expect);
    CHECK(memcmp(ctr, expect, sizeof ctr) == 0, "all-ones + 1 must wrap to zero");

    /* Multi-byte add: ...01 00 00 00 00 00 00 f0 + 01 23 45 67 89 ab cd ef */
    memset(iv, 0, sizeof iv);
    iv[8] = 0x01;
    iv[15] = 0xf0;
    cipher_counter_at(iv, 0x0123456789abcdefull, ctr);
    memset(expect, 0, sizeof expect);
    expect[8] = 0x02; expect[9] = 0x23; expect[10] = 0x45; expect[11] = 0x67;
    expect[12] = 0x89; expect[13] = 0xab; expect[14] = 0xce; expect[15] = 0xdf;
    CHECK(memcmp(ctr, expect, sizeof ctr) == 0, "multi-byte add with carry");

    /* Carry out of the low 64 bits into the high 64 bits. */
    memset(iv, 0xff, sizeof iv);
    iv[7] = 0x01;
    cipher_counter_at(iv, 1, ctr);
    memset(expect, 0xff, 8);
    expect[7] = 0x02;
    memset(expect + 8, 0, 8);
    CHECK(memcmp(ctr, expect, sizeof ctr) == 0, "carry into high 64 bits");
}

/* Encrypting in arbitrary block-aligned pieces must equal one whole call. */
static void test_pieces_match_whole(void)
{
    const size_t len = 300000;
    uint8_t key[ENC_KEY_LEN], iv[ENC_IV_LEN];
    uint8_t *in = malloc(len), *whole = malloc(len), *pieces = malloc(len);
    const size_t piece_sizes[] = { 16, 48, 4096, 16384, 65536, 131072 };

    fill_pattern(key, sizeof key, 1);
    memset(iv, 0xff, sizeof iv);    /* counter wraps inside the message */
    iv[0] = 0x7f;
    fill_pattern(in, len, 2);

    CHECK(cipher_ctr(key, iv, 0, in, whole, len) == 0, "whole call");
    for (size_t p = 0; p < sizeof piece_sizes / sizeof piece_sizes[0]; p++) {
        memset(pieces, 0, len);
        for (size_t off = 0; off < len; off += piece_sizes[p]) {
            size_t n = len - off < piece_sizes[p] ? len - off : piece_sizes[p];
            CHECK(cipher_ctr(key, iv, off, in + off, pieces + off, n) == 0, "piece call");
        }
        CHECK(memcmp(whole, pieces, len) == 0, "pieces of %zu bytes differ from whole", piece_sizes[p]);
    }
    free(in);
    free(whole);
    free(pieces);
}

static void test_round_trip_and_odd_lengths(void)
{
    uint8_t key[ENC_KEY_LEN], iv[ENC_IV_LEN], in[1000], enc[1000], dec[1000];

    fill_pattern(key, sizeof key, 3);
    fill_pattern(iv, sizeof iv, 4);
    fill_pattern(in, sizeof in, 5);
    for (size_t len = 0; len <= sizeof in; len += 37) {
        CHECK(cipher_ctr(key, iv, 0, in, enc, len) == 0, "encrypt %zu", len);
        CHECK(cipher_ctr(key, iv, 0, enc, dec, len) == 0, "decrypt %zu", len);
        CHECK(memcmp(in, dec, len) == 0, "round trip of %zu bytes", len);
    }
}

static void test_rejects_unaligned_offset(void)
{
    uint8_t key[ENC_KEY_LEN] = { 0 }, iv[ENC_IV_LEN] = { 0 }, buf[32] = { 0 };

    CHECK(cipher_ctr(key, iv, 8, buf, buf, sizeof buf) != 0, "offset 8 must be rejected");
}

static int dump(const char *path)
{
    const size_t len = 1024 * 1024 + 5;
    uint8_t key[ENC_KEY_LEN], iv[ENC_IV_LEN];
    uint8_t *in = malloc(len), *out = malloc(len);
    FILE *f = fopen(path, "wb");

    fill_pattern(key, sizeof key, 10);
    memset(iv, 0xfe, sizeof iv);
    fill_pattern(in, len, 11);
    if (f == NULL || cipher_ctr(key, iv, 0, in, out, len) != 0 ||
        fwrite(out, 1, len, f) != len) {
        perror(path);
        return 1;
    }
    fclose(f);
    free(in);
    free(out);
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(cipher_self_test() == 0, "NIST SP 800-38A F.5.5 self-test");
    test_counter_arithmetic();
    test_pieces_match_whole();
    test_round_trip_and_odd_lengths();
    test_rejects_unaligned_offset();

    if (argc == 3 && strcmp(argv[1], "-d") == 0 && dump(argv[2]) != 0) {
        failures++;
    }
    printf("test_cipher [%s]: %s\n", cipher_backend_name(), failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
