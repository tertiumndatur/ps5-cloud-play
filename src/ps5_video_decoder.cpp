/*
 * PS5 VideoDec2/AGC cloud-stream decoder.
 * Decoder ABI and zero-copy memory flow adapted from ProsperoLight.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ps5_video_decoder.hpp"
#include "app_log.hpp"
#include "native_agc_present.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <mutex>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace cloudplay {
namespace {

constexpr size_t kAlignment = 0x4000;
constexpr size_t kInputSlotBytes = 0x800000;
constexpr unsigned kPipelineDepth = 1;
constexpr unsigned kPoolSlots = kPipelineDepth + 2;
constexpr uint16_t kVideoDec2Module = 207;
constexpr uint32_t kVideoDec2OversizeDecode = 0x811d0302u;
constexpr int32_t kH264DpbFrames = 16;
constexpr int32_t kHevcDpbFrames = 4;
constexpr size_t kLoadingSurfaceBytes = 1920u * 1088u * 3u / 2u;

struct VideoDec2DecoderConfig {
    uint64_t size;
    uint32_t resource_type, codec_type, profile, max_level;
    int32_t max_width, max_height, max_dpb_frames;
    uint32_t pipeline_depth;
    uint64_t compute_queue, cpu_affinity;
    int32_t cpu_priority;
    uint32_t optimize_progressive, check_memory_type, reserved;
};

struct VideoDec2DecoderMemory {
    uint64_t size, cpu_size;
    void *cpu;
    uint64_t gpu_size;
    void *gpu;
    uint64_t cpu_gpu_size;
    void *cpu_gpu;
    uint64_t max_frame_size;
    uint32_t frame_alignment, reserved;
};

struct VideoDec2ComputeConfig {
    uint64_t size;
    uint16_t pipe_id, queue_id;
    uint8_t check_memory_type, reserved0;
    uint16_t reserved1;
};

struct VideoDec2ComputeMemory {
    uint64_t size, cpu_gpu_size;
    void *cpu_gpu;
};

struct VideoDec2Input {
    uint64_t size;
    void *au;
    uint64_t au_size, pts, dts, attached;
};

struct VideoDec2Frame {
    uint64_t size;
    void *buffer;
    uint64_t buffer_size;
    uint32_t accepted, reserved;
};

struct VideoDec2Output {
    uint64_t size;
    uint8_t valid, error, picture_count, padding;
    uint32_t codec, width, pitch, height, reserved;
    void *buffer;
    uint64_t buffer_size;
    uint32_t frame_format, pitch_bytes;
};

extern "C" {
int64_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t *);
int32_t sceKernelMapDirectMemory(void **, size_t, int, int, int64_t, size_t);
int32_t sceKernelMunmap(void *, size_t);
int32_t sceKernelReleaseDirectMemory(int64_t, size_t);
int32_t sceKernelAvailableFlexibleMemorySize(size_t *);
int32_t sceKernelMapNamedFlexibleMemory(void **, size_t, int, int, const char *);
int32_t sceKernelReleaseFlexibleMemory(void *, size_t);
int32_t sceSysmoduleLoadModule(uint16_t);
int32_t sceSysmoduleUnloadModule(uint16_t);

int32_t sceVideodec2QueryDecoderMemoryInfo(const VideoDec2DecoderConfig *,
                                           VideoDec2DecoderMemory *);
int32_t sceVideodec2QueryComputeMemoryInfo(VideoDec2ComputeMemory *);
int32_t sceVideodec2AllocateComputeQueue(const VideoDec2ComputeConfig *,
                                         const VideoDec2ComputeMemory *, void **);
int32_t sceVideodec2ReleaseComputeQueue(void *);
int32_t sceVideodec2CreateDecoder(const VideoDec2DecoderConfig *,
                                  const VideoDec2DecoderMemory *, void **);
int32_t sceVideodec2DeleteDecoder(void *);
int32_t sceVideodec2Reset(void *);
int32_t sceVideodec2Decode(void *, VideoDec2Input *, VideoDec2Frame *, VideoDec2Output *);
int32_t sceVideodec2Flush(void *, VideoDec2Frame *, VideoDec2Output *);
}

size_t align_16k(size_t value) {
    return (value + kAlignment - 1) & ~(kAlignment - 1);
}

int32_t allocate_direct(size_t size, int protection, int64_t limit,
                        int64_t &start, void *&address) {
    if (!size) return 0;
    int32_t result = sceKernelAllocateDirectMemory(0, limit, size, kAlignment, 12, &start);
    if (result == 0)
        result = sceKernelMapDirectMemory(&address, size, protection, 0, start, kAlignment);
    return result;
}

void release_direct(void *&address, int64_t &start, size_t size) {
    if (address) (void)sceKernelMunmap(address, size);
    if (start >= 0) (void)sceKernelReleaseDirectMemory(start, size);
    address = nullptr;
    start = -1;
}

bool frame_in_pool(const void *frame, const void *pool, size_t stride) {
    for (unsigned i = 0; i < kPoolSlots; ++i)
        if (frame == static_cast<const uint8_t *>(pool) + i * stride) return true;
    return false;
}

bool contains_vcl(const uint8_t *data, size_t size, bool hevc) {
    bool found_start_code = false;
    for (size_t i = 0; i + 4 < size; ++i) {
        size_t nal = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) nal = i + 3;
        else if (i + 5 < size && data[i] == 0 && data[i + 1] == 0 &&
                 data[i + 2] == 0 && data[i + 3] == 1) nal = i + 4;
        if (!nal || nal >= size) continue;
        found_start_code = true;
        const unsigned type = hevc ? (data[nal] >> 1) & 0x3f : data[nal] & 0x1f;
        if ((hevc && type <= 31) || (!hevc && type >= 1 && type <= 5)) return true;
        i = nal;
    }
    // Treat an unknown framing format as an access unit so it reaches the
    // decoder and produces a useful native error instead of being discarded.
    return !found_start_code;
}

unsigned padded_height(unsigned height) {
    if (height == 1080) return 1088;
    if (height == 2160) return 2176;
    return height;
}

unsigned visible_height(unsigned decoded_height) {
    if (decoded_height == 1088) return 1080;
    if (decoded_height == 2176) return 2160;
    return decoded_height;
}

uint32_t decoder_level(bool hevc, unsigned height) {
    if (hevc) return height >= 2160 ? 153 : height >= 1440 ? 150 : 123;
    return height >= 2160 ? 52 : 51;
}

uint64_t monotonic_us() {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return static_cast<uint64_t>(now.tv_sec) * 1000000u +
           static_cast<uint64_t>(now.tv_nsec) / 1000u;
}

} // namespace

struct Ps5VideoDecoder::Impl {
    bool hevc = true;
    unsigned max_width = 0;
    unsigned max_height = 0;
    unsigned fps = 60;
    bool ready = false;
    bool module_loaded = false;
    bool agc_used = false;
    bool need_header = true;
    void *decoder = nullptr;
    void *compute_queue = nullptr;
    VideoDec2DecoderMemory memory{};
    VideoDec2ComputeMemory compute_memory{};
    void *input_memory = nullptr;
    void *frame_memory = nullptr;
    void *loading_memory = nullptr;
    int64_t compute_start = -1;
    int64_t gpu_start = -1;
    int64_t cpu_gpu_start = -1;
    int64_t input_start = -1;
    int64_t frame_start = -1;
    int64_t loading_start = -1;
    size_t compute_size = 0;
    size_t cpu_mapping_size = 0;
    size_t gpu_size = 0;
    size_t cpu_gpu_size = 0;
    size_t input_size = kInputSlotBytes;
    size_t input_pool_size = 0;
    size_t frame_size = 0;
    size_t frame_pool_size = 0;
    size_t loading_size = 0;
    uint32_t access_units = 0;
    uint32_t presented = 0;
    uint32_t decode_failures = 0;
    bool stream_stats_enabled = false;
    uint64_t incoming_frames = 0;
    uint64_t reported_lost_frames = 0;
    uint64_t decode_total_us = 0;
    uint64_t decode_samples = 0;
    uint64_t processing_total_us = 0;
    uint64_t processing_samples = 0;
    uint64_t processing_min_us = 0;
    uint64_t processing_max_us = 0;
    RateWindow incoming_rate;
    RateWindow rendering_rate;
    std::atomic<uint64_t> stream_rtt_us{0};
    std::atomic<uint64_t> feedback_send_delay_us{0};
    std::atomic<uint64_t> packets_received{0};
    std::atomic<uint64_t> packets_lost{0};
    std::atomic<bool> loading_active{false};
    pthread_t loading_thread{};
    bool loading_thread_started = false;
    bool loading_presented = false;
    std::mutex loading_mutex;
    std::string loading_status;
    std::vector<uint8_t> header;
    std::string last_error;

    void fail(const char *message, int32_t result) {
        last_error = std::string(message) + " (0x";
        char code[16]{};
        std::snprintf(code, sizeof(code), "%08x", static_cast<uint32_t>(result));
        last_error += code;
        last_error += ")";
        app_log("videodec2", message, result);
    }

    std::string current_loading_status() {
        std::lock_guard<std::mutex> lock(loading_mutex);
        return loading_status;
    }

    static void *loading_entry(void *user) {
        auto *self = static_cast<Impl *>(user);
        uint32_t phase = 1;
        while (self->loading_active.load(std::memory_order_acquire)) {
            const std::string status = self->current_loading_status();
            const int result = native_agc_present_loading(
                self->loading_memory, self->loading_size, phase++, 0,
                self->max_width, visible_height(self->max_height), self->fps,
                status.c_str());
            if (result != 0) {
                app_log("loading", "present_failed", result);
                self->loading_active.store(false, std::memory_order_release);
                break;
            }
            for (unsigned slice = 0; slice < 20 &&
                 self->loading_active.load(std::memory_order_acquire); ++slice)
                usleep(10000);
        }
        return nullptr;
    }

    void set_loading(const std::string &status) {
        std::lock_guard<std::mutex> lock(loading_mutex);
        loading_status = status.empty() ? "Preparing Cloud Play" : status.substr(0, 64);
    }

    bool start_loading(const std::string &status) {
        if (!ready || loading_thread_started || loading_active.load()) return false;
        set_loading(status);
        if (!loading_memory) {
            loading_size = align_16k(kLoadingSurfaceBytes);
            const int32_t result = allocate_direct(loading_size, 0x32,
                sceKernelGetDirectMemorySize(), loading_start, loading_memory);
            app_log("loading", "surface_alloc", result);
            if (result != 0) return false;
        }
        const std::string initial = current_loading_status();
        const int present_result = native_agc_present_loading(
            loading_memory, loading_size, 0, 0, max_width, visible_height(max_height), fps,
            initial.c_str());
        app_log("loading", "first_present", present_result);
        if (present_result != 0) return false;
        agc_used = true;
        loading_presented = true;
        loading_active.store(true, std::memory_order_release);
        const int thread_result = pthread_create(&loading_thread, nullptr, loading_entry, this);
        app_log("loading", "thread_create", thread_result);
        if (thread_result != 0) {
            loading_active.store(false, std::memory_order_release);
            return false;
        }
        loading_thread_started = true;
        return true;
    }

    void stop_loading() {
        const bool was_active = loading_active.exchange(false, std::memory_order_acq_rel);
        if (loading_thread_started) {
            (void)pthread_join(loading_thread, nullptr);
            loading_thread_started = false;
        }
        if (loading_presented) {
            const int result = native_agc_finish_frame();
            app_log("loading", "finish", result);
            loading_presented = false;
        }
        if (was_active) app_log("loading", "stopped");
    }

    bool init(bool use_hevc, unsigned width, unsigned height, unsigned requested_fps) {
        shutdown();
        hevc = use_hevc;
        max_width = width;
        max_height = padded_height(height);
        fps = requested_fps ? requested_fps : 60;
        app_log_text("videodec2", "mode",
                     std::string(" codec=") + (hevc ? "hevc" : "h264") +
                     " width=" + std::to_string(max_width) +
                     " height=" + std::to_string(height));

        int32_t result = sceSysmoduleLoadModule(kVideoDec2Module);
        app_log("videodec2", "sysmodule_load", result);
        if (result < 0) { fail("sysmodule_load_failed", result); return false; }
        module_loaded = true;

        const int64_t direct_limit = sceKernelGetDirectMemorySize();
        compute_memory.size = sizeof(compute_memory);
        result = sceVideodec2QueryComputeMemoryInfo(&compute_memory);
        app_log("videodec2", "compute_query", result);
        if (result != 0) { fail("compute_query_failed", result); shutdown(); return false; }
        compute_size = align_16k(static_cast<size_t>(compute_memory.cpu_gpu_size));
        result = allocate_direct(compute_size, 0x33, direct_limit, compute_start,
                                 compute_memory.cpu_gpu);
        if (result == 0) {
            compute_memory.cpu_gpu_size = compute_size;
            VideoDec2ComputeConfig compute_config{};
            compute_config.size = sizeof(compute_config);
            result = sceVideodec2AllocateComputeQueue(&compute_config, &compute_memory,
                                                       &compute_queue);
        }
        app_log("videodec2", "compute_queue", result);
        if (result != 0) { fail("compute_queue_failed", result); shutdown(); return false; }

        VideoDec2DecoderConfig config{};
        config.size = sizeof(config);
        config.resource_type = 1;
        config.codec_type = hevc ? 0x000ee049u : 1u;
        config.profile = hevc ? 1u : 100u;
        config.max_level = decoder_level(hevc, height);
        config.max_width = static_cast<int32_t>(width);
        config.max_height = static_cast<int32_t>(max_height);
        // PS Now's PS4 H.264 sequence advertises a larger decoded-picture
        // buffer than the low-latency PS5 HEVC stream.  Four frames works for
        // HEVC but makes VideoDec2 reject every PS4 IDR with 0x811d0302
        // (OVERSIZE_DECODE).  Allocate the full AVC DPB so adaptive profiles
        // can also switch without recreating the decoder.
        config.max_dpb_frames = hevc ? kHevcDpbFrames : kH264DpbFrames;
        config.pipeline_depth = kPipelineDepth;
        config.compute_queue = reinterpret_cast<uint64_t>(compute_queue);
        config.cpu_affinity = 0x3f;
        config.cpu_priority = 700;
        config.optimize_progressive = 1;
        app_log_text("videodec2", "config",
                     " codec=" + std::string(hevc ? "hevc" : "h264") +
                     " profile=" + std::to_string(config.profile) +
                     " level=" + std::to_string(config.max_level) +
                     " max_width=" + std::to_string(config.max_width) +
                     " max_height=" + std::to_string(config.max_height) +
                     " max_dpb=" + std::to_string(config.max_dpb_frames));
        memory = {};
        memory.size = sizeof(memory);
        result = sceVideodec2QueryDecoderMemoryInfo(&config, &memory);
        app_log("videodec2", "decoder_query", result);
        if (result != 0) { fail("decoder_query_failed", result); shutdown(); return false; }
        app_log_text("videodec2", "memory",
                     " cpu=" + std::to_string(memory.cpu_size) +
                     " gpu=" + std::to_string(memory.gpu_size) +
                     " shared=" + std::to_string(memory.cpu_gpu_size) +
                     " frame=" + std::to_string(memory.max_frame_size));

        cpu_mapping_size = align_16k(static_cast<size_t>(memory.cpu_size));
        size_t flexible_available = 0;
        result = sceKernelAvailableFlexibleMemorySize(&flexible_available);
        if (result == 0)
            result = sceKernelMapNamedFlexibleMemory(&memory.cpu, cpu_mapping_size, 0x03, 0,
                                                     "CloudPlayVdecCpu");
        app_log("videodec2", "cpu_workspace", result);
        if (result != 0) { fail("cpu_workspace_failed", result); shutdown(); return false; }

        gpu_size = align_16k(static_cast<size_t>(memory.gpu_size));
        cpu_gpu_size = align_16k(static_cast<size_t>(memory.cpu_gpu_size));
        frame_size = align_16k(static_cast<size_t>(memory.max_frame_size));
        input_pool_size = input_size * kPoolSlots;
        frame_pool_size = frame_size * kPoolSlots;
        memory.gpu_size = gpu_size;
        memory.cpu_gpu_size = cpu_gpu_size;
        result = allocate_direct(gpu_size, 0x32, direct_limit, gpu_start, memory.gpu);
        if (result == 0)
            result = allocate_direct(cpu_gpu_size, 0x33, direct_limit, cpu_gpu_start,
                                     memory.cpu_gpu);
        if (result == 0)
            result = allocate_direct(input_pool_size, 0x32, direct_limit, input_start,
                                     input_memory);
        if (result == 0)
            result = allocate_direct(frame_pool_size, 0x32, direct_limit, frame_start,
                                     frame_memory);
        app_log("videodec2", "native_pools", result);
        if (result != 0) { fail("native_pool_failed", result); shutdown(); return false; }

        result = sceVideodec2CreateDecoder(&config, &memory, &decoder);
        app_log("videodec2", "decoder_create", result);
        if (result == 0) result = sceVideodec2Reset(decoder);
        app_log("videodec2", "decoder_reset", result);
        if (result != 0) { fail("decoder_create_failed", result); shutdown(); return false; }

        native_agc_reset_performance();
        native_agc_set_hud_enabled(stream_stats_enabled ? 1 : 0);
        native_agc_set_tv_safe_area(0);
        ready = true;
        need_header = true;
        access_units = presented = decode_failures = 0;
        incoming_frames = reported_lost_frames = 0;
        decode_total_us = decode_samples = 0;
        processing_total_us = processing_samples = 0;
        processing_min_us = processing_max_us = 0;
        incoming_rate = {};
        rendering_rate = {};
        last_error.clear();
        app_log("videodec2", "ready", 1);
        app_log_allocator("videodec2", "allocator_ready");
        return true;
    }

    bool decode(const uint8_t *data, size_t size, int32_t frames_lost,
                bool frame_recovered) {
        if (!ready || !decoder || !data || !size || size > input_size) return false;
        if (!contains_vcl(data, size, hevc)) {
            if (size <= 256 * 1024) {
                header.assign(data, data + size);
                need_header = true;
                app_log("videodec2", "parameter_sets", static_cast<int>(size));
                return true;
            }
        }

        // Keep the animated status visible through HTTPS allocation and the
        // stream handshake.  The first compressed video access unit takes
        // ownership of AGC only after the loading submission has completed.
        stop_loading();

        const uint64_t frame_started_us = monotonic_us();
        ++incoming_frames;
        if (frames_lost > 0)
            reported_lost_frames += static_cast<uint64_t>(frames_lost);
        incoming_rate.update(frame_started_us, incoming_frames);

        const size_t prefix = need_header ? header.size() : 0;
        if (prefix > input_size || size > input_size - prefix) {
            fail("access_unit_too_large", -1);
            return false;
        }
        const unsigned slot = access_units % kPoolSlots;
        auto *input_slot = static_cast<uint8_t *>(input_memory) + slot * input_size;
        void *frame_slot = static_cast<uint8_t *>(frame_memory) + slot * frame_size;
        const int wait_result = native_agc_wait_source_idle(frame_slot);
        if (wait_result != 0) {
            fail("source_wait_failed", wait_result);
            return false;
        }
        if (prefix) std::memcpy(input_slot, header.data(), prefix);
        std::memcpy(input_slot + prefix, data, size);

        VideoDec2Input input{};
        input.size = sizeof(input);
        input.au = input_slot;
        input.au_size = prefix + size;
        input.pts = static_cast<uint64_t>(access_units) * 1000000u / fps;
        input.dts = std::numeric_limits<uint64_t>::max();
        VideoDec2Frame frame{};
        frame.size = sizeof(frame);
        frame.buffer = frame_slot;
        frame.buffer_size = frame_size;
        VideoDec2Output output{};
        output.size = sizeof(output);

        const uint64_t decode_started_us = monotonic_us();
        int32_t result = sceVideodec2Decode(decoder, &input, &frame, &output);
        const bool trace = access_units < 4;
        if (trace) {
            app_log("videodec2", "decode", result);
            app_log_text("videodec2", "decode_output",
                         " accepted=" + std::to_string(frame.accepted) +
                         " valid=" + std::to_string(output.valid) +
                         " error=" + std::to_string(output.error) +
                         " pictures=" + std::to_string(output.picture_count));
        }
        ++access_units;
        if (result == 0 && !output.valid && !output.error) {
            output = {};
            output.size = sizeof(output);
            result = sceVideodec2Flush(decoder, &frame, &output);
            if (trace) app_log("videodec2", "flush", result);
        }
        const uint64_t decode_finished_us = monotonic_us();
        if (decode_started_us && decode_finished_us >= decode_started_us) {
            decode_total_us += decode_finished_us - decode_started_us;
            ++decode_samples;
        }
        if (result != 0 || output.error) {
            ++decode_failures;
            const bool trace_failure = decode_failures <= 8 ||
                (decode_failures & (decode_failures - 1)) == 0;
            if (trace_failure) {
                app_log_text("videodec2", "decode_failed",
                             " count=" + std::to_string(decode_failures) +
                             " rc=" + std::to_string(result) +
                             " rc_hex=0x" + [&] {
                                 char value[16]{};
                                 std::snprintf(value, sizeof(value), "%08x",
                                               static_cast<uint32_t>(result));
                                 return std::string(value);
                             }() +
                             " native_error=" + std::to_string(output.error) +
                             " lost=" + std::to_string(frames_lost) +
                             " recovered=" + std::to_string(frame_recovered ? 1 : 0));
                if (static_cast<uint32_t>(result) == kVideoDec2OversizeDecode)
                    app_log_text("videodec2", "oversize_decode",
                                 " max_width=" + std::to_string(max_width) +
                                 " max_height=" + std::to_string(max_height) +
                                 " max_dpb=" + std::to_string(hevc ? kHevcDpbFrames :
                                                                  kH264DpbFrames));
            }
            (void)sceVideodec2Reset(decoder);
            need_header = true;
            return false;
        }
        if (!output.valid) {
            need_header = prefix ? false : need_header;
            return frame.accepted != 0;
        }
        const uint32_t expected_codec = hevc ? 0x000ee049u : 1u;
        if (!frame.accepted || output.picture_count != 1 || output.codec != expected_codec ||
            !frame_in_pool(output.buffer, frame_memory, frame_size) || !output.width ||
            !output.height || output.width > max_width || output.height > max_height ||
            output.pitch < output.width || !output.buffer_size ||
            output.buffer_size > frame_size) {
            app_log_text("videodec2", "output_rejected",
                         " accepted=" + std::to_string(frame.accepted) +
                         " valid=" + std::to_string(output.valid) +
                         " codec=" + std::to_string(output.codec) +
                         " width=" + std::to_string(output.width) +
                         " height=" + std::to_string(output.height) +
                         " pitch=" + std::to_string(output.pitch) +
                         " bytes=" + std::to_string(output.buffer_size));
            (void)sceVideodec2Reset(decoder);
            need_header = true;
            return false;
        }

        const uint64_t processing_finished_us = monotonic_us();
        uint64_t processing_us = 0;
        if (frame_started_us && processing_finished_us >= frame_started_us) {
            processing_us = processing_finished_us - frame_started_us;
            processing_total_us += processing_us;
            ++processing_samples;
            if (!processing_min_us || processing_us < processing_min_us)
                processing_min_us = processing_us;
            if (processing_us > processing_max_us) processing_max_us = processing_us;
        }
        rendering_rate.update(processing_finished_us, static_cast<uint64_t>(presented) + 1u);

        native_agc_metrics_t metrics{};
        const native_agc_metrics_t *metrics_ptr = nullptr;
        if (stream_stats_enabled) {
            const uint64_t received = packets_received.load(std::memory_order_relaxed);
            const uint64_t lost = packets_lost.load(std::memory_order_relaxed);
            const uint64_t packet_total = received + lost;
            const uint64_t frame_total = incoming_frames + reported_lost_frames;
            const uint64_t loss_total = packet_total ? packet_total : frame_total;
            const uint64_t loss_count = packet_total ? lost : reported_lost_frames;
            const uint64_t rtt = stream_rtt_us.load(std::memory_order_relaxed);
            const uint64_t feedback = feedback_send_delay_us.load(std::memory_order_relaxed);
            metrics.video_codec = hevc ? 1u : 0u;
            metrics.total_fps_x100 = rendering_rate.fps_x100;
            metrics.incoming_fps_x100 = incoming_rate.fps_x100;
            metrics.rendering_fps_x100 = rendering_rate.fps_x100;
            metrics.network_drop_percent_x100 = loss_total ? static_cast<uint32_t>(
                std::min<uint64_t>(10000u, loss_count * 10000u / loss_total)) : 0u;
            metrics.rtt_ms = static_cast<uint32_t>(rtt / 1000u);
            metrics.rtt_variance_ms = 0;
            metrics.rtt_valid = rtt ? 1u : 0u;
            metrics.host_min_tenths_ms = static_cast<uint32_t>(processing_min_us / 100u);
            metrics.host_max_tenths_ms = static_cast<uint32_t>(processing_max_us / 100u);
            metrics.host_average_tenths_ms = processing_samples ? static_cast<uint32_t>(
                processing_total_us / processing_samples / 100u) : 0u;
            metrics.stale_presentation_drops = decode_failures;
            metrics.decode_average_us = decode_samples ? decode_total_us / decode_samples : 0u;
            metrics.queue_delay_average_us = 0;
            metrics.queue_delay_max_us = 0;
            metrics.feedback_send_delay_us = feedback;
            metrics.input_estimate_us = feedback + rtt / 2u;
            metrics_ptr = &metrics;
        }

        agc_used = true;
        result = native_agc_present_nv12(output.buffer, static_cast<size_t>(output.buffer_size),
                                         output.pitch, output.height, output.width,
                                         visible_height(output.height), fps, metrics_ptr);
        if (result != 0) {
            fail("agc_present_failed", result);
            return false;
        }
        need_header = false;
        ++presented;
        if (presented == 1) {
            app_log_text("videodec2", "first_frame",
                         " width=" + std::to_string(output.width) +
                         " height=" + std::to_string(visible_height(output.height)) +
                         " pitch=" + std::to_string(output.pitch));
        }
        return true;
    }

    void shutdown() {
        ready = false;
        stop_loading();
        int source_idle = 0;
        int agc_result = 0;
        if (agc_used) {
            source_idle = native_agc_finish_frame();
            agc_result = native_agc_present_shutdown();
            app_log("videodec2", "agc_finish", source_idle);
            app_log("videodec2", "agc_shutdown", agc_result);
        }
        if (source_idle != 0) {
            // AGC may still own a decoder surface. Preserve all native memory
            // until process exit instead of risking a GPU use-after-free.
            fail("agc_surface_busy", source_idle);
            return;
        }
        if (decoder) {
            const int32_t result = sceVideodec2DeleteDecoder(decoder);
            app_log("videodec2", "decoder_delete", result);
            if (result != 0) {
                fail("decoder_delete_failed", result);
                return;
            }
            decoder = nullptr;
        }
        release_direct(frame_memory, frame_start, frame_pool_size);
        release_direct(input_memory, input_start, input_pool_size);
        release_direct(loading_memory, loading_start, loading_size);
        release_direct(memory.cpu_gpu, cpu_gpu_start, cpu_gpu_size);
        release_direct(memory.gpu, gpu_start, gpu_size);
        if (memory.cpu) {
            (void)sceKernelReleaseFlexibleMemory(memory.cpu, cpu_mapping_size);
            (void)sceKernelMunmap(memory.cpu, cpu_mapping_size);
            memory.cpu = nullptr;
        }
        if (compute_queue) {
            app_log("videodec2", "compute_release",
                    sceVideodec2ReleaseComputeQueue(compute_queue));
            compute_queue = nullptr;
        }
        release_direct(compute_memory.cpu_gpu, compute_start, compute_size);
        if (module_loaded) {
            app_log("videodec2", "sysmodule_unload",
                    sceSysmoduleUnloadModule(kVideoDec2Module));
            module_loaded = false;
        }
        memory = {};
        compute_memory = {};
        header.clear();
        agc_used = false;
        need_header = true;
        input_pool_size = frame_pool_size = frame_size = 0;
        loading_size = 0;
    }
};

Ps5VideoDecoder::Ps5VideoDecoder() : impl_(new Impl) {}
Ps5VideoDecoder::~Ps5VideoDecoder() { impl_->shutdown(); }

bool Ps5VideoDecoder::initialize(bool hevc, unsigned width, unsigned height, unsigned fps) {
    return impl_->init(hevc, width, height, fps);
}

void Ps5VideoDecoder::set_stream_stats_enabled(bool enabled) {
    impl_->stream_stats_enabled = enabled;
}

void Ps5VideoDecoder::update_stream_stats(uint64_t rtt_us, uint64_t feedback_send_delay_us,
                                          uint64_t packets_received, uint64_t packets_lost) {
    impl_->stream_rtt_us.store(rtt_us, std::memory_order_relaxed);
    impl_->feedback_send_delay_us.store(feedback_send_delay_us, std::memory_order_relaxed);
    impl_->packets_received.store(packets_received, std::memory_order_relaxed);
    impl_->packets_lost.store(packets_lost, std::memory_order_relaxed);
}

bool Ps5VideoDecoder::submit(const uint8_t *data, size_t size, int32_t frames_lost,
                             bool frame_recovered) {
    return impl_->decode(data, size, frames_lost, frame_recovered);
}

bool Ps5VideoDecoder::start_loading(const std::string &status) {
    return impl_->start_loading(status);
}

void Ps5VideoDecoder::set_loading_status(const std::string &status) {
    impl_->set_loading(status);
}

void Ps5VideoDecoder::stop_loading() { impl_->stop_loading(); }

void Ps5VideoDecoder::shutdown() { impl_->shutdown(); }
bool Ps5VideoDecoder::initialized() const { return impl_->ready; }
uint32_t Ps5VideoDecoder::presented_frames() const { return impl_->presented; }
std::string Ps5VideoDecoder::error() const { return impl_->last_error; }

} // namespace cloudplay
