// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace cloudplay {

// Owns the PS5 VideoDec2 decoder and presents its GPU-visible NV12 output
// through AGC. Compressed access units never pass through FFmpeg or a CPU
// pixel conversion path.
class Ps5VideoDecoder {
public:
    Ps5VideoDecoder();
    ~Ps5VideoDecoder();
    Ps5VideoDecoder(const Ps5VideoDecoder &) = delete;
    Ps5VideoDecoder &operator=(const Ps5VideoDecoder &) = delete;

    bool initialize(bool hevc, unsigned width, unsigned height, unsigned fps);
    void set_stream_stats_enabled(bool enabled);
    void update_stream_stats(uint64_t rtt_us, uint64_t feedback_send_delay_us,
                             uint64_t packets_received, uint64_t packets_lost);
    bool submit(const uint8_t *data, size_t size, int32_t frames_lost,
                bool frame_recovered);
    bool start_loading(const std::string &status);
    void set_loading_status(const std::string &status);
    void stop_loading();
    void shutdown();
    bool initialized() const;
    uint32_t presented_frames() const;
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cloudplay
