/*
 * sync.h - checked wrappers for pthread calls that fail only through a
 * programming error: locking a destroyed mutex, unlocking one this thread does
 * not hold, waiting on a broken condition variable. Each wrapper checks the
 * return value and, on failure, prints which call failed and aborts, so a
 * locking bug can never go unnoticed.
 *
 * Calls that can fail at run time for ordinary reasons (pthread_create,
 * pthread_mutex_init, pthread_cond_init) are not wrapped. Their callers check
 * the result and recover.
 */
#ifndef ENC_SYNC_H
#define ENC_SYNC_H

#include <pthread.h>
#include <time.h>

/* Print "fatal: <what>: <strerror(err)>" on stderr and abort. */
void enc_fatal(const char *what, int err) __attribute__((noreturn));

void enc_lock(pthread_mutex_t *mutex);
void enc_unlock(pthread_mutex_t *mutex);
void enc_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mutex);
void enc_cond_signal(pthread_cond_t *cv);
void enc_cond_broadcast(pthread_cond_t *cv);

/* Wait until signalled or until the CLOCK_REALTIME time `deadline`. Returns 0 or ETIMEDOUT. */
int enc_cond_timedwait(pthread_cond_t *cv, pthread_mutex_t *mutex, const struct timespec *deadline);

void enc_join(pthread_t thread);
void enc_mutex_destroy(pthread_mutex_t *mutex);
void enc_cond_destroy(pthread_cond_t *cv);

/*
 * Initialise a mutex and a condition variable together. Returns 0 or an errno
 * value; on failure neither is left initialised.
 */
int enc_mutex_cond_init(pthread_mutex_t *mutex, pthread_cond_t *cv);

#endif /* ENC_SYNC_H */
