// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cloud_catalog.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cloudplay {

struct RecentLaunch {
    std::string game_id;
    std::string variant_id;
    std::string platform;
    std::string service_type;
    uint64_t launched_at = 0;
};

std::string cloud_game_history_id(const CloudGame &game);
bool recent_launch_matches_game(const RecentLaunch &launch, const CloudGame &game);
void record_recent_launch(std::vector<RecentLaunch> &launches, const CloudGame &game,
                          const CloudVariant &variant);

class RecentLaunchStore {
public:
    explicit RecentLaunchStore(std::string path) : path_(std::move(path)) {}
    bool load(std::vector<RecentLaunch> &launches) const;
    bool save(const std::vector<RecentLaunch> &launches) const;

private:
    std::string path_;
};

} // namespace cloudplay
