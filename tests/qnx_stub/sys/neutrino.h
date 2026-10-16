/*
 * STUB for `make qnx-syntax` and the host tests only. Not the real QNX header
 * and never used on the Pi. Prototypes copied from the QNX SDP 8.0 C Library
 * Reference so that the QNX sources can be compile-checked on Linux before
 * reaching the lab. Values of constants are placeholders.
 */
#ifndef QNX_STUB_NEUTRINO_H
#define QNX_STUB_NEUTRINO_H

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#ifndef EOK
#define EOK 0
#endif

typedef long rcvid_t;
typedef int64_t ssize64_t;

typedef struct iovec iov_t;
#include <sys/uio.h>
#define SETIOV(_iov, _addr, _len) ((_iov)->iov_base = (void *)(_addr), (_iov)->iov_len = (_len))

/*
 * QNX's struct sigevent has a connection id for pulses, which Linux's does not.
 * <signal.h> above already declared the Linux one, so give the QNX one its own
 * tag; the sources keep writing `struct sigevent`.
 */
#define sigevent qnx_stub_sigevent
struct qnx_stub_sigevent {
    int sigev_notify;
    int sigev_coid;
    union sigval sigev_value;
    short sigev_code;
    short sigev_priority;
};

#define SIGEV_PULSE                 4
#define SIGEV_UNBLOCK               5
#define SIGEV_PULSE_PRIO_INHERIT    (-1)
#define SIGEV_PULSE_INIT(__e, __f, __p, __c, __v)                                          \
    ((__e)->sigev_notify = SIGEV_PULSE, (__e)->sigev_coid = (__f),                         \
     (__e)->sigev_value.sival_ptr = (void *)(__v), (__e)->sigev_code = (__c),              \
     (__e)->sigev_priority = (__p))
#define SIGEV_UNBLOCK_INIT(__e)     ((__e)->sigev_notify = SIGEV_UNBLOCK)

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
#define _PULSE_CODE_MINAVAIL    0

#define _NTO_SIDE_CHANNEL       0x40000000
#define _NTO_COF_CLOEXEC        0x0001

#define _NTO_TIMEOUT_RECEIVE    (1 << 2)
#define _NTO_TIMEOUT_SEND       (1 << 3)
#define _NTO_TIMEOUT_REPLY      (1 << 4)

rcvid_t MsgReceive(int chid, void *msg, size_t bytes, struct _msg_info *info);
int MsgReceivePulse(int chid, void *pulse, size_t bytes, struct _msg_info *info);
ssize_t MsgRead(rcvid_t rcvid, void *msg, size_t bytes, size_t offset);
int MsgReply(rcvid_t rcvid, long status, const void *msg, size_t bytes);
int MsgReplyv(rcvid_t rcvid, long status, const iov_t *riov, size_t rparts);
int MsgError(rcvid_t rcvid, int error);
long MsgSend(int coid, const void *smsg, size_t sbytes, void *rmsg, size_t rbytes);
long MsgSendv(int coid, const iov_t *siov, size_t sparts, const iov_t *riov, size_t rparts);
int MsgDeliverEvent(rcvid_t rcvid, const struct sigevent *event);
int MsgRegisterEvent(struct sigevent *event, int coid);
int MsgUnregisterEvent(const struct sigevent *event);

int ChannelCreate(unsigned flags);
int ChannelDestroy(int chid);
int ConnectAttach(uint32_t nd, pid_t pid, int chid, unsigned index, int flags);
int ConnectDetach(int coid);

int TimerTimeout(clockid_t id, int flags, const struct sigevent *notify, const uint64_t *ntime,
                 uint64_t *otime);

#endif
