// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace cloudplay {

struct CloudVariant {
    std::string platform;
    std::string service_type;
    std::string identifier;
    std::string entitlement_id;
    bool is_owned = false;
    bool is_playable = false;
    bool plus_catalog = false;
};

struct CloudGame : CloudVariant {
    std::string name;
    std::string product_id;
    std::string concept_id;
    std::string category;
    std::string image_url;
    std::vector<CloudVariant> variants;
};

struct CloudCatalogContext {
    std::string store_country;
    std::string store_language;
    std::string locale;
    bool foreign = false;
};

bool parse_cloud_catalog(std::string_view json, std::vector<CloudGame> &games,
                         std::string &error, CloudCatalogContext *context = nullptr);
std::string cloud_cover_key(std::string_view product_id);

class CloudCatalogStore {
public:
    explicit CloudCatalogStore(std::string path) : path_(std::move(path)) {}
    bool load(std::vector<CloudGame> &games, CloudCatalogContext *context = nullptr) const;
    bool save_trusted_json(std::string_view json) const;
private:
    std::string path_;
};

} // namespace cloudplay
