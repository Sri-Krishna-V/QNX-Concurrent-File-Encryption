/*
 * encfile.h - the Encrypted File: a readable text header, then the encrypted bytes.
 *
 *     QNX-ENC v1
 *     name    photo.png
 *     size    48213 bytes
 *     blocks  1 of up to 2097152 bytes
 *     iv 1    9f3c0a17e2b44d5c81aa06f3d2e9b710
 *     ---
 *     <encrypted bytes: exactly `size` of them>
 *
 * Data Block i (from 1) covers bytes (i-1)*block_size .. of the original file
 * and was encrypted with "iv i". `head -n 6 photo.png.enc` shows the header.
 */
#ifndef ENC_ENCFILE_H
#define ENC_ENCFILE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "config.h"

#define ENCFILE_MAGIC     "QNX-ENC v1"
#define ENCFILE_NAME_MAX  255

typedef struct {
    char name[ENCFILE_NAME_MAX + 1];    /* original file name, without folders */
    uint64_t size;                      /* original file size in bytes */
    uint32_t block_size;                /* Data Block size the file was split into */
    uint32_t blocks;                    /* number of Data Blocks */
    uint8_t (*ivs)[ENC_IV_LEN];         /* one IV per Data Block */
} encfile_header_t;

/*
 * Set up a header for a file of `size` bytes called `path` (folders are
 * dropped from the name). IVs start as zero. Returns 0 or an errno value.
 */
int encfile_header_init(encfile_header_t *h, const char *path, uint64_t size, uint32_t block_size);

void encfile_header_free(encfile_header_t *h);

/* Length of Data Block i (from 0). */
size_t encfile_block_len(const encfile_header_t *h, uint32_t i);

/*
 * Write the header text into buf (if buf is not NULL and cap is big enough).
 * Returns the header length in bytes. The length does not depend on the IVs.
 */
size_t encfile_header_format(const encfile_header_t *h, char *buf, size_t cap);

/*
 * Read and check a header from the start of f. On success f is positioned at
 * the first encrypted byte, *header_len is set, and 0 is returned. Returns
 * EINVAL if f is not an Encrypted File, or an errno value.
 */
int encfile_header_read(FILE *f, encfile_header_t *h, size_t *header_len);

#endif /* ENC_ENCFILE_H */
