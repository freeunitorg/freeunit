
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_PORT_H_INCLUDED_
#define _NXT_PORT_H_INCLUDED_


struct nxt_port_handlers_s {
    /* RPC responses. */
    nxt_port_handler_t  rpc_ready;
    nxt_port_handler_t  rpc_error;

    /* Main process RPC requests. */
    nxt_port_handler_t  start_process;
    nxt_port_handler_t  socket;
    nxt_port_handler_t  socket_unlink;
    nxt_port_handler_t  modules;
    nxt_port_handler_t  conf_store;
    nxt_port_handler_t  cert_get;
    nxt_port_handler_t  cert_delete;
    nxt_port_handler_t  script_get;
    nxt_port_handler_t  script_delete;
    nxt_port_handler_t  access_log;

    /* File descriptor exchange. */
    nxt_port_handler_t  change_file;
    nxt_port_handler_t  new_port;
    nxt_port_handler_t  get_port;
    nxt_port_handler_t  port_ack;
    nxt_port_handler_t  mmap;
    nxt_port_handler_t  get_mmap;

    /* New process */
    nxt_port_handler_t  process_created;
    nxt_port_handler_t  process_ready;
    nxt_port_handler_t  whoami;

    /* Process exit/crash notification. */
    nxt_port_handler_t  remove_pid;

    /* Stop process command. */
    nxt_port_handler_t  quit;

    /* Request headers. */
    nxt_port_handler_t  req_headers;
    nxt_port_handler_t  req_headers_ack;
    nxt_port_handler_t  req_body;

    /* Websocket frame. */
    nxt_port_handler_t  websocket_frame;

    /* Various data. */
    nxt_port_handler_t  data;
    nxt_port_handler_t  app_restart;

    /* Status report. */
    nxt_port_handler_t  status;

    nxt_port_handler_t  oosm;
    nxt_port_handler_t  shm_ack;
    nxt_port_handler_t  read_queue;
    nxt_port_handler_t  read_socket;

    /*
     * An application that answered a request and kept running, and later
     * that it has finished.  Appended, and appended is the only safe edit
     * here: every _NXT_PORT_MSG_* value is this struct's offset, so
     * inserting or reordering a slot renumbers the wire protocol.
     */
    nxt_port_handler_t  detached;

    /*
     * A prototype reports a child that died before PROCESS_CREATED, by its
     * namespace-local pid.  Appended for the same reason as the slot above.
     */
    nxt_port_handler_t  remove_child_pid;
};


#define nxt_port_handler_idx(name)                                            \
    ( offsetof(nxt_port_handlers_t, name) / sizeof(nxt_port_handler_t) )

#define nxt_msg_last(handler)                                                 \
    (handler | NXT_PORT_MSG_LAST)

typedef enum {
    NXT_PORT_MSG_LAST             = 0x100,
    NXT_PORT_MSG_CLOSE_FD         = 0x200,
    NXT_PORT_MSG_SYNC             = 0x400,

    NXT_PORT_MSG_MASK             = 0xFF,

    _NXT_PORT_MSG_RPC_READY       = nxt_port_handler_idx(rpc_ready),
    _NXT_PORT_MSG_RPC_ERROR       = nxt_port_handler_idx(rpc_error),

    _NXT_PORT_MSG_START_PROCESS   = nxt_port_handler_idx(start_process),
    _NXT_PORT_MSG_SOCKET          = nxt_port_handler_idx(socket),
    _NXT_PORT_MSG_SOCKET_UNLINK   = nxt_port_handler_idx(socket_unlink),
    _NXT_PORT_MSG_MODULES         = nxt_port_handler_idx(modules),
    _NXT_PORT_MSG_CONF_STORE      = nxt_port_handler_idx(conf_store),
    _NXT_PORT_MSG_CERT_GET        = nxt_port_handler_idx(cert_get),
    _NXT_PORT_MSG_CERT_DELETE     = nxt_port_handler_idx(cert_delete),
    _NXT_PORT_MSG_SCRIPT_GET      = nxt_port_handler_idx(script_get),
    _NXT_PORT_MSG_SCRIPT_DELETE   = nxt_port_handler_idx(script_delete),
    _NXT_PORT_MSG_ACCESS_LOG      = nxt_port_handler_idx(access_log),

    _NXT_PORT_MSG_CHANGE_FILE     = nxt_port_handler_idx(change_file),
    _NXT_PORT_MSG_NEW_PORT        = nxt_port_handler_idx(new_port),
    _NXT_PORT_MSG_GET_PORT        = nxt_port_handler_idx(get_port),
    _NXT_PORT_MSG_PORT_ACK        = nxt_port_handler_idx(port_ack),
    _NXT_PORT_MSG_MMAP            = nxt_port_handler_idx(mmap),
    _NXT_PORT_MSG_GET_MMAP        = nxt_port_handler_idx(get_mmap),

    _NXT_PORT_MSG_PROCESS_CREATED = nxt_port_handler_idx(process_created),
    _NXT_PORT_MSG_PROCESS_READY   = nxt_port_handler_idx(process_ready),
    _NXT_PORT_MSG_WHOAMI          = nxt_port_handler_idx(whoami),
    _NXT_PORT_MSG_REMOVE_PID      = nxt_port_handler_idx(remove_pid),
    _NXT_PORT_MSG_QUIT            = nxt_port_handler_idx(quit),

    _NXT_PORT_MSG_REQ_HEADERS     = nxt_port_handler_idx(req_headers),
    _NXT_PORT_MSG_REQ_HEADERS_ACK = nxt_port_handler_idx(req_headers_ack),
    _NXT_PORT_MSG_REQ_BODY        = nxt_port_handler_idx(req_body),
    _NXT_PORT_MSG_WEBSOCKET       = nxt_port_handler_idx(websocket_frame),

    _NXT_PORT_MSG_DATA            = nxt_port_handler_idx(data),
    _NXT_PORT_MSG_APP_RESTART     = nxt_port_handler_idx(app_restart),
    _NXT_PORT_MSG_STATUS          = nxt_port_handler_idx(status),

    _NXT_PORT_MSG_OOSM            = nxt_port_handler_idx(oosm),
    _NXT_PORT_MSG_SHM_ACK         = nxt_port_handler_idx(shm_ack),
    _NXT_PORT_MSG_READ_QUEUE      = nxt_port_handler_idx(read_queue),
    _NXT_PORT_MSG_READ_SOCKET     = nxt_port_handler_idx(read_socket),

    _NXT_PORT_MSG_DETACHED        = nxt_port_handler_idx(detached),

    _NXT_PORT_MSG_REMOVE_CHILD_PID
                                  = nxt_port_handler_idx(remove_child_pid),

    NXT_PORT_MSG_MAX              = sizeof(nxt_port_handlers_t)
                                    / sizeof(nxt_port_handler_t),

    NXT_PORT_MSG_RPC_READY        = _NXT_PORT_MSG_RPC_READY,
    NXT_PORT_MSG_RPC_READY_LAST   = nxt_msg_last(_NXT_PORT_MSG_RPC_READY),
    NXT_PORT_MSG_RPC_ERROR        = nxt_msg_last(_NXT_PORT_MSG_RPC_ERROR),
    NXT_PORT_MSG_START_PROCESS    = nxt_msg_last(_NXT_PORT_MSG_START_PROCESS),
    NXT_PORT_MSG_SOCKET           = nxt_msg_last(_NXT_PORT_MSG_SOCKET),
    NXT_PORT_MSG_SOCKET_UNLINK    = nxt_msg_last(_NXT_PORT_MSG_SOCKET_UNLINK),
    NXT_PORT_MSG_MODULES          = nxt_msg_last(_NXT_PORT_MSG_MODULES),
    NXT_PORT_MSG_CONF_STORE       = nxt_msg_last(_NXT_PORT_MSG_CONF_STORE),
    NXT_PORT_MSG_CERT_GET         = nxt_msg_last(_NXT_PORT_MSG_CERT_GET),
    NXT_PORT_MSG_CERT_DELETE      = nxt_msg_last(_NXT_PORT_MSG_CERT_DELETE),
    NXT_PORT_MSG_SCRIPT_GET       = nxt_msg_last(_NXT_PORT_MSG_SCRIPT_GET),
    NXT_PORT_MSG_SCRIPT_DELETE    = nxt_msg_last(_NXT_PORT_MSG_SCRIPT_DELETE),
    NXT_PORT_MSG_ACCESS_LOG       = nxt_msg_last(_NXT_PORT_MSG_ACCESS_LOG),
    NXT_PORT_MSG_CHANGE_FILE      = nxt_msg_last(_NXT_PORT_MSG_CHANGE_FILE),
    NXT_PORT_MSG_NEW_PORT         = nxt_msg_last(_NXT_PORT_MSG_NEW_PORT),
    NXT_PORT_MSG_GET_PORT         = nxt_msg_last(_NXT_PORT_MSG_GET_PORT),
    NXT_PORT_MSG_PORT_ACK         = nxt_msg_last(_NXT_PORT_MSG_PORT_ACK),
    NXT_PORT_MSG_MMAP             = nxt_msg_last(_NXT_PORT_MSG_MMAP)
                                    | NXT_PORT_MSG_SYNC,
    NXT_PORT_MSG_GET_MMAP         = nxt_msg_last(_NXT_PORT_MSG_GET_MMAP),

    NXT_PORT_MSG_PROCESS_CREATED  = nxt_msg_last(_NXT_PORT_MSG_PROCESS_CREATED),
    NXT_PORT_MSG_PROCESS_READY    = nxt_msg_last(_NXT_PORT_MSG_PROCESS_READY),
    NXT_PORT_MSG_WHOAMI           = nxt_msg_last(_NXT_PORT_MSG_WHOAMI),
    NXT_PORT_MSG_QUIT             = nxt_msg_last(_NXT_PORT_MSG_QUIT),
    NXT_PORT_MSG_REMOVE_PID       = nxt_msg_last(_NXT_PORT_MSG_REMOVE_PID),

    NXT_PORT_MSG_REQ_HEADERS      = _NXT_PORT_MSG_REQ_HEADERS,
    NXT_PORT_MSG_REQ_BODY         = _NXT_PORT_MSG_REQ_BODY,
    NXT_PORT_MSG_WEBSOCKET        = _NXT_PORT_MSG_WEBSOCKET,
    NXT_PORT_MSG_WEBSOCKET_LAST   = nxt_msg_last(_NXT_PORT_MSG_WEBSOCKET),

    NXT_PORT_MSG_DATA             = _NXT_PORT_MSG_DATA,
    NXT_PORT_MSG_DATA_LAST        = nxt_msg_last(_NXT_PORT_MSG_DATA),
    NXT_PORT_MSG_APP_RESTART      = nxt_msg_last(_NXT_PORT_MSG_APP_RESTART),
    NXT_PORT_MSG_STATUS           = nxt_msg_last(_NXT_PORT_MSG_STATUS),

    NXT_PORT_MSG_OOSM             = nxt_msg_last(_NXT_PORT_MSG_OOSM),
    NXT_PORT_MSG_SHM_ACK          = nxt_msg_last(_NXT_PORT_MSG_SHM_ACK),
    NXT_PORT_MSG_READ_QUEUE       = _NXT_PORT_MSG_READ_QUEUE,
    NXT_PORT_MSG_READ_SOCKET      = _NXT_PORT_MSG_READ_SOCKET,
    NXT_PORT_MSG_DETACHED         = nxt_msg_last(_NXT_PORT_MSG_DETACHED),
    NXT_PORT_MSG_REMOVE_CHILD_PID
                              = nxt_msg_last(_NXT_PORT_MSG_REMOVE_CHILD_PID),
} nxt_port_msg_type_t;


/*
 * The message numbers are on the wire, and a language module built from
 * another release reads the same numbers.  Pin the last slot, so that a slot
 * inserted before it fails the build.  Add new slots after it and move the
 * pin to the new last slot.
 */
nxt_static_assert(_NXT_PORT_MSG_REMOVE_CHILD_PID == 35,
                  "a port message slot was inserted, not appended");


/*
 * Wire-format payload for NXT_PORT_MSG_QUIT.  A single byte selects
 * between fast and graceful exit on the receiving side.  When the
 * message arrives without a payload, the receiver defaults to
 * NXT_PORT_QUIT_NORMAL (see src/nxt_unit.c nxt_unit_process_msg).
 */
typedef enum {
    NXT_PORT_QUIT_NORMAL   = 0,
    NXT_PORT_QUIT_GRACEFUL = 1,
} nxt_port_quit_mode_t;


/*
 * Wire-format payload for NXT_PORT_MSG_DETACHED.  A single byte says which
 * edge this is: an application that has answered a request and is still
 * running, or the same application reporting that work done.
 *
 * A byte rather than a flag on nxt_port_msg_t: that header has no spare
 * bit that is reliably zeroed.  Its four "1 bit" fields are whole bytes,
 * the trailing pad byte is never cleared by the senders that build the
 * header field by field, and nxt_port_socket_write() ORs into ->last.  A
 * new type is bounds-checked on both sides instead, so an older peer
 * refuses the message rather than misreading a flag.
 *
 * The two edges are a balanced pair: one start and one finish per unit of
 * detached work.  The payload names neither a request nor a context, so the
 * router can only count the edges per process; it takes the worker out of
 * the idle economy on the first start and gives it back on the last finish.
 * The payload may grow later, and the handler requires at least one byte and
 * reads the first, so an older sender stays readable.
 */
typedef enum {
    NXT_PORT_DETACHED_START  = 0,
    NXT_PORT_DETACHED_FINISH = 1,
} nxt_port_detached_t;


/* Passed as a first iov chunk. */
typedef struct {
    uint32_t             stream;

    nxt_pid_t            pid;       /* not used on Linux and FreeBSD */

    nxt_port_id_t        reply_port;

    uint8_t              type;

    /* Last message for this stream. */
    uint8_t              last;      /* 1 bit */

    /* Message data send using mmap, next chunk is a nxt_port_mmap_msg_t. */
    uint8_t              mmap;      /* 1 bit */

    /* Non-First fragment in fragmented message sequence. */
    uint8_t              nf;        /* 1 bit */

    /* More Fragments followed. */
    uint8_t              mf;        /* 1 bit */
} nxt_port_msg_t;


typedef struct {
    nxt_queue_link_t    link;
    nxt_buf_t           *buf;
    size_t              share;
    nxt_fd_t            fd[2];
    nxt_port_msg_t      port_msg;
    uint8_t             close_fd;   /* 1 bit */
    uint8_t             allocated;  /* 1 bit */
} nxt_port_send_msg_t;

#if (NXT_HAVE_UCRED) || (NXT_HAVE_MSGHDR_CMSGCRED)
#define NXT_USE_CMSG_PID    1
#endif

struct nxt_port_recv_msg_s {
    nxt_fd_t            fd[2];
    nxt_buf_t           *buf;
    nxt_port_t          *port;
    nxt_port_msg_t      port_msg;
    size_t              size;
#if (NXT_USE_CMSG_PID)
    nxt_pid_t           cmsg_pid;
#endif
    nxt_bool_t          cancelled;
    /*
     * Set by nxt_port_new_port_handler() when it had to create the port
     * u.new_port names, clear when it found one already registered.  A
     * caller that refuses the announcement needs the difference: an existing
     * port is live and must be left alone, while one this message brought
     * into the runtime is the caller's to undo.
     */
    nxt_bool_t          new_port_created;
    union {
        nxt_port_t      *new_port;
        nxt_pid_t       removed_pid;
        void            *data;
    } u;
};


#if (NXT_USE_CMSG_PID)
#define nxt_recv_msg_cmsg_pid(msg)      ((msg)->cmsg_pid)
#define nxt_recv_msg_cmsg_pid_ref(msg)  (&(msg)->cmsg_pid)
#else
#define nxt_recv_msg_cmsg_pid(msg)      ((msg)->port_msg.pid)
#define nxt_recv_msg_cmsg_pid_ref(msg)  (NULL)
#endif


/*
 * Close any file descriptors the peer attached to a received message via
 * SCM_RIGHTS.
 *
 * The ownership contract: nxt_port_read_msg_process() closes whatever is
 * left in msg->fd[] once the message has been dispatched, so a handler that
 * KEEPS a descriptor must set its slot to -1.  A kept descriptor whose slot
 * was left set is closed under the handler, and the number is then handed
 * out again by the next open() or accept() on that thread -- the retained
 * handle silently refers to something else, which is a good deal worse than
 * the leak this arrangement replaced.
 *
 * Calling this explicitly is still right on a reject path, where it makes
 * the intent local and obvious, and it is idempotent.
 */
nxt_inline void
nxt_port_recv_msg_close_fds(nxt_port_recv_msg_t *msg)
{
    if (msg->fd[0] != -1) {
        nxt_fd_close(msg->fd[0]);
        msg->fd[0] = -1;
    }

    if (msg->fd[1] != -1) {
        nxt_fd_close(msg->fd[1]);
        msg->fd[1] = -1;
    }
}


/*
 * How many queued messages on one port may carry a file descriptor.
 *
 * port->messages itself stays unbounded.  A message with no descriptor costs
 * only memory, which was true before a queued message took ownership of what
 * it names, and bounding it would change behaviour on paths that never hold a
 * descriptor at all -- every ordinary reply and every request body fragment.
 *
 * A message that does carry one is different: it holds up to two descriptors
 * of this process open for as long as it waits, so a peer that stops reading
 * turns into RLIMIT_NOFILE pressure on the sender rather than memory pressure
 * alone.  That is the cost this bounds.
 *
 * The traffic being bounded is control-plane and one message per event: a new
 * port, a process start, a listening socket, a certificate, a script, a shared
 * memory segment.  A port with 128 of them outstanding is not a busy port, it
 * is a peer that has stopped reading, so the bound is far above any legitimate
 * burst.  At two descriptors an entry it caps one stalled port at 256 open
 * descriptors, which leaves room for several of them under the 1024 soft
 * RLIMIT_NOFILE that is still the common default.
 */
#define NXT_PORT_MAX_FD_MSGS  128


typedef struct nxt_app_s  nxt_app_t;

struct nxt_port_s {
    nxt_fd_event_t      socket;

    nxt_queue_link_t    link;       /* for nxt_process_t.ports */
    nxt_process_t       *process;

    nxt_queue_link_t    app_link;   /* for nxt_app_t.ports */
    nxt_app_t           *app;
    nxt_port_t          *main_app_port;

    nxt_queue_link_t    idle_link;  /* for nxt_app_t.idle_ports */
    nxt_msec_t          idle_start;

    nxt_queue_t         messages;   /* of nxt_port_send_msg_t */
    nxt_thread_mutex_t  write_mutex;

    /*
     * How many entries of ->messages still carry a file descriptor.
     *
     * A queued message owns the descriptors it names, so each such entry
     * holds up to two of this process's descriptors open for as long as it
     * waits.  The count exists to bound that; it is maintained under
     * ->write_mutex and is described with the bound in src/nxt_port_socket.c.
     *
     * ->fd_refusing is set by the first send the bound refuses and cleared
     * by the next descriptor it takes, also under ->write_mutex.  A peer
     * that stops reading keeps the port at the bound for as long as it
     * stalls, so the refusal is logged once when the port gets there rather
     * than once for every send.
     */
    uint32_t            fd_messages;
    uint8_t             fd_refusing;

    /* Maximum size of message part. */
    uint32_t            max_size;
    /* Maximum interleave of message parts. */
    uint32_t            max_share;

    /*
     * Websocket sessions upgraded from a request this worker answered.  A
     * session is counted by NXT_APR_UPGRADE and uncounted by
     * NXT_APR_WEBSOCKET_CLOSE, both in nxt_router_app_port_release().
     */
    uint32_t            active_websockets;

    /*
     * The application answered a request on this port and kept running.
     * Treated exactly like active_websockets by the idle transition in
     * nxt_router_app_port_idle(): the port stays in app->ports and in
     * app->processes, and stays out of the idle queues, so the reaper
     * never sees it and it keeps counting against "processes": {"max"}.
     */
    uint8_t             detached;

    /*
     * An edge the accounting refused -- an unmatched FINISH, or a START at
     * the maximum count -- was already logged as an alert for this port.
     * Such an edge is a valid message the application can send in a loop,
     * so only the first one is an alert and the rest go to the debug log.
     * Touched only by nxt_router_detached_apply(), on the main thread.
     */
    uint8_t             detached_alerted;

    /*
     * How many units of detached work the application reported of its own:
     * one for each START edge whose FINISH edge has not arrived.  Until the
     * last of them does, the port stays out of the idle economy even with no
     * request left.  A count rather than a flag because the edges carry no
     * context id and a worker may run several contexts -- see
     * nxt_port_detached_t above -- so the first context's FINISH must not
     * speak for the rest.  It saturates at UINT32_MAX and detached_router
     * below does not: this one moves on edges the application sends, which
     * the router cannot bound, while detached_router moves only on requests
     * the router itself gave up on, one per request it has in flight.
     */
    uint32_t            detached_app;

    /*
     * The router put the port in the detached state itself: one for each
     * request it has given up on that the worker is still running.  A
     * "limits": {"timeout"} expiry answers the client while the worker keeps
     * executing, and the port may not rejoin the idle economy until every
     * such request has been answered or the port closes.  A count rather
     * than a flag because one worker can run several of them at once, and
     * kept apart from detached_app so that neither clear drops the other's
     * reason.  As wide as active_requests below it, which counts the same
     * population: "threads" is validated up to NXT_INT32_T_MAX, and a wrap
     * would leave a settle unable to clear the state at all.
     */
    uint32_t            detached_router;

    uint32_t            active_requests;

    nxt_port_handler_t  handler;
    nxt_port_handler_t  *data;

    nxt_mp_t            *mem_pool;
    nxt_event_engine_t  *engine;

    /*
     * The deferral that carries the last reference drop to port->engine.
     * Embedded rather than allocated, so that nxt_port_use() has no failure
     * path -- see the comment there.  Single-instance: use_count reaches
     * zero only on port->engine, so only one thread at a time can hand the
     * last reference over, and at most one post is ever in flight.
     */
    nxt_work_t          release_work;

    /*
     * The write event has to be re-armed on port->engine, and a caller on
     * another engine cannot do it directly.  nxt_port_post() would allocate
     * the item it posts, which fails exactly when the re-arm is needed most:
     * the write that could not be held for a later attempt ran out of memory
     * too.  So the item lives here.
     *
     * ->release_work gets its uniqueness from the reference count -- only one
     * thread can hand over the last reference -- and nothing like that holds
     * for a re-arm, which any thread can want at any time.  ->rearm_pending
     * is that guarantee instead: the poster takes it from 0 to 1 and the
     * handler clears it, so the item is on the engine's queue at most once.
     * Re-arming twice would cost nothing, but linking the same item twice
     * makes it its own successor.
     *
     * ->announce says the shared queue holds items whose READ_QUEUE marker
     * was never sent.  It is a fact about the port rather than work to run,
     * so it is a flag and not a second item: any thread may set it, only
     * port->engine clears it, and it is cleared only once a marker has gone
     * out.
     */
    nxt_work_t          rearm_work;
    nxt_atomic_t        rearm_pending;
    nxt_atomic_t        announce;

    /*
     * The pacing of a re-arm that follows a send which failed for want of
     * kernel memory.
     *
     * ENOBUFS and ENOMEM leave the socket writable, so no edge is coming and
     * the retry has to force the readiness re-check itself.  Done on the
     * spot, that re-check comes straight back: a shortage that lasts turns
     * the engine thread into a tight epoll_wait/sendmsg/epoll_ctl loop,
     * which is the worst moment to burn a core on.  The timer paces it
     * instead -- the retry still happens, just not as fast as the kernel can
     * refuse it.  See issue #407.
     *
     * ->retry_delay is what the next arming waits.  It doubles up to a cap
     * and a successful send puts it back to zero.
     *
     * ->retry_timer.enabled says the timer is holding a port reference: set
     * by nxt_timer_add(), left set while the expired timer waits in the work
     * queue, and cleared by nxt_timer_handler() on the line before it calls
     * nxt_port_retry_handler().  Both fields are touched on port->engine
     * only.
     */
    nxt_timer_t         retry_timer;
    nxt_msec_t          retry_delay;

    nxt_buf_t           *free_bufs;
    nxt_socket_t        pair[2];

    nxt_port_id_t       id;
    nxt_pid_t           pid;

    nxt_lvlhsh_t        rpc_streams; /* stream to nxt_port_rpc_reg_t */
    nxt_lvlhsh_t        rpc_peers;   /* peer to queue of nxt_port_rpc_reg_t */

    nxt_lvlhsh_t        frags;

    nxt_atomic_t        use_count;

    nxt_process_type_t  type;

    nxt_fd_t            queue_fd;
    void                *queue;

    void                *socket_msg;
    int                 from_socket;
};


typedef struct {
    nxt_port_id_t       id;
    nxt_pid_t           pid;
    size_t              max_size;
    size_t              max_share;
    nxt_process_type_t  type:8;
} nxt_port_msg_new_port_t;


typedef struct {
    nxt_port_id_t       id;
    nxt_pid_t           pid;
} nxt_port_msg_get_port_t;


typedef struct {
    uint32_t            id;
} nxt_port_msg_get_mmap_t;


/*
 * nxt_port_data_t size is allocation size
 * which enables effective reuse of memory pool cache.
 */
typedef union {
    nxt_buf_t                buf;
    nxt_port_msg_new_port_t  new_port;
} nxt_port_data_t;


typedef void (*nxt_port_post_handler_t)(nxt_task_t *task, nxt_port_t *port,
    void *data);

nxt_port_t *nxt_port_new(nxt_task_t *task, nxt_port_id_t id, nxt_pid_t pid,
    nxt_process_type_t type);

nxt_port_id_t nxt_port_get_next_id(void);
void nxt_port_reset_next_id(void);

nxt_int_t nxt_port_socket_init(nxt_task_t *task, nxt_port_t *port,
    size_t max_size);
void nxt_port_destroy(nxt_port_t *port);
void nxt_port_close(nxt_task_t *task, nxt_port_t *port);
void nxt_port_write_enable(nxt_task_t *task, nxt_port_t *port);
void nxt_port_write_close(nxt_port_t *port);
void nxt_port_read_enable(nxt_task_t *task, nxt_port_t *port);
void nxt_port_read_close(nxt_port_t *port);

/*
 * Ownership contract:
 *   On NXT_OK, ownership of fd, fd2, and b transfers to the port layer:
 *   the port layer will close the descriptor(s) and run b's completion
 *   handler.
 *   On any other return, ownership remains with the caller, which is
 *   responsible for closing fd/fd2 if owned and dispatching b's
 *   completion handler.
 *   The inline nxt_port_socket_write() wrapper below inherits this
 *   contract.
 */
nxt_int_t nxt_port_socket_write2(nxt_task_t *task, nxt_port_t *port,
    nxt_uint_t type, nxt_fd_t fd, nxt_fd_t fd2, uint32_t stream,
    nxt_port_id_t reply_port, nxt_buf_t *b);


typedef enum {
    /*
     * The message was still whole in port->messages and has been taken back
     * out of it.  Its buffers have been completed and the queue's reference
     * to the port released, so nothing the caller handed to write2() is
     * reachable from the port any more.
     */
    NXT_PORT_MSG_CANCELLED = 0,
    /*
     * Found, but a fragment of it has already gone out (port_msg.nf), so the
     * peer is mid-stream and the descriptors have been handed off.  Left
     * queued: taking it back now would strand the peer on an unfinished
     * stream.
     */
    NXT_PORT_MSG_STARTED,
    /*
     * Not in port->messages.  Either it was never queued or it has been sent
     * in full.  NOT a lifetime boundary on its own: the write handler removes
     * the message and only then queues the buffer completion, which can land
     * behind work already queued ahead of it.
     */
    NXT_PORT_MSG_NOT_FOUND,
} nxt_port_msg_cancel_t;

/*
 * Take a not-yet-started message back out of a port's send queue.
 *
 * Callable only on the thread that owns the port's engine, and only against a
 * message this caller queued: it identifies the message by (type, stream,
 * reply_port) and, when b is not NULL, by buffer identity as well, since a
 * stream number alone is only unique per reply port.
 *
 * Descriptors are treated exactly as the send path treats them, through
 * close_fd: a message that owns its descriptors has them closed here, and one
 * that merely borrows them (START_PROCESS borrows the application's shared
 * port) does not, so cancelling can never close a descriptor its owner is
 * still using.
 */
nxt_port_msg_cancel_t nxt_port_socket_cancel(nxt_task_t *task,
    nxt_port_t *port, nxt_uint_t type, uint32_t stream,
    nxt_port_id_t reply_port, nxt_buf_t *b);

#if (NXT_TESTS)
void nxt_port_test_msg_alloc_failures(nxt_uint_t failures);
void nxt_port_test_run_error_handler(nxt_task_t *task, nxt_port_t *port);
void nxt_port_test_run_read_msg_process(nxt_task_t *task, nxt_port_t *port,
    nxt_port_recv_msg_t *msg);

/*
 * Counts entries into nxt_port_send_new_port().  It is nxt_inline and
 * skips the announced process and the receiver itself, so a fixture with
 * one process cannot tell "not broadcast" from "broadcast to nobody" by
 * observing peers; counting the call itself distinguishes them.
 */
NXT_EXPORT extern nxt_uint_t  nxt_port_test_broadcasts;

/*
 * Counts entries into the event-loop pass of nxt_port_write_msgs() -- the
 * dispatches of the port's write handler.  A retry that is not paced makes
 * this climb with the number of poll passes rather than with elapsed time,
 * which is the whole of what the backoff is there to stop and is not
 * observable from the port's state afterwards.
 */
NXT_EXPORT extern nxt_uint_t  nxt_port_test_write_dispatches;
#endif

nxt_inline nxt_int_t
nxt_port_socket_write(nxt_task_t *task, nxt_port_t *port,
    nxt_uint_t type, nxt_fd_t fd, uint32_t stream, nxt_port_id_t reply_port,
    nxt_buf_t *b)
{
    return nxt_port_socket_write2(task, port, type, fd, -1, stream, reply_port,
                                  b);
}

void nxt_port_enable(nxt_task_t *task, nxt_port_t *port,
    const nxt_port_handlers_t *handlers);
nxt_int_t nxt_port_send_port(nxt_task_t *task, nxt_port_t *port,
    nxt_port_t *new_port, uint32_t stream);
void nxt_port_change_log_file(nxt_task_t *task, nxt_runtime_t *rt,
    nxt_uint_t slot, nxt_fd_t fd);
void nxt_port_remove_notify_others(nxt_task_t *task, nxt_process_t *process);
void nxt_port_rearm(nxt_task_t *task, nxt_port_t *port);
void *nxt_port_queue_mmap(nxt_task_t *task, nxt_fd_t fd, size_t size);

void nxt_port_quit_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg);
void nxt_port_new_port_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg);
void nxt_port_process_ready_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg);
void nxt_port_change_log_file_handler(nxt_task_t *task,
    nxt_port_recv_msg_t *msg);
void nxt_port_mmap_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg);
void nxt_port_data_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg);
void nxt_port_remove_pid_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg);
void nxt_port_empty_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg);

nxt_int_t nxt_port_post(nxt_task_t *task, nxt_port_t *port,
    nxt_port_post_handler_t handler, void *data);
void nxt_port_use(nxt_task_t *task, nxt_port_t *port, int i);
nxt_bool_t nxt_port_use_unless_zero(nxt_port_t *port);

nxt_inline void nxt_port_inc_use(nxt_port_t *port)
{
    nxt_atomic_fetch_add(&port->use_count, 1);
}

#endif /* _NXT_PORT_H_INCLUDED_ */
