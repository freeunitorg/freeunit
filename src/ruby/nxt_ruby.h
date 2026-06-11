
/*
 * Copyright (C) Alexander Borisov
 * Copyright (C) NGINX, Inc.
 */

#ifndef _NXT_RUBY_H_INCLUDED_
#define _NXT_RUBY_H_INCLUDED_


#include <ruby.h>
#include <ruby/io.h>
#include <ruby/encoding.h>
#include <ruby/version.h>

#include <nxt_main.h>
#include <nxt_router.h>
#include <nxt_runtime.h>
#include <nxt_application.h>
#include <nxt_unit_typedefs.h>


typedef struct {
    VALUE                    env;
    VALUE                    io_input_class;
    VALUE                    io_error_class;
    VALUE                    thread;
    nxt_unit_ctx_t           *ctx;
    nxt_unit_request_info_t  *req;
    /*
     * Monotonic per-rctx counter, bumped on each request entry.
     * Each rack.input / rack.errors instance snapshots this on
     * creation; stream-IO ops reject calls whose snapshot no longer
     * matches rctx->req_seq, so a handle captured during request A
     * cannot read/write the body of request B handled by the same
     * worker.
     */
    uint64_t                 req_seq;
} nxt_ruby_ctx_t;


VALUE nxt_ruby_stream_io_input_init(void);
VALUE nxt_ruby_stream_io_error_init(void);
VALUE nxt_ruby_stream_io_input_new(VALUE class, nxt_ruby_ctx_t *rctx);
VALUE nxt_ruby_stream_io_error_new(VALUE class, nxt_ruby_ctx_t *rctx);

#endif /* _NXT_RUBY_H_INCLUDED_ */
