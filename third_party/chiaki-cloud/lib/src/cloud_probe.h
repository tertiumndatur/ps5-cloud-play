// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_CLOUD_PROBE_H
#define CHIAKI_CLOUD_PROBE_H

#include <chiaki/common.h>
#include <chiaki/log.h>

#include <stdint.h>

ChiakiErrorCode cloud_probe_datacenter(ChiakiLog *log, const char *host, uint16_t port,
	const char *session_key, ChiakiServiceType service, uint64_t *rtt_us,
	uint32_t *mtu_in, uint32_t *mtu_out);

#endif
