// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_CLOUDCATALOG_H
#define CHIAKI_CLOUDCATALOG_H

#include <chiaki/common.h>
#include <chiaki/log.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct chiaki_cloudcatalog_config_t
{
	const char *npsso;
	const char *locale;
	const char *cache_dir;
	bool force_refresh;
} ChiakiCloudCatalogConfig;

typedef struct chiaki_cloudcatalog_result_t
{
	ChiakiErrorCode err;
	char *json;
	char *error_message;
} ChiakiCloudCatalogResult;

/*
 * Returns a UI-ready object with this stable envelope:
 * {total,nativeMode,fallbackRegion,resolvedStoreLang,
 *  settledLocale,warning,games:[...]}
 *
 * Each game carries productId, name, imageUrl, landscapeImageUrl, conceptId,
 * category, platform, serviceType, streamServiceType, streamIdentifier,
 * entitlementId, storeProductId, conceptUrl, isOwned, streamingSupported,
 * isPlayable and plusCatalog. A cross-generation card additionally carries a
 * variants array with the complete PS5 and PS4 launch rows. Ownership and Cloud
 * Play availability are intentionally independent: a purchased game may not
 * support cloud streaming.
 */
CHIAKI_EXPORT ChiakiErrorCode chiaki_cloudcatalog_fetch_unified(
	const ChiakiCloudCatalogConfig *config,
	ChiakiCloudCatalogResult *result,
	ChiakiLog *log);

CHIAKI_EXPORT void chiaki_cloudcatalog_result_fini(ChiakiCloudCatalogResult *result);

CHIAKI_EXPORT void chiaki_cloud_gaikai_language(const char *locale, char *output, size_t output_size);
CHIAKI_EXPORT size_t chiaki_cloud_supported_locale_count(void);
CHIAKI_EXPORT const char *chiaki_cloud_supported_locale(size_t index);

#ifdef __cplusplus
}
#endif

#endif
