// SPDX-License-Identifier: GPL-3.0-or-later
#include "cloud_settings.hpp"
#include "storage_io.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace cloudplay {
namespace {

constexpr const char *languages[] = {
    "en-US", "en-GB", "en-RU", "de-DE", "fr-FR", "fi-FI",
    "it-IT", "es-ES", "nl-NL", "pt-BR", "ja-JP", "ko-KR", "ru-RU"
};

constexpr const char *required_resolve_hosts[] = {
    "psnow.playstation.com",
    "www.playstation.com",
    "commerce.api.np.km.playstation.net",
    "web.np.playstation.com",
    "accounts.api.playstation.com",
    "apollo2.dl.playstation.net",
    "vulcan.dl.playstation.net",
    "image.api.playstation.com",
    "gs2-sec.ww.prod.dl.playstation.net"
};

bool valid_language(const std::string &language) {
    if (language.empty()) return true;
    for (const char *supported : languages)
        if (language == supported) return true;
    return false;
}

bool valid_datacenter(const std::string &name) {
    if (name.empty() || name.size() > 64) return false;
    for (char ch : name)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.'))
            return false;
    return true;
}

bool valid_host(const std::string &host) {
    if (host.size() < 3 || host.size() > 253 || host.front() == '.' ||
        host.back() == '.') return false;
    bool dot = false;
    size_t label = 0;
    for (char ch : host) {
        if (ch == '.') {
            if (!label || label > 63) return false;
            dot = true;
            label = 0;
        } else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '-') {
            ++label;
        } else return false;
    }
    return dot && label && label <= 63;
}

bool public_ipv4(const std::string &value) {
    unsigned parts[4]{};
    size_t at = 0;
    for (size_t index = 0; index < 4; ++index) {
        unsigned digits = 0;
        while (at < value.size() && value[at] >= '0' && value[at] <= '9') {
            parts[index] = parts[index] * 10 + static_cast<unsigned>(value[at++] - '0');
            if (++digits > 3 || parts[index] > 255) return false;
        }
        if (!digits) return false;
        if (index != 3 && (at == value.size() || value[at++] != '.')) return false;
    }
    return at == value.size() && parts[0] > 0 && parts[0] != 10 &&
        parts[0] != 127 && parts[0] < 224 &&
        !(parts[0] == 100 && parts[1] >= 64 && parts[1] <= 127) &&
        !(parts[0] == 169 && parts[1] == 254) &&
        !(parts[0] == 172 && parts[1] >= 16 && parts[1] <= 31) &&
        !(parts[0] == 192 && parts[1] == 168);
}

bool valid_service(const ServiceSettings &settings) {
    const int resolution = settings.resolution;
    const bool valid_json = settings.datacenters_json.size() <= 8192 &&
        settings.datacenters_json.find('\n') == std::string::npos &&
        nlohmann::json::parse(settings.datacenters_json, nullptr, false).is_array();
    return valid_language(settings.game_language) &&
           (resolution == 720 || resolution == 1080 || resolution == 1440 ||
            resolution == 2160) && settings.bitrate_kbps >= 2000 &&
           settings.bitrate_kbps <= 200000 && settings.bitrate_kbps % 1000 == 0 &&
           valid_datacenter(settings.datacenter) && valid_json &&
           settings.video_recovery_mode >= VideoRecoveryMode::Simple &&
           settings.video_recovery_mode <= VideoRecoveryMode::Hybrid;
}

bool read_recovery_mode(const nlohmann::json &service, VideoRecoveryMode &mode) {
    const auto recovery = service.find("videoRecoveryMode");
    if (recovery == service.end()) {
        mode = VideoRecoveryMode::IDR;
        return true;
    }
    if (!recovery->is_string()) return false;
    const std::string value = recovery->get<std::string>();
    if (value == "simple") mode = VideoRecoveryMode::Simple;
    else if (value == "idr") mode = VideoRecoveryMode::IDR;
    else if (value == "hybrid") mode = VideoRecoveryMode::Hybrid;
    else return false;
    return true;
}

const char *write_recovery_mode(VideoRecoveryMode mode) {
    switch (mode) {
    case VideoRecoveryMode::Simple: return "simple";
    case VideoRecoveryMode::IDR: return "idr";
    case VideoRecoveryMode::Hybrid: return "hybrid";
    }
    return "idr";
}

bool read_service(const nlohmann::json &document, const char *key,
                  ServiceSettings &settings) {
    const auto service = document.find(key);
    if (service == document.end() || !service->is_object()) return false;
    const auto language = service->find("gameLanguage");
    const auto resolution = service->find("resolution");
    const auto bitrate = service->find("bitrateKbps");
    const auto datacenter = service->find("datacenter");
    const auto datacenters = service->find("datacenters");
    if (language == service->end() || !language->is_string() ||
        resolution == service->end() || !resolution->is_number_integer() ||
        bitrate == service->end() || !bitrate->is_number_integer() ||
        datacenter == service->end() || !datacenter->is_string() ||
        datacenters == service->end() || !datacenters->is_array()) return false;
    settings.game_language = language->get<std::string>();
    settings.resolution = resolution->get<int>();
    settings.bitrate_kbps = bitrate->get<int>();
    settings.datacenter = datacenter->get<std::string>();
    settings.datacenters_json = datacenters->dump();
    if (!read_recovery_mode(*service, settings.video_recovery_mode)) return false;
    return valid_service(settings);
}

nlohmann::json write_service(const ServiceSettings &settings) {
    return {
        {"gameLanguage", settings.game_language},
        {"resolution", settings.resolution},
        {"bitrateKbps", settings.bitrate_kbps},
        {"datacenter", settings.datacenter},
        {"datacenters", nlohmann::json::parse(settings.datacenters_json)},
        {"videoRecoveryMode", write_recovery_mode(settings.video_recovery_mode)}
    };
}

bool read_credentials(const nlohmann::json &document, Credentials &credentials) {
    const auto tokens = document.find("psnTokens");
    if (tokens == document.end() || !tokens->is_object()) return false;
    const char *keys[] = {"npsso", "psn_auth_token", "psn_refresh_token",
                          "psn_auth_token_expiry", "psn_account_id"};
    std::string *values[] = {&credentials.npsso, &credentials.psn_access_token,
        &credentials.psn_refresh_token, &credentials.psn_auth_token_expiry,
        &credentials.psn_account_id};
    for (size_t index = 0; index < 5; ++index) {
        const auto found = tokens->find(keys[index]);
        if (found == tokens->end() || !found->is_string()) return false;
        *values[index] = found->get<std::string>();
    }
    return valid_credentials(credentials);
}

nlohmann::json write_credentials(const Credentials &credentials) {
    return {
        {"npsso", credentials.npsso},
        {"psn_auth_token", credentials.psn_access_token},
        {"psn_refresh_token", credentials.psn_refresh_token},
        {"psn_auth_token_expiry", credentials.psn_auth_token_expiry},
        {"psn_account_id", credentials.psn_account_id}
    };
}

bool read_resolves(const nlohmann::json &document,
                   std::map<std::string, std::string> &resolves) {
    const auto source = document.find("dnsResolves");
    if (source == document.end() || !source->is_object() || source->size() > 32)
        return false;
    for (auto found = source->begin(); found != source->end(); ++found) {
        if (!valid_host(found.key()) || !found->is_array() || found->empty() ||
            found->size() > 4) return false;
        std::string joined;
        for (const auto &item : *found) {
            if (!item.is_string()) return false;
            const std::string ip = item.get<std::string>();
            if (!public_ipv4(ip)) return false;
            if (!joined.empty()) joined += ',';
            joined += ip;
        }
        resolves.emplace(found.key(), std::move(joined));
    }
    return true;
}

nlohmann::json write_resolves(const std::map<std::string, std::string> &resolves) {
    nlohmann::json result = nlohmann::json::object();
    for (const auto &[host, joined] : resolves) {
        nlohmann::json addresses = nlohmann::json::array();
        size_t at = 0;
        while (at <= joined.size()) {
            const size_t comma = joined.find(',', at);
            addresses.push_back(joined.substr(at, comma == std::string::npos ?
                joined.size() - at : comma - at));
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        result[host] = std::move(addresses);
    }
    return result;
}

bool resolves_complete(const std::map<std::string, std::string> &resolves) {
    for (const char *host : required_resolve_hosts) {
        const auto found = resolves.find(host);
        if (found == resolves.end() || found->second.empty()) return false;
    }
    return true;
}

} // namespace

std::vector<std::string> cloud_datacenter_names(const ServiceSettings &settings) {
    std::vector<std::string> names{"Auto"};
    const auto list = nlohmann::json::parse(settings.datacenters_json, nullptr, false);
    if (!list.is_array() || list.size() > 64) return names;
    for (const auto &item : list) {
        if (!item.is_object()) continue;
        auto name = item.find("dataCenter");
        if (name == item.end() || !name->is_string()) continue;
        const std::string value = name->get<std::string>();
        if (valid_datacenter(value) && value != "Auto") names.push_back(value);
    }
    return names;
}

ServiceSettings &CloudSettings::selected() {
    return service == CloudService::PS5Cloud ? ps5_cloud : ps_now;
}
const ServiceSettings &CloudSettings::selected() const {
    return service == CloudService::PS5Cloud ? ps5_cloud : ps_now;
}

const char *const *supported_cloud_languages(size_t &count) {
    count = sizeof(languages) / sizeof(languages[0]);
    return languages;
}

bool valid_cloud_settings(const CloudSettings &settings) {
    if (settings.service != CloudService::PS5Cloud &&
        settings.service != CloudService::PSNow) return false;
    if (!valid_service(settings.ps5_cloud) || !valid_service(settings.ps_now) ||
        !valid_credentials(settings.psn_tokens) || settings.dns_resolves.size() > 32)
        return false;
    const nlohmann::json wrapper = {{"dnsResolves", write_resolves(settings.dns_resolves)}};
    std::map<std::string, std::string> checked;
    return read_resolves(wrapper, checked);
}

bool parse_cloud_settings_json(std::string_view json, CloudSettings &settings) {
    const auto document = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
    if (!document.is_object()) return false;
    const auto service = document.find("service");
    if (service == document.end() || !service->is_string()) return false;
    CloudSettings loaded;
    const std::string service_name = service->get<std::string>();
    if (service_name == "ps5_cloud") loaded.service = CloudService::PS5Cloud;
    else if (service_name == "ps_now") loaded.service = CloudService::PSNow;
    else return false;
    const auto show_stream_stats = document.find("showStreamStats");
    if (show_stream_stats != document.end()) {
        if (!show_stream_stats->is_boolean()) return false;
        loaded.show_stream_stats = show_stream_stats->get<bool>();
    }
    if (!read_service(document, "ps5Cloud", loaded.ps5_cloud) ||
        !read_service(document, "psNow", loaded.ps_now) ||
        !read_credentials(document, loaded.psn_tokens) ||
        !read_resolves(document, loaded.dns_resolves) ||
        !valid_cloud_settings(loaded)) return false;
    settings = std::move(loaded);
    return true;
}

bool parse_cloud_access_json(std::string_view json, Credentials &tokens,
                             std::map<std::string, std::string> &resolves) {
    const auto document = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
    if (!document.is_object() || document.size() != 2 ||
        document.find("psnTokens") == document.end() ||
        document.find("dnsResolves") == document.end()) return false;
    Credentials parsed_tokens;
    std::map<std::string, std::string> parsed_resolves;
    if (!read_credentials(document, parsed_tokens) ||
        !read_resolves(document, parsed_resolves)) return false;
    if (parsed_tokens.npsso.empty() || parsed_tokens.psn_access_token.empty() ||
        parsed_tokens.psn_refresh_token.empty() ||
        parsed_tokens.psn_auth_token_expiry.empty() ||
        parsed_tokens.psn_account_id.empty() || !resolves_complete(parsed_resolves))
        return false;
    tokens = std::move(parsed_tokens);
    resolves = std::move(parsed_resolves);
    return true;
}

bool cloud_access_ready(const CloudSettings &settings) {
    return cloud_access_state(settings) != CloudAccessState::Missing;
}

CloudAccessState cloud_access_state(const CloudSettings &settings) {
    const Credentials &tokens = settings.psn_tokens;
    const bool complete = !tokens.npsso.empty() && !tokens.psn_access_token.empty() &&
        !tokens.psn_refresh_token.empty() &&
        !tokens.psn_auth_token_expiry.empty() &&
        !tokens.psn_account_id.empty() && resolves_complete(settings.dns_resolves);
    if (!complete) return CloudAccessState::Missing;
    return psn_token_expiry_state(tokens) == PsnTokenExpiryState::Active ?
        CloudAccessState::Ready : CloudAccessState::Expired;
}

bool CloudSettingsStore::load(CloudSettings &settings) const {
    std::string data;
    if (!storage_read_file(path_, 65536, data)) return false;
    return parse_cloud_settings_json(data, settings);
}

bool CloudSettingsStore::save(const CloudSettings &settings) const {
    if (!valid_cloud_settings(settings)) return false;
    const nlohmann::json document = {
        {"service", settings.service == CloudService::PS5Cloud ? "ps5_cloud" : "ps_now"},
        {"showStreamStats", settings.show_stream_stats},
        {"ps5Cloud", write_service(settings.ps5_cloud)},
        {"psNow", write_service(settings.ps_now)},
        {"psnTokens", write_credentials(settings.psn_tokens)},
        {"dnsResolves", write_resolves(settings.dns_resolves)}
    };
    return storage_write_atomic(path_, document.dump(2) + "\n");
}

} // namespace cloudplay
