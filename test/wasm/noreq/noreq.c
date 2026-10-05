/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) FreeUnit contributors. */

/*
 * A guest that calls a host import before the host serves a request.  It is
 * built without libc or libunit-wasm.  -DCALL_SEND_RESPONSE,
 * -DCALL_SEND_HEADERS or -DCALL_RESPONSE_END selects the import.  With
 * -DCALL_IN_INITIALIZE, the guest exports "_initialize", and wasmtime calls
 * it when it instantiates the module.  "malloc_import" and "init_import"
 * call the import from the malloc handler and from the module_init hook.
 * The other handlers work, so a host that serves a request answers 200 with
 * "ok".
 */

typedef unsigned int  u32;

#define IMPORT(name)  __attribute__((import_module("env"), import_name(name)))
#define EXPORT(name)  __attribute__((export_name(name)))

/* The request buffer starts above the data and the stack of the guest. */
#define BUFFER_OFFSET    (1024 * 1024)
#define RESPONSE_OFFSET  4096

IMPORT("nxt_wasm_send_response") void send_response(u32 offset);
IMPORT("nxt_wasm_send_headers") void send_headers(u32 offset);
IMPORT("nxt_wasm_response_end") void response_end(void);

static void
call_import(void)
{
#if defined(CALL_SEND_RESPONSE)
	send_response(RESPONSE_OFFSET);
#elif defined(CALL_SEND_HEADERS)
	send_headers(RESPONSE_OFFSET);
#elif defined(CALL_RESPONSE_END)
	response_end();
#else
#error "define CALL_SEND_RESPONSE, CALL_SEND_HEADERS or CALL_RESPONSE_END"
#endif
}

#if defined(CALL_IN_INITIALIZE)
EXPORT("_initialize") void
initialize(void)
{
	call_import();
}
#endif

EXPORT("malloc_import") u32
malloc_import(u32 size)
{
	(void) size;

	call_import();

	return BUFFER_OFFSET;
}

EXPORT("init_import") void
init_import(void)
{
	call_import();
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

EXPORT("request_handler") int
request_handler(u32 addr)
{
	unsigned char *resp;

	resp = (unsigned char *) addr + RESPONSE_OFFSET;

	/* The layout of nxt_wasm_response_t: a u32 size, then the data. */
	*(u32 *) resp = 3;
	resp[4] = 'o';
	resp[5] = 'k';
	resp[6] = '\n';

	send_response(RESPONSE_OFFSET);
	response_end();

	return 0;
}
