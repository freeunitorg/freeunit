/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) FreeUnit contributors. */

/*
 * A guest with a 64-bit linear memory, built without libc or libunit-wasm.
 * The host must refuse it when the worker starts.  The handlers work, so a
 * host that accepts the module answers 200 with "ok".
 */

typedef unsigned int  u32;

#define IMPORT(name)  __attribute__((import_module("env"), import_name(name)))
#define EXPORT(name)  __attribute__((export_name(name)))

/* The request buffer starts above the data and the stack of the guest. */
#define BUFFER_OFFSET    (1024 * 1024)
#define RESPONSE_OFFSET  4096

IMPORT("nxt_wasm_send_response") void send_response(u32 offset);
IMPORT("nxt_wasm_response_end") void response_end(void);

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

EXPORT("request_handler") int
request_handler(u32 addr)
{
	unsigned char *resp;

	resp = (unsigned char *) (unsigned long) addr + RESPONSE_OFFSET;

	/* The layout of nxt_wasm_response_t: a u32 size, then the data. */
	*(u32 *) resp = 3;
	resp[4] = 'o';
	resp[5] = 'k';
	resp[6] = '\n';

	send_response(RESPONSE_OFFSET);
	response_end();

	return 0;
}
