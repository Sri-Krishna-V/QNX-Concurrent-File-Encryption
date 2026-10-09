/*
 * util.h - small helpers shared by server, client and tests.
 */
#ifndef ENC_UTIL_H
#define ENC_UTIL_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

/* Monotonic clock in nanoseconds. */
uint64_t now_ns(void);

/* Fill buf with bytes from /dev/urandom. Returns 0 or an errno value. */
int random_bytes(uint8_t *buf, size_t len);

/* Read a key file that holds exactly ENC_KEY_LEN raw bytes. Returns 0 or an errno value. */
int keyfile_load(const char *path, uint8_t key[ENC_KEY_LEN]);

/* Number of CPUs that are online (at least 1). */
unsigned cpu_count(void);

#endif /* ENC_UTIL_H */
