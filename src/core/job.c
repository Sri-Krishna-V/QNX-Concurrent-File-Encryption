/*
 * job.c - Segment Tasks, mutex-protected output, and completion signalling.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "cipher.h"
#include "job.h"
#include "segment.h"
#include "util.h"

typedef struct {
    pool_work_t work;               /* must be first: the pool hands us this pointer */
    job_t *job;
    size_t offset;
    size_t length;
} task_t;

/* Copy one finished Segment into the Encrypted Block and count it. */
static void commit_segment(task_t *task, const uint8_t *scratch, int status)
{
    job_t *job = task->job;

    pthread_mutex_lock(&job->mutex);
    if (status == 0) {
        memcpy(job->output + task->offset, scratch, task->length);
    } else if (job->error == 0) {
        job->error = status;
    }
    job->completed_count++;
    if (job->completed_count == job->total_segments) {
        job->finish_ns = now_ns();
        pthread_cond_signal(&job->done_cv);
    }
    pthread_mutex_unlock(&job->mutex);
}

/* Runs on a worker thread. Encrypts into private memory, so no lock is held here. */
static void encrypt_segment(pool_work_t *work)
{
    task_t *task = (task_t *)work;
    job_t *job = task->job;
    uint8_t *scratch = malloc(task->length);
    int status;

    if (scratch == NULL) {
        status = ENOMEM;
    } else {
        status = cipher_ctr(job->key, job->iv, task->offset,
                            job->input + task->offset, scratch, task->length);
    }
    commit_segment(task, scratch, status);
    free(scratch);
}

int job_init(job_t *job, const uint8_t *key, const uint8_t iv[ENC_IV_LEN],
             const uint8_t *input, uint8_t *output, size_t len)
{
    memset(job, 0, sizeof *job);
    job->key = key;
    memcpy(job->iv, iv, ENC_IV_LEN);
    job->input = input;
    job->output = output;
    job->len = len;

    if (pthread_mutex_init(&job->mutex, NULL) != 0) {
        return EAGAIN;
    }
    if (pthread_cond_init(&job->done_cv, NULL) != 0) {
        pthread_mutex_destroy(&job->mutex);
        return EAGAIN;
    }
    return 0;
}

int job_run(job_t *job, worker_pool_t *pool)
{
    size_t seg_size = segment_size_for(job->len, pool_size(pool));
    size_t count = segment_count(job->len, seg_size);
    task_t *tasks;
    uint64_t start_ns;
    int err;

    if (count == 0) {
        return EINVAL;              /* an empty Data Block would never complete */
    }
    tasks = calloc(count, sizeof *tasks);
    if (tasks == NULL) {
        return ENOMEM;
    }
    for (size_t i = 0; i < count; i++) {
        tasks[i].work.run = encrypt_segment;
        tasks[i].job = job;
        tasks[i].offset = i * seg_size;
        tasks[i].length = (i + 1 == count) ? job->len - i * seg_size : seg_size;
    }

    /* Set before the first submit: workers compare against it. */
    job->total_segments = count;
    job->completed_count = 0;
    job->error = 0;

    start_ns = now_ns();
    for (size_t i = 0; i < count; i++) {
        pool_submit(pool, &tasks[i].work);
    }

    pthread_mutex_lock(&job->mutex);
    while (job->completed_count < job->total_segments) {
        pthread_cond_wait(&job->done_cv, &job->mutex);
    }
    err = job->error;
    job->encryption_ns = job->finish_ns - start_ns;
    pthread_mutex_unlock(&job->mutex);

    free(tasks);
    return err;
}

void job_destroy(job_t *job)
{
    pthread_cond_destroy(&job->done_cv);
    pthread_mutex_destroy(&job->mutex);
}

int reference_encrypt(const uint8_t *key, const uint8_t iv[ENC_IV_LEN],
                      const uint8_t *input, uint8_t *output, size_t len)
{
    return cipher_ctr(key, iv, 0, input, output, len);
}
