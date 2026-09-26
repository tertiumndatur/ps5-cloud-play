// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "cloud_catalog.hpp"
#include "cloud_settings.hpp"
#include "radio_input.hpp"

#include <memory>
#include <string>

namespace cloudplay {

// Owns one cloud allocation and Chiaki streaming session. HTTPS provisioning
// and the stream run off the UI thread. VideoDec2 and AGC own native decode
// and presentation while the stream is active.
class CloudStreamClient {
public:
    CloudStreamClient();
    ~CloudStreamClient();
    CloudStreamClient(const CloudStreamClient &) = delete;
    CloudStreamClient &operator=(const CloudStreamClient &) = delete;

    bool start(const CloudGame &game, const CloudVariant &variant,
               const CloudSettings &settings, const CloudCatalogContext &catalog,
               const std::string &npsso);
    void stop();
    bool active() const;
    std::string status() const;
    bool take_datacenters(std::string &service_type, std::string &pings_json);
    void send_pad(const radio_input_pad_state_t &pad);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cloudplay
