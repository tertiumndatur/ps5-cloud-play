// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cloud_catalog.hpp"

#include <atomic>
#include <string>
#include <chiaki/thread.h>

namespace cloudplay {

class CloudCatalogClient {
public:
    explicit CloudCatalogClient(CloudCatalogStore store);
    ~CloudCatalogClient();
    bool refresh(const std::string &npsso, const std::string &locale);
    void poll();
    bool busy() const { return busy_; }
    unsigned generation() const { return generation_; }
    const std::string &status() const { return status_; }

private:
    static void *run(void *user);
    CloudCatalogStore store_;
    ChiakiThread thread_{};
    std::atomic<bool> finished_{false};
    bool busy_ = false;
    unsigned generation_ = 0;
    std::string npsso_;
    std::string locale_;
    std::string json_;
    std::string error_;
    std::string status_;
};

} // namespace cloudplay
