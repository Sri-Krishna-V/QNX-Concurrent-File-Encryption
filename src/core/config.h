/*
 * config.h - limits and tuning constants shared by server, client and tests.
 */
#ifndef ENC_CONFIG_H
#define ENC_CONFIG_H

/* Largest Data Block the server accepts. Raise this to benchmark bigger blocks. */
#define ENC_MAX_BLOCK           (2u * 1024u * 1024u)

/* A Data Block of this size or smaller is encrypted as a single Segment. */
#define ENC_SINGLE_SEGMENT_MAX  (64u * 1024u)

/* Smallest Segment produced when a Data Block is split. */
#define ENC_MIN_SEGMENT         (16u * 1024u)

/* Segments created per worker, so that fast workers can pick up extra Tasks. */
#define ENC_TASKS_PER_WORKER    4u

/* Most Jobs in progress at once; further clients wait send-blocked. */
#define ENC_MAX_JOBS            8u

/* AES-256-CTR parameters. */
#define ENC_CIPHER_BLOCK        16u
#define ENC_KEY_LEN             32u
#define ENC_IV_LEN              16u

#endif /* ENC_CONFIG_H */
