/*
 * segment.c - Segment planning.
 */
#include "config.h"
#include "segment.h"

static size_t round_up(size_t value, size_t multiple)
{
    return (value + multiple - 1) / multiple * multiple;
}

size_t segment_size_for(size_t len, unsigned num_workers)
{
    size_t target;

    if (len <= ENC_SINGLE_SEGMENT_MAX) {
        return len;
    }
    if (num_workers == 0) {
        num_workers = 1;
    }
    target = (len + (size_t)num_workers * ENC_TASKS_PER_WORKER - 1) /
             ((size_t)num_workers * ENC_TASKS_PER_WORKER);
    if (target < ENC_MIN_SEGMENT) {
        target = ENC_MIN_SEGMENT;
    }
    return round_up(target, ENC_CIPHER_BLOCK);
}

size_t segment_count(size_t len, size_t seg_size)
{
    if (len == 0 || seg_size == 0) {
        return 0;
    }
    return (len + seg_size - 1) / seg_size;
}
