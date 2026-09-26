// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "credentials.hpp"
#include "cloud_settings.hpp"
#include "cloud_access_setup.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace cloudplay {

// Small single-client HTTP receiver. Poll from the UI loop; it never blocks.
class CredentialHttpServer {
public:
    CredentialHttpServer(CloudSettingsStore &settings_store, CloudSettings &settings);
    ~CredentialHttpServer();
    bool start(uint16_t port);
    void poll();
    void stop();
    const std::string &status() const { return status_; }
    CloudAccessState access_state() const { return access_state_; }
    const Credentials &credentials() const { return settings_.psn_tokens; }
    uint16_t port() const { return port_; }
    const std::string &local_address() const { return local_address_; }
    const std::string &setup_url() const { return setup_url_; }
    int error_step() const { return error_step_; }
    int error_number() const { return error_number_; }
    unsigned settings_generation() const { return settings_generation_; }

private:
    void close_client();
    void process_request();
    void respond_json(int code, const char *reason, const std::string &body);
    void respond_status(int code, const char *reason, const char *message);
    void respond_html(const std::string &body);
    void start_setup(CloudSetupInput input);
    void apply_setup_result();
    void update_access_status(bool force = false);
    CloudSettingsStore &settings_store_;
    CloudSettings &settings_;
    std::string status_;
    std::string request_;
    std::string response_;
    std::string local_address_;
    std::string setup_url_;
    std::string login_url_;
    size_t response_sent_ = 0;
    int listener_ = -1;
    int client_ = -1;
    uint16_t port_ = 0;
    int error_step_ = 0;
    int error_number_ = 0;
    uint64_t client_since_ms_ = 0;
    unsigned settings_generation_ = 0;
    CloudAccessState access_state_ = CloudAccessState::Missing;
    std::thread setup_worker_;
    std::mutex setup_mutex_;
    std::string setup_progress_ = "Waiting for setup details";
    std::string setup_error_;
    Credentials setup_credentials_;
    std::map<std::string, std::string> setup_resolves_;
    bool setup_busy_ = false;
    bool setup_result_ready_ = false;
    bool setup_result_ok_ = false;
};

} // namespace cloudplay
