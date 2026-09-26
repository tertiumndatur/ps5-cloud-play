#include "cloud_catalog.hpp"
#include "cloud_access_setup.hpp"
#include "cloud_settings.hpp"
#include "credential_http.hpp"
#include "recent_launches.hpp"

#include <arpa/inet.h>
#include <cassert>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

using namespace cloudplay;

static std::string http_exchange(CredentialHttpServer &server, const std::string &message) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(server.port());
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    assert(connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    assert(send(fd, message.data(), message.size(), 0) ==
           static_cast<ssize_t>(message.size()));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    std::string response;
    for (int index = 0; index < 1000; ++index) {
        server.poll();
        char buffer[512];
        const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count > 0) response.append(buffer, static_cast<size_t>(count));
        if (count == 0) break;
        usleep(1000);
    }
    close(fd);
    return response;
}

static std::string request(CredentialHttpServer &server, const std::string &body) {
    return http_exchange(server, "POST /settings HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Type: application/json\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body);
}

int main() {
    char path[] = "/tmp/chiaki-cloud-test-XXXXXX";
    assert(mkdtemp(path));
    const std::string root(path);
    CloudSettingsStore settings_store(root + "/settings.json");
    CloudSettings settings;
    settings.ps5_cloud.game_language = "ru-RU";
    settings.ps5_cloud.bitrate_kbps = 25000;
    settings.ps_now.game_language = "en-US";
    settings.ps_now.resolution = 1440;
    settings.show_stream_stats = true;
    assert(settings_store.save(settings));
    assert(!cloud_access_ready(settings));

    CredentialHttpServer server(settings_store, settings);
    assert(server.start(0));
    const std::string page = http_exchange(server,
        "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert(page.find("200 OK") != std::string::npos);
    assert(page.find("Cloud Play Setup") != std::string::npos);
    assert(page.find("dns.google/resolve") != std::string::npos);
    assert(page.find("whenever you want to refresh your tokens") != std::string::npos);

    Credentials expiry_check;
    assert(psn_token_expiry_state(expiry_check) == PsnTokenExpiryState::Missing);
    expiry_check.psn_auth_token_expiry = "2099-01-01 00:00:00 GMT+0";
    assert(psn_token_expiry_state(expiry_check) == PsnTokenExpiryState::Active);
    expiry_check.psn_auth_token_expiry = "2000-01-01 00:00:00 GMT+0";
    assert(psn_token_expiry_state(expiry_check) == PsnTokenExpiryState::Expired);
    expiry_check.psn_auth_token_expiry = "not-a-date";
    assert(psn_token_expiry_state(expiry_check) == PsnTokenExpiryState::Invalid);

    const std::string setup_json =
        R"({"redirectUrl":"https://remoteplay.dl.playstation.net/remoteplay/redirect?code=test-code","npsso":"test-npsso","dnsResolves":{)"
        R"("auth.api.sonyentertainmentnetwork.com":["203.0.113.9"],)"
        R"("psnow.playstation.com":["203.0.113.10"],)"
        R"("www.playstation.com":["203.0.113.11"],)"
        R"("commerce.api.np.km.playstation.net":["203.0.113.12"],)"
        R"("web.np.playstation.com":["203.0.113.13"],)"
        R"("accounts.api.playstation.com":["203.0.113.14"],)"
        R"("apollo2.dl.playstation.net":["203.0.113.15"],)"
        R"("vulcan.dl.playstation.net":["203.0.113.16"],)"
        R"("image.api.playstation.com":["203.0.113.17"],)"
        R"("gs2-sec.ww.prod.dl.playstation.net":["203.0.113.18"]}})";
    CloudSetupInput setup_input;
    assert(parse_cloud_setup_request(setup_json, setup_input));
    assert(setup_input.npsso == "test-npsso");
    assert(setup_input.dns_resolves.size() == 10);
    const std::string incomplete =
        R"({"psnTokens":{"npsso":"n","psn_auth_token":"a","psn_refresh_token":"r","psn_auth_token_expiry":"2099-01-01 00:00:00 GMT+0","psn_account_id":"i"},"dnsResolves":{}})";
    assert(request(server, incomplete).find("400 Bad Request") != std::string::npos);

    const std::string complete =
        R"({"psnTokens":{"npsso":"n","psn_auth_token":"a","psn_refresh_token":"r","psn_auth_token_expiry":"2099-01-01 00:00:00 GMT+0","psn_account_id":"i"},"dnsResolves":{)"
        R"("psnow.playstation.com":["203.0.113.10"],)"
        R"("www.playstation.com":["203.0.113.11"],)"
        R"("commerce.api.np.km.playstation.net":["203.0.113.12"],)"
        R"("web.np.playstation.com":["203.0.113.13"],)"
        R"("accounts.api.playstation.com":["203.0.113.14"],)"
        R"("apollo2.dl.playstation.net":["203.0.113.15"],)"
        R"("vulcan.dl.playstation.net":["203.0.113.16"],)"
        R"("image.api.playstation.com":["203.0.113.17"],)"
        R"("gs2-sec.ww.prod.dl.playstation.net":["203.0.113.18"]}})";
    const std::string reply = request(server, complete);
    assert(reply.find("200 OK") != std::string::npos);
    assert(reply.find("203.0.113.10") == std::string::npos);
    assert(cloud_access_ready(settings));
    assert(settings.ps5_cloud.game_language == "ru-RU");
    assert(settings.ps_now.game_language == "en-US");
    assert(server.status().find("Cloud access ready") != std::string::npos);
    const std::string page_after_setup = http_exchange(server,
        "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    assert(page_after_setup.find("200 OK") != std::string::npos);
    CloudSettings loaded;
    assert(settings_store.load(loaded));
    assert(cloud_access_ready(loaded));
    assert(loaded.show_stream_stats);
    settings.psn_tokens.psn_auth_token_expiry = "2000-01-01 00:00:00 GMT+0";
    server.poll();
    assert(server.access_state() == CloudAccessState::Expired);
    assert(server.status().find("token expiry passed") != std::string::npos);
    assert(cloud_access_ready(settings));
    server.stop();

    const std::string catalog_json =
        R"({"nativeMode":false,"fallbackRegion":"PL","resolvedStoreLang":"pl","settledLocale":"pl-PL","games":[{"name":"Test Game","platform":"ps5","serviceType":"pscloud","streamIdentifier":"GAME-1","isOwned":true,"isPlayable":true}]})";
    std::vector<CloudGame> games;
    std::string error;
    CloudCatalogContext context;
    assert(parse_cloud_catalog(catalog_json, games, error, &context));
    assert(games.size() == 1 && games[0].identifier == "GAME-1");

    const std::string recent_path = root + "/recent.json";
    RecentLaunchStore recent_store(recent_path);
    std::vector<RecentLaunch> recent;
    record_recent_launch(recent, games[0], games[0]);
    assert(recent.size() == 1);
    assert(recent_launch_matches_game(recent[0], games[0]));
    assert(recent_store.save(recent));
    recent.clear();
    assert(recent_store.load(recent));
    assert(recent.size() == 1 && recent[0].variant_id == "GAME-1");

    unlink((root + "/settings.json").c_str());
    unlink(recent_path.c_str());
    rmdir(root.c_str());
    return 0;
}
