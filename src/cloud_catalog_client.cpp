// SPDX-License-Identifier: GPL-3.0-or-later
#include "cloud_catalog_client.hpp"
#include "app_log.hpp"

#include <chiaki/cloudcatalog.h>
#include <chiaki/common.h>
#include <chiaki/log.h>

#include <utility>

extern "C" void cloudplay_catalog_trace(int stage);

namespace cloudplay {

CloudCatalogClient::CloudCatalogClient(CloudCatalogStore store)
    : store_(std::move(store)) {}

CloudCatalogClient::~CloudCatalogClient() {
    if (busy_) chiaki_thread_join(&thread_, nullptr);
}

bool CloudCatalogClient::refresh(const std::string &npsso,
                                  const std::string &locale) {
    if (busy_ || npsso.empty()) return false;
    cloudplay_catalog_trace(1);
    npsso_ = npsso;
    locale_ = locale.empty() ? "en-US" : locale;
    json_.clear();
    error_.clear();
    finished_.store(false, std::memory_order_relaxed);
    if (chiaki_thread_create(&thread_, run, this) != CHIAKI_ERR_SUCCESS) {
        cloudplay_catalog_trace(2);
        status_ = "Could not start catalog request";
        return false;
    }
    busy_ = true;
    cloudplay_catalog_trace(3);
    status_ = "Fetching Cloud Play catalog via HTTPS";
    return true;
}

void *CloudCatalogClient::run(void *user) {
    auto *self = static_cast<CloudCatalogClient *>(user);
    cloudplay_catalog_trace(4);
    ChiakiLog log{};
    chiaki_log_init(&log, 0, nullptr, nullptr);
    ChiakiCloudCatalogConfig config{};
    config.npsso = self->npsso_.c_str();
    config.locale = self->locale_.c_str();
    config.cache_dir = "/download0";
    config.force_refresh = true;
    ChiakiCloudCatalogResult result{};
    const ChiakiErrorCode error = chiaki_cloudcatalog_fetch_unified(&config, &result, &log);
    cloudplay_catalog_trace(5);
    app_log("catalog", "fetch_result", static_cast<int>(error));
    if (error == CHIAKI_ERR_SUCCESS && result.json)
        self->json_ = result.json;
    else if (result.error_message)
        self->error_ = result.error_message;
    else
        self->error_ = chiaki_error_string(error);
    chiaki_cloudcatalog_result_fini(&result);
    self->finished_.store(true, std::memory_order_release);
    return nullptr;
}

void CloudCatalogClient::poll() {
    if (!busy_ || !finished_.load(std::memory_order_acquire)) return;
    chiaki_thread_join(&thread_, nullptr);
    busy_ = false;
    npsso_.clear();
    if (json_.empty()) {
        app_log("catalog", "empty_result");
        status_ = "Catalog error: " + error_;
    } else if (store_.save_trusted_json(json_)) {
        app_log("catalog", "saved", static_cast<int>(json_.size()));
        ++generation_;
        status_ = "Cloud Play catalog updated";
    } else {
        app_log("catalog", "save_failed");
        status_ = "Could not save Cloud Play catalog";
    }
    json_.clear();
    error_.clear();
}

} // namespace cloudplay
