// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace cloudplay {

struct Credentials {
    std::string npsso;
    std::string psn_access_token;
    std::string psn_refresh_token;
    std::string psn_auth_token_expiry;
    std::string psn_account_id;
};

enum class PsnTokenExpiryState { Missing, Active, Expired, Invalid };

bool valid_credentials(const Credentials &credentials);
PsnTokenExpiryState psn_token_expiry_state(const Credentials &credentials);

} // namespace cloudplay
