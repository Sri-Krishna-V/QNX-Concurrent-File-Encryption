/*
 * qnx_shim.c - in-process stand-in for QNX message passing, for test_e2e only.
 *
 * Client and server run as threads of one process, so "copying between
 * address spaces" is a plain memcpy between iov lists. Semantics kept from QNX:
 *   - MsgSendv blocks the sender until MsgReplyv/MsgReply/MsgError.
 *   - MsgReceive returns only the first `bytes` of the message; the rest is
 *     pulled with MsgRead at an offset.
 *   - Replying to an unknown rcvid fails with ESRCH.
 * Not modelled: pulses, _IO_CONNECT, process death.
 */
#include <pthread.h>
#include <stdbool.h>
#include <string.h>

#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "qnx_shim.h"

typedef struct shim_msg {
    rcvid_t rcvid;
    const iov_t *siov;
    size_t sparts;
    const iov_t *riov;
    size_t rparts;
    bool received;
    bool replied;
    long status;
    int err;
    struct shim_msg *next;          /* all live messages, newest first */
} shim_msg_t;

static pthread_mutex_t shim_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t shim_cv = PTHREAD_COND_INITIALIZER;
static shim_msg_t *live;
static rcvid_t next_rcvid = 1;
static bool attached;
static unsigned in_server, max_in_server;   /* received but not yet replied */
static name_attach_t the_attach = { .chid = 1 };

unsigned shim_max_in_server(void)
{
    unsigned m;

    pthread_mutex_lock(&shim_mutex);
    m = max_in_server;
    pthread_mutex_unlock(&shim_mutex);
    return m;
}

static size_t iov_total(const iov_t *iov, size_t parts)
{
    size_t total = 0;

    for (size_t i = 0; i < parts; i++) {
        total += iov[i].iov_len;
    }
    return total;
}

/* Copy up to bytes from the flattened iov list, starting at offset. */
static size_t gather(const iov_t *iov, size_t parts, size_t offset, void *dst, size_t bytes)
{
    uint8_t *out = dst;
    size_t copied = 0;

    for (size_t i = 0; i < parts && copied < bytes; i++) {
        size_t len = iov[i].iov_len;
        if (offset >= len) {
            offset -= len;
            continue;
        }
        size_t n = len - offset;
        if (n > bytes - copied) {
            n = bytes - copied;
        }
        memcpy(out + copied, (const uint8_t *)iov[i].iov_base + offset, n);
        copied += n;
        offset = 0;
    }
    return copied;
}

static void scatter(const iov_t *iov, size_t parts, const iov_t *src, size_t sparts)
{
    size_t offset = 0;

    for (size_t i = 0; i < parts; i++) {
        offset += gather(src, sparts, offset, iov[i].iov_base, iov[i].iov_len);
    }
}

static shim_msg_t *find(rcvid_t rcvid)
{
    for (shim_msg_t *m = live; m != NULL; m = m->next) {
        if (m->rcvid == rcvid && m->received && !m->replied) {
            return m;
        }
    }
    return NULL;
}

static void unlink_msg(shim_msg_t *msg)
{
    for (shim_msg_t **p = &live; *p != NULL; p = &(*p)->next) {
        if (*p == msg) {
            *p = msg->next;
            return;
        }
    }
}

long MsgSendv(int coid, const iov_t *siov, size_t sparts, const iov_t *riov, size_t rparts)
{
    shim_msg_t msg = { .siov = siov, .sparts = sparts, .riov = riov, .rparts = rparts };

    (void)coid;
    pthread_mutex_lock(&shim_mutex);
    msg.rcvid = next_rcvid++;
    msg.next = live;
    live = &msg;
    pthread_cond_broadcast(&shim_cv);
    while (!msg.replied) {
        pthread_cond_wait(&shim_cv, &shim_mutex);
    }
    unlink_msg(&msg);
    pthread_mutex_unlock(&shim_mutex);

    if (msg.err != 0) {
        errno = msg.err;
        return -1;
    }
    return msg.status;
}

rcvid_t MsgReceive(int chid, void *buf, size_t bytes, struct _msg_info *info)
{
    shim_msg_t *msg;

    (void)chid;
    pthread_mutex_lock(&shim_mutex);
    for (;;) {
        /* Oldest unreceived message first (the list is newest first). */
        msg = NULL;
        for (shim_msg_t *m = live; m != NULL; m = m->next) {
            if (!m->received) {
                msg = m;
            }
        }
        if (msg != NULL) {
            break;
        }
        pthread_cond_wait(&shim_cv, &shim_mutex);
    }
    msg->received = true;
    if (++in_server > max_in_server) {
        max_in_server = in_server;
    }
    memset(info, 0, sizeof *info);
    info->srcmsglen = (ssize64_t)iov_total(msg->siov, msg->sparts);
    info->msglen = (ssize64_t)gather(msg->siov, msg->sparts, 0, buf, bytes);
    pthread_mutex_unlock(&shim_mutex);
    return msg->rcvid;
}

ssize_t MsgRead(rcvid_t rcvid, void *buf, size_t bytes, size_t offset)
{
    shim_msg_t *msg;
    ssize_t n;

    pthread_mutex_lock(&shim_mutex);
    msg = find(rcvid);
    n = msg ? (ssize_t)gather(msg->siov, msg->sparts, offset, buf, bytes) : -1;
    pthread_mutex_unlock(&shim_mutex);
    if (n == -1) {
        errno = ESRCH;
    }
    return n;
}

static int finish(rcvid_t rcvid, long status, int err, const iov_t *iov, size_t parts)
{
    shim_msg_t *msg;

    pthread_mutex_lock(&shim_mutex);
    msg = find(rcvid);
    if (msg == NULL) {
        pthread_mutex_unlock(&shim_mutex);
        errno = ESRCH;
        return -1;
    }
    if (iov != NULL) {
        scatter(msg->riov, msg->rparts, iov, parts);
    }
    msg->status = status;
    msg->err = err;
    msg->replied = true;
    in_server--;
    pthread_cond_broadcast(&shim_cv);
    pthread_mutex_unlock(&shim_mutex);
    return 0;
}

int MsgReplyv(rcvid_t rcvid, long status, const iov_t *riov, size_t rparts)
{
    return finish(rcvid, status, 0, riov, rparts);
}

int MsgReply(rcvid_t rcvid, long status, const void *msg, size_t bytes)
{
    iov_t iov;

    SETIOV(&iov, msg, bytes);
    return finish(rcvid, status, 0, msg ? &iov : NULL, msg ? 1 : 0);
}

int MsgError(rcvid_t rcvid, int error)
{
    return finish(rcvid, -1, error, NULL, 0);
}

int ConnectDetach(int coid)
{
    (void)coid;
    return 0;
}

name_attach_t *name_attach(dispatch_t *dpp, const char *path, unsigned flags)
{
    (void)dpp; (void)path; (void)flags;
    pthread_mutex_lock(&shim_mutex);
    attached = true;
    pthread_cond_broadcast(&shim_cv);
    pthread_mutex_unlock(&shim_mutex);
    return &the_attach;
}

int name_detach(name_attach_t *attach, unsigned flags)
{
    (void)attach; (void)flags;
    return 0;
}

int name_open(const char *name, int flags)
{
    (void)name; (void)flags;
    pthread_mutex_lock(&shim_mutex);
    while (!attached) {
        pthread_cond_wait(&shim_cv, &shim_mutex);
    }
    pthread_mutex_unlock(&shim_mutex);
    return 1;
}

int name_close(int coid)
{
    (void)coid;
    return 0;
}
