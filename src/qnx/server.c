/*
 * server.c - encserver: the Concurrent Encryption Service.
 *
 *   encserver [-w workers] [-k keyfile] [-v]
 *
 * Threads:
 *   Receive Thread    main(): MsgReceive loop. Pulls each Data Block, starts a Job,
 *                     and goes straight back to MsgReceive. It never replies to an
 *                     ENCRYPT request itself (deferred reply).
 *   Job Coordinator   one detached thread per Job: runs the Job on the Worker Pool,
 *                     waits for completion, then replies with the Encrypted Block.
 *   Worker Pool       encrypts Segments (src/core/worker_pool.c, src/core/job.c).
 *
 * At most ENC_MAX_JOBS Jobs are in progress. When that many are running, the
 * Receive Thread waits before calling MsgReceive, so new clients stay
 * send-blocked in the kernel until a Job finishes.
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>

#include "cipher.h"
#include "config.h"
#include "job.h"
#include "protocol.h"
#include "util.h"
#include "worker_pool.h"

typedef struct {
    job_t job;
    rcvid_t rcvid;                  /* client to reply to when the Job completes */
    uint8_t *buffers;               /* Data Block followed by Encrypted Block */
} server_job_t;

static struct {
    worker_pool_t *pool;
    uint8_t key[ENC_KEY_LEN];
    bool verbose;

    pthread_mutex_t jobs_mutex;
    pthread_cond_t slot_free_cv;    /* signalled when active_jobs drops */
    unsigned active_jobs;           /* guarded by jobs_mutex */
} server = {
    .jobs_mutex = PTHREAD_MUTEX_INITIALIZER,
    .slot_free_cv = PTHREAD_COND_INITIALIZER,
};

static void wait_for_job_slot(void)
{
    pthread_mutex_lock(&server.jobs_mutex);
    while (server.active_jobs >= ENC_MAX_JOBS) {
        pthread_cond_wait(&server.slot_free_cv, &server.jobs_mutex);
    }
    server.active_jobs++;
    pthread_mutex_unlock(&server.jobs_mutex);
}

static void release_job_slot(void)
{
    pthread_mutex_lock(&server.jobs_mutex);
    server.active_jobs--;
    pthread_cond_signal(&server.slot_free_cv);
    pthread_mutex_unlock(&server.jobs_mutex);
}

static void server_job_free(server_job_t *sj)
{
    job_destroy(&sj->job);
    free(sj->buffers);
    free(sj);
}

/* Deferred reply: send the Encrypted Block, or the error, to the waiting client. */
static void send_reply(server_job_t *sj, int err)
{
    encsvc_reply_t reply;
    iov_t iov[2];

    if (err != 0) {
        MsgError(sj->rcvid, err);
        return;
    }

    memset(&reply, 0, sizeof reply);
    reply.data_len = (uint32_t)sj->job.len;
    reply.segments = (uint32_t)sj->job.total_segments;
    memcpy(reply.iv, sj->job.iv, ENC_IV_LEN);
    reply.encryption_ns = sj->job.encryption_ns;
    reply.workers = pool_size(server.pool);

    SETIOV(&iov[0], &reply, sizeof reply);
    SETIOV(&iov[1], sj->job.output, sj->job.len);
    if (MsgReplyv(sj->rcvid, EOK, iov, 2) == -1) {
        /* Abandoned Job: the client went away; the Encrypted Block is discarded. */
        fprintf(stderr, "encserver: client gone before reply (%s); result discarded\n",
                strerror(errno));
    }
}

/* Job Coordinator thread: one per Job. */
static void *job_coordinator(void *arg)
{
    server_job_t *sj = arg;
    int err = job_run(&sj->job, server.pool);

    if (server.verbose) {
        fprintf(stderr, "encserver: job %zu bytes, %zu segments, %.3f ms, %s\n",
                sj->job.len, sj->job.total_segments, (double)sj->job.encryption_ns / 1e6,
                err ? strerror(err) : "ok");
    }
    send_reply(sj, err);
    server_job_free(sj);
    release_job_slot();
    return NULL;
}

/*
 * Handle one ENCRYPT request on the Receive Thread. Takes ownership of the
 * job slot: either a Job Coordinator releases it later, or this function
 * replies with an error and releases it now.
 */
static void handle_encrypt(rcvid_t rcvid, const encsvc_request_t *req)
{
    size_t len = req->data_len;
    uint8_t iv[ENC_IV_LEN];
    server_job_t *sj = NULL;
    pthread_attr_t attr;
    pthread_t tid;
    ssize_t got;
    int err;

    if (len == 0) {
        err = EINVAL;
        goto fail;
    }
    if (len > ENC_MAX_BLOCK) {
        err = EMSGSIZE;
        goto fail;
    }

    sj = calloc(1, sizeof *sj);
    if (sj == NULL || (sj->buffers = malloc(2 * len)) == NULL) {
        err = ENOMEM;
        goto fail;
    }
    sj->rcvid = rcvid;

    /* Pull the Data Block out of the client's send buffer, after the header. */
    got = MsgRead(rcvid, sj->buffers, len, sizeof *req);
    if (got != (ssize_t)len) {
        err = EBADMSG;
        goto fail;
    }

    err = random_bytes(iv, sizeof iv);
    if (err != 0) {
        goto fail;
    }
    err = job_init(&sj->job, server.key, iv, sj->buffers, sj->buffers + len, len);
    if (err != 0) {
        goto fail;
    }

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    err = pthread_create(&tid, &attr, job_coordinator, sj);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        job_destroy(&sj->job);
        goto fail;
    }
    return;                         /* the Job Coordinator replies later */

fail:
    MsgError(rcvid, err);
    if (sj != NULL) {
        free(sj->buffers);
        free(sj);
    }
    release_job_slot();
}

static void receive_loop(name_attach_t *attach)
{
    union {
        struct _pulse pulse;
        uint16_t type;
        encsvc_request_t request;
    } msg;

    for (;;) {
        struct _msg_info info;
        rcvid_t rcvid;

        wait_for_job_slot();
        rcvid = MsgReceive(attach->chid, &msg, sizeof msg, &info);

        if (rcvid == -1) {
            perror("encserver: MsgReceive");
            release_job_slot();
            continue;
        }
        if (rcvid == 0) {
            /* Pulse. Disconnect: free the client's connection. Unblock: a reply-
             * blocked client got a signal; its Job finishes and is answered as usual. */
            if (msg.pulse.code == _PULSE_CODE_DISCONNECT) {
                ConnectDetach(msg.pulse.scoid);
            }
            release_job_slot();
            continue;
        }
        if (msg.type == _IO_CONNECT) {
            /* Sent by name_open(); accept the connection. */
            MsgReply(rcvid, EOK, NULL, 0);
            release_job_slot();
            continue;
        }
        if (msg.type != ENCSVC_MSG_ENCRYPT || info.msglen < (ssize_t)sizeof msg.request) {
            MsgError(rcvid, ENOSYS);
            release_job_slot();
            continue;
        }
        handle_encrypt(rcvid, &msg.request);
    }
}

static void usage(void)
{
    fprintf(stderr,
            "usage: encserver [-w workers] [-k keyfile] [-v]\n"
            "  -w workers  Worker Pool size (default: number of CPUs)\n"
            "  -k keyfile  32-byte key file (default: ./enc.key)\n"
            "  -v          log one line per Job\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *keyfile = "enc.key";
    unsigned workers = cpu_count();
    name_attach_t *attach;
    int opt, err;

    while ((opt = getopt(argc, argv, "w:k:v")) != -1) {
        switch (opt) {
        case 'w': workers = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'k': keyfile = optarg; break;
        case 'v': server.verbose = true; break;
        default: usage();
        }
    }
    if (workers == 0) {
        usage();
    }

    err = keyfile_load(keyfile, server.key);
    if (err != 0) {
        fprintf(stderr, "encserver: cannot load key file %s: %s\n", keyfile, strerror(err));
        return 1;
    }
    if (cipher_self_test() != 0) {
        fprintf(stderr, "encserver: cipher self-test FAILED (%s backend)\n", cipher_backend_name());
        return 1;
    }
    server.pool = pool_create(workers);
    if (server.pool == NULL) {
        fprintf(stderr, "encserver: cannot start %u workers\n", workers);
        return 1;
    }

    attach = name_attach(NULL, ENCSVC_NAME, 0);
    if (attach == NULL) {
        perror("encserver: name_attach");
        if (errno == EPERM) {
            fprintf(stderr, "encserver: try running as root (su)\n");
        }
        return 1;
    }

    printf("encserver: ready as \"%s\", %u workers, cipher %s, max block %u bytes, max %u jobs\n",
           ENCSVC_NAME, workers, cipher_backend_name(), ENC_MAX_BLOCK, ENC_MAX_JOBS);
    fflush(stdout);

    receive_loop(attach);           /* never returns; stop the server with a signal */
    return 0;
}
