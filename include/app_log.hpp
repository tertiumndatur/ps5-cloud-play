// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>

namespace cloudplay {

// Events and numeric results contain no account secrets.
void app_log(const char *component, const char *event, int result = 0);
// Only pass details already filtered for credentials, URLs and account IDs.
void app_log_text(const char *component, const char *event, std::string_view detail);
// Records mmap allocator counters without logging allocation contents.
void app_log_allocator(const char *component, const char *event);

} // namespace cloudplay
