/* qnx_shim.h - test-only hooks into qnx_shim.c. */
#ifndef QNX_SHIM_H
#define QNX_SHIM_H

/* Highest number of Jobs the server held at once (ENCRYPT accepted, not yet fetched). */
unsigned shim_max_open_jobs(void);

/* Jobs the server holds right now. */
unsigned shim_open_jobs(void);

/* _PULSE_CODE_UNBLOCK pulses sent to the server because a client's timeout expired. */
unsigned shim_unblock_pulses(void);

#endif
