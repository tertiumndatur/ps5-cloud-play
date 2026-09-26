// SPDX-License-Identifier: GPL-3.0-or-later
#include "psn_resolve.h"
#include "app_log.hpp"

#include <netdb.h>
#include <netinet/in.h>

#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::mutex configured_rules_mutex;
std::map<std::string, std::string> configured_rules;
std::mutex dynamic_rules_mutex;
std::map<std::string, std::string> dynamic_rules;

bool public_ipv4(std::string_view value) {
    unsigned parts[4]{};
    size_t at = 0;
    for (size_t index = 0; index < 4; ++index) {
        unsigned &part = parts[index];
        if (at == value.size()) return false;
        unsigned digits = 0;
        while (at < value.size() && value[at] >= '0' && value[at] <= '9') {
            part = part * 10 + static_cast<unsigned>(value[at++] - '0');
            if (++digits > 3 || part > 255) return false;
        }
        if (!digits) return false;
        if (index != 3) {
            if (at == value.size() || value[at++] != '.') return false;
        }
    }
    if (at != value.size()) return false;
    return parts[0] > 0 && parts[0] != 10 && parts[0] != 127 &&
        parts[0] < 224 && !(parts[0] == 100 && parts[1] >= 64 && parts[1] <= 127) &&
        !(parts[0] == 169 && parts[1] == 254) &&
        !(parts[0] == 172 && parts[1] >= 16 && parts[1] <= 31) &&
        !(parts[0] == 192 && parts[1] == 168);
}

bool valid_host(std::string_view host) {
    if (host.size() < 3 || host.size() > 253 || host.front() == '.' ||
        host.back() == '.') return false;
    bool has_dot = false;
    size_t label_length = 0;
    for (char ch : host) {
        if (ch == '.') {
            if (label_length == 0 || label_length > 63) return false;
            has_dot = true;
            label_length = 0;
        } else if ((ch >= 'a' && ch <= 'z') ||
                   (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '-') {
            ++label_length;
        } else return false;
    }
    return has_dot && label_length > 0 && label_length <= 63;
}

std::string url_host(const char *url) {
    if (!url) return {};
    const std::string_view address(url);
    if (address.substr(0, 8) != "https://") return {};
    const auto end = address.find_first_of("/:?#", 8);
    const auto host = address.substr(8, end == std::string_view::npos ? end : end - 8);
    return valid_host(host) ? std::string(host) : std::string();
}

std::string resolve_with_ps5_dns(const std::string &host) {
    cloudplay::app_log_text("dns", "native_lookup_start", " host=" + host);
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *addresses = nullptr;
    const int error = getaddrinfo(host.c_str(), "443", &hints, &addresses);
    if (error != 0 || !addresses) {
        cloudplay::app_log_text("dns", "native_lookup_failed",
            " host=" + host + " eai=" + std::to_string(error));
        if (addresses) freeaddrinfo(addresses);
        return {};
    }
    std::string result;
    for (auto *address = addresses; address; address = address->ai_next) {
        if (address->ai_family != AF_INET || !address->ai_addr ||
            address->ai_addrlen < sizeof(struct sockaddr_in)) continue;
        const auto *ipv4 = reinterpret_cast<const struct sockaddr_in *>(address->ai_addr);
        const auto *octets = reinterpret_cast<const unsigned char *>(&ipv4->sin_addr.s_addr);
        char ip[16];
        std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
            octets[0], octets[1], octets[2], octets[3]);
        if (public_ipv4(ip)) { result = ip; break; }
    }
    freeaddrinfo(addresses);
    cloudplay::app_log_text("dns", result.empty() ?
        "native_lookup_invalid" : "native_lookup_succeeded",
        " host=" + host + " ip=" + (result.empty() ? "none" : result));
    return result;
}

std::map<std::string, std::string> configured_snapshot() {
    std::lock_guard<std::mutex> guard(configured_rules_mutex);
    return configured_rules;
}

} // namespace

namespace cloudplay {

void psn_set_resolve_rules(const std::map<std::string, std::string> &rules) {
    {
        std::lock_guard<std::mutex> guard(configured_rules_mutex);
        configured_rules = rules;
    }
    {
        std::lock_guard<std::mutex> guard(dynamic_rules_mutex);
        dynamic_rules.clear();
    }
    app_log("dns", "settings_ip_map_loaded", static_cast<int>(rules.size()));
}

std::vector<std::pair<std::string, std::string>> psn_resolve_rules() {
    const auto static_rules = configured_snapshot();
    std::vector<std::pair<std::string, std::string>> result(
        static_rules.begin(), static_rules.end());
    std::lock_guard<std::mutex> guard(dynamic_rules_mutex);
    for (const auto &entry : dynamic_rules)
        if (static_rules.find(entry.first) == static_rules.end()) result.push_back(entry);
    return result;
}

} // namespace cloudplay

extern "C" struct curl_slist *cloudplay_psn_resolves(const char *url) {
    const auto static_rules = configured_snapshot();
    const std::string host = url_host(url);
    if (host.empty()) {
        cloudplay::app_log("dns", "invalid_https_host");
        return nullptr;
    }
    std::string dynamic_address;
    if (static_rules.find(host) == static_rules.end()) {
        {
            std::lock_guard<std::mutex> guard(dynamic_rules_mutex);
            const auto found = dynamic_rules.find(host);
            if (found != dynamic_rules.end()) dynamic_address = found->second;
        }
        if (dynamic_address.empty()) {
            dynamic_address = resolve_with_ps5_dns(host);
            if (dynamic_address.empty()) return nullptr;
            std::lock_guard<std::mutex> guard(dynamic_rules_mutex);
            dynamic_rules[host] = dynamic_address;
        }
    }
    curl_slist *list = nullptr;
    for (const auto &[host, addresses] : static_rules) {
        const std::string entry = host + ":443:" + addresses;
        curl_slist *next = curl_slist_append(list, entry.c_str());
        if (!next) {
            curl_slist_free_all(list);
            cloudplay::app_log("dns", "ip_map_allocation_failed");
            return nullptr;
        }
        list = next;
    }
    if (!dynamic_address.empty()) {
        const std::string entry = host + ":443:" + dynamic_address;
        curl_slist *next = curl_slist_append(list, entry.c_str());
        if (!next) {
            curl_slist_free_all(list);
            cloudplay::app_log("dns", "dynamic_ip_map_allocation_failed");
            return nullptr;
        }
        list = next;
    }
    return list;
}
