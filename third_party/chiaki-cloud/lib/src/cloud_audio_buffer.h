// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_CLOUD_AUDIO_BUFFER_H
#define CHIAKI_CLOUD_AUDIO_BUFFER_H

#include <chiaki/common.h>
#include <chiaki/log.h>
#include <chiaki/takion.h>

typedef struct cloud_audio_buffer_t CloudAudioBuffer;

typedef void (*CloudAudioEmit)(ChiakiSeqNum16 frame_index, uint8_t *data, size_t size,
	bool haptics, void *user);

CloudAudioBuffer *cloud_audio_buffer_create(ChiakiLog *log);
void cloud_audio_buffer_destroy(CloudAudioBuffer *buffer);
ChiakiErrorCode cloud_audio_buffer_push(CloudAudioBuffer *buffer, const ChiakiTakionAVPacket *packet,
	CloudAudioEmit emit, void *emit_user);

#endif
