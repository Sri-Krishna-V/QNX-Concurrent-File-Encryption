/*
 * STUB for `make qnx-syntax` only. Not the real QNX header and never linked.
 * Prototypes copied from the QNX SDP 8.0 C Library Reference so that the QNX
 * sources can be compile-checked on Linux before reaching the lab.
 */
#ifndef QNX_STUB_NEUTRINO_H
#define QNX_STUB_NEUTRINO_H

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef EOK
#define EOK 0
#endif

typedef long rcvid_t;
typedef int64_t ssize64_t;

typedef struct iovec iov_t;
#include <sys/uio.h>
#define SETIOV(_iov, _addr, _len) ((_iov)->iov_base = (void *)(_addr), (_iov)->iov_len = (_len))

struct _msg_info {
    uint32_t nd, srcnd;
    pid_t pid;
    int32_t tid, chid, scoid, coid;
    int16_t priority, flags;
    ssize64_t msglen, srcmsglen, dstmsglen;
};

struct _pulse {
    uint16_t type;
    uint16_t subtype;
    int8_t code;
    uint8_t zero[3];
    union sigval value;
    int32_t scoid;
};

#define _PULSE_CODE_UNBLOCK     (-32)
#define _PULSE_CODE_DISCONNECT  (-33)

rcvid_t MsgReceive(int chid, void *msg, size_t bytes, struct _msg_info *info);
ssize_t MsgRead(rcvid_t rcvid, void *msg, size_t bytes, size_t offset);
int MsgReply(rcvid_t rcvid, long status, const void *msg, size_t bytes);
int MsgReplyv(rcvid_t rcvid, long status, const iov_t *riov, size_t rparts);
int MsgError(rcvid_t rcvid, int error);
long MsgSendv(int coid, const iov_t *siov, size_t sparts, const iov_t *riov, size_t rparts);
int ConnectDetach(int coid);

#endif
