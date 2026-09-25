/* SPDX-License-Identifier: Apache-2.0 */

/*
 * A small libunit-wasm guest for the legacy wasm runtime's smoke tests.
 *
 * "/" answers with a fixed body.  Any other path names a file the guest
 * tries to open; the host preopens every "access.filesystem" entry with the
 * same path in the guest and hard-coded read+write permissions, so a path
 * inside a preopen reads and anything else fails.  The relative-offset
 * constants below come from the upstream luw-echo-request example.
 */

#include <stdio.h>
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
send_simple(luw_ctx_t *ctx, luw_http_status_t status, const char *body)
{
	char clen[32];

	snprintf(clen, sizeof(clen), "%zu", strlen(body));

	luw_http_set_response_status(status);
	luw_http_init_headers(ctx, 2, 0);
	luw_http_add_header(ctx, "Content-Type", "text/plain");
	luw_http_add_header(ctx, "Content-Length", clen);
	luw_http_send_headers(ctx);
	luw_mem_writep_data(ctx, (const u8 *)body, strlen(body));
	luw_http_send_response(ctx);
	luw_http_response_end();
}

static void
send_file(luw_ctx_t *ctx, const char *path)
{
	char clen[32];
	u8 *data;
	long size;
	FILE *file;

	file = fopen(path, "rb");
	if (file == NULL) {
		send_simple(ctx, LUW_HTTP_NOT_FOUND, "not found\n");
		return;
	}

	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		send_simple(ctx, LUW_HTTP_INTERNAL_SERVER_ERROR, "seek failed\n");
		return;
	}

	size = ftell(file);
	rewind(file);

	if (size < 0) {
		fclose(file);
		send_simple(ctx, LUW_HTTP_INTERNAL_SERVER_ERROR, "tell failed\n");
		return;
	}

	data = malloc(size);
	if (data == NULL) {
		fclose(file);
		send_simple(ctx, LUW_HTTP_INTERNAL_SERVER_ERROR, "out of memory\n");
		return;
	}

	if (fread(data, 1, size, file) != (size_t) size) {
		free(data);
		fclose(file);
		send_simple(ctx, LUW_HTTP_INTERNAL_SERVER_ERROR, "read failed\n");
		return;
	}

	fclose(file);

	snprintf(clen, sizeof(clen), "%ld", size);

	luw_http_init_headers(ctx, 2, 0);
	luw_http_add_header(ctx, "Content-Type", "application/octet-stream");
	luw_http_add_header(ctx, "Content-Length", clen);
	luw_http_send_headers(ctx);
	luw_mem_writep_data(ctx, data, size);
	luw_http_send_response(ctx);
	luw_http_response_end();

	free(data);
}

__luw_export_name("luw_request_handler")
int luw_request_handler(u8 *addr)
{
	luw_ctx_t ctx;
	const char *path;

	luw_init_ctx(&ctx, addr, RESPONSE_OFFSET);
	luw_set_req_buf(&ctx, &request_buf, LUW_SRB_NONE);

	path = luw_get_http_path(&ctx);

	if (strcmp(path, "/") == 0) {
		send_simple(&ctx, LUW_HTTP_OK, "Hello from wasm\n");
	} else {
		send_file(&ctx, path);
	}

	return 0;
}
