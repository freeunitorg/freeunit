/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) FreeUnit contributors. */

/*
 * A guest whose malloc handlers return offsets that the host must check.
 * The host refuses a bad offset when the worker starts, so the request
 * handler runs only for "edge_malloc".
 */

#include "unit/unit-wasm.h"

#define WASM_PAGE_SIZE   65536u
#define RESPONSE_OFFSET  4096

/*
 * Adds at least "bytes" of new pages to the linear memory and returns its
 * new size.  The handlers below return offsets in the new pages, so the
 * host never writes over the data or the stack of the guest.
 */
static u32
grow_by(u32 bytes)
{
	__builtin_wasm_memory_grow(0, (bytes + WASM_PAGE_SIZE - 1)
				      / WASM_PAGE_SIZE);

	return (u32) __builtin_wasm_memory_size(0) * WASM_PAGE_SIZE;
}

__luw_export_name("bad_malloc_neg")
u32 bad_malloc_neg(size_t size)
{
	(void) size;

	return 0x80000000u;
}

__luw_export_name("bad_malloc_big")
u32 bad_malloc_big(size_t size)
{
	(void) size;

	return 0x7fff0000u;
}

/* The last offset that leaves "size" bytes.  The host accepts it. */
__luw_export_name("edge_malloc")
u32 edge_malloc(size_t size)
{
	return grow_by(size) - size;
}

/*
 * The memory is larger than "size", but the offset leaves 8 bytes too few.
 * The offset is aligned, so only the offset check refuses it.
 */
__luw_export_name("bad_malloc_end")
u32 bad_malloc_end(size_t size)
{
	return grow_by(size) - size + 8;
}

/* The offset leaves enough bytes, but it is odd. */
__luw_export_name("bad_malloc_odd")
u32 bad_malloc_odd(size_t size)
{
	return grow_by(size + WASM_PAGE_SIZE) - size - WASM_PAGE_SIZE + 1;
}

__luw_export_name("luw_request_handler")
int luw_request_handler(u8 *addr)
{
	luw_ctx_t ctx;

	luw_init_ctx(&ctx, addr, RESPONSE_OFFSET);
	luw_mem_writep(&ctx, "accepted\n");
	luw_http_send_response(&ctx);
	luw_http_response_end();

	return 0;
}
