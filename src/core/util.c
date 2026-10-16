/*
 * util.c - small helpers shared by server, client and tests.
 */
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#ifdef __QNX__
#include <sys/syspage.h>
#endif

#include "sync.h"
#include "util.h"

uint64_t now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        enc_fatal("clock_gettime", errno);      /* cannot fail with a valid clock id */
    }
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static int read_exact(int fd, uint8_t *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = read(fd, buf, len);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return n == 0 ? EIO : errno;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

int random_bytes(uint8_t *buf, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY);
    int err;

    if (fd < 0) {
        return errno;
    }
    err = read_exact(fd, buf, len);
    if (close(fd) != 0 && err == 0) {
        err = errno;
    }
    return err;
}

int keyfile_load(const char *path, uint8_t key[ENC_KEY_LEN])
{
    uint8_t extra;
    int fd = open(path, O_RDONLY);
    ssize_t n;
    int err;

    if (fd < 0) {
        return errno;
    }
    err = read_exact(fd, key, ENC_KEY_LEN);
    if (err == 0) {
        n = read(fd, &extra, 1);
        if (n < 0) {
            err = errno;
        } else if (n > 0) {
            err = EINVAL;           /* longer than ENC_KEY_LEN: wrong file */
        }
    }
    if (close(fd) != 0 && err == 0) {
        err = errno;
    }
    return err;
}

unsigned cpu_count(void)
{
#ifdef __QNX__
    return _syspage_ptr->num_cpu > 0 ? _syspage_ptr->num_cpu : 1u;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);

    return n > 0 ? (unsigned)n : 1u;
#endif
}
