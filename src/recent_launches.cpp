// SPDX-License-Identifier: GPL-3.0-or-later
#include "recent_launches.hpp"
#include "storage_io.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>

namespace cloudplay {
namespace {

constexpr size_t max_recent_launches = 20;
constexpr size_t max_recent_bytes = 64 * 1024;

std::string json_string(const nlohmann::json &item, const char *key) {
    const auto value = item.find(key);
    if (value == item.end() || !value->is_string()) return {};
    const std::string result = value->get<std::string>();
    return result.size() <= 1024 ? result : std::string{};
}

uint64_t now_seconds() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<seconds>(
        system_clock::now().time_since_epoch()).count());
}

} // namespace

std::string cloud_game_history_id(const CloudGame &game) {
    if (!game.concept_id.empty()) return game.concept_id;
    if (!game.product_id.empty()) return game.product_id;
    return game.identifier;
}

bool recent_launch_matches_game(const RecentLaunch &launch, const CloudGame &game) {
    const std::string id = cloud_game_history_id(game);
    return !id.empty() && launch.game_id == id;
}

void record_recent_launch(std::vector<RecentLaunch> &launches, const CloudGame &game,
                          const CloudVariant &variant) {
    const std::string game_id = cloud_game_history_id(game);
    if (game_id.empty()) return;
    launches.erase(std::remove_if(launches.begin(), launches.end(), [&](const auto &item) {
        return item.game_id == game_id;
    }), launches.end());
    launches.insert(launches.begin(), RecentLaunch{game_id, variant.identifier,
                                                    variant.platform, variant.service_type,
                                                    now_seconds()});
    if (launches.size() > max_recent_launches)
        launches.resize(max_recent_launches);
}

bool RecentLaunchStore::load(std::vector<RecentLaunch> &launches) const {
    launches.clear();
    std::string data;
    if (!storage_read_file(path_, max_recent_bytes, data)) return false;
    const auto document = nlohmann::json::parse(data.begin(), data.end(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) return false;
    const auto items = document.find("items");
    if (items == document.end() || !items->is_array()) return false;
    for (const auto &item : *items) {
        if (!item.is_object() || launches.size() >= max_recent_launches) break;
        RecentLaunch launch;
        launch.game_id = json_string(item, "gameId");
        launch.variant_id = json_string(item, "variantId");
        launch.platform = json_string(item, "platform");
        launch.service_type = json_string(item, "service");
        const auto timestamp = item.find("launchedAt");
        if (timestamp != item.end() && timestamp->is_number_unsigned())
            launch.launched_at = timestamp->get<uint64_t>();
        if (!launch.game_id.empty()) launches.push_back(std::move(launch));
    }
    return true;
}

bool RecentLaunchStore::save(const std::vector<RecentLaunch> &launches) const {
    nlohmann::json items = nlohmann::json::array();
    for (size_t index = 0; index < launches.size() && index < max_recent_launches; ++index) {
        const auto &launch = launches[index];
        if (launch.game_id.empty()) continue;
        items.push_back({{"gameId", launch.game_id},
                         {"variantId", launch.variant_id},
                         {"platform", launch.platform},
                         {"service", launch.service_type},
                         {"launchedAt", launch.launched_at}});
    }
    const std::string data = nlohmann::json{{"items", std::move(items)}}.dump();
    return storage_write_atomic(path_, data);
}

} // namespace cloudplay
