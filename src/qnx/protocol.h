/*
 * protocol.h - the messages between encclient and encserver.
 *
 * One Data Block takes two message exchanges and one event:
 *
 *   1. ENCRYPT   client -> server   MsgSendv  [ encsvc_encrypt_t | Data Block ]
 *                server -> client   MsgReply  [ encsvc_accepted_t ]              at once
 *   2. event     server -> client   MsgDeliverEvent(the client's pulse)         when the Job is done
 *   3. FETCH     client -> server   MsgSend   [ encsvc_fetch_t ]
 *                server -> client   MsgReplyv [ encsvc_result_t | Encrypted Block ]
 *
 * The event in step 1 is a pulse to the client's own private channel, which the
 * client registers with MsgRegisterEvent() before sending it. The server never
 * builds or changes an event; it only delivers the one the client gave it.
 *
 * Errors are returned with MsgError: MsgSend(v) returns -1 and errno is
 *     EINVAL    ENCRYPT: empty Data Block
 *     EMSGSIZE  ENCRYPT: Data Block larger than ENC_MAX_BLOCK
 *     EBADMSG   ENCRYPT: fewer bytes sent than data_len says
 *     EBUSY     ENCRYPT: too many clients already waiting for a Job slot
 *     ENOMEM    server out of memory
 *     ENOENT    FETCH: no such Job for this client
 *     EAGAIN    FETCH: the Job is not done yet (the client fetched before the event)
 *     EIO       FETCH: the Job failed (cipher failure)
 *     ETIMEDOUT ENCRYPT: the client's 10 s timeout ran out while it waited for a Job slot
 */
#ifndef ENC_PROTOCOL_H
#define ENC_PROTOCOL_H

#include <stdint.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>

#include "config.h"

/* Name registered with name_attach(); appears as /dev/name/local/encsvc. */
#define ENCSVC_NAME "encsvc"

/* User message types must be above _IO_MAX so they never clash with system messages. */
#define ENCSVC_MSG_ENCRYPT (_IO_MAX + 1)
#define ENCSVC_MSG_FETCH   (_IO_MAX + 2)

/* Pulse code of the client's "Job done" event (user pulse codes start at _PULSE_CODE_MINAVAIL). */
#define ENCSVC_PULSE_DONE  (_PULSE_CODE_MINAVAIL + 1)

typedef struct {
    uint16_t type;                  /* ENCSVC_MSG_ENCRYPT */
    uint16_t reserved;              /* 0 */
    uint32_t data_len;              /* Data Block size in bytes */
    struct sigevent event;          /* registered pulse, delivered when the Job is done */
} encsvc_encrypt_t;

typedef struct {
    uint32_t job_id;                /* quote this in FETCH */
    uint32_t reserved;              /* 0 */
} encsvc_accepted_t;

typedef struct {
    uint16_t type;                  /* ENCSVC_MSG_FETCH */
    uint16_t reserved;              /* 0 */
    uint32_t job_id;                /* from encsvc_accepted_t */
} encsvc_fetch_t;

typedef struct {
    uint32_t data_len;              /* Encrypted Block size, equal to the Data Block size */
    uint32_t segments;              /* number of Segments the Job used */
    uint8_t  iv[ENC_IV_LEN];        /* the Job's IV, needed to decrypt */
    uint64_t encryption_ns;         /* Encryption Time */
    uint32_t workers;               /* Worker Pool size */
    uint32_t reserved;              /* 0 */
} encsvc_result_t;

#endif /* ENC_PROTOCOL_H */
