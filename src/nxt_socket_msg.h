/*
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_SOCKET_MSG_H_INCLUDED_
#define _NXT_SOCKET_MSG_H_INCLUDED_

#if (NXT_HAVE_UCRED)
#include <sys/un.h>
#endif


#if (NXT_HAVE_UCRED)
#define NXT_CRED_USECMSG    1
#define NXT_CRED_CMSGTYPE   SCM_CREDENTIALS
#define NXT_CRED_GETPID(u)  (u->pid)

typedef struct ucred        nxt_socket_cred_t;

#elif (NXT_HAVE_MSGHDR_CMSGCRED)
#define NXT_CRED_USECMSG    1
#define NXT_CRED_CMSGTYPE   SCM_CREDS
#define NXT_CRED_GETPID(u)  (u->cmcred_pid)

typedef struct cmsgcred     nxt_socket_cred_t;
#endif

#if (NXT_CRED_USECMSG)
#define NXT_OOB_RECV_SIZE                                                     \
    (CMSG_SPACE(2 * sizeof(int)) + CMSG_SPACE(sizeof(nxt_socket_cred_t)))
#else
#define NXT_OOB_RECV_SIZE                                                     \
    CMSG_SPACE(2 * sizeof(int))
#endif

#if (NXT_HAVE_MSGHDR_CMSGCRED)
#define NXT_OOB_SEND_SIZE                                                     \
    (CMSG_SPACE(2 * sizeof(int)) + CMSG_SPACE(sizeof(nxt_socket_cred_t)))
#else
#define NXT_OOB_SEND_SIZE                                                     \
    CMSG_SPACE(2 * sizeof(int))
#endif


/*
 * NXT_OOB_RECV_SIZE is the largest control block Unit can legitimately be
 * handed: the two descriptors a message may carry -- nxt_port_recv_msg_t
 * has exactly two fd slots and nxt_socket_msg_oob_init() never attaches
 * more -- plus, where the platform passes credentials, the one credential
 * cmsg the kernel or the sender adds.  A receive buffer of that size can
 * therefore never truncate a well-formed message: MSG_CTRUNC means either
 * that the kernel dropped part of the control data it was going to deliver
 * (an SCM_RIGHTS whose descriptors it could not install, typically under
 * RLIMIT_NOFILE pressure), or that a peer sent more control data than any
 * Unit message type uses.  Both are receive errors, not conditions a
 * handler can be expected to notice: what reaches it is a well-formed,
 * authenticated message whose descriptor is simply -1.
 */

typedef struct {
    size_t      size;
    /* recvmsg() reported MSG_CTRUNC: the control data is incomplete. */
    nxt_bool_t  truncated;
    u_char      buf[NXT_OOB_RECV_SIZE];
} nxt_recv_oob_t;


typedef struct {
    size_t  size;
    u_char  buf[NXT_OOB_SEND_SIZE];
} nxt_send_oob_t;


/**
 * The nxt_sendmsg is a wrapper for sendmsg.
 * The oob struct must be initialized using nxt_socket_msg_oob_init().
 */
NXT_EXPORT ssize_t nxt_sendmsg(nxt_socket_t s, nxt_iobuf_t *iob,
    nxt_uint_t niob, const nxt_send_oob_t *oob);

/**
 * The nxt_recvmsg is a wrapper for recvmsg.
 * The oob buffer must be consumed by using nxt_socket_msg_oob_get().
 */
NXT_EXPORT ssize_t nxt_recvmsg(nxt_socket_t s,
    nxt_iobuf_t *iob, nxt_uint_t niob, nxt_recv_oob_t *oob);


nxt_inline struct cmsghdr *
NXT_CMSG_NXTHDR(struct msghdr *msgh, struct cmsghdr *cmsg)
{
#if !defined(__GLIBC__) && defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#endif
    return CMSG_NXTHDR(msgh, cmsg);
#if !defined(__GLIBC__) && defined(__clang__)
#pragma clang diagnostic pop
#endif
}


/*
 * Put a receive buffer into the "carries nothing" state.  Callers that fill
 * oob themselves, or that recycle a buffer, use this rather than assigning
 * ->size alone, so that every field describing the control data is retired
 * together.
 */

nxt_inline void
nxt_socket_msg_oob_reset(nxt_recv_oob_t *oob)
{
    oob->size = 0;
    oob->truncated = 0;
}


nxt_inline void
nxt_socket_msg_oob_init(nxt_send_oob_t *oob, int *fds)
{
    int             nfds;
    struct cmsghdr  *cmsg;

    *oob = (nxt_send_oob_t) {};

#if (NXT_HAVE_MSGHDR_CMSGCRED)
    cmsg = (struct cmsghdr *) (oob->buf);
    /*
     * Fill all padding fields with 0.
     * Code in Go 1.11 validate cmsghdr using padding field as part of len.
     * See Cmsghdr definition and socketControlMessageHeaderAndData function.
     */
    nxt_memzero(cmsg, sizeof(struct cmsghdr));

    cmsg->cmsg_len = CMSG_LEN(sizeof(nxt_socket_cred_t));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = NXT_CRED_CMSGTYPE;

    oob->size = CMSG_SPACE(sizeof(nxt_socket_cred_t));

#else
    oob->size = 0;
#endif

    nfds = (fds[0] != -1 ? 1 : 0) + (fds[1] != -1 ? 1 : 0);

    if (nfds == 0) {
        return;
    }

    cmsg = (struct cmsghdr *) (oob->buf + oob->size);

    nxt_memzero(cmsg, sizeof(struct cmsghdr));

    cmsg->cmsg_len = CMSG_LEN(nfds * sizeof(int));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;

    /*
     * nxt_memcpy() is used instead of simple
     *   *(int *) CMSG_DATA(&cmsg.cm) = fd;
     * because GCC 4.4 with -O2/3/s optimization may issue a warning:
     *   dereferencing type-punned pointer will break strict-aliasing rules
     *
     * Fortunately, GCC with -O1 compiles this nxt_memcpy()
     * in the same simple assignment as in the code above.
     */
    nxt_memcpy(CMSG_DATA(cmsg), fds, nfds * sizeof(int));

    oob->size += CMSG_SPACE(nfds * sizeof(int));
}


nxt_inline nxt_int_t
nxt_socket_msg_oob_get_fds(nxt_recv_oob_t *oob, nxt_fd_t *fd)
{
    size_t          size;
    struct msghdr   msg;
    struct cmsghdr  *cmsg;

    msg.msg_control = oob->buf;
    msg.msg_controllen = oob->size;

    for (cmsg = CMSG_FIRSTHDR(&msg);
         cmsg != NULL;
         cmsg = NXT_CMSG_NXTHDR(&msg, cmsg))
    {
        size = cmsg->cmsg_len - CMSG_LEN(0);

        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            if (nxt_slow_path(size != sizeof(int) && size != 2 * sizeof(int))) {
                return NXT_ERROR;
            }

            nxt_memcpy(fd, CMSG_DATA(cmsg), size);

            break;
        }
    }

    /*
     * Checked after the loop, not before it: a truncated control block can
     * still have delivered a descriptor, and the caller can only close what
     * it has been told about.
     */
    if (nxt_slow_path(oob->truncated)) {
        return NXT_ERROR;
    }

    return NXT_OK;
}


nxt_inline nxt_int_t
nxt_socket_msg_oob_get(nxt_recv_oob_t *oob, nxt_fd_t *fd, nxt_pid_t *pid)
{
    size_t          size;
    struct msghdr   msg;
    struct cmsghdr  *cmsg;

    if (oob->size == 0 && !oob->truncated) {
        return NXT_OK;
    }

#if (NXT_CRED_USECMSG)
    *pid = -1;
#endif

    msg.msg_control = oob->buf;
    msg.msg_controllen = oob->size;

    for (cmsg = CMSG_FIRSTHDR(&msg);
         cmsg != NULL;
         cmsg = NXT_CMSG_NXTHDR(&msg, cmsg))
    {
        size = cmsg->cmsg_len - CMSG_LEN(0);

        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            if (nxt_slow_path(size != sizeof(int) && size != 2 * sizeof(int))) {
                return NXT_ERROR;
            }

            nxt_memcpy(fd, CMSG_DATA(cmsg), size);

#if (!NXT_CRED_USECMSG)
            break;
#endif
        }

#if (NXT_CRED_USECMSG)
        else if (cmsg->cmsg_level == SOL_SOCKET
                 && cmsg->cmsg_type == NXT_CRED_CMSGTYPE)
        {
            nxt_socket_cred_t  *creds;

            if (nxt_slow_path(size != sizeof(nxt_socket_cred_t))) {
                return NXT_ERROR;
            }

            creds = (nxt_socket_cred_t *) CMSG_DATA(cmsg);
            *pid = NXT_CRED_GETPID(creds);
        }
#endif
    }

    /* See nxt_socket_msg_oob_get_fds() on why this follows the loop. */
    if (nxt_slow_path(oob->truncated)) {
        return NXT_ERROR;
    }

#if (NXT_CRED_USECMSG)
    /* For platforms supporting credential passing, it's enforced */
    if (nxt_slow_path(*pid == -1)) {
        return NXT_ERROR;
    }
#endif

    return NXT_OK;
}


#endif /* _NXT_SOCKET_MSG_H_INCLUDED_ */
