// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_CLOUDSESSION_H
#define CHIAKI_CLOUDSESSION_H

#include <chiaki/common.h>
#include <chiaki/log.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chiaki_cloud_provision_config_t
{
	const char *service_type;
	const char *game_identifier;
	const char *game_name;
	const char *concept_id;
	const char *npsso;
	const char *store_country;
	const char *store_lang;
	const char *owned_entitlement_id;
	const char *owned_platform;
	bool plus_catalog;
	bool catalog_is_foreign;
	bool skip_account_attr_check;
	const char *forced_datacenter;
	const char *prior_datacenters_json;
	const char *game_language;
	int resolution;
	int bitrate_kbps;
	void (*progress)(const char *message, void *user);
	bool (*is_cancelled)(void *user);
	void *user;
} ChiakiCloudProvisionConfig;

typedef struct chiaki_cloud_provision_result_t
{
	ChiakiErrorCode err;
	char server_ip[64];
	int server_port;
	char *handshake_key;
	char *launch_spec;
	char *session_id;
	char entitlement_id[128];
	char platform[8];
	uint8_t psn_wrapper_type;
	uint32_t mtu_in;
	uint32_t mtu_out;
	uint64_t rtt_us;
	char *datacenter_pings;
	char *error_message;
} ChiakiCloudProvisionResult;

/* Blocking network operation. Invoke it outside the UI thread. */
CHIAKI_EXPORT ChiakiErrorCode chiaki_cloud_provision_session(
	const ChiakiCloudProvisionConfig *config,
	ChiakiCloudProvisionResult *result,
	ChiakiLog *log);

CHIAKI_EXPORT void chiaki_cloud_provision_result_fini(ChiakiCloudProvisionResult *result);

#ifdef __cplusplus
}
#endif

#endif
