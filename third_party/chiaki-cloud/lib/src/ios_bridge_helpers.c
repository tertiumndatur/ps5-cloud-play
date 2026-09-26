// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/ios_bridge_helpers.h>
#include <stdlib.h>

CHIAKI_EXPORT size_t chiaki_session_get_sizeof(void)
{
	return sizeof(ChiakiSession);
}

CHIAKI_EXPORT ChiakiSession *chiaki_session_new_ex(void)
{
	return calloc(1, sizeof(ChiakiSession));
}

CHIAKI_EXPORT void chiaki_session_delete_ex(ChiakiSession *session)
{
	free(session);
}

CHIAKI_EXPORT void chiaki_session_set_event_cb_ex(ChiakiSession *session, ChiakiEventCallback cb, void *user)
{
	chiaki_session_set_event_cb(session, cb, user);
}

CHIAKI_EXPORT void chiaki_session_set_video_sample_cb_ex(ChiakiSession *session, ChiakiVideoSampleCallback cb, void *user)
{
	chiaki_session_set_video_sample_cb(session, cb, user);
}

CHIAKI_EXPORT void chiaki_session_set_audio_sink_ex(ChiakiSession *session, ChiakiAudioSink *sink)
{
	chiaki_session_set_audio_sink(session, sink);
}

CHIAKI_EXPORT void chiaki_session_set_haptics_sink_ex(ChiakiSession *session, ChiakiAudioSink *sink)
{
	chiaki_session_set_haptics_sink(session, sink);
}

CHIAKI_EXPORT void chiaki_session_ctrl_set_display_sink_ex(ChiakiSession *session, ChiakiCtrlDisplaySink *sink)
{
	chiaki_session_ctrl_set_display_sink(session, sink);
}

CHIAKI_EXPORT void chiaki_session_set_log_ex(ChiakiSession *session, ChiakiLog *log)
{
	session->log = log;
}

CHIAKI_EXPORT void chiaki_session_set_host_addrinfo_selected_ex(ChiakiSession *session, struct addrinfo *ai)
{
	session->connect_info.host_addrinfo_selected = ai;
}

CHIAKI_EXPORT void chiaki_session_set_enable_dualsense_ex(ChiakiSession *session, bool val)
{
	session->connect_info.enable_dualsense = val;
}

CHIAKI_EXPORT void chiaki_session_set_target_ex(ChiakiSession *session, ChiakiTarget target)
{
	session->target = target;
}

CHIAKI_EXPORT void chiaki_session_set_cloud_port_ex(ChiakiSession *session, uint16_t port)
{
	session->cloud_port = port;
}

CHIAKI_EXPORT void chiaki_session_set_cloud_psn_wrapper_type_ex(ChiakiSession *session, uint8_t type)
{
	session->cloud_psn_wrapper_type = type;
}

CHIAKI_EXPORT void chiaki_session_set_service_type_ex(ChiakiSession *session, ChiakiServiceType service_type)
{
	session->service_type = service_type;
}

CHIAKI_EXPORT void chiaki_session_get_stream_stats_ex(ChiakiSession *session, uint64_t *rtt_us,
	uint64_t *feedback_send_delay_us, uint64_t *packets_received, uint64_t *packets_lost)
{
	if(rtt_us)
		*rtt_us = session->rtt_us;
	if(feedback_send_delay_us)
		*feedback_send_delay_us = 0;
	if(packets_received)
		*packets_received = 0;
	if(packets_lost)
		*packets_lost = 0;

	ChiakiStreamConnection *connection = &session->stream_connection;
	if(packets_received && packets_lost)
		chiaki_packet_stats_get(&connection->packet_stats, false, packets_received, packets_lost);
	if(feedback_send_delay_us &&
		chiaki_mutex_lock(&connection->feedback_sender_mutex) == CHIAKI_ERR_SUCCESS)
	{
		if(connection->feedback_sender_active)
			*feedback_send_delay_us =
				chiaki_feedback_sender_get_send_delay_us(&connection->feedback_sender);
		chiaki_mutex_unlock(&connection->feedback_sender_mutex);
	}
}
