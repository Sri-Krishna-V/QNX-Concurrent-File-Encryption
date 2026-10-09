/*
 * worker_pool.h - fixed-size Worker Pool fed from one shared FIFO queue.
 *
 * Work items are intrusive: embed a pool_work_t as the FIRST member of your
 * own struct, set run, and submit a pointer to it. The pool never allocates
 * or frees work items.
 */
#ifndef ENC_WORKER_POOL_H
#define ENC_WORKER_POOL_H

typedef struct pool_work {
    void (*run)(struct pool_work *work);
    struct pool_work *next;     /* owned by the pool while queued */
} pool_work_t;

typedef struct worker_pool worker_pool_t;

/* Start num_workers threads. Returns NULL on failure. */
worker_pool_t *pool_create(unsigned num_workers);

/* Queue one work item and wake one idle worker. */
void pool_submit(worker_pool_t *pool, pool_work_t *work);

unsigned pool_size(const worker_pool_t *pool);

/* Finish every queued item, stop all workers and free the pool. */
void pool_destroy(worker_pool_t *pool);

#endif /* ENC_WORKER_POOL_H */
