// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/packetstats.h>
#include <chiaki/log.h>
#include <assert.h>

CHIAKI_EXPORT ChiakiErrorCode chiaki_packet_stats_init(ChiakiPacketStats *stats)
{
	ChiakiErrorCode err = chiaki_mutex_init(&stats->mutex, false);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;
	err = chiaki_mutex_lock(&stats->mutex);
	assert(err == CHIAKI_ERR_SUCCESS);
	stats->gen_received = 0;
	stats->gen_lost = 0;
	stats->seq_min = 0;
	stats->seq_max = 0;
	stats->seq_received = 0;
	stats->seq_seen_window = 0;
	stats->seq_initialized = false;
	err = chiaki_mutex_unlock(&stats->mutex);
	return err;
}

CHIAKI_EXPORT void chiaki_packet_stats_fini(ChiakiPacketStats *stats)
{
	chiaki_mutex_fini(&stats->mutex);
}

static void reset_stats(ChiakiPacketStats *stats)
{
	stats->gen_received = 0;
	stats->gen_lost = 0;
	stats->seq_min = stats->seq_max;
	stats->seq_received = 0;
	stats->seq_seen_window = 0;
}

CHIAKI_EXPORT void chiaki_packet_stats_reset(ChiakiPacketStats *stats)
{
	chiaki_mutex_lock(&stats->mutex);
	reset_stats(stats);
	chiaki_mutex_unlock(&stats->mutex);
}

CHIAKI_EXPORT void chiaki_packet_stats_push_generation(ChiakiPacketStats *stats, uint64_t received, uint64_t lost)
{
	chiaki_mutex_lock(&stats->mutex);
	stats->gen_received += received;
	stats->gen_lost += lost;
	chiaki_mutex_unlock(&stats->mutex);
}

CHIAKI_EXPORT void chiaki_packet_stats_push_seq(ChiakiPacketStats *stats, ChiakiSeqNum16 seq_num)
{
	chiaki_mutex_lock(&stats->mutex);
	if(!stats->seq_initialized)
	{
		// Include the first packet in the expected range without assuming that a
		// stream starts at sequence zero.
		stats->seq_min = (ChiakiSeqNum16)(seq_num - 1);
		stats->seq_max = seq_num;
		stats->seq_seen_window = 1;
		stats->seq_initialized = true;
		stats->seq_received++;
		chiaki_mutex_unlock(&stats->mutex);
		return;
	}
	if(chiaki_seq_num_16_gt(seq_num, stats->seq_max))
	{
		uint16_t advance = (uint16_t)(seq_num - stats->seq_max);
		stats->seq_seen_window = advance >= 64
			? 1
			: (stats->seq_seen_window << advance) | 1;
		stats->seq_max = seq_num;
		stats->seq_received++;
	}
	else if(seq_num != stats->seq_max
		&& chiaki_seq_num_16_gt(seq_num, stats->seq_min))
	{
		uint16_t behind = (uint16_t)(stats->seq_max - seq_num);
		if(behind < 64 && !(stats->seq_seen_window & (UINT64_C(1) << behind)))
		{
			stats->seq_seen_window |= UINT64_C(1) << behind;
			stats->seq_received++;
		}
	}
	chiaki_mutex_unlock(&stats->mutex);
}

CHIAKI_EXPORT void chiaki_packet_stats_get(ChiakiPacketStats *stats, bool reset, uint64_t *received, uint64_t *lost)
{
	chiaki_mutex_lock(&stats->mutex);

	// gen
	*received = stats->gen_received;
	*lost = stats->gen_lost;

	//CHIAKI_LOGD(NULL, "gen received: %llu, lost: %llu",
	//		(unsigned long long)stats->gen_received,
	//		(unsigned long long)stats->gen_lost);

	// seq
	uint64_t seq_diff = stats->seq_initialized
		? (uint16_t)(stats->seq_max - stats->seq_min)
		: 0;
	uint64_t seq_lost = seq_diff > stats->seq_received
		? seq_diff - stats->seq_received
		: 0;
	*received += stats->seq_received;
	*lost += seq_lost;

	//CHIAKI_LOGD(NULL, "seq received: %llu, lost: %llu",
	//		(unsigned long long)stats->seq_received,
	//		(unsigned long long)seq_lost);

	if(reset)
		reset_stats(stats);
	chiaki_mutex_unlock(&stats->mutex);
}
