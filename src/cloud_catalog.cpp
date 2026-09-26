// SPDX-License-Identifier: GPL-3.0-or-later
#include "cloud_catalog.hpp"
#include "storage_io.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <utility>

namespace cloudplay {
namespace {

constexpr size_t max_catalog_bytes = 16 * 1024 * 1024;
constexpr size_t max_games = 12000;

std::string string_field(const nlohmann::json &item, const char *key) {
    auto it = item.find(key);
    if (it == item.end() || !it->is_string()) return {};
    const std::string value = it->get<std::string>();
    return value.size() <= 1024 ? value : std::string{};
}

bool bool_field(const nlohmann::json &item, const char *key) {
    auto it = item.find(key);
    return it != item.end() && it->is_boolean() && it->get<bool>();
}

CloudVariant variant_from(const nlohmann::json &item) {
    CloudVariant result;
    result.platform = string_field(item, "platform");
    result.service_type = string_field(item, "streamServiceType");
    if (result.service_type.empty()) result.service_type = string_field(item, "serviceType");
    result.identifier = string_field(item, "streamIdentifier");
    if (result.identifier.empty()) result.identifier = string_field(item, "productId");
    result.entitlement_id = string_field(item, "entitlementId");
    result.is_owned = bool_field(item, "isOwned");
    result.is_playable = bool_field(item, "isPlayable");
    result.plus_catalog = bool_field(item, "plusCatalog");
    return result;
}

} // namespace

std::string cloud_cover_key(std::string_view product_id) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char ch : product_id)
        hash = (hash ^ ch) * UINT64_C(1099511628211);
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(hash));
    return hex;
}

bool parse_cloud_catalog(std::string_view json, std::vector<CloudGame> &games,
                         std::string &error, CloudCatalogContext *context) {
    games.clear();
    error.clear();
    if (json.size() > max_catalog_bytes) {
        error = "Catalog is too large";
        return false;
    }
    const auto root = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
    if (root.is_discarded() || !root.is_object() || !root.contains("games") ||
        !root["games"].is_array() || root["games"].size() > max_games) {
        error = "Invalid cloud catalog";
        return false;
    }
    if (context) {
        context->store_country = string_field(root, "fallbackRegion");
        context->store_language = string_field(root, "resolvedStoreLang");
        context->locale = string_field(root, "settledLocale");
        context->foreign = root.contains("nativeMode") && !bool_field(root, "nativeMode");
    }
    games.reserve(root["games"].size());
    for (const auto &item : root["games"]) {
        if (!item.is_object()) continue;
        CloudGame game;
        static_cast<CloudVariant &>(game) = variant_from(item);
        game.name = string_field(item, "name");
        game.product_id = string_field(item, "productId");
        game.concept_id = string_field(item, "conceptId");
        game.category = string_field(item, "category");
        game.image_url = string_field(item, "imageUrl");
        if (auto variants = item.find("variants");
            variants != item.end() && variants->is_array()) {
            for (const auto &variant : *variants) {
                if (variant.is_object() && game.variants.size() < 4) {
                    CloudVariant row = variant_from(variant);
                    if (!variant.contains("isOwned")) row.is_owned = game.is_owned;
                    if (!variant.contains("isPlayable")) row.is_playable = game.is_playable;
                    if (!variant.contains("plusCatalog")) row.plus_catalog = game.plus_catalog;
                    game.variants.push_back(std::move(row));
                }
            }
        }
        if (!game.name.empty()) games.push_back(std::move(game));
    }
    return true;
}

bool CloudCatalogStore::load(std::vector<CloudGame> &games, CloudCatalogContext *context) const {
    std::string data;
    if (!storage_read_file(path_, max_catalog_bytes, data)) return false;
    std::string error;
    return parse_cloud_catalog(data, games, error, context);
}

bool CloudCatalogStore::save_trusted_json(std::string_view json) const {
    return !json.empty() && json.size() <= max_catalog_bytes &&
        storage_write_atomic(path_, json);
}

} // namespace cloudplay
