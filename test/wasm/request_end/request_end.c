/* SPDX-License-Identifier: Apache-2.0 */

/*
 * A libunit-wasm guest that ends its requests in unusual ways.
 *
 * "/trap" sends a full response, ends it, and then traps.  The host must
 * not finish the request a second time.
 *
 * "/noend" sends a response but never calls luw_http_response_end().  The
 * host must finish the request when the handler returns.
 *
 * Any other path sends a normal response.
 */

#include <stdlib.h>
#include <string.h>

#include "unit/unit-wasm.h"

#define RESPONSE_OFFSET 4096

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

static void
send_body(luw_ctx_t *ctx, const char *body)
{
	luw_http_set_response_status(LUW_HTTP_OK);
	luw_http_init_headers(ctx, 1, 0);
	luw_http_add_header(ctx, "Content-Type", "text/plain");
	luw_http_send_headers(ctx);
	luw_mem_writep_data(ctx, (const u8 *)body, strlen(body));
	luw_http_send_response(ctx);
}

__luw_export_name("luw_request_handler")
int luw_request_handler(u8 *addr)
{
	luw_ctx_t ctx;
	const char *path;

	luw_init_ctx(&ctx, addr, RESPONSE_OFFSET);
	luw_set_req_buf(&ctx, &request_buf, LUW_SRB_NONE);

	path = luw_get_http_path(&ctx);

	if (strcmp(path, "/trap") == 0) {
		send_body(&ctx, "trap\n");
		luw_http_response_end();
		__builtin_trap();
	}

	if (strcmp(path, "/noend") == 0) {
		send_body(&ctx, "noend\n");
		return 0;
	}

	send_body(&ctx, "ok\n");
	luw_http_response_end();

	return 0;
}
