// SPDX-License-Identifier: GPL-3.0-or-later
#include "ca_bundle.h"
#include "app_log.hpp"
#include "storage_io.hpp"

#include <mutex>
#include <string>

extern "C" const unsigned char *cloudplay_ca_bundle(size_t *size) {
    static std::once_flag once;
    static std::string certificates;
    std::call_once(once, [] {
        if (!cloudplay::storage_read_file("/app0/assets/ca-certificates.crt",
                256 * 1024, certificates) ||
            certificates.find("-----BEGIN CERTIFICATE-----") == std::string::npos) {
            certificates.clear();
            cloudplay::app_log("tls", "ca_bundle_load_failed");
        } else {
            cloudplay::app_log("tls", "ca_bundle_loaded",
                static_cast<int>(certificates.size()));
        }
    });
    if (size) *size = certificates.size();
    return certificates.empty() ? nullptr :
        reinterpret_cast<const unsigned char *>(certificates.data());
}
