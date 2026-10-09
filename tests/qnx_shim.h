/* qnx_shim.h - test-only hooks into qnx_shim.c. */
#ifndef QNX_SHIM_H
#define QNX_SHIM_H

/* Highest number of messages the server held at once (received, not yet replied). */
unsigned shim_max_in_server(void);

#endif
