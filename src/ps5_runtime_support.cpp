// SPDX-License-Identifier: GPL-3.0-or-later
#include "app_log.hpp"

#include <string>

#if CLOUDPLAY_PS5
extern "C" int sceKernelUsleep(unsigned int microseconds);

extern "C" __attribute__((noreturn)) void __assert(
    const char *function, const char *file, int line, const char *expression) {
    cloudplay::app_log_text(
        "runtime", "assertion",
        " expression=" + std::string(expression ? expression : "unknown") +
        " function=" + std::string(function ? function : "unknown") +
        " file=" + std::string(file ? file : "unknown") +
        " line=" + std::to_string(line));
    for (;;) sceKernelUsleep(100000);
}
#endif
