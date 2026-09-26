// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_PSN_TRANSPORT_H
#define CHIAKI_PSN_TRANSPORT_H

#include <chiaki/common.h>
#include <chiaki/log.h>

#include <stdbool.h>
#include <stddef.h>

typedef enum psn_http_method_t
{
	PSN_HTTP_GET,
	PSN_HTTP_POST,
	PSN_HTTP_PUT,
	PSN_HTTP_DELETE,
} PsnHttpMethod;

typedef struct psn_http_request_t
{
	PsnHttpMethod method;
	const char *url;
	const char *const *headers;
	size_t headers_count;
	const void *payload;
	size_t payload_size;
	long deadline_ms;
	bool follow_redirects;
	bool keep_response_headers;
} PsnHttpRequest;

typedef struct psn_http_reply_t
{
	long status;
	char *body;
	size_t body_size;
	char *headers;
	char *redirect;
} PsnHttpReply;

ChiakiErrorCode psn_http_execute(ChiakiLog *log, const PsnHttpRequest *request,
	PsnHttpReply *reply);
void psn_http_reply_clear(PsnHttpReply *reply);

char *psn_http_header(const PsnHttpReply *reply, const char *name);
char *psn_http_cookie(const char *name, const char *value);
char *psn_http_bearer(const char *token);
char *psn_url_escape(const char *value);
char *psn_url_parameter(const char *url, const char *name);

#endif
