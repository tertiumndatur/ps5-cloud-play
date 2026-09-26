// SPDX-License-Identifier: GPL-3.0-or-later
#include "cloud_stream_client.hpp"
#include "app_log.hpp"
#include "ps5_video_decoder.hpp"

#if CLOUDPLAY_PS5
#include <chiaki/cloudsession.h>
#include <chiaki/controller.h>
#include <chiaki/ios_bridge_helpers.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/session.h>
#include <chiaki/thread.h>

#include <SDL2/SDL.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>

namespace cloudplay {
namespace {

std::atomic<unsigned> repeated_video_warnings{0};

int16_t stick(uint8_t value) {
    const int centered = static_cast<int>(value) - 128;
    return static_cast<int16_t>(std::clamp(centered * 258, -32767, 32767));
}

void chiaki_log_event(ChiakiLogLevel level, const char *message, void *) {
    const char *event = level == CHIAKI_LOG_ERROR ? "error" :
                        level == CHIAKI_LOG_WARNING ? "warning" : "info";
    std::string safe = message ? message : "";
    if (safe.size() > 400) safe.resize(400);
    for (char &ch : safe)
        if (static_cast<unsigned char>(ch) < 0x20) ch = ' ';
    const bool repeated_video_warning =
        safe.find("Video callback did not process frame") != std::string::npos ||
        safe.find("Missing reference frame") != std::string::npos ||
        safe.find("waiting for IDR frame") != std::string::npos ||
        safe.find("Detected missing frame") != std::string::npos ||
        safe.find("reporting corrupt frame") != std::string::npos;
    if (repeated_video_warning) {
        const unsigned count = repeated_video_warnings.fetch_add(1) + 1;
        if (count > 16 && (count & (count - 1)) != 0) return;
        safe += " repeat=" + std::to_string(count);
    }
    app_log_text("chiaki", event, " message=" + safe);
}

} // namespace

struct CloudStreamClient::Impl {
    CloudGame game;
    CloudVariant variant;
    CloudSettings settings;
    CloudCatalogContext catalog;
    std::string npsso;
    std::string status_text;
    std::string datacenter_pings;
    bool datacenter_pending = false;
    mutable std::mutex mutex;
    std::mutex decoder_mutex;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> running{false};
    std::atomic<bool> first_video_logged{false};
    std::atomic<bool> first_audio_logged{false};
    std::atomic<bool> first_haptics_logged{false};
    std::atomic<unsigned> video_samples_seen{0};
    std::atomic<uint8_t> haptic_intensity{Strong};
    std::atomic<uint8_t> trigger_intensity{Strong};
    ChiakiThread worker{};
    bool worker_started = false;
    // ChiakiSession contains platform-dependent synchronization objects.  It
    // must be allocated by the same libchiaki build that operates on it.
    ChiakiSession *session = nullptr;
    bool session_live = false;
    ChiakiLog log{};
    ChiakiOpusDecoder opus{};
    SDL_AudioDeviceID audio_device = 0;
    unsigned audio_channels = 0;
    unsigned audio_rate = 0;
    uint64_t last_stats_poll_ms = 0;
    Ps5VideoDecoder video_decoder;

    void set_status(std::string value) {
        video_decoder.set_loading_status(value);
        std::lock_guard<std::mutex> lock(mutex);
        status_text = std::move(value);
    }

    static void progress(const char *message, void *user) {
        auto *self = static_cast<Impl *>(user);
        if (message) {
            app_log("stream", "provision_progress");
            self->set_status(message);
        }
    }

    static bool is_cancelled(void *user) {
        return static_cast<Impl *>(user)->cancelled.load(std::memory_order_acquire);
    }

    static void event(ChiakiEvent *value, void *user) {
        auto *self = static_cast<Impl *>(user);
        if (value->type == CHIAKI_EVENT_CONNECTED)
        {
            app_log("stream", "connected");
            self->set_status("Connected - waiting for first video frame");
        }
        else if (value->type == CHIAKI_EVENT_QUIT) {
            (void)radio_input_set_vibration(0, 0);
            (void)radio_input_clear_trigger_effects();
            app_log("stream", "quit", static_cast<int>(value->quit.reason));
            const char *reason = chiaki_quit_reason_string(value->quit.reason);
            self->set_status(std::string("Cloud stream ended: ") + (reason ? reason : "unknown"));
        }
        else if (value->type == CHIAKI_EVENT_RUMBLE) {
            (void)radio_input_set_vibration(value->rumble.left, value->rumble.right);
        }
        else if (value->type == CHIAKI_EVENT_TRIGGER_EFFECTS) {
            const bool applied = radio_input_set_trigger_effects(
                value->trigger_effects.type_left, value->trigger_effects.left,
                value->trigger_effects.type_right, value->trigger_effects.right,
                self->trigger_intensity.load(std::memory_order_relaxed));
            if (!applied) app_log("stream", "trigger_effect_failed");
        }
        else if (value->type == CHIAKI_EVENT_HAPTIC_INTENSITY) {
            self->haptic_intensity.store(static_cast<uint8_t>(value->intensity),
                                         std::memory_order_relaxed);
            if (value->intensity == Off) (void)radio_input_set_vibration(0, 0);
        }
        else if (value->type == CHIAKI_EVENT_TRIGGER_INTENSITY) {
            self->trigger_intensity.store(static_cast<uint8_t>(value->intensity),
                                          std::memory_order_relaxed);
            if (value->intensity == Off) (void)radio_input_clear_trigger_effects();
        }
        else if (value->type == CHIAKI_EVENT_MOTION_RESET) {
            (void)radio_input_reset_orientation();
        }
    }

    static void audio_settings(uint32_t channels, uint32_t rate, void *user) {
        auto *self = static_cast<Impl *>(user);
        self->audio_channels = channels;
        self->audio_rate = rate;
        app_log("stream", "audio_channels", static_cast<int>(channels));
        app_log("stream", "audio_rate", static_cast<int>(rate));
    }

    static void audio_frame(int16_t *samples, size_t count, void *user) {
        auto *self = static_cast<Impl *>(user);
        if (!self->audio_device || !samples || self->audio_channels != 2 ||
            self->audio_rate != 48000 || count > 48000) return;
        if (!self->first_audio_logged.exchange(true)) app_log("stream", "first_audio");
        if (SDL_GetQueuedAudioSize(self->audio_device) > 48000 * 2 * 2)
            SDL_ClearQueuedAudio(self->audio_device);
        SDL_QueueAudio(self->audio_device, samples,
                       static_cast<Uint32>(count * self->audio_channels * sizeof(int16_t)));
    }

    static void haptics_frame(uint8_t *data, size_t size, void *user) {
        auto *self = static_cast<Impl *>(user);
        if (!data || size == 0 || (size % (2U * sizeof(int16_t))) != 0)
            return;
        if (!self->first_haptics_logged.exchange(true))
            app_log("stream", "first_dualsense_haptics", static_cast<int>(size));
        (void)radio_input_set_haptic_audio(
            reinterpret_cast<const int16_t *>(data),
            size / (2U * sizeof(int16_t)),
            self->haptic_intensity.load(std::memory_order_relaxed));
    }

    static bool video_sample(uint8_t *data, size_t size, int32_t lost,
                             bool recovered, void *user) {
        auto *self = static_cast<Impl *>(user);
        const unsigned sample_index = self->video_samples_seen.fetch_add(1, std::memory_order_relaxed);
        const bool trace_sample = sample_index < 4;
        if (trace_sample) {
            app_log("stream", "video_sample_enter", static_cast<int>(size));
            app_log("stream", "video_sample_index", static_cast<int>(sample_index));
        }
        if (!self->video_decoder.initialized() || !data || !size || size > 16 * 1024 * 1024)
            return false;
        std::lock_guard<std::mutex> decode_lock(self->decoder_mutex);
        const uint32_t before = self->video_decoder.presented_frames();
        const bool success = self->video_decoder.submit(data, size, lost, recovered);
        if (success && self->video_decoder.presented_frames() > before &&
            !self->first_video_logged.exchange(true)) {
            app_log("stream", "first_video_native");
            self->set_status("Cloud stream playing with PS5 hardware decode");
        }
        if (trace_sample) app_log("stream", "video_sample_done", success ? 1 : 0);
        return success;
    }

    bool prepare_decoder() {
        const bool ps5 = variant.service_type == "pscloud";
        const ServiceSettings &service = ps5 ? settings.ps5_cloud : settings.ps_now;
        app_log_text("stream", "decoder_codec",
                     ps5 ? " name=hevc backend=sceVideodec2" :
                           " name=h264 backend=sceVideodec2");
        app_log_allocator("stream", "allocator_before_decoder");
        const unsigned width = service.resolution == 2160 ? 3840 :
            service.resolution == 1440 ? 2560 : service.resolution == 720 ? 1280 : 1920;
        video_decoder.set_stream_stats_enabled(settings.show_stream_stats);
        if (!video_decoder.initialize(ps5, width, service.resolution, 60)) {
            set_status(std::string("PS5 hardware decoder failed: ") + video_decoder.error());
            app_log("stream", "native_decoder_init_failed");
            return false;
        }
        const bool loading_started = video_decoder.start_loading(
            ps5 ? "Preparing PS5 cloud stream" : "Preparing PS4 cloud stream");
        app_log("stream", "loading_started", loading_started ? 1 : 0);
        app_log_allocator("stream", "allocator_after_decoder");
        return true;
    }

    void release_decoder() {
        std::lock_guard<std::mutex> lock(decoder_mutex);
        video_decoder.shutdown();
    }

    void run() {
        app_log("stream", "worker_started");
        if (!prepare_decoder()) {
            app_log("stream", "decoder_failed");
            release_decoder();
            running.store(false);
            return;
        }
        app_log("stream", "decoder_ready");
        chiaki_log_init(&log, CHIAKI_LOG_INFO | CHIAKI_LOG_WARNING | CHIAKI_LOG_ERROR,
                        chiaki_log_event, this);
        chiaki_opus_decoder_init(&opus, &log);
        chiaki_opus_decoder_set_cb(&opus, audio_settings, audio_frame, this);
        const bool ps5 = variant.service_type == "pscloud";
        const ServiceSettings &service = ps5 ? settings.ps5_cloud : settings.ps_now;
        const std::string locale = catalog.locale.empty() ? "en-US" : catalog.locale;
        const std::string country = catalog.store_country.empty() ?
            (locale.size() >= 5 ? locale.substr(locale.size() - 2) : "US") :
            catalog.store_country;
        const std::string language = catalog.store_language.empty() ?
            locale.substr(0, 2) : catalog.store_language;
        const std::string game_language = service.game_language.empty() ?
            locale : service.game_language;
        ChiakiCloudProvisionConfig config{};
        config.service_type = variant.service_type.c_str();
        config.game_identifier = variant.identifier.c_str();
        config.game_name = game.name.c_str();
        config.concept_id = game.concept_id.c_str();
        config.npsso = npsso.c_str();
        config.store_country = country.c_str();
        config.store_lang = language.c_str();
        config.owned_entitlement_id = variant.is_owned ? variant.entitlement_id.c_str() : "";
        config.owned_platform = variant.is_owned ? variant.platform.c_str() : "";
        config.plus_catalog = variant.plus_catalog;
        config.catalog_is_foreign = catalog.foreign;
        config.forced_datacenter = service.datacenter == "Auto" ? "" :
            service.datacenter.c_str();
        config.prior_datacenters_json = service.datacenters_json.c_str();
        config.game_language = game_language.c_str();
        config.resolution = service.resolution;
        config.bitrate_kbps = service.bitrate_kbps;
        config.progress = progress;
        config.is_cancelled = is_cancelled;
        config.user = this;
        ChiakiCloudProvisionResult result{};
        set_status("Allocating Cloud Play session via HTTPS");
        app_log("stream", "provision_started");
        const ChiakiErrorCode provision_error =
            chiaki_cloud_provision_session(&config, &result, &log);
        app_log("stream", "provision_result", static_cast<int>(provision_error));
        std::fill(npsso.begin(), npsso.end(), '\0');
        npsso.clear();
        if (result.datacenter_pings && std::strlen(result.datacenter_pings) <= 8192) {
            std::lock_guard<std::mutex> lock(mutex);
            datacenter_pings = result.datacenter_pings;
            datacenter_pending = true;
        }
        if (provision_error != CHIAKI_ERR_SUCCESS || cancelled.load()) {
            if (!cancelled.load()) set_status(result.error_message ? result.error_message :
                chiaki_error_string(provision_error));
            chiaki_cloud_provision_result_fini(&result);
            chiaki_opus_decoder_fini(&opus);
            release_decoder();
            running.store(false);
            return;
        }
        in_addr stream_address{};
        app_log("stream", "allocated_host_is_ipv4",
                inet_pton(AF_INET, result.server_ip, &stream_address) == 1);
        app_log("stream", "allocated_port", result.server_port);
        ChiakiConnectInfo connect{};
        connect.ps5 = ps5;
        connect.enable_dualsense = ps5;
        connect.host = result.server_ip;
        connect.service_type = ps5 ? CHIAKI_SERVICE_TYPE_PSCLOUD : CHIAKI_SERVICE_TYPE_PSNOW;
        connect.video_profile.width = service.resolution == 2160 ? 3840 :
            service.resolution == 1440 ? 2560 : service.resolution == 720 ? 1280 : 1920;
        connect.video_profile.height = service.resolution;
        connect.video_profile.max_fps = 60;
        connect.video_profile.bitrate = service.bitrate_kbps;
        connect.video_profile.codec = ps5 ? CHIAKI_CODEC_H265 : CHIAKI_CODEC_H264;
        connect.video_profile_auto_downgrade = true;
        switch (service.video_recovery_mode) {
        case VideoRecoveryMode::Simple:
            connect.video_recovery_mode = CHIAKI_VIDEO_RECOVERY_MODE_SIMPLE;
            break;
        case VideoRecoveryMode::Hybrid:
            connect.video_recovery_mode = CHIAKI_VIDEO_RECOVERY_MODE_HYBRID;
            break;
        case VideoRecoveryMode::IDR:
        default:
            connect.video_recovery_mode = CHIAKI_VIDEO_RECOVERY_MODE_IDR;
            break;
        }
        app_log("stream", "video_recovery_mode",
                static_cast<int>(connect.video_recovery_mode));
        connect.cloud_launch_spec = result.launch_spec;
        connect.cloud_handshake_key = result.handshake_key;
        connect.cloud_session_id = result.session_id;
        connect.cloud_port = static_cast<uint16_t>(result.server_port);
        connect.cloud_psn_wrapper_type = result.psn_wrapper_type;
        connect.cloud_mtu_in = result.mtu_in;
        connect.cloud_mtu_out = result.mtu_out;
        connect.cloud_rtt_us = result.rtt_us;
        video_decoder.update_stream_stats(result.rtt_us, 0, 0, 0);
        const size_t library_session_size = chiaki_session_get_sizeof();
        app_log("stream", "session_size_app", static_cast<int>(sizeof(ChiakiSession)));
        app_log("stream", "session_size_library", static_cast<int>(library_session_size));
        app_log("stream", "session_opaque_abi_bridge",
                library_session_size == sizeof(ChiakiSession) ? 0 : 1);
        session = chiaki_session_new_ex();
        if (!session) {
            set_status("Could not allocate streaming session");
            app_log("stream", "session_alloc_failed");
            chiaki_cloud_provision_result_fini(&result);
            chiaki_opus_decoder_fini(&opus);
            release_decoder();
            running.store(false, std::memory_order_release);
            return;
        }
        app_log("stream", "session_allocated");
        const ChiakiErrorCode init_error = chiaki_session_init(session, &connect, &log);
        app_log("stream", "session_init_result", static_cast<int>(init_error));
        if (init_error != CHIAKI_ERR_SUCCESS) {
            set_status(std::string("Streaming session init failed: ") + chiaki_error_string(init_error));
            chiaki_session_delete_ex(session);
            session = nullptr;
        } else {
            // These exported wrappers compile inside libchiaki and therefore
            // use the library's exact ChiakiSession layout.  The inline
            // setters are unsafe across a separately built ABI boundary.
            chiaki_session_set_video_sample_cb_ex(session, video_sample, this);
            chiaki_session_set_event_cb_ex(session, event, this);
            ChiakiAudioSink sink{};
            chiaki_opus_decoder_get_sink(&opus, &sink);
            chiaki_session_set_audio_sink_ex(session, &sink);
            ChiakiAudioSink haptics_sink{};
            haptics_sink.user = this;
            haptics_sink.frame_cb = haptics_frame;
            chiaki_session_set_haptics_sink_ex(session, &haptics_sink);
            {
                std::lock_guard<std::mutex> lock(mutex);
                session_live = true;
            }
            set_status("Connecting to Cloud Play stream");
            const ChiakiErrorCode start_error = chiaki_session_start(session);
            app_log("stream", "session_start_result", static_cast<int>(start_error));
            if (start_error == CHIAKI_ERR_SUCCESS) {
                if (cancelled.load()) chiaki_session_stop(session);
                chiaki_session_join(session);
                app_log("stream", "session_joined");
            } else {
                set_status(std::string("Stream start failed: ") +
                           chiaki_error_string(start_error));
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                session_live = false;
            }
            chiaki_session_fini(session);
            chiaki_session_delete_ex(session);
            session = nullptr;
        }
        chiaki_cloud_provision_result_fini(&result);
        chiaki_opus_decoder_fini(&opus);
        release_decoder();
        running.store(false, std::memory_order_release);
        app_log("stream", "worker_finished");
    }

    static void *run_thread(void *user) {
        static_cast<Impl *>(user)->run();
        return nullptr;
    }
};

CloudStreamClient::CloudStreamClient() : impl_(new Impl) {}
CloudStreamClient::~CloudStreamClient() {
    stop();
    impl_->release_decoder();
    if (impl_->audio_device) SDL_CloseAudioDevice(impl_->audio_device);
}

bool CloudStreamClient::start(const CloudGame &game, const CloudVariant &variant,
                              const CloudSettings &settings,
                              const CloudCatalogContext &catalog,
                              const std::string &npsso) {
    if (impl_->running.load() || npsso.empty() || variant.identifier.empty()) return false;
    app_log("stream", "start_requested");
    repeated_video_warnings.store(0);
    if (impl_->worker_started) {
        chiaki_thread_join(&impl_->worker, nullptr);
        impl_->worker_started = false;
    }
    impl_->game = game;
    impl_->variant = variant;
    impl_->settings = settings;
    impl_->catalog = catalog;
    impl_->npsso = npsso;
    impl_->cancelled.store(false);
        impl_->first_video_logged.store(false);
        impl_->first_audio_logged.store(false);
        impl_->first_haptics_logged.store(false);
        impl_->video_samples_seen.store(0);
        impl_->haptic_intensity.store(Strong);
        impl_->trigger_intensity.store(Strong);
        impl_->last_stats_poll_ms = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->datacenter_pings.clear();
        impl_->datacenter_pending = false;
    }
    impl_->set_status("Starting Cloud Play");
    if (!impl_->audio_device) {
        SDL_AudioSpec requested{};
        requested.freq = 48000;
        requested.format = AUDIO_S16SYS;
        requested.channels = 2;
        requested.samples = 1024;
        impl_->audio_device = SDL_OpenAudioDevice(nullptr, 0, &requested, nullptr, 0);
        if (impl_->audio_device) SDL_PauseAudioDevice(impl_->audio_device, 0);
    }
    impl_->running.store(true);
    if (chiaki_thread_create(&impl_->worker, Impl::run_thread, impl_.get()) != CHIAKI_ERR_SUCCESS) {
        app_log("stream", "worker_start_failed");
        impl_->running.store(false);
        impl_->set_status("Could not start cloud worker");
        return false;
    }
    impl_->worker_started = true;
    return true;
}

void CloudStreamClient::stop() {
    if (impl_->worker_started) app_log("stream", "stop_requested");
    impl_->cancelled.store(true);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->session_live && impl_->session) chiaki_session_stop(impl_->session);
    }
    if (impl_->worker_started) {
        chiaki_thread_join(&impl_->worker, nullptr);
        impl_->worker_started = false;
    }
    impl_->running.store(false);
    (void)radio_input_set_vibration(0, 0);
    (void)radio_input_clear_trigger_effects();
    if (impl_->audio_device) SDL_ClearQueuedAudio(impl_->audio_device);
}

bool CloudStreamClient::active() const { return impl_->running.load(); }

std::string CloudStreamClient::status() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->status_text;
}

bool CloudStreamClient::take_datacenters(std::string &service_type,
                                         std::string &pings_json) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->datacenter_pending) return false;
    service_type = impl_->variant.service_type;
    pings_json = std::move(impl_->datacenter_pings);
    impl_->datacenter_pending = false;
    return true;
}

void CloudStreamClient::send_pad(const radio_input_pad_state_t &pad) {
    ChiakiControllerState state{};
    chiaki_controller_state_set_idle(&state);
    if (pad.connected) {
        const uint32_t map[][2] = {
            {0x4000, CHIAKI_CONTROLLER_BUTTON_CROSS},
            {0x2000, CHIAKI_CONTROLLER_BUTTON_MOON},
            {0x8000, CHIAKI_CONTROLLER_BUTTON_BOX},
            {0x1000, CHIAKI_CONTROLLER_BUTTON_PYRAMID},
            {0x10, CHIAKI_CONTROLLER_BUTTON_DPAD_UP},
            {0x40, CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN},
            {0x80, CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT},
            {0x20, CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT},
            {0x400, CHIAKI_CONTROLLER_BUTTON_L1},
            {0x800, CHIAKI_CONTROLLER_BUTTON_R1},
            {0x2, CHIAKI_CONTROLLER_BUTTON_L3},
            {0x4, CHIAKI_CONTROLLER_BUTTON_R3},
            {0x8, CHIAKI_CONTROLLER_BUTTON_OPTIONS},
            {0x1, CHIAKI_CONTROLLER_BUTTON_SHARE},
            {0x100000, CHIAKI_CONTROLLER_BUTTON_TOUCHPAD},
        };
        for (const auto &entry : map)
            if (pad.buttons & entry[0]) state.buttons |= entry[1];
        state.l2_state = pad.left_trigger;
        state.r2_state = pad.right_trigger;
        state.left_x = stick(pad.left_x);
        state.left_y = stick(pad.left_y);
        state.right_x = stick(pad.right_x);
        state.right_y = stick(pad.right_y);
        for (uint8_t i = 0; i < pad.touch_count && i < CHIAKI_CONTROLLER_TOUCHES_MAX; ++i) {
            state.touches[i].id = pad.touches[i].id;
            state.touches[i].x = pad.touches[i].x;
            state.touches[i].y = pad.touches[i].y;
        }
        state.gyro_x = pad.angular_velocity_x;
        state.gyro_y = pad.angular_velocity_y;
        state.gyro_z = pad.angular_velocity_z;
        state.accel_x = pad.acceleration_x;
        state.accel_y = pad.acceleration_y;
        state.accel_z = pad.acceleration_z;
        state.orient_x = pad.orientation_x;
        state.orient_y = pad.orientation_y;
        state.orient_z = pad.orientation_z;
        state.orient_w = pad.orientation_w;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->session_live && impl_->session) {
        chiaki_session_set_controller_state(impl_->session, &state);
        const uint64_t now_ms = SDL_GetTicks64();
        if (impl_->settings.show_stream_stats &&
            (!impl_->last_stats_poll_ms || now_ms - impl_->last_stats_poll_ms >= 500u)) {
            uint64_t rtt_us = 0;
            uint64_t feedback_send_delay_us = 0;
            uint64_t packets_received = 0;
            uint64_t packets_lost = 0;
            chiaki_session_get_stream_stats_ex(impl_->session, &rtt_us,
                &feedback_send_delay_us, &packets_received, &packets_lost);
            impl_->video_decoder.update_stream_stats(rtt_us, feedback_send_delay_us,
                                                      packets_received, packets_lost);
            impl_->last_stats_poll_ms = now_ms;
        }
    }
}

} // namespace cloudplay
#endif
