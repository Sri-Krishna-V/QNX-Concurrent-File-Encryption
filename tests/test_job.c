/*
 * test_job.c - checks Segment planning, the Worker Pool and Job completion.
 *
 * Every Job result is compared with the Reference Encryption.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cipher.h"
#include "config.h"
#include "job.h"
#include "segment.h"
#include "worker_pool.h"

static int failures;
static pthread_mutex_t failures_mutex = PTHREAD_MUTEX_INITIALIZER;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            pthread_mutex_lock(&failures_mutex);            \
            failures++;                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                   \
            fputc('\n', stderr);                            \
            pthread_mutex_unlock(&failures_mutex);          \
        }                                                   \
    } while (0)

static void fill_pattern(uint8_t *buf, size_t len, uint32_t seed)
{
    for (size_t i = 0; i < len; i++) {
        seed = seed * 1103515245u + 12345u;
        buf[i] = (uint8_t)(seed >> 16);
    }
}

static void test_segment_plan(void)
{
    const size_t lens[] = { 1, 15, 16, 17, 65535, 65536, 65537, 100000,
                            1048576 + 7, ENC_MAX_BLOCK };
    const unsigned workers[] = { 1, 2, 3, 4, 8, 64 };

    for (size_t i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        for (size_t w = 0; w < sizeof workers / sizeof workers[0]; w++) {
            size_t len = lens[i];
            size_t seg = segment_size_for(len, workers[w]);
            size_t count = segment_count(len, seg);

            if (len <= ENC_SINGLE_SEGMENT_MAX) {
                CHECK(count == 1 && seg == len, "len %zu must be one Segment", len);
            } else {
                CHECK(seg % ENC_CIPHER_BLOCK == 0, "segment size %zu not block aligned", seg);
                CHECK(seg >= ENC_MIN_SEGMENT, "segment size %zu below minimum", seg);
                CHECK(count >= 2, "len %zu should split", len);
            }
            CHECK(count * seg >= len && (count - 1) * seg < len,
                  "segments do not exactly cover len %zu", len);
        }
    }
    /* 2 MB on the Pi 4B's 4 cores: 16 Segments of 128 KB. */
    CHECK(segment_size_for(ENC_MAX_BLOCK, 4) == 128 * 1024, "2 MB / 4 workers");
    CHECK(segment_count(0, 16) == 0, "empty Data Block has no Segments");
}

/* Run one Job and compare it with the Reference Encryption. */
static void check_one_job(worker_pool_t *pool, size_t len, uint32_t seed)
{
    uint8_t key[ENC_KEY_LEN], iv[ENC_IV_LEN];
    uint8_t *in = malloc(len), *out = malloc(len), *ref = malloc(len);
    job_t job;

    fill_pattern(key, sizeof key, seed);
    fill_pattern(iv, sizeof iv, seed + 1);
    fill_pattern(in, len, seed + 2);
    memset(out, 0, len);

    CHECK(job_init(&job, key, iv, in, out, len) == 0, "job_init");
    CHECK(job_run(&job, pool) == 0, "job_run len %zu", len);
    CHECK(reference_encrypt(key, iv, in, ref, len) == 0, "reference_encrypt");
    CHECK(memcmp(out, ref, len) == 0, "Job differs from Reference Encryption (len %zu, %u workers)",
          len, pool_size(pool));
    CHECK(job.completed_count == job.total_segments, "completed_count");
    job_destroy(&job);

    free(in);
    free(out);
    free(ref);
}

static void test_jobs_match_reference(void)
{
    const size_t lens[] = { 1, 17, 65536, 65537, 100003, 1048576 + 7, ENC_MAX_BLOCK };
    const unsigned workers[] = { 1, 2, 3, 4, 8 };

    for (size_t w = 0; w < sizeof workers / sizeof workers[0]; w++) {
        worker_pool_t *pool = pool_create(workers[w]);
        CHECK(pool != NULL, "pool_create(%u)", workers[w]);
        for (size_t i = 0; i < sizeof lens / sizeof lens[0]; i++) {
            check_one_job(pool, lens[i], (uint32_t)(i * 31 + w));
        }
        pool_destroy(pool);
    }
}

static void test_empty_job_rejected(void)
{
    worker_pool_t *pool = pool_create(2);
    uint8_t key[ENC_KEY_LEN] = { 0 }, iv[ENC_IV_LEN] = { 0 }, byte = 0;
    job_t job;

    job_init(&job, key, iv, &byte, &byte, 0);
    CHECK(job_run(&job, pool) != 0, "empty Data Block must be rejected");
    job_destroy(&job);
    pool_destroy(pool);
}

/* Several Jobs at once on one shared Worker Pool, as the server runs them. */
typedef struct {
    worker_pool_t *pool;
    unsigned id;
} coordinator_arg_t;

static void *coordinator(void *p)
{
    coordinator_arg_t *arg = p;

    for (unsigned round = 0; round < 10; round++) {
        size_t len = 65537 + (size_t)((arg->id * 7919u + round * 104729u) % (ENC_MAX_BLOCK - 65537));
        check_one_job(arg->pool, len, arg->id * 1000u + round);
    }
    return NULL;
}

static void test_concurrent_jobs(void)
{
    worker_pool_t *pool = pool_create(4);
    pthread_t threads[ENC_MAX_JOBS];
    coordinator_arg_t args[ENC_MAX_JOBS];

    for (unsigned i = 0; i < ENC_MAX_JOBS; i++) {
        args[i].pool = pool;
        args[i].id = i;
        pthread_create(&threads[i], NULL, coordinator, &args[i]);
    }
    for (unsigned i = 0; i < ENC_MAX_JOBS; i++) {
        pthread_join(threads[i], NULL);
    }
    pool_destroy(pool);
}

int main(void)
{
    if (cipher_self_test() != 0) {
        fprintf(stderr, "cipher self-test failed; not running Job tests\n");
        return 1;
    }
    test_segment_plan();
    test_jobs_match_reference();
    test_empty_job_rejected();
    test_concurrent_jobs();

    printf("test_job [%s]: %s\n", cipher_backend_name(), failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
