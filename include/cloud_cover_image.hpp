// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace cloudplay {

// A decoded cover prepared by the background downloader and consumed directly
// by the UI renderer. Keeping this in memory avoids the low-resolution BMP
// round-trip through /download0.
struct CloudCoverImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

} // namespace cloudplay
