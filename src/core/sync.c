/*
 * sync.c - checked pthread wrappers, see sync.h.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sync.h"

void enc_fatal(const char *what, int err)
{
    fprintf(stderr, "fatal: %s: %s\n", what, strerror(err));
    abort();
}

void enc_lock(pthread_mutex_t *mutex)
{
    int err = pthread_mutex_lock(mutex);

    if (err != 0) {
        enc_fatal("pthread_mutex_lock", err);
    }
}

void enc_unlock(pthread_mutex_t *mutex)
{
    int err = pthread_mutex_unlock(mutex);

    if (err != 0) {
        enc_fatal("pthread_mutex_unlock", err);
    }
}

void enc_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mutex)
{
    int err = pthread_cond_wait(cv, mutex);

    if (err != 0) {
        enc_fatal("pthread_cond_wait", err);
    }
}

int enc_cond_timedwait(pthread_cond_t *cv, pthread_mutex_t *mutex, const struct timespec *deadline)
{
    int err = pthread_cond_timedwait(cv, mutex, deadline);

    if (err != 0 && err != ETIMEDOUT) {
        enc_fatal("pthread_cond_timedwait", err);
    }
    return err;
}

void enc_cond_signal(pthread_cond_t *cv)
{
    int err = pthread_cond_signal(cv);

    if (err != 0) {
        enc_fatal("pthread_cond_signal", err);
    }
}

void enc_cond_broadcast(pthread_cond_t *cv)
{
    int err = pthread_cond_broadcast(cv);

    if (err != 0) {
        enc_fatal("pthread_cond_broadcast", err);
    }
}

void enc_join(pthread_t thread)
{
    int err = pthread_join(thread, NULL);

    if (err != 0) {
        enc_fatal("pthread_join", err);
    }
}

void enc_mutex_destroy(pthread_mutex_t *mutex)
{
    int err = pthread_mutex_destroy(mutex);

    if (err != 0) {
        enc_fatal("pthread_mutex_destroy", err);
    }
}

void enc_cond_destroy(pthread_cond_t *cv)
{
    int err = pthread_cond_destroy(cv);

    if (err != 0) {
        enc_fatal("pthread_cond_destroy", err);
    }
}

int enc_mutex_cond_init(pthread_mutex_t *mutex, pthread_cond_t *cv)
{
    int err = pthread_mutex_init(mutex, NULL);

    if (err != 0) {
        return err;
    }
    err = pthread_cond_init(cv, NULL);
    if (err != 0) {
        enc_mutex_destroy(mutex);
    }
    return err;
}
