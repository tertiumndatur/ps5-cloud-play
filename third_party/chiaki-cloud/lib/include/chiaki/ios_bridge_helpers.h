// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_IOS_BRIDGE_HELPERS_H
#define CHIAKI_IOS_BRIDGE_HELPERS_H

#include <chiaki/common.h>
#include <chiaki/session.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ChiakiSession is built by CMake while an iOS bridge is built by Xcode. Keep
 * field access in libchiaki so build-option-dependent struct layout never
 * crosses that ABI boundary.
 */
CHIAKI_EXPORT size_t chiaki_session_get_sizeof(void);
CHIAKI_EXPORT ChiakiSession *chiaki_session_new_ex(void);
CHIAKI_EXPORT void chiaki_session_delete_ex(ChiakiSession *session);
CHIAKI_EXPORT void chiaki_session_set_event_cb_ex(ChiakiSession *session, ChiakiEventCallback cb, void *user);
CHIAKI_EXPORT void chiaki_session_set_video_sample_cb_ex(ChiakiSession *session, ChiakiVideoSampleCallback cb, void *user);
CHIAKI_EXPORT void chiaki_session_set_audio_sink_ex(ChiakiSession *session, ChiakiAudioSink *sink);
CHIAKI_EXPORT void chiaki_session_set_haptics_sink_ex(ChiakiSession *session, ChiakiAudioSink *sink);
CHIAKI_EXPORT void chiaki_session_ctrl_set_display_sink_ex(ChiakiSession *session, ChiakiCtrlDisplaySink *sink);
CHIAKI_EXPORT void chiaki_session_set_log_ex(ChiakiSession *session, ChiakiLog *log);
CHIAKI_EXPORT void chiaki_session_set_host_addrinfo_selected_ex(ChiakiSession *session, struct addrinfo *ai);
CHIAKI_EXPORT void chiaki_session_set_enable_dualsense_ex(ChiakiSession *session, bool val);
CHIAKI_EXPORT void chiaki_session_set_target_ex(ChiakiSession *session, ChiakiTarget target);
CHIAKI_EXPORT void chiaki_session_set_cloud_port_ex(ChiakiSession *session, uint16_t port);
CHIAKI_EXPORT void chiaki_session_set_cloud_psn_wrapper_type_ex(ChiakiSession *session, uint8_t type);
CHIAKI_EXPORT void chiaki_session_set_service_type_ex(ChiakiSession *session, ChiakiServiceType service_type);
CHIAKI_EXPORT void chiaki_session_get_stream_stats_ex(ChiakiSession *session, uint64_t *rtt_us,
	uint64_t *feedback_send_delay_us, uint64_t *packets_received, uint64_t *packets_lost);

#ifdef __cplusplus
}
#endif

#endif
