/*
 * client_file.c - encclient -e (encrypt a file) and -d (decrypt it again).
 *
 * Encrypting: the file is split into Data Blocks of up to ENC_MAX_BLOCK bytes.
 * A few sender threads (-j, default 4) each take the next Data Block, send it
 * to the server (client_msg.c) and write its Encrypted Block into place, so
 * several Jobs are in progress on the server at once. The main thread waits on a condition
 * variable and redraws the progress bar each time a Data Block comes back.
 * When all are done it writes the readable header (see encfile.h) in front.
 *
 * Decrypting needs no server: the client reads the header, then decrypts each
 * Data Block with its IV and the shared key (CTR decryption = encryption).
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "cipher.h"
#include "client.h"
#include "display.h"
#include "encfile.h"
#include "job.h"
#include "sync.h"
#include "util.h"

#define PREVIEW_LEN 16

static int pread_full(int fd, uint8_t *buf, size_t len, off_t at)
{
    while (len > 0) {
        ssize_t n = pread(fd, buf, len, at);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return n == 0 ? EIO : errno;
        }
        buf += n;
        len -= (size_t)n;
        at += n;
    }
    return 0;
}

static int pwrite_full(int fd, const uint8_t *buf, size_t len, off_t at)
{
    while (len > 0) {
        ssize_t n = pwrite(fd, buf, len, at);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return n == 0 ? EIO : errno;
        }
        buf += n;
        len -= (size_t)n;
        at += n;
    }
    return 0;
}

/*
 * Close a file opened only for reading. Nothing was written to it, so a failed
 * close loses no data and there is nothing to report.
 */
static void close_read_only(int fd)
{
    (void)close(fd);
}

static void fclose_read_only(FILE *f)
{
    (void)fclose(f);
}

/* Remove a half-written output file; say so if that fails, so the user can delete it. */
static void remove_partial(const char *path)
{
    if (unlink(path) != 0 && errno != ENOENT) {
        ui_error("encclient", "could not remove the incomplete %s: %s", path, strerror(errno));
    }
}

/* "examples/photo.png" -> "photo.png" */
static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash != NULL ? slash + 1 : path;
}

/* ---------------------------------------------------------------- encrypt */

typedef struct {
    /* Read-only while the sender threads run. */
    int coid;
    const file_options_t *opt;
    int in_fd, out_fd;
    encfile_header_t *header;       /* each thread fills in only its own Data Blocks' IVs */
    size_t header_len;

    /* Guarded by mutex. */
    pthread_mutex_t mutex;
    pthread_cond_t progress_cv;     /* signalled when a Data Block is done or a thread stops */
    uint32_t next_block;
    uint32_t done_blocks;
    unsigned running;               /* sender threads still running */
    int err;                        /* first failure, 0 if none */
    uint32_t err_block;
    uint64_t server_ns;             /* Encryption Time, summed over all Data Blocks */
    uint64_t segments;
    unsigned workers;
    uint8_t preview_in[PREVIEW_LEN], preview_out[PREVIEW_LEN];
} encrypt_run_t;

/* Encrypt Data Block i. buf_in, buf_out and scratch each hold one Data Block. */
static int encrypt_block(encrypt_run_t *run, const encsvc_notify_t *notify, uint32_t i,
                         uint8_t *buf_in, uint8_t *buf_out, uint8_t *scratch, encsvc_result_t *reply)
{
    size_t len = encfile_block_len(run->header, i);
    off_t at = (off_t)i * run->header->block_size;
    int err;

    err = pread_full(run->in_fd, buf_in, len, at);
    if (err == 0) {
        err = encsvc_encrypt(run->coid, notify, buf_in, buf_out, len, reply);
    }
    if (err == 0 && run->opt->verify &&
        (reference_encrypt(run->opt->key, reply->iv, buf_in, scratch, len) != 0 ||
         memcmp(scratch, buf_out, len) != 0)) {
        err = EILSEQ;               /* our marker for: differs from the Reference Encryption */
    }
    if (err == 0) {
        err = pwrite_full(run->out_fd, buf_out, len, (off_t)run->header_len + at);
    }
    if (err == 0) {
        memcpy(run->header->ivs[i], reply->iv, ENC_IV_LEN);
    }
    return err;
}

static void *sender_thread(void *arg)
{
    encrypt_run_t *run = arg;
    size_t cap = run->header->block_size;
    uint8_t *buf_in = malloc(cap), *buf_out = malloc(cap);
    uint8_t *scratch = run->opt->verify ? malloc(cap) : NULL;
    int close_err, err = (buf_in == NULL || buf_out == NULL || (run->opt->verify && scratch == NULL))
                             ? ENOMEM : 0;
    encsvc_notify_t notify;
    bool notify_open = false;
    uint32_t i = 0;

    if (err == 0) {
        err = encsvc_notify_open(&notify, run->coid);
        notify_open = err == 0;
    }
    while (err == 0) {
        encsvc_result_t reply;

        enc_lock(&run->mutex);
        if (run->err != 0 || run->next_block >= run->header->blocks) {
            enc_unlock(&run->mutex);
            break;
        }
        i = run->next_block++;
        enc_unlock(&run->mutex);

        err = encrypt_block(run, &notify, i, buf_in, buf_out, scratch, &reply);
        if (err != 0) {
            break;
        }

        enc_lock(&run->mutex);
        run->done_blocks++;
        run->server_ns += reply.encryption_ns;
        run->segments += reply.segments;
        run->workers = reply.workers;
        if (i == 0) {
            memcpy(run->preview_in, buf_in, PREVIEW_LEN);
            memcpy(run->preview_out, buf_out, PREVIEW_LEN);
        }
        enc_cond_signal(&run->progress_cv);
        enc_unlock(&run->mutex);
    }
    if (notify_open) {
        close_err = encsvc_notify_close(&notify);
        if (err == 0) {
            err = close_err;
        }
    }

    enc_lock(&run->mutex);
    if (err != 0 && run->err == 0) {
        run->err = err;
        run->err_block = i;
    }
    run->running--;
    enc_cond_signal(&run->progress_cv);
    enc_unlock(&run->mutex);

    free(buf_in);
    free(buf_out);
    free(scratch);
    return NULL;
}

/* Start the sender threads and wait for them, drawing progress. Returns 0 or an errno value. */
static int run_senders(encrypt_run_t *run)
{
    unsigned threads = run->opt->in_flight;
    pthread_t *tids;
    unsigned started = 0;
    int err;

    if (threads > run->header->blocks) {
        threads = run->header->blocks;
    }
    tids = calloc(threads > 0 ? threads : 1, sizeof *tids);
    if (tids == NULL) {
        return ENOMEM;
    }

    enc_lock(&run->mutex);
    for (; started < threads; started++) {
        if (pthread_create(&tids[started], NULL, sender_thread, run) != 0) {
            break;
        }
        run->running++;
    }
    if (started == 0 && threads > 0) {
        run->err = EAGAIN;
    }
    while (run->running > 0) {
        ui_progress("progress", run->done_blocks, run->header->blocks, "Data Blocks");
        enc_cond_wait(&run->progress_cv, &run->mutex);
    }
    err = run->err;
    enc_unlock(&run->mutex);

    for (unsigned t = 0; t < started; t++) {
        enc_join(tids[t]);
    }
    free(tids);
    if (err == 0) {
        ui_progress("progress", run->done_blocks, run->header->blocks, "Data Blocks");
    } else if (ui_live()) {
        putchar('\n');              /* leave the half-drawn progress bar */
    }
    return err;
}

static bool same_inode(const char *a, const char *b)
{
    struct stat sa, sb;

    return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

int encrypt_file(int coid, const char *in_path, const file_options_t *opt)
{
    encrypt_run_t run = { .coid = coid, .opt = opt, .in_fd = -1, .out_fd = -1 };
    encfile_header_t header;
    char default_out[ENCFILE_NAME_MAX + 8], title[1024], a[32], b[32], c[32], d[32];
    const char *out_path = opt->out_path;
    struct stat st;
    char *header_text = NULL;
    uint64_t start, wall;
    bool created = false;           /* only a file we created may be removed on failure */
    int err;

    run.in_fd = open(in_path, O_RDONLY);
    if (run.in_fd < 0 || fstat(run.in_fd, &st) != 0) {
        ui_error("encclient", "cannot open %s: %s", in_path, strerror(errno));
        if (run.in_fd >= 0) {
            close_read_only(run.in_fd);
        }
        return 1;
    }
    if (!S_ISREG(st.st_mode)) {
        ui_error("encclient", "%s is not a regular file", in_path);
        close_read_only(run.in_fd);
        return 1;
    }
    if (out_path == NULL) {
        snprintf(default_out, sizeof default_out, "%s.enc", base_name(in_path));
        out_path = default_out;
    }
    if (same_inode(in_path, out_path)) {
        ui_error("encclient", "%s would overwrite the file being encrypted", out_path);
        close_read_only(run.in_fd);
        return 1;
    }

    err = encfile_header_init(&header, in_path, (uint64_t)st.st_size, ENC_MAX_BLOCK);
    if (err != 0) {
        ui_error("encclient", "%s: %s", in_path, strerror(err));
        close_read_only(run.in_fd);
        return 1;
    }
    run.header = &header;
    run.header_len = encfile_header_format(&header, NULL, 0);

    snprintf(title, sizeof title, "%s %s %s", in_path, ui_arrow(), out_path);
    ui_title("encclient", "encrypt", title);
    ui_row("input", "%s  %s (%s byte%s)", header.name, ui_size(a, sizeof a, header.size),
           ui_count(b, sizeof b, header.size), ui_s(header.size));
    ui_row("Data Blocks", "%u of up to %s, %u in flight at once", header.blocks,
           ui_size(c, sizeof c, header.block_size),
           opt->in_flight < header.blocks ? opt->in_flight : header.blocks);

    run.out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (run.out_fd < 0) {
        err = errno;
        ui_error("encclient", "cannot create %s: %s", out_path, strerror(err));
        goto out;
    }
    created = true;
    err = enc_mutex_cond_init(&run.mutex, &run.progress_cv);
    if (err != 0) {
        ui_error("encclient", "cannot set up the sender threads: %s", strerror(err));
        goto out;
    }

    start = now_ns();
    err = run_senders(&run);
    wall = now_ns() - start;

    enc_cond_destroy(&run.progress_cv);
    enc_mutex_destroy(&run.mutex);

    if (err != 0) {
        ui_error("encclient", "Data Block %u failed: %s", run.err_block + 1,
                 err == EILSEQ ? "differs from the Reference Encryption"
                                           : encsvc_strerror(err));
        goto out;
    }

    /* All IVs are known now: write the header in front of the encrypted bytes. */
    header_text = malloc(run.header_len + 1);
    if (header_text == NULL) {
        err = ENOMEM;
    } else if (encfile_header_format(&header, header_text, run.header_len + 1) != run.header_len) {
        err = EIO;                  /* cannot happen: the same header, formatted twice */
    } else {
        err = pwrite_full(run.out_fd, (const uint8_t *)header_text, run.header_len, 0);
    }
    if (close(run.out_fd) != 0 && err == 0) {
        err = errno;
    }
    run.out_fd = -1;
    if (err != 0) {
        ui_error("encclient", "cannot write %s: %s", out_path, strerror(err));
        goto out;
    }

    if (header.blocks > 0) {
        ui_row("server", "%u workers, %llu Segment%s in %u Job%s, %s encrypting", run.workers,
               (unsigned long long)run.segments, ui_s(run.segments), header.blocks,
               ui_s(header.blocks), ui_ms(a, sizeof a, run.server_ns));
    } else {
        ui_row("server", "not needed: the file is empty");
    }
    ui_row("wall time", "%s  (%s)", ui_ms(b, sizeof b, wall), ui_rate(c, sizeof c, header.size, wall));
    ui_row("saved as", "%s  %s (%s B readable header + %s B encrypted)", out_path,
           ui_size(c, sizeof c, run.header_len + header.size),
           ui_count(d, sizeof d, run.header_len), ui_count(a, sizeof a, header.size));
    if (opt->preview && header.blocks > 0) {
        size_t n = header.size < PREVIEW_LEN ? (size_t)header.size : PREVIEW_LEN;

        ui_row("first bytes", "%s", "");
        ui_hex_row("original", run.preview_in, n);
        ui_hex_row("encrypted", run.preview_out, n);
    }
    ui_row("status", "%s%s encrypted%s%s", ui(UI_GREEN), ui_ok(),
           opt->verify ? ", every Data Block verified" : "", ui(UI_RESET));

out:
    if (run.out_fd >= 0) {
        (void)close(run.out_fd);    /* only on a failure path: the file is removed next */
    }
    if (err != 0 && created) {
        remove_partial(out_path);   /* never leave half an Encrypted File behind */
    }
    close_read_only(run.in_fd);
    free(header_text);
    encfile_header_free(&header);
    return err == 0 ? 0 : 1;
}

/* ---------------------------------------------------------------- decrypt */

int decrypt_file(const char *in_path, const file_options_t *opt)
{
    encfile_header_t header;
    char title[1024], a[32], b[32];
    const char *out_path = opt->out_path;
    uint8_t *buf_in = NULL, *buf_out = NULL;
    uint8_t preview_in[PREVIEW_LEN], preview_out[PREVIEW_LEN];
    size_t header_len;
    struct stat st;
    uint64_t start, wall;
    int out_fd = -1, err;
    FILE *in = fopen(in_path, "rb");

    if (in == NULL || fstat(fileno(in), &st) != 0) {
        ui_error("encclient", "cannot open %s: %s", in_path, strerror(errno));
        if (in != NULL) {
            fclose_read_only(in);
        }
        return 1;
    }
    err = encfile_header_read(in, &header, &header_len);
    if (err != 0) {
        ui_error("encclient", "%s is not an Encrypted File (it must start with \"%s\")", in_path,
                 ENCFILE_MAGIC);
        fclose_read_only(in);
        return 1;
    }
    if ((uint64_t)st.st_size != header_len + header.size) {
        ui_error("encclient", "%s is damaged: the header promises %s encrypted bytes, the file has %s",
                 in_path, ui_count(a, sizeof a, header.size),
                 ui_count(b, sizeof b, (uint64_t)st.st_size > header_len ? (uint64_t)st.st_size - header_len : 0));
        encfile_header_free(&header);
        fclose_read_only(in);
        return 1;
    }
    if (out_path == NULL) {
        out_path = header.name;     /* checked by encfile_header_read: no folders in it */
    }

    snprintf(title, sizeof title, "%s %s %s", in_path, ui_arrow(), out_path);
    ui_title("encclient", "decrypt", title);
    ui_row("header", "%s, original name %s, %s, %u Data Block%s", ENCFILE_MAGIC, header.name,
           ui_size(a, sizeof a, header.size), header.blocks, ui_s(header.blocks));

    out_fd = open(out_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (out_fd < 0) {
        err = errno;
        if (err == EEXIST) {
            ui_error("encclient", "%s already exists; delete it or choose another name with -o",
                     out_path);
        } else {
            ui_error("encclient", "cannot create %s: %s", out_path, strerror(err));
        }
        encfile_header_free(&header);
        fclose_read_only(in);
        return 1;
    }

    buf_in = malloc(header.block_size);
    buf_out = malloc(header.block_size);
    err = buf_in == NULL || buf_out == NULL ? ENOMEM : 0;
    start = now_ns();
    for (uint32_t i = 0; i < header.blocks && err == 0; i++) {
        size_t len = encfile_block_len(&header, i);

        if (fread(buf_in, 1, len, in) != len) {
            err = EIO;
        } else if (cipher_ctr(opt->key, header.ivs[i], 0, buf_in, buf_out, len) != 0) {
            err = EIO;
        } else {
            err = pwrite_full(out_fd, buf_out, len, (off_t)i * header.block_size);
        }
        if (err == 0 && i == 0) {
            memcpy(preview_in, buf_in, len < PREVIEW_LEN ? len : PREVIEW_LEN);
            memcpy(preview_out, buf_out, len < PREVIEW_LEN ? len : PREVIEW_LEN);
        }
        if (err == 0) {
            ui_progress("progress", i + 1, header.blocks, "Data Blocks");
        }
    }
    wall = now_ns() - start;
    if (close(out_fd) != 0 && err == 0) {
        err = errno;
    }

    if (err != 0) {
        if (ui_live()) {
            putchar('\n');
        }
        ui_error("encclient", "cannot decrypt %s: %s", in_path, strerror(err));
        remove_partial(out_path);
    } else {
        if (header.blocks == 0) {
            ui_progress("progress", 0, 0, "Data Blocks");
        }
        ui_row("wall time", "%s on this client, 1 thread", ui_ms(a, sizeof a, wall));
        ui_row("saved as", "%s  %s (%s byte%s)", out_path, ui_size(a, sizeof a, header.size),
               ui_count(b, sizeof b, header.size), ui_s(header.size));
        if (opt->preview && header.blocks > 0) {
            size_t n = header.size < PREVIEW_LEN ? (size_t)header.size : PREVIEW_LEN;

            ui_row("first bytes", "%s", "");
            ui_hex_row("encrypted", preview_in, n);
            ui_hex_row("decrypted", preview_out, n);
        }
        ui_row("status", "%s%s decrypted%s", ui(UI_GREEN), ui_ok(), ui(UI_RESET));
    }

    free(buf_in);
    free(buf_out);
    encfile_header_free(&header);
    fclose_read_only(in);
    return err == 0 ? 0 : 1;
}
