// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/audioreceiver.h>
#include <chiaki/session.h>
#include <chiaki/log.h>
#include "cloud_audio_buffer.h"

#include <stdlib.h>
#include <string.h>

#ifdef CHIAKI_PS5
extern void cloudplay_session_trace(int stage);
#define AUDIO_TRACE(stage) cloudplay_session_trace(stage)
#else
#define AUDIO_TRACE(stage) ((void)0)
#endif

#define CHIAKI_AUDIO_JITTER_PREFILL 3
#define CHIAKI_AUDIO_JITTER_BUFFER_SIZE 8

static void chiaki_audio_receiver_frame(ChiakiAudioReceiver *audio_receiver, ChiakiSeqNum16 frame_index, bool is_haptics, uint8_t *buf, size_t buf_size);
static void chiaki_audio_receiver_clear_jitter_buffer(ChiakiAudioReceiver *audio_receiver);
static bool chiaki_audio_receiver_store_audio_frame_locked(ChiakiAudioReceiver *audio_receiver, ChiakiSeqNum16 frame_index, const uint8_t *buf, size_t buf_size);
static int chiaki_audio_receiver_find_audio_slot(const ChiakiAudioReceiver *audio_receiver, ChiakiSeqNum16 frame_index);
static int chiaki_audio_receiver_find_oldest_audio_slot(const ChiakiAudioReceiver *audio_receiver);
static int chiaki_audio_receiver_find_newest_audio_slot(const ChiakiAudioReceiver *audio_receiver);

// Callback for PSCLOUD audio reassembler - emits units with frame_index from reassembler
static void cloud_audio_emit(ChiakiSeqNum16 frame_index, uint8_t *buf, size_t buf_size, bool is_haptics, void *user)
{
	ChiakiAudioReceiver *audio_receiver = (ChiakiAudioReceiver *)user;
	chiaki_audio_receiver_frame(audio_receiver, frame_index, is_haptics, buf, buf_size);
	if(audio_receiver->packet_stats)
		chiaki_packet_stats_push_seq(audio_receiver->packet_stats, frame_index);
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_audio_receiver_init(ChiakiAudioReceiver *audio_receiver, ChiakiSession *session, ChiakiPacketStats *packet_stats)
{
	audio_receiver->session = session;
	audio_receiver->log = session->log;
	audio_receiver->packet_stats = packet_stats;

	audio_receiver->frame_index_prev = 0;
	audio_receiver->next_frame_index = 0;
	audio_receiver->next_frame_index_valid = false;
	audio_receiver->playback_started = false;
	audio_receiver->frame_index_startup = true;
	audio_receiver->jitter_buffer_count = 0;
	memset(audio_receiver->jitter_buffer, 0, sizeof(audio_receiver->jitter_buffer));
	audio_receiver->cloud_audio_buffer = NULL;

	ChiakiErrorCode err = chiaki_mutex_init(&audio_receiver->mutex, false);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;

	// Initialize PSCLOUD audio reassembler if this is a PSCLOUD session
	if(session && session->service_type == CHIAKI_SERVICE_TYPE_PSCLOUD)
	{
		CHIAKI_LOGI(session->log, "Audio Receiver: Detected PSCLOUD service type - initializing custom audio reassembler");
		CloudAudioBuffer *buffer = cloud_audio_buffer_create(session->log);
		if(!buffer)
		{
			chiaki_mutex_fini(&audio_receiver->mutex);
			return CHIAKI_ERR_MEMORY;
		}
		audio_receiver->cloud_audio_buffer = buffer;
		CHIAKI_LOGI(session->log, "Audio Receiver: ✓ PSCLOUD audio reassembler initialized successfully");
	}
	else
	{
		if(session)
			CHIAKI_LOGI(session->log, "Audio Receiver: Using standard audio decoder (service_type=%s)",
				chiaki_service_type_string(session->service_type));
		else
			CHIAKI_LOGI(session->log, "Audio Receiver: Using standard audio decoder (no session)");
		audio_receiver->cloud_audio_buffer = NULL;
	}

	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT void chiaki_audio_receiver_fini(ChiakiAudioReceiver *audio_receiver)
{
	chiaki_audio_receiver_clear_jitter_buffer(audio_receiver);

	if(audio_receiver->cloud_audio_buffer)
	{
		cloud_audio_buffer_destroy((CloudAudioBuffer *)audio_receiver->cloud_audio_buffer);
		audio_receiver->cloud_audio_buffer = NULL;
	}

	chiaki_mutex_fini(&audio_receiver->mutex);
}

CHIAKI_EXPORT void chiaki_audio_receiver_stream_info(ChiakiAudioReceiver *audio_receiver, ChiakiAudioHeader *audio_header)
{
	AUDIO_TRACE(540);
	ChiakiAudioSinkHeader header_cb = NULL;
	void *header_cb_user = NULL;

	ChiakiErrorCode err = chiaki_mutex_lock(&audio_receiver->mutex);
	AUDIO_TRACE(541);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(audio_receiver->log, "Failed to lock audio receiver mutex: %s", chiaki_error_string(err));
		return;
	}

	CHIAKI_LOGI(audio_receiver->log, "Audio Header:");
	CHIAKI_LOGI(audio_receiver->log, "  channels = %d", audio_header->channels);
	CHIAKI_LOGI(audio_receiver->log, "  bits = %d", audio_header->bits);
	CHIAKI_LOGI(audio_receiver->log, "  rate = %d", audio_header->rate);
	CHIAKI_LOGI(audio_receiver->log, "  frame size = %d", audio_header->frame_size);
	CHIAKI_LOGI(audio_receiver->log, "  unknown = %d", audio_header->unknown);
	AUDIO_TRACE(542);

	header_cb = audio_receiver->session->audio_sink.header_cb;
	header_cb_user = audio_receiver->session->audio_sink.user;
	AUDIO_TRACE(543);

	audio_receiver->frame_index_prev = 0;
	audio_receiver->next_frame_index = 0;
	audio_receiver->next_frame_index_valid = false;
	audio_receiver->playback_started = false;
	audio_receiver->frame_index_startup = true;
	chiaki_audio_receiver_clear_jitter_buffer(audio_receiver);
	AUDIO_TRACE(544);

	if(header_cb)
	{
		AUDIO_TRACE(545);
		header_cb(audio_header, header_cb_user);
		AUDIO_TRACE(546);
	}

	err = chiaki_mutex_unlock(&audio_receiver->mutex);
	AUDIO_TRACE(547);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(audio_receiver->log, "Failed to unlock audio receiver mutex: %s", chiaki_error_string(err));
		return;
	}
}

CHIAKI_EXPORT void chiaki_audio_receiver_av_packet(ChiakiAudioReceiver *audio_receiver, ChiakiTakionAVPacket *packet)
{
	if(packet->codec != 5)
	{
		CHIAKI_LOGE(audio_receiver->log, "Received Audio Packet with unknown Codec");
		return;
	}
	// PSCLOUD uses unitized format: one unit per AV packet, units_in_frame_fec is literal FEC count
	bool is_pscloud = audio_receiver->session
		&& audio_receiver->session->service_type == CHIAKI_SERVICE_TYPE_PSCLOUD;

	if(is_pscloud && audio_receiver->cloud_audio_buffer)
	{
		ChiakiErrorCode err = cloud_audio_buffer_push(
			(CloudAudioBuffer *)audio_receiver->cloud_audio_buffer, packet, cloud_audio_emit, audio_receiver);
		if(err != CHIAKI_ERR_SUCCESS && err != CHIAKI_ERR_FEC_FAILED)
		{
			CHIAKI_LOGW(audio_receiver->log, "PSCLOUD audio reassembler error: %d", (int)err);
		}
		return;
	}

	// this is mostly observation-based, so may not necessarily cover everything yet.

	uint8_t source_units_count = chiaki_takion_av_packet_audio_source_units_count(packet);
	uint8_t fec_units_count = chiaki_takion_av_packet_audio_fec_units_count(packet);
	uint8_t unit_size = chiaki_takion_av_packet_audio_unit_size(packet);

	if(!packet->data_size)
	{
		CHIAKI_LOGE(audio_receiver->log, "Audio AV Packet is empty");
		return;
	}

	if((uint16_t)fec_units_count + (uint16_t)source_units_count != packet->units_in_frame_total)
	{
		CHIAKI_LOGE(audio_receiver->log, "Source Units + FEC Units != Total Units in Audio AV Packet");
		return;
	}

	if(packet->data_size != (size_t)unit_size * (size_t)packet->units_in_frame_total)
	{
		CHIAKI_LOGE(audio_receiver->log, "Audio AV Packet size mismatch %#llx vs %#llx",
			(unsigned long long)packet->data_size,
			(unsigned long long)(unit_size * packet->units_in_frame_total));
		return;
	}

	if(packet->frame_index > (1 << 15))
		audio_receiver->frame_index_startup = false;

	for(size_t i = 0; i < source_units_count + fec_units_count; i++)
	{
		ChiakiSeqNum16 frame_index;
		if(i < source_units_count)
			frame_index = packet->frame_index + i;
		else
		{
			size_t fec_index = i - source_units_count;
			if(audio_receiver->frame_index_startup && packet->frame_index + fec_index < fec_units_count + 1)
				continue;
			frame_index = packet->frame_index - fec_units_count + fec_index;
		}

		chiaki_audio_receiver_frame(audio_receiver, frame_index, packet->is_haptics, packet->data + unit_size * i, unit_size);
	}

	if(audio_receiver->packet_stats)
		chiaki_packet_stats_push_seq(audio_receiver->packet_stats, packet->frame_index);
}

static void chiaki_audio_receiver_frame(ChiakiAudioReceiver *audio_receiver, ChiakiSeqNum16 frame_index, bool is_haptics, uint8_t *buf, size_t buf_size)
{
	while(true)
	{
		ChiakiAudioSinkFrame frame_cb = NULL;
		void *frame_cb_user = NULL;
		uint8_t *deliver_buf = NULL;
		size_t deliver_buf_size = 0;
		bool deliver_owned_buf = false;

		ChiakiErrorCode err = chiaki_mutex_lock(&audio_receiver->mutex);
		if(err != CHIAKI_ERR_SUCCESS)
		{
			CHIAKI_LOGE(audio_receiver->log, "Failed to lock audio receiver mutex: %s", chiaki_error_string(err));
			return;
		}

		if(is_haptics)
		{
			if(chiaki_seq_num_16_gt(frame_index, audio_receiver->frame_index_prev))
			{
				audio_receiver->frame_index_prev = frame_index;
				frame_cb = audio_receiver->session->haptics_sink.frame_cb;
				frame_cb_user = audio_receiver->session->haptics_sink.user;
				deliver_buf = buf;
				deliver_buf_size = buf_size;
			}
			err = chiaki_mutex_unlock(&audio_receiver->mutex);
			if(err != CHIAKI_ERR_SUCCESS)
			{
				CHIAKI_LOGE(audio_receiver->log, "Failed to unlock audio receiver mutex: %s", chiaki_error_string(err));
				return;
			}
			if(frame_cb)
				frame_cb(deliver_buf, deliver_buf_size, frame_cb_user);
			return;
		}

		if(buf)
		{
			if(audio_receiver->next_frame_index_valid && chiaki_seq_num_16_lt(frame_index, audio_receiver->next_frame_index))
				goto unlock_and_exit;

			if(!chiaki_audio_receiver_store_audio_frame_locked(audio_receiver, frame_index, buf, buf_size))
				goto unlock_and_exit;
			buf = NULL;
			buf_size = 0;
		}

		if(!audio_receiver->playback_started && audio_receiver->jitter_buffer_count >= CHIAKI_AUDIO_JITTER_PREFILL)
		{
			int oldest = chiaki_audio_receiver_find_oldest_audio_slot(audio_receiver);
			if(oldest >= 0)
			{
				audio_receiver->next_frame_index = audio_receiver->jitter_buffer[oldest].frame_index;
				audio_receiver->next_frame_index_valid = true;
				audio_receiver->playback_started = true;
			}
		}

		if(audio_receiver->playback_started && audio_receiver->next_frame_index_valid)
		{
			int slot = chiaki_audio_receiver_find_audio_slot(audio_receiver, audio_receiver->next_frame_index);
			if(slot >= 0)
			{
				frame_cb = audio_receiver->session->audio_sink.frame_cb;
				frame_cb_user = audio_receiver->session->audio_sink.user;
				deliver_buf = audio_receiver->jitter_buffer[slot].buf;
				deliver_buf_size = audio_receiver->jitter_buffer[slot].buf_size;
				deliver_owned_buf = true;
				audio_receiver->jitter_buffer[slot].buf = NULL;
				audio_receiver->jitter_buffer[slot].buf_size = 0;
				audio_receiver->jitter_buffer[slot].occupied = false;
				audio_receiver->jitter_buffer_count--;
				audio_receiver->frame_index_prev = audio_receiver->next_frame_index;
				audio_receiver->next_frame_index++;
			}
			else if(audio_receiver->jitter_buffer_count > 0)
			{
				int oldest = chiaki_audio_receiver_find_oldest_audio_slot(audio_receiver);
				int newest = chiaki_audio_receiver_find_newest_audio_slot(audio_receiver);
				bool newer_audio_buffered = oldest >= 0
					&& chiaki_seq_num_16_gt(audio_receiver->jitter_buffer[oldest].frame_index, audio_receiver->next_frame_index);
				bool can_conceal_loss = false;
				if(newer_audio_buffered && newest >= 0)
				{
					if(audio_receiver->jitter_buffer_count >= CHIAKI_AUDIO_JITTER_PREFILL)
					{
						ChiakiSeqNum16 required_lookahead = audio_receiver->next_frame_index + CHIAKI_AUDIO_JITTER_PREFILL;
						can_conceal_loss = chiaki_seq_num_16_gt(audio_receiver->jitter_buffer[newest].frame_index, required_lookahead - 1);
					}
					else
					{
						can_conceal_loss = true;
					}
				}
				if(can_conceal_loss)
				{
					frame_cb = audio_receiver->session->audio_sink.frame_cb;
					frame_cb_user = audio_receiver->session->audio_sink.user;
					deliver_buf = NULL;
					deliver_buf_size = 0;
					audio_receiver->frame_index_prev = audio_receiver->next_frame_index;
					audio_receiver->next_frame_index++;
				}
			}
		}

unlock_and_exit:
		err = chiaki_mutex_unlock(&audio_receiver->mutex);
		if(err != CHIAKI_ERR_SUCCESS)
		{
			CHIAKI_LOGE(audio_receiver->log, "Failed to unlock audio receiver mutex: %s", chiaki_error_string(err));
			if(deliver_owned_buf)
				free(deliver_buf);
			return;
		}

		if(!frame_cb)
			return;

		frame_cb(deliver_buf, deliver_buf_size, frame_cb_user);
		if(deliver_owned_buf)
			free(deliver_buf);
	}
}

static void chiaki_audio_receiver_clear_jitter_buffer(ChiakiAudioReceiver *audio_receiver)
{
	for(size_t i = 0; i < CHIAKI_AUDIO_JITTER_BUFFER_SIZE; i++)
	{
		free(audio_receiver->jitter_buffer[i].buf);
		audio_receiver->jitter_buffer[i].buf = NULL;
		audio_receiver->jitter_buffer[i].buf_size = 0;
		audio_receiver->jitter_buffer[i].occupied = false;
	}
	audio_receiver->jitter_buffer_count = 0;
}

static int chiaki_audio_receiver_find_audio_slot(const ChiakiAudioReceiver *audio_receiver, ChiakiSeqNum16 frame_index)
{
	for(size_t i = 0; i < CHIAKI_AUDIO_JITTER_BUFFER_SIZE; i++)
	{
		if(audio_receiver->jitter_buffer[i].occupied && audio_receiver->jitter_buffer[i].frame_index == frame_index)
			return (int)i;
	}
	return -1;
}

static int chiaki_audio_receiver_find_oldest_audio_slot(const ChiakiAudioReceiver *audio_receiver)
{
	int oldest = -1;
	for(size_t i = 0; i < CHIAKI_AUDIO_JITTER_BUFFER_SIZE; i++)
	{
		if(!audio_receiver->jitter_buffer[i].occupied)
			continue;
		if(oldest < 0 || chiaki_seq_num_16_lt(audio_receiver->jitter_buffer[i].frame_index,
			audio_receiver->jitter_buffer[oldest].frame_index))
			oldest = (int)i;
	}
	return oldest;
}

static int chiaki_audio_receiver_find_newest_audio_slot(const ChiakiAudioReceiver *audio_receiver)
{
	int newest = -1;
	for(size_t i = 0; i < CHIAKI_AUDIO_JITTER_BUFFER_SIZE; i++)
	{
		if(!audio_receiver->jitter_buffer[i].occupied)
			continue;
		if(newest < 0 || chiaki_seq_num_16_gt(audio_receiver->jitter_buffer[i].frame_index,
			audio_receiver->jitter_buffer[newest].frame_index))
			newest = (int)i;
	}
	return newest;
}

static bool chiaki_audio_receiver_store_audio_frame_locked(ChiakiAudioReceiver *audio_receiver, ChiakiSeqNum16 frame_index, const uint8_t *buf, size_t buf_size)
{
	if(chiaki_audio_receiver_find_audio_slot(audio_receiver, frame_index) >= 0)
		return false;

	int free_slot = -1;
	for(size_t i = 0; i < CHIAKI_AUDIO_JITTER_BUFFER_SIZE; i++)
	{
		if(!audio_receiver->jitter_buffer[i].occupied)
		{
			free_slot = (int)i;
			break;
		}
	}

	if(free_slot < 0)
	{
		int newest = chiaki_audio_receiver_find_newest_audio_slot(audio_receiver);
		if(newest < 0)
			return false;
		if(!chiaki_seq_num_16_lt(frame_index, audio_receiver->jitter_buffer[newest].frame_index))
			return false;
		free(audio_receiver->jitter_buffer[newest].buf);
		audio_receiver->jitter_buffer[newest].buf = NULL;
		audio_receiver->jitter_buffer[newest].buf_size = 0;
		audio_receiver->jitter_buffer[newest].occupied = false;
		audio_receiver->jitter_buffer_count--;
		free_slot = newest;
	}

	uint8_t *copy = NULL;
	if(buf_size > 0)
	{
		copy = malloc(buf_size);
		if(!copy)
			return false;
		memcpy(copy, buf, buf_size);
	}

	audio_receiver->jitter_buffer[free_slot].occupied = true;
	audio_receiver->jitter_buffer[free_slot].frame_index = frame_index;
	audio_receiver->jitter_buffer[free_slot].buf = copy;
	audio_receiver->jitter_buffer[free_slot].buf_size = buf_size;
	audio_receiver->jitter_buffer_count++;
	return true;
}
