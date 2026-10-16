
/*
 * client_msg.c - the client side of the protocol in protocol.h.
 *
 * For each Data Block a sender thread:
 *   1. sends ENCRYPT with the Data Block and its registered pulse; the server
 *      copies the data and replies at once with a Job id,
 *   2. waits on its private channel for the pulse that the server delivers
 *      with MsgDeliverEvent() when the Job is done,
 *   3. sends FETCH and gets the Encrypted Block in the reply.
 *
 * Every blocking kernel call is preceded by TimerTimeout(), so the whole
 * exchange gives up with ETIMEDOUT after ENC_CLIENT_TIMEOUT_NS (10 s).
 */
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>                 /* SETIOV (QNX 8.0 declares it here) */

#include <sys/neutrino.h>
#include <sys/netmgr.h>

#include "client.h"
#include "util.h"

int encsvc_notify_open(encsvc_notify_t *notify, int server_coid)
{
    int err;

    memset(notify, 0, sizeof *notify);
    notify->self_coid = -1;
    notify->chid = ChannelCreate(0);
    if (notify->chid == -1) {
        return errno;
    }
    /*
     * The client's only ConnectAttach(), and it is to its own channel: a pulse
     * needs a connection to travel on. The server is reached with name_open().
     */
    notify->self_coid = ConnectAttach(ND_LOCAL_NODE, 0, notify->chid, _NTO_SIDE_CHANNEL,
                                      _NTO_COF_CLOEXEC);
    if (notify->self_coid == -1) {
        err = errno;
        (void)encsvc_notify_close(notify);
        return err;
    }
    SIGEV_PULSE_INIT(&notify->event, notify->self_coid, SIGEV_PULSE_PRIO_INHERIT, ENCSVC_PULSE_DONE, 0);

    /*
     * Secure events (QNX 7.1 and later): register the event so the kernel lets
     * this server deliver it, and only exactly as we built it.
     */
    if (MsgRegisterEvent(&notify->event, server_coid) == -1) {
        err = errno;
        (void)encsvc_notify_close(notify);
        return err;
    }
    notify->registered = true;
    return 0;
}

int encsvc_notify_close(encsvc_notify_t *notify)
{
    int err = 0;

    if (notify->registered && MsgUnregisterEvent(&notify->event) == -1) {
        err = errno;
    }
    notify->registered = false;
    if (notify->self_coid != -1 && ConnectDetach(notify->self_coid) == -1 && err == 0) {
        err = errno;
    }
    if (notify->chid != -1 && ChannelDestroy(notify->chid) == -1 && err == 0) {
        err = errno;
    }
    notify->self_coid = notify->chid = -1;
    return err;
}

/* Arm a kernel timeout, ending at `deadline`, for this thread's next blocking call in `states`. */
static int arm_timeout(uint32_t states, uint64_t deadline)
{
    struct sigevent unblock;
    uint64_t now = now_ns(), left;

    if (now >= deadline) {
        return ETIMEDOUT;
    }
    left = deadline - now;
    SIGEV_UNBLOCK_INIT(&unblock);
    if (TimerTimeout(CLOCK_MONOTONIC, states, &unblock, &left, NULL) == -1) {
        return errno;
    }
    return 0;
}

/* Wait on the private channel for our "Job done" pulse. Returns 0, ETIMEDOUT or an errno value. */
static int wait_for_done(const encsvc_notify_t *notify, uint64_t deadline)
{
    for (;;) {
        struct _pulse pulse;
        int err = arm_timeout(_NTO_TIMEOUT_RECEIVE, deadline);

        if (err != 0) {
            return err;
        }
        if (MsgReceivePulse(notify->chid, &pulse, sizeof pulse, NULL) == -1) {
            if (errno == EINTR) {
                continue;
            }
            return errno;
        }
        if (pulse.code == ENCSVC_PULSE_DONE) {
            return 0;               /* a late one from an earlier Data Block is caught by FETCH */
        }
        /* Nothing else should arrive on this private channel; ignore it. */
    }
}

int encsvc_encrypt(int coid, const encsvc_notify_t *notify, const uint8_t *data,
                   uint8_t *encrypted, size_t len, encsvc_result_t *result)
{
    uint64_t deadline = now_ns() + ENC_CLIENT_TIMEOUT_NS;
    encsvc_encrypt_t request = { .type = ENCSVC_MSG_ENCRYPT, .data_len = (uint32_t)len,
                                 .event = notify->event };
    encsvc_accepted_t accepted;
    encsvc_fetch_t fetch = { .type = ENCSVC_MSG_FETCH };
    iov_t siov[2], riov[2];
    int err;

    /* 1. ENCRYPT: the server copies the Data Block and answers at once. */
    SETIOV(&siov[0], &request, sizeof request);
    SETIOV(&siov[1], (void *)data, len);    /* send parts are only read; SETIOV takes void * */
    SETIOV(&riov[0], &accepted, sizeof accepted);
    err = arm_timeout(_NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, deadline);
    if (err != 0) {
        return err;
    }
    if (MsgSendv(coid, siov, 2, riov, 1) == -1) {
        return errno;
    }

    fetch.job_id = accepted.job_id;
    SETIOV(&siov[0], &fetch, sizeof fetch);
    SETIOV(&riov[0], result, sizeof *result);
    SETIOV(&riov[1], encrypted, len);
    for (;;) {
        /* 2. The server delivers our pulse when the Job is done. */
        err = wait_for_done(notify, deadline);
        if (err != 0) {
            return err;
        }
        /* 3. FETCH the Encrypted Block. */
        err = arm_timeout(_NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, deadline);
        if (err != 0) {
            return err;
        }
        if (MsgSendv(coid, siov, 1, riov, 2) != -1) {
            return result->data_len == len ? 0 : EBADMSG;
        }
        if (errno != EAGAIN) {
            return errno;
        }
        /* EAGAIN: a late pulse from an earlier Data Block woke us; wait for ours. */
    }
}

const char *encsvc_strerror(int err)
{
    switch (err) {
    case ETIMEDOUT:
        return "no Encrypted Block within 10 s (is encserver overloaded or stopped?)";
    case ENOENT:
    case EBADF:
    case ESRCH:
        return "encserver went away (is it still running?)";
    case EBUSY:
        return "encserver is busy with too many clients; try again";
    default:
        return strerror(err);
    }
}
