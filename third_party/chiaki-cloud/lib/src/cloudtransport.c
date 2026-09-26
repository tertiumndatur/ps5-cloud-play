// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/cloudtransport.h>

const char *chiaki_service_type_string(ChiakiServiceType value)
{
	static const char *const names[] = { "remote_play", "psnow", "pscloud" };
	return names[chiaki_service_type_normalize(value)];
}
