/*
 * worker_pool.c - Worker Pool implementation.
 *
 * Synchronisation:
 *   queue_mutex   guards head, tail and shutdown.
 *   not_empty_cv  is signalled when an item is queued, and broadcast on shutdown.
 * Workers run items with no pool lock held.
 */
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

#include "sync.h"
#include "worker_pool.h"

typedef struct {
    worker_pool_t *pool;
    unsigned index;
} worker_arg_t;

struct worker_pool {
    pthread_t *threads;
    worker_arg_t *args;
    unsigned num_workers;

    pthread_mutex_t queue_mutex;
    pthread_cond_t not_empty_cv;
    pool_work_t *head;              /* guarded by queue_mutex */
    pool_work_t *tail;              /* guarded by queue_mutex */
    bool shutdown;                  /* guarded by queue_mutex */
};

/* Index of the worker running on this thread, or -1 on any other thread. */
static _Thread_local int current_worker = -1;

int pool_current_worker(void)
{
    return current_worker;
}

static void *worker_loop(void *arg)
{
    const worker_arg_t *self = arg;
    worker_pool_t *pool = self->pool;

    current_worker = (int)self->index;

    for (;;) {
        pool_work_t *work;

        enc_lock(&pool->queue_mutex);
        while (pool->head == NULL && !pool->shutdown) {
            enc_cond_wait(&pool->not_empty_cv, &pool->queue_mutex);
        }
        if (pool->head == NULL) {
            /* Shutdown requested and nothing left to do. */
            enc_unlock(&pool->queue_mutex);
            return NULL;
        }
        work = pool->head;
        pool->head = work->next;
        if (pool->head == NULL) {
            pool->tail = NULL;
        }
        enc_unlock(&pool->queue_mutex);

        work->next = NULL;
        work->run(work);
    }
}

worker_pool_t *pool_create(unsigned num_workers)
{
    worker_pool_t *pool;

    if (num_workers == 0) {
        return NULL;
    }
    pool = calloc(1, sizeof *pool);
    if (pool == NULL) {
        return NULL;
    }
    pool->threads = calloc(num_workers, sizeof *pool->threads);
    pool->args = calloc(num_workers, sizeof *pool->args);
    if (pool->threads == NULL || pool->args == NULL) {
        free(pool->threads);
        free(pool->args);
        free(pool);
        return NULL;
    }
    if (enc_mutex_cond_init(&pool->queue_mutex, &pool->not_empty_cv) != 0) {
        free(pool->threads);
        free(pool->args);
        free(pool);
        return NULL;
    }

    for (unsigned i = 0; i < num_workers; i++) {
        pool->args[i].pool = pool;
        pool->args[i].index = i;
        if (pthread_create(&pool->threads[i], NULL, worker_loop, &pool->args[i]) != 0) {
            pool->num_workers = i;  /* stop the ones already running */
            pool_destroy(pool);
            return NULL;
        }
    }
    pool->num_workers = num_workers;
    return pool;
}

void pool_submit(worker_pool_t *pool, pool_work_t *work)
{
    work->next = NULL;

    enc_lock(&pool->queue_mutex);
    if (pool->tail == NULL) {
        pool->head = work;
    } else {
        pool->tail->next = work;
    }
    pool->tail = work;
    enc_cond_signal(&pool->not_empty_cv);
    enc_unlock(&pool->queue_mutex);
}

unsigned pool_size(const worker_pool_t *pool)
{
    return pool->num_workers;
}

void pool_destroy(worker_pool_t *pool)
{
    enc_lock(&pool->queue_mutex);
    pool->shutdown = true;
    enc_cond_broadcast(&pool->not_empty_cv);
    enc_unlock(&pool->queue_mutex);

    for (unsigned i = 0; i < pool->num_workers; i++) {
        enc_join(pool->threads[i]);
    }
    enc_cond_destroy(&pool->not_empty_cv);
    enc_mutex_destroy(&pool->queue_mutex);
    free(pool->threads);
    free(pool->args);
    free(pool);
}
