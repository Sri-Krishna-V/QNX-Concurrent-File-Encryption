/* STUB for `make qnx-syntax` only. See neutrino.h in this folder. */
#ifndef QNX_STUB_DISPATCH_H
#define QNX_STUB_DISPATCH_H

typedef struct _dispatch dispatch_t;

typedef struct _name_attach {
    dispatch_t *dpp;
    int chid;
    int mntid;
    int zero[2];
} name_attach_t;

name_attach_t *name_attach(dispatch_t *dpp, const char *path, unsigned flags);
int name_detach(name_attach_t *attach, unsigned flags);
int name_open(const char *name, int flags);
int name_close(int coid);

#endif
