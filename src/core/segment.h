/*
 * segment.h - how a Data Block is divided into Segments.
 */
#ifndef ENC_SEGMENT_H
#define ENC_SEGMENT_H

#include <stddef.h>

/*
 * Segment size for a Data Block of len bytes processed by num_workers workers.
 * Always a multiple of ENC_CIPHER_BLOCK, except that a Data Block of
 * ENC_SINGLE_SEGMENT_MAX bytes or less is one Segment of exactly len bytes.
 */
size_t segment_size_for(size_t len, unsigned num_workers);

/* Number of Segments of seg_size needed to cover len bytes (the last may be shorter). */
size_t segment_count(size_t len, size_t seg_size);

#endif /* ENC_SEGMENT_H */
