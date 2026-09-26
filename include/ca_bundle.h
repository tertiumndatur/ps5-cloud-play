// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Stable for the lifetime of the process; libcurl may retain this pointer.
const unsigned char *cloudplay_ca_bundle(size_t *size);

#ifdef __cplusplus
}
#endif
