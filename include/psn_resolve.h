// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <curl/curl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Caller owns the returned list and must keep it alive until the curl transfer
// finishes. NULL means the HTTPS request must not be started.
struct curl_slist *cloudplay_psn_resolves(const char *url);

#ifdef __cplusplus
}

#include <string>
#include <map>
#include <utility>
#include <vector>

namespace cloudplay {

// Replaces the configured CURLOPT_RESOLVE map and clears native lookup cache.
void psn_set_resolve_rules(const std::map<std::string, std::string> &rules);

// Snapshot of the static overrides and the successful native lookups currently
// used to build CURLOPT_RESOLVE entries.
std::vector<std::pair<std::string, std::string>> psn_resolve_rules();

} // namespace cloudplay
#endif
