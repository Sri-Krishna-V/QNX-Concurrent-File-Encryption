/*
 * qnx_shim.c - in-process stand-in for QNX message passing, for test_e2e only.
 *
 * Client and server run as threads of one process, so "copying between
 * address spaces" is a plain memcpy between iov lists. Semantics kept from QNX:
 *   - MsgSendv blocks the sender until MsgReply(v)/MsgError.
 *   - MsgReceive returns only the first `bytes` of the message; the rest is
 *     pulled with MsgRead at an offset. Pulses are received before messages.
 *   - Each name_open() is its own connection with its own scoid; name_close()
 *     sends the server a _PULSE_CODE_DISCONNECT pulse.
 *   - MsgDeliverEvent queues the event's pulse on the channel that the event's
 *     connection (from ConnectAttach) points to.
 *   - TimerTimeout applies to the calling thread's next blocking call only.
 *     When a reply-blocked send times out, the server gets _PULSE_CODE_UNBLOCK
 *     (as with _NTO_CHF_UNBLOCK) and the sender stays blocked until answered.
 *   - Replying to an unknown rcvid fails with ESRCH.
 *   - MsgRegisterEvent() changes the event (QNX swaps it for a handle), and
 *     MsgDeliverEvent() refuses an event that was not registered (EACCES).
 *   - Each name_open() stands for a separate client process: on QNX the
 *     scoid is per process, and the disconnect pulse comes when all of a
 *     process's connections are closed.
 * Not modelled: priorities, process death.
 */
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "protocol.h"
#include "qnx_shim.h"

#define SERVER_CHID     1
#define MAX_CHANNELS    256
#define MAX_PULSES      64
#define CHANNEL_COID(c) (10000 + (c))   /* ConnectAttach() result for channel c */
#define SHIM_REGISTERED 0x10000         /* sigev_notify bit: passed through MsgRegisterEvent() */

typedef struct shim_msg {
    rcvid_t rcvid;
    int scoid;
    uint16_t type;
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

typedef struct {
    bool used;
    struct _pulse pulses[MAX_PULSES];
    unsigned count;
} channel_t;

static pthread_mutex_t shim_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t shim_cv = PTHREAD_COND_INITIALIZER;
static shim_msg_t *live;
static rcvid_t next_rcvid = 1;
static int next_coid = 1;
static bool attached;
static channel_t channels[MAX_CHANNELS];
#define MAX_COIDS       4096
static int open_jobs, max_open_jobs;    /* accepted by the server, not yet fetched */
static int open_per_coid[MAX_COIDS];    /* the same, per connection */
static unsigned unblock_pulses;
static name_attach_t the_attach = { .chid = SERVER_CHID };

/* This thread's armed TimerTimeout, for its next blocking call. */
static _Thread_local struct {
    bool armed;
    int states;
    struct timespec deadline;       /* CLOCK_REALTIME, for pthread_cond_timedwait */
} timeout;

unsigned shim_max_open_jobs(void)
{
    unsigned m;

    pthread_mutex_lock(&shim_mutex);
    m = (unsigned)max_open_jobs;
    pthread_mutex_unlock(&shim_mutex);
    return m;
}

unsigned shim_open_jobs(void)
{
    unsigned m;

    pthread_mutex_lock(&shim_mutex);
    m = (unsigned)open_jobs;
    pthread_mutex_unlock(&shim_mutex);
    return m;
}

unsigned shim_unblock_pulses(void)
{
    unsigned m;

    pthread_mutex_lock(&shim_mutex);
    m = unblock_pulses;
    pthread_mutex_unlock(&shim_mutex);
    return m;
}

/* Take this thread's timeout if it covers `state`. Returns true and the deadline if so. */
static bool take_timeout(int state, struct timespec *deadline)
{
    bool use = timeout.armed && (timeout.states & state);

    if (use) {
        *deadline = timeout.deadline;
    }
    timeout.armed = false;          /* a timeout covers one kernel call only */
    return use;
}

/* Wait on shim_cv, until the deadline if there is one. Returns false on timeout. */
static bool wait_until(bool timed, const struct timespec *deadline)
{
    if (!timed) {
        pthread_cond_wait(&shim_cv, &shim_mutex);
        return true;
    }
    return pthread_cond_timedwait(&shim_cv, &shim_mutex, deadline) == 0;
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

/* Queue a pulse on channel chid (shim_mutex held). Returns 0 or an errno value. */
static int queue_pulse(int chid, int8_t code, union sigval value, int scoid)
{
    channel_t *ch = chid > 0 && chid < MAX_CHANNELS ? &channels[chid] : NULL;

    if (ch == NULL || !ch->used) {
        return ESRCH;
    }
    if (ch->count == MAX_PULSES) {
        return EAGAIN;
    }
    memset(&ch->pulses[ch->count], 0, sizeof ch->pulses[0]);
    ch->pulses[ch->count].code = code;
    ch->pulses[ch->count].value = value;
    ch->pulses[ch->count].scoid = scoid;
    ch->count++;
    pthread_cond_broadcast(&shim_cv);
    return 0;
}

static void take_pulse(channel_t *ch, struct _pulse *out)
{
    *out = ch->pulses[0];
    ch->count--;
    memmove(&ch->pulses[0], &ch->pulses[1], ch->count * sizeof ch->pulses[0]);
}

long MsgSendv(int coid, const iov_t *siov, size_t sparts, const iov_t *riov, size_t rparts)
{
    shim_msg_t msg = { .scoid = coid, .siov = siov, .sparts = sparts, .riov = riov, .rparts = rparts };
    struct timespec deadline;
    bool armed = timeout.armed;
    bool send_timed = armed && (timeout.states & _NTO_TIMEOUT_SEND);
    bool reply_timed = armed && (timeout.states & _NTO_TIMEOUT_REPLY);
    bool unblocked = false;

    deadline = timeout.deadline;
    timeout.armed = false;          /* a timeout covers one kernel call only */
    gather(siov, sparts, 0, &msg.type, sizeof msg.type);

    pthread_mutex_lock(&shim_mutex);
    if (!attached) {
        pthread_mutex_unlock(&shim_mutex);
        errno = ESRCH;
        return -1;
    }
    msg.rcvid = next_rcvid++;
    msg.next = live;
    live = &msg;
    pthread_cond_broadcast(&shim_cv);
    while (!msg.replied) {
        if (!msg.received) {
            if (!wait_until(send_timed, &deadline) && !msg.received) {
                unlink_msg(&msg);       /* send-blocked: the kernel simply gives up */
                pthread_mutex_unlock(&shim_mutex);
                errno = ETIMEDOUT;
                return -1;
            }
        } else if (!wait_until(reply_timed && !unblocked, &deadline) && !msg.replied) {
            /* Reply-blocked on an _NTO_CHF_UNBLOCK channel: ask the server to answer. */
            union sigval v = { .sival_int = (int)msg.rcvid };

            queue_pulse(SERVER_CHID, _PULSE_CODE_UNBLOCK, v, coid);
            unblock_pulses++;
            unblocked = true;
        }
    }
    unlink_msg(&msg);
    pthread_mutex_unlock(&shim_mutex);

    if (msg.err != 0) {
        errno = msg.err;
        return -1;
    }
    return msg.status;
}

long MsgSend(int coid, const void *smsg, size_t sbytes, void *rmsg, size_t rbytes)
{
    iov_t s, r;

    SETIOV(&s, smsg, sbytes);
    SETIOV(&r, rmsg, rbytes);
    return MsgSendv(coid, &s, 1, &r, 1);
}

rcvid_t MsgReceive(int chid, void *buf, size_t bytes, struct _msg_info *info)
{
    shim_msg_t *msg;

    pthread_mutex_lock(&shim_mutex);
    for (;;) {
        if (channels[chid].count > 0) {
            struct _pulse pulse;

            take_pulse(&channels[chid], &pulse);
            if (pulse.code == _PULSE_CODE_DISCONNECT && pulse.scoid > 0 && pulse.scoid < MAX_COIDS) {
                open_jobs -= open_per_coid[pulse.scoid];    /* the server drops them now */
                open_per_coid[pulse.scoid] = 0;
            }
            pthread_mutex_unlock(&shim_mutex);
            memcpy(buf, &pulse, bytes < sizeof pulse ? bytes : sizeof pulse);
            return 0;
        }
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
    memset(info, 0, sizeof *info);
    info->scoid = msg->scoid;
    info->pid = 1000 + msg->scoid;  /* a different "pid" per connection, for the log */
    info->srcmsglen = (ssize64_t)iov_total(msg->siov, msg->sparts);
    info->msglen = (ssize64_t)gather(msg->siov, msg->sparts, 0, buf, bytes);
    pthread_cond_broadcast(&shim_cv);
    pthread_mutex_unlock(&shim_mutex);
    return msg->rcvid;
}

int MsgReceivePulse(int chid, void *pulse, size_t bytes, struct _msg_info *info)
{
    struct timespec deadline;
    bool timed = take_timeout(_NTO_TIMEOUT_RECEIVE, &deadline);
    struct _pulse p;

    (void)info;
    pthread_mutex_lock(&shim_mutex);
    while (chid > 0 && chid < MAX_CHANNELS && channels[chid].used && channels[chid].count == 0) {
        if (!wait_until(timed, &deadline) && channels[chid].count == 0) {
            pthread_mutex_unlock(&shim_mutex);
            errno = ETIMEDOUT;
            return -1;
        }
    }
    if (chid <= 0 || chid >= MAX_CHANNELS || !channels[chid].used) {
        pthread_mutex_unlock(&shim_mutex);
        errno = EBADF;
        return -1;
    }
    take_pulse(&channels[chid], &p);
    pthread_mutex_unlock(&shim_mutex);
    memcpy(pulse, &p, bytes < sizeof p ? bytes : sizeof p);
    return 0;
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
    /* Count Jobs held by the server: accepted ENCRYPTs minus answered FETCHes. */
    if (msg->scoid > 0 && msg->scoid < MAX_COIDS) {
        if (msg->type == ENCSVC_MSG_ENCRYPT && err == 0) {
            open_per_coid[msg->scoid]++;
            if (++open_jobs > max_open_jobs) {
                max_open_jobs = open_jobs;
            }
        } else if (msg->type == ENCSVC_MSG_FETCH && err != EAGAIN && err != ENOENT) {
            open_per_coid[msg->scoid]--;
            open_jobs--;
        }
    }
    msg->status = status;
    msg->err = err;
    msg->replied = true;
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

int MsgDeliverEvent(rcvid_t rcvid, const struct sigevent *event)
{
    int err;

    if (!(event->sigev_notify & SHIM_REGISTERED)) {
        errno = EACCES;             /* QNX 8 accepts only registered events by default */
        return -1;
    }
    if (rcvid <= 0 || (event->sigev_notify & ~SHIM_REGISTERED) != SIGEV_PULSE ||
        event->sigev_coid < CHANNEL_COID(1) || event->sigev_coid >= CHANNEL_COID(MAX_CHANNELS)) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&shim_mutex);
    err = queue_pulse(event->sigev_coid - CHANNEL_COID(0), (int8_t)event->sigev_code,
                      event->sigev_value, 0);
    pthread_mutex_unlock(&shim_mutex);
    if (err != 0) {
        errno = err;
        return -1;
    }
    return 0;
}

/* QNX replaces a registered event with a handle; the shim marks it instead. */
int MsgRegisterEvent(struct sigevent *event, int coid)
{
    if (coid <= 0 || (event->sigev_notify & SHIM_REGISTERED)) {
        errno = EINVAL;
        return -1;
    }
    event->sigev_notify |= SHIM_REGISTERED;
    return 0;
}

int MsgUnregisterEvent(const struct sigevent *event)
{
    if (!(event->sigev_notify & SHIM_REGISTERED)) {
        errno = ESRCH;
        return -1;
    }
    return 0;
}

int ChannelCreate(unsigned flags)
{
    (void)flags;
    pthread_mutex_lock(&shim_mutex);
    for (int c = SERVER_CHID + 1; c < MAX_CHANNELS; c++) {
        if (!channels[c].used) {
            channels[c].used = true;
            channels[c].count = 0;
            pthread_mutex_unlock(&shim_mutex);
            return c;
        }
    }
    pthread_mutex_unlock(&shim_mutex);
    errno = EAGAIN;
    return -1;
}

int ChannelDestroy(int chid)
{
    pthread_mutex_lock(&shim_mutex);
    if (chid <= SERVER_CHID || chid >= MAX_CHANNELS || !channels[chid].used) {
        pthread_mutex_unlock(&shim_mutex);
        errno = EINVAL;
        return -1;
    }
    channels[chid].used = false;
    channels[chid].count = 0;
    pthread_cond_broadcast(&shim_cv);
    pthread_mutex_unlock(&shim_mutex);
    return 0;
}

int ConnectAttach(uint32_t nd, pid_t pid, int chid, unsigned index, int flags)
{
    (void)nd; (void)pid; (void)index; (void)flags;
    if (chid <= SERVER_CHID || chid >= MAX_CHANNELS) {
        errno = ESRCH;
        return -1;
    }
    return CHANNEL_COID(chid);
}

int ConnectDetach(int coid)
{
    (void)coid;
    return 0;
}

int TimerTimeout(clockid_t id, uint32_t flags, const struct sigevent *notify, const uint64_t *ntime,
                 uint64_t *otime)
{
    struct timespec now;

    (void)id; (void)notify; (void)otime;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return -1;
    }
    now.tv_sec += (time_t)(*ntime / 1000000000u);
    now.tv_nsec += (long)(*ntime % 1000000000u);
    if (now.tv_nsec >= 1000000000L) {
        now.tv_sec++;
        now.tv_nsec -= 1000000000L;
    }
    timeout.armed = true;
    timeout.states = (int)flags;
    timeout.deadline = now;
    return 0;
}

name_attach_t *name_attach(dispatch_t *dpp, const char *path, unsigned flags)
{
    (void)dpp; (void)path; (void)flags;
    pthread_mutex_lock(&shim_mutex);
    attached = true;
    channels[SERVER_CHID].used = true;
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
    int coid;

    (void)name; (void)flags;
    pthread_mutex_lock(&shim_mutex);
    while (!attached) {
        pthread_cond_wait(&shim_cv, &shim_mutex);
    }
    coid = next_coid++;
    pthread_mutex_unlock(&shim_mutex);
    return coid;
}

int name_close(int coid)
{
    union sigval v = { .sival_int = 0 };

    pthread_mutex_lock(&shim_mutex);
    queue_pulse(SERVER_CHID, _PULSE_CODE_DISCONNECT, v, coid);
    pthread_mutex_unlock(&shim_mutex);
    return 0;
}
