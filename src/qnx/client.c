/*
 * client.c - encclient: encrypts files through encserver, decrypts them again,
 * and benchmarks the server.
 *
 *   encclient -e file [-o out] [-j n] [-V] [-x]     encrypt a file   -> file.enc
 *   encclient -d file.enc [-o out] [-x]             decrypt it again -> file
 *   encclient -s bytes [-n runs] [-W warmups] [-V] [-c csv] [-S]   benchmark
 *
 * File mode is in client_file.c, the protocol in client_msg.c. The benchmark
 * sends one random Data Block per run. Each request is asynchronous for the
 * client: a sender thread does the exchange with the server (ENCRYPT, wait for
 * the server's pulse, FETCH), and the main thread is notified through a
 * condition variable when the Encrypted Block has arrived.
 *
 * A benchmark result is checked two ways:
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
#include "client.h"
#include "config.h"
#include "display.h"
#include "job.h"
#include "protocol.h"
#include "sync.h"
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
    encsvc_result_t reply;
    int err;
    uint64_t round_trip_ns;         /* Round-Trip Time */
} request_t;

static void *sender_thread(void *arg)
{
    request_t *req = arg;
    encsvc_notify_t notify;
    uint64_t start;
    int close_err, err = encsvc_notify_open(&notify, req->coid);

    if (err == 0) {
        start = now_ns();
        err = encsvc_encrypt(req->coid, &notify, req->data, req->encrypted, req->len, &req->reply);
        req->round_trip_ns = now_ns() - start;
        close_err = encsvc_notify_close(&notify);
        if (err == 0) {
            err = close_err;
        }
    }
    req->err = err;

    enc_lock(&req->mutex);
    req->done = true;
    enc_cond_signal(&req->done_cv);
    enc_unlock(&req->mutex);
    return NULL;
}

/* Start the sender thread for one request. Returns 0 or an errno value. */
static int request_start(request_t *req, int coid, const uint8_t *data, size_t len,
                         uint8_t *encrypted)
{
    int err;

    memset(req, 0, sizeof *req);
    req->coid = coid;
    req->data = data;
    req->len = len;
    req->encrypted = encrypted;
    err = enc_mutex_cond_init(&req->mutex, &req->done_cv);
    if (err != 0) {
        return err;
    }
    err = pthread_create(&req->sender, NULL, sender_thread, req);
    if (err != 0) {
        enc_cond_destroy(&req->done_cv);
        enc_mutex_destroy(&req->mutex);
    }
    return err;
}

/*
 * Wait for the reply. The main thread wakes every 5 ms meanwhile, to show it
 * is free while the server works. Returns how many 5 ms ticks passed.
 */
static unsigned request_wait(request_t *req, bool show_ticks)
{
    unsigned ticks = 0;

    enc_lock(&req->mutex);
    while (!req->done) {
        struct timespec deadline;

        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
            enc_fatal("clock_gettime", errno);
        }
        deadline.tv_nsec += 5 * 1000 * 1000;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        if (enc_cond_timedwait(&req->done_cv, &req->mutex, &deadline) == ETIMEDOUT) {
            ticks++;
            if (show_ticks && ui_live()) {
                printf("\r  %s%-12s%s free while waiting: %u tick%s of 5 ms", ui(UI_DIM),
                       "main thread", ui(UI_RESET), ticks, ui_s(ticks));
                fflush(stdout);
            }
        }
    }
    enc_unlock(&req->mutex);

    enc_join(req->sender);
    enc_cond_destroy(&req->done_cv);
    enc_mutex_destroy(&req->mutex);
    if (show_ticks && ui_live()) {
        printf("\r\033[K");         /* clear the live line; the final row replaces it */
    }
    return ticks;
}

/* Returns true if the Encrypted Block is correct. */
static bool verify(const uint8_t key[ENC_KEY_LEN], const request_t *req, uint8_t *scratch)
{
    if (reference_encrypt(key, req->reply.iv, req->data, scratch, req->len) != 0 ||
        memcmp(scratch, req->encrypted, req->len) != 0) {
        ui_error("encclient", "Encrypted Block differs from the Reference Encryption");
        return false;
    }
    if (cipher_ctr(key, req->reply.iv, 0, req->encrypted, scratch, req->len) != 0 ||
        memcmp(scratch, req->data, req->len) != 0) {
        ui_error("encclient", "decrypting the result does not give the Data Block back");
        return false;
    }
    return true;
}

typedef struct {
    size_t len;
    unsigned runs, warmups;
    bool verify_all;
    const char *csv_path;           /* -c: append one row per run */
    bool summary_only;              /* -S: print one machine-readable line */
} bench_options_t;

typedef struct {
    uint64_t sum, min, max;
} stat_t;

static void stat_add(stat_t *s, uint64_t v)
{
    s->sum += v;
    if (v < s->min) s->min = v;
    if (v > s->max) s->max = v;
}

static int benchmark(int coid, const uint8_t key[ENC_KEY_LEN], const bench_options_t *opt)
{
    uint8_t *data = malloc(opt->len), *encrypted = malloc(opt->len), *scratch = malloc(opt->len);
    bool human = !opt->summary_only, ok = true;
    stat_t enc = { 0, UINT64_MAX, 0 }, rtt = { 0, UINT64_MAX, 0 };
    FILE *csv = NULL;
    request_t req;
    unsigned ticks = 0, done = 0;
    char a[32], b[32], c[32], title[128];

    if (data == NULL || encrypted == NULL || scratch == NULL || random_bytes(data, opt->len) != 0) {
        ui_error("encclient", "cannot create %zu random bytes", opt->len);
        ok = false;
        goto out;
    }
    if (opt->csv_path != NULL && (csv = fopen(opt->csv_path, "a")) == NULL) {
        ui_error("encclient", "cannot open %s: %s", opt->csv_path, strerror(errno));
        ok = false;
        goto out;
    }

    if (human) {
        snprintf(title, sizeof title, "%s random Data Block, %u run%s%s", ui_size(a, sizeof a, opt->len),
                 opt->runs, ui_s(opt->runs), opt->warmups ? " after warm-up" : "");
        ui_title("encclient", "benchmark", title);
        if (opt->runs == 1) {
            ui_row("sent", "%s (%s bytes), waiting for the reply...", ui_size(a, sizeof a, opt->len),
                   ui_count(b, sizeof b, opt->len));
        }
    }

    for (unsigned i = 0; i < opt->warmups + opt->runs && ok; i++) {
        bool measured = i >= opt->warmups;
        unsigned run = i - opt->warmups + 1;
        bool checked = measured && (run == 1 || opt->verify_all);

        int err = request_start(&req, coid, data, opt->len, encrypted);

        if (err != 0) {
            ui_error("encclient", "cannot start a sender thread: %s", strerror(err));
            ok = false;
            break;
        }
        ticks = request_wait(&req, human && opt->runs == 1);
        if (req.err != 0) {
            if (human && opt->runs > 1 && ui_live()) {
                putchar('\n');
            }
            ui_error("encclient", "request failed: %s", encsvc_strerror(req.err));
            ok = false;
            break;
        }
        if (!measured) {
            continue;
        }
        if (checked && !verify(key, &req, scratch)) {
            ok = false;
        }
        stat_add(&enc, req.reply.encryption_ns);
        stat_add(&rtt, req.round_trip_ns);
        done++;
        if (csv != NULL &&
            fprintf(csv, "%u,%zu,%u,%u,%.1f,%.1f,%s\n", req.reply.workers, opt->len,
                    req.reply.segments, run, (double)req.reply.encryption_ns / 1e3,
                    (double)req.round_trip_ns / 1e3, checked ? (ok ? "pass" : "FAIL") : "-") < 0) {
            ui_error("encclient", "cannot write %s: %s", opt->csv_path, strerror(errno));
            ok = false;
        }
        if (human && opt->runs > 1) {
            ui_progress("progress", run, opt->runs, "runs");
        }
    }
    if (!ok || done == 0) {
        goto out;
    }

    if (opt->summary_only) {
        /* workers bytes runs enc_mean_us enc_min_us rtt_mean_us rtt_min_us */
        printf("%u %zu %u %llu %llu %llu %llu\n", req.reply.workers, opt->len, done,
               (unsigned long long)(enc.sum / done / 1000), (unsigned long long)(enc.min / 1000),
               (unsigned long long)(rtt.sum / done / 1000), (unsigned long long)(rtt.min / 1000));
        goto out;
    }

    if (opt->runs == 1) {
        ui_row("main thread", "free for %u tick%s of 5 ms while the server worked", ticks, ui_s(ticks));
        ui_row("server", "%u workers, %u Segment%s", req.reply.workers, req.reply.segments,
               ui_s(req.reply.segments));
        ui_row("encryption", "%s on the server  (%s)", ui_ms(a, sizeof a, enc.sum),
               ui_rate(c, sizeof c, opt->len, enc.sum));
        ui_row("round trip", "%s on the client", ui_ms(b, sizeof b, rtt.sum));
        ui_row("check", "%s%s verified%s: same as the Reference Encryption, and decrypts back",
               ui(UI_GREEN), ui_ok(), ui(UI_RESET));
    } else {
        ui_row("server", "%u workers, %u Segment%s per run", req.reply.workers, req.reply.segments,
               ui_s(req.reply.segments));
        ui_row("encryption", "mean %s   min %s   max %s", ui_ms(a, sizeof a, enc.sum / done),
               ui_ms(b, sizeof b, enc.min), ui_ms(c, sizeof c, enc.max));
        ui_row("round trip", "mean %s   min %s   max %s", ui_ms(a, sizeof a, rtt.sum / done),
               ui_ms(b, sizeof b, rtt.min), ui_ms(c, sizeof c, rtt.max));
        ui_row("speed", "%s (mean encryption)", ui_rate(a, sizeof a, opt->len, enc.sum / done));
        if (opt->verify_all) {
            ui_row("check", "%s%s all %u runs verified%s", ui(UI_GREEN), ui_ok(), done, ui(UI_RESET));
        } else {
            ui_row("check", "%s%s run 1 verified%s  (add -V to check every run)", ui(UI_GREEN),
                   ui_ok(), ui(UI_RESET));
        }
    }

out:
    if (csv != NULL && fclose(csv) != 0 && ok) {
        ui_error("encclient", "cannot write %s: %s", opt->csv_path, strerror(errno));
        ok = false;
    }
    free(data);
    free(encrypted);
    free(scratch);
    return ok ? 0 : 1;
}

static int usage(void)
{
    fprintf(stderr,
            "usage:\n"
            "  encclient -e file [-o out.enc] [-j n] [-V] [-x]   encrypt a file (default out: ./file.enc)\n"
            "  encclient -d file.enc [-o out] [-x]               decrypt it (default out: ./<original name>)\n"
            "  encclient -s bytes [-n runs] [-W warmups] [-V] [-c file.csv] [-S]   benchmark\n"
            "\n"
            "  -k keyfile  32-byte key file shared with the server (default: ./enc.key)\n"
            "  -o out      output file name\n"
            "  -j n        Data Blocks in flight at once, 1 to %u (default: 4)\n"
            "  -x          show the first 16 bytes before and after\n"
            "  -V          verify every Data Block / run against the Reference Encryption\n"
            "  -s bytes    benchmark with one random Data Block of this size (at most %u)\n"
            "  -n runs     measured requests (default: 1)\n"
            "  -W warmups  unmeasured requests sent first (default: 0)\n"
            "  -c file     append one CSV row per run to file\n"
            "  -S          print only a summary line: workers bytes runs enc_mean_us enc_min_us\n"
            "              rtt_mean_us rtt_min_us\n",
            ENC_MAX_JOBS, ENC_MAX_BLOCK);
    return 2;
}

int main(int argc, char **argv)
{
    const char *keyfile = "enc.key", *encrypt_path = NULL, *decrypt_path = NULL;
    bench_options_t bench = { .runs = 1 };
    file_options_t file = { .in_flight = 4 };
    uint8_t key[ENC_KEY_LEN];
    int coid, opt, err, modes, status;

    while ((opt = getopt(argc, argv, "k:e:d:o:j:xs:n:W:c:SV")) != -1) {
        switch (opt) {
        case 'k': keyfile = optarg; break;
        case 'e': encrypt_path = optarg; break;
        case 'd': decrypt_path = optarg; break;
        case 'o': file.out_path = optarg; break;
        case 'j': file.in_flight = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'x': file.preview = true; break;
        case 's': bench.len = strtoul(optarg, NULL, 10); break;
        case 'n': bench.runs = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'W': bench.warmups = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'c': bench.csv_path = optarg; break;
        case 'S': bench.summary_only = true; break;
        case 'V': file.verify = bench.verify_all = true; break;
        default: return usage();
        }
    }
    modes = (encrypt_path != NULL) + (decrypt_path != NULL) + (bench.len != 0);
    if (modes != 1 || optind != argc || bench.runs == 0 || file.in_flight == 0 ||
        file.in_flight > ENC_MAX_JOBS) {
        return usage();
    }
    ui_init();
    if (bench.len > ENC_MAX_BLOCK) {
        ui_error("encclient", "-s %zu exceeds the limit of %u bytes", bench.len, ENC_MAX_BLOCK);
        return 1;
    }

    err = keyfile_load(keyfile, key);
    if (err != 0) {
        ui_error("encclient", "cannot load key file %s: %s", keyfile, strerror(err));
        return 1;
    }
    file.key = key;

    if (decrypt_path != NULL) {
        return decrypt_file(decrypt_path, &file);   /* needs no server */
    }

    coid = name_open(ENCSVC_NAME, 0);
    if (coid == -1) {
        ui_error("encclient", "cannot reach \"%s\": %s. Is encserver running?", ENCSVC_NAME,
                 strerror(errno));
        return 1;
    }
    if (encrypt_path != NULL) {
        status = encrypt_file(coid, encrypt_path, &file);
    } else {
        status = benchmark(coid, key, &bench);
    }
    if (name_close(coid) == -1) {
        ui_error("encclient", "name_close: %s", strerror(errno));
        status = 1;
    }
    return status;
}
