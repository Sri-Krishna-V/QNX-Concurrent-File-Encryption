/*
 * test_e2e.c - runs the real server.c and client.c together in one process,
 * on top of qnx_shim.c instead of the QNX kernel.
 *
 * server.c is compiled with -Dmain=encserver_main, client.c with -Dmain=encclient_main.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "cipher.h"
#include "client.h"
#include "config.h"
#include "job.h"
#include "protocol.h"
#include "qnx_shim.h"
#include "sync.h"
#include "util.h"

int encserver_main(int argc, char **argv);
int encclient_main(int argc, char **argv);

#define KEYFILE "build/host/e2e.key"
#define INFILE  "build/host/e2e_in.bin"
#define ENCFILE "build/host/e2e_in.bin.enc"
#define DECFILE "build/host/e2e_dec.bin"

static int failures;
static uint8_t key[ENC_KEY_LEN];
static pthread_mutex_t failures_mutex = PTHREAD_MUTEX_INITIALIZER;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            enc_lock(&failures_mutex);                      \
            failures++;                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                   \
            fputc('\n', stderr);                            \
            enc_unlock(&failures_mutex);                    \
        }                                                   \
    } while (0)

static void *server_thread(void *arg)
{
    char *argv[] = { "encserver", "-w", "4", "-k", KEYFILE, NULL };

    (void)arg;
    encserver_main(5, argv);
    return NULL;
}

static int run_client(char **argv)
{
    int argc = 0;

    while (argv[argc] != NULL) {
        argc++;
    }
    optind = 0;                     /* glibc: fully reset getopt between runs */
    return encclient_main(argc, argv);
}

static int write_file(const char *path, const uint8_t *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    int ok = f != NULL && fwrite(buf, 1, len, f) == len;

    if (f != NULL) {
        fclose(f);
    }
    return ok ? 0 : -1;
}

/*
 * Send a hand-made ENCRYPT on connection coid. Returns 0 and the Job id, or the
 * errno that MsgSendv failed with. Without `event` the request carries an empty one.
 */
static int raw_encrypt(int coid, uint16_t type, uint32_t claimed_len, const uint8_t *data,
                       size_t sent_len, const struct sigevent *event, uint32_t *job_id)
{
    encsvc_encrypt_t hdr = { .type = type, .data_len = claimed_len };
    encsvc_accepted_t accepted = { 0 };
    iov_t siov[2], riov[1];

    if (event != NULL) {
        hdr.event = *event;
    }
    SETIOV(&siov[0], &hdr, sizeof hdr);
    SETIOV(&siov[1], data, sent_len);
    SETIOV(&riov[0], &accepted, sizeof accepted);
    if (MsgSendv(coid, siov, 2, riov, 1) == -1) {
        return errno;
    }
    if (job_id != NULL) {
        *job_id = accepted.job_id;
    }
    return 0;
}

/* Send FETCH for job_id. Returns 0 or the errno that MsgSendv failed with. */
static int raw_fetch(int coid, uint32_t job_id, uint8_t *out, size_t len)
{
    encsvc_fetch_t fetch = { .type = ENCSVC_MSG_FETCH, .job_id = job_id };
    encsvc_result_t result;
    iov_t siov[1], riov[2];

    SETIOV(&siov[0], &fetch, sizeof fetch);
    SETIOV(&riov[0], &result, sizeof result);
    SETIOV(&riov[1], out, len);
    return MsgSendv(coid, siov, 1, riov, 2) == -1 ? errno : 0;
}

/* Wait up to 5 s for the server to hold exactly `jobs` Jobs. */
static int wait_open_jobs(unsigned jobs)
{
    for (int i = 0; i < 500 && shim_open_jobs() != jobs; i++) {
        usleep(10000);
    }
    return shim_open_jobs() == jobs;
}

static void test_client_runs(void)
{
    char *big[] = { "encclient", "-k", KEYFILE, "-s", "2097152", "-n", "3", "-V", NULL };
    char *tiny[] = { "encclient", "-k", KEYFILE, "-s", "1", "-n", "2", "-V", NULL };
    char *edge[] = { "encclient", "-k", KEYFILE, "-s", "65536", "-n", "2", "-c", "build/host/e2e.csv", "-V", NULL };
    char *over[] = { "encclient", "-k", KEYFILE, "-s", "2097153", NULL };

    CHECK(run_client(big) == 0, "2 MB client run");
    CHECK(run_client(tiny) == 0, "1-byte client run");
    CHECK(run_client(edge) == 0, "64 KB client run (CSV mode)");
    CHECK(run_client(over) != 0, "client must refuse more than 2 MB");
}

/* Write len random bytes to path. Returns 0 or -1. */
static int write_random_file(const char *path, size_t len)
{
    uint8_t *buf = malloc(len > 0 ? len : 1);
    int rc = buf != NULL && random_bytes(buf, len) == 0 ? write_file(path, buf, len) : -1;

    free(buf);
    return rc;
}

/* 1 if both files exist and hold the same bytes. */
static int same_file(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    int same = fa != NULL && fb != NULL;

    while (same) {
        int ca = fgetc(fa), cb = fgetc(fb);
        if (ca != cb) {
            same = 0;
        } else if (ca == EOF) {
            break;
        }
    }
    if (fa != NULL) {
        fclose(fa);
    }
    if (fb != NULL) {
        fclose(fb);
    }
    return same;
}

/* Encrypt INFILE to ENCFILE with -e, decrypt that to DECFILE with -d. 1 if DECFILE == INFILE. */
static int round_trip(const char *jobs)
{
    char *enc[] = { "encclient", "-k", KEYFILE, "-e", INFILE, "-o", ENCFILE, "-j", (char *)jobs, NULL };
    char *dec[] = { "encclient", "-k", KEYFILE, "-d", ENCFILE, "-o", DECFILE, NULL };

    unlink(ENCFILE);
    unlink(DECFILE);
    return run_client(enc) == 0 && run_client(dec) == 0 && same_file(INFILE, DECFILE);
}

static void test_file_round_trip(void)
{
    CHECK(write_random_file(INFILE, 100000) == 0, "write input file");
    CHECK(round_trip("4"), "100 KB file: encrypt then decrypt gives the same bytes");

    /* Several Data Blocks, the last one short, with 1 and with 8 in flight. */
    CHECK(write_random_file(INFILE, 2 * ENC_MAX_BLOCK + 12345) == 0, "write input file");
    CHECK(round_trip("1"), "3-block file, 1 in flight");
    CHECK(round_trip("8"), "3-block file, 8 in flight");

    CHECK(write_random_file(INFILE, ENC_MAX_BLOCK) == 0, "write input file");
    CHECK(round_trip("4"), "file of exactly one full Data Block");

    CHECK(write_random_file(INFILE, 1) == 0, "write input file");
    CHECK(round_trip("4"), "1-byte file");

    CHECK(write_random_file(INFILE, 0) == 0, "write input file");
    CHECK(round_trip("4"), "empty file");
}

/* Read the first `cap - 1` bytes of path as text. */
static void read_text(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n = f != NULL ? fread(buf, 1, cap - 1, f) : 0;

    buf[n] = '\0';
    if (f != NULL) {
        fclose(f);
    }
}

static void test_encrypted_file_header_is_readable(void)
{
    static const char expect[] = "QNX-ENC v1\n"
                                 "name    e2e_in.bin\n"
                                 "size    2097162 bytes\n"
                                 "blocks  2 of up to 2097152 bytes\n"
                                 "iv 1    ";
    char text[4096];

    CHECK(write_random_file(INFILE, ENC_MAX_BLOCK + 10) == 0, "write input file");
    CHECK(round_trip("4"), "2-block file");
    read_text(ENCFILE, text, sizeof text);
    CHECK(strncmp(text, expect, strlen(expect)) == 0,
          "header starts with magic, name, size and blocks lines:\n%.120s", text);
    CHECK(strstr(text, "\niv 2    ") != NULL && strstr(text, "\n---\n") != NULL,
          "header lists iv 2 and ends with ---");
}

static void test_decrypt_refuses_bad_input(void)
{
    char *dec_plain[] = { "encclient", "-k", KEYFILE, "-d", INFILE, "-o", DECFILE, NULL };
    char *dec[] = { "encclient", "-k", KEYFILE, "-d", ENCFILE, "-o", DECFILE, NULL };
    char *dec_short[] = { "encclient", "-k", KEYFILE, "-d", DECFILE ".short", "-o", DECFILE, NULL };
    uint8_t marker[] = "keep me";
    uint8_t *buf = malloc(200000);
    FILE *f;
    size_t n;

    CHECK(write_random_file(INFILE, 100000) == 0, "write input file");
    CHECK(round_trip("4"), "100 KB file");

    /* An existing output file is never overwritten. */
    CHECK(write_file(DECFILE, marker, sizeof marker) == 0, "write marker file");
    CHECK(run_client(dec) != 0, "-d must refuse to overwrite an existing file");
    f = fopen(DECFILE, "rb");
    n = f != NULL ? fread(buf, 1, 200000, f) : 0;
    CHECK(n == sizeof marker && memcmp(buf, marker, n) == 0, "existing file left untouched");
    if (f != NULL) {
        fclose(f);
    }

    /* A file that is not an Encrypted File is rejected and nothing is written. */
    unlink(DECFILE);
    CHECK(run_client(dec_plain) != 0, "-d must reject a file without the header");
    CHECK(access(DECFILE, F_OK) != 0, "no output after rejecting a plain file");

    /* A cut-short Encrypted File is rejected and nothing is written. */
    f = fopen(ENCFILE, "rb");
    n = f != NULL ? fread(buf, 1, 200000, f) : 0;
    if (f != NULL) {
        fclose(f);
    }
    CHECK(n > 1000 && write_file(DECFILE ".short", buf, n - 1000) == 0, "write short copy");
    CHECK(run_client(dec_short) != 0, "-d must reject a cut-short Encrypted File");
    CHECK(access(DECFILE, F_OK) != 0, "no output after rejecting a short file");
    free(buf);
}

/* Without -o, -e writes ./<name>.enc and -d writes ./<original name>. */
static void test_default_output_names(void)
{
    char cwd[4096], in_path[4200];
    char *enc[] = { "encclient", "-k", NULL, "-e", in_path, NULL };
    char *dec[] = { "encclient", "-k", NULL, "-d", "e2e_in.bin.enc", NULL };
    char key_path[4200];

    CHECK(getcwd(cwd, sizeof cwd) != NULL, "getcwd");
    snprintf(in_path, sizeof in_path, "%s/%s", cwd, INFILE);
    snprintf(key_path, sizeof key_path, "%s/%s", cwd, KEYFILE);
    enc[2] = dec[2] = key_path;
    CHECK(write_random_file(INFILE, 5000) == 0, "write input file");
    mkdir("build/host/e2e_cwd", 0755);
    CHECK(chdir("build/host/e2e_cwd") == 0, "chdir");
    unlink("e2e_in.bin.enc");
    unlink("e2e_in.bin");

    CHECK(run_client(enc) == 0 && access("e2e_in.bin.enc", F_OK) == 0,
          "-e without -o writes ./e2e_in.bin.enc");
    CHECK(run_client(dec) == 0 && same_file("e2e_in.bin", in_path),
          "-d without -o restores ./e2e_in.bin");
    CHECK(chdir(cwd) == 0, "chdir back");
}

static void test_server_rejects_bad_requests(void)
{
    uint8_t data[128] = { 0 }, out[128];
    int coid = name_open(ENCSVC_NAME, 0);
    encsvc_notify_t notify;
    encsvc_result_t result;

    CHECK(raw_encrypt(coid, ENCSVC_MSG_ENCRYPT, 0, data, 0, NULL, NULL) == EINVAL,
          "empty Data Block -> EINVAL");
    CHECK(raw_encrypt(coid, ENCSVC_MSG_ENCRYPT, ENC_MAX_BLOCK + 1, data, sizeof data, NULL, NULL) ==
          EMSGSIZE, "over 2 MB -> EMSGSIZE");
    CHECK(raw_encrypt(coid, ENCSVC_MSG_ENCRYPT, 100, data, 50, NULL, NULL) == EBADMSG,
          "claims 100 bytes, sends 50 -> EBADMSG");
    CHECK(raw_encrypt(coid, ENCSVC_MSG_FETCH + 1, 16, data, 16, NULL, NULL) == ENOSYS,
          "unknown message type -> ENOSYS");
    CHECK(raw_fetch(coid, 999999, out, sizeof out) == ENOENT, "FETCH of an unknown Job -> ENOENT");

    /* The server must still work after all of those. */
    CHECK(encsvc_notify_open(&notify, coid) == 0, "notify open");
    CHECK(encsvc_encrypt(coid, &notify, data, out, 16, &result) == 0,
          "server still serves after errors");
    CHECK(encsvc_notify_close(&notify) == 0, "notify close");
    name_close(coid);
}

/* A Job can be fetched only on the connection that sent it, and only once. */
static void test_fetch_needs_same_connection(void)
{
    uint8_t data[64] = { 1 }, out[64];
    int a = name_open(ENCSVC_NAME, 0), b = name_open(ENCSVC_NAME, 0);
    encsvc_notify_t notify;
    uint32_t id = 0;
    int err;

    CHECK(encsvc_notify_open(&notify, a) == 0, "notify open");
    CHECK(raw_encrypt(a, ENCSVC_MSG_ENCRYPT, sizeof data, data, sizeof data, &notify.event, &id) == 0,
          "ENCRYPT accepted");
    CHECK(raw_fetch(b, id, out, sizeof out) == ENOENT, "another connection cannot FETCH the Job");
    do {
        err = raw_fetch(a, id, out, sizeof out);     /* EAGAIN until the Job is done */
    } while (err == EAGAIN && usleep(1000) == 0);
    CHECK(err == 0, "the sending connection can FETCH it: %s", strerror(err));
    CHECK(raw_fetch(a, id, out, sizeof out) == ENOENT, "a second FETCH finds nothing");
    CHECK(encsvc_notify_close(&notify) == 0, "notify close");
    name_close(a);
    name_close(b);
}

/*
 * True if the server accepts ENC_MAX_JOBS Data Blocks on a new connection
 * without making any of them wait 2 s for a slot (then disconnects again).
 */
static int slots_all_free(void)
{
    uint8_t data[512] = { 4 };
    int coid = name_open(ENCSVC_NAME, 0), ok = 1;
    encsvc_notify_t notify;
    struct sigevent unblock;
    uint64_t ns = 2000 * 1000000ull;

    if (encsvc_notify_open(&notify, coid) != 0) {
        return 0;
    }
    SIGEV_UNBLOCK_INIT(&unblock);
    for (unsigned i = 0; i < ENC_MAX_JOBS && ok; i++) {
        ok = TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, &unblock, &ns,
                          NULL) == 0 &&
             raw_encrypt(coid, ENCSVC_MSG_ENCRYPT, sizeof data, data, sizeof data, &notify.event,
                         NULL) == 0;
    }
    name_close(coid);
    ok = wait_open_jobs(0) && ok;
    return encsvc_notify_close(&notify) == 0 && ok;
}

/* A client that goes away without FETCH leaves nothing behind (disconnect pulse). */
static void test_disconnect_frees_jobs(void)
{
    uint8_t data[4096] = { 2 };
    int coid = name_open(ENCSVC_NAME, 0);
    encsvc_notify_t notify;

    CHECK(wait_open_jobs(0), "no Jobs held before the test");
    CHECK(encsvc_notify_open(&notify, coid) == 0, "notify open");
    for (int i = 0; i < 3; i++) {
        CHECK(raw_encrypt(coid, ENCSVC_MSG_ENCRYPT, sizeof data, data, sizeof data, &notify.event,
                          NULL) == 0, "ENCRYPT %d accepted", i);
    }
    CHECK(shim_open_jobs() == 3, "server holds the 3 unfetched Jobs");
    name_close(coid);
    CHECK(wait_open_jobs(0), "disconnect frees every Job of that client (holds %u)", shim_open_jobs());
    CHECK(encsvc_notify_close(&notify) == 0, "notify close");
    CHECK(slots_all_free(), "after the disconnect, all %u Job slots are free again", ENC_MAX_JOBS);
}

/*
 * With every Job slot taken, a further ENCRYPT waits. When the client's
 * TimerTimeout expires, the server answers it with ETIMEDOUT.
 */
static void test_waiting_request_times_out(void)
{
    uint8_t data[1024] = { 3 };
    int coid = name_open(ENCSVC_NAME, 0);
    encsvc_notify_t notify;
    struct sigevent unblock;
    uint64_t ns = 200 * 1000000ull;
    unsigned before = shim_unblock_pulses();

    CHECK(wait_open_jobs(0), "no Jobs held before the test");
    CHECK(encsvc_notify_open(&notify, coid) == 0, "notify open");
    for (unsigned i = 0; i < ENC_MAX_JOBS; i++) {
        CHECK(raw_encrypt(coid, ENCSVC_MSG_ENCRYPT, sizeof data, data, sizeof data, &notify.event,
                          NULL) == 0, "ENCRYPT %u accepted", i);
    }
    SIGEV_UNBLOCK_INIT(&unblock);
    CHECK(TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, &unblock, &ns, NULL) == 0,
          "TimerTimeout");
    CHECK(raw_encrypt(coid, ENCSVC_MSG_ENCRYPT, sizeof data, data, sizeof data, &notify.event, NULL) ==
          ETIMEDOUT, "a request beyond ENC_MAX_JOBS times out while waiting for a slot");
    CHECK(shim_unblock_pulses() == before + 1, "the timeout reached the server as an unblock pulse");
    name_close(coid);
    CHECK(wait_open_jobs(0), "disconnect frees the held Jobs");
    CHECK(encsvc_notify_close(&notify) == 0, "notify close");
    CHECK(slots_all_free(), "after the disconnect, all %u Job slots are free again", ENC_MAX_JOBS);
}

static void *concurrent_client(void *arg)
{
    unsigned id = (unsigned)(uintptr_t)arg;
    int coid = name_open(ENCSVC_NAME, 0);
    encsvc_notify_t notify;

    CHECK(encsvc_notify_open(&notify, coid) == 0, "notify open %u", id);
    for (unsigned round = 0; round < 3; round++) {
        size_t len = 1 + (size_t)((id * 7919u + round * 104729u) * 2654435761u % ENC_MAX_BLOCK);
        uint8_t *in = malloc(len), *out = malloc(len), *ref = malloc(len);
        encsvc_result_t result;

        CHECK(in != NULL && out != NULL && ref != NULL, "malloc");
        if (in != NULL && out != NULL && ref != NULL) {
            CHECK(random_bytes(in, len) == 0, "random_bytes");
            CHECK(encsvc_encrypt(coid, &notify, in, out, len, &result) == 0,
                  "concurrent request %u/%u", id, round);
            CHECK(result.data_len == len, "reply length");
            CHECK(reference_encrypt(key, result.iv, in, ref, len) == 0, "reference_encrypt");
            CHECK(memcmp(out, ref, len) == 0,
                  "concurrent result %u/%u differs from Reference Encryption", id, round);
        }
        free(in);
        free(out);
        free(ref);
    }
    CHECK(encsvc_notify_close(&notify) == 0, "notify close %u", id);
    name_close(coid);
    return NULL;
}

static void test_concurrent_clients(void)
{
    enum { CLIENTS = 16 };
    pthread_t threads[CLIENTS];
    unsigned started = 0;

    for (uintptr_t i = 0; i < CLIENTS; i++) {
        if (pthread_create(&threads[started], NULL, concurrent_client, (void *)i) == 0) {
            started++;
        }
    }
    CHECK(started == CLIENTS, "started %u of %u client threads", started, (unsigned)CLIENTS);
    for (unsigned i = 0; i < started; i++) {
        enc_join(threads[i]);
    }
    printf("test_e2e: most Jobs held by the server at once: %u (limit %u)\n",
           shim_max_open_jobs(), ENC_MAX_JOBS);
    CHECK(shim_max_open_jobs() <= ENC_MAX_JOBS, "server held more than ENC_MAX_JOBS");
    CHECK(shim_max_open_jobs() >= 2, "server never ran Jobs concurrently");
}

int main(void)
{
    pthread_t server;

    if (random_bytes(key, sizeof key) != 0 || write_file(KEYFILE, key, sizeof key) != 0) {
        fprintf(stderr, "cannot create %s\n", KEYFILE);
        return 1;
    }
    if (pthread_create(&server, NULL, server_thread, NULL) != 0) {
        fprintf(stderr, "cannot start the server thread\n");
        return 1;
    }
    name_close(name_open(ENCSVC_NAME, 0));     /* waits until the server has attached */

    test_client_runs();
    test_file_round_trip();
    test_encrypted_file_header_is_readable();
    test_decrypt_refuses_bad_input();
    test_default_output_names();
    test_server_rejects_bad_requests();
    test_fetch_needs_same_connection();
    test_disconnect_frees_jobs();
    test_waiting_request_times_out();
    test_concurrent_clients();

    printf("test_e2e [%s]: %s\n", cipher_backend_name(), failures ? "FAILED" : "passed");
    fflush(stdout);
    _exit(failures ? 1 : 0);        /* the server thread never returns */
}
