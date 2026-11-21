/* SPDX-License-Identifier: Apache-2.0 */

/*
 * c-echo: echo the request back as text/plain.
 *
 * Adapted from examples/c/luw-echo-request.c in freeunitorg/freeunit-wasm
 * (libunit-wasm, Apache-2.0):
 *
 *   Copyright (C) Andrew Clayton
 *   Copyright (C) F5, Inc.
 *
 * The smallest of the wasm-bench guests: libunit-wasm plus wasi-libc and a
 * request handler, no other code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "unit/unit-wasm.h"

static u8 *request_buf;

__luw_export_name("luw_module_init_handler")
void luw_module_init_handler(void)
{
	request_buf = malloc(luw_mem_get_init_size());
}

__luw_export_name("luw_module_end_handler")
void luw_module_end_handler(void)
{
	free(request_buf);
}

static bool
hdr_iter_func(luw_ctx_t *ctx, const char *name, const char *value,
	      void *user_data __luw_unused)
{
	luw_mem_writep(ctx, "%s = %s\n", name, value);

	return true;
}

__luw_export_name("luw_request_handler")
int luw_request_handler(u8 *addr)
{
	luw_ctx_t ctx;
	char clen[32];
	const char *method;

	luw_init_ctx(&ctx, addr, 4096 /* Response offset */);
	luw_set_req_buf(&ctx, &request_buf, LUW_SRB_NONE);

	luw_mem_writep(&ctx, "c-echo\n\n[Request Info]\n");
	luw_mem_writep(&ctx, "REQUEST_PATH = %s\n", luw_get_http_path(&ctx));
	luw_mem_writep(&ctx, "METHOD       = %s\n", luw_get_http_method(&ctx));
	luw_mem_writep(&ctx, "VERSION      = %s\n", luw_get_http_version(&ctx));
	luw_mem_writep(&ctx, "QUERY        = %s\n", luw_get_http_query(&ctx));

	luw_mem_writep(&ctx, "\n[Request Headers]\n");
	luw_http_hdr_iter(&ctx, hdr_iter_func, NULL);

	method = luw_get_http_method(&ctx);
	if (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) {
		luw_mem_writep(&ctx, "\n[%s data]\n", method);
		luw_mem_writep_data(&ctx, luw_get_http_content(&ctx),
				    luw_get_http_content_len(&ctx));
		luw_mem_writep(&ctx, "\n");
	}

	luw_http_init_headers(&ctx, 2, 0);

	snprintf(clen, sizeof(clen), "%lu", luw_get_response_data_size(&ctx));
	luw_http_add_header(&ctx, "Content-Type", "text/plain");
	luw_http_add_header(&ctx, "Content-Length", clen);

	luw_http_send_headers(&ctx);
	luw_http_send_response(&ctx);
	luw_http_response_end();

	return 0;
}
