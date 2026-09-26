// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "credentials.hpp"

#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace cloudplay {

struct CloudSetupInput {
    std::string redirect_url;
    std::string npsso;
    std::map<std::string, std::string> dns_resolves;
};

bool parse_cloud_setup_request(std::string_view json, CloudSetupInput &input);
std::string cloud_setup_login_url();

bool provision_cloud_access(
    const CloudSetupInput &input, Credentials &credentials,
    const std::function<void(const char *)> &progress, std::string &error);

} // namespace cloudplay
