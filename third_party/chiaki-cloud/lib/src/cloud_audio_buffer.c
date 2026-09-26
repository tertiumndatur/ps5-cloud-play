// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "cloud_audio_buffer.h"

#include <chiaki/fec.h>
#include <chiaki/time.h>

#include <stdlib.h>
#include <string.h>

#define CLOUD_AUDIO_MAX_UNITS 512
#define CLOUD_AUDIO_GROUP_LIFETIME_US 150000

typedef struct cloud_audio_group_t
{
	bool active;
	bool delivered;
	bool haptics;
	ChiakiSeqNum16 first_sequence;
	uint16_t data_units;
	uint16_t parity_units;
	uint16_t unit_count;
	uint16_t data_received;
	uint16_t parity_received;
	size_t unit_size;
	size_t stride;
	uint64_t started_at;
	uint8_t *storage;
	size_t storage_capacity;
	bool *present;
	size_t metadata_capacity;
} CloudAudioGroup;

struct cloud_audio_buffer_t
{
	ChiakiLog *log;
	CloudAudioGroup group;
};

CloudAudioBuffer *cloud_audio_buffer_create(ChiakiLog *log)
{
	CloudAudioBuffer *buffer = calloc(1, sizeof(*buffer));
	if(buffer)
		buffer->log = log;
	return buffer;
}

void cloud_audio_buffer_destroy(CloudAudioBuffer *buffer)
{
	if(!buffer)
		return;
	free(buffer->group.storage);
	free(buffer->group.present);
	free(buffer);
}

static ChiakiErrorCode cloud_audio_begin_group(CloudAudioGroup *group,
	const ChiakiTakionAVPacket *packet)
{
	uint16_t total = packet->units_in_frame_total;
	uint16_t parity = packet->units_in_frame_fec;
	if(total == 0 || total > CLOUD_AUDIO_MAX_UNITS || parity >= total
		|| packet->unit_index >= total || packet->data_size == 0)
		return CHIAKI_ERR_INVALID_DATA;

	size_t stride = (packet->data_size + 15) & ~(size_t)15;
	if(stride < packet->data_size || stride > SIZE_MAX / total)
		return CHIAKI_ERR_OVERFLOW;
	size_t required = stride * total;
	if(required > group->storage_capacity)
	{
		uint8_t *storage = realloc(group->storage, required);
		if(!storage)
			return CHIAKI_ERR_MEMORY;
		group->storage = storage;
		group->storage_capacity = required;
	}
	if(total > group->metadata_capacity)
	{
		bool *present = realloc(group->present, total * sizeof(*present));
		if(!present)
			return CHIAKI_ERR_MEMORY;
		group->present = present;
		group->metadata_capacity = total;
	}

	memset(group->storage, 0, required);
	memset(group->present, 0, total * sizeof(*group->present));
	group->first_sequence = (ChiakiSeqNum16)(packet->frame_index - packet->unit_index);
	group->unit_count = total;
	group->parity_units = parity;
	group->data_units = total - parity;
	group->data_received = 0;
	group->parity_received = 0;
	group->unit_size = packet->data_size;
	group->stride = stride;
	group->started_at = chiaki_time_now_monotonic_us();
	group->haptics = packet->is_haptics;
	group->delivered = false;
	group->active = true;
	return CHIAKI_ERR_SUCCESS;
}

static ChiakiErrorCode cloud_audio_try_repair(CloudAudioGroup *group)
{
	uint16_t available = group->data_received + group->parity_received;
	if(group->data_received >= group->data_units)
		return CHIAKI_ERR_SUCCESS;
	if(available < group->data_units)
		return CHIAKI_ERR_FEC_FAILED;

	size_t missing_count = group->unit_count - available;
	unsigned int *missing = calloc(missing_count, sizeof(*missing));
	if(!missing)
		return CHIAKI_ERR_MEMORY;
	size_t cursor = 0;
	for(uint16_t i = 0; i < group->unit_count; i++)
		if(!group->present[i])
			missing[cursor++] = i;

	ChiakiErrorCode error = chiaki_fec_decode(group->storage, group->unit_size, group->stride,
		group->data_units, group->parity_units, missing, missing_count);
	if(error == CHIAKI_ERR_SUCCESS)
	{
		for(size_t i = 0; i < missing_count; i++)
		{
			unsigned int index = missing[i];
			if(index < group->data_units && !group->present[index])
			{
				group->present[index] = true;
				group->data_received++;
			}
		}
	}
	free(missing);
	return error == CHIAKI_ERR_SUCCESS ? CHIAKI_ERR_SUCCESS : CHIAKI_ERR_FEC_FAILED;
}

static void cloud_audio_deliver(CloudAudioGroup *group, CloudAudioEmit emit, void *user)
{
	if(group->delivered)
		return;
	cloud_audio_try_repair(group);
	for(uint16_t i = 0; i < group->data_units; i++)
	{
		if(group->present[i] && emit)
			emit((ChiakiSeqNum16)(group->first_sequence + i),
				group->storage + i * group->stride, group->unit_size, group->haptics, user);
	}
	group->delivered = true;
}

static void cloud_audio_finish_group(CloudAudioGroup *group, CloudAudioEmit emit, void *user)
{
	if(!group->active)
		return;
	cloud_audio_deliver(group, emit, user);
	group->active = false;
}

ChiakiErrorCode cloud_audio_buffer_push(CloudAudioBuffer *buffer, const ChiakiTakionAVPacket *packet,
	CloudAudioEmit emit, void *emit_user)
{
	if(!buffer || !packet || !packet->data || !packet->data_size
		|| packet->unit_index >= packet->units_in_frame_total)
		return CHIAKI_ERR_INVALID_DATA;

	uint64_t now = chiaki_time_now_monotonic_us();
	CloudAudioGroup *group = &buffer->group;
	bool stale = group->active && now - group->started_at > CLOUD_AUDIO_GROUP_LIFETIME_US;
	bool complete = group->active
		&& group->data_received + group->parity_received >= group->unit_count;
	bool format_changed = group->active
		&& (group->unit_count != packet->units_in_frame_total
			|| group->parity_units != packet->units_in_frame_fec
			|| group->unit_size != packet->data_size
			|| group->haptics != packet->is_haptics);
	bool unit_zero_starts_next = group->active && packet->unit_index == 0
		&& packet->frame_index != group->first_sequence;
	bool start_group = !group->active || stale || complete || format_changed
		|| packet->unit_index >= group->unit_count || unit_zero_starts_next;

	if(start_group)
	{
		cloud_audio_finish_group(group, emit, emit_user);
		ChiakiErrorCode error = cloud_audio_begin_group(group, packet);
		if(error != CHIAKI_ERR_SUCCESS)
		{
			group->active = false;
			return error;
		}
	}

	uint16_t index = packet->unit_index;
	if(group->delivered || group->present[index])
		return CHIAKI_ERR_SUCCESS;
	if(index == 0)
		group->first_sequence = packet->frame_index;
	memcpy(group->storage + index * group->stride, packet->data, group->unit_size);
	group->present[index] = true;
	if(index < group->data_units)
		group->data_received++;
	else
		group->parity_received++;

	if(group->data_received == group->data_units)
	{
		cloud_audio_deliver(group, emit, emit_user);
		return CHIAKI_ERR_SUCCESS;
	}
	if(group->data_received + group->parity_received >= group->data_units)
	{
		ChiakiErrorCode error = cloud_audio_try_repair(group);
		if(error == CHIAKI_ERR_SUCCESS)
			cloud_audio_deliver(group, emit, emit_user);
		return error;
	}
	return CHIAKI_ERR_SUCCESS;
}
