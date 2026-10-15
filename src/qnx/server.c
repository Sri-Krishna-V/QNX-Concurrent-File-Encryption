/*
 * server.c - encserver: the Concurrent Encryption Service.
 *
 *   encserver [-w workers] [-k keyfile] [-v]
 *
 * With -v it prints one line when a Job arrives and one when it finishes,
 * showing how many Segments each worker encrypted.
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
#include "display.h"
#include "job.h"
#include "protocol.h"
#include "util.h"
#include "worker_pool.h"

typedef struct {
    job_t job;
    unsigned id;                    /* Job number, for the log */
    pid_t client_pid;
    rcvid_t rcvid;                  /* client to reply to when the Job completes */
    uint8_t *buffers;               /* Data Block followed by Encrypted Block */
} server_job_t;

static struct {
    worker_pool_t *pool;
    uint8_t key[ENC_KEY_LEN];
    bool verbose;
    unsigned workers;
    unsigned next_job_id;           /* used by the Receive Thread only */

    pthread_mutex_t log_mutex;      /* keeps log lines from different threads whole */
    pthread_mutex_t jobs_mutex;
    pthread_cond_t slot_free_cv;    /* signalled when active_jobs drops */
    unsigned active_jobs;           /* guarded by jobs_mutex */
} server = {
    .log_mutex = PTHREAD_MUTEX_INITIALIZER,
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

/* Print one whole log line; lines come from the Receive Thread and every Job Coordinator. */
static void log_line(const char *line)
{
    pthread_mutex_lock(&server.log_mutex);
    fputs(line, stdout);
    fflush(stdout);
    pthread_mutex_unlock(&server.log_mutex);
}

/* Start of every Job line: "  #12  ✔ pid 4231      2.0 MB  ". */
static int log_prefix(char *buf, size_t cap, unsigned id, const char *symbol,
                      ui_style_t colour, pid_t pid, size_t len)
{
    char size[16];

    return snprintf(buf, cap, "  %s#%-4u%s %s%-2s%s pid %-7d %8s  ", ui(UI_BOLD), id, ui(UI_RESET),
                    ui(colour), symbol, ui(UI_RESET), (int)pid, ui_size(size, sizeof size, len));
}

static void log_received(const server_job_t *sj, unsigned in_progress)
{
    char line[256];
    int n = log_prefix(line, sizeof line, sj->id, ui_arrow(), UI_CYAN, sj->client_pid, sj->job.len);

    snprintf(line + n, sizeof line - n, "%sreceived, %u Job%s in progress%s\n", ui(UI_DIM),
             in_progress, ui_s(in_progress), ui(UI_RESET));
    log_line(line);
}

static void log_failed(unsigned id, pid_t pid, size_t len, const char *what, int err)
{
    char line[256];
    int n = log_prefix(line, sizeof line, id, ui_fail(), UI_RED, pid, len);

    snprintf(line + n, sizeof line - n, "%s%s: %s%s\n", ui(UI_RED), what, strerror(err),
             ui(UI_RESET));
    log_line(line);
}

/* "  #12  ✔ pid 4231      2.0 MB         16    50.064 ms   w0 ████ 4  w1 ███ 3 ..." */
static void log_done(const server_job_t *sj)
{
    char line[4096], time[24], bar[64];   /* room for ENC_MAX_WORKERS columns */
    int n = log_prefix(line, sizeof line, sj->id, ui_ok(), UI_GREEN, sj->client_pid, sj->job.len);

    n += snprintf(line + n, sizeof line - n, "%9zu  %11s  ", sj->job.total_segments,
                  ui_ms(time, sizeof time, sj->job.encryption_ns));
    for (unsigned w = 0; w < server.workers && w < ENC_MAX_WORKERS && n < (int)sizeof line; w++) {
        unsigned count = sj->job.worker_segments[w];
        unsigned cells = count < 8 ? count : 8;

        n += snprintf(line + n, sizeof line - n, " %sw%u%s %s%s%s%s%u ", ui(UI_DIM), w, ui(UI_RESET),
                      ui(UI_CYAN), ui_bar(bar, sizeof bar, cells, cells), ui(UI_RESET),
                      cells > 0 ? " " : "", count);
    }
    if (n < (int)sizeof line) {
        snprintf(line + n, sizeof line - n, "\n");
    }
    log_line(line);
}

/*
 * Deferred reply: send the Encrypted Block, or the error, to the waiting client.
 * Returns 0, or the errno of a failed reply.
 */
static int send_reply(server_job_t *sj, int err)
{
    encsvc_reply_t reply;
    iov_t iov[2];

    if (err != 0) {
        MsgError(sj->rcvid, err);
        return 0;
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
        return errno;               /* Abandoned Job: the client went away */
    }
    return 0;
}

/* Job Coordinator thread: one per Job. */
static void *job_coordinator(void *arg)
{
    server_job_t *sj = arg;
    int err = job_run(&sj->job, server.pool);
    int reply_err = send_reply(sj, err);

    if (reply_err != 0) {
        /* Abandoned Job: logged even without -v, the Encrypted Block is discarded. */
        log_failed(sj->id, sj->client_pid, sj->job.len, "client gone, Encrypted Block discarded", reply_err);
    } else if (err != 0) {
        log_failed(sj->id, sj->client_pid, sj->job.len, "encryption failed", err);
    } else if (server.verbose) {
        log_done(sj);
    }
    server_job_free(sj);
    release_job_slot();
    return NULL;
}

/*
 * Handle one ENCRYPT request on the Receive Thread. Takes ownership of the
 * job slot: either a Job Coordinator releases it later, or this function
 * replies with an error and releases it now.
 */
static void handle_encrypt(rcvid_t rcvid, const encsvc_request_t *req, const struct _msg_info *info)
{
    unsigned id = ++server.next_job_id;
    unsigned in_progress;
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
    sj->id = id;
    sj->client_pid = info->pid;

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
    if (server.verbose) {
        pthread_mutex_lock(&server.jobs_mutex);
        in_progress = server.active_jobs;
        pthread_mutex_unlock(&server.jobs_mutex);
        log_received(sj, in_progress);   /* before the Coordinator can log "done" */
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
    log_failed(id, info->pid, len, "rejected", err);
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
        handle_encrypt(rcvid, &msg.request, &info);
    }
}

static int usage(void)
{
    fprintf(stderr,
            "usage: encserver [-w workers] [-k keyfile] [-v]\n"
            "  -w workers  Worker Pool size, 1 to %u (default: number of CPUs)\n"
            "  -k keyfile  32-byte key file (default: ./enc.key)\n"
            "  -v          live log: one line when a Job arrives, one when it is done\n",
            ENC_MAX_WORKERS);
    return 2;
}

static void print_banner(unsigned workers)
{
    char block[16];

    printf("\n%sencserver ready%s\n", ui(UI_GREEN), ui(UI_RESET));
    printf("  %-9s %s/dev/name/local/%s%s\n", "name", ui(UI_BOLD), ENCSVC_NAME, ui(UI_RESET));
    printf("  %-9s %u\n", "workers", workers);
    printf("  %-9s AES-256-CTR, %s backend, self-test passed\n", "cipher", cipher_backend_name());
    printf("  %-9s %s per Data Block, %u Jobs at once\n", "limits",
           ui_size(block, sizeof block, ENC_MAX_BLOCK), ENC_MAX_JOBS);
    if (server.verbose) {
        printf("\n  %s%-5s  %-12s %8s  %9s  %11s   %s%s\n", ui(UI_BOLD), "JOB", " CLIENT", "SIZE",
               "SEGMENTS", "TIME", "WORKERS (segments each)", ui(UI_RESET));
    } else {
        printf("  %-9s off (start with -v to see every Job)\n", "log");
    }
    fflush(stdout);
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
        default: return usage();
        }
    }
    if (workers == 0 || workers > ENC_MAX_WORKERS) {
        return usage();
    }
    server.workers = workers;
    ui_init();

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
        } else if (errno == EEXIST) {
            fprintf(stderr, "encserver: another encserver is already running (slay encserver)\n");
        }
        return 1;
    }

    print_banner(workers);

    receive_loop(attach);           /* never returns; stop the server with a signal */
    return 0;
}
