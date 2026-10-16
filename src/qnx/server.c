/*
 * server.c - encserver: the Concurrent Encryption Service.
 *
 *   encserver [-w workers] [-k keyfile] [-v]
 *
 * With -v it prints one line when a Job arrives and one when it finishes,
 * showing how many Segments each worker encrypted.
 *
 * Protocol (protocol.h): ENCRYPT is answered at once with a Job id; when the
 * Job is done the server delivers the client's registered pulse with
 * MsgDeliverEvent(); the client then sends FETCH and gets the Encrypted Block.
 *
 * Threads:
 *   Receive Thread    main(): the MsgReceive loop. Handles ENCRYPT, FETCH and
 *                     pulses, and never waits for a Job.
 *   Job Coordinator   one detached thread per Job: runs the Job on the Worker Pool,
 *                     waits for completion, then delivers the client's event.
 *   Worker Pool       encrypts Segments (src/core/worker_pool.c, src/core/job.c).
 *
 * The server holds at most ENC_MAX_JOBS Jobs, from accepting a Data Block until
 * its Encrypted Block is fetched. Further ENCRYPT requests wait in a FIFO, with
 * their clients reply-blocked, until a slot frees up.
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
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
#include "sync.h"
#include "util.h"
#include "worker_pool.h"

typedef struct server_job {
    job_t job;
    unsigned id;                    /* Job number: the client quotes it in FETCH */
    pid_t client_pid;
    int scoid;                      /* the client's connection; FETCH must come on it */
    rcvid_t rcvid;                  /* the ENCRYPT message, kept for MsgDeliverEvent */
    struct sigevent event;          /* the client's registered pulse */
    uint8_t *buffers;               /* Data Block followed by Encrypted Block */
    bool done;                      /* guarded by jobs_mutex */
    bool abandoned;                 /* guarded by jobs_mutex: client gone, free when done */
    int err;                        /* job_run() result, valid once done */
    struct server_job *next;        /* guarded by jobs_mutex */
} server_job_t;

/* An ENCRYPT request waiting for a Job slot. Its client stays reply-blocked. */
typedef struct waiting {
    rcvid_t rcvid;
    int scoid;
    pid_t client_pid;
    encsvc_encrypt_t request;
    struct waiting *next;
} waiting_t;

static struct {
    worker_pool_t *pool;
    uint8_t key[ENC_KEY_LEN];
    bool verbose;
    unsigned workers;

    pthread_mutex_t log_mutex;      /* keeps log lines from different threads whole */

    pthread_mutex_t jobs_mutex;     /* guards everything below */
    server_job_t *jobs;             /* every Job held: running, or done and not yet fetched */
    unsigned held;                  /* Job slots in use, including ones being set up */
    unsigned next_job_id;
    waiting_t *wait_head, *wait_tail;
    unsigned waiting;
} server = {
    .log_mutex = PTHREAD_MUTEX_INITIALIZER,
    .jobs_mutex = PTHREAD_MUTEX_INITIALIZER,
};

/* ---------------------------------------------------------------- log */

/* Print one whole log line; lines come from the Receive Thread and every Job Coordinator. */
static void log_line(const char *line)
{
    enc_lock(&server.log_mutex);
    fputs(line, stdout);
    fflush(stdout);
    enc_unlock(&server.log_mutex);
}

/* Start of every Job line: "  #12  ✔ pid 4231      2.0 MB  ". */
static int log_prefix(char *buf, size_t cap, unsigned id, const char *symbol,
                      ui_style_t colour, pid_t pid, size_t len)
{
    char size[16];
    int n = snprintf(buf, cap, "  %s#%-4u%s %s%-2s%s pid %-7d %8s  ", ui(UI_BOLD), id, ui(UI_RESET),
                     ui(colour), symbol, ui(UI_RESET), (int)pid, ui_size(size, sizeof size, len));

    return n < 0 ? 0 : (n >= (int)cap ? (int)cap - 1 : n);
}

static void log_note(unsigned id, pid_t pid, size_t len, const char *symbol, const char *what)
{
    char line[256];
    int n = log_prefix(line, sizeof line, id, symbol, UI_CYAN, pid, len);

    snprintf(line + n, sizeof line - n, "%s%s%s\n", ui(UI_DIM), what, ui(UI_RESET));
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

/* ---------------------------------------------------------------- replies */

/*
 * MsgError fails only when the client has gone away (its process died or it
 * was unblocked by a signal), and then there is nobody left to tell.
 */
static void reply_error(rcvid_t rcvid, int err)
{
    if (MsgError(rcvid, err) == -1 && server.verbose) {
        log_failed(0, 0, 0, "could not report an error to a client", errno);
    }
}

/* ---------------------------------------------------------------- Job table (jobs_mutex held) */

static void unlink_job_locked(server_job_t *sj)
{
    for (server_job_t **p = &server.jobs; *p != NULL; p = &(*p)->next) {
        if (*p == sj) {
            *p = sj->next;
            server.held--;
            return;
        }
    }
}

static server_job_t *find_job_locked(unsigned id, int scoid)
{
    for (server_job_t *sj = server.jobs; sj != NULL; sj = sj->next) {
        if (sj->id == id && sj->scoid == scoid) {
            return sj;
        }
    }
    return NULL;
}

static void server_job_free(server_job_t *sj)
{
    job_destroy(&sj->job);
    free(sj->buffers);
    free(sj);
}

/* ---------------------------------------------------------------- Job Coordinator */

static void admit_waiting(void);

/*
 * A Job has finished (err is job_run()'s result). Log it, mark it done and
 * deliver the client's event, so that the client sends FETCH. If the client
 * has gone, free the Job instead. Runs on the Job's Coordinator thread.
 */
static void finish_job(server_job_t *sj, int err)
{
    bool gone;
    int why = ESRCH;
    const char *what = "client gone, Encrypted Block discarded";

    /* Log first: once done is set, the Receive Thread may hand the Job out and free it. */
    if (err != 0) {
        log_failed(sj->id, sj->client_pid, sj->job.len, "Job failed", err);
    } else if (server.verbose) {
        log_done(sj);
    }

    enc_lock(&server.jobs_mutex);
    sj->done = true;
    sj->err = err;
    gone = sj->abandoned;
    if (!gone && MsgDeliverEvent(sj->rcvid, &sj->event) == -1) {
        why = errno;                /* normally ESRCH: the client went away after ENCRYPT */
        gone = true;
        if (why != ESRCH && why != EBADF && why != ENXIO) {
            what = "cannot deliver the event, Encrypted Block discarded";   /* e.g. EACCES */
        }
    }
    if (gone) {
        unlink_job_locked(sj);
    }
    enc_unlock(&server.jobs_mutex);

    if (gone) {
        /* Abandoned Job: logged even without -v, the Encrypted Block is discarded. */
        log_failed(sj->id, sj->client_pid, sj->job.len, what, why);
        server_job_free(sj);
        admit_waiting();
    }
}

/* Job Coordinator thread: one per Job. */
static void *job_coordinator(void *arg)
{
    server_job_t *sj = arg;

    finish_job(sj, job_run(&sj->job, server.pool));
    return NULL;
}

static int start_coordinator(server_job_t *sj)
{
    pthread_attr_t attr;
    pthread_t tid;
    int destroy_err, err = pthread_attr_init(&attr);

    if (err != 0) {
        return err;
    }
    err = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (err == 0) {
        err = pthread_create(&tid, &attr, job_coordinator, sj);
    }
    /* The result says only whether a Coordinator runs, so a failed destroy must not change it. */
    destroy_err = pthread_attr_destroy(&attr);
    if (destroy_err != 0) {
        enc_fatal("pthread_attr_destroy", destroy_err);
    }
    return err;
}

/* ---------------------------------------------------------------- ENCRYPT */

/*
 * Start a Job for one ENCRYPT request whose Job slot is already reserved:
 * pull the Data Block, answer with the Job id, and start the Job Coordinator.
 * If that fails before the answer, the client gets an error and the slot is
 * released; after the answer, the failure reaches the client through FETCH.
 */
static void admit(const waiting_t *w)
{
    size_t len = w->request.data_len;
    server_job_t *sj = NULL;
    encsvc_accepted_t accepted = { 0 };
    uint8_t iv[ENC_IV_LEN];
    ssize_t got;
    unsigned id, in_progress;
    int err;

    enc_lock(&server.jobs_mutex);
    id = ++server.next_job_id;
    enc_unlock(&server.jobs_mutex);

    sj = calloc(1, sizeof *sj);
    if (sj == NULL || (sj->buffers = malloc(2 * len)) == NULL) {
        err = ENOMEM;
        goto fail;
    }
    sj->id = id;
    sj->client_pid = w->client_pid;
    sj->scoid = w->scoid;
    sj->rcvid = w->rcvid;
    /*
     * Kept as received, not inspected: MsgRegisterEvent() replaced the
     * client's event with a handle, which only the kernel can resolve when we
     * deliver it.
     */
    sj->event = w->request.event;

    /* Pull the Data Block out of the client's send buffer, after the header. */
    got = MsgRead(w->rcvid, sj->buffers, len, sizeof w->request);
    if (got != (ssize_t)len) {
        err = got == -1 ? errno : EBADMSG;
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

    enc_lock(&server.jobs_mutex);
    sj->next = server.jobs;
    server.jobs = sj;
    in_progress = server.held;
    enc_unlock(&server.jobs_mutex);
    if (server.verbose) {
        char what[64];

        snprintf(what, sizeof what, "received, %u Job%s in progress", in_progress, ui_s(in_progress));
        log_note(id, sj->client_pid, len, ui_arrow(), what);
    }

    /* The early answer: the Encrypted Block follows the event, on FETCH. */
    accepted.job_id = id;
    if (MsgReply(w->rcvid, EOK, &accepted, sizeof accepted) == -1) {
        err = errno;
        enc_lock(&server.jobs_mutex);
        unlink_job_locked(sj);      /* no Coordinator yet, so nobody else holds it */
        enc_unlock(&server.jobs_mutex);
        log_failed(id, w->client_pid, len, "client gone before the Job was accepted", err);
        server_job_free(sj);
        return;                     /* the caller admits the next waiting request */
    }

    err = start_coordinator(sj);
    if (err != 0) {
        /* Accepted already, so report the failure the normal way: event, then FETCH. */
        finish_job(sj, err);
    }
    return;

fail:
    reply_error(w->rcvid, err);
    log_failed(id, w->client_pid, len, "rejected", err);
    if (sj != NULL) {
        free(sj->buffers);
        free(sj);
    }
    enc_lock(&server.jobs_mutex);
    server.held--;
    enc_unlock(&server.jobs_mutex);
}

/* Admit waiting ENCRYPT requests, oldest first, while Job slots are free. */
static void admit_waiting(void)
{
    for (;;) {
        waiting_t *w = NULL;

        enc_lock(&server.jobs_mutex);
        if (server.held < ENC_MAX_JOBS && server.wait_head != NULL) {
            w = server.wait_head;
            server.wait_head = w->next;
            if (server.wait_head == NULL) {
                server.wait_tail = NULL;
            }
            server.waiting--;
            server.held++;
        }
        enc_unlock(&server.jobs_mutex);
        if (w == NULL) {
            return;
        }
        admit(w);
        free(w);
    }
}

static void handle_encrypt(rcvid_t rcvid, const encsvc_encrypt_t *req, const struct _msg_info *info)
{
    waiting_t *w;
    bool now = false, queued = false;

    if (req->data_len == 0 || req->data_len > ENC_MAX_BLOCK) {
        int err = req->data_len == 0 ? EINVAL : EMSGSIZE;

        reply_error(rcvid, err);
        log_failed(0, info->pid, req->data_len, "rejected", err);
        return;
    }
    w = calloc(1, sizeof *w);
    if (w == NULL) {
        reply_error(rcvid, ENOMEM);
        return;
    }
    w->rcvid = rcvid;
    w->scoid = info->scoid;
    w->client_pid = info->pid;
    w->request = *req;

    enc_lock(&server.jobs_mutex);
    if (server.held < ENC_MAX_JOBS && server.wait_head == NULL) {
        server.held++;              /* first come, first served: only when nobody waits */
        now = true;
    } else if (server.waiting < ENC_MAX_WAITING) {
        if (server.wait_tail != NULL) {
            server.wait_tail->next = w;
        } else {
            server.wait_head = w;
        }
        server.wait_tail = w;
        server.waiting++;
        queued = true;
    }
    enc_unlock(&server.jobs_mutex);

    if (now) {
        admit(w);
        free(w);
    } else if (!queued) {
        reply_error(rcvid, EBUSY);
        log_failed(0, info->pid, req->data_len, "rejected, too many clients waiting", EBUSY);
        free(w);
    } else if (server.verbose) {
        log_note(0, info->pid, req->data_len, ui_arrow(), "waiting for a free Job slot");
    }
}

/* ---------------------------------------------------------------- FETCH */

static void handle_fetch(rcvid_t rcvid, const encsvc_fetch_t *req, const struct _msg_info *info)
{
    server_job_t *sj;
    encsvc_result_t result;
    iov_t iov[2];
    int err = 0;

    enc_lock(&server.jobs_mutex);
    sj = find_job_locked(req->job_id, info->scoid);
    if (sj == NULL) {
        err = ENOENT;
    } else if (!sj->done) {
        err = EAGAIN;
    } else {
        unlink_job_locked(sj);      /* ours now: nobody else touches a done Job */
    }
    enc_unlock(&server.jobs_mutex);
    if (err != 0) {
        reply_error(rcvid, err);
        return;
    }

    if (sj->err != 0) {
        reply_error(rcvid, EIO);    /* the real cause is in the log; EAGAIN etc. mean something else here */
    } else {
        memset(&result, 0, sizeof result);
        result.data_len = (uint32_t)sj->job.len;
        result.segments = (uint32_t)sj->job.total_segments;
        memcpy(result.iv, sj->job.iv, ENC_IV_LEN);
        result.encryption_ns = sj->job.encryption_ns;
        result.workers = pool_size(server.pool);

        SETIOV(&iov[0], &result, sizeof result);
        SETIOV(&iov[1], sj->job.output, sj->job.len);
        if (MsgReplyv(rcvid, EOK, iov, 2) == -1) {
            log_failed(sj->id, sj->client_pid, sj->job.len, "client gone, Encrypted Block discarded",
                       errno);
        }
    }
    server_job_free(sj);
    admit_waiting();
}

/* ---------------------------------------------------------------- pulses */

/*
 * A client's TimerTimeout expired while its ENCRYPT waited for a Job slot (the
 * channel has _NTO_CHF_UNBLOCK, so the kernel asks us first). Answer it now.
 */
static void handle_unblock(int rcvid_value)
{
    waiting_t *w = NULL;

    enc_lock(&server.jobs_mutex);
    for (waiting_t **p = &server.wait_head, *prev = NULL; *p != NULL; prev = *p, p = &(*p)->next) {
        if ((int)(*p)->rcvid == rcvid_value) {
            w = *p;
            *p = w->next;
            if (server.wait_tail == w) {
                server.wait_tail = prev;
            }
            server.waiting--;
            break;
        }
    }
    enc_unlock(&server.jobs_mutex);
    /* Not waiting: the request is being answered right now, which unblocks the client anyway. */
    if (w != NULL) {
        reply_error(w->rcvid, ETIMEDOUT);
        log_failed(0, w->client_pid, w->request.data_len, "client timed out waiting for a Job slot",
                   ETIMEDOUT);
        free(w);
    }
}

/* A client closed its connection or died: drop everything it left behind. */
static void handle_disconnect(int scoid)
{
    server_job_t *done_list = NULL;

    enc_lock(&server.jobs_mutex);
    for (waiting_t **p = &server.wait_head, *prev = NULL; *p != NULL;) {
        if ((*p)->scoid == scoid) {
            waiting_t *w = *p;

            *p = w->next;
            if (server.wait_tail == w) {
                server.wait_tail = prev;
            }
            server.waiting--;
            free(w);
        } else {
            prev = *p;
            p = &(*p)->next;
        }
    }
    for (server_job_t **p = &server.jobs; *p != NULL;) {
        server_job_t *sj = *p;

        if (sj->scoid != scoid) {
            p = &sj->next;
        } else if (sj->done) {
            *p = sj->next;          /* finished, never fetched: free it below */
            server.held--;
            sj->next = done_list;
            done_list = sj;
        } else {
            sj->abandoned = true;   /* its Coordinator frees it when the Job finishes */
            p = &sj->next;
        }
    }
    enc_unlock(&server.jobs_mutex);

    while (done_list != NULL) {
        server_job_t *sj = done_list;

        done_list = sj->next;
        log_failed(sj->id, sj->client_pid, sj->job.len, "client gone, Encrypted Block discarded", ESRCH);
        server_job_free(sj);
    }
    if (ConnectDetach(scoid) == -1) {
        log_failed(0, 0, 0, "ConnectDetach", errno);
    }
    admit_waiting();
}

/* ---------------------------------------------------------------- Receive Thread */

static void receive_loop(name_attach_t *attach)
{
    union {
        struct _pulse pulse;
        uint16_t type;
        encsvc_encrypt_t encrypt;
        encsvc_fetch_t fetch;
    } msg;

    unsigned failures = 0;

    for (;;) {
        struct _msg_info info;
        rcvid_t rcvid = MsgReceive(attach->chid, &msg, sizeof msg, &info);

        if (rcvid == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("encserver: MsgReceive");
            if (++failures == 10) {
                fprintf(stderr, "encserver: the channel keeps failing; stopping\n");
                exit(1);
            }
            continue;
        }
        failures = 0;
        if (rcvid == 0) {
            switch (msg.pulse.code) {
            case _PULSE_CODE_DISCONNECT:
                handle_disconnect(msg.pulse.scoid);
                break;
            case _PULSE_CODE_UNBLOCK:
                handle_unblock(msg.pulse.value.sival_int);
                break;
            default:
                break;              /* no other pulses are expected */
            }
            continue;
        }
        if (msg.type == _IO_CONNECT) {
            /* Sent by name_open(); accept the connection. */
            if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
                log_failed(0, info.pid, 0, "connect", errno);
            }
        } else if (msg.type == ENCSVC_MSG_ENCRYPT && info.msglen >= (ssize_t)sizeof msg.encrypt) {
            handle_encrypt(rcvid, &msg.encrypt, &info);
        } else if (msg.type == ENCSVC_MSG_FETCH && info.msglen >= (ssize_t)sizeof msg.fetch) {
            handle_fetch(rcvid, &msg.fetch, &info);
        } else {
            reply_error(rcvid, ENOSYS);
        }
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
    if (workers == 0 || workers > ENC_MAX_WORKERS || optind != argc) {
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

    /* The channel gets _NTO_CHF_DISCONNECT and _NTO_CHF_UNBLOCK: we handle both pulses. */
    attach = name_attach(NULL, ENCSVC_NAME, 0);
    if (attach == NULL) {
        err = errno;                /* perror() may change errno */
        perror("encserver: name_attach");
        if (err == EPERM) {
            fprintf(stderr, "encserver: try running as root (su)\n");
        } else if (err == EEXIST) {
            fprintf(stderr, "encserver: another encserver is already running (slay encserver)\n");
        }
        pool_destroy(server.pool);
        return 1;
    }

    print_banner(workers);

    receive_loop(attach);           /* never returns; stop the server with a signal */
    return 0;
}
