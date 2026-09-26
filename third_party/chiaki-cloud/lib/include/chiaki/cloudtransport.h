// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_CLOUDTRANSPORT_H
#define CHIAKI_CLOUDTRANSPORT_H

#include <chiaki/common.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Selects the wire framing used by a streaming session. */
typedef enum chiaki_service_type_t
{
	CHIAKI_SERVICE_TYPE_REMOTE_PLAY = 0,
	CHIAKI_SERVICE_TYPE_PSNOW,
	CHIAKI_SERVICE_TYPE_PSCLOUD,
} ChiakiServiceType;

static inline ChiakiServiceType chiaki_service_type_normalize(ChiakiServiceType value)
{
	switch(value)
	{
		case CHIAKI_SERVICE_TYPE_PSNOW:
		case CHIAKI_SERVICE_TYPE_PSCLOUD:
			return value;
		default:
			return CHIAKI_SERVICE_TYPE_REMOTE_PLAY;
	}
}

static inline bool chiaki_service_type_is_cloud(ChiakiServiceType value)
{
	return value == CHIAKI_SERVICE_TYPE_PSNOW || value == CHIAKI_SERVICE_TYPE_PSCLOUD;
}

CHIAKI_EXPORT const char *chiaki_service_type_string(ChiakiServiceType value);

#ifdef __cplusplus
}
#endif

#endif
