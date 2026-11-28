/* SPDX-License-Identifier: Apache-2.0 */

/*
 * c-json: a JSON transform service on libunit-wasm and cJSON.
 *
 * POST/PUT a JSON document: the guest parses it with cJSON and answers with
 * a summary (top-level type, member count, string/number totals) plus the
 * document itself, compacted.  GET answers with the same for a built-in
 * sample document, so the service can be probed with a plain curl.
 *
 * cJSON (MIT, https://github.com/DaveGamble/cJSON) is not vendored: build.sh
 * downloads the pinned release and checks its digest.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unit/unit-wasm.h"
#include "cJSON.h"

static u8 *request_buf;

static const char sample[] =
	"{\"service\":\"c-json\",\"items\":[{\"id\":1,\"name\":\"alpha\","
	"\"price\":9.5},{\"id\":2,\"name\":\"beta\",\"price\":12.25}],"
	"\"tags\":[\"wasm\",\"freeunit\"],\"active\":true}";

struct stats {
	int strings;
	int numbers;
	int objects;
	int arrays;
	double sum;
};

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
walk(const cJSON *item, struct stats *st)
{
	const cJSON *child;

	if (cJSON_IsString(item)) {
		st->strings++;
	} else if (cJSON_IsNumber(item)) {
		st->numbers++;
		st->sum += item->valuedouble;
	} else if (cJSON_IsObject(item)) {
		st->objects++;
	} else if (cJSON_IsArray(item)) {
		st->arrays++;
	}

	cJSON_ArrayForEach(child, item) {
		walk(child, st);
	}
}

static void
respond(luw_ctx_t *ctx, luw_http_status_t status, const char *body)
{
	char clen[32];
	size_t len = strlen(body);

	snprintf(clen, sizeof(clen), "%zu", len);

	luw_http_set_response_status(status);
	luw_http_init_headers(ctx, 2, 0);
	luw_http_add_header(ctx, "Content-Type", "application/json");
	luw_http_add_header(ctx, "Content-Length", clen);
	luw_http_send_headers(ctx);
	luw_mem_writep_data(ctx, (const u8 *)body, len);
	luw_http_send_response(ctx);
	luw_http_response_end();
}

__luw_export_name("luw_request_handler")
int luw_request_handler(u8 *addr)
{
	luw_ctx_t ctx;
	const char *method;
	char *input, *out;
	size_t len;
	cJSON *doc, *reply, *summary;
	struct stats st = { 0 };

	luw_init_ctx(&ctx, addr, 4096 /* Response offset */);
	luw_set_req_buf(&ctx, &request_buf, LUW_SRB_NONE);

	method = luw_get_http_method(&ctx);

	if (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) {
		len = luw_get_http_content_len(&ctx);
		input = malloc(len + 1);
		if (input == NULL) {
			respond(&ctx, LUW_HTTP_INTERNAL_SERVER_ERROR,
				"{\"error\":\"out of memory\"}\n");
			return 0;
		}
		memcpy(input, luw_get_http_content(&ctx), len);
		input[len] = '\0';
	} else {
		input = strdup(sample);
	}

	doc = cJSON_Parse(input);
	free(input);

	if (doc == NULL) {
		respond(&ctx, LUW_HTTP_BAD_REQUEST,
			"{\"error\":\"invalid JSON\"}\n");
		return 0;
	}

	walk(doc, &st);

	reply = cJSON_CreateObject();
	summary = cJSON_AddObjectToObject(reply, "summary");
	cJSON_AddStringToObject(summary, "type",
				cJSON_IsObject(doc) ? "object"
				: cJSON_IsArray(doc) ? "array" : "scalar");
	cJSON_AddNumberToObject(summary, "members", cJSON_GetArraySize(doc));
	cJSON_AddNumberToObject(summary, "objects", st.objects);
	cJSON_AddNumberToObject(summary, "arrays", st.arrays);
	cJSON_AddNumberToObject(summary, "strings", st.strings);
	cJSON_AddNumberToObject(summary, "numbers", st.numbers);
	cJSON_AddNumberToObject(summary, "number_sum", st.sum);
	cJSON_AddItemToObject(reply, "document", doc);

	out = cJSON_PrintUnformatted(reply);
	cJSON_Delete(reply);

	if (out == NULL) {
		respond(&ctx, LUW_HTTP_INTERNAL_SERVER_ERROR,
			"{\"error\":\"out of memory\"}\n");
		return 0;
	}

	/* Response buffer is the shared memory past the 4 KiB offset. */
	respond(&ctx, LUW_HTTP_OK, out);
	free(out);

	return 0;
}
