// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifdef _WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

#include <chiaki/cloudcatalog.h>
#include <chiaki/cloudsession.h>
#include <chiaki/remote/holepunch.h>
#include <chiaki/thread.h>

#include "cloud_json.h"
#include "cloud_probe.h"
#include "psn_transport.h"

#include <json-c/json_tokener.h>
#include <json-c/linkhash.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define cloud_pause_ms(milliseconds) Sleep(milliseconds)
#else
#include <unistd.h>
#define cloud_pause_ms(milliseconds) usleep((milliseconds) * 1000)
#endif

#define CLOUD_ACCOUNT_BASE "https://ca.account.sony.com"
#define CLOUD_GAIKAI_BASE "https://cc.prod.gaikai.com/v1"
#define CLOUD_CONFIG_BASE "https://config.cc.prod.gaikai.com/v1"
#define CLOUD_KAMAJI_BASE "https://psnow.playstation.com/kamaji/api/pcnow/00_09_000"
#define CLOUD_STORE_GRAPHQL "https://web.np.playstation.com/api/graphql/v1/op"
#define CLOUD_STORE_PRICING_HASH "abcb311ea830e679fe2b697a27f755764535d825b24510ab1239a4ca3092bd09"
#define CLOUD_PSNOW_CLIENT "bc6b0777-abb5-40da-92ca-e133cf18e989"
#define CLOUD_COMMERCE_CLIENT "dc523cc2-b51b-4190-bff0-3397c06871b3"
#define CLOUD_PSNOW_REDIRECT "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/grc-response.html"
#define CLOUD_PSNOW_AGENT "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) playstation-now/0.0.0 Chrome/83.0.4103.104 Electron/9.0.4 Safari/537.36 gkApollo"
#define CLOUD_PS5_AGENT "PlayStation Portal/6.0.0-rel.444+6a9cea6f5"

typedef struct cloud_license_t
{
	char entitlement[128];
	char sku[160];
	char platform[8];
	char session_cookie[512];
	char *commerce_token;
} CloudLicense;

typedef struct cloud_allocation_t
{
	ChiakiLog *log;
	const ChiakiCloudProvisionConfig *config;
	ChiakiCloudProvisionResult *result;
	bool ps5;
	const char *platform;
	const char *virtualization;
	const char *agent;
	const char *oauth_api;
	const char *redirect;
	char duid[CHIAKI_DUID_STR_SIZE];
	char client_id[128];
	char ps3_client_id[128];
	char stream_client_id[128];
	char *session_key;
	char *locked_key;
	char *session_id;
	char *primary_code;
	char *console_code;
	char *stream_code;
	struct json_object *specification;
	struct json_object *pings;
	struct json_object *selected_ping;
	char selected_name[128];
	int selected_port;
} CloudAllocation;

static void cloud_copy_text(char *destination, size_t capacity, const char *source)
{
	if(destination && capacity)
		snprintf(destination, capacity, "%s", source ? source : "");
}

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

static const char *cloud_nonnull(const char *value)
{
	return value ? value : "";
}

static char *cloud_format_header(const char *name, const char *value)
{
	if(!name || !value)
		return NULL;
	size_t size = strlen(name) + strlen(value) + 3;
	char *header = malloc(size);
	if(header)
		snprintf(header, size, "%s: %s", name, value);
	return header;
}

static void cloud_set_error(ChiakiCloudProvisionResult *result, const char *message)
{
	free(result->error_message);
	result->error_message = message ? strdup(message) : NULL;
}

static void cloud_set_stage_error(ChiakiLog *log, ChiakiCloudProvisionResult *result,
	ChiakiErrorCode error, const char *stage)
{
	if(error == CHIAKI_ERR_SUCCESS || error == CHIAKI_ERR_CANCELED)
		return;
	CHIAKI_LOGE(log, "Cloud Play failed while %s: %s", stage, chiaki_error_string(error));
	if(result->error_message)
		return;
	char message[256];
	snprintf(message, sizeof(message), "Cloud Play failed while %s.", stage);
	cloud_set_error(result, message);
}

static bool cloud_cancelled(const ChiakiCloudProvisionConfig *config)
{
	return config->is_cancelled && config->is_cancelled(config->user);
}

static void cloud_progress(const ChiakiCloudProvisionConfig *config, const char *message)
{
	if(config->progress)
		config->progress(message, config->user);
}

static bool cloud_wait(const ChiakiCloudProvisionConfig *config, unsigned seconds)
{
	for(unsigned tick = 0; tick < seconds * 10; tick++)
	{
		if(cloud_cancelled(config))
			return false;
		cloud_pause_ms(100);
	}
	return true;
}

static struct json_object *cloud_reply_json(const PsnHttpReply *reply)
{
	return reply && reply->body ? json_tokener_parse(reply->body) : NULL;
}

static bool cloud_kamaji_ok(struct json_object *document)
{
	struct json_object *header = cloud_json_object(document, "header");
	return header && strcmp(cloud_json_string(header, "status_code"), "0x0000") == 0;
}

static char *cloud_cookie_from_headers(const char *headers, const char *cookie_name)
{
	if(!headers || !cookie_name)
		return NULL;
	size_t name_size = strlen(cookie_name);
	for(const char *cursor = headers; (cursor = cloud_case_find(cursor, cookie_name)); cursor++)
	{
		if(cursor[name_size] != '=')
			continue;
		const char *value = cursor + name_size + 1;
		size_t length = strcspn(value, ";\r\n");
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

static ChiakiErrorCode cloud_oauth_redirect(ChiakiLog *log, const char *url,
	const char *npsso, const char *agent, const char *parameter, char **value)
{
	*value = NULL;
	char *cookie = psn_http_cookie("npsso", npsso);
	char *agent_header = cloud_format_header("User-Agent", agent);
	if(!cookie || !agent_header)
	{
		free(cookie); free(agent_header);
		return CHIAKI_ERR_MEMORY;
	}
	const char *headers[] = { cookie, agent_header };
	PsnHttpRequest request = {
		.method = PSN_HTTP_GET,
		.url = url,
		.headers = headers,
		.headers_count = 2,
		.keep_response_headers = true,
	};
	PsnHttpReply reply;
	ChiakiErrorCode error = psn_http_execute(log, &request, &reply);
	free(cookie); free(agent_header);
	if(error != CHIAKI_ERR_SUCCESS)
		return error;
	long status = reply.status;
	char *location = psn_http_header(&reply, "Location");
	const char *redirect = location ? location : reply.redirect;
	bool had_redirect = redirect && *redirect;
	*value = psn_url_parameter(redirect, parameter);
	char *oauth_error = psn_url_parameter(redirect, "error");
	char *oauth_error_code = psn_url_parameter(redirect, "error_code");
	char *content_type = psn_http_header(&reply, "Content-Type");
	free(location);
	psn_http_reply_clear(&reply);
	if(!*value)
		CHIAKI_LOGE(log,
			"PSN OAuth response did not contain %s (HTTP %ld, redirect %s, error %.80s, code %.80s, content-type %.80s)",
			parameter, status, had_redirect ? "yes" : "no",
			oauth_error ? oauth_error : "none", oauth_error_code ? oauth_error_code : "none",
			content_type ? content_type : "none");
	free(oauth_error);
	free(oauth_error_code);
	free(content_type);
	return *value ? CHIAKI_ERR_SUCCESS : CHIAKI_ERR_UNKNOWN;
}

static ChiakiErrorCode cloud_authorization_check(ChiakiLog *log,
	const ChiakiCloudProvisionConfig *config, const char *duid)
{
	bool ps5 = strcmp(config->service_type, "pscloud") == 0;
	const char *client = ps5 ? "19ae39c4-3f88-4d11-a792-94e4f52c996d" : CLOUD_PSNOW_CLIENT;
	const char *scope = ps5
		? "id_token:psn.basic_claims kamaji:s2s.subscriptionsPremium.get id_token:duid id_token:online_id openid psn:s2s"
		: "kamaji:commerce_native kamaji:commerce_container kamaji:lists kamaji:s2s.subscriptionsPremium.get";
	const char *redirect = ps5 ? "gaikai://local" : CLOUD_PSNOW_REDIRECT;
	const char *agent = ps5 ? CLOUD_PS5_AGENT : CLOUD_PSNOW_AGENT;
	struct json_object *body = json_object_new_object();
	cloud_json_put_string(body, "client_id", client);
	cloud_json_put_string(body, "scope", scope);
	cloud_json_put_string(body, "redirect_uri", redirect);
	cloud_json_put_string(body, "response_type", "code");
	cloud_json_put_string(body, "service_entity", "urn:service-entity:psn");
	cloud_json_put_string(body, "duid", duid);
	const char *serialized = json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN);
	char *cookie = psn_http_cookie("npsso", config->npsso);
	char *agent_header = cloud_format_header("User-Agent", agent);
	const char *headers[] = { "Content-Type: application/json; charset=UTF-8", cookie, agent_header };
	PsnHttpRequest request = {
		.method = PSN_HTTP_POST,
		.url = CLOUD_ACCOUNT_BASE "/api/authz/v3/oauth/authorizeCheck",
		.headers = headers,
		.headers_count = 3,
		.payload = serialized,
	};
	PsnHttpReply reply;
	ChiakiErrorCode error = psn_http_execute(log, &request, &reply);
	free(cookie); free(agent_header);
	json_object_put(body);
	if(error != CHIAKI_ERR_SUCCESS)
		return error;
	bool accepted = reply.status >= 200 && reply.status < 300;
	psn_http_reply_clear(&reply);
	return accepted ? CHIAKI_ERR_SUCCESS : CHIAKI_ERR_UNKNOWN;
}

static void cloud_title_segment(const char *product, char output[64])
{
	output[0] = '\0';
	const char *start = product ? strchr(product, '-') : NULL;
	if(!start)
		return;
	start++;
	size_t length = strcspn(start, "_");
	if(length >= 64) length = 63;
	memcpy(output, start, length);
	output[length] = '\0';
}

static bool cloud_legacy_product(const char *product)
{
	char title[64];
	cloud_title_segment(product, title);
	return strncmp(title, "CUSA", 4) != 0 && strncmp(title, "PPSA", 4) != 0;
}

static bool cloud_americas(const char *country)
{
	static const char *const list[] = {
		"US", "CA", "MX", "BR", "AR", "CL", "CO", "PE", "EC", "BO", "PY", "UY", NULL,
	};
	for(size_t i = 0; list[i]; i++)
		if(strcasecmp(cloud_nonnull(country), list[i]) == 0)
			return true;
	return false;
}

static void cloud_store_location(const ChiakiCloudProvisionConfig *config, char country[8], char language[8])
{
	cloud_copy_text(country, 8, *cloud_nonnull(config->store_country) ? config->store_country : "US");
	cloud_copy_text(language, 8, *cloud_nonnull(config->store_lang) ? config->store_lang : "en");
	if(config->catalog_is_foreign && cloud_legacy_product(config->game_identifier))
	{
		cloud_copy_text(country, 8, cloud_americas(country) ? "US" : "GB");
		cloud_copy_text(language, 8, "en");
	}
}

static bool cloud_pick_entitlement_from_sku(struct json_object *sku, const char *title,
	int strategy, bool title_required, CloudLicense *license)
{
	struct json_object *entitlements = cloud_json_array(sku, "entitlements");
	for(size_t i = 0; entitlements && i < json_object_array_length(entitlements); i++)
	{
		struct json_object *entry = json_object_array_get_idx(entitlements, i);
		const char *id = cloud_json_string(entry, "id");
		const char *package = cloud_json_string(entry, "packageType");
		bool match = strategy == 0 ? cloud_json_integer(entry, "license_type") == 4
			: strategy == 1 ? cloud_string_ends(package, "GS")
			: cloud_string_ends(package, "GD");
		if(!match || !*id || (title_required && *title && !strstr(id, title)))
			continue;
		cloud_copy_text(license->entitlement, sizeof(license->entitlement), id);
		cloud_copy_text(license->sku, sizeof(license->sku), cloud_json_string(sku, "id"));
		return true;
	}
	return false;
}

static bool cloud_pick_entitlement(struct json_object *product, const char *title, CloudLicense *license)
{
	struct json_object *default_sku = cloud_json_object(product, "default_sku");
	struct json_object *skus = cloud_json_array(product, "skus");
	for(int strategy = 0; strategy < 3; strategy++)
	{
		int passes = strategy == 0 ? 1 : 2;
		for(int pass = 0; pass < passes; pass++)
		{
			bool require_title = pass == 0 && strategy != 0;
			if(default_sku && cloud_pick_entitlement_from_sku(default_sku, title, strategy, require_title, license))
				return true;
			for(size_t i = 0; skus && i < json_object_array_length(skus); i++)
				if(cloud_pick_entitlement_from_sku(json_object_array_get_idx(skus, i), title,
					strategy, require_title, license))
					return true;
		}
	}
	return false;
}

static void cloud_product_platform(struct json_object *product, CloudLicense *license)
{
	struct json_object *platforms = cloud_json_array(product, "playable_platform");
	if(!platforms)
	{
		struct json_object *metadata = cloud_json_object(product, "metadata");
		struct json_object *playable = cloud_json_object(metadata, "playable_platform");
		platforms = cloud_json_array(playable, "values");
	}
	bool ps3 = false, ps4 = false, ps5 = false;
	for(size_t i = 0; platforms && i < json_object_array_length(platforms); i++)
	{
		const char *value = json_object_get_string(json_object_array_get_idx(platforms, i));
		if(value && cloud_case_find(value, "PS5")) ps5 = true;
		else if(value && cloud_case_find(value, "PS4")) ps4 = true;
		else if(value && cloud_case_find(value, "PS3")) ps3 = true;
	}
	cloud_copy_text(license->platform, sizeof(license->platform), ps5 ? "ps5" : ps4 ? "ps4" : ps3 ? "ps3" : "ps4");
}

static const char *cloud_action_parameter(struct json_object *action, const char *name)
{
	struct json_object *parameters = cloud_json_array(action, "param");
	for(size_t i = 0; parameters && i < json_object_array_length(parameters); i++)
	{
		struct json_object *parameter = json_object_array_get_idx(parameters, i);
		if(strcmp(cloud_json_string(parameter, "name"), name) != 0)
			continue;
		struct json_object *values = cloud_json_array(parameter, "values");
		if(values && json_object_array_length(values))
		{
			const char *value = json_object_get_string(json_object_array_get_idx(values, 0));
			return value ? value : "";
		}
	}
	return "";
}

static bool cloud_pick_psplus_sku(struct json_object *product,
	const char *product_id, CloudLicense *license)
{
	struct json_object *ctas = cloud_json_array(product, "mobilectas");
	for(size_t i = 0; ctas && i < json_object_array_length(ctas); i++)
	{
		struct json_object *cta = json_object_array_get_idx(ctas, i);
		struct json_object *action = cloud_json_object(cta, "action");
		struct json_object *price = cloud_json_object(cta, "price");
		if(!action || !price || strcmp(cloud_json_string(action, "type"), "ADD_TO_CART") != 0
			|| !cloud_json_boolean(price, "isFree")
			|| !cloud_json_boolean(price, "isTiedToSubscription"))
			continue;
		const char *sku = cloud_action_parameter(action, "skuId");
		size_t product_size = strlen(product_id);
		if(!*sku || strncmp(sku, product_id, product_size) != 0 || sku[product_size] != '-')
			continue;
		cloud_copy_text(license->sku, sizeof(license->sku), sku);
		cloud_copy_text(license->entitlement, sizeof(license->entitlement), product_id);
		cloud_copy_text(license->platform, sizeof(license->platform), "ps5");
		return true;
	}
	return false;
}

static ChiakiErrorCode cloud_resolve_psplus_offer(ChiakiLog *log,
	const ChiakiCloudProvisionConfig *config, CloudLicense *license)
{
	if(!*cloud_nonnull(config->concept_id))
		return CHIAKI_ERR_INVALID_DATA;
	cloud_progress(config, "Resolving PlayStation Plus license...");
	struct json_object *variables = json_object_new_object();
	struct json_object *extensions = json_object_new_object();
	struct json_object *persisted = json_object_new_object();
	if(!variables || !extensions || !persisted)
	{
		if(variables) json_object_put(variables);
		if(extensions) json_object_put(extensions);
		if(persisted) json_object_put(persisted);
		return CHIAKI_ERR_MEMORY;
	}
	cloud_json_put_string(variables, "conceptId", config->concept_id);
	json_object_object_add(persisted, "version", json_object_new_int(1));
	cloud_json_put_string(persisted, "sha256Hash", CLOUD_STORE_PRICING_HASH);
	json_object_object_add(extensions, "persistedQuery", persisted);
	char *encoded_variables = psn_url_escape(json_object_to_json_string_ext(variables, JSON_C_TO_STRING_PLAIN));
	char *encoded_extensions = psn_url_escape(json_object_to_json_string_ext(extensions, JSON_C_TO_STRING_PLAIN));
	json_object_put(variables);
	json_object_put(extensions);
	if(!encoded_variables || !encoded_extensions)
	{
		free(encoded_variables); free(encoded_extensions);
		return CHIAKI_ERR_MEMORY;
	}
	char url[2200];
	snprintf(url, sizeof(url), CLOUD_STORE_GRAPHQL
		"?operationName=metGetPricingDataByConceptId&variables=%s&extensions=%s",
		encoded_variables, encoded_extensions);
	free(encoded_variables); free(encoded_extensions);
	char locale[24];
	snprintf(locale, sizeof(locale), "%s-%s",
		*cloud_nonnull(config->store_lang) ? config->store_lang : "en",
		*cloud_nonnull(config->store_country) ? config->store_country : "US");
	char *locale_header = cloud_format_header("x-psn-store-locale-override", locale);
	if(!locale_header)
		return CHIAKI_ERR_MEMORY;
	const char *headers[] = { locale_header, "Accept: application/json", "Content-Type: application/json" };
	PsnHttpRequest request = {
		.method = PSN_HTTP_GET, .url = url, .headers = headers, .headers_count = 3,
	};
	PsnHttpReply reply;
	ChiakiErrorCode error = psn_http_execute(log, &request, &reply);
	free(locale_header);
	if(error != CHIAKI_ERR_SUCCESS)
		return error;
	struct json_object *document = reply.status == 200 ? cloud_reply_json(&reply) : NULL;
	psn_http_reply_clear(&reply);
	struct json_object *data = cloud_json_object(document, "data");
	struct json_object *concept = cloud_json_object(data, "conceptRetrieve");
	struct json_object *product = cloud_json_object(concept, "defaultProduct");
	bool found = product && cloud_pick_psplus_sku(product, config->game_identifier, license);
	if(document) json_object_put(document);
	if(found)
		CHIAKI_LOGI(log, "Resolved PS Plus title '%s': entitlement=%s sku=%s",
			*cloud_nonnull(config->game_name) ? config->game_name : config->game_identifier,
			license->entitlement, license->sku);
	return found ? CHIAKI_ERR_SUCCESS : CHIAKI_ERR_UNKNOWN;
}

static ChiakiErrorCode cloud_kamaji_oauth(ChiakiLog *log, const ChiakiCloudProvisionConfig *config,
	const char *duid, const char *scope, const char *response_type, const char *client,
	bool browser_options, char **value)
{
	char url[2500];
	bool token_response = strcmp(response_type, "token") == 0;
	snprintf(url, sizeof(url),
		CLOUD_ACCOUNT_BASE "/api/v1/oauth/authorize?smcid=pc:psnow&applicationId=psnow"
		"&response_type=%s&scope=%s&client_id=%s&redirect_uri=" CLOUD_PSNOW_REDIRECT
		"%s&service_entity=urn:service-entity:psn&prompt=none%s"
		"&mid=PSNOW&duid=%s&layout_type=popup"
		"&service_logo=ps&tp_psn=true&noEVBlock=true",
		response_type, scope, client,
		token_response ? "&grant_type=authorization_code" : "",
		browser_options
			? "&renderMode=mobilePortrait&hidePageElements=forgotPasswordLink&displayFooter=none&disableLinks=qriocityLink"
			: "",
		duid);
	return cloud_oauth_redirect(log, url, config->npsso, CLOUD_PSNOW_AGENT,
		token_response ? "access_token" : "code", value);
}

static ChiakiErrorCode cloud_kamaji_session(ChiakiLog *log, const char *code, const char *duid,
	bool capture_cookie, CloudLicense *license)
{
	char body[1400];
	snprintf(body, sizeof(body), "code=%s&client_id=" CLOUD_PSNOW_CLIENT "&duid=%s", code, duid);
	const char *headers[] = {
		"Content-Type: text/plain;charset=UTF-8", "Accept: */*", "User-Agent: " CLOUD_PSNOW_AGENT,
		"Origin: https://psnow.playstation.com",
		"Referer: https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/",
		"X-Alt-Referer: " CLOUD_PSNOW_REDIRECT,
	};
	PsnHttpRequest request = {
		.method = PSN_HTTP_POST, .url = CLOUD_KAMAJI_BASE "/user/session",
		.headers = headers, .headers_count = 6, .payload = body,
		.keep_response_headers = capture_cookie,
	};
	PsnHttpReply reply;
	ChiakiErrorCode error = psn_http_execute(log, &request, &reply);
	if(error != CHIAKI_ERR_SUCCESS)
		return error;
	struct json_object *document = cloud_reply_json(&reply);
	bool accepted = reply.status == 200 && cloud_kamaji_ok(document);
	if(accepted && capture_cookie)
	{
		char *cookie = cloud_cookie_from_headers(reply.headers, "JSESSIONID");
		if(cookie)
		{
			cloud_copy_text(license->session_cookie, sizeof(license->session_cookie), cookie);
			free(cookie);
		}
		accepted = *license->session_cookie;
	}
	if(!accepted)
	{
		struct json_object *header = cloud_json_object(document, "header");
		CHIAKI_LOGE(log, "PS Now session authorization was rejected (HTTP %ld, status %s)",
			reply.status, cloud_json_string(header, "status_code"));
	}
	if(document) json_object_put(document);
	psn_http_reply_clear(&reply);
	return accepted ? CHIAKI_ERR_SUCCESS : CHIAKI_ERR_UNKNOWN;
}

static ChiakiErrorCode cloud_resolve_product(ChiakiLog *log,
	const ChiakiCloudProvisionConfig *config, CloudLicense *license)
{
	cloud_progress(config, "Resolving game license...");
	char country[8], language[8];
	cloud_copy_text(country, sizeof(country),
		*cloud_nonnull(config->store_country) ? config->store_country : "US");
	cloud_copy_text(language, sizeof(language),
		*cloud_nonnull(config->store_lang) ? config->store_lang : "en");
	const bool legacy_fallback = config->catalog_is_foreign
		&& cloud_legacy_product(config->game_identifier);
	char *product = psn_url_escape(config->game_identifier);
	if(!product)
		return CHIAKI_ERR_MEMORY;
	struct json_object *document = NULL;
	ChiakiErrorCode error = CHIAKI_ERR_UNKNOWN;
	for(int attempt = 0; attempt < (legacy_fallback ? 2 : 1); attempt++)
	{
		char url[900];
		snprintf(url, sizeof(url),
			"https://psnow.playstation.com/store/api/pcnow/00_09_000/container/%s/%s/19/%s"
			"?useOffers=true&gkb=1&gkb2=1", country, language, product);
		CHIAKI_LOGI(log, "Resolving cloud product %s in storefront %s/%s",
			config->game_identifier, country, language);
		const char *headers[] = { "Accept: application/json", "User-Agent: Mozilla/5.0" };
		PsnHttpRequest request = {
			.method = PSN_HTTP_GET, .url = url, .headers = headers, .headers_count = 2,
		};
		PsnHttpReply reply;
		error = psn_http_execute(log, &request, &reply);
		if(error != CHIAKI_ERR_SUCCESS)
			break;
		long status = reply.status;
		if(status == 200)
			document = cloud_reply_json(&reply);
		psn_http_reply_clear(&reply);
		if(document)
		{
			error = CHIAKI_ERR_SUCCESS;
			break;
		}
		if(status != 404 || attempt != 0)
			break;
		char fallback_country[8], fallback_language[8];
		cloud_store_location(config, fallback_country, fallback_language);
		if(strcmp(country, fallback_country) == 0 && strcmp(language, fallback_language) == 0)
			break;
		CHIAKI_LOGW(log, "Cloud product is unavailable in storefront %s/%s; retrying %s/%s",
			country, language, fallback_country, fallback_language);
		cloud_copy_text(country, sizeof(country), fallback_country);
		cloud_copy_text(language, sizeof(language), fallback_language);
	}
	free(product);
	if(error != CHIAKI_ERR_SUCCESS || !document)
		return error;
	char title[64];
	cloud_title_segment(config->game_identifier, title);
	bool found = document && cloud_pick_entitlement(document, title, license);
	if(document)
	{
		cloud_product_platform(document, license);
		json_object_put(document);
	}
	if(found)
		CHIAKI_LOGI(log, "Resolved cloud title '%s': platform=%s entitlement=%s sku=%s",
			*config->game_name ? config->game_name : config->game_identifier,
			license->platform, license->entitlement, license->sku);
	return found ? CHIAKI_ERR_SUCCESS : CHIAKI_ERR_UNKNOWN;
}

static ChiakiErrorCode cloud_commerce_token(ChiakiLog *log,
	const ChiakiCloudProvisionConfig *config, const char *duid, CloudLicense *license)
{
	char *token = NULL;
	ChiakiErrorCode error = cloud_kamaji_oauth(log, config, duid,
		"kamaji:get_internal_entitlements%20user:account.attributes.validate%20kamaji:get_privacy_settings"
		"%20user:account.settings.privacy.get%20kamaji:s2s.subscriptionsPremium.get",
		"token", CLOUD_COMMERCE_CLIENT, true, &token);
	if(error == CHIAKI_ERR_SUCCESS)
	{
		free(license->commerce_token);
		license->commerce_token = token;
	}
	else
		free(token);
	return error;
}

static ChiakiErrorCode cloud_check_attributes(ChiakiLog *log,
	const ChiakiCloudProvisionConfig *config, const CloudLicense *license)
{
	if(config->skip_account_attr_check)
		return CHIAKI_ERR_SUCCESS;
	static const char body[] =
		"{\"attributes\":[\"ONLINE_ID\",\"BIRTH_DATE\",\"CITY\",\"REAL_NAME\","
		"\"PRIVACY_SETTING_ACTIVITYSTREAM\",\"PRIVACY_SETTING_FRIENDSLIST\","
		"\"PRIVACY_SETTING_FRIENDREQUESTS\",\"PRIVACY_SETTING_MESSAGES\"]}";
	char *authorization = psn_http_bearer(license->commerce_token);
	const char *headers[] = { authorization, "Accept: application/json", "Content-Type: application/json" };
	PsnHttpRequest request = {
		.method = PSN_HTTP_POST,
		.url = "https://accounts.api.playstation.com/api/v2/accounts/me/attributes",
		.headers = headers, .headers_count = 3, .payload = body,
	};
	PsnHttpReply reply;
	ChiakiErrorCode error = psn_http_execute(log, &request, &reply);
	free(authorization);
	if(error != CHIAKI_ERR_SUCCESS)
		return error;
	bool accepted = reply.status == 200 || reply.status == 204;
	psn_http_reply_clear(&reply);
	return accepted ? CHIAKI_ERR_SUCCESS : CHIAKI_ERR_UNKNOWN;
}

static ChiakiErrorCode cloud_purchase_free_license(ChiakiLog *log,
	const ChiakiCloudProvisionConfig *config, CloudLicense *license,
	ChiakiCloudProvisionResult *result)
{
	if(cloud_cancelled(config))
		return CHIAKI_ERR_CANCELED;
	cloud_progress(config, "Adding cloud license to library...");
	char form[300];
	snprintf(form, sizeof(form), "sku=%s", *license->sku ? license->sku : license->entitlement);
	char *authorization = psn_http_bearer(license->commerce_token);
	char *cookie = *license->session_cookie ? psn_http_cookie("JSESSIONID", license->session_cookie) : NULL;
	const char *headers[] = {
		authorization, cookie ? cookie : "Accept: application/json",
		"User-Agent: " CLOUD_PSNOW_AGENT,
		"Content-Type: application/x-www-form-urlencoded; charset=UTF-8",
		"Accept: application/json, text/javascript, */*; q=0.01",
		"X-Requested-With: XMLHttpRequest", "Origin: https://psnow.playstation.com",
		"Referer: https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/",
	};
	PsnHttpRequest preview_request = {
		.method = PSN_HTTP_POST, .url = CLOUD_KAMAJI_BASE "/user/checkout/buynow/preview",
		.headers = headers, .headers_count = 8, .payload = form,
		.keep_response_headers = true,
	};
	PsnHttpReply preview_reply;
	ChiakiErrorCode error = psn_http_execute(log, &preview_request, &preview_reply);
	if(error != CHIAKI_ERR_SUCCESS)
	{
		free(authorization); free(cookie);
		return error;
	}
	struct json_object *preview = cloud_reply_json(&preview_reply);
	struct json_object *data = cloud_json_object(preview, "data");
	struct json_object *cart = cloud_json_object(data, "cart");
	bool free_offer = preview_reply.status == 200 && cloud_kamaji_ok(preview) && cart
		&& cloud_json_integer(cart, "total_price_value") == 0;
	if(free_offer)
	{
		char *refreshed_cookie = cloud_cookie_from_headers(preview_reply.headers, "JSESSIONID");
		if(refreshed_cookie && *refreshed_cookie)
			cloud_copy_text(license->session_cookie, sizeof(license->session_cookie), refreshed_cookie);
		free(refreshed_cookie);
		struct json_object *items = cloud_json_array(cart, "items");
		if(items && json_object_array_length(items))
		{
			const char *sku = cloud_json_string(json_object_array_get_idx(items, 0), "sku_id");
			if(*sku) cloud_copy_text(license->sku, sizeof(license->sku), sku);
		}
	}
	if(preview) json_object_put(preview);
	psn_http_reply_clear(&preview_reply);
	if(!free_offer)
	{
		free(authorization); free(cookie);
		cloud_set_error(result, "This title is not currently available as a free cloud entitlement.");
		return CHIAKI_ERR_UNKNOWN;
	}
	if(cloud_cancelled(config))
	{
		free(authorization); free(cookie);
		return CHIAKI_ERR_CANCELED;
	}
	char buy_form[320];
	snprintf(buy_form, sizeof(buy_form), "sku=%s&skipEmail=true", license->sku);
	free(cookie);
	cookie = *license->session_cookie ? psn_http_cookie("JSESSIONID", license->session_cookie) : NULL;
	headers[1] = cookie ? cookie : "Accept: application/json";
	PsnHttpRequest buy_request = {
		.method = PSN_HTTP_POST, .url = CLOUD_KAMAJI_BASE "/user/checkout/buynow",
		.headers = headers, .headers_count = 8, .payload = buy_form,
	};
	PsnHttpReply buy_reply;
	error = psn_http_execute(log, &buy_request, &buy_reply);
	free(authorization); free(cookie);
	if(error != CHIAKI_ERR_SUCCESS)
		return error;
	struct json_object *buy = cloud_reply_json(&buy_reply);
	bool accepted = buy_reply.status == 200 && cloud_kamaji_ok(buy);
	if(buy) json_object_put(buy);
	psn_http_reply_clear(&buy_reply);
	return accepted ? CHIAKI_ERR_SUCCESS : CHIAKI_ERR_UNKNOWN;
}

static ChiakiErrorCode cloud_ensure_license(ChiakiLog *log,
	const ChiakiCloudProvisionConfig *config, const char *duid,
	CloudLicense *license, ChiakiCloudProvisionResult *result)
{
	cloud_progress(config, "Checking cloud license...");
	ChiakiErrorCode error = cloud_commerce_token(log, config, duid, license);
	if(error != CHIAKI_ERR_SUCCESS)
		return error;
	error = cloud_check_attributes(log, config, license);
	if(error != CHIAKI_ERR_SUCCESS)
	{
		cloud_set_error(result, "ACCOUNT_PRIVACY_SETTINGS");
		return error;
	}
	char *escaped = psn_url_escape(license->entitlement);
	char url[800];
	snprintf(url, sizeof(url),
		"https://commerce.api.np.km.playstation.net/commerce/api/v1/users/me/internal_entitlements/%s?fields=game_meta",
		escaped ? escaped : "");
	free(escaped);
	char *authorization = psn_http_bearer(license->commerce_token);
	const char *headers[] = { authorization, "Accept: application/json" };
	PsnHttpRequest request = { .method = PSN_HTTP_GET, .url = url, .headers = headers, .headers_count = 2 };
	PsnHttpReply reply;
	error = psn_http_execute(log, &request, &reply);
	free(authorization);
	if(error != CHIAKI_ERR_SUCCESS)
		return error;
	long status = reply.status;
	psn_http_reply_clear(&reply);
	if(status == 200)
		return CHIAKI_ERR_SUCCESS;
	if(status != 404)
		return CHIAKI_ERR_UNKNOWN;
	error = cloud_purchase_free_license(log, config, license, result);
	if(error == CHIAKI_ERR_SUCCESS || error == CHIAKI_ERR_CANCELED
		|| !config->catalog_is_foreign || !cloud_legacy_product(config->game_identifier))
		return error;
	CHIAKI_LOGW(log, "The fallback PS3 storefront could not issue a free license; continuing with subscription validation");
	cloud_set_error(result, NULL);
	return CHIAKI_ERR_SUCCESS;
}

static ChiakiErrorCode cloud_prepare_psnow(ChiakiLog *log,
	const ChiakiCloudProvisionConfig *config, const char *duid,
	CloudLicense *license, ChiakiCloudProvisionResult *result)
{
	memset(license, 0, sizeof(*license));
	bool fast_path = *cloud_nonnull(config->owned_entitlement_id);
	if(fast_path)
	{
		cloud_copy_text(license->entitlement, sizeof(license->entitlement), config->owned_entitlement_id);
		cloud_copy_text(license->platform, sizeof(license->platform),
			*cloud_nonnull(config->owned_platform) ? config->owned_platform : "ps4");
	}
	else
	{
		char *anonymous_code = NULL;
		ChiakiErrorCode error = cloud_kamaji_oauth(log, config, duid,
			"kamaji:commerce_native%20kamaji:commerce_container%20kamaji:lists%20kamaji:s2s.subscriptionsPremium.get",
			"code", CLOUD_PSNOW_CLIENT, true, &anonymous_code);
		if(error != CHIAKI_ERR_SUCCESS)
		{
			cloud_set_stage_error(log, result, error, "requesting a PS Now authorization code");
			free(anonymous_code);
			return error;
		}
		error = cloud_kamaji_session(log, anonymous_code, duid, true, license);
		free(anonymous_code);
		if(error != CHIAKI_ERR_SUCCESS)
		{
			cloud_set_stage_error(log, result, error, "creating the PS Now catalog session");
			return error;
		}
		if(cloud_cancelled(config))
			return CHIAKI_ERR_CANCELED;
		error = cloud_resolve_product(log, config, license);
		if(error != CHIAKI_ERR_SUCCESS)
		{
			cloud_set_error(result, "The selected title has no cloud-streaming entitlement.");
			return error;
		}
		error = cloud_ensure_license(log, config, duid, license, result);
		if(error != CHIAKI_ERR_SUCCESS)
		{
			cloud_set_stage_error(log, result, error, "checking the PS Now streaming license");
			return error;
		}
	}

	if(cloud_cancelled(config))
		return CHIAKI_ERR_CANCELED;
	cloud_progress(config, "Creating PlayStation Now session...");
	const char *scope = strcmp(license->platform, "ps3") == 0
		? "kamaji:commerce_native"
		: "kamaji:commerce_native%20kamaji:commerce_container%20kamaji:lists%20kamaji:s2s.subscriptionsPremium.get";
	char *authenticated_code = NULL;
	ChiakiErrorCode error = cloud_kamaji_oauth(log, config, duid, scope, "code",
		CLOUD_PSNOW_CLIENT, false, &authenticated_code);
	if(error != CHIAKI_ERR_SUCCESS)
	{
		cloud_set_stage_error(log, result, error, "authorizing the selected PS Now title");
		free(authenticated_code);
		return error;
	}
	error = cloud_kamaji_session(log, authenticated_code, duid, false, license);
	free(authenticated_code);
	if(error != CHIAKI_ERR_SUCCESS)
		cloud_set_stage_error(log, result, error, "creating the authenticated PS Now session");
	return error;
}

static ChiakiErrorCode cloud_prepare_pscloud(ChiakiLog *log,
	const ChiakiCloudProvisionConfig *config, const char *duid,
	CloudLicense *license, ChiakiCloudProvisionResult *result)
{
	memset(license, 0, sizeof(*license));
	if(*cloud_nonnull(config->owned_entitlement_id))
	{
		cloud_copy_text(license->entitlement, sizeof(license->entitlement), config->owned_entitlement_id);
		cloud_copy_text(license->platform, sizeof(license->platform), "ps5");
		return CHIAKI_ERR_SUCCESS;
	}
	if(!config->plus_catalog)
	{
		cloud_set_error(result, "The selected PS5 title is not owned by this account.");
		return CHIAKI_ERR_UNKNOWN;
	}

	/* PS Plus PS5 rows are public catalog products, not account entitlements.
	 * Establish a Kamaji commerce session, resolve the subscription-only Store
	 * SKU, and perform the same zero-price entitlement activation Sony uses for
	 * Add to Library before asking Gaikai to authorize the stream. */
	char *code = NULL;
	ChiakiErrorCode error = cloud_kamaji_oauth(log, config, duid,
		"kamaji:commerce_native%20kamaji:commerce_container%20kamaji:lists%20kamaji:s2s.subscriptionsPremium.get",
		"code", CLOUD_PSNOW_CLIENT, true, &code);
	if(error != CHIAKI_ERR_SUCCESS)
	{
		free(code);
		cloud_set_stage_error(log, result, error, "requesting a PlayStation Plus authorization code");
		return error;
	}
	error = cloud_kamaji_session(log, code, duid, true, license);
	free(code);
	if(error != CHIAKI_ERR_SUCCESS)
	{
		cloud_set_stage_error(log, result, error, "creating the PlayStation Plus commerce session");
		return error;
	}
	if(cloud_cancelled(config))
		return CHIAKI_ERR_CANCELED;
	error = cloud_resolve_psplus_offer(log, config, license);
	if(error != CHIAKI_ERR_SUCCESS)
	{
		cloud_set_error(result, "The PS Plus offer for this title could not be resolved.");
		return error;
	}
	error = cloud_ensure_license(log, config, duid, license, result);
	if(error != CHIAKI_ERR_SUCCESS)
	{
		cloud_set_stage_error(log, result, error, "activating the PlayStation Plus title");
		return error;
	}
	/* Entitlement propagation is normally immediate, but authorizing Gaikai in
	 * the same millisecond can race the commerce backend. */
	if(!cloud_wait(config, 1))
		return CHIAKI_ERR_CANCELED;
	return CHIAKI_ERR_SUCCESS;
}

static void cloud_allocation_update_key(CloudAllocation *allocation, const PsnHttpReply *reply)
{
	char *value = psn_http_header(reply, "x-gaikai-session");
	if(value && *value)
	{
		free(allocation->session_key);
		allocation->session_key = value;
	}
	else
		free(value);
}

static ChiakiErrorCode cloud_allocation_json_request(CloudAllocation *allocation,
	PsnHttpMethod method, const char *url, struct json_object *payload,
	bool with_session_headers, PsnHttpReply *reply)
{
	char *agent = cloud_format_header("User-Agent", allocation->agent);
	char *session = with_session_headers ? cloud_format_header("X-Gaikai-Session", cloud_nonnull(allocation->session_key)) : NULL;
	char *session_id = with_session_headers && allocation->session_id && *allocation->session_id
		? cloud_format_header("X-Gaikai-SessionId", allocation->session_id) : NULL;
	const char *headers[5] = { "Accept: */*", "Content-Type: application/json", agent };
	size_t header_count = 3;
	if(session) headers[header_count++] = session;
	if(session_id) headers[header_count++] = session_id;
	const char *body = payload ? json_object_to_json_string_ext(payload, JSON_C_TO_STRING_PLAIN) : NULL;
	PsnHttpRequest request = {
		.method = method, .url = url, .headers = headers,
		.headers_count = header_count,
		.payload = body, .keep_response_headers = true,
	};
	ChiakiErrorCode error = psn_http_execute(allocation->log, &request, reply);
	free(agent); free(session); free(session_id);
	if(error == CHIAKI_ERR_SUCCESS)
		cloud_allocation_update_key(allocation, reply);
	return error;
}

static struct json_object *cloud_stream_specification(CloudAllocation *allocation, const char *entitlement)
{
	int resolution = allocation->config->resolution;
	int width = 1920, height = 1080;
	const char *resolution_name = "1080";
	if(resolution == 720) { width = 1280; height = 720; resolution_name = "720"; }
	else if(resolution == 1440) { width = 2560; height = 1440; resolution_name = "1440"; }
	else if(resolution == 2160) { width = 3840; height = 2160; resolution_name = "2160"; }
	char language[16];
	chiaki_cloud_gaikai_language(allocation->config->game_language, language, sizeof(language));

	time_t now = time(NULL);
	struct tm local_tm, utc_tm;
#ifdef _WIN32
	local_tm = *localtime(&now);
	utc_tm = *gmtime(&now);
#else
	localtime_r(&now, &local_tm);
	gmtime_r(&now, &utc_tm);
#endif
	utc_tm.tm_isdst = local_tm.tm_isdst;
	long offset = (long)difftime(mktime(&local_tm), mktime(&utc_tm));
	char timezone[16];
	snprintf(timezone, sizeof(timezone), "UTC%c%02ld:%02ld", offset >= 0 ? '+' : '-',
		labs(offset / 3600), labs(offset % 3600) / 60);

	struct json_object *spec = json_object_new_object();
#define SPEC_STRING(key, value) cloud_json_put_string(spec, key, value)
#define SPEC_INT(key, value) json_object_object_add(spec, key, json_object_new_int(value))
#define SPEC_BOOL(key, value) cloud_json_put_boolean(spec, key, value)
	SPEC_STRING("entitlementId", entitlement);
	SPEC_STRING("npEnv", "np");
	SPEC_STRING("language", language);
	SPEC_STRING("cloudEndpoint", "https://cc.prod.gaikai.com");
	SPEC_STRING("redirectUri", allocation->redirect);
	SPEC_STRING("resolutionSetting", resolution_name);
	SPEC_INT("clientWidth", width);
	SPEC_INT("clientHeight", height);
	SPEC_STRING("adaptiveStreamMode", "resize");
	SPEC_BOOL("useClientBwLadder", true);
	SPEC_BOOL("audioUploadEnabled", true);
	SPEC_INT("audioUploadNumChannels", 1);
	SPEC_INT("audioUploadSamplingFrequency", 48000);
	SPEC_STRING("acceptButton", "X");
	SPEC_BOOL("encryptionSupported", true);
	SPEC_INT("summerTime", 0);
	SPEC_STRING("timeZone", timezone);
	SPEC_STRING("httpUserAgent", allocation->agent);
	SPEC_STRING("gkCloudAuthCode", "");
	SPEC_STRING("ps3AuthCode", "");
	SPEC_STRING("streamServerAuthCode", "");
	SPEC_BOOL("isPlusMember", true);
	SPEC_BOOL("partyCapability", false);
	SPEC_BOOL("homesharing", false);
	SPEC_BOOL("isFirstBoot", false);
	SPEC_INT("parentalLevel", 0);
	SPEC_STRING("yuvCoefficient", "");

	struct json_object *controllers = json_object_new_array();
	if(allocation->ps5)
	{
		json_object_array_add(controllers, json_object_new_string("ds4"));
		json_object_array_add(controllers, json_object_new_string("ds5"));
		json_object_array_add(controllers, json_object_new_string("xinput"));
	}
	else
		json_object_array_add(controllers, json_object_new_string("xinput"));
	json_object_object_add(spec, "connectedControllers", json_object_get(controllers));
	struct json_object *input = json_object_new_object();
	json_object_object_add(input, "controllers", controllers);
	json_object_object_add(spec, "input", input);

	struct json_object *capabilities = json_object_new_array();
	json_object_array_add(capabilities, json_object_new_string("cloudDrivenSenkushaTest"));
	json_object_array_add(capabilities, json_object_new_string(allocation->ps5 ? "cronos" : "kratos"));
	json_object_object_add(spec, "capabilities", capabilities);
	if(allocation->ps5)
	{
		SPEC_STRING("model", "portal"); SPEC_STRING("platform", "qlite");
		SPEC_STRING("gaikaiPlayer", "16.4.0"); SPEC_INT("protocolVersion", 12);
		SPEC_STRING("videoEncoderProfile", "hw5.0"); SPEC_STRING("audioChannels", "2");
		SPEC_STRING("audioEncoderProfile", "default");
		struct json_object *video = json_object_new_object();
		json_object_object_add(video, "clientHeight", json_object_new_int(height));
		json_object_object_add(video, "supportedMaxResolution", json_object_new_int(height));
		struct json_object *profiles = json_object_new_array();
		json_object_array_add(profiles, json_object_new_string("hevc_hw4"));
		json_object_object_add(video, "supportedVideoEncoderProfiles", profiles);
		cloud_json_put_string(video, "supportedDynamicRange", "sdr");
		json_object_object_add(video, "preferredMaxResolution", json_object_new_int(height));
		cloud_json_put_string(video, "preferredDynamicRange", "sdr");
		json_object_object_add(video, "hqMode", json_object_new_int(1));
		json_object_object_add(spec, "videoStreamSettings", video);
		struct json_object *audio = json_object_new_object();
		cloud_json_put_string(audio, "audioEncoderProfile", "default");
		cloud_json_put_string(audio, "maxAudioChannels", "2");
		cloud_json_put_string(audio, "preferredNumberAudioChannels", "2");
		json_object_object_add(spec, "audioStreamSettings", audio);
	}
	else
	{
		SPEC_STRING("model", "WINDOWS"); SPEC_STRING("platform", "PC");
		SPEC_STRING("gaikaiPlayer", "12.5.0"); SPEC_INT("protocolVersion", 9);
		SPEC_STRING("videoEncoderProfile", "hw4.1"); SPEC_STRING("audioChannels", "2.1");
		SPEC_STRING("audioEncoderProfile", "default");
	}
#undef SPEC_STRING
#undef SPEC_INT
#undef SPEC_BOOL
	return spec;
}

static struct json_object *cloud_session_payload(CloudAllocation *allocation, struct json_object *extra)
{
	struct json_object *root = json_object_new_object();
	json_object_object_add(root, "requestGameSpecification", json_object_get(allocation->specification));
	if(extra)
	{
		json_object_object_foreach(extra, key, value)
			json_object_object_add(root, key, json_object_get(value));
	}
	return root;
}

static ChiakiErrorCode cloud_session_action(CloudAllocation *allocation, const char *suffix,
	struct json_object *extra, PsnHttpReply *reply)
{
	char url[700];
	snprintf(url, sizeof(url), CLOUD_GAIKAI_BASE "/sessions/%s%s",
		cloud_nonnull(allocation->session_id), suffix);
	struct json_object *payload = cloud_session_payload(allocation, extra);
	ChiakiErrorCode error = cloud_allocation_json_request(allocation, PSN_HTTP_POST, url, payload, true, reply);
	json_object_put(payload);
	return error;
}

static ChiakiErrorCode cloud_fetch_client_ids(CloudAllocation *allocation)
{
	cloud_progress(allocation->config, "Preparing cloud client...");
	char url[400];
	snprintf(url, sizeof(url), CLOUD_GAIKAI_BASE "/client_ids?virtType=%s", allocation->virtualization);
	PsnHttpReply reply;
	ChiakiErrorCode error = cloud_allocation_json_request(allocation, PSN_HTTP_GET, url, NULL, false, &reply);
	struct json_object *document = error == CHIAKI_ERR_SUCCESS && reply.status == 200 ? cloud_reply_json(&reply) : NULL;
	if(document)
	{
		cloud_copy_text(allocation->client_id, sizeof(allocation->client_id), cloud_json_string(document, "gkClientId"));
		cloud_copy_text(allocation->ps3_client_id, sizeof(allocation->ps3_client_id), cloud_json_string(document, "ps3GkClientId"));
		cloud_copy_text(allocation->stream_client_id, sizeof(allocation->stream_client_id), cloud_json_string(document, "streamServerClientId"));
		json_object_put(document);
	}
	if(error == CHIAKI_ERR_SUCCESS && (!document || !*allocation->client_id))
		CHIAKI_LOGE(allocation->log, "Cloud client IDs response was invalid (HTTP %ld)", reply.status);
	psn_http_reply_clear(&reply);
	return error == CHIAKI_ERR_SUCCESS && *allocation->client_id ? CHIAKI_ERR_SUCCESS : CHIAKI_ERR_UNKNOWN;
}

static ChiakiErrorCode cloud_fetch_configuration(CloudAllocation *allocation)
{
	cloud_progress(allocation->config, "Loading cloud configuration...");
	struct json_object *payload = json_object_new_object();
	cloud_json_put_string(payload, "product", allocation->ps5 ? "qlite" : "psnow");
	cloud_json_put_string(payload, "platform", allocation->ps5 ? "qlite" : "PC");
	cloud_json_put_string(payload, "sessionId", "");
	PsnHttpReply reply;
	ChiakiErrorCode error = cloud_allocation_json_request(allocation, PSN_HTTP_POST,
		CLOUD_CONFIG_BASE "/config", payload, false, &reply);
	json_object_put(payload);
	struct json_object *document = error == CHIAKI_ERR_SUCCESS && reply.status == 200 ? cloud_reply_json(&reply) : NULL;
	if(document)
	{
		const char *key = cloud_json_string(document, "configKey");
		if(*key)
		{
			free(allocation->session_key);
			allocation->session_key = strdup(key);
		}
		json_object_put(document);
	}
	if(error == CHIAKI_ERR_SUCCESS && !allocation->session_key)
		CHIAKI_LOGE(allocation->log, "Cloud configuration response was invalid (HTTP %ld)", reply.status);
	psn_http_reply_clear(&reply);
	return allocation->session_key ? CHIAKI_ERR_SUCCESS : CHIAKI_ERR_UNKNOWN;
}

static ChiakiErrorCode cloud_start_gaikai_session(CloudAllocation *allocation)
{
	cloud_progress(allocation->config, "Starting cloud session...");
	struct json_object *payload = json_object_new_object();
	json_object_object_add(payload, "requestGameSpecification", json_object_get(allocation->specification));
	PsnHttpReply reply;
	ChiakiErrorCode error = cloud_allocation_json_request(allocation, PSN_HTTP_POST,
		CLOUD_GAIKAI_BASE "/sessions/start?npEnv=np", payload, true, &reply);
	json_object_put(payload);
	if(error != CHIAKI_ERR_SUCCESS || reply.status != 200)
	{
		if(reply.body) cloud_set_error(allocation->result, reply.body);
		psn_http_reply_clear(&reply);
		return error == CHIAKI_ERR_SUCCESS ? CHIAKI_ERR_UNKNOWN : error;
	}
	struct json_object *document = cloud_reply_json(&reply);
	const char *session_id = cloud_json_string(document, "sessionId");
	if(*session_id) allocation->session_id = strdup(session_id);
	if(document) json_object_put(document);
	psn_http_reply_clear(&reply);
	return allocation->session_id ? CHIAKI_ERR_SUCCESS : CHIAKI_ERR_UNKNOWN;
}

static ChiakiErrorCode cloud_gaikai_oauth(CloudAllocation *allocation)
{
	cloud_progress(allocation->config, "Authorizing cloud services...");
	char url[2600];
	if(allocation->ps5)
		snprintf(url, sizeof(url), CLOUD_ACCOUNT_BASE "%s/oauth/authorize?response_type=code&client_id=%s"
			"&redirect_uri=gaikai://local&service_entity=urn:service-entity:psn&prompt=none&duid=%s"
			"&smcid=qlite&applicationId=qlite&mid=qlite&scope=id_token:psn.basic_claims%%20kamaji:s2s.subscriptionsPremium.get"
			"%%20id_token:duid%%20id_token:online_id%%20openid%%20psn:s2s",
			allocation->oauth_api, allocation->client_id, allocation->duid);
	else
		snprintf(url, sizeof(url), CLOUD_ACCOUNT_BASE "%s/oauth/authorize?response_type=code&client_id=%s"
			"&redirect_uri=" CLOUD_PSNOW_REDIRECT "&service_entity=urn:service-entity:psn&prompt=none&duid=%s"
			"&smcid=pc:psnow&applicationId=psnow&mid=PSNOW&scope=kamaji:commerce_native%%20versa:user_update_entitlements_first_play%%20kamaji:lists"
			"&renderMode=mobilePortrait&hidePageElements=forgotPasswordLink&displayFooter=none"
			"&disableLinks=qriocityLink&layout_type=popup&service_logo=ps&tp_psn=true&noEVBlock=true",
			allocation->oauth_api, allocation->client_id, allocation->duid);
	ChiakiErrorCode error = cloud_oauth_redirect(allocation->log, url, allocation->config->npsso,
		allocation->agent, "code", &allocation->primary_code);
	if(error != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(allocation->log, "PSN cloud primary OAuth authorization failed");
		return error;
	}

	if(allocation->ps5)
		snprintf(url, sizeof(url), CLOUD_ACCOUNT_BASE "%s/oauth/authorize?response_type=code&redirect_uri=gaikai://local"
			"&service_entity=urn:service-entity:psn&prompt=none&client_id=%s&smcid=qlite&applicationId=qlite&mid=qlite"
			"&scope=id_token:duid%%20id_token:online_id%%20openid%%20oauth:create_authn_ticket_for_cloud_console_signin&duid=%s",
			allocation->oauth_api, allocation->stream_client_id, allocation->duid);
	else
	{
		bool ps3 = strcmp(allocation->platform, "ps3") == 0;
		char duid_parameter[CHIAKI_DUID_STR_SIZE + 8] = "";
		if(!ps3)
			snprintf(duid_parameter, sizeof(duid_parameter), "&duid=%s", allocation->duid);
		snprintf(url, sizeof(url), CLOUD_ACCOUNT_BASE "%s/oauth/authorize?response_type=code&redirect_uri=" CLOUD_PSNOW_REDIRECT
			"&service_entity=urn:service-entity:psn&prompt=none&client_id=%s&smcid=pc:psnow&applicationId=psnow&mid=PSNOW"
			"&scope=%s%s&renderMode=mobilePortrait&hidePageElements=forgotPasswordLink&displayFooter=none"
			"&disableLinks=qriocityLink&layout_type=popup&service_logo=ps&tp_psn=true&noEVBlock=true",
			allocation->oauth_api, allocation->ps3_client_id,
			ps3 ? "kamaji:commerce_native" : "sso:none", duid_parameter);
	}
	char *server_code = NULL;
	error = cloud_oauth_redirect(allocation->log, url, allocation->config->npsso,
		allocation->agent, "code", &server_code);
	if(error != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(allocation->log, "PSN cloud server OAuth authorization failed");
		return error;
	}
	if(allocation->ps5)
	{
		allocation->stream_code = server_code;
		allocation->console_code = strdup("");
	}
	else
	{
		allocation->console_code = server_code;
		allocation->stream_code = strdup(server_code);
	}
	cloud_json_put_string(allocation->specification, "gkCloudAuthCode", allocation->primary_code);
	cloud_json_put_string(allocation->specification, "ps3AuthCode", allocation->console_code);
	cloud_json_put_string(allocation->specification, "streamServerAuthCode", allocation->stream_code);
	return CHIAKI_ERR_SUCCESS;
}

static void cloud_gaikai_rejection_fields(const char *text,
	char code[64], char name[128], char message[192])
{
	if(!text || !*text)
		return;
	struct json_object *document = json_tokener_parse(text);
	if(!document)
		return;
	struct json_object *details = document;
	struct json_object *error = cloud_json_object(document, "error");
	if(error)
		details = error;
	struct json_object *errors = cloud_json_array(document, "errors");
	if(errors && json_object_array_length(errors))
		details = json_object_array_get_idx(errors, 0);
	const char *value = cloud_json_string(details, "eventCode");
	if(!*value) value = cloud_json_string(details, "code");
	if(!*code && *value) cloud_copy_text(code, 64, value);
	value = cloud_json_string(details, "name");
	if(!*name && *value) cloud_copy_text(name, 128, value);
	value = cloud_json_string(details, "message");
	if(!*message && *value) cloud_copy_text(message, 192, value);
	json_object_put(document);
}

static ChiakiErrorCode cloud_authorize_gaikai(CloudAllocation *allocation)
{
	cloud_progress(allocation->config, "Authorizing streaming session...");
	PsnHttpReply reply;
	ChiakiErrorCode error = cloud_session_action(allocation, "/authorize", NULL, &reply);
	if(error != CHIAKI_ERR_SUCCESS || reply.status != 200)
	{
		char *event = psn_http_header(&reply, "x-gaikai-event");
		char event_code[64] = "", event_name[128] = "", event_message[192] = "";
		cloud_gaikai_rejection_fields(event, event_code, event_name, event_message);
		cloud_gaikai_rejection_fields(reply.body, event_code, event_name, event_message);
		const bool subscription_rejected = (event && strstr(event, "002.2001"))
			|| (reply.body && strstr(reply.body, "002.2001"));
		CHIAKI_LOGE(allocation->log,
			"Cloud authorization rejected (HTTP %ld, platform=%s, game=%s, entitlement=%s, event=%s, name=%s, message=%s)",
			reply.status, allocation->platform, allocation->config->game_identifier,
			cloud_json_string(allocation->specification, "entitlementId"),
			*event_code ? event_code : "unknown", *event_name ? event_name : "unknown",
			*event_message ? event_message : "unknown");
		if(subscription_rejected)
			cloud_set_error(allocation->result, strcmp(allocation->platform, "ps3") == 0
				? "PS3_CLOUD_TITLE_NOT_AUTHORIZED" : "PS_PLUS_SUBSCRIPTION_REQUIRED");
		else if(reply.body)
			cloud_set_error(allocation->result, reply.body);
		free(event);
		psn_http_reply_clear(&reply);
		return error == CHIAKI_ERR_SUCCESS ? CHIAKI_ERR_UNKNOWN : error;
	}
	psn_http_reply_clear(&reply);
	return CHIAKI_ERR_SUCCESS;
}

static ChiakiErrorCode cloud_lock_gaikai(CloudAllocation *allocation)
{
	cloud_progress(allocation->config, "Closing any previous cloud session...");
	for(int attempt = 0; attempt < 13; attempt++)
	{
		if(cloud_cancelled(allocation->config))
			return CHIAKI_ERR_CANCELED;
		PsnHttpReply reply;
		ChiakiErrorCode error = cloud_session_action(allocation, "/lock?forceLogout=true", NULL, &reply);
		if(error != CHIAKI_ERR_SUCCESS)
		{
			psn_http_reply_clear(&reply);
			if(attempt >= 5 || !cloud_wait(allocation->config, 5))
				return error;
			continue;
		}
		struct json_object *document = reply.status == 200 ? cloud_reply_json(&reply) : NULL;
		bool acquired = cloud_json_boolean(document, "lockAcquired");
		int poll = cloud_json_integer(document, "pollFrequency");
		if(document) json_object_put(document);
		psn_http_reply_clear(&reply);
		if(acquired)
		{
			free(allocation->locked_key);
			allocation->locked_key = allocation->session_key ? strdup(allocation->session_key) : NULL;
			return CHIAKI_ERR_SUCCESS;
		}
		if(!cloud_wait(allocation->config, poll > 0 ? (unsigned)poll : 10))
			return CHIAKI_ERR_CANCELED;
	}
	return CHIAKI_ERR_UNKNOWN;
}

typedef struct cloud_ping_job_t
{
	ChiakiLog *log;
	const char *session_key;
	ChiakiServiceType service;
	char name[128];
	char host[128];
	uint16_t port;
	int bandwidth;
	uint64_t rtt_us;
	uint32_t mtu_in;
	uint32_t mtu_out;
	ChiakiErrorCode error;
	ChiakiThread thread;
	bool thread_started;
} CloudPingJob;

static void *cloud_ping_worker(void *opaque)
{
	CloudPingJob *job = opaque;
	job->error = cloud_probe_datacenter(job->log, job->host, job->port, job->session_key,
		job->service, &job->rtt_us, &job->mtu_in, &job->mtu_out);
	return NULL;
}

static struct json_object *cloud_ping_json(const CloudPingJob *job, bool measured)
{
	struct json_object *row = json_object_new_object();
	cloud_json_put_string(row, "dataCenter", job->name);
	int milliseconds = measured ? (int)(job->rtt_us / 1000) : 999;
	json_object_object_add(row, "rtt", json_object_new_int(milliseconds));
	struct json_object *rtts = json_object_new_array();
	json_object_array_add(rtts, json_object_new_int(milliseconds));
	json_object_object_add(row, "rtts", rtts);
	json_object_object_add(row, "mtu_in", json_object_new_int((int)job->mtu_in));
	json_object_object_add(row, "mtu_out", json_object_new_int((int)job->mtu_out));
	json_object_object_add(row, "port", json_object_new_int(job->port));
	cloud_json_put_string(row, "publicIp", job->host);
	json_object_object_add(row, "maxBandwidth", json_object_new_int(job->bandwidth));
	cloud_json_put_boolean(row, "measured", measured);
	return row;
}

static int cloud_ping_compare(const void *left_ptr, const void *right_ptr)
{
	struct json_object *left = *(struct json_object *const *)left_ptr;
	struct json_object *right = *(struct json_object *const *)right_ptr;
	return cloud_json_integer(left, "rtt") - cloud_json_integer(right, "rtt");
}

static struct json_object *cloud_find_named_ping(struct json_object *pings, const char *name)
{
	for(size_t i = 0; pings && i < json_object_array_length(pings); i++)
	{
		struct json_object *row = json_object_array_get_idx(pings, i);
		if(strcmp(cloud_json_string(row, "dataCenter"), cloud_nonnull(name)) == 0)
			return row;
	}
	return NULL;
}

static ChiakiErrorCode cloud_measure_datacenters(CloudAllocation *allocation,
	struct json_object *datacenters)
{
	size_t count = json_object_array_length(datacenters);
	if(count == 0)
		return CHIAKI_ERR_UNKNOWN;
	const char *forced = cloud_nonnull(allocation->config->forced_datacenter);
	bool use_forced = *forced && strcmp(forced, "Auto") != 0;
	allocation->pings = json_object_new_array();
	if(use_forced)
	{
		struct json_object *entry = cloud_find_named_ping(datacenters, forced);
		if(!entry)
		{
			cloud_set_error(allocation->result, "The selected datacenter is unavailable for this title.");
			return CHIAKI_ERR_UNKNOWN;
		}
		CloudPingJob job = { 0 };
		cloud_copy_text(job.name, sizeof(job.name), forced);
		cloud_copy_text(job.host, sizeof(job.host), cloud_json_string(entry, "publicIp"));
		job.port = (uint16_t)cloud_json_integer(entry, "port");
		job.bandwidth = cloud_json_integer(entry, "maxBandwidth");
		job.rtt_us = 20000; job.mtu_in = 1454; job.mtu_out = 1254;
		json_object_array_add(allocation->pings, cloud_ping_json(&job, true));
		return CHIAKI_ERR_SUCCESS;
	}

	cloud_progress(allocation->config, "Measuring cloud datacenters...");
	CloudPingJob *jobs = calloc(count, sizeof(*jobs));
	if(!jobs)
		return CHIAKI_ERR_MEMORY;
	for(size_t i = 0; i < count; i++)
	{
		struct json_object *entry = json_object_array_get_idx(datacenters, i);
		jobs[i].log = allocation->log;
		jobs[i].session_key = allocation->locked_key;
		jobs[i].service = allocation->ps5 ? CHIAKI_SERVICE_TYPE_PSCLOUD : CHIAKI_SERVICE_TYPE_PSNOW;
		cloud_copy_text(jobs[i].name, sizeof(jobs[i].name), cloud_json_string(entry, "dataCenter"));
		cloud_copy_text(jobs[i].host, sizeof(jobs[i].host), cloud_json_string(entry, "publicIp"));
		jobs[i].port = (uint16_t)cloud_json_integer(entry, "port");
		jobs[i].bandwidth = cloud_json_integer(entry, "maxBandwidth");
		jobs[i].thread_started = chiaki_thread_create(&jobs[i].thread, cloud_ping_worker, &jobs[i]) == CHIAKI_ERR_SUCCESS;
		if(!jobs[i].thread_started)
			cloud_ping_worker(&jobs[i]);
	}
	for(size_t i = 0; i < count; i++)
	{
		if(jobs[i].thread_started)
			chiaki_thread_join(&jobs[i].thread, NULL);
		bool measured = jobs[i].error == CHIAKI_ERR_SUCCESS && jobs[i].rtt_us > 0;
		if(!measured)
			CHIAKI_LOGW(allocation->log, "Datacenter probe failed: %s (%s:%u): %s",
				jobs[i].name, jobs[i].host, jobs[i].port, chiaki_error_string(jobs[i].error));
		json_object_array_add(allocation->pings, cloud_ping_json(&jobs[i], measured));
	}
	free(jobs);

	struct json_object **sorted = calloc(count, sizeof(*sorted));
	if(sorted)
	{
		for(size_t i = 0; i < count; i++) sorted[i] = json_object_get(json_object_array_get_idx(allocation->pings, i));
		qsort(sorted, count, sizeof(*sorted), cloud_ping_compare);
		struct json_object *ordered = json_object_new_array();
		for(size_t i = 0; i < count; i++) json_object_array_add(ordered, sorted[i]);
		free(sorted);
		json_object_put(allocation->pings);
		allocation->pings = ordered;
	}
	return CHIAKI_ERR_SUCCESS;
}

static void cloud_merge_prior_pings(CloudAllocation *allocation, struct json_object *datacenters)
{
	struct json_object *prior = *cloud_nonnull(allocation->config->prior_datacenters_json)
		? json_tokener_parse(allocation->config->prior_datacenters_json) : NULL;
	struct json_object *picker = json_object_new_array();
	for(size_t i = 0; i < json_object_array_length(datacenters); i++)
	{
		struct json_object *dc = json_object_array_get_idx(datacenters, i);
		const char *name = cloud_json_string(dc, "dataCenter");
		struct json_object *row = cloud_find_named_ping(allocation->pings, name);
		if(!row) row = cloud_find_named_ping(prior, name);
		if(row)
			json_object_array_add(picker, cloud_json_copy(row));
	}
	for(size_t i = 0; prior && i < json_object_array_length(prior); i++)
	{
		struct json_object *row = json_object_array_get_idx(prior, i);
		if(!cloud_find_named_ping(picker, cloud_json_string(row, "dataCenter")))
			json_object_array_add(picker, cloud_json_copy(row));
	}
	if(prior) json_object_put(prior);
	const char *serialized = json_object_to_json_string_ext(picker, JSON_C_TO_STRING_PLAIN);
	free(allocation->result->datacenter_pings);
	allocation->result->datacenter_pings = serialized ? strdup(serialized) : NULL;
	json_object_put(picker);
}

static ChiakiErrorCode cloud_select_datacenter(CloudAllocation *allocation)
{
	const char *forced = cloud_nonnull(allocation->config->forced_datacenter);
	bool use_forced = *forced && strcmp(forced, "Auto") != 0;
	allocation->selected_ping = use_forced ? cloud_find_named_ping(allocation->pings, forced)
		: json_object_array_get_idx(allocation->pings, 0);
	if(!allocation->selected_ping || (!use_forced && !cloud_json_boolean(allocation->selected_ping, "measured")))
	{
		cloud_set_error(allocation->result, "PING_TIMEOUT");
		return CHIAKI_ERR_UNKNOWN;
	}
	int rtt = cloud_json_integer(allocation->selected_ping, "rtt");
	if(!use_forced && rtt > 120)
	{
		cloud_set_error(allocation->result, "PING_TIMEOUT");
		return CHIAKI_ERR_UNKNOWN;
	}
	cloud_copy_text(allocation->selected_name, sizeof(allocation->selected_name),
		cloud_json_string(allocation->selected_ping, "dataCenter"));
	allocation->selected_port = cloud_json_integer(allocation->selected_ping, "port");
	if(allocation->selected_port <= 0) allocation->selected_port = 2053;

	cloud_progress(allocation->config, "Selecting cloud datacenter...");
	struct json_object *extra = json_object_new_object();
	json_object_object_add(extra, "pingResults", json_object_get(allocation->pings));
	PsnHttpReply reply;
	ChiakiErrorCode error = cloud_session_action(allocation, "/datacenters/select", extra, &reply);
	json_object_put(extra);
	if(error == CHIAKI_ERR_SUCCESS && reply.status == 200)
	{
		struct json_object *document = cloud_reply_json(&reply);
		int port = cloud_json_integer(document, "port");
		if(port <= 0)
			port = cloud_json_integer(cloud_json_object(document, "network"), "port");
		if(port > 0) allocation->selected_port = port;
		if(document) json_object_put(document);
	}
	else if(error == CHIAKI_ERR_SUCCESS)
		error = CHIAKI_ERR_UNKNOWN;
	psn_http_reply_clear(&reply);
	return error;
}

static ChiakiErrorCode cloud_get_datacenters(CloudAllocation *allocation)
{
	cloud_progress(allocation->config, "Loading cloud datacenters...");
	PsnHttpReply reply;
	ChiakiErrorCode error = cloud_session_action(allocation, "/datacenters", NULL, &reply);
	struct json_object *datacenters = error == CHIAKI_ERR_SUCCESS && reply.status == 200 ? cloud_reply_json(&reply) : NULL;
	psn_http_reply_clear(&reply);
	if(!datacenters || json_object_get_type(datacenters) != json_type_array || !json_object_array_length(datacenters))
	{
		if(datacenters) json_object_put(datacenters);
		return error == CHIAKI_ERR_SUCCESS ? CHIAKI_ERR_UNKNOWN : error;
	}
	error = cloud_measure_datacenters(allocation, datacenters);
	cloud_merge_prior_pings(allocation, datacenters);
	json_object_put(datacenters);
	return error == CHIAKI_ERR_SUCCESS ? cloud_select_datacenter(allocation) : error;
}

static ChiakiErrorCode cloud_allocate_slot(CloudAllocation *allocation)
{
	cloud_progress(allocation->config, "Allocating cloud streaming slot...");
	int elapsed = 0;
	int limit = 300;
	int network_errors = 0;
	while(elapsed <= limit)
	{
		if(cloud_cancelled(allocation->config))
			return CHIAKI_ERR_CANCELED;
		int rtt = cloud_json_integer(allocation->selected_ping, "rtt");
		int mtu_in = cloud_json_integer(allocation->selected_ping, "mtu_in");
		int mtu_out = cloud_json_integer(allocation->selected_ping, "mtu_out");
		if(rtt <= 0) rtt = 25;
		if(mtu_in <= 0) mtu_in = 1454;
		if(mtu_out <= 0) mtu_out = 1254;
		struct json_object *extra = json_object_new_object();
		cloud_json_put_string(extra, "dataCenter", allocation->selected_name);
		struct json_object *network = json_object_new_object();
		json_object_object_add(network, "bwKbpsSent", json_object_new_int(allocation->config->bitrate_kbps));
		json_object_object_add(network, "bwKbpsReceived", json_object_new_int(allocation->config->bitrate_kbps));
		json_object_object_add(network, "bwLoss", json_object_new_double(0.001));
		json_object_object_add(network, "bwLossUpstream", json_object_new_int(0));
		json_object_object_add(network, "mtu", json_object_new_int(mtu_in));
		json_object_object_add(network, "mtuUpstream", json_object_new_int(mtu_out));
		json_object_object_add(network, "rtt", json_object_new_int(rtt));
		json_object_object_add(network, "port", json_object_new_int(allocation->selected_port));
		json_object_object_add(extra, "network", network);
		json_object_object_add(extra, "stateExecutionTime", json_object_new_double(5974.7632));
		json_object_object_add(extra, "streamTestTime", json_object_new_double(11262.8423));
		PsnHttpReply reply;
		ChiakiErrorCode error = cloud_session_action(allocation, "/allocate", extra, &reply);
		json_object_put(extra);
		if(error != CHIAKI_ERR_SUCCESS)
		{
			psn_http_reply_clear(&reply);
			if(++network_errors > 5 || !cloud_wait(allocation->config, 5))
			{
				cloud_set_error(allocation->result, "The cloud allocation request lost its network connection.");
				return error;
			}
			elapsed += 5;
			continue;
		}
		network_errors = 0;
		if(reply.status != 200)
		{
			psn_http_reply_clear(&reply);
			return CHIAKI_ERR_UNKNOWN;
		}
		struct json_object *document = cloud_reply_json(&reply);
		psn_http_reply_clear(&reply);
		if(!document)
			return CHIAKI_ERR_UNKNOWN;
		bool waiting = cloud_json_boolean(document, "queued") || cloud_json_boolean(document, "dataMigration");
		if(waiting)
		{
			int estimate = cloud_json_integer(document, "waitTimeEstimate");
			if(estimate > 0 && elapsed == 0) limit = estimate * 2 < 900 ? estimate * 2 : 900;
			int poll = cloud_json_integer(document, "pollFrequency");
			if(poll <= 0) poll = 15;
			char progress[160];
			if(cloud_json_boolean(document, "dataMigration"))
				snprintf(progress, sizeof(progress), "Migrating cloud data (%d%%)...",
					cloud_json_integer(document, "dataMigrationPercentageComplete"));
			else
				snprintf(progress, sizeof(progress), "Waiting for a cloud slot (queue %d)...",
					cloud_json_integer(document, "displayQueuePosition"));
			cloud_progress(allocation->config, progress);
			json_object_put(document);
			if(!cloud_wait(allocation->config, (unsigned)poll)) return CHIAKI_ERR_CANCELED;
			elapsed += poll;
			continue;
		}
		struct json_object *slot = cloud_json_object(document, "launchSlot");
		if(!slot)
		{
			json_object_put(document);
			return CHIAKI_ERR_UNKNOWN;
		}
		cloud_copy_text(allocation->result->server_ip, sizeof(allocation->result->server_ip),
			cloud_json_string(slot, "publicIp"));
		allocation->result->server_port = cloud_json_integer(slot, "port");
		allocation->result->handshake_key = strdup(cloud_json_string(document, "handshakeKey"));
		allocation->result->launch_spec = strdup(cloud_json_string(document, "launchSpecification"));
		allocation->result->session_id = strdup(cloud_json_string(document, "sessionId"));
		allocation->result->mtu_in = (uint32_t)mtu_in;
		allocation->result->mtu_out = (uint32_t)mtu_out;
		allocation->result->rtt_us = (uint64_t)rtt * 1000;
		allocation->result->psn_wrapper_type = 1;
		const char *private_ip = cloud_json_string(slot, "privateIp");
		const char *last_dot = strrchr(private_ip, '.');
		if(last_dot)
		{
			int wrapper = atoi(last_dot + 1);
			if(wrapper >= 0 && wrapper <= 255) allocation->result->psn_wrapper_type = (uint8_t)wrapper;
		}
		json_object_put(document);
		return *allocation->result->server_ip && allocation->result->server_port > 0
			? CHIAKI_ERR_SUCCESS : CHIAKI_ERR_UNKNOWN;
	}
	cloud_set_error(allocation->result, "Timed out waiting for a cloud streaming slot.");
	return CHIAKI_ERR_TIMEOUT;
}

static void cloud_allocation_clear(CloudAllocation *allocation)
{
	free(allocation->session_key); free(allocation->locked_key); free(allocation->session_id);
	free(allocation->primary_code); free(allocation->console_code); free(allocation->stream_code);
	if(allocation->specification) json_object_put(allocation->specification);
	if(allocation->pings) json_object_put(allocation->pings);
}

static ChiakiErrorCode cloud_run_allocation(ChiakiLog *log,
	const ChiakiCloudProvisionConfig *config, const char *duid,
	const char *platform, const char *entitlement, ChiakiCloudProvisionResult *result)
{
	CloudAllocation allocation;
	memset(&allocation, 0, sizeof(allocation));
	allocation.log = log;
	allocation.config = config;
	allocation.result = result;
	allocation.platform = *cloud_nonnull(platform) ? platform : "ps4";
	allocation.ps5 = strcmp(config->service_type, "pscloud") == 0;
	allocation.virtualization = strcmp(allocation.platform, "ps3") == 0 ? "konan"
		: strcmp(allocation.platform, "ps5") == 0 ? "cronos" : "kratos";
	allocation.agent = allocation.ps5 ? CLOUD_PS5_AGENT : CLOUD_PSNOW_AGENT;
	allocation.oauth_api = allocation.ps5 ? "/api/authz/v3" : "/api/v1";
	allocation.redirect = allocation.ps5 ? "gaikai://local" : CLOUD_PSNOW_REDIRECT;
	cloud_copy_text(allocation.duid, sizeof(allocation.duid), duid);
	allocation.specification = cloud_stream_specification(&allocation, entitlement);
	cloud_copy_text(result->platform, sizeof(result->platform), allocation.platform);
	cloud_copy_text(result->entitlement_id, sizeof(result->entitlement_id), entitlement);

	const char *stage = "fetching PSN cloud client IDs";
	ChiakiErrorCode error = cloud_fetch_client_ids(&allocation);
	if(error == CHIAKI_ERR_SUCCESS && cloud_cancelled(config)) error = CHIAKI_ERR_CANCELED;
	if(error == CHIAKI_ERR_SUCCESS) { stage = "loading the cloud configuration"; error = cloud_fetch_configuration(&allocation); }
	if(error == CHIAKI_ERR_SUCCESS) { stage = "starting the PSN cloud session"; error = cloud_start_gaikai_session(&allocation); }
	if(error == CHIAKI_ERR_SUCCESS) { stage = "requesting PSN cloud authorization codes"; error = cloud_gaikai_oauth(&allocation); }
	if(error == CHIAKI_ERR_SUCCESS) { stage = "authorizing the PSN cloud session"; error = cloud_authorize_gaikai(&allocation); }
	if(error == CHIAKI_ERR_SUCCESS) { stage = "locking the PSN cloud session"; error = cloud_lock_gaikai(&allocation); }
	if(error == CHIAKI_ERR_SUCCESS) { stage = "loading PSN cloud datacenters"; error = cloud_get_datacenters(&allocation); }
	if(error == CHIAKI_ERR_SUCCESS) { stage = "allocating a PSN cloud streaming slot"; error = cloud_allocate_slot(&allocation); }
	cloud_set_stage_error(log, result, error, stage);
	cloud_allocation_clear(&allocation);
	return error;
}

void chiaki_cloud_provision_result_fini(ChiakiCloudProvisionResult *result)
{
	if(!result)
		return;
	free(result->handshake_key);
	free(result->launch_spec);
	free(result->session_id);
	free(result->datacenter_pings);
	free(result->error_message);
	memset(result, 0, sizeof(*result));
}

ChiakiErrorCode chiaki_cloud_provision_session(const ChiakiCloudProvisionConfig *input,
	ChiakiCloudProvisionResult *result, ChiakiLog *log)
{
	if(!result)
		return CHIAKI_ERR_INVALID_DATA;
	memset(result, 0, sizeof(*result));
	result->err = CHIAKI_ERR_UNKNOWN;
	if(!input || !input->npsso || !*input->npsso || !input->game_identifier || !*input->game_identifier)
	{
		result->err = CHIAKI_ERR_INVALID_DATA;
		return result->err;
	}
	ChiakiCloudProvisionConfig config = *input;
	config.service_type = cloud_nonnull(config.service_type);
	config.game_identifier = cloud_nonnull(config.game_identifier);
	config.game_name = cloud_nonnull(config.game_name);
	config.concept_id = cloud_nonnull(config.concept_id);
	config.store_country = cloud_nonnull(config.store_country);
	config.store_lang = cloud_nonnull(config.store_lang);
	config.owned_entitlement_id = cloud_nonnull(config.owned_entitlement_id);
	config.owned_platform = cloud_nonnull(config.owned_platform);
	config.forced_datacenter = cloud_nonnull(config.forced_datacenter);
	config.prior_datacenters_json = cloud_nonnull(config.prior_datacenters_json);
	config.game_language = cloud_nonnull(config.game_language);
	CHIAKI_LOGI(log, "Starting Cloud Play provisioning: service=%s game='%s' identifier=%s country=%s language=%s foreign_catalog=%s",
		config.service_type, *config.game_name ? config.game_name : config.game_identifier,
		config.game_identifier, config.store_country, config.store_lang,
		config.catalog_is_foreign ? "yes" : "no");
	if(strcmp(config.service_type, "pscloud") != 0 && strcmp(config.service_type, "psnow") != 0)
	{
		result->err = CHIAKI_ERR_INVALID_DATA;
		return result->err;
	}
	if(cloud_cancelled(&config))
	{
		result->err = CHIAKI_ERR_CANCELED;
		return result->err;
	}
	char duid[CHIAKI_DUID_STR_SIZE];
	size_t duid_size = sizeof(duid);
	if(chiaki_holepunch_generate_client_device_uid(duid, &duid_size) != CHIAKI_ERR_SUCCESS)
	{
		result->err = CHIAKI_ERR_UNKNOWN;
		return result->err;
	}
	ChiakiErrorCode error = cloud_authorization_check(log, &config, duid);
	if(error != CHIAKI_ERR_SUCCESS)
	{
		cloud_set_error(result, error == CHIAKI_ERR_NETWORK
			? "Network error while contacting PlayStation."
			: "AUTHORIZATION_FAILED");
		result->err = error;
		return error;
	}

	char entitlement[128];
	char platform[8];
	if(strcmp(config.service_type, "pscloud") == 0)
	{
		CloudLicense license;
		error = cloud_prepare_pscloud(log, &config, duid, &license, result);
		if(error != CHIAKI_ERR_SUCCESS)
		{
			free(license.commerce_token);
			result->err = error;
			return error;
		}
		cloud_copy_text(entitlement, sizeof(entitlement), license.entitlement);
		cloud_copy_text(platform, sizeof(platform), "ps5");
		free(license.commerce_token);
	}
	else
	{
		CloudLicense license;
		error = cloud_prepare_psnow(log, &config, duid, &license, result);
		if(error != CHIAKI_ERR_SUCCESS)
		{
			free(license.commerce_token);
			result->err = error;
			return error;
		}
		cloud_copy_text(entitlement, sizeof(entitlement), license.entitlement);
		cloud_copy_text(platform, sizeof(platform), license.platform);
		free(license.commerce_token);
	}
	error = cloud_run_allocation(log, &config, duid, platform, entitlement, result);
	if(error != CHIAKI_ERR_SUCCESS && *config.owned_entitlement_id && result->error_message
		&& strstr(result->error_message, "noGameForEntitlement") && !cloud_cancelled(&config))
	{
		chiaki_cloud_provision_result_fini(result);
		config.owned_entitlement_id = "";
		config.owned_platform = "";
		CloudLicense retry_license;
		error = cloud_prepare_psnow(log, &config, duid, &retry_license, result);
		if(error == CHIAKI_ERR_SUCCESS)
			error = cloud_run_allocation(log, &config, duid, retry_license.platform,
				retry_license.entitlement, result);
		free(retry_license.commerce_token);
	}
	result->err = error;
	return error;
}
