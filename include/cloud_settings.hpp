// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "credentials.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace cloudplay {

enum class CloudService { PS5Cloud = 0, PSNow = 1 };
enum class VideoRecoveryMode { Simple = 0, IDR = 1, Hybrid = 2 };
enum class CloudAccessState { Missing, Expired, Ready };

struct ServiceSettings {
    std::string game_language = "en-US";
    int resolution = 1080;
    int bitrate_kbps = 20000;
    std::string datacenter = "Auto";
    std::string datacenters_json = "[]";
    VideoRecoveryMode video_recovery_mode = VideoRecoveryMode::IDR;
};

struct CloudSettings {
    CloudService service = CloudService::PS5Cloud;
    bool show_stream_stats = false;
    ServiceSettings ps5_cloud{"en-US", 1080, 25000, "Auto", "[]",
                              VideoRecoveryMode::IDR};
    ServiceSettings ps_now{"en-US", 1080, 20000, "Auto", "[]",
                           VideoRecoveryMode::IDR};
    Credentials psn_tokens;
    std::map<std::string, std::string> dns_resolves;

    ServiceSettings &selected();
    const ServiceSettings &selected() const;
};

bool valid_cloud_settings(const CloudSettings &settings);
bool parse_cloud_settings_json(std::string_view json, CloudSettings &settings);
bool parse_cloud_access_json(std::string_view json, Credentials &tokens,
                             std::map<std::string, std::string> &resolves);
CloudAccessState cloud_access_state(const CloudSettings &settings);
bool cloud_access_ready(const CloudSettings &settings);
const char *const *supported_cloud_languages(size_t &count);
std::vector<std::string> cloud_datacenter_names(const ServiceSettings &settings);

class CloudSettingsStore {
public:
    explicit CloudSettingsStore(std::string path) : path_(std::move(path)) {}
    bool load(CloudSettings &settings) const;
    bool save(const CloudSettings &settings) const;
private:
    std::string path_;
};

} // namespace cloudplay
