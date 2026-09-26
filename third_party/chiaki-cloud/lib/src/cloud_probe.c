// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "cloud_probe.h"

#include <chiaki/senkusha.h>
#include <chiaki/session.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

ChiakiErrorCode cloud_probe_datacenter(ChiakiLog *log, const char *host, uint16_t port,
	const char *session_key, ChiakiServiceType service, uint64_t *rtt_us,
	uint32_t *mtu_in, uint32_t *mtu_out)
{
	if(rtt_us) *rtt_us = 0;
	if(mtu_in) *mtu_in = 0;
	if(mtu_out) *mtu_out = 0;
	if(!host || !*host || port == 0)
		return CHIAKI_ERR_INVALID_DATA;

	char service_port[8];
	snprintf(service_port, sizeof(service_port), "%u", (unsigned)port);
	struct addrinfo hint;
	memset(&hint, 0, sizeof(hint));
	hint.ai_family = AF_INET;
	hint.ai_socktype = SOCK_DGRAM;
	hint.ai_protocol = IPPROTO_UDP;
	struct addrinfo *addresses = NULL;
	if(getaddrinfo(host, service_port, &hint, &addresses) != 0 || !addresses)
		return CHIAKI_ERR_HOST_DOWN;

	ChiakiSession *session = calloc(1, sizeof(*session));
	if(!session)
	{
		freeaddrinfo(addresses);
		return CHIAKI_ERR_MEMORY;
	}
	session->log = log;
	session->target = service == CHIAKI_SERVICE_TYPE_PSCLOUD ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_9;
	session->service_type = service;
	session->cloud_port = port;
	session->cloud_psn_wrapper_type = service == CHIAKI_SERVICE_TYPE_PSCLOUD ? 0 : 1;
	session->connect_info.host_addrinfo_selected = addresses;

	ChiakiSenkusha probe;
	ChiakiErrorCode error = chiaki_senkusha_init(&probe, session);
	if(error == CHIAKI_ERR_SUCCESS)
	{
		/* Datacenter measurement uses the v9 Senkusha exchange for both services.
		 * The allocated PS Cloud media stream switches to v12 later. */
		probe.wire_version = 9;
		probe.allocation_key = session_key && *session_key ? strdup(session_key) : NULL;
		uint64_t measured_rtt = 0;
		uint32_t measured_in = 0;
		uint32_t measured_out = 0;
		error = chiaki_senkusha_run(&probe, &measured_in, &measured_out, &measured_rtt, NULL);
		free(probe.allocation_key);
		probe.allocation_key = NULL;
		chiaki_senkusha_fini(&probe);
		if(error == CHIAKI_ERR_SUCCESS && measured_rtt)
		{
			if(rtt_us) *rtt_us = measured_rtt;
			if(mtu_in) *mtu_in = measured_in;
			if(mtu_out) *mtu_out = measured_out;
		}
		else if(error == CHIAKI_ERR_SUCCESS)
			error = CHIAKI_ERR_UNKNOWN;
	}
	free(session);
	freeaddrinfo(addresses);
	return error;
}
