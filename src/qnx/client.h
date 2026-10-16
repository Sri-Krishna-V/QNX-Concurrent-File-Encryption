/*
 * client.h - parts of encclient shared between client.c (options, benchmark
 * mode), client_file.c (encrypting and decrypting files) and client_msg.c
 * (the client side of the protocol).
 */
#ifndef ENC_CLIENT_H
#define ENC_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "protocol.h"

/*
 * Where the server's "Job done" event arrives: a private channel owned by one
 * sender thread, and the pulse event for it, registered for delivery by the
 * server. Each sender thread has its own, so each thread receives only the
 * pulses for its own Data Blocks.
 */
typedef struct {
    int chid;                       /* this thread's private channel */
    int self_coid;                  /* connection to that channel; the pulse travels on it */
    struct sigevent event;          /* the registered pulse, sent with every ENCRYPT */
    bool registered;                /* event is registered and must be unregistered */
} encsvc_notify_t;

/*
 * Create the channel and register the pulse with the server connection
 * server_coid (from name_open). Returns 0 or an errno value.
 */
int encsvc_notify_open(encsvc_notify_t *notify, int server_coid);

/* Unregister the pulse and remove the channel. Returns 0 or the first errno value. */
int encsvc_notify_close(encsvc_notify_t *notify);

/*
 * Encrypt one Data Block through the server: ENCRYPT, wait for the event,
 * FETCH. Blocks the calling thread only, and for at most ENC_CLIENT_TIMEOUT_NS
 * in total. Returns 0, ETIMEDOUT, or another errno value.
 */
int encsvc_encrypt(int coid, const encsvc_notify_t *notify, const uint8_t *data,
                   uint8_t *encrypted, size_t len, encsvc_result_t *result);

typedef struct {
    const uint8_t *key;
    const char *out_path;           /* NULL: choose a name in the current folder */
    unsigned in_flight;             /* Data Blocks sent at once (-j) */
    bool verify;                    /* check each Encrypted Block (-V) */
    bool preview;                   /* show the first bytes before and after (-x) */
} file_options_t;

/* -e: encrypt a file into an Encrypted File. Returns the exit status. */
int encrypt_file(int coid, const char *in_path, const file_options_t *opt);

/* -d: turn an Encrypted File back into the original file, on this client. Returns the exit status. */
int decrypt_file(const char *in_path, const file_options_t *opt);

/* Text for an errno value from encsvc_encrypt, with a hint for the common cases. */
const char *encsvc_strerror(int err);

#endif /* ENC_CLIENT_H */
