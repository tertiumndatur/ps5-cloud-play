// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifdef _WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

#include <chiaki/cloudcatalog.h>
#include <chiaki/remote/holepunch.h>

#include "cloud_json.h"
#include "psn_transport.h"

#include <json-c/json_tokener.h>
#include <json-c/linkhash.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#ifdef CHIAKI_PS5
extern void cloudplay_catalog_trace(int stage);
extern void cloudplay_catalog_metric(int metric, int value);
#define CLOUD_TRACE(stage) cloudplay_catalog_trace(stage)
#define CLOUD_METRIC(metric, value) cloudplay_catalog_metric((metric), (int)(value))
#else
#define CLOUD_TRACE(stage) ((void)0)
#define CLOUD_METRIC(metric, value) ((void)0)
#endif

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define cloud_mkdir(path) _mkdir(path)
#define cloud_unlink(path) DeleteFileA(path)
#else
#include <unistd.h>
#define cloud_mkdir(path) mkdir(path, 0755)
#define cloud_unlink(path) unlink(path)
#endif

#define CATALOG_CACHE_SECONDS (24 * 60 * 60)
#define CATALOG_CACHE_FILE "catalog.json"

#define PSN_ACCOUNT_API "https://ca.account.sony.com/api"
#define PSNOW_API "https://psnow.playstation.com/kamaji/api/pcnow/00_09_000"
#define PSNOW_REDIRECT "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/grc-response.html"
#define PSNOW_CLIENT_ID "bc6b0777-abb5-40da-92ca-e133cf18e989"
#define COMMERCE_CLIENT_ID "dc523cc2-b51b-4190-bff0-3397c06871b3"
#define PSNOW_AGENT "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) playstation-now/0.0.0 Chrome/83.0.4103.104 Electron/9.0.4 Safari/537.36 gkApollo"
#define BROWSER_AGENT "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"

typedef struct cloud_account_t
{
	char country[8];
	char language[8];
	char store_country[8];
	char store_language[8];
	char store_root[1024];
	char session_cookie[512];
	bool authenticated;
	bool region_supported;
	bool transport_failed;
} CloudAccount;

typedef struct cloud_sources_t
{
	struct json_object *psnow;
	struct json_object *browse;
	struct json_object *owned;
	bool browse_complete;
	bool psnow_complete;
	bool owned_complete;
} CloudSources;

static const char *cloud_case_find(const char *text, const char *needle)
{
	if(!text || !needle || !*needle)
		return text;
	size_t needle_size = strlen(needle);
	for(; *text; text++)
		if(strncasecmp(text, needle, needle_size) == 0)
			return text;
	return NULL;
}

static const char *const cloud_locales[] = {
	"en-US", "en-GB", "en-RU", "de-DE", "fr-FR", "fi-FI",
	"it-IT", "es-ES", "nl-NL", "pt-BR", "ja-JP", "ko-KR",
};

static const char *const imagic_lists[] = {
	"plus-games-list",
	"ubisoft-classics-list",
	"plus-classics-list",
	"plus-monthly-games-list",
	"free-to-play-list",
	"all-ps5-list",
};

static void cloud_copy_text(char *destination, size_t capacity, const char *source)
{
	if(!destination || capacity == 0)
		return;
	snprintf(destination, capacity, "%s", source ? source : "");
}

static void cloud_locale_normalize(const char *raw, char output[16])
{
	const char *input = raw && *raw ? raw : "en-US";
	char language[8] = { 0 };
	char country[8] = { 0 };
	size_t language_size = strcspn(input, "-_");
	if(language_size >= sizeof(language))
		language_size = sizeof(language) - 1;
	for(size_t i = 0; i < language_size; i++)
		language[i] = (char)tolower((unsigned char)input[i]);
	const char *separator = input[language_size] ? input + language_size + 1 : NULL;
	if(separator)
	{
		size_t country_size = strcspn(separator, " \t");
		if(country_size >= sizeof(country))
			country_size = sizeof(country) - 1;
		for(size_t i = 0; i < country_size; i++)
			country[i] = (char)toupper((unsigned char)separator[i]);
	}
	if(!*language)
		cloud_copy_text(language, sizeof(language), "en");
	if(!*country)
		cloud_copy_text(country, sizeof(country), "US");
	snprintf(output, 16, "%s-%s", language, country);
}

static void cloud_locale_country(const char *locale, char output[8])
{
	char normalized[16];
	cloud_locale_normalize(locale, normalized);
	const char *dash = strchr(normalized, '-');
	cloud_copy_text(output, 8, dash ? dash + 1 : "US");
}

void chiaki_cloud_gaikai_language(const char *locale, char *output, size_t output_size)
{
	if(!output || output_size == 0)
		return;
	const char *cursor = locale ? locale : "";
	while(isspace((unsigned char)*cursor))
		cursor++;
	size_t length = 0;
	while(cursor[length] && cursor[length] != '-' && cursor[length] != '_'
		&& length + 1 < output_size)
	{
		output[length] = (char)tolower((unsigned char)cursor[length]);
		length++;
	}
	output[length] = '\0';
	if(length == 0)
		cloud_copy_text(output, output_size, "en");
}

size_t chiaki_cloud_supported_locale_count(void)
{
	return sizeof(cloud_locales) / sizeof(cloud_locales[0]);
}

const char *chiaki_cloud_supported_locale(size_t index)
{
	return index < chiaki_cloud_supported_locale_count() ? cloud_locales[index] : "";
}

static bool cloud_path(char *output, size_t output_size, const char *directory, const char *name)
{
	if(!output || !output_size || !directory || !*directory || !name || !*name)
		return false;
	return snprintf(output, output_size, "%s/%s", directory, name) < (int)output_size;
}

static bool cloud_make_directories(const char *directory)
{
	if(!directory || !*directory)
		return false;
	char path[2048];
	if(strlen(directory) >= sizeof(path))
		return false;
	cloud_copy_text(path, sizeof(path), directory);
	for(char *cursor = path + 1; *cursor; cursor++)
	{
		if(*cursor != '/' && *cursor != '\\')
			continue;
		char saved = *cursor;
		*cursor = '\0';
		if(cloud_mkdir(path) != 0 && errno != EEXIST)
			return false;
		*cursor = saved;
	}
	return cloud_mkdir(path) == 0 || errno == EEXIST;
}

static struct json_object *cloud_cache_load(const char *directory, const char *name, bool accept_expired)
{
	char path[2300];
	if(!cloud_path(path, sizeof(path), directory, name))
		return NULL;
	struct stat attributes;
	if(stat(path, &attributes) != 0)
		return NULL;
	if(!accept_expired && time(NULL) - attributes.st_mtime > CATALOG_CACHE_SECONDS)
		return NULL;
	FILE *file = fopen(path, "rb");
	if(!file)
		return NULL;
	if(fseek(file, 0, SEEK_END) != 0)
	{
		fclose(file);
		return NULL;
	}
	long file_size = ftell(file);
	if(file_size <= 0 || file_size > 128 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0)
	{
		fclose(file);
		return NULL;
	}
	char *data = malloc((size_t)file_size + 1);
	if(!data)
	{
		fclose(file);
		return NULL;
	}
	size_t read_size = fread(data, 1, (size_t)file_size, file);
	fclose(file);
	data[read_size] = '\0';
	struct json_object *json = read_size == (size_t)file_size ? json_tokener_parse(data) : NULL;
	free(data);
	return json;
}

static void cloud_cache_save(const char *directory, const char *name, struct json_object *json)
{
	if(!json || !cloud_make_directories(directory))
		return;
	char path[2300];
	char temporary[2350];
	if(!cloud_path(path, sizeof(path), directory, name))
		return;
	if(snprintf(temporary, sizeof(temporary), "%s.new", path) >= (int)sizeof(temporary))
		return;
	FILE *file = fopen(temporary, "wb");
	if(!file)
		return;
	const char *serialized = json_object_to_json_string_ext(json, JSON_C_TO_STRING_PLAIN);
	size_t size = strlen(serialized);
	bool written = fwrite(serialized, 1, size, file) == size && fflush(file) == 0;
	fclose(file);
	if(written)
	{
#ifdef _WIN32
		MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING);
#else
		rename(temporary, path);
#endif
	}
	else
		cloud_unlink(temporary);
}

static struct json_object *cloud_parse_reply(const PsnHttpReply *reply)
{
	if(!reply || !reply->body || !reply->body_size)
		return NULL;
	return json_tokener_parse(reply->body);
}

static bool cloud_kamaji_success(struct json_object *envelope)
{
	struct json_object *header = cloud_json_object(envelope, "header");
	return header && strcmp(cloud_json_string(header, "status_code"), "0x0000") == 0;
}

static char *cloud_cookie_value(const char *headers, const char *cookie_name)
{
	if(!headers || !cookie_name)
		return NULL;
	size_t name_size = strlen(cookie_name);
	const char *cursor = headers;
	while((cursor = cloud_case_find(cursor, cookie_name)))
	{
		if(cursor[name_size] != '=')
		{
			cursor++;
			continue;
		}
		const char *value = cursor + name_size + 1;
		size_t size = strcspn(value, ";\r\n");
		char *copy = malloc(size + 1);
		if(copy)
		{
			memcpy(copy, value, size);
			copy[size] = '\0';
		}
		return copy;
	}
	return NULL;
}

static bool cloud_parse_store_root(const char *url, CloudAccount *account)
{
	const char *container = url ? strstr(url, "/container/") : NULL;
	if(!container)
		return false;
	container += strlen("/container/");
	const char *country_end = strchr(container, '/');
	const char *language_end = country_end ? strchr(country_end + 1, '/') : NULL;
	if(!country_end || !language_end || country_end == container || language_end == country_end + 1)
		return false;
	size_t country_size = (size_t)(country_end - container);
	size_t language_size = (size_t)(language_end - country_end - 1);
	if(country_size >= sizeof(account->store_country) || language_size >= sizeof(account->store_language))
		return false;
	memcpy(account->store_country, container, country_size);
	account->store_country[country_size] = '\0';
	memcpy(account->store_language, country_end + 1, language_size);
	account->store_language[language_size] = '\0';
	return true;
}

static void cloud_account_discover(ChiakiLog *log, const char *npsso, CloudAccount *account)
{
	CLOUD_TRACE(10);
	memset(account, 0, sizeof(*account));
	if(!npsso || !*npsso)
		return;
	#ifdef CHIAKI_PS5
	CLOUD_TRACE(31);
	struct json_object *parser_check = json_tokener_parse(
		"{\"header\":{\"status_code\":\"0x0000\"},\"data\":{\"value\":1.25,\"items\":[true,null]}}");
	CLOUD_TRACE(parser_check ? 32 : 33);
	if(!parser_check)
	{
		account->transport_failed = true;
		return;
	}
	json_object_put(parser_check);
	#endif

	char duid[CHIAKI_DUID_STR_SIZE] = { 0 };
	size_t duid_size = sizeof(duid);
	if(chiaki_holepunch_generate_client_device_uid(duid, &duid_size) != CHIAKI_ERR_SUCCESS)
	{
		account->transport_failed = true;
		return;
	}
	CLOUD_TRACE(11);
	char *scope = psn_url_escape("kamaji:commerce_native kamaji:commerce_container kamaji:lists kamaji:s2s.subscriptionsPremium.get");
	char *redirect = psn_url_escape(PSNOW_REDIRECT);
	char *device = psn_url_escape(duid);
	if(!scope || !redirect || !device)
	{
		free(scope); free(redirect); free(device);
		account->transport_failed = true;
		return;
	}
	CLOUD_TRACE(12);
	char url[2300];
	snprintf(url, sizeof(url),
		PSN_ACCOUNT_API "/v1/oauth/authorize?smcid=pc%%3Apsnow&applicationId=psnow&response_type=code"
		"&scope=%s&client_id=" PSNOW_CLIENT_ID "&redirect_uri=%s&service_entity=urn%%3Aservice-entity%%3Apsn"
		"&prompt=none&mid=PSNOW&duid=%s&layout_type=popup&service_logo=ps&tp_psn=true&noEVBlock=true",
		scope, redirect, device);
	free(scope); free(redirect); free(device);
	char *npsso_header = psn_http_cookie("npsso", npsso);
	const char *oauth_headers[] = { "User-Agent: " PSNOW_AGENT, npsso_header };
	PsnHttpRequest oauth_request = {
		.method = PSN_HTTP_GET, .url = url, .headers = oauth_headers, .headers_count = 2,
		.keep_response_headers = true,
	};
	PsnHttpReply oauth_reply;
	CLOUD_TRACE(13);
	ChiakiErrorCode error = psn_http_execute(log, &oauth_request, &oauth_reply);
	CLOUD_TRACE(14);
	free(npsso_header);
	if(error != CHIAKI_ERR_SUCCESS)
	{
		account->transport_failed = true;
		return;
	}
	char *location = psn_http_header(&oauth_reply, "Location");
	char *code = psn_url_parameter(location ? location : oauth_reply.redirect, "code");
	free(location);
	psn_http_reply_clear(&oauth_reply);
	if(!code)
		return;

	char session_body[1400];
	snprintf(session_body, sizeof(session_body), "code=%s&client_id=" PSNOW_CLIENT_ID "&duid=%s", code, duid);
	free(code);
	const char *session_headers[] = {
		"Content-Type: text/plain;charset=UTF-8", "Accept: */*", "User-Agent: " PSNOW_AGENT,
		"Origin: https://psnow.playstation.com",
		"Referer: https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/",
		"X-Alt-Referer: " PSNOW_REDIRECT,
	};
	PsnHttpRequest session_request = {
		.method = PSN_HTTP_POST, .url = PSNOW_API "/user/session",
		.headers = session_headers, .headers_count = 6, .payload = session_body,
		.keep_response_headers = true,
	};
	PsnHttpReply session_reply;
	error = psn_http_execute(log, &session_request, &session_reply);
	CLOUD_TRACE(30);
	if(error != CHIAKI_ERR_SUCCESS)
	{
		account->transport_failed = true;
		return;
	}
	CLOUD_TRACE(34);
	struct json_object *session_json = cloud_parse_reply(&session_reply);
	CLOUD_TRACE(35);
	bool session_success = session_json && cloud_kamaji_success(session_json);
	if(session_success)
	{
		struct json_object *data = cloud_json_object(session_json, "data");
		cloud_copy_text(account->country, sizeof(account->country), cloud_json_string(data, "country"));
		cloud_copy_text(account->language, sizeof(account->language), cloud_json_string(data, "language"));
		char *cookie = cloud_cookie_value(session_reply.headers, "JSESSIONID");
		if(cookie)
		{
			cloud_copy_text(account->session_cookie, sizeof(account->session_cookie), cookie);
			free(cookie);
			account->authenticated = true;
		}
	}
	if(session_json)
		json_object_put(session_json);
	psn_http_reply_clear(&session_reply);
	if(!account->authenticated)
		return;

	char *session_cookie = psn_http_cookie("JSESSIONID", account->session_cookie);
	const char *store_headers[] = {
		"User-Agent: " PSNOW_AGENT, session_cookie, "Accept: application/json",
		"Origin: https://psnow.playstation.com",
		"Referer: https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/",
	};
	PsnHttpRequest store_request = {
		.method = PSN_HTTP_GET, .url = PSNOW_API "/user/stores",
		.headers = store_headers, .headers_count = 5,
	};
	PsnHttpReply store_reply;
	error = psn_http_execute(log, &store_request, &store_reply);
	free(session_cookie);
	if(error != CHIAKI_ERR_SUCCESS)
	{
		account->transport_failed = true;
		return;
	}
	if(store_reply.status == 200)
	{
		struct json_object *store_json = cloud_parse_reply(&store_reply);
		if(store_json && cloud_kamaji_success(store_json))
		{
			struct json_object *data = cloud_json_object(store_json, "data");
			cloud_copy_text(account->store_root, sizeof(account->store_root), cloud_json_string(data, "base_url"));
			account->region_supported = *account->store_root && cloud_parse_store_root(account->store_root, account);
		}
		if(store_json)
			json_object_put(store_json);
	}
	else if(store_reply.status >= 500)
		account->transport_failed = true;
	psn_http_reply_clear(&store_reply);
}

static const char *cloud_cover(struct json_object *game, char output[1024])
{
	output[0] = '\0';
	struct json_object *images = cloud_json_array(game, "images");
	int preferred_types[] = { 10, 12, 13 };
	for(size_t type_index = 0; images && type_index < 3 && !*output; type_index++)
	{
		for(size_t i = 0; i < json_object_array_length(images); i++)
		{
			struct json_object *image = json_object_array_get_idx(images, i);
			if(cloud_json_integer(image, "type") == preferred_types[type_index])
			{
				cloud_copy_text(output, 1024, cloud_json_string(image, "url"));
				break;
			}
		}
	}
	if(!*output)
		cloud_copy_text(output, 1024, cloud_json_string(game, "imageUrl"));
	return output;
}

static bool cloud_is_product(struct json_object *row)
{
	const char *type = cloud_json_string(row, "container_type");
	return !*type || cloud_string_equal_ci(type, "product");
}

static void cloud_reference_member(struct json_object *destination,
	struct json_object *source, const char *key)
{
	struct json_object *value = NULL;
	if(destination && source && json_object_object_get_ex(source, key, &value) && value)
		json_object_object_add(destination, key, json_object_get(value));
}

/* Sony's catalog rows contain descriptions, legal text, offers, media sets and
 * other Store metadata that is never used to draw or launch a cloud title.
 * Retaining full rows multiplied the live heap by every fetched source page.
 * Keep only fields consumed by cloud_game_row() and provisioning. */
static struct json_object *cloud_compact_source(struct json_object *source, bool owned)
{
	if(!source || json_object_get_type(source) != json_type_object)
		return NULL;
	struct json_object *copy = json_object_new_object();
	if(!copy)
		return NULL;
	static const char *const fields[] = {
		"productId", "id", "product_id", "name", "landscapeImageUrl",
		"conceptId", "conceptUrl", "entitlementId", "serviceType",
		"plusCatalog", "streamingSupported", "isOwned", "active_flag",
		"feature_type",
	};
	for(size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
		cloud_reference_member(copy, source, fields[i]);

	char cover[1024];
	cloud_cover(source, cover);
	if(*cover)
		cloud_json_put_string(copy, "imageUrl", cover);

	struct json_object *devices = cloud_json_array(source, "device");
	if(devices)
	{
		struct json_object *kept = json_object_new_array();
		if(!kept)
		{
			json_object_put(copy);
			return NULL;
		}
		for(size_t i = 0; i < json_object_array_length(devices) && i < 8; i++)
		{
			struct json_object *device = json_object_array_get_idx(devices, i);
			if(device && json_object_get_type(device) == json_type_string)
				json_object_array_add(kept, json_object_get(device));
		}
		json_object_object_add(copy, "device", kept);
	}

	if(owned)
	{
		struct json_object *attributes = cloud_json_array(source, "entitlement_attributes");
		if(attributes)
		{
			struct json_object *kept = json_object_new_array();
			if(!kept)
			{
				json_object_put(copy);
				return NULL;
			}
			for(size_t i = 0; i < json_object_array_length(attributes) && i < 16; i++)
			{
				struct json_object *attribute = json_object_array_get_idx(attributes, i);
				const char *platform = cloud_json_string(attribute, "platform_id");
				if(!*platform)
					continue;
				struct json_object *entry = json_object_new_object();
				if(!entry)
				{
					json_object_put(kept);
					json_object_put(copy);
					return NULL;
				}
				cloud_json_put_string(entry, "platform_id", platform);
				json_object_array_add(kept, entry);
			}
			json_object_object_add(copy, "entitlement_attributes", kept);
		}

		struct json_object *metadata = cloud_json_object(source, "game_meta");
		if(metadata)
		{
			struct json_object *kept = json_object_new_object();
			if(!kept)
			{
				json_object_put(copy);
				return NULL;
			}
			cloud_reference_member(kept, metadata, "name");
			cloud_cover(metadata, cover);
			if(*cover)
				cloud_json_put_string(kept, "imageUrl", cover);
			json_object_object_add(copy, "game_meta", kept);
		}
	}
	return copy;
}

static void cloud_append_unique(struct json_object *destination, struct json_object *seen,
	struct json_object *row, const char *id_key)
{
	if(!row || !cloud_is_product(row))
		return;
	const char *id = cloud_json_string(row, id_key);
	if(!*id)
		id = cloud_json_string(row, strcmp(id_key, "id") == 0 ? "productId" : "id");
	struct json_object *ignored = NULL;
	if(!*id || json_object_object_get_ex(seen, id, &ignored))
		return;
	json_object_object_add(seen, id, json_object_new_boolean(true));
	struct json_object *copy = cloud_compact_source(row, false);
	if(copy)
		json_object_array_add(destination, copy);
}

static bool cloud_fetch_container(ChiakiLog *log, const char *base_url, const char *cookie,
	struct json_object *games, struct json_object *seen, struct json_object *child_ids,
	int *request_budget)
{
	bool complete = true;
	for(int offset = 0; offset < 10000; offset += 100)
	{
		if(*request_budget <= 0)
			return false;
		(*request_budget)--;
		char url[1500];
		snprintf(url, sizeof(url), "%s%cuseOffers=true&gkb=1&gkb2=1&start=%d&size=100",
			base_url, strchr(base_url, '?') ? '&' : '?', offset);
		const char *headers_with_cookie[] = { "Accept: application/json", "User-Agent: " BROWSER_AGENT, cookie };
		PsnHttpRequest request = {
			.method = PSN_HTTP_GET, .url = url, .headers = headers_with_cookie,
			.headers_count = cookie ? 3 : 2,
		};
		PsnHttpReply reply;
		if(psn_http_execute(log, &request, &reply) != CHIAKI_ERR_SUCCESS || reply.status != 200)
		{
			psn_http_reply_clear(&reply);
			return false;
		}
		struct json_object *page = cloud_parse_reply(&reply);
		psn_http_reply_clear(&reply);
		if(!page)
			return false;
		struct json_object *links = cloud_json_array(page, "links");
		size_t count = links ? json_object_array_length(links) : 0;
		for(size_t i = 0; i < count; i++)
		{
			struct json_object *row = json_object_array_get_idx(links, i);
			const char *type = cloud_json_string(row, "container_type");
			if(cloud_string_equal_ci(type, "container") && child_ids)
			{
				const char *id = cloud_json_string(row, "id");
				if(*id)
					json_object_array_add(child_ids, json_object_new_string(id));
			}
			else
				cloud_append_unique(games, seen, row, "id");
		}
		int total = cloud_json_integer(page, "total_results");
		if(total <= 0)
			total = cloud_json_integer(page, "totalResults");
		json_object_put(page);
		if(count < 100 || (total > 0 && offset + (int)count >= total))
			break;
		if(count == 0)
		{
			complete = false;
			break;
		}
	}
	return complete;
}

static struct json_object *cloud_fetch_psnow(ChiakiLog *log, const CloudAccount *account,
	const char *locale_country, bool *complete)
{
	struct json_object *games = json_object_new_array();
	struct json_object *seen = json_object_new_object();
	struct json_object *children = json_object_new_array();
	char root[1300];
	char *cookie = NULL;
	if(account->region_supported)
	{
		cloud_copy_text(root, sizeof(root), account->store_root);
		cookie = psn_http_cookie("JSESSIONID", account->session_cookie);
	}
	else
	{
		static const char *const americas[] = {
			"US", "CA", "MX", "BR", "AR", "CL", "CO", "PE", "EC", "BO", "PY", "UY", NULL,
		};
		bool american = false;
		for(size_t i = 0; americas[i]; i++)
			if(strcasecmp(locale_country, americas[i]) == 0)
				american = true;
		const char *store = american ? "US" : "GB";
		const char *container = american ? "STORE-MSF192018-APOLLOROOT" : "STORE-MSF192014-APOLLOROOT";
		snprintf(root, sizeof(root),
			"https://psnow.playstation.com/store/api/pcnow/00_09_000/container/%s/en/19/%s",
			store, container);
	}
	int budget = 150;
	*complete = cloud_fetch_container(log, root, cookie, games, seen, children, &budget);
	char child_base[1300];
	const char *slash = strrchr(root, '/');
	if(slash)
	{
		size_t prefix = (size_t)(slash - root + 1);
		if(prefix < sizeof(child_base))
		{
			memcpy(child_base, root, prefix);
			child_base[prefix] = '\0';
			for(size_t i = 0; i < json_object_array_length(children) && i < 50; i++)
			{
				const char *id = json_object_get_string(json_object_array_get_idx(children, i));
				char url[1400];
				snprintf(url, sizeof(url), "%s%s", child_base, id ? id : "");
				if(!cloud_fetch_container(log, url, cookie, games, seen, NULL, &budget))
					*complete = false;
			}
		}
	}
	free(cookie);
	json_object_put(children);
	json_object_put(seen);
	return games;
}

static void cloud_locale_candidates(const char *locale, char output[3][16], size_t *count)
{
	char normalized[16];
	cloud_locale_normalize(locale, normalized);
	*count = 0;
	cloud_copy_text(output[(*count)++], 16, normalized);
	const char *dash = strchr(normalized, '-');
	char english[16];
	snprintf(english, sizeof(english), "en-%s", dash ? dash + 1 : "US");
	if(strcmp(english, normalized) != 0)
		cloud_copy_text(output[(*count)++], 16, english);
	bool already_us = false;
	for(size_t i = 0; i < *count; i++)
		already_us |= strcmp(output[i], "en-US") == 0;
	if(!already_us)
		cloud_copy_text(output[(*count)++], 16, "en-US");
}

static void cloud_map_imagic_document(struct json_object *document, const char *list_name,
	struct json_object *by_id)
{
	bool plus_list = strstr(list_name, "plus-") || strstr(list_name, "ubisoft-");
	if(!document || json_object_get_type(document) != json_type_array)
		return;
	for(size_t group_index = 0; group_index < json_object_array_length(document); group_index++)
	{
		struct json_object *group = json_object_array_get_idx(document, group_index);
		struct json_object *games = cloud_json_array(group, "games");
		for(size_t game_index = 0; games && game_index < json_object_array_length(games); game_index++)
		{
			struct json_object *game = json_object_array_get_idx(games, game_index);
			const char *product = cloud_json_string(game, "productId");
			if(!*product)
				continue;
			struct json_object *existing = NULL;
			if(json_object_object_get_ex(by_id, product, &existing))
			{
				if(strcmp(list_name, "all-ps5-list") == 0)
				{
					struct json_object *copy = cloud_compact_source(game, false);
					if(copy)
					{
						cloud_json_put_boolean(copy, "plusCatalog",
							cloud_json_boolean(existing, "plusCatalog"));
						json_object_object_add(by_id, product, copy);
					}
				}
				if(plus_list)
					cloud_json_put_boolean(existing, "plusCatalog", true);
				continue;
			}
			struct json_object *copy = cloud_compact_source(game, false);
			if(!copy)
				continue;
			cloud_json_put_boolean(copy, "plusCatalog", plus_list);
			json_object_object_add(by_id, product, copy);
		}
	}
}

static struct json_object *cloud_fetch_imagic(ChiakiLog *log, const char *requested_locale,
	char settled_locale[16], bool *complete)
{
	char candidates[3][16];
	size_t candidate_count = 0;
	cloud_locale_candidates(requested_locale, candidates, &candidate_count);
	struct json_object *by_id = json_object_new_object();
	int total_successes = 0;
	bool main_list_loaded = false;
	cloud_locale_normalize(requested_locale, settled_locale);
	for(size_t locale_index = 0; locale_index < candidate_count; locale_index++)
	{
		char web_locale[16];
		cloud_copy_text(web_locale, sizeof(web_locale), candidates[locale_index]);
		for(char *cursor = web_locale; *cursor; cursor++)
			*cursor = (char)tolower((unsigned char)*cursor);
		int successes = 0;
		bool locale_main_loaded = false;
		for(size_t list_index = 0; list_index < sizeof(imagic_lists) / sizeof(imagic_lists[0]); list_index++)
		{
			char url[512];
			snprintf(url, sizeof(url),
				"https://www.playstation.com/bin/imagic/gameslist?locale=%s&categoryList=%s",
				web_locale, imagic_lists[list_index]);
			const char *headers[] = { "Accept: application/json", "User-Agent: " BROWSER_AGENT };
			PsnHttpRequest request = {
				.method = PSN_HTTP_GET, .url = url, .headers = headers, .headers_count = 2,
			};
			PsnHttpReply reply;
			if(psn_http_execute(log, &request, &reply) != CHIAKI_ERR_SUCCESS || reply.status != 200)
			{
				psn_http_reply_clear(&reply);
				continue;
			}
			struct json_object *document = cloud_parse_reply(&reply);
			psn_http_reply_clear(&reply);
			if(!document || json_object_get_type(document) != json_type_array)
			{
				if(document) json_object_put(document);
				continue;
			}
			cloud_map_imagic_document(document, imagic_lists[list_index], by_id);
			json_object_put(document);
			successes++;
			locale_main_loaded |= strcmp(imagic_lists[list_index], "all-ps5-list") == 0;
		}
		CLOUD_METRIC(30 + locale_index, successes);
		CLOUD_METRIC(40 + locale_index, locale_main_loaded);
		if(successes && total_successes == 0)
			cloud_copy_text(settled_locale, 16, candidates[locale_index]);
		total_successes += successes;
		if(locale_main_loaded)
		{
			cloud_copy_text(settled_locale, 16, candidates[locale_index]);
			main_list_loaded = true;
			break;
		}
	}
	struct json_object *games = json_object_new_array();
	json_object_object_foreach(by_id, key, value)
	{
		(void)key;
		json_object_array_add(games, json_object_get(value));
	}
	json_object_put(by_id);
	*complete = main_list_loaded;
	return games;
}

static char *cloud_fetch_owned_token(ChiakiLog *log, const char *npsso)
{
	if(!npsso || !*npsso)
		return NULL;
	char *scope = psn_url_escape("kamaji:get_internal_entitlements user:account.attributes.validate");
	char *redirect = psn_url_escape(PSNOW_REDIRECT);
	if(!scope || !redirect)
	{
		free(scope); free(redirect);
		return NULL;
	}
	char url[1800];
	snprintf(url, sizeof(url),
		PSN_ACCOUNT_API "/v1/oauth/authorize?response_type=token&scope=%s&client_id=" COMMERCE_CLIENT_ID
		"&redirect_uri=%s&service_entity=urn%%3Aservice-entity%%3Apsn&prompt=none", scope, redirect);
	free(scope); free(redirect);
	char *cookie = psn_http_cookie("npsso", npsso);
	const char *headers[] = { cookie, "User-Agent: " BROWSER_AGENT };
	PsnHttpRequest request = {
		.method = PSN_HTTP_GET, .url = url, .headers = headers, .headers_count = 2,
		.keep_response_headers = true,
	};
	PsnHttpReply reply;
	ChiakiErrorCode error = psn_http_execute(log, &request, &reply);
	free(cookie);
	if(error != CHIAKI_ERR_SUCCESS)
		return NULL;
	char *location = psn_http_header(&reply, "Location");
	char *token = psn_url_parameter(location ? location : reply.redirect, "access_token");
	free(location);
	psn_http_reply_clear(&reply);
	return token;
}

static const char *cloud_owned_service(struct json_object *entitlement)
{
	struct json_object *attributes = cloud_json_array(entitlement, "entitlement_attributes");
	for(size_t i = 0; attributes && i < json_object_array_length(attributes); i++)
	{
		const char *platform = cloud_json_string(json_object_array_get_idx(attributes, i), "platform_id");
		if(cloud_string_equal_ci(platform, "ps5"))
			return "pscloud";
		if(cloud_string_equal_ci(platform, "ps4") || cloud_string_equal_ci(platform, "ps3"))
			return "psnow";
	}
	const char *product = cloud_json_string(entitlement, "product_id");
	return strstr(product, "PPSA") ? "pscloud" : "psnow";
}

static struct json_object *cloud_fetch_owned(ChiakiLog *log, const char *npsso, bool *complete)
{
	*complete = true;
	struct json_object *result = json_object_new_array();
	char *token = cloud_fetch_owned_token(log, npsso);
	if(!token)
	{
		*complete = !npsso || !*npsso;
		return result;
	}
	char *authorization = psn_http_bearer(token);
	free(token);
	for(int offset = 0, page = 0; page < 100; page++)
	{
		char url[700];
		snprintf(url, sizeof(url),
			"https://commerce.api.np.km.playstation.net/commerce/api/v1/users/me/internal_entitlements"
			"?fields=game_meta&entitlement_type=5&start=%d&size=300", offset);
		const char *headers[] = { authorization, "Accept: application/json" };
		PsnHttpRequest request = {
			.method = PSN_HTTP_GET, .url = url, .headers = headers, .headers_count = 2,
		};
		PsnHttpReply reply;
		if(psn_http_execute(log, &request, &reply) != CHIAKI_ERR_SUCCESS || reply.status != 200)
		{
			psn_http_reply_clear(&reply);
			*complete = false;
			break;
		}
		struct json_object *document = cloud_parse_reply(&reply);
		psn_http_reply_clear(&reply);
		struct json_object *page_rows = cloud_json_array(document, "entitlements");
		size_t count = page_rows ? json_object_array_length(page_rows) : 0;
		for(size_t i = 0; i < count; i++)
		{
			struct json_object *row = json_object_array_get_idx(page_rows, i);
			struct json_object *metadata = cloud_json_object(row, "game_meta");
			const char *product = cloud_json_string(row, "product_id");
			if(!metadata || !cloud_json_boolean(row, "active_flag") || !*product
				|| strncmp(product, "IP", 2) == 0 || strncmp(product, "SUB", 3) == 0
				|| cloud_json_integer(row, "feature_type") == 0)
				continue;
			struct json_object *copy = cloud_compact_source(row, true);
			if(copy)
			{
				cloud_json_put_string(copy, "serviceType", cloud_owned_service(row));
				json_object_array_add(result, copy);
			}
		}
		if(document) json_object_put(document);
		if(count < 300)
			break;
		offset += (int)count;
	}
	free(authorization);
	return result;
}

static const char *cloud_platform(struct json_object *game, const char *product)
{
	/* The product namespace identifies the actual build more accurately than
	 * Store device compatibility (a PS4 SKU can also list PS5 as a device). */
	if(product && strstr(product, "PPSA")) return "ps5";
	if(product && strstr(product, "CUSA")) return "ps4";
	struct json_object *devices = cloud_json_array(game, "device");
	for(size_t i = 0; devices && i < json_object_array_length(devices); i++)
	{
		const char *device = json_object_get_string(json_object_array_get_idx(devices, i));
		if(device && cloud_case_find(device, "PS5")) return "ps5";
	}
	for(size_t i = 0; devices && i < json_object_array_length(devices); i++)
	{
		const char *device = json_object_get_string(json_object_array_get_idx(devices, i));
		if(device && cloud_case_find(device, "PS4")) return "ps4";
		if(device && cloud_case_find(device, "PS3")) return "ps3";
	}
	return "ps3";
}

static void cloud_stable_product(const char *product, char output[128])
{
	output[0] = '\0';
	if(!product)
		return;
	const char *last_dash = strrchr(product, '-');
	const char *last_underscore = strrchr(product, '_');
	const char *last = last_dash > last_underscore ? last_dash : last_underscore;
	if(!last || last == product)
		return;
	size_t size = (size_t)(last - product);
	if(size >= 128) size = 127;
	memcpy(output, product, size);
	output[size] = '\0';
}

static struct json_object *cloud_owned_index(struct json_object *owned)
{
	struct json_object *index = json_object_new_object();
	for(size_t i = 0; owned && i < json_object_array_length(owned); i++)
	{
		struct json_object *row = json_object_array_get_idx(owned, i);
		const char *product = cloud_json_string(row, "product_id");
		if(*product)
			json_object_object_add(index, product, json_object_get(row));
		char stable[128];
		cloud_stable_product(product, stable);
		if(*stable)
			json_object_object_add(index, stable, json_object_get(row));
	}
	return index;
}

static struct json_object *cloud_find_owned(struct json_object *index, const char *product)
{
	struct json_object *owned = NULL;
	if(product && json_object_object_get_ex(index, product, &owned))
		return owned;
	char stable[128];
	cloud_stable_product(product, stable);
	return *stable && json_object_object_get_ex(index, stable, &owned) ? owned : NULL;
}

static void cloud_product_index_add(struct json_object *index, const char *product)
{
	if(!index || !product || !*product)
		return;
	json_object_object_add(index, product, json_object_new_boolean(true));
	char stable[128];
	cloud_stable_product(product, stable);
	if(*stable)
		json_object_object_add(index, stable, json_object_new_boolean(true));
}

static int cloud_game_preference(struct json_object *game)
{
	const char *product = cloud_json_string(game, "productId");
	const char *stream = cloud_json_string(game, "streamIdentifier");
	const char *entitlement = cloud_json_string(game, "entitlementId");
	int score = 0;
	const char *platform = cloud_json_string(game, "platform");
	if(strcmp(platform, "ps5") == 0) score += 1024;
	else if(strcmp(platform, "ps4") == 0) score += 512;
	else if(strcmp(platform, "ps3") == 0) score += 256;
	if(*product && strcmp(product, stream) == 0) score += 32;
	if(*product && strcmp(product, entitlement) == 0) score += 16;
	if(cloud_json_boolean(game, "isPlayable")) score += 8;
	if(cloud_json_boolean(game, "isOwned")) score += 4;
	if(cloud_json_boolean(game, "plusCatalog")) score += 2;
	if(*cloud_json_string(game, "imageUrl")) score += 1;
	return score;
}

static char *cloud_normalized_game_name(const char *name)
{
	if(!name || !*name)
		return NULL;
	char *normalized = strdup(name);
	if(!normalized)
		return NULL;
	for(char *cursor = normalized; *cursor; cursor++)
		*cursor = (char)tolower((unsigned char)*cursor);
	char *end = normalized + strlen(normalized);
	while(end > normalized && isspace((unsigned char)end[-1]))
		*--end = '\0';
	if(end > normalized && (end[-1] == ')' || end[-1] == ']'))
	{
		char opening = end[-1] == ')' ? '(' : '[';
		char *suffix = strrchr(normalized, opening);
		if(suffix && (cloud_case_find(suffix, "ps4") || cloud_case_find(suffix, "ps5")
			|| cloud_case_find(suffix, "playstation")))
		{
			*suffix = '\0';
			end = suffix;
			while(end > normalized && isspace((unsigned char)end[-1]))
				*--end = '\0';
		}
	}
	char *read = normalized;
	char *write = normalized;
	bool whitespace = false;
	while(*read)
	{
		/* Store display names frequently put trademark marks directly after
		 * PS4/PS5. They are presentation glyphs and must not keep otherwise
		 * identical cross-generation titles in separate UI cards. */
		if(strncmp(read, "\xE2\x84\xA2", 3) == 0)
		{
			read += 3;
			continue;
		}
		if(strncmp(read, "\xC2\xAE", 2) == 0)
		{
			read += 2;
			continue;
		}
		if(isspace((unsigned char)*read))
		{
			whitespace = write != normalized;
			read++;
			continue;
		}
		if(whitespace)
			*write++ = ' ';
		whitespace = false;
		*write++ = *read++;
	}
	*write = '\0';

	/* Cross-generation Store rows often append a platform marker while the
	 * PS Now row keeps the plain title.  Remove only an explicit trailing
	 * platform suffix; edition names remain part of the identity. */
	static const char *const platform_suffixes[] = {
		" ps4 & ps5", " ps5 & ps4", " ps4 and ps5", " ps5 and ps4",
		" ps4/ps5", " ps5/ps4", " ps4 + ps5", " ps5 + ps4", " ps4", " ps5",
	};
	for(size_t i = 0; i < sizeof(platform_suffixes) / sizeof(platform_suffixes[0]); i++)
	{
		size_t name_size = strlen(normalized);
		size_t suffix_size = strlen(platform_suffixes[i]);
		if(name_size >= suffix_size
			&& strcmp(normalized + name_size - suffix_size, platform_suffixes[i]) == 0)
		{
			normalized[name_size - suffix_size] = '\0';
			end = normalized + name_size - suffix_size;
			while(end > normalized && isspace((unsigned char)end[-1]))
				*--end = '\0';
			if(end > normalized && (end[-1] == '-' || end[-1] == ':' || end[-1] == '|'))
			{
				*--end = '\0';
				while(end > normalized && isspace((unsigned char)end[-1]))
					*--end = '\0';
			}
			break;
		}
	}
	return normalized;
}

static char *cloud_game_launch_key(struct json_object *game)
{
	const char *identifier = cloud_json_string(game, "streamIdentifier");
	const char *platform = cloud_json_string(game, "platform");
	if(!*identifier)
		identifier = cloud_json_string(game, "entitlementId");
	if(!*identifier)
		return NULL;
	size_t size = strlen(identifier) + strlen(platform) + 9;
	char *key = malloc(size);
	if(key)
		snprintf(key, size, "launch|%s|%s", platform, identifier);
	return key;
}

static char *cloud_game_concept_key(struct json_object *game)
{
	const char *concept = cloud_json_string(game, "conceptUrl");
	if(!*concept)
		concept = cloud_json_string(game, "conceptId");
	const char *platform = cloud_json_string(game, "platform");
	char *name = cloud_normalized_game_name(cloud_json_string(game, "name"));
	if(!*concept || !name || !*name)
	{
		free(name);
		return NULL;
	}
	size_t size = strlen(concept) + strlen(name) + strlen(platform) + 12;
	char *key = malloc(size);
	if(key)
		snprintf(key, size, "concept|%s|%s|%s", platform, concept, name);
	free(name);
	return key;
}

static char *cloud_game_title_key(struct json_object *game)
{
	char *name = cloud_normalized_game_name(cloud_json_string(game, "name"));
	if(!name || !*name)
	{
		free(name);
		return NULL;
	}
	size_t size = strlen(name) + 7;
	char *key = malloc(size);
	if(key)
		snprintf(key, size, "title|%s", name);
	free(name);
	return key;
}

typedef char *(*CloudGameIdentityKey)(struct json_object *game);

static struct json_object *cloud_deduplicate_games(struct json_object *games,
	CloudGameIdentityKey identity)
{
	struct json_object *deduplicated = json_object_new_array();
	struct json_object *indices = json_object_new_object();
	if(!deduplicated || !indices)
	{
		if(deduplicated) json_object_put(deduplicated);
		if(indices) json_object_put(indices);
		return NULL;
	}
	for(size_t i = 0; i < json_object_array_length(games); i++)
	{
		struct json_object *game = json_object_array_get_idx(games, i);
		char *identity_key = identity(game);
		const char *key = identity_key ? identity_key : cloud_json_string(game, "productId");
		struct json_object *index_value = NULL;
		if(json_object_object_get_ex(indices, key, &index_value))
		{
			size_t index = (size_t)json_object_get_int64(index_value);
			struct json_object *existing = json_object_array_get_idx(deduplicated, index);
			if(cloud_game_preference(game) > cloud_game_preference(existing))
				json_object_array_put_idx(deduplicated, index, json_object_get(game));
		}
		else
		{
			size_t index = json_object_array_length(deduplicated);
			json_object_array_add(deduplicated, json_object_get(game));
			json_object_object_add(indices, key, json_object_new_int64((int64_t)index));
		}
		free(identity_key);
	}
	json_object_put(indices);
	return deduplicated;
}

static struct json_object *cloud_group_game_versions(struct json_object *games)
{
	struct json_object *group_indices = json_object_new_object();
	struct json_object *groups = json_object_new_array();
	struct json_object *grouped = json_object_new_array();
	if(!group_indices || !groups || !grouped)
	{
		if(group_indices) json_object_put(group_indices);
		if(groups) json_object_put(groups);
		if(grouped) json_object_put(grouped);
		return NULL;
	}

	for(size_t i = 0; i < json_object_array_length(games); i++)
	{
		struct json_object *game = json_object_array_get_idx(games, i);
		char *title_key = cloud_game_title_key(game);
		const char *key = title_key ? title_key : cloud_json_string(game, "productId");
		struct json_object *index_value = NULL;
		struct json_object *versions = NULL;
		if(json_object_object_get_ex(group_indices, key, &index_value))
			versions = json_object_array_get_idx(groups, (size_t)json_object_get_int64(index_value));
		else
		{
			versions = json_object_new_array();
			size_t index = json_object_array_length(groups);
			json_object_array_add(groups, versions);
			json_object_object_add(group_indices, key, json_object_new_int64((int64_t)index));
		}
		json_object_array_add(versions, json_object_get(game));
		free(title_key);
	}
	json_object_put(group_indices);

	for(size_t group_index = 0; group_index < json_object_array_length(groups); group_index++)
	{
		struct json_object *versions = json_object_array_get_idx(groups, group_index);
		struct json_object *ps5 = NULL;
		struct json_object *ps4 = NULL;
		struct json_object *preferred = NULL;
		for(size_t i = 0; i < json_object_array_length(versions); i++)
		{
			struct json_object *game = json_object_array_get_idx(versions, i);
			const char *platform = cloud_json_string(game, "platform");
			if(!preferred || cloud_game_preference(game) > cloud_game_preference(preferred))
				preferred = game;
			if(strcmp(platform, "ps5") == 0
				&& (!ps5 || cloud_game_preference(game) > cloud_game_preference(ps5)))
				ps5 = game;
			else if(strcmp(platform, "ps4") == 0
				&& (!ps4 || cloud_game_preference(game) > cloud_game_preference(ps4)))
				ps4 = game;
		}

		if(ps5 && ps4)
		{
			struct json_object *card = cloud_json_copy(ps5);
			struct json_object *variants = json_object_new_array();
			if(!card || !variants)
			{
				if(card) json_object_put(card);
				if(variants) json_object_put(variants);
				json_object_put(groups);
				json_object_put(grouped);
				return NULL;
			}
			json_object_array_add(variants, json_object_get(ps5));
			json_object_array_add(variants, json_object_get(ps4));
			json_object_object_add(card, "variants", variants);
			cloud_json_put_string(card, "platform", "ps5/ps4");
			bool owned = cloud_json_boolean(ps5, "isOwned") || cloud_json_boolean(ps4, "isOwned");
			bool supported = cloud_json_boolean(ps5, "streamingSupported")
				|| cloud_json_boolean(ps4, "streamingSupported");
			bool playable = cloud_json_boolean(ps5, "isPlayable") || cloud_json_boolean(ps4, "isPlayable");
			bool plus = cloud_json_boolean(ps5, "plusCatalog") || cloud_json_boolean(ps4, "plusCatalog");
			cloud_json_put_boolean(card, "isOwned", owned);
			cloud_json_put_boolean(card, "streamingSupported", supported);
			cloud_json_put_boolean(card, "isPlayable", playable);
			cloud_json_put_boolean(card, "plusCatalog", plus);
			cloud_json_put_string(card, "category", owned ? "owned" : (playable ? "streamable" : "purchaseable"));
			json_object_array_add(grouped, card);
		}
		else if(preferred)
			json_object_array_add(grouped, json_object_get(preferred));
	}

	json_object_put(groups);
	return grouped;
}

/* A presentation pass must never turn a successfully downloaded catalog into
 * an empty result.  Keep the previous object when a deduplication/grouping
 * pass unexpectedly produces no rows, and report the candidate size so the
 * failing pass remains visible in the PS5 log. */
static struct json_object *cloud_accept_nonempty_catalog_pass(
	struct json_object *current, struct json_object *candidate, int metric)
{
	int count = candidate ? (int)json_object_array_length(candidate) : -1;
	CLOUD_METRIC(metric, count);
	if(candidate && count > 0)
	{
		json_object_put(current);
		return candidate;
	}
	if(candidate)
		json_object_put(candidate);
	return current;
}

static void cloud_indexed_game_add(struct json_object *indices, struct json_object *games,
	struct json_object *row, bool replace_existing)
{
	if(!row)
		return;
	const char *product = cloud_json_string(row, "productId");
	struct json_object *index_value = NULL;
	if(*product && json_object_object_get_ex(indices, product, &index_value))
	{
		size_t index = (size_t)json_object_get_int64(index_value);
		struct json_object *existing = json_object_array_get_idx(games, index);
		if(replace_existing || cloud_game_preference(row) > cloud_game_preference(existing))
			json_object_array_put_idx(games, index, row);
		else
			json_object_put(row);
		return;
	}
	if(!*product || json_object_array_add(games, row) != 0)
	{
		json_object_put(row);
		return;
	}
	json_object_object_add(indices, product,
		json_object_new_int64((int64_t)(json_object_array_length(games) - 1)));
}

static struct json_object *cloud_game_row(struct json_object *source, const char *forced_service,
	struct json_object *owned_index)
{
	const char *product = cloud_json_string(source, "productId");
	if(!*product) product = cloud_json_string(source, "id");
	if(!*product) product = cloud_json_string(source, "product_id");
	if(!*product)
		return NULL;
	const char *name = cloud_json_string(source, "name");
	struct json_object *metadata = cloud_json_object(source, "game_meta");
	if(!*name && metadata) name = cloud_json_string(metadata, "name");
	const char *platform = cloud_platform(source, product);
	const char *service = forced_service ? forced_service : (strcmp(platform, "ps5") == 0 ? "pscloud" : "psnow");
	struct json_object *owned = cloud_find_owned(owned_index, product);
	bool is_owned = owned != NULL || cloud_json_boolean(source, "isOwned");
	bool plus = cloud_json_boolean(source, "plusCatalog");
	/* Ownership and Cloud Play support are separate.  Store metadata is the
	 * authority for PS5 streaming support, while every title returned by the
	 * dedicated PS Now catalog already has a cloud route. */
	bool psnow_catalog = forced_service && strcmp(forced_service, "psnow") == 0;
	bool streaming_supported = psnow_catalog || cloud_json_boolean(source, "streamingSupported");
	bool is_playable = streaming_supported && (is_owned || plus || psnow_catalog);
	const char *category = is_owned ? "owned" : (is_playable ? "streamable" : "purchaseable");
	const char *entitlement = owned ? cloud_json_string(owned, "id") : cloud_json_string(source, "entitlementId");
	const char *stream_id = strcmp(service, "pscloud") == 0 && *entitlement ? entitlement : product;

	char image[1024];
	cloud_cover(source, image);
	if(!*image && metadata)
		cloud_cover(metadata, image);
	struct json_object *row = json_object_new_object();
	cloud_json_put_string(row, "productId", product);
	cloud_json_put_string(row, "name", name);
	cloud_json_put_string(row, "imageUrl", image);
	cloud_json_put_string(row, "landscapeImageUrl", cloud_json_string(source, "landscapeImageUrl"));
	char concept_id[32] = { 0 };
	const char *source_concept_id = cloud_json_string(source, "conceptId");
	if(*source_concept_id)
		cloud_copy_text(concept_id, sizeof(concept_id), source_concept_id);
	else
	{
		int numeric_concept_id = cloud_json_integer(source, "conceptId");
		if(numeric_concept_id > 0)
			snprintf(concept_id, sizeof(concept_id), "%d", numeric_concept_id);
	}
	cloud_json_put_string(row, "conceptId", concept_id);
	cloud_json_put_string(row, "category", category);
	cloud_json_put_string(row, "serviceType", service);
	cloud_json_put_string(row, "platform", platform);
	cloud_json_put_boolean(row, "isOwned", is_owned);
	cloud_json_put_boolean(row, "streamingSupported", streaming_supported);
	cloud_json_put_boolean(row, "isPlayable", is_playable);
	cloud_json_put_string(row, "streamServiceType", service);
	cloud_json_put_string(row, "streamIdentifier", stream_id);
	cloud_json_put_string(row, "entitlementId", entitlement);
	cloud_json_put_string(row, "storeProductId", owned ? cloud_json_string(owned, "product_id") : product);
	cloud_json_put_string(row, "conceptUrl", cloud_json_string(source, "conceptUrl"));
	cloud_json_put_boolean(row, "plusCatalog", plus);
	return row;
}

static int cloud_game_compare(const void *left_ptr, const void *right_ptr)
{
	struct json_object *left = *(struct json_object *const *)left_ptr;
	struct json_object *right = *(struct json_object *const *)right_ptr;
	bool left_owned = cloud_json_boolean(left, "isOwned");
	bool right_owned = cloud_json_boolean(right, "isOwned");
	if(left_owned != right_owned)
		return left_owned ? -1 : 1;
	return strcasecmp(cloud_json_string(left, "name"), cloud_json_string(right, "name"));
}

static struct json_object *cloud_assemble(const CloudSources *sources, const CloudAccount *account,
	const char *fallback_country, const char *settled_locale, const char *warning)
{
	struct json_object *owned_index = cloud_owned_index(sources->owned);
	struct json_object *product_indices = json_object_new_object();
	struct json_object *catalog_products = json_object_new_object();
	struct json_object *games = json_object_new_array();
	if(!owned_index || !product_indices || !catalog_products || !games)
	{
		if(owned_index) json_object_put(owned_index);
		if(product_indices) json_object_put(product_indices);
		if(catalog_products) json_object_put(catalog_products);
		if(games) json_object_put(games);
		return NULL;
	}
	for(size_t i = 0; i < json_object_array_length(sources->psnow); i++)
	{
		struct json_object *row = cloud_game_row(json_object_array_get_idx(sources->psnow, i), "psnow", owned_index);
		if(row)
		{
			cloud_product_index_add(catalog_products, cloud_json_string(row, "productId"));
			cloud_indexed_game_add(product_indices, games, row, false);
		}
	}
	CLOUD_METRIC(17, json_object_array_length(games));
	for(size_t i = 0; i < json_object_array_length(sources->browse); i++)
	{
		struct json_object *source = json_object_array_get_idx(sources->browse, i);
		struct json_object *row = cloud_game_row(source, NULL, owned_index);
		if(!row)
			continue;
		const char *product = cloud_json_string(row, "productId");
		cloud_product_index_add(catalog_products, product);
		cloud_indexed_game_add(product_indices, games, row,
			strcmp(cloud_json_string(row, "platform"), "ps5") == 0);
	}
	CLOUD_METRIC(18, json_object_array_length(games));

	/* Keep owned PS5 titles absent from the public lists visible, but do not
	 * assume that ownership alone makes them Cloud Play compatible. */
	for(size_t i = 0; i < json_object_array_length(sources->owned); i++)
	{
		struct json_object *owned = json_object_array_get_idx(sources->owned, i);
		if(strcmp(cloud_owned_service(owned), "pscloud") != 0)
			continue;
		const char *product = cloud_json_string(owned, "product_id");
		if(*product && !cloud_find_owned(catalog_products, product))
		{
			struct json_object *row = cloud_game_row(owned, "pscloud", owned_index);
			if(row)
			{
				cloud_indexed_game_add(product_indices, games, row, false);
				cloud_product_index_add(catalog_products, product);
			}
		}
	}
	json_object_put(product_indices);
	json_object_put(catalog_products);
	CLOUD_METRIC(19, json_object_array_length(games));
	/* Collapse identical rows within each platform, then preserve a PS5+PS4
	 * pair as one UI card with two complete launch variants. */
	struct json_object *deduplicated = cloud_deduplicate_games(games, cloud_game_launch_key);
	games = cloud_accept_nonempty_catalog_pass(games, deduplicated, 20);
	deduplicated = cloud_deduplicate_games(games, cloud_game_concept_key);
	games = cloud_accept_nonempty_catalog_pass(games, deduplicated, 21);
	deduplicated = cloud_group_game_versions(games);
	games = cloud_accept_nonempty_catalog_pass(games, deduplicated, 22);
	json_object_array_sort(games, cloud_game_compare);
	json_object_put(owned_index);

	struct json_object *envelope = json_object_new_object();
	json_object_object_add(envelope, "total", json_object_new_int((int)json_object_array_length(games)));
	json_object_object_add(envelope, "nativeMode", json_object_new_boolean(account->region_supported));
	cloud_json_put_string(envelope, "fallbackRegion",
		*account->store_country ? account->store_country : fallback_country);
	cloud_json_put_string(envelope, "resolvedStoreLang", account->store_language);
	cloud_json_put_string(envelope, "settledLocale", settled_locale);
	cloud_json_put_string(envelope, "warning", warning);
	json_object_object_add(envelope, "games", games);
	return envelope;
}

static ChiakiErrorCode cloud_result_from_json(ChiakiCloudCatalogResult *result, struct json_object *json)
{
	const char *serialized = json_object_to_json_string_ext(json, JSON_C_TO_STRING_PLAIN);
	result->json = serialized ? strdup(serialized) : NULL;
	result->err = result->json ? CHIAKI_ERR_SUCCESS : CHIAKI_ERR_MEMORY;
	return result->err;
}

ChiakiErrorCode chiaki_cloudcatalog_fetch_unified(const ChiakiCloudCatalogConfig *config,
		ChiakiCloudCatalogResult *result, ChiakiLog *log)
{
	CLOUD_TRACE(20);
	if(!config || !result || !config->cache_dir || !*config->cache_dir)
		return CHIAKI_ERR_INVALID_DATA;
	memset(result, 0, sizeof(*result));
	if(!config->force_refresh)
	{
		struct json_object *cached = cloud_cache_load(config->cache_dir, CATALOG_CACHE_FILE, false);
		if(cached)
		{
			ChiakiErrorCode error = cloud_result_from_json(result, cached);
			json_object_put(cached);
			return error;
		}
	}

	const char *locale = config->locale && *config->locale ? config->locale : "en-US";
	CloudAccount account;
	cloud_account_discover(log, config->npsso, &account);
	CLOUD_TRACE(21);
	CLOUD_METRIC(1, account.authenticated);
	CLOUD_METRIC(2, account.region_supported);
	CLOUD_METRIC(3, account.transport_failed);
	char country[8];
	cloud_locale_country(locale, country);
	if(*account.country)
		cloud_copy_text(country, sizeof(country), account.country);
	char effective_locale[16];
	if(*account.country && *account.language)
	{
		char language[8];
		chiaki_cloud_gaikai_language(account.language, language, sizeof(language));
		snprintf(effective_locale, sizeof(effective_locale), "%s-%s", language, country);
	}
	else
		cloud_locale_normalize(locale, effective_locale);

	CloudSources sources = { 0 };
	sources.psnow = cloud_fetch_psnow(log, &account, country, &sources.psnow_complete);
	CLOUD_TRACE(22);
	CLOUD_METRIC(10, sources.psnow ? json_object_array_length(sources.psnow) : -1);
	CLOUD_METRIC(11, sources.psnow_complete);
	char settled[16] = { 0 };
	sources.browse = cloud_fetch_imagic(log, effective_locale, settled, &sources.browse_complete);
	CLOUD_TRACE(23);
	CLOUD_METRIC(12, sources.browse ? json_object_array_length(sources.browse) : -1);
	CLOUD_METRIC(13, sources.browse_complete);
	sources.owned = cloud_fetch_owned(log, config->npsso, &sources.owned_complete);
	CLOUD_TRACE(24);
	CLOUD_METRIC(14, sources.owned ? json_object_array_length(sources.owned) : -1);
	CLOUD_METRIC(15, sources.owned_complete);
	if(!sources.psnow || !sources.browse || !sources.owned)
	{
		result->err = CHIAKI_ERR_MEMORY;
		result->error_message = strdup("The PlayStation catalog could not be prepared.");
		goto cleanup;
	}
	if(json_object_array_length(sources.psnow) == 0
		&& json_object_array_length(sources.browse) == 0
		&& json_object_array_length(sources.owned) == 0)
	{
		result->err = CHIAKI_ERR_NETWORK;
		result->error_message = strdup("The PlayStation catalog could not be loaded. Check debug.log for network errors.");
		CLOUD_TRACE(27);
		goto cleanup;
	}
	const char *warning = config->npsso && *config->npsso && !account.authenticated && !account.transport_failed
		? "Your PSN sign-in key has expired. Update it in Settings to load owned games."
		: "";
	CLOUD_TRACE(25);
	struct json_object *envelope = cloud_assemble(&sources, &account, country, settled, warning);
	CLOUD_TRACE(26);
	if(!envelope)
	{
		result->err = CHIAKI_ERR_MEMORY;
		result->error_message = strdup("The PlayStation catalog could not be assembled.");
		goto cleanup;
	}
	int total = cloud_json_integer(envelope, "total");
	CLOUD_METRIC(16, total);
	if(total == 0)
	{
		result->err = CHIAKI_ERR_NETWORK;
		result->error_message = strdup("The PlayStation catalog could not be loaded.");
	}
	else
	{
		cloud_result_from_json(result, envelope);
		if(result->err == CHIAKI_ERR_SUCCESS && sources.psnow_complete && sources.browse_complete
			&& sources.owned_complete && !*warning)
			cloud_cache_save(config->cache_dir, CATALOG_CACHE_FILE, envelope);
	}
	json_object_put(envelope);
cleanup:
	json_object_put(sources.psnow);
	json_object_put(sources.browse);
	json_object_put(sources.owned);
	return result->err;
}

void chiaki_cloudcatalog_result_fini(ChiakiCloudCatalogResult *result)
{
	if(!result)
		return;
	free(result->json);
	free(result->error_message);
	memset(result, 0, sizeof(*result));
}
