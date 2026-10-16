/*
 * encfile.c - write and read the readable header of an Encrypted File.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "encfile.h"

/* Upper bound on Data Blocks in one file, so a damaged header can't ask for huge memory. */
#define ENCFILE_MAX_BLOCKS 1000000u

/* Longest header line we accept (the name line is the longest). */
#define ENCFILE_LINE_MAX (ENCFILE_NAME_MAX + 32)

static void set_name(encfile_header_t *h, const char *path)
{
    const char *base = strrchr(path, '/');
    size_t n;

    base = base != NULL ? base + 1 : path;
    n = strlen(base);
    if (n > ENCFILE_NAME_MAX) {
        n = ENCFILE_NAME_MAX;
    }
    memcpy(h->name, base, n);
    h->name[n] = '\0';
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)h->name[i] < 0x20) {
            h->name[i] = '_';           /* keep the header one name per line */
        }
    }
    if (n == 0 || strcmp(h->name, ".") == 0 || strcmp(h->name, "..") == 0) {
        strcpy(h->name, "file");
    }
}

int encfile_header_init(encfile_header_t *h, const char *path, uint64_t size, uint32_t block_size)
{
    uint64_t blocks;

    memset(h, 0, sizeof *h);
    if (block_size == 0) {
        return EINVAL;
    }
    blocks = (size + block_size - 1) / block_size;
    if (blocks > ENCFILE_MAX_BLOCKS) {
        return EFBIG;
    }
    set_name(h, path);
    h->size = size;
    h->block_size = block_size;
    h->blocks = (uint32_t)blocks;
    h->ivs = calloc(blocks > 0 ? blocks : 1, sizeof *h->ivs);
    return h->ivs != NULL ? 0 : ENOMEM;
}

void encfile_header_free(encfile_header_t *h)
{
    free(h->ivs);
    h->ivs = NULL;
}

size_t encfile_block_len(const encfile_header_t *h, uint32_t i)
{
    uint64_t start = (uint64_t)i * h->block_size;
    uint64_t left = h->size > start ? h->size - start : 0;

    return left < h->block_size ? (size_t)left : h->block_size;
}

/* snprintf that keeps counting once the buffer is full, so it can also measure. */
static size_t put(char *buf, size_t cap, size_t at, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf != NULL && at < cap ? buf + at : NULL, buf != NULL && at < cap ? cap - at : 0,
                  fmt, ap);
    va_end(ap);
    return n > 0 ? (size_t)n : 0;
}

static const char hex_digits[] = "0123456789abcdef";

size_t encfile_header_format(const encfile_header_t *h, char *buf, size_t cap)
{
    size_t len = 0;

    len += put(buf, cap, len, "%s\n", ENCFILE_MAGIC);
    len += put(buf, cap, len, "name    %s\n", h->name);
    len += put(buf, cap, len, "size    %" PRIu64 " bytes\n", h->size);
    len += put(buf, cap, len, "blocks  %" PRIu32 " of up to %" PRIu32 " bytes\n",
               h->blocks, h->block_size);
    for (uint32_t i = 0; i < h->blocks; i++) {
        char hex[2 * ENC_IV_LEN + 1];

        for (unsigned b = 0; b < ENC_IV_LEN; b++) {
            hex[2 * b] = hex_digits[h->ivs[i][b] >> 4];
            hex[2 * b + 1] = hex_digits[h->ivs[i][b] & 0x0f];
        }
        hex[2 * ENC_IV_LEN] = '\0';
        /* "iv " plus the index padded to 4 columns: the same 7-column label as before. */
        len += put(buf, cap, len, "iv %-4" PRIu32 " %s\n", i + 1, hex);
    }
    len += put(buf, cap, len, "---\n");
    return len;
}

/* Read one line without its '\n'. Returns 0, or EINVAL if missing or too long. */
static int read_line(FILE *f, char *line, size_t cap)
{
    size_t n;

    if (fgets(line, (int)cap, f) == NULL) {
        return EINVAL;
    }
    n = strlen(line);
    if (n == 0 || line[n - 1] != '\n') {
        return EINVAL;
    }
    line[n - 1] = '\0';
    return 0;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_iv(const char *hex, uint8_t iv[ENC_IV_LEN])
{
    if (strlen(hex) != 2 * ENC_IV_LEN) {
        return EINVAL;
    }
    for (unsigned b = 0; b < ENC_IV_LEN; b++) {
        int hi = hex_value(hex[2 * b]), lo = hex_value(hex[2 * b + 1]);
        if (hi < 0 || lo < 0) {
            return EINVAL;
        }
        iv[b] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

int encfile_header_read(FILE *f, encfile_header_t *h, size_t *header_len)
{
    char line[ENCFILE_LINE_MAX + 2], hex[2 * ENC_IV_LEN + 2], tail[8];
    unsigned long long size;
    unsigned blocks, block_size, index;
    const char *name;
    long pos;
    int err;

    memset(h, 0, sizeof *h);
    if (read_line(f, line, sizeof line) != 0 || strcmp(line, ENCFILE_MAGIC) != 0) {
        return EINVAL;
    }

    if (read_line(f, line, sizeof line) != 0 || strncmp(line, "name ", 5) != 0) {
        return EINVAL;
    }
    name = line + 5;
    while (*name == ' ') {
        name++;
    }
    if (*name == '\0' || strlen(name) > ENCFILE_NAME_MAX || strchr(name, '/') != NULL ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return EINVAL;                  /* never let a header choose another folder */
    }
    strcpy(h->name, name);

    if (read_line(f, line, sizeof line) != 0 ||
        sscanf(line, "size %llu bytes%1s", &size, tail) != 1) {
        return EINVAL;
    }
    if (read_line(f, line, sizeof line) != 0 ||
        sscanf(line, "blocks %u of up to %u bytes%1s", &blocks, &block_size, tail) != 2 ||
        block_size == 0 || blocks != (size + block_size - 1) / block_size ||
        blocks > ENCFILE_MAX_BLOCKS) {
        return EINVAL;
    }

    {
        char name_copy[ENCFILE_NAME_MAX + 1];
        strcpy(name_copy, h->name);
        err = encfile_header_init(h, name_copy, size, block_size);
        if (err != 0) {
            return err;
        }
    }

    for (uint32_t i = 0; i < h->blocks; i++) {
        if (read_line(f, line, sizeof line) != 0 ||
            sscanf(line, "iv %u %33s%1s", &index, hex, tail) != 2 || index != i + 1 ||
            parse_iv(hex, h->ivs[i]) != 0) {
            encfile_header_free(h);
            return EINVAL;
        }
    }
    if (read_line(f, line, sizeof line) != 0 || strcmp(line, "---") != 0) {
        encfile_header_free(h);
        return EINVAL;
    }

    pos = ftell(f);
    if (pos < 0) {
        err = errno != 0 ? errno : EIO;
        encfile_header_free(h);
        return err;
    }
    *header_len = (size_t)pos;
    return 0;
}
