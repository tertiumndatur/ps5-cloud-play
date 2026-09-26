// SPDX-License-Identifier: GPL-3.0-or-later
#include "cloud_access_setup.hpp"

#include "psn_resolve.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <random>
#include <vector>

#if CLOUDPLAY_PS5
#include <chiaki/base64.h>
#include <chiaki/random.h>
extern "C" {
#include "../third_party/chiaki-cloud/lib/src/psn_transport.h"
}
#endif

namespace cloudplay {
namespace {

constexpr const char *client_id = "ba495a24-818c-472b-b12d-ff231c1b5745";
#if CLOUDPLAY_PS5
constexpr const char *client_secret = "mvaiZkRsAsI1IBkY";
constexpr const char *token_url =
    "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token";
constexpr const char *redirect_uri =
    "https://remoteplay.dl.playstation.net/remoteplay/redirect";
constexpr const char *scope =
    "psn:clientapp referenceDataService:countryConfig.read "
    "pushNotification:webSocket.desktop.connect "
    "sessionManager:remotePlaySession.system.update";
#endif

constexpr const char *setup_hosts[] = {
    "auth.api.sonyentertainmentnetwork.com",
    "psnow.playstation.com",
    "www.playstation.com",
    "commerce.api.np.km.playstation.net",
    "web.np.playstation.com",
    "accounts.api.playstation.com",
    "apollo2.dl.playstation.net",
    "vulcan.dl.playstation.net",
    "image.api.playstation.com",
    "gs2-sec.ww.prod.dl.playstation.net"
};

bool printable_secret(const std::string &value) {
    if (value.empty() || value.size() > 2048) return false;
    for (unsigned char ch : value)
        if (ch < 0x21 || ch > 0x7e) return false;
    return true;
}

bool public_ipv4(const std::string &value) {
    unsigned parts[4]{};
    size_t at = 0;
    for (size_t index = 0; index < 4; ++index) {
        unsigned digits = 0;
        while (at < value.size() && value[at] >= '0' && value[at] <= '9') {
            parts[index] = parts[index] * 10 + static_cast<unsigned>(value[at++] - '0');
            if (++digits > 3 || parts[index] > 255) return false;
        }
        if (!digits) return false;
        if (index != 3 && (at == value.size() || value[at++] != '.')) return false;
    }
    return at == value.size() && parts[0] > 0 && parts[0] != 10 &&
        parts[0] != 127 && parts[0] < 224 &&
        !(parts[0] == 100 && parts[1] >= 64 && parts[1] <= 127) &&
        !(parts[0] == 169 && parts[1] == 254) &&
        !(parts[0] == 172 && parts[1] >= 16 && parts[1] <= 31) &&
        !(parts[0] == 192 && parts[1] == 168);
}

bool parse_npsso(const nlohmann::json &value, std::string &npsso) {
    if (!value.is_string()) return false;
    npsso = value.get<std::string>();
    if (!npsso.empty() && npsso.front() == '{') {
        const auto document = nlohmann::json::parse(npsso, nullptr, false);
        if (!document.is_object()) return false;
        const auto found = document.find("npsso");
        if (found == document.end() || !found->is_string()) return false;
        npsso = found->get<std::string>();
    }
    if (npsso.size() >= 6 && npsso.substr(0, 6) == "npsso=") npsso.erase(0, 6);
    return printable_secret(npsso);
}

bool valid_redirect(const std::string &url) {
    constexpr std::string_view prefix =
        "https://remoteplay.dl.playstation.net/remoteplay/redirect?";
    if (url.size() > 8192 || url.compare(0, prefix.size(), prefix) != 0) return false;
    const size_t code = url.find("code=", prefix.size());
    return code != std::string::npos && code + 5 < url.size();
}

std::string random_duid() {
    std::array<uint8_t, 16> bytes{};
#if CLOUDPLAY_PS5
    if (chiaki_random_bytes_crypt(bytes.data(), bytes.size()) != CHIAKI_ERR_SUCCESS)
        return {};
#else
    std::random_device source;
    for (uint8_t &byte : bytes) byte = static_cast<uint8_t>(source());
#endif
    std::string result = "0000000700410080";
    char pair[3];
    for (uint8_t byte : bytes) {
        std::snprintf(pair, sizeof(pair), "%02x", byte);
        result += pair;
    }
    return result;
}

#if CLOUDPLAY_PS5
std::string base64(const uint8_t *data, size_t size) {
    std::string output((size + 2) / 3 * 4 + 1, '\0');
    if (chiaki_base64_encode(data, size, output.data(), output.size()) !=
        CHIAKI_ERR_SUCCESS) return {};
    output.resize(std::strlen(output.c_str()));
    return output;
}

bool request_json(PsnHttpMethod method, const std::string &url,
                  const std::vector<std::string> &header_values,
                  const std::string &payload, nlohmann::json &document,
                  std::string &error) {
    std::vector<const char *> headers;
    for (const std::string &value : header_values) headers.push_back(value.c_str());
    PsnHttpRequest request{};
    request.method = method;
    request.url = url.c_str();
    request.headers = headers.data();
    request.headers_count = headers.size();
    request.payload = payload.empty() ? nullptr : payload.data();
    request.payload_size = payload.size();
    request.deadline_ms = 30000;
    request.follow_redirects = false;
    PsnHttpReply reply{};
    const ChiakiErrorCode result = psn_http_execute(nullptr, &request, &reply);
    if (result != CHIAKI_ERR_SUCCESS) {
        error = "Could not connect to the account service";
        return false;
    }
    const bool ok = reply.status >= 200 && reply.status < 300 && reply.body &&
        (document = nlohmann::json::parse(reply.body,
            reply.body + reply.body_size, nullptr, false)).is_object();
    if (!ok) error = "The account service rejected the setup request";
    psn_http_reply_clear(&reply);
    return ok;
}

std::string escaped(const char *value) {
    char *encoded = psn_url_escape(value);
    std::string result = encoded ? encoded : "";
    std::free(encoded);
    return result;
}

std::string authorization_code(const std::string &redirect) {
    char *code = psn_url_parameter(redirect.c_str(), "code");
    std::string result;
    if (code) {
        CURL *curl = curl_easy_init();
        int size = 0;
        char *decoded = curl ? curl_easy_unescape(curl, code, 0, &size) : nullptr;
        if (decoded && size > 0) result.assign(decoded, static_cast<size_t>(size));
        if (decoded) curl_free(decoded);
        if (curl) curl_easy_cleanup(curl);
    }
    std::free(code);
    return result;
}

std::string expiry_string(int seconds) {
    const std::time_t expiry = std::time(nullptr) + seconds;
    std::tm utc{};
    gmtime_r(&expiry, &utc);
    char result[64];
    std::strftime(result, sizeof(result), "%Y-%m-%d %H:%M:%S GMT+0", &utc);
    return result;
}
#endif

} // namespace

bool parse_cloud_setup_request(std::string_view json, CloudSetupInput &input) {
    const auto document = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
    if (!document.is_object() || document.size() != 3) return false;
    const auto redirect = document.find("redirectUrl");
    const auto npsso = document.find("npsso");
    const auto resolves = document.find("dnsResolves");
    if (redirect == document.end() || !redirect->is_string() ||
        npsso == document.end() || resolves == document.end() ||
        !resolves->is_object() || resolves->size() != std::size(setup_hosts)) return false;
    CloudSetupInput parsed;
    parsed.redirect_url = redirect->get<std::string>();
    if (!valid_redirect(parsed.redirect_url) || !parse_npsso(*npsso, parsed.npsso))
        return false;
    for (const char *host : setup_hosts) {
        const auto found = resolves->find(host);
        if (found == resolves->end() || !found->is_array() || found->empty() ||
            found->size() > 4) return false;
        std::string joined;
        for (const auto &address : *found) {
            if (!address.is_string()) return false;
            const std::string ip = address.get<std::string>();
            if (!public_ipv4(ip)) return false;
            if (!joined.empty()) joined += ',';
            joined += ip;
        }
        parsed.dns_resolves.emplace(host, std::move(joined));
    }
    input = std::move(parsed);
    return true;
}

std::string cloud_setup_login_url() {
    const std::string duid = random_duid();
    if (duid.empty()) return {};
    return std::string("https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize?") +
        "service_entity=urn%3Aservice-entity%3Apsn&response_type=code&client_id=" +
        client_id + "&redirect_uri=https%3A%2F%2Fremoteplay.dl.playstation.net%2F"
        "remoteplay%2Fredirect&scope=psn%3Aclientapp%20referenceDataService%3A"
        "countryConfig.read%20pushNotification%3AwebSocket.desktop.connect%20"
        "sessionManager%3AremotePlaySession.system.update&request_locale=en_US&ui=pr&"
        "service_logo=ps&layout_type=popup&smcid=remoteplay&prompt=always&"
        "PlatformPrivacyWs1=minimal&duid=" + duid;
}

bool provision_cloud_access(const CloudSetupInput &input, Credentials &credentials,
                            const std::function<void(const char *)> &progress,
                            std::string &error) {
#if !CLOUDPLAY_PS5
    (void)input; (void)credentials; (void)progress;
    error = "Setup provisioning is available on PS5";
    return false;
#else
    psn_set_resolve_rules(input.dns_resolves);
    progress("Exchanging authorization code...");
    const std::string code = authorization_code(input.redirect_url);
    if (code.empty()) { error = "The redirect URL has no authorization code"; return false; }
    const std::string client_credentials = std::string(client_id) + ":" + client_secret;
    const std::string auth = base64(reinterpret_cast<const uint8_t *>(
        client_credentials.data()), client_credentials.size());
    const std::string form = "grant_type=authorization_code&code=" + escaped(code.c_str()) +
        "&scope=" + escaped(scope) + "&redirect_uri=" + escaped(redirect_uri);
    nlohmann::json token;
    if (!request_json(PSN_HTTP_POST, token_url,
            {"Authorization: Basic " + auth,
             "Content-Type: application/x-www-form-urlencoded",
             "Accept: application/json"}, form, token, error)) return false;
    const auto access = token.find("access_token");
    const auto refresh = token.find("refresh_token");
    const auto expires = token.find("expires_in");
    if (access == token.end() || !access->is_string() || refresh == token.end() ||
        !refresh->is_string() || expires == token.end() || !expires->is_number_integer() ||
        expires->get<int>() <= 0) {
        error = "The token response is incomplete"; return false;
    }
    Credentials result;
    result.npsso = input.npsso;
    result.psn_access_token = access->get<std::string>();
    result.psn_refresh_token = refresh->get<std::string>();
    result.psn_auth_token_expiry = expiry_string(expires->get<int>());

    progress("Getting account information...");
    nlohmann::json account;
    if (!request_json(PSN_HTTP_GET,
            std::string(token_url) + "/" + escaped(result.psn_access_token.c_str()),
            {"Authorization: Basic " + auth, "Accept: application/json"}, {},
            account, error)) return false;
    const auto user = account.find("user_id");
    if (user == account.end() || (!user->is_string() && !user->is_number_unsigned() &&
                                  !user->is_number_integer())) {
        error = "The account response has no user ID"; return false;
    }
    uint64_t account_id = 0;
    try {
        account_id = user->is_string() ? std::stoull(user->get<std::string>()) :
            user->get<uint64_t>();
    } catch (...) {
        error = "The account response has an invalid user ID"; return false;
    }
    std::array<uint8_t, 8> little_endian{};
    for (size_t index = 0; index < little_endian.size(); ++index)
        little_endian[index] = static_cast<uint8_t>(account_id >> (index * 8));
    result.psn_account_id = base64(little_endian.data(), little_endian.size());
    if (!valid_credentials(result)) { error = "The generated credentials are incomplete"; return false; }
    credentials = std::move(result);
    progress("Saving settings...");
    return true;
#endif
}

} // namespace cloudplay
