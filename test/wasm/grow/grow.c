/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) FreeUnit contributors. */

/*
 * A guest that grows its linear memory by one page before each point where
 * the host reads or writes the memory:
 *
 *   - in the module init hook, before the host writes the first request;
 *   - before it calls nxt_wasm_send_headers();
 *   - before it calls nxt_wasm_send_response();
 *   - at the end of the request handler, before the host writes the next
 *     request.
 *
 * test_wasm_memory_move.py runs it with an engine that moves the memory on
 * each grow.  Built without libc or libunit-wasm.
 */

typedef unsigned int  u32;

#define IMPORT(name)  __attribute__((import_module("env"), import_name(name)))
#define EXPORT(name)  __attribute__((export_name(name)))

/* The request buffer starts above the data and the stack of the guest. */
#define BUFFER_OFFSET    (1024 * 1024)
#define HEADERS_OFFSET   4096
#define RESPONSE_OFFSET  8192

IMPORT("nxt_wasm_send_headers") void send_headers(u32 offset);
IMPORT("nxt_wasm_send_response") void send_response(u32 offset);
IMPORT("nxt_wasm_response_end") void response_end(void);

static int  grow_failed;


static void
grow(void)
{
	if (__builtin_wasm_memory_grow(0, 1) == (__SIZE_TYPE__) -1) {
		grow_failed = 1;
	}
}


/* There is no memcpy() here.  The volatile stores stop clang making one. */
static void
copy(unsigned char *dst, const char *src, u32 len)
{
	volatile unsigned char  *d = dst;

	while (len-- > 0) {
		*d++ = *src++;
	}
}


/* nxt_wasm_response_fields_t with one field.  Offsets are from its start. */
static void
put_headers(unsigned char *rh)
{
	u32  *table = (u32 *) rh;

	table[0] = 1;	/* nfields */
	table[1] = 32;	/* name_off */
	table[2] = 14;	/* name_len */
	table[3] = 48;	/* value_off */
	table[4] = 1;	/* value_len */

	copy(rh + 32, "Content-Length", 14);
	copy(rh + 48, "6", 1);
}


/* nxt_wasm_response_t: a u32 size, then the data. */
static void
put_body(unsigned char *resp, const char *body)
{
	*(u32 *) resp = 6;
	copy(resp + 4, body, 6);
}


EXPORT("malloc_handler") u32
malloc_handler(u32 size)
{
	(void) size;

	return BUFFER_OFFSET;
}


EXPORT("free_handler") void
free_handler(u32 addr)
{
	(void) addr;
}


EXPORT("module_init_handler") void
module_init_handler(void)
{
	grow();
}


EXPORT("request_handler") int
request_handler(u32 addr)
{
	unsigned char  *buf = (unsigned char *) addr;

	grow();
	put_headers(buf + HEADERS_OFFSET);
	send_headers(HEADERS_OFFSET);

	/* If the host reads the old region and it is still mapped: "stale". */
	put_body(buf + RESPONSE_OFFSET, "stale\n");
	grow();
	put_body(buf + RESPONSE_OFFSET, grow_failed ? "nogrow" : "fresh\n");
	send_response(RESPONSE_OFFSET);
	response_end();

	grow();

	return 0;
}
