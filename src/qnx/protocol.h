/*
 * protocol.h - the message format between encclient and encserver.
 *
 * Request (client -> server), sent with MsgSendv as two parts:
 *     [ encsvc_request_t | Data Block (data_len bytes) ]
 *
 * Reply (server -> client), sent with MsgReplyv as two parts:
 *     [ encsvc_reply_t | Encrypted Block (data_len bytes) ]
 *
 * Errors are returned with MsgError: MsgSendv returns -1 and errno is
 *     EINVAL    empty Data Block
 *     EMSGSIZE  Data Block larger than ENC_MAX_BLOCK
 *     EBADMSG   fewer bytes sent than data_len says
 *     ENOMEM    server out of memory
 *     EIO       cipher failure
 */
#ifndef ENC_PROTOCOL_H
#define ENC_PROTOCOL_H

#include <stdint.h>
#include <sys/iomsg.h>

#include "config.h"

/* Name registered with name_attach(); appears as /dev/name/local/encsvc. */
#define ENCSVC_NAME "encsvc"

/* User message types must be above _IO_MAX so they never clash with system messages. */
#define ENCSVC_MSG_ENCRYPT (_IO_MAX + 1)

typedef struct {
    uint16_t type;                  /* ENCSVC_MSG_ENCRYPT */
    uint16_t reserved;              /* 0 */
    uint32_t data_len;              /* Data Block size in bytes */
} encsvc_request_t;

typedef struct {
    uint32_t data_len;              /* Encrypted Block size, equal to the Data Block size */
    uint32_t segments;              /* number of Segments the Job used */
    uint8_t  iv[ENC_IV_LEN];        /* the Job's IV, needed to decrypt */
    uint64_t encryption_ns;         /* Encryption Time */
    uint32_t workers;               /* Worker Pool size */
    uint32_t reserved;              /* 0 */
} encsvc_reply_t;

#endif /* ENC_PROTOCOL_H */
