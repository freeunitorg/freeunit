/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_CAPABILITY_INCLUDED_
#define _NXT_CAPABILITY_INCLUDED_

typedef struct {
    uint8_t  setid;     /* 1 bit */
    uint8_t  chroot;    /* 1 bit */
    uint8_t  unknown;   /* 1 bit */
} nxt_capabilities_t;


NXT_EXPORT nxt_int_t nxt_capability_set(nxt_task_t *task,
    nxt_capabilities_t *cap);
/*
 * NXT_OK: nothing is left to inherit -- either the sets were emptied,
 * or they were already empty when a filter denied the attempt.
 * NXT_DECLINED: capset() is filtered and this process is still holding
 * capabilities, or cannot determine that it is not -- advisory, the
 * caller decides whether that is fatal.  NXT_ERROR: the call was
 * malformed.
 */
NXT_EXPORT nxt_int_t nxt_capability_drop(nxt_task_t *task);

#endif /* _NXT_CAPABILITY_INCLUDED_ */
