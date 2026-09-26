// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL


#include "chiaki/common.h"
#include <chiaki/streamconnection.h>
#include <chiaki/session.h>
#include <chiaki/launchspec.h>
#include <chiaki/base64.h>
#include <chiaki/audio.h>
#include <chiaki/video.h>
#include <chiaki/time.h>

#include <string.h>
#include <inttypes.h>
#include <assert.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#endif

#include <takion.pb.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include <pb.h>

#include "utils.h"
#include "pb_utils.h"

#ifdef CHIAKI_PS5
extern void cloudplay_session_trace(int stage);
#define STREAM_TRACE(stage) cloudplay_session_trace(stage)
#define STREAM_NOINLINE __attribute__((noinline))
#else
#define STREAM_TRACE(stage) ((void)0)
#define STREAM_NOINLINE
#endif


#define STREAM_CONNECTION_CLOUD_KEY_BUF_CHUNKS 0x200 // 2 MiB

#define STREAM_CONNECTION_PORT 9296

#define EXPECT_TIMEOUT_MS 5000

#define HEARTBEAT_INTERVAL_MS 1000

static uint8_t stream_connection_wire_version(const ChiakiSession *session)
{
	if(session->service_type == CHIAKI_SERVICE_TYPE_PSCLOUD)
		return 12;
	if(session->service_type == CHIAKI_SERVICE_TYPE_PSNOW)
		return 9;
	return chiaki_target_is_ps5(session->target) ? 12 : 9;
}

typedef enum {
	STATE_IDLE,
	STATE_TAKION_CONNECT,
	STATE_EXPECT_BANG,
	STATE_EXPECT_STREAMINFO
} StreamConnectionState;

void chiaki_session_send_event(ChiakiSession *session, ChiakiEvent *event);

static void stream_connection_takion_cb(ChiakiTakionEvent *event, void *user);
static void stream_connection_takion_data(ChiakiStreamConnection *stream_connection, ChiakiTakionMessageDataType data_type, uint8_t *buf, size_t buf_size);
static void stream_connection_takion_data_protobuf(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size);
static void stream_connection_takion_data_rumble(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size);
static void stream_connection_takion_data_pad_info(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size);
static void stream_connection_takion_data_trigger_effects(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size);
static ChiakiErrorCode stream_connection_send_big(ChiakiStreamConnection *stream_connection);
static STREAM_NOINLINE ChiakiErrorCode stream_connection_send_controller_connection(ChiakiStreamConnection *stream_connection);
static STREAM_NOINLINE ChiakiErrorCode stream_connection_enable_microphone(ChiakiStreamConnection *stream_connection);
static ChiakiErrorCode stream_connection_send_disconnect(ChiakiStreamConnection *stream_connection);
static void stream_connection_takion_data_idle(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size);
static void stream_connection_takion_data_expect_bang(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size);
static void stream_connection_takion_data_expect_streaminfo(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size);
static STREAM_NOINLINE ChiakiErrorCode stream_connection_send_streaminfo_ack(ChiakiStreamConnection *stream_connection);
static void stream_connection_takion_av(ChiakiStreamConnection *stream_connection, ChiakiTakionAVPacket *packet);
static ChiakiErrorCode stream_connection_send_heartbeat(ChiakiStreamConnection *stream_connection);

CHIAKI_EXPORT ChiakiErrorCode chiaki_stream_connection_init(ChiakiStreamConnection *stream_connection, ChiakiSession *session, double packet_loss_max)
{
	stream_connection->session = session;
	stream_connection->log = session->log;
	stream_connection->packet_loss_max = packet_loss_max;

	stream_connection->measured_bitrate = 0.0;

	stream_connection->ecdh_secret = NULL;
	stream_connection->gkcrypt_remote = NULL;
	stream_connection->gkcrypt_local = NULL;
	stream_connection->streaminfo_early_buf = NULL;
	stream_connection->streaminfo_early_buf_size = 0;
	stream_connection->player_index = 0;
	memset(stream_connection->led_state, 0, sizeof(stream_connection->led_state));

	stream_connection->haptic_intensity = Strong;
	stream_connection->trigger_intensity = Strong;
	stream_connection->feedback_sender_active = false;

	ChiakiErrorCode err = chiaki_mutex_init(&stream_connection->state_mutex, false);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error;

	err = chiaki_cond_init(&stream_connection->state_cond);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error_state_mutex;

	err = chiaki_packet_stats_init(&stream_connection->packet_stats);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error_state_cond;

	stream_connection->video_receiver = NULL;
	stream_connection->audio_receiver = NULL;
	stream_connection->haptics_receiver = NULL;

	err = chiaki_mutex_init(&stream_connection->feedback_sender_mutex, false);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error_packet_stats;

	stream_connection->state = STATE_IDLE;
	stream_connection->state_finished = false;
	stream_connection->state_failed = false;
	stream_connection->should_stop = false;
	stream_connection->remote_disconnected = false;
	stream_connection->remote_disconnect_reason = NULL;

	return CHIAKI_ERR_SUCCESS;

error_packet_stats:
	chiaki_packet_stats_fini(&stream_connection->packet_stats);
error_state_cond:
	chiaki_cond_fini(&stream_connection->state_cond);
error_state_mutex:
	chiaki_mutex_fini(&stream_connection->state_mutex);
error:
	return err;
}

CHIAKI_EXPORT void chiaki_stream_connection_fini(ChiakiStreamConnection *stream_connection)
{
	free(stream_connection->remote_disconnect_reason);

	chiaki_gkcrypt_free(stream_connection->gkcrypt_remote);
	chiaki_gkcrypt_free(stream_connection->gkcrypt_local);

	free(stream_connection->ecdh_secret);
	if (stream_connection->congestion_control.thread.thread)
		chiaki_congestion_control_stop(&stream_connection->congestion_control);

	chiaki_packet_stats_fini(&stream_connection->packet_stats);

	chiaki_mutex_fini(&stream_connection->feedback_sender_mutex);

	chiaki_cond_fini(&stream_connection->state_cond);
	chiaki_mutex_fini(&stream_connection->state_mutex);
}

static bool state_finished_cond_check(void *user)
{
	ChiakiStreamConnection *stream_connection = user;
	return stream_connection->state_finished || stream_connection->should_stop || stream_connection->remote_disconnected;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_stream_connection_run(ChiakiStreamConnection *stream_connection, chiaki_socket_t *socket)
{
	STREAM_TRACE(300);
	ChiakiSession *session = stream_connection->session;
	ChiakiErrorCode err;

	ChiakiTakionConnectInfo takion_info = { 0 };
	takion_info.log = stream_connection->log;
	takion_info.disable_audio_video = stream_connection->session->connect_info.disable_audio_video;
	takion_info.close_socket = true;
	if(!socket)
	{
		takion_info.sa_len = session->connect_info.host_addrinfo_selected->ai_addrlen;
		takion_info.sa = malloc(takion_info.sa_len);
		if(!takion_info.sa)
			return CHIAKI_ERR_MEMORY;
		memcpy(takion_info.sa, session->connect_info.host_addrinfo_selected->ai_addr, takion_info.sa_len);
		uint16_t port = chiaki_service_type_is_cloud(session->service_type)
			? session->cloud_port : STREAM_CONNECTION_PORT;
		err = set_port(takion_info.sa, htons(port));
		assert(err == CHIAKI_ERR_SUCCESS);
	}
	takion_info.ip_dontfrag = session->dontfrag;

	takion_info.enable_crypt = true;
	takion_info.enable_dualsense = session->connect_info.enable_dualsense;
	takion_info.protocol_version = stream_connection_wire_version(session);
	takion_info.route = session->service_type;
	takion_info.outbound_prefix = chiaki_service_type_is_cloud(session->service_type) ? session->cloud_psn_wrapper_type : 0;
	takion_info.probe_connection = false;
	CHIAKI_LOGI(session->log, "Takion stream route=%s port=%u version=%u",
		chiaki_service_type_string(session->service_type),
		chiaki_service_type_is_cloud(session->service_type) ? session->cloud_port : STREAM_CONNECTION_PORT,
		takion_info.protocol_version);

	takion_info.cb = stream_connection_takion_cb;
	takion_info.cb_user = stream_connection;

	err = chiaki_mutex_lock(&stream_connection->state_mutex);
	assert(err == CHIAKI_ERR_SUCCESS);
	STREAM_TRACE(301);

#define CHECK_STOP(quit_label) do { \
	if(stream_connection->should_stop) \
	{ \
		goto quit_label; \
	} } while(0)

	STREAM_TRACE(310);
	stream_connection->audio_receiver = chiaki_audio_receiver_new(session, &stream_connection->packet_stats);
	if(!stream_connection->audio_receiver)
	{
		CHIAKI_LOGE(session->log, "StreamConnection failed to initialize Audio Receiver");
		if(!socket)
			free(takion_info.sa);
		chiaki_mutex_unlock(&stream_connection->state_mutex);
		return CHIAKI_ERR_UNKNOWN;
	}
	STREAM_TRACE(311);

	STREAM_TRACE(312);
	stream_connection->haptics_receiver = chiaki_audio_receiver_new(session, NULL);
	if(!stream_connection->haptics_receiver)
	{
		CHIAKI_LOGE(session->log, "StreamConnection failed to initialize Haptics Receiver");
		err = CHIAKI_ERR_UNKNOWN;
		chiaki_mutex_unlock(&stream_connection->state_mutex);
		goto err_audio_receiver;
	}
	STREAM_TRACE(313);

	STREAM_TRACE(314);
	stream_connection->video_receiver = chiaki_video_receiver_new(session, &stream_connection->packet_stats);
	if(!stream_connection->video_receiver)
	{
		CHIAKI_LOGE(session->log, "StreamConnection failed to initialize Video Receiver");
		err = CHIAKI_ERR_UNKNOWN;
		chiaki_mutex_unlock(&stream_connection->state_mutex);
		goto err_haptics_receiver;
	}
	STREAM_TRACE(315);

	stream_connection->state = STATE_TAKION_CONNECT;
	stream_connection->state_finished = false;
	stream_connection->state_failed = false;
	STREAM_TRACE(320);
	err = chiaki_takion_connect(&stream_connection->takion, &takion_info, socket);

	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(session->log, "StreamConnection connect failed");
		chiaki_mutex_unlock(&stream_connection->state_mutex);
		goto err_video_receiver;
	}
	STREAM_TRACE(321);

	STREAM_TRACE(330);
	err = chiaki_congestion_control_start(&stream_connection->congestion_control, &stream_connection->takion, &stream_connection->packet_stats, stream_connection->packet_loss_max);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(session->log, "StreamConnection failed to start Congestion Control");
		goto close_takion;
	}
	STREAM_TRACE(331);

	STREAM_TRACE(340);
	err = chiaki_cond_timedwait_pred(&stream_connection->state_cond, &stream_connection->state_mutex, EXPECT_TIMEOUT_MS, state_finished_cond_check, stream_connection);
	assert(err == CHIAKI_ERR_SUCCESS || err == CHIAKI_ERR_TIMEOUT);
	CHECK_STOP(close_takion);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(session->log, "StreamConnection Takion connect failed");
		goto err_congestion_control;
	}
	STREAM_TRACE(341);

	CHIAKI_LOGI(session->log, "StreamConnection sending big");

	stream_connection->state = STATE_EXPECT_BANG;
	stream_connection->state_finished = false;
	stream_connection->state_failed = false;
	STREAM_TRACE(350);
	err = stream_connection_send_big(stream_connection);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(session->log, "StreamConnection failed to send big");
		goto disconnect;
	}
	STREAM_TRACE(351);

	STREAM_TRACE(360);
	err = chiaki_cond_timedwait_pred(&stream_connection->state_cond, &stream_connection->state_mutex, EXPECT_TIMEOUT_MS, state_finished_cond_check, stream_connection);
	assert(err == CHIAKI_ERR_SUCCESS || err == CHIAKI_ERR_TIMEOUT);
	CHECK_STOP(disconnect);

	if(!stream_connection->state_finished)
	{
		if(err == CHIAKI_ERR_TIMEOUT)
			CHIAKI_LOGE(session->log, "StreamConnection bang receive timeout");

		CHIAKI_LOGE(session->log, "StreamConnection didn't receive bang or failed to handle it");
		err = CHIAKI_ERR_UNKNOWN;
		goto disconnect;
	}
	STREAM_TRACE(361);
	CHIAKI_LOGI(session->log, "StreamConnection successfully received bang");
	stream_connection->state = STATE_EXPECT_STREAMINFO;
	stream_connection->state_finished = false;
	stream_connection->state_failed = false;
	if(stream_connection->streaminfo_early_buf)
	{
		stream_connection_takion_data_expect_streaminfo(stream_connection, stream_connection->streaminfo_early_buf, stream_connection->streaminfo_early_buf_size);
		free(stream_connection->streaminfo_early_buf);
		stream_connection->streaminfo_early_buf = NULL;
	}
	if(!stream_connection->state_finished)
	{
		STREAM_TRACE(370);
		err = chiaki_cond_timedwait_pred(&stream_connection->state_cond, &stream_connection->state_mutex, EXPECT_TIMEOUT_MS, state_finished_cond_check, stream_connection);
	}
	assert(err == CHIAKI_ERR_SUCCESS || err == CHIAKI_ERR_TIMEOUT);
	CHECK_STOP(disconnect);

	if(!stream_connection->state_finished)
	{
		if(err == CHIAKI_ERR_TIMEOUT)
			CHIAKI_LOGE(session->log, "StreamConnection streaminfo receive timeout");

		CHIAKI_LOGE(session->log, "StreamConnection didn't receive streaminfo");
		err = CHIAKI_ERR_UNKNOWN;
		goto disconnect;
	}
	STREAM_TRACE(371);

	CHIAKI_LOGI(session->log, "StreamConnection successfully received streaminfo");

	err = chiaki_mutex_lock(&stream_connection->feedback_sender_mutex);
	assert(err == CHIAKI_ERR_SUCCESS);
	err = chiaki_feedback_sender_init(&stream_connection->feedback_sender, &stream_connection->takion);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		chiaki_mutex_unlock(&stream_connection->feedback_sender_mutex);
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to start Feedback Sender");
		goto disconnect;
	}
	stream_connection->feedback_sender_active = true;
	chiaki_feedback_sender_set_controller_state(&stream_connection->feedback_sender, &session->controller_state);
	chiaki_mutex_unlock(&stream_connection->feedback_sender_mutex);

	stream_connection->state = STATE_IDLE;
	stream_connection->state_finished = false;
	stream_connection->state_failed = false;

	ChiakiEvent event = { 0 };
	event.type = CHIAKI_EVENT_CONNECTED;
	chiaki_mutex_unlock(&stream_connection->state_mutex);
	chiaki_session_send_event(session, &event);
	err = chiaki_mutex_lock(&stream_connection->state_mutex);
	assert(err == CHIAKI_ERR_SUCCESS);

	while(true)
	{
		err = chiaki_cond_timedwait_pred(&stream_connection->state_cond, &stream_connection->state_mutex, HEARTBEAT_INTERVAL_MS, state_finished_cond_check, stream_connection);
		if(err != CHIAKI_ERR_TIMEOUT)
			break;

		err = stream_connection_send_heartbeat(stream_connection);
		if(err != CHIAKI_ERR_SUCCESS)
			CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to send heartbeat");
		else
			CHIAKI_LOGV(stream_connection->log, "StreamConnection sent heartbeat");
	}

	err = chiaki_mutex_lock(&stream_connection->feedback_sender_mutex);
	assert(err == CHIAKI_ERR_SUCCESS);
	stream_connection->feedback_sender_active = false;
	chiaki_feedback_sender_fini(&stream_connection->feedback_sender);
	chiaki_mutex_unlock(&stream_connection->feedback_sender_mutex);

	err = CHIAKI_ERR_SUCCESS;

disconnect:
	CHIAKI_LOGI(session->log, "StreamConnection is disconnecting");
	if(stream_connection->streaminfo_early_buf)
	{
		free(stream_connection->streaminfo_early_buf);
		stream_connection->streaminfo_early_buf = NULL;
	}
	stream_connection_send_disconnect(stream_connection);

	if(stream_connection->should_stop)
	{
		CHIAKI_LOGI(stream_connection->log, "StreamConnection was requested to stop");
		err = CHIAKI_ERR_CANCELED;
	}
	else if(stream_connection->remote_disconnected)
	{
		CHIAKI_LOGI(stream_connection->log, "StreamConnection closing after Remote disconnected");
		err = CHIAKI_ERR_DISCONNECTED;
	}

err_congestion_control:
	chiaki_congestion_control_stop(&stream_connection->congestion_control);

close_takion:
	chiaki_mutex_unlock(&stream_connection->state_mutex);

	chiaki_takion_close(&stream_connection->takion);
	CHIAKI_LOGI(session->log, "StreamConnection closed takion");

err_video_receiver:
	chiaki_mutex_lock(&stream_connection->state_mutex);
	chiaki_video_receiver_free(stream_connection->video_receiver);
	stream_connection->video_receiver = NULL;
	chiaki_mutex_unlock(&stream_connection->state_mutex);

err_haptics_receiver:
	chiaki_mutex_lock(&stream_connection->state_mutex);
	chiaki_audio_receiver_free(stream_connection->haptics_receiver);
	stream_connection->haptics_receiver = NULL;
	chiaki_mutex_unlock(&stream_connection->state_mutex);

err_audio_receiver:
	chiaki_mutex_lock(&stream_connection->state_mutex);
	chiaki_audio_receiver_free(stream_connection->audio_receiver);
	stream_connection->audio_receiver = NULL;
	chiaki_mutex_unlock(&stream_connection->state_mutex);
	if(!socket)
		free(takion_info.sa);
	return err;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_stream_connection_stop(ChiakiStreamConnection *stream_connection)
{
	ChiakiErrorCode err = chiaki_mutex_lock(&stream_connection->state_mutex);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;
	stream_connection->should_stop = true;
	ChiakiErrorCode unlock_err = chiaki_mutex_unlock(&stream_connection->state_mutex);
	err = chiaki_cond_signal(&stream_connection->state_cond);
	return err == CHIAKI_ERR_SUCCESS ? unlock_err : err;
}

static void stream_connection_takion_cb(ChiakiTakionEvent *event, void *user)
{
	ChiakiStreamConnection *stream_connection = user;
	switch(event->type)
	{
		case CHIAKI_TAKION_EVENT_TYPE_CONNECTED:
		case CHIAKI_TAKION_EVENT_TYPE_DISCONNECT:
			chiaki_mutex_lock(&stream_connection->state_mutex);
			if(stream_connection->state == STATE_TAKION_CONNECT)
			{
				stream_connection->state_finished = event->type == CHIAKI_TAKION_EVENT_TYPE_CONNECTED;
				stream_connection->state_failed = event->type == CHIAKI_TAKION_EVENT_TYPE_DISCONNECT;
				chiaki_cond_signal(&stream_connection->state_cond);
			}
			chiaki_mutex_unlock(&stream_connection->state_mutex);
			break;
		case CHIAKI_TAKION_EVENT_TYPE_DATA:
			stream_connection_takion_data(stream_connection, event->data.data_type, event->data.buf, event->data.buf_size);
			break;
		case CHIAKI_TAKION_EVENT_TYPE_AV:
			stream_connection_takion_av(stream_connection, event->av);
			break;
		default:
			break;
	}
}

static void stream_connection_takion_data(ChiakiStreamConnection *stream_connection, ChiakiTakionMessageDataType data_type, uint8_t *buf, size_t buf_size)
{
	switch(data_type)
	{
		case CHIAKI_TAKION_MESSAGE_DATA_TYPE_PROTOBUF:
			stream_connection_takion_data_protobuf(stream_connection, buf, buf_size);
			break;
		case CHIAKI_TAKION_MESSAGE_DATA_TYPE_RUMBLE:
			stream_connection_takion_data_rumble(stream_connection, buf, buf_size);
			break;
		case CHIAKI_TAKION_MESSAGE_DATA_TYPE_PAD_INFO:
			stream_connection_takion_data_pad_info(stream_connection, buf, buf_size);
			break;
		case CHIAKI_TAKION_MESSAGE_DATA_TYPE_TRIGGER_EFFECTS:
			stream_connection_takion_data_trigger_effects(stream_connection, buf, buf_size);
			break;
		default:
			break;
	}
}

static void stream_connection_takion_data_protobuf(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size)
{
	STREAM_TRACE(500);
	chiaki_mutex_lock(&stream_connection->state_mutex);
	STREAM_TRACE(501);
	switch(stream_connection->state)
	{
		case STATE_EXPECT_BANG:
			STREAM_TRACE(502);
			stream_connection_takion_data_expect_bang(stream_connection, buf, buf_size);
			break;
		case STATE_EXPECT_STREAMINFO:
			STREAM_TRACE(503);
			stream_connection_takion_data_expect_streaminfo(stream_connection, buf, buf_size);
			break;
		default: // STATE_IDLE
			STREAM_TRACE(504);
			stream_connection_takion_data_idle(stream_connection, buf, buf_size);
			break;
	}
	chiaki_mutex_unlock(&stream_connection->state_mutex);
	STREAM_TRACE(505);
}

static void stream_connection_takion_data_rumble(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size)
{
	if(buf_size < 3)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection got rumble packet with size %#llx < 3",
				(unsigned long long)buf_size);
		return;
	}
	ChiakiEvent event = { 0 };
	event.type = CHIAKI_EVENT_RUMBLE;
	event.rumble.unknown = buf[0];
	event.rumble.left = buf[1];
	event.rumble.right = buf[2];
	chiaki_session_send_event(stream_connection->session, &event);
}


static void stream_connection_takion_data_trigger_effects(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size)
{
	if(buf_size < 25)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection got trigger effects packet with size %#llx < 25",
				(unsigned long long)buf_size);
		return;
	}
	ChiakiEvent event = { 0 };
	event.type = CHIAKI_EVENT_TRIGGER_EFFECTS;
	event.trigger_effects.type_left = buf[1];
	event.trigger_effects.type_right = buf[2];
	memcpy(&event.trigger_effects.left, buf + 5, 10);
	memcpy(&event.trigger_effects.right, buf + 15, 10);
	chiaki_session_send_event(stream_connection->session, &event);
}

static char* DualSenseIntensity(ChiakiDualSenseEffectIntensity intensity)
{
	switch(intensity)
	{
		case Strong:
			return "Strong";
			break;
		case Weak:
			return "Weak";
			break;
		case Medium:
			return "Medium";
			break;
		case Off:
			return "Off";
			break;
		default:
			return "Invalid";
			break;
	}
}
static void stream_connection_takion_data_pad_info(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size)
{
	bool led_changed = false;
	bool player_index_changed = false;
	bool motion_reset = false;
	bool haptic_intensity_changed = false;
	bool trigger_intensity_changed = false;

	CHIAKI_LOGV(stream_connection->log, "Pad info packet: ");
	chiaki_log_hexdump(stream_connection->log, CHIAKI_LOG_VERBOSE, buf, buf_size);

	switch(buf_size)
	{
		case 0x7:
		{
			/* PS Now's PS4 hosts send the legacy pad-info prefix only.  It
			 * contains the same player, LED and motion-reset fields as the
			 * 0x11 packet, without DualSense intensity fields. */
			if(buf[4])
				motion_reset = true;
			if(buf[0] != stream_connection->player_index)
			{
				player_index_changed = true;
				stream_connection->player_index = buf[0];
			}
			if(memcmp(buf + 1, stream_connection->led_state, 3) != 0)
			{
				led_changed = true;
				memcpy(stream_connection->led_state, buf + 1, 3);
			}
			break;
		}
		case 0x19:
		{
			// sequence number of feedback packet this is responding to
			uint16_t feedback_packet_seq_num = ntohs(*(chiaki_unaligned_uint16_t *)(buf));
			// int16_t unknown = ntohs(*(chiaki_unaligned_uint16_t *)(buf + 2));
			uint32_t timestamp = ntohs(*(chiaki_unaligned_uint32_t *)(buf + 4));
			if(stream_connection->haptic_intensity != buf[20])
			{
				stream_connection->haptic_intensity = buf[20];
				haptic_intensity_changed = true;
			}
			if(stream_connection->trigger_intensity != buf[21])
			{
				stream_connection->trigger_intensity = buf[21];
				trigger_intensity_changed = true;
			}
			if(buf[12])
			{
				motion_reset = true;
				CHIAKI_LOGV(stream_connection->log, "StreamConnection received motion reset request in response to feedback packet with seqnum %"PRIu16"x , %"PRIu32" seconds after stream began", feedback_packet_seq_num, timestamp);
			}
			if(buf[8] != stream_connection->player_index)
			{
				player_index_changed = true;
				stream_connection->player_index = buf[8];
			}
			if(memcmp(buf + 9, stream_connection->led_state, 3) != 0)
			{
				led_changed = true;
				memcpy(stream_connection->led_state, buf + 9, 3);
			}
			break;
		}
		case 0x11:
		{
			if(stream_connection->haptic_intensity != buf[12])
			{
				stream_connection->haptic_intensity = buf[12];
				haptic_intensity_changed = true;
			}
			if(stream_connection->trigger_intensity != buf[13])
			{
				stream_connection->trigger_intensity = buf[13];
				trigger_intensity_changed = true;
			}
			if(buf[4])
			{
				motion_reset = true;
			}
			if(buf[0] != stream_connection->player_index)
			{
				player_index_changed = true;
				stream_connection->player_index = buf[0];
			}
			if(memcmp(buf + 1, stream_connection->led_state, 3) != 0)
			{
				led_changed = true;
				memcpy(stream_connection->led_state, buf + 1, 3);
			}
			break;
		}
		default:
		{
			CHIAKI_LOGE(stream_connection->log, "StreamConnection got pad info with unsupported size %#llx",
					(unsigned long long)buf_size);
			return;
		}
	}
	if(motion_reset)
	{
		CHIAKI_LOGI(stream_connection->log, "Setting motion control origin to current position");
		ChiakiEvent event = { 0 };
		event.type = CHIAKI_EVENT_MOTION_RESET;
		chiaki_session_send_event(stream_connection->session, &event);
	}
	if(haptic_intensity_changed)
	{
		CHIAKI_LOGI(stream_connection->log, "Set haptic intensity to: %s", DualSenseIntensity(stream_connection->haptic_intensity));
		ChiakiEvent event = { 0 };
		event.type = CHIAKI_EVENT_HAPTIC_INTENSITY;
		event.intensity = stream_connection->haptic_intensity;
		chiaki_session_send_event(stream_connection->session, &event);
	}
	if(trigger_intensity_changed)
	{
		CHIAKI_LOGI(stream_connection->log, "Set adaptive trigger intensity to: %s", DualSenseIntensity(stream_connection->trigger_intensity));
		ChiakiEvent event = { 0 };
		event.type = CHIAKI_EVENT_TRIGGER_INTENSITY;
		event.intensity = stream_connection->trigger_intensity;
		chiaki_session_send_event(stream_connection->session, &event);
	}
	if(led_changed)
	{
		CHIAKI_LOGV(stream_connection->log, "Set LED state to - red: %x, green: %x, blue: %x", stream_connection->led_state[0], stream_connection->led_state[1], stream_connection->led_state[2]);
		ChiakiEvent event = { 0 };
		event.type = CHIAKI_EVENT_LED_COLOR;
		memcpy(event.led_state, stream_connection->led_state, 3);
		chiaki_session_send_event(stream_connection->session, &event);
	}
	if(player_index_changed)
	{
		CHIAKI_LOGV(stream_connection->log, "Set player index to - %d", stream_connection->player_index);
		ChiakiEvent event = { 0 };
		event.type = CHIAKI_EVENT_PLAYER_INDEX;
		event.player_index = stream_connection->player_index;
		chiaki_session_send_event(stream_connection->session, &event);
	}
}

static void stream_connection_takion_data_handle_disconnect(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size)
{
	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));

	char reason[256];
	ChiakiPBDecodeBuf decode_buf;
	decode_buf.size = 0;
	decode_buf.max_size = sizeof(reason) - 1;
	decode_buf.buf = (uint8_t *)reason;
	msg.disconnect_payload.reason.arg = &decode_buf;
	msg.disconnect_payload.reason.funcs.decode = chiaki_pb_decode_buf;

	pb_istream_t stream = pb_istream_from_buffer(buf, buf_size);
	bool r = pb_decode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!r)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to decode data protobuf");
		chiaki_log_hexdump(stream_connection->log, CHIAKI_LOG_ERROR, buf, buf_size);
		return;
	}

	reason[decode_buf.size] = '\0';
	CHIAKI_LOGI(stream_connection->log, "Remote disconnected from StreamConnection with reason \"%s\"", reason);

	stream_connection->remote_disconnected = true;
	free(stream_connection->remote_disconnect_reason);
	stream_connection->remote_disconnect_reason = strdup(reason);
	chiaki_cond_signal(&stream_connection->state_cond);
}

static void stream_connection_takion_data_idle(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size)
{
	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));

	pb_istream_t stream = pb_istream_from_buffer(buf, buf_size);
	bool r = pb_decode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!r)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to decode data protobuf");
		chiaki_log_hexdump(stream_connection->log, CHIAKI_LOG_ERROR, buf, buf_size);
		return;
	}

	CHIAKI_LOGV(stream_connection->log, "StreamConnection received data with msg.type == %d", msg.type);
	chiaki_log_hexdump(stream_connection->log, CHIAKI_LOG_VERBOSE, buf, buf_size);

	switch (msg.type)
	{
	case tkproto_TakionMessage_PayloadType_DISCONNECT:
		stream_connection_takion_data_handle_disconnect(stream_connection, buf, buf_size);
		break;
	case tkproto_TakionMessage_PayloadType_CONNECTIONQUALITY:
	{
		tkproto_ConnectionQualityPayload q = msg.connection_quality_payload;
		CHIAKI_LOGV(
			stream_connection->log,
			"StreamConnection received connection quality: target_bitrate=%d, "
			"upstream_bitrate=%d, upstream_loss=%.4f, "
			"disable_upstream_audio=%d, rtt=%.4f, loss=%lld",
			 q.target_bitrate, q.upstream_bitrate,
			 q.upstream_loss,
			 q.disable_upstream_audio, q.rtt, q.loss);
		stream_connection->measured_bitrate = chiaki_stream_stats_bitrate(&stream_connection->video_receiver->frame_processor.stream_stats, stream_connection->session->connect_info.video_profile.max_fps) / 1000000.0;
		CHIAKI_LOGV(stream_connection->log, "StreamConnection measured bitrate: %.4f MBit/s", stream_connection->measured_bitrate);

		chiaki_stream_stats_reset(&stream_connection->video_receiver->frame_processor.stream_stats);
		break;
	}
	case tkproto_TakionMessage_PayloadType_CORRUPTFRAME:
		CHIAKI_LOGE(stream_connection->log, "StreamConnection received corrupt frame from %d to %d",
			msg.corrupt_payload.start, msg.corrupt_payload.end);
		break;
	case tkproto_TakionMessage_PayloadType_STREAMINFOACK:
		CHIAKI_LOGV(stream_connection->log, "StreamConnection received streaminfo ack");
		break;
	default:
		break;
	}
}

static ChiakiErrorCode stream_connection_init_crypt(ChiakiStreamConnection *stream_connection)
{
	ChiakiSession *session = stream_connection->session;
	size_t key_buf_chunks = chiaki_service_type_is_cloud(session->service_type)
		? STREAM_CONNECTION_CLOUD_KEY_BUF_CHUNKS
		: CHIAKI_GKCRYPT_KEY_BUF_BLOCKS_DEFAULT;

	stream_connection->gkcrypt_local = chiaki_gkcrypt_new(stream_connection->log, key_buf_chunks, 2, session->handshake_key, stream_connection->ecdh_secret);
	if(!stream_connection->gkcrypt_local)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to initialize local GKCrypt with index 2");
		return CHIAKI_ERR_UNKNOWN;
	}
	stream_connection->gkcrypt_remote = chiaki_gkcrypt_new(stream_connection->log, key_buf_chunks, 3, session->handshake_key, stream_connection->ecdh_secret);
	if(!stream_connection->gkcrypt_remote)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to initialize remote GKCrypt with index 3");
		free(stream_connection->gkcrypt_local);
		stream_connection->gkcrypt_local = NULL;
		return CHIAKI_ERR_UNKNOWN;
	}

	chiaki_takion_set_crypt(&stream_connection->takion, stream_connection->gkcrypt_local, stream_connection->gkcrypt_remote);

	return CHIAKI_ERR_SUCCESS;
}

static void stream_connection_takion_data_expect_bang(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size)
{
	char ecdh_pub_key[128];
	ChiakiPBDecodeBuf ecdh_pub_key_buf = { sizeof(ecdh_pub_key), 0, (uint8_t *)ecdh_pub_key };
	char ecdh_sig[32];
	ChiakiPBDecodeBuf ecdh_sig_buf = { sizeof(ecdh_sig), 0, (uint8_t *)ecdh_sig };

	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));

	msg.bang_payload.ecdh_pub_key.arg = &ecdh_pub_key_buf;
	msg.bang_payload.ecdh_pub_key.funcs.decode = chiaki_pb_decode_buf;
	msg.bang_payload.ecdh_sig.arg = &ecdh_sig_buf;
	msg.bang_payload.ecdh_sig.funcs.decode = chiaki_pb_decode_buf;

	pb_istream_t stream = pb_istream_from_buffer(buf, buf_size);
	bool r = pb_decode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!r)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to decode data protobuf");
		return;
	}

	if(msg.type != tkproto_TakionMessage_PayloadType_BANG || !msg.has_bang_payload)
	{
		if(msg.type == tkproto_TakionMessage_PayloadType_DISCONNECT)
		{
			stream_connection_takion_data_handle_disconnect(stream_connection, buf, buf_size);
			return;
		}
		if(msg.type == tkproto_TakionMessage_PayloadType_STREAMINFO)
		{
			if(!stream_connection->streaminfo_early_buf)
			{
				stream_connection->streaminfo_early_buf = malloc(buf_size);
				memcpy(stream_connection->streaminfo_early_buf, buf, buf_size);
				stream_connection->streaminfo_early_buf_size = buf_size;
				CHIAKI_LOGI(stream_connection->log, "StreamConnection received streaminfo early, saving ...");
				return;
			}

		}
		CHIAKI_LOGW(stream_connection->log, "StreamConnection expected bang payload but received something else: %d", msg.type);
		chiaki_log_hexdump(stream_connection->log, CHIAKI_LOG_WARNING, buf, buf_size);
		return;
	}

	CHIAKI_LOGI(stream_connection->log, "BANG received: server_version=%u, token=%u, encrypted_key_accepted=%d, version_accepted=%d",
		msg.bang_payload.server_version, msg.bang_payload.token,
		msg.bang_payload.encrypted_key_accepted, msg.bang_payload.version_accepted);

	if(!msg.bang_payload.version_accepted)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection bang remote didn't accept version (sent client_version=%u, server reports server_version=%u)",
			stream_connection->takion.version, msg.bang_payload.server_version);
		goto error;
	}

	if(!msg.bang_payload.encrypted_key_accepted)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection bang remote didn't accept encrypted key");
		goto error;
	}

	if(!ecdh_pub_key_buf.size)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection didn't get remote ECDH pub key from bang");
		goto error;
	}

	if(!ecdh_sig_buf.size)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection didn't get remote ECDH sig from bang");
		goto error;
	}

	assert(!stream_connection->ecdh_secret);
	stream_connection->ecdh_secret = malloc(CHIAKI_ECDH_SECRET_SIZE);
	if(!stream_connection->ecdh_secret)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to alloc ECDH secret memory");
		goto error;
	}

	ChiakiErrorCode err = chiaki_ecdh_derive_secret(&stream_connection->session->ecdh,
			stream_connection->ecdh_secret,
			ecdh_pub_key_buf.buf, ecdh_pub_key_buf.size,
			stream_connection->session->handshake_key,
			ecdh_sig_buf.buf, ecdh_sig_buf.size);

	if(err != CHIAKI_ERR_SUCCESS)
	{
		free(stream_connection->ecdh_secret);
		stream_connection->ecdh_secret = NULL;
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to derive secret from bang");
		goto error;
	}

	err = stream_connection_init_crypt(stream_connection);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to init crypt after receiving bang");
		goto error;
	}

	// stream_connection->state_mutex is expected to be locked by the caller of this function
	stream_connection->state_finished = true;
	chiaki_cond_signal(&stream_connection->state_cond);
	return;
error:
	stream_connection->state_failed = true;
	chiaki_cond_signal(&stream_connection->state_cond);
}

typedef struct decode_resolutions_context_t
{
	ChiakiStreamConnection *stream_connection;
	ChiakiVideoProfile video_profiles[CHIAKI_VIDEO_PROFILES_MAX];
	size_t video_profiles_count;
} DecodeResolutionsContext;

static bool pb_decode_resolution(pb_istream_t *stream, const pb_field_t *field, void **arg)
{
	(void)field;
	DecodeResolutionsContext *ctx = *arg;
	STREAM_TRACE(530);
	if(!ctx || !ctx->stream_connection || ctx->video_profiles_count >= CHIAKI_VIDEO_PROFILES_MAX)
	{
		if(ctx && ctx->stream_connection)
			CHIAKI_LOGE(ctx->stream_connection->session->log, "Received more resolutions than the maximum");
		return false;
	}

	tkproto_ResolutionPayload resolution = { 0 };
	ChiakiPBDecodeBufAlloc header_buf = { 0 };
	resolution.video_header.arg = &header_buf;
	resolution.video_header.funcs.decode = chiaki_pb_decode_buf_alloc;
	if(!pb_decode(stream, tkproto_ResolutionPayload_fields, &resolution))
		return false;
	STREAM_TRACE(531);

	if(!header_buf.buf || header_buf.size == 0 || resolution.width < 160 || resolution.height < 90
			|| resolution.width > 7680 || resolution.height > 4320)
	{
		free(header_buf.buf);
		CHIAKI_LOGE(ctx->stream_connection->session->log,
			"Invalid video profile in streaminfo: %ux%u header=%zu",
			(unsigned int)resolution.width, (unsigned int)resolution.height, header_buf.size);
		return false;
	}

	if(header_buf.size > SIZE_MAX - CHIAKI_VIDEO_BUFFER_PADDING_SIZE)
	{
		free(header_buf.buf);
		return false;
	}
	uint8_t *header_buf_padded = realloc(header_buf.buf, header_buf.size + CHIAKI_VIDEO_BUFFER_PADDING_SIZE);
	if(!header_buf_padded)
	{
		free(header_buf.buf);
		CHIAKI_LOGE(ctx->stream_connection->session->log, "Failed to realloc video header with padding");
		return false;
	}
	memset(header_buf_padded + header_buf.size, 0, CHIAKI_VIDEO_BUFFER_PADDING_SIZE);

	ChiakiVideoProfile *profile = &ctx->video_profiles[ctx->video_profiles_count++];
	profile->width = resolution.width;
	profile->height = resolution.height;
	profile->header_sz = header_buf.size;
	profile->header = header_buf_padded;
	STREAM_TRACE(532);

	return true;
}

static void stream_connection_free_decoded_profiles(DecodeResolutionsContext *ctx)
{
	if(!ctx)
		return;
	for(size_t i = 0; i < ctx->video_profiles_count; i++)
	{
		free(ctx->video_profiles[i].header);
		ctx->video_profiles[i].header = NULL;
		ctx->video_profiles[i].header_sz = 0;
	}
	ctx->video_profiles_count = 0;
}

static void stream_connection_takion_data_expect_streaminfo(ChiakiStreamConnection *stream_connection, uint8_t *buf, size_t buf_size)
{
	STREAM_TRACE(510);
	CHIAKI_LOGI(stream_connection->log, "StreamConnection processing streaminfo (%zu bytes)", buf_size);
	/* Keep the generated protobuf object and decoded profiles off the Takion
	 * thread stack.  The PS5 pthread shim uses a small default stack and the
	 * optimized version of this function previously needed almost 5 KiB before
	 * nanopb, logging, Opus and send calls used their own frames. */
	tkproto_TakionMessage *msg = calloc(1, sizeof(*msg));
	DecodeResolutionsContext *decode_resolutions_context = calloc(1, sizeof(*decode_resolutions_context));
	if(!msg || !decode_resolutions_context)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection could not allocate streaminfo decode state");
		free(msg);
		free(decode_resolutions_context);
		goto error;
	}
	STREAM_TRACE(511);

	uint8_t audio_header[CHIAKI_AUDIO_HEADER_SIZE];
	ChiakiPBDecodeBuf audio_header_buf = { sizeof(audio_header), 0, (uint8_t *)audio_header };
	msg->stream_info_payload.audio_header.arg = &audio_header_buf;
	msg->stream_info_payload.audio_header.funcs.decode = chiaki_pb_decode_buf;

	decode_resolutions_context->stream_connection = stream_connection;
	msg->stream_info_payload.resolution.arg = decode_resolutions_context;
	msg->stream_info_payload.resolution.funcs.decode = pb_decode_resolution;

	pb_istream_t stream = pb_istream_from_buffer(buf, buf_size);
	STREAM_TRACE(512);
	bool r = pb_decode(&stream, tkproto_TakionMessage_fields, msg);
	if(!r)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to decode streaminfo protobuf: %s", PB_GET_ERROR(&stream));
		goto error_allocated;
	}
	STREAM_TRACE(513);

	if(msg->type != tkproto_TakionMessage_PayloadType_STREAMINFO || !msg->has_stream_info_payload)
	{
		if(msg->type == tkproto_TakionMessage_PayloadType_DISCONNECT)
		{
			stream_connection_takion_data_handle_disconnect(stream_connection, buf, buf_size);
			stream_connection_free_decoded_profiles(decode_resolutions_context);
			free(decode_resolutions_context);
			free(msg);
			return;
		}

		CHIAKI_LOGW(stream_connection->log, "StreamConnection expected streaminfo payload but received type %d", msg->type);
		chiaki_log_hexdump(stream_connection->log, CHIAKI_LOG_WARNING, buf, buf_size);
		goto error_allocated;
	}

	CHIAKI_LOGI(stream_connection->log, "StreamConnection received audio header:");
	chiaki_log_hexdump(stream_connection->log, CHIAKI_LOG_DEBUG, audio_header, audio_header_buf.size);

	if(audio_header_buf.size != CHIAKI_AUDIO_HEADER_SIZE || decode_resolutions_context->video_profiles_count == 0)
	{
		CHIAKI_LOGE(stream_connection->log,
			"StreamConnection received incomplete streaminfo: audio=%zu profiles=%zu",
			audio_header_buf.size, decode_resolutions_context->video_profiles_count);
		goto error_allocated;
	}
	STREAM_TRACE(514);

	ChiakiAudioHeader audio_header_s;
	chiaki_audio_header_load(&audio_header_s, audio_header);
	if(audio_header_s.channels == 0 || audio_header_s.channels > 8 || audio_header_s.bits == 0
			|| audio_header_s.bits > 32 || audio_header_s.rate < 8000 || audio_header_s.rate > 192000
			|| audio_header_s.frame_size == 0 || audio_header_s.frame_size > 5760)
	{
		CHIAKI_LOGE(stream_connection->log,
			"StreamConnection rejected invalid audio header: channels=%u bits=%u rate=%u frame=%u",
			(unsigned int)audio_header_s.channels, (unsigned int)audio_header_s.bits,
			(unsigned int)audio_header_s.rate, (unsigned int)audio_header_s.frame_size);
		goto error_allocated;
	}
	STREAM_TRACE(515);
	STREAM_TRACE(516);
	chiaki_audio_receiver_stream_info(stream_connection->audio_receiver, &audio_header_s);
	STREAM_TRACE(517);

	STREAM_TRACE(518);
	chiaki_video_receiver_stream_info(stream_connection->video_receiver,
			decode_resolutions_context->video_profiles,
			decode_resolutions_context->video_profiles_count);
	/* chiaki_video_receiver_stream_info takes ownership of the header buffers. */
	decode_resolutions_context->video_profiles_count = 0;
	STREAM_TRACE(519);

	STREAM_TRACE(520);
	ChiakiErrorCode err = stream_connection_send_streaminfo_ack(stream_connection);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to send streaminfo acknowledgement");
		goto error_allocated;
	}
	STREAM_TRACE(521);

	STREAM_TRACE(522);
	err = stream_connection_send_controller_connection(stream_connection);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to send controller connection");
		goto error_allocated;
	}
	STREAM_TRACE(523);

	STREAM_TRACE(524);
	err = stream_connection_enable_microphone(stream_connection);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to enable microphone input");
		goto error_allocated;
	}
	STREAM_TRACE(525);

	// stream_connection->state_mutex is expected to be locked by the caller of this function
	stream_connection->state_finished = true;
	chiaki_cond_signal(&stream_connection->state_cond);
	free(decode_resolutions_context);
	free(msg);
	STREAM_TRACE(526);
	return;
error_allocated:
	stream_connection_free_decoded_profiles(decode_resolutions_context);
	free(decode_resolutions_context);
	free(msg);
error:
	stream_connection->state_failed = true;
	chiaki_cond_signal(&stream_connection->state_cond);
	STREAM_TRACE(529);
}

static bool chiaki_pb_encode_zero_encrypted_key(pb_ostream_t *stream, const pb_field_t *field, void *const *arg)
{
	if(!pb_encode_tag_for_field(stream, field))
		return false;
	uint8_t data[] = { 0, 0, 0, 0 };
	return pb_encode_string(stream, data, sizeof(data));
}

#define LAUNCH_SPEC_JSON_BUF_SIZE 1024

static ChiakiErrorCode stream_connection_encode_local_spec(ChiakiStreamConnection *connection,
	char *encoded, size_t encoded_capacity)
{
	ChiakiSession *session = connection->session;
	ChiakiLaunchSpec spec = {
		.target = session->target,
		.mtu = session->mtu_in,
		.rtt = session->rtt_us / 1000,
		.handshake_key = session->handshake_key,
		.width = session->connect_info.video_profile.width,
		.height = session->connect_info.video_profile.height,
		.max_fps = session->connect_info.video_profile.max_fps,
		.codec = session->connect_info.video_profile.codec,
		.bw_kbps_sent = session->connect_info.video_profile.bitrate,
	};
	char json[LAUNCH_SPEC_JSON_BUF_SIZE];
	int json_size = chiaki_launchspec_format(json, sizeof(json), &spec);
	if(json_size < 0 || (size_t)json_size + 1 > sizeof(json))
		return CHIAKI_ERR_UNKNOWN;
	json_size++;

	uint8_t mask[LAUNCH_SPEC_JSON_BUF_SIZE] = { 0 };
	ChiakiErrorCode err = chiaki_rpcrypt_encrypt(&session->rpcrypt, 0, mask, mask, (size_t)json_size);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;
	xor_bytes(mask, (uint8_t *)json, (size_t)json_size);
	return chiaki_base64_encode(mask, (size_t)json_size, encoded, encoded_capacity);
}

static ChiakiErrorCode stream_connection_send_big_fragments(ChiakiStreamConnection *connection,
	uint8_t *data, size_t size)
{
	const bool cloud = chiaki_service_type_is_cloud(connection->session->service_type);
	const uint32_t transport_overhead = cloud ? 28 : 50;
	const uint32_t first_overhead = cloud ? 30 : 26;
	const uint32_t continuation_overhead = cloud ? 29 : 25;
	uint32_t mtu = connection->session->mtu_in < connection->session->mtu_out
		? connection->session->mtu_in : connection->session->mtu_out;
	if(mtu <= transport_overhead + first_overhead)
		return CHIAKI_ERR_INVALID_DATA;
	const size_t packet_budget = mtu - transport_overhead;

	size_t offset = 0;
	bool continuation = false;
	while(offset < size)
	{
		const size_t overhead = continuation ? continuation_overhead : first_overhead;
		const size_t capacity = packet_budget - overhead;
		const size_t chunk = size - offset < capacity ? size - offset : capacity;
		const bool final = offset + chunk == size;
		ChiakiErrorCode err = continuation
			? chiaki_takion_send_message_data_cont(&connection->takion, final, 1, data + offset, chunk, NULL)
			: chiaki_takion_send_message_data(&connection->takion, final, 1, data + offset, chunk, NULL);
		if(err != CHIAKI_ERR_SUCCESS)
			return err;
		offset += chunk;
		continuation = true;
	}
	return CHIAKI_ERR_SUCCESS;
}

static ChiakiErrorCode stream_connection_send_big(ChiakiStreamConnection *stream_connection)
{
	ChiakiSession *session = stream_connection->session;
	char local_spec[LAUNCH_SPEC_JSON_BUF_SIZE * 2];
	const bool cloud = chiaki_service_type_is_cloud(session->service_type);
	const char *launch_spec = session->cloud_launch_spec;
	ChiakiErrorCode err;
	if(!cloud)
	{
		err = stream_connection_encode_local_spec(stream_connection, local_spec, sizeof(local_spec));
		if(err != CHIAKI_ERR_SUCCESS)
			return err;
		launch_spec = local_spec;
	}
	if(!launch_spec || !*launch_spec)
		return CHIAKI_ERR_INVALID_DATA;

	uint8_t ecdh_pub_key[128];
	ChiakiPBBuf ecdh_pub_key_buf = { sizeof(ecdh_pub_key), ecdh_pub_key };
	uint8_t ecdh_sig[32];
	ChiakiPBBuf ecdh_sig_buf = { sizeof(ecdh_sig), ecdh_sig };
	err = chiaki_ecdh_get_local_pub_key(&session->ecdh,
			ecdh_pub_key, &ecdh_pub_key_buf.size,
			session->handshake_key,
			ecdh_sig, &ecdh_sig_buf.size);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection failed to get ECDH key and sig");
		return err;
	}

	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));

	msg.type = tkproto_TakionMessage_PayloadType_BIG;
	msg.has_big_payload = true;
	msg.big_payload.client_version = stream_connection->takion.version;
	msg.big_payload.session_key.arg = session->session_id;
	msg.big_payload.session_key.funcs.encode = chiaki_pb_encode_string;
	msg.big_payload.launch_spec.arg = (void *)launch_spec;
	msg.big_payload.launch_spec.funcs.encode = chiaki_pb_encode_string;
	msg.big_payload.encrypted_key.funcs.encode = chiaki_pb_encode_zero_encrypted_key;
	msg.big_payload.ecdh_pub_key.arg = &ecdh_pub_key_buf;
	msg.big_payload.ecdh_pub_key.funcs.encode = chiaki_pb_encode_buf;
	msg.big_payload.ecdh_sig.arg = &ecdh_sig_buf;
	msg.big_payload.ecdh_sig.funcs.encode = chiaki_pb_encode_buf;

	size_t capacity = strlen(launch_spec) + sizeof(ecdh_pub_key) + sizeof(ecdh_sig) + 256;
	uint8_t *buf = malloc(capacity);
	if(!buf)
		return CHIAKI_ERR_MEMORY;
	pb_ostream_t stream = pb_ostream_from_buffer(buf, capacity);
	bool pbr = pb_encode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!pbr)
	{
		free(buf);
		CHIAKI_LOGE(stream_connection->log, "StreamConnection big protobuf encoding failed");
		return CHIAKI_ERR_UNKNOWN;
	}
	err = stream_connection_send_big_fragments(stream_connection, buf, stream.bytes_written);
	free(buf);
	return err;
}

static ChiakiErrorCode stream_connection_send_controller_connection(ChiakiStreamConnection *stream_connection)
{
	ChiakiSession *session = stream_connection->session;
	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));

	msg.type = tkproto_TakionMessage_PayloadType_CONTROLLERCONNECTION;
	msg.has_controller_connection_payload = true;
	msg.controller_connection_payload.has_connected = true;
	msg.controller_connection_payload.connected = true;
	msg.controller_connection_payload.has_controller_id = false;
	msg.controller_connection_payload.has_controller_type = true;
	msg.controller_connection_payload.controller_type = session->connect_info.enable_dualsense
		? tkproto_ControllerConnectionPayload_ControllerType_DUALSENSE
		: tkproto_ControllerConnectionPayload_ControllerType_DUALSHOCK4;

	uint8_t buf[2048];
	size_t buf_size;

	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
	bool pbr = pb_encode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!pbr)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection controller connection protobuf encoding failed");
		return CHIAKI_ERR_UNKNOWN;
	}

	buf_size = stream.bytes_written;
	return chiaki_takion_send_message_data(&stream_connection->takion, 1, 1, buf, buf_size, NULL);
}

static ChiakiErrorCode stream_connection_enable_microphone(ChiakiStreamConnection *stream_connection)
{
	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));

	ChiakiAudioHeader audio_header_input;
	chiaki_audio_header_set(&audio_header_input, 16, 1, 48000, 480);
	uint8_t audio_header[CHIAKI_AUDIO_HEADER_SIZE];
	chiaki_audio_header_save(&audio_header_input, audio_header);
	ChiakiPBBuf audio_header_buf = { sizeof(audio_header), (uint8_t *)audio_header };

	msg.type = tkproto_TakionMessage_PayloadType_STREAMINFO;
	msg.has_stream_info_payload = true;
	msg.stream_info_payload.audio_header.arg = &audio_header_buf;
	msg.stream_info_payload.audio_header.funcs.encode = chiaki_pb_encode_buf;
	msg.stream_info_payload.has_start_timeout = false;
	msg.stream_info_payload.has_afk_timeout = false;
	msg.stream_info_payload.has_afk_timeout_disconnect = false;
	msg.stream_info_payload.has_congestion_control_interval = false;

	uint8_t buf[2048];
	size_t buf_size;

	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
	bool pbr = pb_encode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!pbr)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection controller connection protobuf encoding failed");
		return CHIAKI_ERR_UNKNOWN;
	}

	buf_size = stream.bytes_written;
	return chiaki_takion_send_message_data(&stream_connection->takion, 1, 1, buf, buf_size, NULL);
}

static ChiakiErrorCode stream_connection_send_streaminfo_ack(ChiakiStreamConnection *stream_connection)
{
	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));
	msg.type = tkproto_TakionMessage_PayloadType_STREAMINFOACK;

	uint8_t buf[3];
	size_t buf_size;

	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
	bool pbr = pb_encode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!pbr)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection streaminfo ack protobuf encoding failed");
		return CHIAKI_ERR_UNKNOWN;
	}

	buf_size = stream.bytes_written;
	return chiaki_takion_send_message_data(&stream_connection->takion, 1, 9, buf, buf_size, NULL);
}

static ChiakiErrorCode stream_connection_send_disconnect(ChiakiStreamConnection *stream_connection)
{
	tkproto_TakionMessage msg;
	memset(&msg, 0, sizeof(msg));

	msg.type = tkproto_TakionMessage_PayloadType_DISCONNECT;
	msg.has_disconnect_payload = true;
	msg.disconnect_payload.reason.arg = "Client Disconnecting";
	msg.disconnect_payload.reason.funcs.encode = chiaki_pb_encode_string;

	uint8_t buf[26];
	size_t buf_size;

	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
	bool pbr = pb_encode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!pbr)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection disconnect protobuf encoding failed");
		return CHIAKI_ERR_UNKNOWN;
	}

	CHIAKI_LOGI(stream_connection->log, "StreamConnection sending Disconnect");

	buf_size = stream.bytes_written;
	ChiakiErrorCode err = chiaki_takion_send_message_data(&stream_connection->takion, 1, 1, buf, buf_size, NULL);

	return err;
}

static void stream_connection_takion_av(ChiakiStreamConnection *stream_connection, ChiakiTakionAVPacket *packet)
{
	chiaki_gkcrypt_decrypt(stream_connection->gkcrypt_remote, packet->key_pos + CHIAKI_GKCRYPT_BLOCK_SIZE, packet->data, packet->data_size);

	if(packet->codec == 3)
		packet->codec = stream_connection->session->connect_info.video_profile.codec;

	if(packet->is_video)
		chiaki_video_receiver_av_packet(stream_connection->video_receiver, packet);
	else if(packet->is_haptics)
		chiaki_audio_receiver_av_packet(stream_connection->haptics_receiver, packet);
	else
		chiaki_audio_receiver_av_packet(stream_connection->audio_receiver, packet);
}

static ChiakiErrorCode stream_connection_send_heartbeat(ChiakiStreamConnection *stream_connection)
{
	tkproto_TakionMessage msg = { 0 };
	msg.type = tkproto_TakionMessage_PayloadType_HEARTBEAT;

	uint8_t buf[8];

	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
	bool pbr = pb_encode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!pbr)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection heartbeat protobuf encoding failed");
		return CHIAKI_ERR_UNKNOWN;
	}

	return chiaki_takion_send_message_data(&stream_connection->takion, 1, 1, buf, stream.bytes_written, NULL);
}

CHIAKI_EXPORT ChiakiErrorCode stream_connection_send_corrupt_frame(ChiakiStreamConnection *stream_connection, ChiakiSeqNum16 start, ChiakiSeqNum16 end)
{
	tkproto_TakionMessage msg = { 0 };
	msg.type = tkproto_TakionMessage_PayloadType_CORRUPTFRAME;
	msg.has_corrupt_payload = true;
	msg.corrupt_payload.start = start;
	msg.corrupt_payload.end = end;

	uint8_t buf[0x10];

	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
	bool pbr = pb_encode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!pbr)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection heartbeat protobuf encoding failed");
		return CHIAKI_ERR_UNKNOWN;
	}

	CHIAKI_LOGW(stream_connection->log, "StreamConnection reporting corrupt frame(s) from %u to %u", (unsigned int)start, (unsigned int)end);
	return chiaki_takion_send_message_data(&stream_connection->takion, 1, 2, buf, stream.bytes_written, NULL);
}

CHIAKI_EXPORT ChiakiErrorCode stream_connection_send_idr_request(ChiakiStreamConnection *stream_connection)
{
	tkproto_TakionMessage msg = { 0 };
	msg.type = tkproto_TakionMessage_PayloadType_IDRREQUEST;

	uint8_t buf[0x10];

	pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
	bool pbr = pb_encode(&stream, tkproto_TakionMessage_fields, &msg);
	if(!pbr)
	{
		CHIAKI_LOGE(stream_connection->log, "StreamConnection IDR request protobuf encoding failed");
		return CHIAKI_ERR_UNKNOWN;
	}

	CHIAKI_LOGI(stream_connection->log, "StreamConnection requesting IDR frame");
	return chiaki_takion_send_message_data(&stream_connection->takion, 1, 2, buf, stream.bytes_written, NULL);
}
