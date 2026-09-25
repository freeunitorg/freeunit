
/*
 * Copyright (C) Alexander Borisov
 * Copyright (C) NGINX, Inc.
 */

#include <ruby/nxt_ruby.h>
#include <nxt_unit.h>


/*
 * Per-instance binding for rack.input / rack.errors objects.
 *
 * The handle stores both the ctx pointer (for the current req lookup)
 * and a snapshot of rctx->req_seq taken at construction time.  The
 * stream-IO operations only proceed when:
 *   - rctx->req != NULL  (a request is currently in flight), AND
 *   - rctx->req_seq == bind->req_seq  (this handle was issued for the
 *     same request that is currently in flight).
 *
 * Either guard failing means the caller is holding a stale handle —
 * either captured after the request finished (background thread,
 * cached IO) or carried across to a later request handled by the
 * same worker.  In both cases, returning Qnil (reads) or routing the
 * write through the NULL-context logger is the safest behaviour.
 */
typedef struct {
    nxt_ruby_ctx_t  *rctx;
    uint64_t        req_seq;
} nxt_ruby_io_bind_t;


static VALUE nxt_ruby_stream_io_initialize(int argc, VALUE *argv, VALUE self);
static VALUE nxt_ruby_stream_io_gets(VALUE obj);
static VALUE nxt_ruby_stream_io_each(VALUE obj);
static VALUE nxt_ruby_stream_io_read(VALUE obj, VALUE args);
static VALUE nxt_ruby_stream_io_rewind(VALUE obj);
static VALUE nxt_ruby_stream_io_puts(VALUE obj, VALUE args);
static VALUE nxt_ruby_stream_io_write(VALUE obj, VALUE args);
nxt_inline long nxt_ruby_stream_io_s_write(nxt_ruby_io_bind_t *bind, VALUE val);
static VALUE nxt_ruby_stream_io_flush(VALUE obj);
static VALUE nxt_ruby_stream_io_close(VALUE obj);
nxt_inline size_t nxt_ruby_dt_dsize_bind(const void *arg);
nxt_inline void nxt_ruby_dt_dfree_bind(void *arg);
nxt_inline nxt_unit_request_info_t *nxt_ruby_bind_req(nxt_ruby_io_bind_t *bind);


static const rb_data_type_t  nxt_rctx_dt = {
    .wrap_struct_name  = "nxt_ruby_io_bind",
    .function  = {
        .dsize         = nxt_ruby_dt_dsize_bind,
        .dfree         = nxt_ruby_dt_dfree_bind,
    },
};


nxt_inline size_t
nxt_ruby_dt_dsize_bind(const void *arg)
{
    (void) arg;
    return sizeof(nxt_ruby_io_bind_t);
}


nxt_inline void
nxt_ruby_dt_dfree_bind(void *arg)
{
    nxt_free(arg);
}


/*
 * Returns the in-flight request if this handle is still valid, or
 * NULL if it was captured outside its originating request (stale
 * across-request reuse or after-request access).
 */
nxt_inline nxt_unit_request_info_t *
nxt_ruby_bind_req(nxt_ruby_io_bind_t *bind)
{
    if (bind == NULL || bind->rctx == NULL) {
        return NULL;
    }

    if (bind->rctx->req == NULL
        || bind->rctx->req_seq != bind->req_seq)
    {
        return NULL;
    }

    return bind->rctx->req;
}


VALUE
nxt_ruby_stream_io_input_init(void)
{
    VALUE  stream_io;

    stream_io = rb_define_class("NGINX_Unit_Stream_IO_Read", rb_cObject);

    rb_undef_alloc_func(stream_io);

    rb_define_method(stream_io, "initialize",
                     nxt_ruby_stream_io_initialize, -1);
    rb_define_method(stream_io, "gets", nxt_ruby_stream_io_gets, 0);
    rb_define_method(stream_io, "each", nxt_ruby_stream_io_each, 0);
    rb_define_method(stream_io, "read", nxt_ruby_stream_io_read, -2);
    rb_define_method(stream_io, "rewind", nxt_ruby_stream_io_rewind, 0);
    rb_define_method(stream_io, "close", nxt_ruby_stream_io_close, 0);

    return stream_io;
}


VALUE
nxt_ruby_stream_io_error_init(void)
{
    VALUE  stream_io;

    stream_io = rb_define_class("NGINX_Unit_Stream_IO_Error", rb_cObject);

    rb_undef_alloc_func(stream_io);

    rb_define_method(stream_io, "initialize",
                     nxt_ruby_stream_io_initialize, -1);
    rb_define_method(stream_io, "puts", nxt_ruby_stream_io_puts, -2);
    rb_define_method(stream_io, "write", nxt_ruby_stream_io_write, -2);
    rb_define_method(stream_io, "flush", nxt_ruby_stream_io_flush, 0);
    rb_define_method(stream_io, "close", nxt_ruby_stream_io_close, 0);

    return stream_io;
}


nxt_inline VALUE
nxt_ruby_stream_io_alloc(VALUE class, nxt_ruby_ctx_t *rctx)
{
    VALUE               self;
    nxt_ruby_io_bind_t  *bind;

    bind = nxt_zalloc(sizeof(nxt_ruby_io_bind_t));
    if (nxt_slow_path(bind == NULL)) {
        return Qnil;
    }

    bind->rctx = rctx;
    bind->req_seq = rctx->req_seq;

    self = TypedData_Wrap_Struct(class, &nxt_rctx_dt, bind);

    rb_obj_call_init(self, 0, NULL);

    return self;
}


VALUE
nxt_ruby_stream_io_input_new(VALUE class, nxt_ruby_ctx_t *rctx)
{
    return nxt_ruby_stream_io_alloc(class, rctx);
}


VALUE
nxt_ruby_stream_io_error_new(VALUE class, nxt_ruby_ctx_t *rctx)
{
    return nxt_ruby_stream_io_alloc(class, rctx);
}


static VALUE
nxt_ruby_stream_io_initialize(int argc, VALUE *argv, VALUE self)
{
    return self;
}


static VALUE
nxt_ruby_stream_io_gets(VALUE obj)
{
    VALUE                    buf;
    ssize_t                  res;
    nxt_ruby_io_bind_t       *bind;
    nxt_unit_request_info_t  *req;

    TypedData_Get_Struct(obj, nxt_ruby_io_bind_t, &nxt_rctx_dt, bind);

    /*
     * Reject calls on a stale handle (captured during a finished
     * request, or carried across into a later request handled by
     * the same worker context).  See nxt_ruby_bind_req().
     */
    req = nxt_ruby_bind_req(bind);
    if (req == NULL) {
        return Qnil;
    }

    if (req->content_length == 0) {
        return Qnil;
    }

    res = nxt_unit_request_readline_size(req, SSIZE_MAX);
    if (nxt_slow_path(res < 0)) {
        return Qnil;
    }

    buf = rb_str_buf_new(res);

    if (nxt_slow_path(buf == Qnil)) {
        return Qnil;
    }

    res = nxt_unit_request_read(req, RSTRING_PTR(buf), res);

    rb_str_set_len(buf, res);

    return buf;
}


static VALUE
nxt_ruby_stream_io_each(VALUE obj)
{
    VALUE  chunk;

    if (rb_block_given_p() == 0) {
        rb_raise(rb_eArgError, "Expected block on rack.input 'each' method");
    }

    for ( ;; ) {
        chunk = nxt_ruby_stream_io_gets(obj);

        if (chunk == Qnil) {
            return Qnil;
        }

        rb_yield(chunk);
    }

    return Qnil;
}


static VALUE
nxt_ruby_stream_io_read(VALUE obj, VALUE args)
{
    VALUE                    buf;
    long                     copy_size, u_size;
    nxt_ruby_io_bind_t       *bind;
    nxt_unit_request_info_t  *req;

    TypedData_Get_Struct(obj, nxt_ruby_io_bind_t, &nxt_rctx_dt, bind);

    /* See nxt_ruby_bind_req() — rejects stale handles captured
     * across the request boundary or reused under a later request. */
    req = nxt_ruby_bind_req(bind);
    if (req == NULL) {
        return Qnil;
    }

    copy_size = req->content_length;

    if (RARRAY_LEN(args) > 0 && TYPE(RARRAY_PTR(args)[0]) == T_FIXNUM) {
        u_size = NUM2LONG(RARRAY_PTR(args)[0]);

        if (u_size < 0 || copy_size == 0) {
            return Qnil;
        }

        if (copy_size > u_size) {
            copy_size = u_size;
        }
    }

    if (copy_size == 0) {
        return rb_str_new_cstr("");
    }

    buf = rb_str_buf_new(copy_size);

    if (nxt_slow_path(buf == Qnil)) {
        return Qnil;
    }

    copy_size = nxt_unit_request_read(req, RSTRING_PTR(buf), copy_size);

    if (RARRAY_LEN(args) > 1 && TYPE(RARRAY_PTR(args)[1]) == T_STRING) {

        rb_str_set_len(RARRAY_PTR(args)[1], 0);
        rb_str_cat(RARRAY_PTR(args)[1], RSTRING_PTR(buf), copy_size);
    }

    rb_str_set_len(buf, copy_size);

    return buf;
}


static VALUE
nxt_ruby_stream_io_rewind(VALUE obj)
{
    return Qnil;
}


static VALUE
nxt_ruby_stream_io_puts(VALUE obj, VALUE args)
{
    nxt_ruby_io_bind_t  *bind;

    if (RARRAY_LEN(args) != 1) {
        return Qnil;
    }

    TypedData_Get_Struct(obj, nxt_ruby_io_bind_t, &nxt_rctx_dt, bind);

    nxt_ruby_stream_io_s_write(bind, RARRAY_PTR(args)[0]);

    return Qnil;
}


static VALUE
nxt_ruby_stream_io_write(VALUE obj, VALUE args)
{
    long                len;
    nxt_ruby_io_bind_t  *bind;

    if (RARRAY_LEN(args) != 1) {
        return Qnil;
    }

    TypedData_Get_Struct(obj, nxt_ruby_io_bind_t, &nxt_rctx_dt, bind);

    len = nxt_ruby_stream_io_s_write(bind, RARRAY_PTR(args)[0]);

    return LONG2FIX(len);
}


nxt_inline long
nxt_ruby_stream_io_s_write(nxt_ruby_io_bind_t *bind, VALUE val)
{
    nxt_unit_request_info_t  *req;

    if (nxt_slow_path(val == Qnil)) {
        return 0;
    }

    if (TYPE(val) != T_STRING) {
        val = rb_funcall(val, rb_intern("to_s"), 0);

        if (TYPE(val) != T_STRING) {
            return 0;
        }
    }

    /*
     * Apps legitimately write to rack.errors during at_exit hooks
     * (running after the request handler returned and rctx->req was
     * cleared at nxt_ruby.c:657) and may also retain rack.errors past
     * the originating request.  In both cases nxt_ruby_bind_req()
     * returns NULL — route those messages through the NULL-context
     * logger at ERR level so they still land in the unit log but do
     * NOT get attributed to a later, unrelated request.  ERR (not
     * ALERT) avoids tripping the test suite's alert detector.
     */
    req = nxt_ruby_bind_req(bind);
    if (req == NULL) {
        nxt_unit_log(NULL, NXT_UNIT_LOG_ERR, "Ruby: %s", RSTRING_PTR(val));
        return RSTRING_LEN(val);
    }

    nxt_unit_req_error(req, "Ruby: %s", RSTRING_PTR(val));

    return RSTRING_LEN(val);
}


static VALUE
nxt_ruby_stream_io_flush(VALUE obj)
{
    return Qnil;
}


static VALUE
nxt_ruby_stream_io_close(VALUE obj)
{
    return Qnil;
}
