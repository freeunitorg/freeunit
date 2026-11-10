
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>


#define NXT_HTTP_CHUNK_MIDDLE         0
#define NXT_HTTP_CHUNK_END_ON_BORDER  1
#define NXT_HTTP_CHUNK_END            2

/*
 * Cap on the trailer section's field lines, counted cumulatively across every
 * line and across buffer boundaries.  The section's final CRLF is not counted:
 * it ends the parse either way.
 */
#define NXT_HTTP_TRAILER_MAX_SIZE     4096


#define nxt_size_is_sufficient(cs)                                            \
    (cs < ((__typeof__(cs)) 1 << (sizeof(cs) * 8 - 4)))

/*
 * HTAB, SP, VCHAR and obs-text (RFC 9110, sec 5.6.4): the byte after the "\"
 * of a quoted-pair.  qdtext is the same set without '"' and "\".
 */
#define nxt_http_chunk_ext_text(ch)                                           \
    (((ch) >= 0x20 && (ch) != 0x7f) || (ch) == '\t')


static nxt_int_t nxt_http_chunk_buffer(nxt_http_chunk_parse_t *hcp,
    nxt_buf_t ***tail, nxt_buf_t *in);


/*
 * Trailer field-name and chunk-ext token bytes: tchar, RFC 9110 (sec 5.6.2) --
 * the same token set the request header parser accepts for a field-name.
 * Everything else is 0, including SP, HTAB, ':', ';', '=', '"', DEL and
 * obs-text (%x80-FF, implicitly zero-filled).
 */
static const uint8_t  nxt_http_trailer_tchar[256]  nxt_aligned(64) = {
    /* 0x00-0x0F */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x10-0x1F */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* SP ! " # $ % & ' ( ) * + , - . /  */
                     0, 1, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0,
    /* 0 1 2 3 4 5 6 7 8 9 : ; < = > ?  */
                     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,
    /* @ A B C D E F G H I J K L M N O  */
                     0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* P Q R S T U V W X Y Z [ \ ] ^ _  */
                     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1,
    /* ` a b c d e f g h i j k l m n o  */
                     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* p q r s t u v w x y z { | } ~ DEL */
                     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0,
};


static void nxt_http_chunk_buf_completion(nxt_task_t *task, void *obj,
    void *data);


nxt_buf_t *
nxt_http_chunk_parse(nxt_task_t *task, nxt_http_chunk_parse_t *hcp,
    nxt_buf_t *in)
{
    u_char        c, ch;
    nxt_int_t     ret;
    nxt_buf_t     *b, *out, *next, **tail;
    enum {
        sw_start = 0,
        sw_chunk_size,
        sw_chunk_ext_name_start,
        sw_chunk_ext_name,
        sw_chunk_ext_name_end,
        sw_chunk_ext_value_start,
        sw_chunk_ext_value,
        sw_chunk_ext_quoted,
        sw_chunk_ext_quoted_pair,
        sw_chunk_ext_value_end,
        sw_chunk_ext_bws,
        sw_chunk_size_linefeed,
        sw_chunk_end_newline,
        sw_chunk_end_linefeed,
        sw_chunk,
        sw_trailer_name,
        sw_trailer_value,
        sw_trailer_linefeed,
    } state;

    next = NULL;
    out = NULL;
    tail = &out;

    state = hcp->state;

    for (b = in; b != NULL; b = next) {

        while (b->mem.pos < b->mem.free) {
            /*
             * The sw_chunk state is tested outside the switch
             * to preserve hcp->pos and to not touch memory.
             */
            if (state == sw_chunk) {
                ret = nxt_http_chunk_buffer(hcp, &tail, b);

                if (ret == NXT_HTTP_CHUNK_MIDDLE) {
                    goto next;
                }

                if (nxt_slow_path(ret == NXT_ERROR)) {
                    hcp->error = 1;
                    goto done;
                }

                state = sw_chunk_end_newline;

                if (ret == NXT_HTTP_CHUNK_END_ON_BORDER) {
                    goto next;
                }

                /* ret == NXT_HTTP_CHUNK_END */
            }

            ch = *b->mem.pos++;

            switch (state) {

            case sw_start:
                state = sw_chunk_size;

                c = ch - '0';

                if (c <= 9) {
                    hcp->chunk_size = c;
                    continue;
                }

                c = (ch | 0x20) - 'a';

                if (c <= 5) {
                    hcp->chunk_size = 0x0A + c;
                    continue;
                }

                goto chunk_error;

            case sw_chunk_size:

                c = ch - '0';

                if (c > 9) {
                    c = (ch | 0x20) - 'a';

                    if (nxt_fast_path(c <= 5)) {
                        c += 0x0A;

                    } else if (nxt_fast_path(ch == '\r')) {
                        state = sw_chunk_size_linefeed;
                        continue;

                    } else if (ch == ';') {
                        state = sw_chunk_ext_name_start;
                        continue;

                    } else if (ch == ' ' || ch == '\t') {
                        state = sw_chunk_ext_bws;
                        continue;

                    } else {
                        goto chunk_error;
                    }
                }

                if (nxt_fast_path(nxt_size_is_sufficient(hcp->chunk_size))) {
                    hcp->chunk_size = (hcp->chunk_size << 4) + c;
                    continue;
                }

                goto chunk_error;

            /*
             * chunk-ext = *( BWS ";" BWS chunk-ext-name
             *                [ BWS "=" BWS chunk-ext-val ] ),
             * chunk-ext-name = token, chunk-ext-val = token / quoted-string
             * (RFC 9112, sec 7.1.1).  Extensions are ignored, but their syntax
             * is checked: skipping to the next CR accepted any bytes, and a
             * bare LF in an extension was taken as part of it, so the chunk
             * data was read from the wrong place.  A peer that frames the
             * body differently would disagree with Unit about where the
             * request ends.  Every byte not in the grammar is a chunk_error,
             * a bare LF and a bare CR among them.
             */
            case sw_chunk_ext_name_start:
                if (ch == ' ' || ch == '\t') {
                    continue;
                }

                if (nxt_fast_path(nxt_http_trailer_tchar[ch] != 0)) {
                    state = sw_chunk_ext_name;
                    continue;
                }

                goto chunk_error;

            case sw_chunk_ext_name:
                if (nxt_http_trailer_tchar[ch] != 0) {
                    continue;
                }

                if (ch == '\r') {
                    state = sw_chunk_size_linefeed;
                    continue;
                }

                nxt_fallthrough;

            case sw_chunk_ext_name_end:
                switch (ch) {
                case ' ':
                case '\t':
                    state = sw_chunk_ext_name_end;
                    continue;
                case '=':
                    state = sw_chunk_ext_value_start;
                    continue;
                case ';':
                    state = sw_chunk_ext_name_start;
                    continue;
                }

                goto chunk_error;

            case sw_chunk_ext_value_start:
                if (ch == ' ' || ch == '\t') {
                    continue;
                }

                if (ch == '"') {
                    state = sw_chunk_ext_quoted;
                    continue;
                }

                if (nxt_fast_path(nxt_http_trailer_tchar[ch] != 0)) {
                    state = sw_chunk_ext_value;
                    continue;
                }

                goto chunk_error;

            case sw_chunk_ext_value:
                if (nxt_http_trailer_tchar[ch] != 0) {
                    continue;
                }

                nxt_fallthrough;

            case sw_chunk_ext_value_end:
                /* After a token or the closing quote of a quoted-string. */
                switch (ch) {
                case ' ':
                case '\t':
                    state = sw_chunk_ext_bws;
                    continue;
                case ';':
                    state = sw_chunk_ext_name_start;
                    continue;
                case '\r':
                    state = sw_chunk_size_linefeed;
                    continue;
                }

                goto chunk_error;

            case sw_chunk_ext_bws:
                /*
                 * BWS after the chunk size or after a value may only
                 * precede an extension (RFC 9112, 7.1.1).
                 */
                if (ch == ' ' || ch == '\t') {
                    continue;
                }

                if (nxt_fast_path(ch == ';')) {
                    state = sw_chunk_ext_name_start;
                    continue;
                }

                goto chunk_error;

            case sw_chunk_ext_quoted:
                /* qdtext: HTAB, SP, %x21, %x23-5B, %x5D-7E, obs-text. */
                if (ch == '"') {
                    state = sw_chunk_ext_value_end;
                    continue;
                }

                if (ch == '\\') {
                    state = sw_chunk_ext_quoted_pair;
                    continue;
                }

                if (nxt_fast_path(nxt_http_chunk_ext_text(ch))) {
                    continue;
                }

                goto chunk_error;

            case sw_chunk_ext_quoted_pair:
                /* "\" ( HTAB / SP / VCHAR / obs-text ) */
                if (nxt_fast_path(nxt_http_chunk_ext_text(ch))) {
                    state = sw_chunk_ext_quoted;
                    continue;
                }

                goto chunk_error;

            case sw_chunk_size_linefeed:
                if (nxt_fast_path(ch == '\n')) {

                    if (hcp->chunk_size != 0) {
                        state = sw_chunk;
                        continue;
                    }

                    /*
                     * The terminal 0-chunk was reached.  hcp->trailer marks
                     * that the terminal region was entered (the final CRLF is
                     * still pending), not that a trailer is necessarily
                     * present.  A trailer section may follow before that CRLF;
                     * hcp->last is deferred until the whole terminal sequence
                     * (including any trailer) has been consumed, so a buffer
                     * ending mid-trailer does not desync keepalive.
                     * hcp->chunk_size is 0 here and unused from now on, so it
                     * is reused as the trailer byte counter, bounded by
                     * NXT_HTTP_TRAILER_MAX_SIZE so an abusive upstream cannot
                     * stream an unbounded trailer.  Per RFC 9112 (sec 7.1.2) a
                     * recipient MAY discard trailer fields, so Unit consumes
                     * the trailer only for framing and never exposes it; the
                     * line grammar is still validated, in sw_trailer_name and
                     * sw_trailer_value below.
                     */
                    hcp->trailer = 1;
                    state = sw_chunk_end_newline;
                    continue;
                }

                goto chunk_error;

            case sw_chunk_end_newline:
                if (nxt_fast_path(ch == '\r')) {
                    state = sw_chunk_end_linefeed;
                    continue;
                }

                if (hcp->trailer) {
                    /*
                     * First byte of a trailer field line, which must open a
                     * field-name, i.e. be a tchar.  That rejects every line
                     * with no field-name at all: a CTL, a leading ':' (empty
                     * name), and the SP / HTAB of an obs-fold continuation,
                     * which RFC 9112 (sec 5.2) forbids in a trailer section.
                     */
                    if (nxt_slow_path(nxt_http_trailer_tchar[ch] == 0)) {
                        goto chunk_error;
                    }

                    if (nxt_slow_path(++hcp->chunk_size

                                      > NXT_HTTP_TRAILER_MAX_SIZE))

                    {
                        goto chunk_error;
                    }

                    state = sw_trailer_name;
                    continue;
                }

                goto chunk_error;

            case sw_chunk_end_linefeed:
                if (nxt_fast_path(ch == '\n')) {

                    if (!hcp->trailer) {
                        state = sw_start;
                        continue;
                    }

                    hcp->last = 1;
                    goto done;
                }

                goto chunk_error;

            case sw_chunk:
                /*
                 * This state is processed before the switch.
                 * It added here just to suppress a warning.
                 */
                continue;

            case sw_trailer_name:
                if (ch == ':') {
                    state = sw_trailer_value;

                } else if (nxt_slow_path(nxt_http_trailer_tchar[ch] == 0)) {
                    /*
                     * A field-name is a token, so the only byte that may end it
                     * is the colon.  This is what rejects a trailer line that
                     * never presents one -- "GET /admin HTTP/1.1" and the like,
                     * all printable and CTL-free.  Consuming such a line as a
                     * field to be discarded lets Unit and a peer disagree about
                     * where the message ends: a front end that stops at the
                     * terminal CRLF reads those bytes as the start of the next
                     * request, so one side answers a request the other has
                     * swallowed.  It also rejects the SP before ':' that
                     * RFC 9112 (sec 5.1) requires a server to reject.
                     */
                    goto chunk_error;
                }

                if (nxt_slow_path(++hcp->chunk_size

                                  > NXT_HTTP_TRAILER_MAX_SIZE))

                {
                    goto chunk_error;
                }

                continue;

            case sw_trailer_value:
                if (ch == '\r') {
                    state = sw_trailer_linefeed;

                } else if (nxt_slow_path((ch < 0x20 || ch == 0x7f)
                                         && ch != '\t'))
                {
                    /*
                     * Field values are consumed, not parsed: any VCHAR, SP,
                     * HTAB or obs-text (%x80-FF) may appear.  Only the CTL
                     * bytes RFC 9110 (sec 5.5) forbids in a field value --
                     * %x00-1F / %x7F other than HTAB, e.g. NUL or a bare LF --
                     * are never legitimate; reject them rather than consume.
                     */
                    goto chunk_error;
                }

                if (nxt_slow_path(++hcp->chunk_size

                                  > NXT_HTTP_TRAILER_MAX_SIZE))

                {
                    goto chunk_error;
                }

                continue;

            case sw_trailer_linefeed:
                if (nxt_fast_path(ch == '\n')) {

                    if (nxt_slow_path(++hcp->chunk_size

                                      > NXT_HTTP_TRAILER_MAX_SIZE))

                    {
                        goto chunk_error;
                    }

                    /* Next line is another trailer field or the final CRLF. */
                    state = sw_chunk_end_newline;
                    continue;
                }

                goto chunk_error;
            }
        }

        if (b->retain == 0 && !hcp->retain_buffers) {
            /* No chunk data was found in a buffer. */
            nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                               b->completion_handler, task, b, b->parent);

        }

    next:

        next = b->next;
        b->next = NULL;
    }

    hcp->state = state;

    return out;

chunk_error:

    hcp->chunk_error = 1;

done:

    while (b != NULL) {
        next = b->next;
        b->next = NULL;

        if (b->retain == 0 && !hcp->retain_buffers) {
            nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                               b->completion_handler, task, b, b->parent);
        }

        b = next;
    }

    return out;
}


static nxt_int_t
nxt_http_chunk_buffer(nxt_http_chunk_parse_t *hcp, nxt_buf_t ***tail,
    nxt_buf_t *in)
{
    u_char     *p;
    size_t     size;
    nxt_buf_t  *b;

    p = in->mem.pos;
    size = in->mem.free - p;

    b = nxt_buf_mem_alloc(hcp->mem_pool, 0, 0);
    if (nxt_slow_path(b == NULL)) {
        return NXT_ERROR;
    }

    **tail = b;
    *tail = &b->next;

    b->parent = in;

    if (!hcp->retain_buffers) {
        nxt_mp_retain(hcp->mem_pool);
        b->completion_handler = nxt_http_chunk_buf_completion;
        in->retain++;
    }
    b->mem.pos = p;
    b->mem.start = p;

    if (hcp->chunk_size < size) {
        p += hcp->chunk_size;
        in->mem.pos = p;

        b->mem.free = p;
        b->mem.end = p;

        return NXT_HTTP_CHUNK_END;
    }

    b->mem.free = in->mem.free;
    b->mem.end = in->mem.free;

    hcp->chunk_size -= size;

    if (hcp->chunk_size == 0) {
        /*
         * The whole chunk data is in the output buffer.  Consume it, so
         * that a caller that keeps the bytes after pos for the next read
         * does not parse the data again as framing.
         */
        in->mem.pos = in->mem.free;

        return NXT_HTTP_CHUNK_END_ON_BORDER;
    }

    return NXT_HTTP_CHUNK_MIDDLE;
}


static void
nxt_http_chunk_buf_completion(nxt_task_t *task, void *obj, void *data)
{
    nxt_mp_t   *mp;
    nxt_buf_t  *b, *next, *parent;

    b = obj;

    nxt_debug(task, "buf completion: %p %p", b, b->mem.start);

    nxt_assert(data == b->parent);

    do {
        next = b->next;
        parent = b->parent;
        mp = b->data;

        nxt_mp_free(mp, b);
        nxt_mp_release(mp);

        nxt_buf_parent_completion(task, parent);

        b = next;
    } while (b != NULL);
}
