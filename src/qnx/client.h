/*
 * client.h - parts of encclient shared between client.c (options, benchmark
 * mode) and client_file.c (encrypting and decrypting files).
 */
#ifndef ENC_CLIENT_H
#define ENC_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "protocol.h"

/*
 * Send one Data Block to the server and wait for its Encrypted Block.
 * Blocks the calling thread only. Returns 0 or an errno value.
 */
int encsvc_encrypt(int coid, const uint8_t *data, uint8_t *encrypted, size_t len,
                   encsvc_reply_t *reply);

typedef struct {
    const uint8_t *key;
    const char *out_path;           /* NULL: choose a name in the current folder */
    unsigned in_flight;             /* Data Blocks sent at once (-j) */
    bool verify;                    /* check each Encrypted Block (-V) */
    bool preview;                   /* show the first bytes before and after (-x) */
} file_options_t;

/* -e: encrypt a file into an Encrypted File. Returns the exit status. */
int encrypt_file(int coid, const char *in_path, const file_options_t *opt);

/* -d: turn an Encrypted File back into the original file, on this client. Returns the exit status. */
int decrypt_file(const char *in_path, const file_options_t *opt);

#endif /* ENC_CLIENT_H */
