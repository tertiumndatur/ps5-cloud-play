// SPDX-License-Identifier: GPL-3.0-or-later
#include "credential_http.hpp"
#include "app_log.hpp"
#include "psn_resolve.h"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#if CLOUDPLAY_PS5
extern "C" int ps5_socket_fcntl(int socket_id, int command, ...);
#endif

namespace cloudplay {
namespace {

uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool nonblocking(int fd) {
#if CLOUDPLAY_PS5
    // PS5 network descriptors belong to sceNet; libc fcntl returns EACCES.
    return ps5_socket_fcntl(fd, F_SETFL, O_NONBLOCK) == 0;
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

std::string lower(std::string input) {
    for (char &ch : input) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return input;
}

std::string local_ipv4() {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return {};
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    inet_pton(AF_INET, "1.1.1.1", &remote.sin_addr);
    std::string result;
    if (connect(fd, reinterpret_cast<sockaddr *>(&remote), sizeof(remote)) == 0) {
        sockaddr_in local{};
        socklen_t size = sizeof(local);
        char text[INET_ADDRSTRLEN]{};
        if (getsockname(fd, reinterpret_cast<sockaddr *>(&local), &size) == 0 &&
            inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text))) result = text;
    }
    close(fd);
    return result;
}

std::string setup_page(const std::string &login_url) {
    static constexpr const char *page = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cloud Play Setup</title><style>
:root{color-scheme:dark;font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:linear-gradient(145deg,#07132d,#171044);color:#f7f7fb;display:grid;place-items:center;padding:24px}
main{width:min(720px,100%);background:#ffffff10;border:1px solid #ffffff24;border-radius:24px;padding:32px;box-shadow:0 24px 80px #0008}
h1{font-size:34px;margin:0 0 8px}p{color:#c9cbe0;line-height:1.5}.step{margin-top:28px;padding-top:24px;border-top:1px solid #ffffff1f}
h2{font-size:20px;margin:0 0 12px}.button,button{display:inline-block;border:0;border-radius:12px;padding:13px 18px;background:#f5f1e9;color:#11152f;font-weight:700;font-size:16px;text-decoration:none;cursor:pointer}
textarea{display:block;width:100%;min-height:96px;margin-top:14px;padding:14px;border-radius:12px;border:1px solid #ffffff30;background:#080d22;color:#fff;resize:vertical;font:14px ui-monospace,monospace}
button{width:100%;margin-top:28px;background:#5da9ff}button:disabled{opacity:.55;cursor:wait}#status{min-height:24px;margin-top:18px;color:#b9d7ff}.error{color:#ff9f9f!important}.success{color:#94ebb0!important}footer{margin-top:28px;padding-top:20px;border-top:1px solid #ffffff1f;color:#9296b0;font-size:13px;line-height:1.5}
</style></head><body><main><h1>Cloud Play Setup</h1><p>Keep Cloud Play open on your PS5 while completing these steps. You can return to this page whenever you want to refresh your tokens.</p>
<section class="step"><h2>1. Sign in</h2><a class="button" href="__LOGIN_URL__" target="_blank" rel="noopener">Open sign-in page</a><p>After sign-in, copy the complete URL from the address bar and paste it below.</p><textarea id="redirect" autocomplete="off" spellcheck="false" placeholder="Redirect URL"></textarea></section>
<section class="step"><h2>2. Get NPSSO</h2><a class="button" href="https://ca.account.sony.com/api/v1/ssocookie" target="_blank" rel="noopener">Open NPSSO page</a><p>Copy the displayed JSON or its npsso value and paste it below.</p><textarea id="npsso" autocomplete="off" spellcheck="false" placeholder="NPSSO or JSON"></textarea></section>
<button id="submit">Complete setup</button><div id="status"></div><footer>Cloud Play is an independent, unofficial project. It is not affiliated with, endorsed by, certified by, or sponsored by Sony Interactive Entertainment. PlayStation and related trademarks belong to their respective owners.</footer></main><script>
const hosts=["auth.api.sonyentertainmentnetwork.com","psnow.playstation.com","www.playstation.com","commerce.api.np.km.playstation.net","web.np.playstation.com","accounts.api.playstation.com","apollo2.dl.playstation.net","vulcan.dl.playstation.net","image.api.playstation.com","gs2-sec.ww.prod.dl.playstation.net"];
const status=document.querySelector('#status'),submit=document.querySelector('#submit');
function show(message,kind=''){status.textContent=message;status.className=kind}
function publicIPv4(value){const p=value.split('.').map(Number);return p.length===4&&p.every((v,i)=>Number.isInteger(v)&&v>=0&&v<=255)&&p[0]>0&&p[0]!==10&&p[0]!==127&&p[0]<224&&!(p[0]===100&&p[1]>=64&&p[1]<=127)&&!(p[0]===169&&p[1]===254)&&!(p[0]===172&&p[1]>=16&&p[1]<=31)&&!(p[0]===192&&p[1]===168)}
async function resolveHost(host){const providers=['https://cloudflare-dns.com/dns-query?name='+encodeURIComponent(host)+'&type=A','https://dns.google/resolve?name='+encodeURIComponent(host)+'&type=A'];for(const url of providers){try{const response=await fetch(url,{cache:'no-store',headers:{Accept:'application/dns-json'}});if(!response.ok)continue;const data=await response.json();const addresses=[...new Set((data.Answer||[]).filter(a=>a.type===1&&publicIPv4(a.data)).map(a=>a.data))].slice(0,4);if(addresses.length)return addresses}catch(error){}}throw new Error('No public address found for '+host)}
async function poll(){const response=await fetch('/setup/status',{cache:'no-store'});const data=await response.json();show(data.message,data.state==='error'?'error':data.state==='success'?'success':'');if(data.state==='working')setTimeout(poll,750);else submit.disabled=false}
submit.addEventListener('click',async()=>{const redirectUrl=document.querySelector('#redirect').value.trim(),npsso=document.querySelector('#npsso').value.trim();if(!redirectUrl||!npsso){show('Paste both the redirect URL and NPSSO.','error');return}submit.disabled=true;try{show('Resolving required hosts...');const pairs=await Promise.all(hosts.map(async host=>[host,await resolveHost(host)]));const dnsResolves=Object.fromEntries(pairs);show('Sending setup details to PS5...');const response=await fetch('/setup',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({redirectUrl,npsso,dnsResolves})});const data=await response.json();if(!response.ok)throw new Error(data.message||'Setup request was rejected');show('Starting setup...');setTimeout(poll,300)}catch(error){show(error.message||String(error),'error');submit.disabled=false}});
</script></body></html>)HTML";
    std::string result(page);
    std::string html_login_url;
    html_login_url.reserve(login_url.size() + 64);
    for (char ch : login_url) {
        if (ch == '&') html_login_url += "&amp;";
        else html_login_url += ch;
    }
    const size_t marker = result.find("__LOGIN_URL__");
    if (marker != std::string::npos) result.replace(marker, 13, html_login_url);
    return result;
}

} // namespace

CredentialHttpServer::CredentialHttpServer(CloudSettingsStore &settings_store,
                                           CloudSettings &settings)
    : settings_store_(settings_store), settings_(settings) {}

CredentialHttpServer::~CredentialHttpServer() { stop(); }

void CredentialHttpServer::update_access_status(bool force) {
    const CloudAccessState state = cloud_access_state(settings_);
    if (!force && state == access_state_) return;
    access_state_ = state;
    if (state == CloudAccessState::Ready)
        status_ = "Cloud access ready · Setup: " + setup_url_;
    else if (state == CloudAccessState::Expired)
        status_ = "PSN token expiry passed · Setup: " + setup_url_;
    else
        status_ = "Setup required · Open " + setup_url_ + " in a browser";
}

bool CredentialHttpServer::start(uint16_t port) {
    stop();
    app_log("http", "listener_start", port);
    error_step_ = 0;
    error_number_ = 0;
    listener_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) {
        error_step_ = 2;
        error_number_ = errno;
        app_log("http", "socket_failed", error_number_);
        status_ = "Socket unavailable";
        return false;
    }
    const int reuse = 1;
    setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        error_step_ = 3;
        error_number_ = errno;
    } else if (listen(listener_, 1) != 0) {
        error_step_ = 4;
        error_number_ = errno;
    } else if (!nonblocking(listener_)) {
        error_step_ = 5;
        error_number_ = errno;
    }
    if (error_step_) {
        app_log("http", "listener_failed_step", error_step_);
        app_log("http", "listener_errno", error_number_);
        status_ = "Could not start HTTP receiver";
        stop();
        return false;
    }
    socklen_t length = sizeof(address);
    if (getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        error_step_ = 6;
        error_number_ = errno;
        app_log("http", "getsockname_failed", error_number_);
        status_ = "Could not read HTTP port";
        stop();
        return false;
    }
    port_ = ntohs(address.sin_port);
    local_address_ = local_ipv4();
    setup_url_ = local_address_.empty() ? "http://PS5_IP:" + std::to_string(port_) :
        "http://" + local_address_ + ":" + std::to_string(port_);
    login_url_ = cloud_setup_login_url();
    app_log("http", "listener_ready", port_);
    update_access_status(true);
    return true;
}

void CredentialHttpServer::close_client() {
    if (client_ >= 0) close(client_);
    client_ = -1;
    request_.clear();
    response_.clear();
    response_sent_ = 0;
}

void CredentialHttpServer::stop() {
    close_client();
    if (listener_ >= 0) close(listener_);
    listener_ = -1;
    port_ = 0;
    if (setup_worker_.joinable()) setup_worker_.join();
}

void CredentialHttpServer::respond_json(int code, const char *reason,
                                        const std::string &body) {
    app_log("http", "response", code);
    char header[256];
    const int size = std::snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\nCache-Control: no-store\r\n"
        "Connection: close\r\n\r\n", code, reason, body.size());
    response_.assign(header, static_cast<size_t>(size));
    response_ += body;
    response_sent_ = 0;
}

void CredentialHttpServer::respond_status(int code, const char *reason,
                                          const char *message) {
    respond_json(code, reason,
        nlohmann::json{{"status", message}, {"message", message}}.dump() + "\n");
}

void CredentialHttpServer::respond_html(const std::string &body) {
    char header[256];
    const int size = std::snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n", body.size());
    response_.assign(header, static_cast<size_t>(size));
    response_ += body;
    response_sent_ = 0;
}

void CredentialHttpServer::start_setup(CloudSetupInput input) {
    if (setup_worker_.joinable()) setup_worker_.join();
    {
        std::lock_guard<std::mutex> guard(setup_mutex_);
        setup_busy_ = true;
        setup_result_ready_ = false;
        setup_result_ok_ = false;
        setup_progress_ = "Starting setup...";
        setup_error_.clear();
        setup_resolves_ = input.dns_resolves;
    }
    setup_worker_ = std::thread([this, input = std::move(input)]() mutable {
        Credentials credentials;
        std::string error;
        const bool ok = provision_cloud_access(input, credentials,
            [this](const char *progress) {
                std::lock_guard<std::mutex> guard(setup_mutex_);
                setup_progress_ = progress;
            }, error);
        std::lock_guard<std::mutex> guard(setup_mutex_);
        setup_credentials_ = std::move(credentials);
        setup_error_ = std::move(error);
        setup_result_ok_ = ok;
        setup_result_ready_ = true;
    });
}

void CredentialHttpServer::apply_setup_result() {
    Credentials credentials;
    std::map<std::string, std::string> resolves;
    std::string error;
    bool ok = false;
    {
        std::lock_guard<std::mutex> guard(setup_mutex_);
        if (!setup_result_ready_) return;
        credentials = std::move(setup_credentials_);
        resolves = std::move(setup_resolves_);
        error = std::move(setup_error_);
        ok = setup_result_ok_;
        setup_result_ready_ = false;
    }
    if (setup_worker_.joinable()) setup_worker_.join();
    if (ok) {
        CloudSettings updated = settings_;
        updated.psn_tokens = std::move(credentials);
        updated.dns_resolves = std::move(resolves);
        ok = settings_store_.save(updated);
        if (ok) {
            settings_ = std::move(updated);
            ++settings_generation_;
            update_access_status(true);
            app_log("http", "browser_setup_saved");
        } else error = "Could not save settings";
    }
    if (!ok) {
#if CLOUDPLAY_PS5
        psn_set_resolve_rules(settings_.dns_resolves);
#endif
        status_ = "Setup failed; open " + setup_url_ + " to try again";
        app_log("http", "browser_setup_failed");
    }
    std::lock_guard<std::mutex> guard(setup_mutex_);
    setup_busy_ = false;
    setup_result_ok_ = ok;
    setup_progress_ = ok ? "Setup complete. Games are now available." : error;
}

void CredentialHttpServer::process_request() {
    const size_t end = request_.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (request_.size() > 2048) respond_status(413, "Too Large", "bad_request");
        return;
    }
    if (end > 2048) { respond_status(413, "Too Large", "bad_request"); return; }
    const size_t first_end = request_.find("\r\n");
    if (first_end == std::string::npos) {
        respond_status(400, "Bad Request", "bad_request"); return;
    }
    const std::string line = request_.substr(0, first_end);
    const bool settings_request = line == "POST /settings HTTP/1.1" ||
                                  line == "POST /settings HTTP/1.0";
    const bool setup_request = line == "POST /setup HTTP/1.1" ||
                               line == "POST /setup HTTP/1.0";
    const bool page_request = line == "GET / HTTP/1.1" || line == "GET / HTTP/1.0" ||
                              line == "GET /setup HTTP/1.1" || line == "GET /setup HTTP/1.0";
    const bool status_request = line == "GET /setup/status HTTP/1.1" ||
                                line == "GET /setup/status HTTP/1.0";
    if (page_request) { respond_html(setup_page(login_url_)); return; }
    if (status_request) {
        std::lock_guard<std::mutex> guard(setup_mutex_);
        const char *state = setup_busy_ ? "working" :
            (setup_result_ok_ ? "success" : (setup_progress_ == "Waiting for setup details" ?
             "waiting" : "error"));
        respond_json(200, "OK", nlohmann::json{{"state", state},
            {"message", setup_progress_}}.dump() + "\n");
        return;
    }
    if (!settings_request && !setup_request) {
        respond_status(404, "Not Found", "not_found"); return;
    }
    size_t content_length = 0;
    bool length_seen = false, type_seen = false;
    for (size_t at = first_end + 2; at < end;) {
        const size_t next = request_.find("\r\n", at);
        if (next == std::string::npos || next > end) {
            respond_status(400, "Bad Request", "bad_request"); return;
        }
        const std::string line = request_.substr(at, next - at);
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            respond_status(400, "Bad Request", "bad_request"); return;
        }
        const std::string name = lower(line.substr(0, colon));
        size_t value_at = colon + 1;
        while (value_at < line.size() && line[value_at] == ' ') ++value_at;
        const std::string value = line.substr(value_at);
        if (name == "content-length") {
            if (length_seen || value.empty() || value.size() > 8) {
                respond_status(400, "Bad Request", "bad_request"); return;
            }
            for (char ch : value) {
                if (ch < '0' || ch > '9') {
                    respond_status(400, "Bad Request", "bad_request"); return;
                }
                content_length = content_length * 10 + static_cast<size_t>(ch - '0');
            }
            length_seen = true;
        } else if (name == "content-type") {
            const std::string content_type = lower(value);
            const bool correct = content_type == "application/json" ||
                content_type.rfind("application/json;", 0) == 0;
            if (type_seen || !correct) {
                respond_status(415, "Unsupported Media Type", "bad_content_type"); return;
            }
            type_seen = true;
        } else if (name == "transfer-encoding") {
            respond_status(400, "Bad Request", "bad_request"); return;
        }
        at = next + 2;
    }
    if (!length_seen || !type_seen || content_length == 0 ||
        content_length > 64u * 1024u) {
        respond_status(400, "Bad Request", "bad_request"); return;
    }
    if (request_.size() - end - 4 < content_length) return;
    const std::string_view body(request_.data() + end + 4, content_length);
    if (settings_request) {
        Credentials tokens;
        std::map<std::string, std::string> resolves;
        if (!parse_cloud_access_json(body, tokens, resolves)) {
            respond_status(400, "Bad Request", "invalid_settings"); return;
        }
        CloudSettings updated = settings_;
        updated.psn_tokens = std::move(tokens);
        updated.dns_resolves = std::move(resolves);
        if (!settings_store_.save(updated)) {
            status_ = "Could not save settings";
            respond_status(500, "Internal Server Error", "save_failed"); return;
        }
        settings_ = std::move(updated);
        ++settings_generation_;
        app_log("http", "settings_updated", static_cast<int>(content_length));
        update_access_status(true);
        respond_status(200, "OK", "saved");
        return;
    }
    {
        std::lock_guard<std::mutex> guard(setup_mutex_);
        if (setup_busy_) {
            respond_status(409, "Conflict", "setup_in_progress"); return;
        }
    }
    CloudSetupInput input;
    if (!parse_cloud_setup_request(body, input)) {
        respond_status(400, "Bad Request", "invalid_setup"); return;
    }
    start_setup(std::move(input));
    respond_status(202, "Accepted", "started");
}

void CredentialHttpServer::poll() {
    apply_setup_result();
    update_access_status();
    if (listener_ < 0) return;
    if (client_ < 0) {
        client_ = accept(listener_, nullptr, nullptr);
        if (client_ < 0) return;
        app_log("http", "client_connected");
        if (!nonblocking(client_)) { close_client(); return; }
        client_since_ms_ = now_ms();
    }
    if (now_ms() - client_since_ms_ > 30000) {
        app_log("http", "client_timeout");
        close_client();
        return;
    }
    if (response_.empty()) {
        char buffer[32768];
        for (int i = 0; i < 8 && response_.empty(); ++i) {
            const ssize_t count = recv(client_, buffer, sizeof(buffer), 0);
            if (count == 0) { close_client(); return; }
            if (count > 0) {
                request_.append(buffer, static_cast<size_t>(count));
                if (request_.size() > 64u * 1024u + 2052u)
                    respond_status(413, "Too Large", "bad_request");
                else process_request();
            } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                break;
            } else {
                close_client(); return;
            }
        }
    }
    if (!response_.empty()) {
        const ssize_t count = send(client_, response_.data() + response_sent_,
                                   response_.size() - response_sent_, 0);
        if (count > 0) response_sent_ += static_cast<size_t>(count);
        else if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close_client(); return;
        }
        if (response_sent_ == response_.size()) close_client();
    }
}

} // namespace cloudplay
