/*
 * client.c - encclient: sends a Data Block to encserver and checks the result.
 *
 *   encclient [-k keyfile] (-f file | -s bytes) [-n runs] [-W warmups] [-o outfile] [-c] [-V]
 *
 * The request is asynchronous for the client: a sender thread makes the
 * blocking MsgSendv call, and the main thread is notified through a condition
 * variable when the reply arrives.
 *
 * Every result can be checked two ways:
 *   1. it must equal the Reference Encryption of the Data Block, and
 *   2. decrypting it must give back the original Data Block.
 * The first run is always checked; -V checks every run.
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "cipher.h"
#include "config.h"
#include "job.h"
#include "protocol.h"
#include "util.h"

/* One in-flight request, shared between the main thread and its sender thread. */
typedef struct {
    int coid;
    const uint8_t *data;
    size_t len;
    uint8_t *encrypted;

    pthread_t sender;
    pthread_mutex_t mutex;
    pthread_cond_t done_cv;
    bool done;                      /* guarded by mutex */

    /* Written by the sender thread before done is set. */
    encsvc_reply_t reply;
    int err;
    uint64_t round_trip_ns;         /* Round-Trip Time */
} request_t;

static void *sender_thread(void *arg)
{
    request_t *req = arg;
    encsvc_request_t hdr = { .type = ENCSVC_MSG_ENCRYPT, .data_len = (uint32_t)req->len };
    iov_t siov[2], riov[2];
    uint64_t start;
    long status;

    SETIOV(&siov[0], &hdr, sizeof hdr);
    SETIOV(&siov[1], (void *)req->data, req->len);
    SETIOV(&riov[0], &req->reply, sizeof req->reply);
    SETIOV(&riov[1], req->encrypted, req->len);

    start = now_ns();
    status = MsgSendv(req->coid, siov, 2, riov, 2);   /* blocks this thread only */
    req->round_trip_ns = now_ns() - start;

    if (status == -1) {
        req->err = errno;
    } else if (req->reply.data_len != req->len) {
        req->err = EBADMSG;
    }

    pthread_mutex_lock(&req->mutex);
    req->done = true;
    pthread_cond_signal(&req->done_cv);
    pthread_mutex_unlock(&req->mutex);
    return NULL;
}

static int request_start(request_t *req, int coid, const uint8_t *data, size_t len,
                         uint8_t *encrypted)
{
    memset(req, 0, sizeof *req);
    req->coid = coid;
    req->data = data;
    req->len = len;
    req->encrypted = encrypted;
    pthread_mutex_init(&req->mutex, NULL);
    pthread_cond_init(&req->done_cv, NULL);
    return pthread_create(&req->sender, NULL, sender_thread, req);
}

/* Wait for the reply. If show_progress, the main thread does visible work meanwhile. */
static void request_wait(request_t *req, bool show_progress)
{
    unsigned ticks = 0;

    pthread_mutex_lock(&req->mutex);
    while (!req->done) {
        if (show_progress) {
            /* Wake every 5 ms to show the main thread is free while the server works. */
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += 5 * 1000 * 1000;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }
            if (pthread_cond_timedwait(&req->done_cv, &req->mutex, &deadline) == ETIMEDOUT) {
                ticks++;
            }
        } else {
            pthread_cond_wait(&req->done_cv, &req->mutex);
        }
    }
    pthread_mutex_unlock(&req->mutex);

    pthread_join(req->sender, NULL);
    pthread_cond_destroy(&req->done_cv);
    pthread_mutex_destroy(&req->mutex);

    if (show_progress) {
        printf("encclient: main thread stayed free for %u ticks of 5 ms while waiting\n", ticks);
    }
}

/* Returns true if the Encrypted Block is correct. */
static bool verify(const uint8_t key[ENC_KEY_LEN], const request_t *req, uint8_t *scratch)
{
    if (reference_encrypt(key, req->reply.iv, req->data, scratch, req->len) != 0 ||
        memcmp(scratch, req->encrypted, req->len) != 0) {
        fprintf(stderr, "encclient: result differs from Reference Encryption\n");
        return false;
    }
    if (cipher_ctr(key, req->reply.iv, 0, req->encrypted, scratch, req->len) != 0 ||
        memcmp(scratch, req->data, req->len) != 0) {
        fprintf(stderr, "encclient: decrypting the result does not give the Data Block\n");
        return false;
    }
    return true;
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long size;

    if (f == NULL) {
        perror(path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        perror(path);
        fclose(f);
        return NULL;
    }
    if ((unsigned long)size > ENC_MAX_BLOCK) {
        fprintf(stderr, "encclient: %s is %ld bytes; the limit is %u\n", path, size, ENC_MAX_BLOCK);
        fclose(f);
        return NULL;
    }
    buf = malloc(size > 0 ? (size_t)size : 1);
    if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "encclient: cannot read %s\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)size;
    return buf;
}

/* Output file format: the 16-byte IV, then the Encrypted Block. */
static int write_output(const char *path, const request_t *req)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL || fwrite(req->reply.iv, 1, ENC_IV_LEN, f) != ENC_IV_LEN ||
        fwrite(req->encrypted, 1, req->len, f) != req->len) {
        perror(path);
        if (f != NULL) {
            fclose(f);
        }
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: encclient [-k keyfile] (-f file | -s bytes) [-n runs] [-W warmups]\n"
            "                 [-o outfile] [-c] [-V]\n"
            "  -k keyfile  32-byte key file shared with the server (default: ./enc.key)\n"
            "  -f file     encrypt this file (at most %u bytes)\n"
            "  -s bytes    encrypt this many random bytes instead\n"
            "  -n runs     measured requests to send (default: 1)\n"
            "  -W warmups  unmeasured requests sent first (default: 0)\n"
            "  -o outfile  write IV + Encrypted Block of the last run to outfile\n"
            "  -c          print one CSV row per run on stdout (benchmark mode)\n"
            "  -V          verify every run, not only the first\n",
            ENC_MAX_BLOCK);
    exit(2);
}

int main(int argc, char **argv)
{
    const char *keyfile = "enc.key", *infile = NULL, *outfile = NULL;
    size_t len = 0;
    unsigned runs = 1, warmups = 0;
    bool csv = false, verify_all = false;
    uint8_t key[ENC_KEY_LEN];
    uint8_t *data, *encrypted, *scratch;
    uint64_t enc_sum = 0, rtt_sum = 0, enc_min = UINT64_MAX, rtt_min = UINT64_MAX;
    request_t req;
    int coid, opt, err;
    bool ok = true;

    while ((opt = getopt(argc, argv, "k:f:s:n:W:o:cV")) != -1) {
        switch (opt) {
        case 'k': keyfile = optarg; break;
        case 'f': infile = optarg; break;
        case 's': len = strtoul(optarg, NULL, 10); break;
        case 'n': runs = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'W': warmups = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'o': outfile = optarg; break;
        case 'c': csv = true; break;
        case 'V': verify_all = true; break;
        default: usage();
        }
    }
    if ((infile == NULL) == (len == 0) || runs == 0) {
        usage();
    }
    if (infile == NULL && len > ENC_MAX_BLOCK) {
        fprintf(stderr, "encclient: -s %zu exceeds the limit of %u bytes\n", len, ENC_MAX_BLOCK);
        return 1;
    }

    err = keyfile_load(keyfile, key);
    if (err != 0) {
        fprintf(stderr, "encclient: cannot load key file %s: %s\n", keyfile, strerror(err));
        return 1;
    }
    if (infile != NULL) {
        data = read_file(infile, &len);
        if (data == NULL) {
            return 1;
        }
    } else {
        data = malloc(len);
        if (data == NULL || random_bytes(data, len) != 0) {
            fprintf(stderr, "encclient: cannot create %zu random bytes\n", len);
            return 1;
        }
    }
    encrypted = malloc(len > 0 ? len : 1);
    scratch = malloc(len > 0 ? len : 1);
    if (encrypted == NULL || scratch == NULL) {
        fprintf(stderr, "encclient: out of memory\n");
        return 1;
    }

    coid = name_open(ENCSVC_NAME, 0);
    if (coid == -1) {
        perror("encclient: name_open(\"" ENCSVC_NAME "\") - is encserver running?");
        return 1;
    }

    for (unsigned i = 0; i < warmups + runs && ok; i++) {
        bool measured = i >= warmups;
        unsigned run = i - warmups + 1;

        if (request_start(&req, coid, data, len, encrypted) != 0) {
            fprintf(stderr, "encclient: cannot start sender thread\n");
            ok = false;
            break;
        }
        if (!csv && measured) {
            printf("encclient: sent %zu bytes, waiting for the reply...\n", len);
        }
        request_wait(&req, !csv && measured);

        if (req.err != 0) {
            fprintf(stderr, "encclient: request failed: %s\n", strerror(req.err));
            ok = false;
            break;
        }
        if (!measured) {
            continue;
        }

        bool checked = run == 1 || verify_all;
        if (checked && !verify(key, &req, scratch)) {
            ok = false;
        }

        enc_sum += req.reply.encryption_ns;
        rtt_sum += req.round_trip_ns;
        if (req.reply.encryption_ns < enc_min) enc_min = req.reply.encryption_ns;
        if (req.round_trip_ns < rtt_min) rtt_min = req.round_trip_ns;

        if (csv) {
            printf("%u,%zu,%u,%u,%.1f,%.1f,%s\n", req.reply.workers, len, req.reply.segments,
                   run, (double)req.reply.encryption_ns / 1e3, (double)req.round_trip_ns / 1e3,
                   checked ? (ok ? "pass" : "FAIL") : "-");
        } else {
            printf("encclient: run %u: %u workers, %u segments, encryption %.3f ms, "
                   "round trip %.3f ms%s\n",
                   run, req.reply.workers, req.reply.segments,
                   (double)req.reply.encryption_ns / 1e6, (double)req.round_trip_ns / 1e6,
                   checked ? (ok ? ", verified" : ", VERIFY FAILED") : "");
        }
    }

    if (ok && runs > 1) {
        fprintf(stderr, "encclient: %u runs, %u workers: encryption mean %.3f ms (min %.3f), "
                        "round trip mean %.3f ms (min %.3f)\n",
                runs, req.reply.workers, (double)enc_sum / runs / 1e6, (double)enc_min / 1e6,
                (double)rtt_sum / runs / 1e6, (double)rtt_min / 1e6);
    }
    if (ok && outfile != NULL && write_output(outfile, &req) != 0) {
        ok = false;
    }

    name_close(coid);
    free(data);
    free(encrypted);
    free(scratch);
    return ok ? 0 : 1;
}
