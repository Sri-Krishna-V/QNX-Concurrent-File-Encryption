/*
 * job.h - one Job: the encryption of one Data Block by the Worker Pool.
 *
 * Usage (on the Job Coordinator thread):
 *     job_init(&job, key, iv, data_block, encrypted_block, len);
 *     err = job_run(&job, pool);      // blocks until every Segment is done
 *     ... use job.encryption_ns, job.total_segments ...
 *     job_destroy(&job);
 */
#ifndef ENC_JOB_H
#define ENC_JOB_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "worker_pool.h"

typedef struct job {
    /* Read-only once job_run() starts; shared by all workers without locking. */
    const uint8_t *key;
    uint8_t iv[ENC_IV_LEN];
    const uint8_t *input;           /* the Data Block */
    size_t len;
    size_t total_segments;

    /* Shared and guarded by mutex. */
    uint8_t *output;                /* the Encrypted Block */
    size_t completed_count;
    int error;                      /* first Task failure, 0 if none */
    uint64_t finish_ns;             /* set by the worker that completes the last Segment */

    pthread_mutex_t mutex;
    pthread_cond_t done_cv;         /* signalled when completed_count == total_segments */

    /* Results, valid after job_run() returns 0. */
    uint64_t encryption_ns;         /* Encryption Time */
} job_t;

int job_init(job_t *job, const uint8_t *key, const uint8_t iv[ENC_IV_LEN],
             const uint8_t *input, uint8_t *output, size_t len);

/*
 * Split the Data Block into Segments, queue one Task per Segment on the pool,
 * and wait on done_cv until all Tasks have finished. Returns 0 or an errno value.
 */
int job_run(job_t *job, worker_pool_t *pool);

void job_destroy(job_t *job);

/* Reference Encryption: the whole Data Block on the calling thread, no Segments. */
int reference_encrypt(const uint8_t *key, const uint8_t iv[ENC_IV_LEN],
                      const uint8_t *input, uint8_t *output, size_t len);

#endif /* ENC_JOB_H */
