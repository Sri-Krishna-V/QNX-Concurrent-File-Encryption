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

#include "worker_pool.h"

struct worker_pool {
    pthread_t *threads;
    unsigned num_workers;

    pthread_mutex_t queue_mutex;
    pthread_cond_t not_empty_cv;
    pool_work_t *head;              /* guarded by queue_mutex */
    pool_work_t *tail;              /* guarded by queue_mutex */
    bool shutdown;                  /* guarded by queue_mutex */
};

static void *worker_loop(void *arg)
{
    worker_pool_t *pool = arg;

    for (;;) {
        pool_work_t *work;

        pthread_mutex_lock(&pool->queue_mutex);
        while (pool->head == NULL && !pool->shutdown) {
            pthread_cond_wait(&pool->not_empty_cv, &pool->queue_mutex);
        }
        if (pool->head == NULL) {
            /* Shutdown requested and nothing left to do. */
            pthread_mutex_unlock(&pool->queue_mutex);
            return NULL;
        }
        work = pool->head;
        pool->head = work->next;
        if (pool->head == NULL) {
            pool->tail = NULL;
        }
        pthread_mutex_unlock(&pool->queue_mutex);

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
    if (pool->threads == NULL) {
        free(pool);
        return NULL;
    }
    pthread_mutex_init(&pool->queue_mutex, NULL);
    pthread_cond_init(&pool->not_empty_cv, NULL);

    for (unsigned i = 0; i < num_workers; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_loop, pool) != 0) {
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

    pthread_mutex_lock(&pool->queue_mutex);
    if (pool->tail == NULL) {
        pool->head = work;
    } else {
        pool->tail->next = work;
    }
    pool->tail = work;
    pthread_cond_signal(&pool->not_empty_cv);
    pthread_mutex_unlock(&pool->queue_mutex);
}

unsigned pool_size(const worker_pool_t *pool)
{
    return pool->num_workers;
}

void pool_destroy(worker_pool_t *pool)
{
    pthread_mutex_lock(&pool->queue_mutex);
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->not_empty_cv);
    pthread_mutex_unlock(&pool->queue_mutex);

    for (unsigned i = 0; i < pool->num_workers; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    pthread_cond_destroy(&pool->not_empty_cv);
    pthread_mutex_destroy(&pool->queue_mutex);
    free(pool->threads);
    free(pool);
}
