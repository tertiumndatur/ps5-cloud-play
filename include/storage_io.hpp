// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace cloudplay {

// On PS5 these use the same sceKernel file APIs as ProsperoLight.
bool storage_read_file(const std::string &path, size_t maximum, std::string &data);
bool storage_write_atomic(const std::string &path, std::string_view data);
bool storage_remove(const std::string &path);
bool storage_exists(const std::string &path);

} // namespace cloudplay
