// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "psn_transport.h"

#include <curl/curl.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdatomic.h>

#ifdef CHIAKI_PS5
extern void cloudplay_catalog_trace(int stage);
extern void cloudplay_network_result(int kind, int value);
extern void cloudplay_http_request(int id, const char *method, const char *url);
extern void cloudplay_http_debug(int id, int kind, const char *data, size_t size);
extern void cloudplay_http_detail(int id, const char *event, const char *detail);
extern void cloudplay_http_response(int id, long status, const char *body, size_t body_size);
extern struct curl_slist *cloudplay_psn_resolves(const char *url);
extern const unsigned char *cloudplay_ca_bundle(size_t *size);
extern void cloudplay_psn_http_lock(void);
extern void cloudplay_psn_http_unlock(void);
extern size_t cloudplay_allocator_metric(int metric);
static CURL *shared_easy;
static bool curl_global_ready;
static _Atomic int traced_requests;
static void trace_request(int request, int step)
{
	if(request < 3)
		cloudplay_catalog_trace(100 + request * 10 + step);
}
#else
static void trace_request(int request, int step) { (void)request; (void)step; }
#endif

typedef struct psn_buffer_t
{
	char *bytes;
	size_t length;
	size_t capacity;
} PsnBuffer;

static size_t psn_buffer_append(char *incoming, size_t width, size_t count, void *opaque)
{
	PsnBuffer *buffer = opaque;
	if(width != 0 && count > SIZE_MAX / width)
		return 0;
	size_t addition = width * count;
	if(addition > SIZE_MAX - buffer->length - 1)
		return 0;
	size_t required = buffer->length + addition + 1;
	if(required > buffer->capacity)
	{
		size_t capacity = buffer->capacity ? buffer->capacity : 16 * 1024;
		while(capacity < required)
		{
			if(capacity > SIZE_MAX / 2)
			{
				capacity = required;
				break;
			}
			capacity *= 2;
		}
		char *resized = realloc(buffer->bytes, capacity);
		if(!resized)
			return 0;
		buffer->bytes = resized;
		buffer->capacity = capacity;
	}
	memcpy(buffer->bytes + buffer->length, incoming, addition);
	buffer->length += addition;
	buffer->bytes[buffer->length] = '\0';
	return addition;
}

#ifndef CHIAKI_PS5
static bool psn_secret_key(const char *start, size_t available, size_t *key_length)
{
	static const char *const names[] = {
		"npsso", "jsessionid", "authorization", "access_token", "refresh_token",
		"id_token", "code", "configkey", "handshakekey", "launchspecification",
		"x-gaikai-session",
	};
	for(size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
	{
		size_t n = strlen(names[i]);
		if(n <= available && strncasecmp(start, names[i], n) == 0)
		{
			*key_length = n;
			return true;
		}
	}
	return false;
}

static void psn_mask_secrets(char *text, size_t length)
{
	for(size_t pos = 0; pos < length; pos++)
	{
		size_t key_length = 0;
		if((pos && (isalnum((unsigned char)text[pos - 1]) || text[pos - 1] == '_'))
			|| !psn_secret_key(text + pos, length - pos, &key_length))
			continue;
		size_t value = pos + key_length;
		while(value < length && strchr(" \t\"':=", text[value]))
			value++;
		if(value == pos + key_length)
			continue;
		if(strncasecmp(text + value, "Bearer ", 7) == 0)
			value += 7;
		while(value < length && !strchr("&\"',; \t\r\n", text[value]))
			text[value++] = '*';
		pos = value;
	}
}

static void psn_log_fragment(ChiakiLog *log, const char *prefix, const char *data, size_t size)
{
	if(!log || !(log->level_mask & CHIAKI_LOG_VERBOSE))
		return;
	char *safe = malloc(size + 1);
	if(!safe)
		return;
	memcpy(safe, data, size);
	safe[size] = '\0';
	psn_mask_secrets(safe, size);
	CHIAKI_LOGV(log, "%s%s", prefix, safe);
	free(safe);
}
#endif

static int psn_curl_trace(CURL *curl, curl_infotype kind, char *data, size_t size, void *opaque)
{
	(void)curl;
#ifdef CHIAKI_PS5
	cloudplay_http_debug(*(const int *)opaque, (int)kind, data, size);
#else
	ChiakiLog *log = opaque;
	switch(kind)
	{
		case CURLINFO_HEADER_OUT: psn_log_fragment(log, "PSN HTTP > ", data, size); break;
		case CURLINFO_DATA_OUT:   psn_log_fragment(log, "PSN HTTP > ", data, size); break;
		case CURLINFO_HEADER_IN:  psn_log_fragment(log, "PSN HTTP < ", data, size); break;
		case CURLINFO_DATA_IN:    psn_log_fragment(log, "PSN HTTP < ", data, size); break;
		default: break;
	}
#endif
	return 0;
}

static const char *psn_method_name(PsnHttpMethod method)
{
	switch(method)
	{
		case PSN_HTTP_POST: return "POST";
		case PSN_HTTP_PUT: return "PUT";
		case PSN_HTTP_DELETE: return "DELETE";
		case PSN_HTTP_GET:
		default: return "GET";
	}
}

ChiakiErrorCode psn_http_execute(ChiakiLog *log, const PsnHttpRequest *request,
	PsnHttpReply *reply)
{
	int trace_number = 0;
#ifdef CHIAKI_PS5
	trace_number = atomic_fetch_add(&traced_requests, 1);
#endif
	trace_request(trace_number, 0);
	if(!request || !reply || !request->url || !*request->url)
		return CHIAKI_ERR_INVALID_DATA;
	#ifdef CHIAKI_PS5
	cloudplay_psn_http_lock();
	#endif
	memset(reply, 0, sizeof(*reply));
	#ifdef CHIAKI_PS5
	cloudplay_http_request(trace_number, psn_method_name(request->method), request->url);
	#endif
	trace_request(trace_number, 1);
	CURLcode global_result = CURLE_OK;
	#ifdef CHIAKI_PS5
	if(!curl_global_ready)
	#endif
		global_result = curl_global_init(CURL_GLOBAL_DEFAULT);
	if(global_result != CURLE_OK)
	{
		#ifdef CHIAKI_PS5
		cloudplay_network_result(10, (int)global_result);
		cloudplay_psn_http_unlock();
		#endif
		return CHIAKI_ERR_UNKNOWN;
	}
	#ifdef CHIAKI_PS5
	curl_global_ready = true;
	#endif
	trace_request(trace_number, 2);
	#ifdef CHIAKI_PS5
	bool reused_handle = shared_easy != NULL;
	CURL *easy = shared_easy;
	if(!easy)
		shared_easy = easy = curl_easy_init();
	else
		curl_easy_reset(easy);
	cloudplay_http_detail(trace_number, "handle", reused_handle ? "reused" : "new");
	#else
	CURL *easy = curl_easy_init();
	#endif
	if(!easy)
	{
		#ifdef CHIAKI_PS5
		cloudplay_http_detail(trace_number, "init_failed", "curl_easy_init returned null");
		cloudplay_psn_http_unlock();
		#endif
		return CHIAKI_ERR_MEMORY;
	}
	trace_request(trace_number, 3);
	char curl_error[CURL_ERROR_SIZE] = { 0 };
	curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, curl_error);

	PsnBuffer response_body = { 0 };
	PsnBuffer response_headers = { 0 };
	struct curl_slist *headers = NULL;
	#ifdef CHIAKI_PS5
	struct curl_slist *resolves = NULL;
	#endif
	ChiakiErrorCode result = CHIAKI_ERR_SUCCESS;

	for(size_t i = 0; i < request->headers_count; i++)
	{
		if(!request->headers[i])
			continue;
		struct curl_slist *next = curl_slist_append(headers, request->headers[i]);
		if(!next)
		{
			result = CHIAKI_ERR_MEMORY;
			goto finish;
		}
		headers = next;
	}

	curl_easy_setopt(easy, CURLOPT_URL, request->url);
	curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
	// Individual Portal/PS Now calls supply their own User-Agent header. Use
	// the catalog's browser agent for generic store/account calls.
	curl_easy_setopt(easy, CURLOPT_USERAGENT,
		"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
	curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, request->deadline_ms > 0 ? request->deadline_ms : 30000L);
	#ifdef CHIAKI_PS5
	curl_easy_setopt(easy, CURLOPT_MAXCONNECTS, 8L);
	#endif
	curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, request->follow_redirects ? 1L : 0L);
	curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, psn_buffer_append);
	curl_easy_setopt(easy, CURLOPT_WRITEDATA, &response_body);
	if(headers)
		curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);

	if(request->keep_response_headers)
	{
		curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, psn_buffer_append);
		curl_easy_setopt(easy, CURLOPT_HEADERDATA, &response_headers);
	}
	if(request->method != PSN_HTTP_GET)
	{
		curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, psn_method_name(request->method));
		const char *payload = request->payload ? request->payload : "";
		size_t payload_size = request->payload_size;
		if(request->payload && payload_size == 0)
			payload_size = strlen(payload);
		curl_easy_setopt(easy, CURLOPT_POSTFIELDS, payload);
		curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)payload_size);
	}

	const char *ca_path = getenv("CHIAKI_CA_BUNDLE");
#ifdef CHIAKI_PS5
	// Provide current IPs only for Sony names blocked by nanoDNS. The original
	// HTTPS URL remains in place for Host, SNI and certificate verification.
	resolves = cloudplay_psn_resolves(request->url);
	if(!resolves || curl_easy_setopt(easy, CURLOPT_RESOLVE, resolves) != CURLE_OK ||
		curl_easy_setopt(easy, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4) != CURLE_OK)
	{
		cloudplay_network_result(3, 1);
		cloudplay_http_detail(trace_number, "setup_failed", "IP map configuration");
		result = CHIAKI_ERR_NETWORK;
		goto finish;
	}
	cloudplay_network_result(3, 0);
	if(curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L) != CURLE_OK ||
		curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L) != CURLE_OK ||
		curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "https") != CURLE_OK ||
		curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "https") != CURLE_OK)
	{
		cloudplay_http_detail(trace_number, "setup_failed", "HTTPS verification or protocol configuration");
		result = CHIAKI_ERR_NETWORK;
		goto finish;
	}
#endif
	if(ca_path && *ca_path)
	{
		if(curl_easy_setopt(easy, CURLOPT_CAINFO, ca_path) != CURLE_OK)
		{
			#ifdef CHIAKI_PS5
			cloudplay_http_detail(trace_number, "setup_failed", "CA bundle configuration");
			#endif
			result = CHIAKI_ERR_NETWORK;
			goto finish;
		}
	}
	#ifdef CHIAKI_PS5
	else
	{
		size_t ca_size = 0;
		const unsigned char *ca_data = cloudplay_ca_bundle(&ca_size);
		struct curl_blob ca_blob = { (void *)ca_data, ca_size, CURL_BLOB_NOCOPY };
		if(!ca_data || curl_easy_setopt(easy, CURLOPT_CAINFO_BLOB, &ca_blob) != CURLE_OK)
		{
			cloudplay_http_detail(trace_number, "setup_failed", "CA bundle in memory");
			result = CHIAKI_ERR_NETWORK;
			goto finish;
		}
	}
	#endif
	#ifdef CHIAKI_PS5
	curl_easy_setopt(easy, CURLOPT_DEBUGFUNCTION, psn_curl_trace);
	curl_easy_setopt(easy, CURLOPT_DEBUGDATA, &trace_number);
	curl_easy_setopt(easy, CURLOPT_VERBOSE, 1L);
	#else
	if(log && (log->level_mask & CHIAKI_LOG_VERBOSE))
	{
		curl_easy_setopt(easy, CURLOPT_VERBOSE, 1L);
		curl_easy_setopt(easy, CURLOPT_DEBUGFUNCTION, psn_curl_trace);
		curl_easy_setopt(easy, CURLOPT_DEBUGDATA, log);
	}
	#endif

	trace_request(trace_number, 4);
	CURLcode curl_result = curl_easy_perform(easy);
	trace_request(trace_number, 5);
	#ifdef CHIAKI_PS5
	long diagnostic_status = 0;
	long os_errno = 0;
	long primary_port = 0;
	long ssl_verify = 0;
	long new_connections = 0;
	char *primary_ip = NULL;
	curl_off_t total_us = 0;
	curl_off_t lookup_us = 0;
	curl_off_t connect_us = 0;
	curl_off_t tls_us = 0;
	curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &diagnostic_status);
	curl_easy_getinfo(easy, CURLINFO_OS_ERRNO, &os_errno);
	curl_easy_getinfo(easy, CURLINFO_PRIMARY_IP, &primary_ip);
	curl_easy_getinfo(easy, CURLINFO_PRIMARY_PORT, &primary_port);
	curl_easy_getinfo(easy, CURLINFO_SSL_VERIFYRESULT, &ssl_verify);
	curl_easy_getinfo(easy, CURLINFO_NUM_CONNECTS, &new_connections);
	curl_easy_getinfo(easy, CURLINFO_TOTAL_TIME_T, &total_us);
	curl_easy_getinfo(easy, CURLINFO_NAMELOOKUP_TIME_T, &lookup_us);
	curl_easy_getinfo(easy, CURLINFO_CONNECT_TIME_T, &connect_us);
	curl_easy_getinfo(easy, CURLINFO_APPCONNECT_TIME_T, &tls_us);
	char metrics[640];
	snprintf(metrics, sizeof(metrics),
		"curl=%d (%s) os_errno=%ld ip=%s port=%ld tls_verify=%ld new_connections=%ld lookup_us=%lld connect_us=%lld tls_us=%lld total_us=%lld allocator_pools=%zu allocator_mapped=%zu allocator_live=%zu allocator_peak=%zu allocator_failures=%zu",
		(int)curl_result, curl_easy_strerror(curl_result), os_errno,
		primary_ip ? primary_ip : "none", primary_port, ssl_verify, new_connections,
		(long long)lookup_us, (long long)connect_us, (long long)tls_us,
		(long long)total_us, cloudplay_allocator_metric(0),
		cloudplay_allocator_metric(1), cloudplay_allocator_metric(2),
		cloudplay_allocator_metric(3), cloudplay_allocator_metric(4));
	cloudplay_http_detail(trace_number, "result", metrics);
	if(curl_error[0]) cloudplay_http_detail(trace_number, "error_buffer", curl_error);
	cloudplay_http_response(trace_number, diagnostic_status,
		response_body.bytes, response_body.length);
	#endif
	if(curl_result != CURLE_OK)
	{
	#ifdef CHIAKI_PS5
		cloudplay_network_result(1, (int)curl_result);
		cloudplay_network_result(4, (int)os_errno);
	#endif
		CHIAKI_LOGE(log, "PSN request failed: %s", curl_easy_strerror(curl_result));
		result = CHIAKI_ERR_NETWORK;
		goto finish;
	}

	curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &reply->status);
	#ifdef CHIAKI_PS5
	cloudplay_network_result(2, (int)reply->status);
	#endif
	char *redirect = NULL;
	curl_easy_getinfo(easy, CURLINFO_REDIRECT_URL, &redirect);
	if(redirect)
		reply->redirect = strdup(redirect);
	reply->body = response_body.bytes;
	reply->body_size = response_body.length;
	response_body.bytes = NULL;
	if(request->keep_response_headers)
	{
		reply->headers = response_headers.bytes;
		response_headers.bytes = NULL;
	}

finish:
	free(response_body.bytes);
	free(response_headers.bytes);
	#ifdef CHIAKI_PS5
	curl_easy_reset(easy);
	#endif
	if(headers)
		curl_slist_free_all(headers);
	#ifndef CHIAKI_PS5
	curl_easy_cleanup(easy);
	#endif
	#ifdef CHIAKI_PS5
	if(resolves)
		curl_slist_free_all(resolves);
	cloudplay_psn_http_unlock();
	#endif
	if(result != CHIAKI_ERR_SUCCESS)
		psn_http_reply_clear(reply);
	trace_request(trace_number, 6);
	return result;
}

void psn_http_reply_clear(PsnHttpReply *reply)
{
	if(!reply)
		return;
	free(reply->body);
	free(reply->headers);
	free(reply->redirect);
	memset(reply, 0, sizeof(*reply));
}

char *psn_http_header(const PsnHttpReply *reply, const char *name)
{
	if(!reply || !reply->headers || !name || !*name)
		return NULL;
	size_t name_size = strlen(name);
	const char *line = reply->headers;
	while(*line)
	{
		const char *end = strpbrk(line, "\r\n");
		size_t line_size = end ? (size_t)(end - line) : strlen(line);
		if(line_size > name_size && line[name_size] == ':' && strncasecmp(line, name, name_size) == 0)
		{
			const char *value = line + name_size + 1;
			while(value < line + line_size && isspace((unsigned char)*value))
				value++;
			const char *value_end = line + line_size;
			while(value_end > value && isspace((unsigned char)value_end[-1]))
				value_end--;
			size_t value_size = (size_t)(value_end - value);
			char *copy = malloc(value_size + 1);
			if(copy)
			{
				memcpy(copy, value, value_size);
				copy[value_size] = '\0';
			}
			return copy;
		}
		if(!end)
			break;
		line = end + 1;
		if(end[0] == '\r' && end[1] == '\n')
			line++;
	}
	return NULL;
}

static char *psn_join_header(const char *prefix, const char *a, const char *separator, const char *b)
{
	if(!prefix || !a || !separator || !b)
		return NULL;
	size_t size = strlen(prefix) + strlen(a) + strlen(separator) + strlen(b) + 1;
	char *value = malloc(size);
	if(value)
		snprintf(value, size, "%s%s%s%s", prefix, a, separator, b);
	return value;
}

char *psn_http_cookie(const char *name, const char *value)
{
	return psn_join_header("Cookie: ", name, "=", value);
}

char *psn_http_bearer(const char *token)
{
	return token ? psn_join_header("Authorization: ", "Bearer", " ", token) : NULL;
}

char *psn_url_escape(const char *value)
{
	if(!value)
		return NULL;
	CURL *easy = curl_easy_init();
	if(!easy)
		return NULL;
	char *escaped = curl_easy_escape(easy, value, 0);
	char *copy = escaped ? strdup(escaped) : NULL;
	if(escaped)
		curl_free(escaped);
	curl_easy_cleanup(easy);
	return copy;
}

char *psn_url_parameter(const char *url, const char *name)
{
	if(!url || !name || !*name)
		return NULL;
	size_t name_size = strlen(name);
	for(const char *cursor = url; (cursor = strstr(cursor, name)); cursor++)
	{
		if(cursor != url && !strchr("?&#", cursor[-1]))
			continue;
		if(cursor[name_size] != '=')
			continue;
		const char *value = cursor + name_size + 1;
		size_t length = strcspn(value, "&#");
		char *copy = malloc(length + 1);
		if(copy)
		{
			memcpy(copy, value, length);
			copy[length] = '\0';
		}
		return copy;
	}
	return NULL;
}
