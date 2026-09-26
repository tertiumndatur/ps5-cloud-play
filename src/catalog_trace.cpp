// SPDX-License-Identifier: GPL-3.0-or-later
#include "app_log.hpp"

#if CLOUDPLAY_PS5
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>

namespace {

std::mutex &psn_http_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::string lower(std::string_view input) {
    std::string result(input);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

bool sensitive(std::string_view input) {
    const std::string value = lower(input);
    static constexpr std::string_view markers[] = {
        "npsso", "token", "secret", "authorization", "cookie", "bearer",
        "password", "code=", "sessionid", "https://", "http://", "?"
    };
    for (const auto marker : markers)
        if (value.find(marker) != std::string::npos) return true;
    unsigned run = 0;
    for (const unsigned char ch : input) {
        run = std::isalnum(ch) ? run + 1 : 0;
        if (run >= 24) return true;
    }
    return false;
}

std::string safe_value(std::string_view input, size_t maximum = 240) {
    if (sensitive(input)) return "[redacted]";
    std::string result;
    for (const unsigned char ch : input.substr(0, maximum))
        result += ch >= 0x20 && ch != 0x7f ? static_cast<char>(ch) : ' ';
    if (input.size() > maximum) result += " [truncated]";
    return result;
}

std::string request_name(int id) { return "http#" + std::to_string(id); }

std::string_view trim_line(std::string_view input) {
    while (!input.empty() && (input.back() == '\n' || input.back() == '\r'))
        input.remove_suffix(1);
    return input;
}

bool allowed_header(std::string_view line, bool incoming) {
    const std::string value = lower(line);
    if (value.rfind("http/", 0) == 0) return true;
    static constexpr std::string_view names[] = {
        "host:", "accept:", "user-agent:", "content-type:", "content-length:",
        "server:", "date:", "retry-after:", "cache-control:",
        "x-request-id:", "x-correlation-id:", "x-psn-request-id:"
    };
    for (const auto name : names) {
        if (value.rfind(name, 0) == 0) {
            if (!incoming && (name == "server:" || name == "date:")) return false;
            return true;
        }
    }
    return false;
}

bool allowed_curl_text(std::string_view line) {
    const std::string value = lower(line);
    static constexpr std::string_view prefixes[] = {
        "trying ", "connected to ", "could not resolve", "failed to connect",
        "connection #", "closing connection", "alpn", "ssl", "tls",
        "certificate", "subject:", "issuer:", "host ", "ipv4:", "ipv6:",
        "operation timed out", "resolve", "recached", "connect to"
    };
    for (const auto prefix : prefixes)
        if (value.rfind(prefix, 0) == 0) return true;
    return false;
}

void log_error_fields(const std::string &component, const nlohmann::json &value) {
    if (!value.is_object()) return;
    static constexpr const char *fields[] = {
        "error", "error_description", "message", "reason", "status", "code"
    };
    for (const char *field : fields) {
        auto found = value.find(field);
        if (found == value.end()) continue;
        if (found->is_string())
            cloudplay::app_log_text(component.c_str(), field,
                " value=" + safe_value(found->get_ref<const std::string &>(), 160));
        else if (found->is_number_integer())
            cloudplay::app_log(component.c_str(), field, found->get<int>());
        else if (found->is_object()) log_error_fields(component, *found);
    }
}

} // namespace

extern "C" void cloudplay_psn_http_lock(void) {
    psn_http_mutex().lock();
}

extern "C" void cloudplay_psn_http_unlock(void) {
    psn_http_mutex().unlock();
}

extern "C" void cloudplay_http_request(int id, const char *method, const char *url) {
    std::string host;
    if (url) {
        const std::string_view address(url);
        const size_t scheme = address.find("://");
        if (scheme != std::string_view::npos) {
            const size_t start = scheme + 3;
            const size_t end = address.find_first_of("/:?#", start);
            const auto candidate = address.substr(start, end - start);
            if (candidate.size() <= 253 && !candidate.empty() &&
                std::all_of(candidate.begin(), candidate.end(), [](unsigned char ch) {
                    return std::isalnum(ch) || ch == '.' || ch == '-';
                })) host.assign(candidate);
        }
    }
    const auto component = request_name(id);
    cloudplay::app_log_text(component.c_str(), "request",
        " method=" + safe_value(method ? method : "", 8) +
        " host=" + (host.empty() ? "[invalid]" : host));
}

extern "C" void cloudplay_http_debug(int id, int kind, const char *data, size_t size) {
    if (!data || size == 0) return;
    const auto component = request_name(id);
    // libcurl may deliver multiple lines per callback. Each line is filtered
    // before it reaches the persistent log; request/response bodies are omitted.
    std::string_view remaining(data, std::min(size, static_cast<size_t>(4096)));
    unsigned lines = 0;
    while (!remaining.empty() && lines++ < 16) {
        const size_t end = remaining.find('\n');
        const auto line = trim_line(remaining.substr(0, end));
        if (kind == CURLINFO_TEXT && allowed_curl_text(line) && !sensitive(line))
            cloudplay::app_log_text(component.c_str(), "curl_info",
                " text=" + safe_value(line));
        else if ((kind == CURLINFO_HEADER_IN || kind == CURLINFO_HEADER_OUT) &&
                 allowed_header(line, kind == CURLINFO_HEADER_IN) && !sensitive(line))
            cloudplay::app_log_text(component.c_str(),
                kind == CURLINFO_HEADER_IN ? "header_in" : "header_out",
                " line=" + safe_value(line));
        if (end == std::string_view::npos) break;
        remaining.remove_prefix(end + 1);
    }
}

extern "C" void cloudplay_http_detail(int id, const char *event, const char *detail) {
    const auto component = request_name(id);
    cloudplay::app_log_text(component.c_str(), event ? event : "detail",
        " value=" + safe_value(detail ? detail : ""));
}

extern "C" void cloudplay_http_response(int id, long status,
    const char *body, size_t body_size) {
    const auto component = request_name(id);
    cloudplay::app_log_text(component.c_str(), "response",
        " status=" + std::to_string(status) + " body_bytes=" +
        std::to_string(body_size));
    if (status < 400 || !body || body_size == 0 || body_size > 16384) return;
    const auto parsed = nlohmann::json::parse(body, body + body_size, nullptr, false);
    if (parsed.is_discarded()) return;
    log_error_fields(component, parsed);
}

extern "C" void cloudplay_socket_result(int domain, int type, int native_type,
    int protocol, int result, int error) {
    cloudplay::app_log_text("socket", "create",
        " domain=" + std::to_string(domain) + " type=" + std::to_string(type) +
        " native_type=" + std::to_string(native_type) +
        " protocol=" + std::to_string(protocol) + " result=" +
        std::to_string(result) + " errno=" + std::to_string(error));
}

extern "C" void cloudplay_catalog_trace(int stage) {
    const char *component = "catalog";
    const char *event = "stage";
    switch (stage) {
    case 1: event = "refresh_requested"; break;
    case 2: event = "worker_start_failed"; break;
    case 3: event = "worker_started"; break;
    case 4: event = "ip_mapped_catalog_fetch_started"; break;
    case 5: event = "fetch_finished"; break;
    case 10: event = "account_discovery_started"; break;
    case 11: event = "oauth_prepare"; break;
    case 12: event = "oauth_request"; break;
    case 13: event = "oauth_response"; break;
    case 14: event = "account_discovery_finished"; break;
    case 20: event = "chiaki_fetch_started"; break;
    case 21: event = "account_discovery_finished"; break;
    case 22: event = "psnow_source_finished"; break;
    case 23: event = "browse_source_finished"; break;
    case 24: event = "owned_source_finished"; break;
    case 25: event = "catalog_merge_started"; break;
    case 26: event = "catalog_merge_finished"; break;
    case 27: event = "catalog_sources_empty"; break;
    case 31: event = "json_smoke_started"; break;
    case 32: event = "json_smoke_succeeded"; break;
    case 33: event = "json_smoke_failed"; break;
    case 34: event = "account_json_parse_started"; break;
    case 35: event = "account_json_parse_finished"; break;
    case 80: event = "json_hash_seed_started"; break;
    case 81: event = "json_hash_seed_clock_ready"; break;
    case 82: event = "json_hash_seed_clock_failed"; break;
    default:
        if (stage >= 100 && stage < 130) {
            component = "psn_http";
            static const char *const steps[] = {
                "request_start", "curl_global_ready", "curl_handle_ready",
                "request_configured", "perform_start", "perform_finished"
            };
            const int step = stage % 10;
            event = step < 6 ? steps[step] : "stage";
        } else if (stage >= 300 && stage < 330) {
            component = "dns";
            const int step = stage % 10;
            event = step == 0 ? "lookup_start" : step == 1 ?
                "resolver_start" : step == 2 ? "lookup_succeeded" :
                "lookup_failed";
        }
    }
    cloudplay::app_log(component, event, stage);
}

extern "C" void cloudplay_session_trace(int stage) {
    const char *event = "stage";
    switch (stage) {
    case 100: event = "init_enter"; break;
    case 110: event = "cond_init_start"; break;
    case 111: event = "cond_init_done"; break;
    case 120: event = "mutex_init_start"; break;
    case 121: event = "mutex_init_done"; break;
    case 130: event = "stop_pipe_init_start"; break;
    case 131: event = "stop_pipe_init_done"; break;
    case 140: event = "ctrl_init_start"; break;
    case 141: event = "ctrl_init_done"; break;
    case 150: event = "stream_connection_init_start"; break;
    case 151: event = "stream_connection_init_done"; break;
    case 160: event = "cloud_endpoint_start"; break;
    case 162: event = "handshake_decode_start"; break;
    case 163: event = "handshake_decode_done"; break;
    case 164: event = "endpoint_resolve_start"; break;
    case 165: event = "endpoint_resolve_done"; break;
    case 166: event = "endpoint_numeric_start"; break;
    case 167: event = "endpoint_numeric_done"; break;
    case 169: event = "cloud_endpoint_done"; break;
    case 170: event = "cloud_config_done"; break;
    case 180: event = "controller_init_start"; break;
    case 181: event = "controller_init_done"; break;
    case 190: event = "init_done"; break;
    case 200: event = "thread_enter"; break;
    case 201: event = "thread_affinity_done"; break;
    case 202: event = "thread_mutex_locked"; break;
    case 210: event = "cloud_prepare_start"; break;
    case 211: event = "cloud_prepare_done"; break;
    case 212: event = "cloud_defaults_done"; break;
    case 213: event = "cloud_nonce_done"; break;
    case 214: event = "cloud_rpcrypt_start"; break;
    case 215: event = "cloud_rpcrypt_done"; break;
    case 219: event = "cloud_prepare_failed"; break;
    case 220: event = "ecdh_init_start"; break;
    case 221: event = "ecdh_init_done"; break;
    case 230: event = "stream_connection_run_start"; break;
    case 231: event = "stream_connection_run_done"; break;
    case 232: event = "ecdh_fini_done"; break;
    case 240: event = "session_cleanup_start"; break;
    case 241: event = "cloud_ctrl_cleanup_skipped"; break;
    case 250: event = "thread_quit"; break;
    case 300: event = "stream_run_enter"; break;
    case 301: event = "stream_mutex_locked"; break;
    case 310: event = "audio_receiver_start"; break;
    case 311: event = "audio_receiver_done"; break;
    case 312: event = "haptics_receiver_start"; break;
    case 313: event = "haptics_receiver_done"; break;
    case 314: event = "video_receiver_start"; break;
    case 315: event = "video_receiver_done"; break;
    case 320: event = "takion_connect_start"; break;
    case 321: event = "takion_connect_done"; break;
    case 330: event = "congestion_start"; break;
    case 331: event = "congestion_done"; break;
    case 340: event = "takion_wait_start"; break;
    case 341: event = "takion_wait_done"; break;
    case 350: event = "big_send_start"; break;
    case 351: event = "big_send_done"; break;
    case 360: event = "bang_wait_start"; break;
    case 361: event = "bang_wait_done"; break;
    case 370: event = "streaminfo_wait_start"; break;
    case 371: event = "streaminfo_wait_done"; break;
    case 400: event = "takion_enter"; break;
    case 401: event = "takion_stop_pipe_done"; break;
    case 410: event = "takion_socket_start"; break;
    case 411: event = "takion_socket_done"; break;
    case 412: event = "takion_rcvbuf_start"; break;
    case 413: event = "takion_rcvbuf_done"; break;
    case 414: event = "takion_udp_connect_start"; break;
    case 415: event = "takion_udp_connect_done"; break;
    case 420: event = "takion_thread_start"; break;
    case 421: event = "takion_thread_started"; break;
    case 430: event = "takion_thread_enter"; break;
    case 431: event = "takion_handshake_start"; break;
    case 432: event = "takion_handshake_done"; break;
    case 440: event = "takion_control_packet"; break;
    case 500: event = "protobuf_enter"; break;
    case 501: event = "protobuf_mutex_locked"; break;
    case 502: event = "protobuf_expect_bang"; break;
    case 503: event = "protobuf_expect_streaminfo"; break;
    case 504: event = "protobuf_idle"; break;
    case 505: event = "protobuf_done"; break;
    case 510: event = "streaminfo_enter"; break;
    case 511: event = "streaminfo_state_allocated"; break;
    case 512: event = "streaminfo_decode_start"; break;
    case 513: event = "streaminfo_decode_done"; break;
    case 514: event = "streaminfo_payload_valid"; break;
    case 515: event = "streaminfo_audio_valid"; break;
    case 516: event = "streaminfo_audio_start"; break;
    case 517: event = "streaminfo_audio_done"; break;
    case 518: event = "streaminfo_video_start"; break;
    case 519: event = "streaminfo_video_done"; break;
    case 520: event = "streaminfo_ack_start"; break;
    case 521: event = "streaminfo_ack_done"; break;
    case 522: event = "controller_connection_start"; break;
    case 523: event = "controller_connection_done"; break;
    case 524: event = "microphone_enable_start"; break;
    case 525: event = "microphone_enable_done"; break;
    case 526: event = "streaminfo_complete"; break;
    case 529: event = "streaminfo_failed"; break;
    case 530: event = "resolution_decode_start"; break;
    case 531: event = "resolution_decode_done"; break;
    case 532: event = "resolution_stored"; break;
    case 540: event = "audio_receiver_enter"; break;
    case 541: event = "audio_receiver_mutex_locked"; break;
    case 542: event = "audio_header_logged"; break;
    case 543: event = "audio_sink_loaded"; break;
    case 544: event = "audio_jitter_cleared"; break;
    case 545: event = "audio_header_callback_start"; break;
    case 546: event = "audio_header_callback_done"; break;
    case 547: event = "audio_receiver_unlocked"; break;
    case 550: event = "opus_header_enter"; break;
    case 551: event = "opus_previous_decoder_cleared"; break;
    case 552: event = "opus_decoder_create_start"; break;
    case 553: event = "opus_decoder_create_done"; break;
    case 554: event = "opus_pcm_buffer_ready"; break;
    case 555: event = "opus_settings_callback_start"; break;
    case 556: event = "opus_settings_callback_done"; break;
    }
    cloudplay::app_log("session", event, stage);
}

extern "C" void cloudplay_catalog_metric(int metric, int value) {
    const char *event = "metric";
    switch (metric) {
    case 1: event = "account_authenticated"; break;
    case 2: event = "account_region_supported"; break;
    case 3: event = "account_transport_failed"; break;
    case 10: event = "psnow_count"; break;
    case 11: event = "psnow_complete"; break;
    case 12: event = "browse_count"; break;
    case 13: event = "browse_complete"; break;
    case 14: event = "owned_count"; break;
    case 15: event = "owned_complete"; break;
    case 16: event = "merged_count"; break;
    case 17: event = "merge_after_psnow"; break;
    case 18: event = "merge_after_browse"; break;
    case 19: event = "merge_after_owned"; break;
    case 20: event = "merge_after_launch_dedup"; break;
    case 21: event = "merge_after_concept_dedup"; break;
    case 22: event = "merge_after_version_grouping"; break;
    case 30: event = "browse_locale_0_successes"; break;
    case 31: event = "browse_locale_1_successes"; break;
    case 32: event = "browse_locale_2_successes"; break;
    case 40: event = "browse_locale_0_ps5_list"; break;
    case 41: event = "browse_locale_1_ps5_list"; break;
    case 42: event = "browse_locale_2_ps5_list"; break;
    }
    cloudplay::app_log("catalog", event, value);
    if (metric == 10 || metric == 12 || metric == 14 || metric == 16)
        cloudplay::app_log_allocator("catalog", "allocator");
}

extern "C" void cloudplay_network_result(int kind, int value) {
    const char *event = kind == 1 ? "curl_error" :
        kind == 2 ? "http_status" : kind == 3 ?
        "ip_map_config_result" : kind == 4 ? "curl_os_errno" :
        kind == 5 ? "socket_create_errno" :
        kind == 6 ? "connect_errno" : kind == 7 ?
        "socket_created" : kind == 8 ? "connect_started_port" :
        kind == 9 ? "socket_fcntl_command" :
        kind == 10 ? "curl_global_init_error" :
        kind == 11 ? "connect_pending" :
        "network_result";
    cloudplay::app_log("psn_http", event, value);
}
#endif
