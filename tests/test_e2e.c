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

#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "cipher.h"
#include "config.h"
#include "job.h"
#include "protocol.h"
#include "qnx_shim.h"
#include "util.h"

int encserver_main(int argc, char **argv);
int encclient_main(int argc, char **argv);

#define KEYFILE "build/host/e2e.key"
#define INFILE  "build/host/e2e_in.bin"
#define OUTFILE "build/host/e2e_out.bin"

static int failures;
static uint8_t key[ENC_KEY_LEN];
static pthread_mutex_t failures_mutex = PTHREAD_MUTEX_INITIALIZER;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            pthread_mutex_lock(&failures_mutex);            \
            failures++;                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                   \
            fputc('\n', stderr);                            \
            pthread_mutex_unlock(&failures_mutex);          \
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

/* Send a hand-made request. Returns 0, or the errno that MsgSendv failed with. */
static int raw_request(uint16_t type, uint32_t claimed_len, const uint8_t *data, size_t sent_len,
                       encsvc_reply_t *reply, uint8_t *out, size_t out_len)
{
    encsvc_request_t hdr = { .type = type, .data_len = claimed_len };
    iov_t siov[2], riov[2];

    SETIOV(&siov[0], &hdr, sizeof hdr);
    SETIOV(&siov[1], data, sent_len);
    SETIOV(&riov[0], reply, sizeof *reply);
    SETIOV(&riov[1], out, out_len);
    return MsgSendv(1, siov, 2, riov, 2) == -1 ? errno : 0;
}

static void test_client_runs(void)
{
    char *big[] = { "encclient", "-k", KEYFILE, "-s", "2097152", "-n", "3", "-V", NULL };
    char *tiny[] = { "encclient", "-k", KEYFILE, "-s", "1", "-n", "2", "-V", NULL };
    char *edge[] = { "encclient", "-k", KEYFILE, "-s", "65536", "-n", "2", "-c", "-V", NULL };
    char *over[] = { "encclient", "-k", KEYFILE, "-s", "2097153", NULL };
    char *file[] = { "encclient", "-k", KEYFILE, "-f", INFILE, "-o", OUTFILE, "-W", "1", NULL };
    size_t len = 100000;
    uint8_t *in = malloc(len), *out = malloc(ENC_IV_LEN + len), *dec = malloc(len);
    FILE *f;

    CHECK(run_client(big) == 0, "2 MB client run");
    CHECK(run_client(tiny) == 0, "1-byte client run");
    CHECK(run_client(edge) == 0, "64 KB client run (CSV mode)");
    CHECK(run_client(over) != 0, "client must refuse more than 2 MB");

    /* File in, file out: output is IV + Encrypted Block, and decrypts to the file. */
    random_bytes(in, len);
    CHECK(write_file(INFILE, in, len) == 0, "write input file");
    CHECK(run_client(file) == 0, "file client run");
    f = fopen(OUTFILE, "rb");
    CHECK(f != NULL && fread(out, 1, ENC_IV_LEN + len, f) == ENC_IV_LEN + len,
          "output file has IV + %zu bytes", len);
    if (f != NULL) {
        fclose(f);
    }
    CHECK(cipher_ctr(key, out, 0, out + ENC_IV_LEN, dec, len) == 0 && memcmp(dec, in, len) == 0,
          "output file decrypts to the input file");
    free(in);
    free(out);
    free(dec);
}

static void test_server_rejects_bad_requests(void)
{
    uint8_t data[128] = { 0 }, out[128];
    encsvc_reply_t reply;

    CHECK(raw_request(ENCSVC_MSG_ENCRYPT, 0, data, 0, &reply, out, 0) == EINVAL,
          "empty Data Block -> EINVAL");
    CHECK(raw_request(ENCSVC_MSG_ENCRYPT, ENC_MAX_BLOCK + 1, data, sizeof data, &reply, out,
                      sizeof out) == EMSGSIZE, "over 2 MB -> EMSGSIZE");
    CHECK(raw_request(ENCSVC_MSG_ENCRYPT, 100, data, 50, &reply, out, sizeof out) == EBADMSG,
          "claims 100 bytes, sends 50 -> EBADMSG");
    CHECK(raw_request(ENCSVC_MSG_ENCRYPT + 1, 16, data, 16, &reply, out, sizeof out) == ENOSYS,
          "unknown message type -> ENOSYS");
    /* The server must still work after all of those. */
    CHECK(raw_request(ENCSVC_MSG_ENCRYPT, 16, data, 16, &reply, out, 16) == 0,
          "server still serves after errors");
}

static void *concurrent_client(void *arg)
{
    unsigned id = (unsigned)(uintptr_t)arg;

    for (unsigned round = 0; round < 3; round++) {
        size_t len = 1 + (size_t)((id * 7919u + round * 104729u) * 2654435761u % ENC_MAX_BLOCK);
        uint8_t *in = malloc(len), *out = malloc(len), *ref = malloc(len);
        encsvc_reply_t reply;

        random_bytes(in, len);
        CHECK(raw_request(ENCSVC_MSG_ENCRYPT, (uint32_t)len, in, len, &reply, out, len) == 0,
              "concurrent request %u/%u", id, round);
        CHECK(reply.data_len == len, "reply length");
        reference_encrypt(key, reply.iv, in, ref, len);
        CHECK(memcmp(out, ref, len) == 0, "concurrent result %u/%u differs from Reference Encryption",
              id, round);
        free(in);
        free(out);
        free(ref);
    }
    return NULL;
}

static void test_concurrent_clients(void)
{
    enum { CLIENTS = 16 };
    pthread_t threads[CLIENTS];

    for (uintptr_t i = 0; i < CLIENTS; i++) {
        pthread_create(&threads[i], NULL, concurrent_client, (void *)i);
    }
    for (unsigned i = 0; i < CLIENTS; i++) {
        pthread_join(threads[i], NULL);
    }
    printf("test_e2e: most Jobs held by the server at once: %u (limit %u)\n",
           shim_max_in_server(), ENC_MAX_JOBS);
    CHECK(shim_max_in_server() <= ENC_MAX_JOBS, "server held more than ENC_MAX_JOBS");
    CHECK(shim_max_in_server() >= 2, "server never ran Jobs concurrently");
}

int main(void)
{
    pthread_t server;

    if (random_bytes(key, sizeof key) != 0 || write_file(KEYFILE, key, sizeof key) != 0) {
        fprintf(stderr, "cannot create %s\n", KEYFILE);
        return 1;
    }
    pthread_create(&server, NULL, server_thread, NULL);
    name_open(ENCSVC_NAME, 0);      /* waits until the server has attached */

    test_client_runs();
    test_server_rejects_bad_requests();
    test_concurrent_clients();

    printf("test_e2e [%s]: %s\n", cipher_backend_name(), failures ? "FAILED" : "passed");
    fflush(stdout);
    _exit(failures ? 1 : 0);        /* the server thread never returns */
}
