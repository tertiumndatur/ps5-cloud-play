// SPDX-License-Identifier: GPL-3.0-or-later
#include "cloud_settings.hpp"
#include "credential_http.hpp"
#include "recent_launches.hpp"
#include "radio_input.hpp"
#include "radio_ime.hpp"
#include "app_log.hpp"
#if CLOUDPLAY_PS5
#include "cloud_catalog_client.hpp"
#include "cloud_cover_downloader.hpp"
#include "cloud_ui_view.hpp"
#include "cloud_stream_client.hpp"
#include "psn_resolve.h"
#include <chiaki/common.h>
#endif

#include <SDL2/SDL.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr size_t catalog_columns = 6;
constexpr size_t catalog_rows = 2;
constexpr size_t catalog_slots = catalog_columns * catalog_rows;
constexpr int settings_row_count = 7;
#if CLOUDPLAY_PS5
constexpr uint64_t catalog_repeat_delay_ms = 320;
constexpr uint64_t catalog_repeat_interval_ms = 90;
#endif

void debug_event(const char *event, int result = 0) {
    cloudplay::app_log("app", event, result);
}

std::string resolution_label(int value) { return std::to_string(value) + "p"; }

struct CatalogView {
    std::vector<cloudplay::CloudGame> games;
    cloudplay::CloudCatalogContext context;
    std::vector<size_t> visible;
    std::string search;
    std::string status;
    int category = 1;
    int platform = 0;
    int service = 0;
    int sort = 0;
    int focus = 0; // 0..4 filters, 5 cards
    size_t selected = 0;
    bool choosing_variant = false;
    bool showing_purchase = false;
    std::string purchase_url;
    size_t variant_choice = 0;
    std::vector<cloudplay::RecentLaunch> recent_launches;
#if CLOUDPLAY_PS5
    cloudplay::CloudGame launch_game;
    cloudplay::CloudVariant launch_variant;
#endif
};

std::string lowercase(std::string value) {
    for (char &ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

bool equal_ci(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) return false;
    return true;
}

bool contains_lower(std::string_view value, std::string_view lower_needle) {
    if (lower_needle.empty()) return true;
    if (lower_needle.size() > value.size()) return false;
    for (size_t start = 0; start + lower_needle.size() <= value.size(); ++start) {
        size_t i = 0;
        while (i < lower_needle.size() &&
               std::tolower(static_cast<unsigned char>(value[start + i])) ==
                   static_cast<unsigned char>(lower_needle[i])) ++i;
        if (i == lower_needle.size()) return true;
    }
    return false;
}

void replace_all(std::string &value, std::string_view needle, std::string_view replacement) {
    if (needle.empty()) return;
    size_t position = 0;
    while ((position = value.find(needle, position)) != std::string::npos) {
        value.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
}

std::string display_game_name(std::string value) {
    replace_all(value, "\xE2\x80\x98", "'");
    replace_all(value, "\xE2\x80\x99", "'");
    replace_all(value, "\xCA\xBC", "'");
    replace_all(value, "\xC2\xAE", " (R)");
    replace_all(value, "\xE2\x84\xA2", " TM");
    return value;
}

int compare_ci(std::string_view left, std::string_view right) {
    const size_t count = std::min(left.size(), right.size());
    for (size_t i = 0; i < count; ++i) {
        const unsigned char a = static_cast<unsigned char>(
            std::tolower(static_cast<unsigned char>(left[i])));
        const unsigned char b = static_cast<unsigned char>(
            std::tolower(static_cast<unsigned char>(right[i])));
        if (a != b) return a < b ? -1 : 1;
    }
    return left.size() == right.size() ? 0 : left.size() < right.size() ? -1 : 1;
}

void filter_catalog(CatalogView &view) {
    view.choosing_variant = false;
    view.showing_purchase = false;
    view.visible.clear();
    static const char *platforms[] = {"", "ps5", "ps4", "ps3"};
    static const char *services[] = {"", "pscloud", "psnow"};
    const std::string query = lowercase(view.search);
    auto recent_rank = [&](const cloudplay::CloudGame &game) {
        for (size_t rank = 0; rank < view.recent_launches.size(); ++rank)
            if (cloudplay::recent_launch_matches_game(view.recent_launches[rank], game))
                return rank;
        return view.recent_launches.size();
    };
    for (size_t i = 0; i < view.games.size(); ++i) {
        const auto &game = view.games[i];
        if (view.category == 0 && recent_rank(game) == view.recent_launches.size()) continue;
        if (view.category == 2 && !game.is_playable) continue;
        if (view.category == 3 && !equal_ci(game.category, "owned")) continue;
        if (view.category == 4 && !equal_ci(game.category, "streamable")) continue;
        if (view.category == 5 && !equal_ci(game.category, "purchaseable")) continue;
        auto matches_variant = [&](const cloudplay::CloudVariant &variant) {
            return (view.platform == 0 || equal_ci(variant.platform, platforms[view.platform])) &&
                   (view.service == 0 || equal_ci(variant.service_type, services[view.service]));
        };
        bool matching = game.variants.empty() && matches_variant(game);
        for (const auto &variant : game.variants)
            matching = matching || matches_variant(variant);
        if (!matching) continue;
        if (!query.empty() && !contains_lower(game.name, query) &&
            !contains_lower(game.product_id, query)) continue;
        view.visible.push_back(i);
    }
    std::sort(view.visible.begin(), view.visible.end(), [&](size_t a, size_t b) {
        const auto &left = view.games[a];
        const auto &right = view.games[b];
        if (view.category == 0)
            return recent_rank(left) < recent_rank(right);
        if (view.sort == 0) {
            const int left_rank = left.is_playable ? (left.is_owned ? 0 : 1) :
                                  (left.is_owned ? 2 : 3);
            const int right_rank = right.is_playable ? (right.is_owned ? 0 : 1) :
                                   (right.is_owned ? 2 : 3);
            if (left_rank != right_rank) return left_rank < right_rank;
        }
        const int order = compare_ci(left.name, right.name);
        return view.sort == 2 ? order > 0 : order < 0;
    });
    if (view.selected >= view.visible.size()) view.selected = 0;
}

bool has_recent_games(const CatalogView &view) {
    return std::any_of(view.games.begin(), view.games.end(), [&](const auto &game) {
        return std::any_of(view.recent_launches.begin(), view.recent_launches.end(),
                           [&](const auto &launch) {
            return cloudplay::recent_launch_matches_game(launch, game);
        });
    });
}

void reset_focused_catalog_filter(CatalogView &view) {
    if (view.focus == 0)
        view.search.clear();
    else if (view.focus == 1)
        view.category = has_recent_games(view) ? 0 : 1;
    else if (view.focus == 2)
        view.platform = 0;
    else if (view.focus == 3)
        view.service = 0;
    else if (view.focus == 4)
        view.sort = 0;
    else
        return;
    view.selected = 0;
    filter_catalog(view);
}

#if CLOUDPLAY_PS5
void search_result(const char *value, void *user) {
    auto *view = static_cast<CatalogView *>(user);
    if (view && value) {
        view->search = value;
        filter_catalog(*view);
    }
}
#endif

void move_catalog(CatalogView &view, int direction) {
    if (view.showing_purchase) return;
    if (view.choosing_variant && !view.visible.empty()) {
        const auto &game = view.games[view.visible[view.selected]];
        if (!game.variants.empty())
            view.variant_choice = direction > 0 ?
                (view.variant_choice + 1) % game.variants.size() :
                (view.variant_choice + game.variants.size() - 1) % game.variants.size();
        return;
    }
    if (view.focus < 5) {
        if (direction == static_cast<int>(catalog_columns)) view.focus = 5;
        else if (direction == 1 || direction == -1)
            view.focus = (view.focus + direction + 5) % 5;
        return;
    }
    if (direction == -static_cast<int>(catalog_columns) &&
        view.selected < catalog_columns) { view.focus = 0; return; }
    if (view.visible.empty()) return;
    const int next = static_cast<int>(view.selected) + direction;
    if (next >= 0 && next < static_cast<int>(view.visible.size()))
        view.selected = static_cast<size_t>(next);
}

bool activate_catalog(CatalogView &view
#if CLOUDPLAY_PS5
                      , const std::string &npsso
#endif
                      ) {
    if (view.showing_purchase) return false;
    if (view.focus == 0) {
#if CLOUDPLAY_PS5
        radio_ime_request(view.search.c_str(), "Search games", "Game title",
                          search_result, &view);
#endif
        return false;
    }
    if (view.focus < 5) {
        int *option = view.focus == 1 ? &view.category :
                      view.focus == 2 ? &view.platform :
                      view.focus == 3 ? &view.service : &view.sort;
        const int count = view.focus == 1 ? 6 : view.focus == 2 ? 4 : 3;
        *option = (*option + 1) % count;
        filter_catalog(view);
        return false;
    }
    if (view.visible.empty()) return false;
    const auto &game = view.games[view.visible[view.selected]];
    if (!view.choosing_variant && game.variants.size() > 1) {
        view.choosing_variant = true;
        view.variant_choice = 0;
        const auto recent = std::find_if(view.recent_launches.begin(),
                                        view.recent_launches.end(), [&](const auto &launch) {
            return cloudplay::recent_launch_matches_game(launch, game);
        });
        if (recent != view.recent_launches.end()) {
            for (size_t index = 0; index < game.variants.size(); ++index) {
                const auto &candidate = game.variants[index];
                const bool identifier_match = !recent->variant_id.empty() &&
                    candidate.identifier == recent->variant_id;
                const bool service_match = recent->variant_id.empty() &&
                    candidate.platform == recent->platform &&
                    candidate.service_type == recent->service_type;
                if (identifier_match || service_match) {
                    view.variant_choice = index;
                    break;
                }
            }
        }
        return false;
    }
    const auto &variant = view.choosing_variant ? game.variants[view.variant_choice] :
                           static_cast<const cloudplay::CloudVariant &>(game);
    if (!variant.is_playable || variant.identifier.empty()) {
        const bool purchasable = equal_ci(game.category, "purchaseable") ||
                                 equal_ci(game.category, "purchasable");
        if (purchasable && (!game.concept_id.empty() || !game.product_id.empty())) {
            std::string locale = view.context.locale;
            if (locale.empty()) locale = view.context.store_language;
            if (locale.empty()) locale = "en-us";
            for (char &ch : locale) {
                if (ch == '_') ch = '-';
                else ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            locale.erase(std::remove_if(locale.begin(), locale.end(), [](unsigned char ch) {
                return !std::isalnum(ch) && ch != '-';
            }), locale.end());
            if (locale.find('-') == std::string::npos && !view.context.store_country.empty()) {
                locale += "-";
                for (unsigned char ch : view.context.store_country)
                    if (std::isalnum(ch)) locale += static_cast<char>(std::tolower(ch));
            }
            if (locale.empty()) locale = "en-us";
            view.purchase_url = "https://store.playstation.com/" + locale +
                (game.concept_id.empty() ? "/product/" + game.product_id :
                                           "/concept/" + game.concept_id);
            view.showing_purchase = true;
            view.status.clear();
        } else {
            view.status = "This version cannot be streamed";
        }
        view.choosing_variant = false;
        return false;
    }
#if CLOUDPLAY_PS5
    if (npsso.empty()) {
        view.status = "Set NPSSO before launching Cloud Play";
        return false;
    }
    view.launch_game = game;
    view.launch_variant = variant;
    view.status = "Allocating Cloud Play session";
#else
    view.status = "Stream launch requires PS5 hardware";
#endif
    view.choosing_variant = false;
#if CLOUDPLAY_PS5
    return true;
#else
    return false;
#endif
}

void change(cloudplay::CloudSettings &settings, int focused, int direction) {
    using namespace cloudplay;
    if (focused == 0) {
        settings.service = settings.service == CloudService::PS5Cloud ?
            CloudService::PSNow : CloudService::PS5Cloud;
    } else if (focused == 1) {
        constexpr int resolutions[] = {720, 1080, 1440, 2160};
        int at = 1;
        for (int i = 0; i < 4; ++i)
            if (resolutions[i] == settings.selected().resolution) at = i;
        settings.selected().resolution = resolutions[(at + direction + 4) % 4];
    } else if (focused == 2) {
        size_t count = 0;
        const char *const *languages = supported_cloud_languages(count);
        size_t at = 0;
        for (size_t i = 0; i < count; ++i)
            if (settings.selected().game_language == languages[i]) at = i + 1;
        at = (at + count + 1 + direction) % (count + 1);
        settings.selected().game_language = at ? languages[at - 1] : "";
    } else if (focused == 3) {
        const auto choices = cloud_datacenter_names(settings.selected());
        size_t at = 0;
        for (size_t i = 0; i < choices.size(); ++i)
            if (choices[i] == settings.selected().datacenter) at = i;
        settings.selected().datacenter = choices[
            (at + choices.size() + direction) % choices.size()];
    } else if (focused == 4) {
        int mbps = settings.selected().bitrate_kbps / 1000 + direction;
        if (mbps < 2) mbps = 2;
        if (mbps > 200) mbps = 200;
        settings.selected().bitrate_kbps = mbps * 1000;
    } else if (focused == 5) {
        int mode = static_cast<int>(settings.selected().video_recovery_mode);
        mode = (mode + direction + 3) % 3;
        settings.selected().video_recovery_mode =
            static_cast<VideoRecoveryMode>(mode);
    } else if (focused == 6) {
        settings.show_stream_stats = !settings.show_stream_stats;
    }
}

#if CLOUDPLAY_PS5
cloudplay::CloudUiModel settings_ui_model(
    const cloudplay::CloudSettings &settings,
    const cloudplay::CloudSettings &defaults,
    const cloudplay::CredentialHttpServer &server, int focused,
    bool catalog_enabled) {
    using namespace cloudplay;
    CloudUiModel model;
    model.screen = CloudUiScreen::Settings;
    const ServiceSettings &default_service = settings.service == CloudService::PS5Cloud ?
        defaults.ps5_cloud : defaults.ps_now;
    const auto recovery_label = [](VideoRecoveryMode mode) {
        switch (mode) {
        case VideoRecoveryMode::Simple: return "Simple";
        case VideoRecoveryMode::IDR: return "IDR";
        case VideoRecoveryMode::Hybrid: return "Hybrid";
        }
        return "IDR";
    };
    const std::string values[] = {
        settings.service == CloudService::PS5Cloud ? "PS5 Cloud" : "PS Now",
        resolution_label(settings.selected().resolution),
        settings.selected().game_language.empty() ? "Auto" :
        settings.selected().game_language,
        settings.selected().datacenter,
        std::to_string(settings.selected().bitrate_kbps / 1000) + " Mbps",
        recovery_label(settings.selected().video_recovery_mode),
        settings.show_stream_stats ? "On" : "Off"
    };
    const std::string defaults_text[] = {
        "", "Default " + resolution_label(default_service.resolution),
        "Default " + (default_service.game_language.empty() ? std::string("Auto") :
                       default_service.game_language),
        "Default " + default_service.datacenter,
        "Default " + std::to_string(default_service.bitrate_kbps / 1000) + " Mbps",
        "Default " + std::string(recovery_label(default_service.video_recovery_mode)),
        std::string("Default ") + (defaults.show_stream_stats ? "On" : "Off")
    };
    const char *labels[] = {"Cloud service", "Resolution", "Game language",
                            "Datacenter", "Bitrate", "Video recovery",
                            "Show stream statistics"};
    for (int index = 0; index < settings_row_count; ++index) {
        CloudUiRow row{labels[index], values[index], defaults_text[index], focused == index};
        if (index == 4) {
            const int mbps = settings.selected().bitrate_kbps / 1000;
            row.slider_percent = std::clamp((mbps - 2) * 100 / 198, 0, 100);
        }
        model.rows.push_back(std::move(row));
    }
    model.status = server.status();
    model.controls = "X - Change value";
    model.catalog_enabled = catalog_enabled;
    return model;
}

cloudplay::CloudUiModel tokens_ui_model(
    const cloudplay::CredentialHttpServer &server, bool catalog_enabled) {
    using namespace cloudplay;
    CloudUiModel model;
    model.screen = CloudUiScreen::Tokens;
    const Credentials &credentials = server.credentials();
    const PsnTokenExpiryState expiry_state = psn_token_expiry_state(credentials);
    std::string expiry = credentials.psn_auth_token_expiry.empty() ? "Not set" :
        credentials.psn_auth_token_expiry;
    if (expiry_state == PsnTokenExpiryState::Expired) expiry += " · Expired";
    else if (expiry_state == PsnTokenExpiryState::Invalid) expiry += " · Invalid expiry";
    const char *labels[] = {"NPSSO", "PSN access token", "PSN refresh token",
                            "Access token expiry", "PSN account ID"};
    const std::string values[] = {
        credentials.npsso.empty() ? "Not set" : "••••••••••••••••••••••••••••••••",
        credentials.psn_access_token.empty() ? "Not set" :
            "••••••••••••••••••••••••••••••••",
        credentials.psn_refresh_token.empty() ? "Not set" :
            "••••••••••••••••••••••••••••••••",
        expiry,
        credentials.psn_account_id.empty() ? "Not set" :
            "••••••••••••••••••••••••••••••••"
    };
    for (int index = 0; index < 5; ++index) {
        CloudUiRow row;
        row.label = labels[index];
        row.value = values[index];
        model.rows.push_back(std::move(row));
    }
    model.status = server.status();
    model.controls.clear();
    model.catalog_enabled = catalog_enabled;
    return model;
}

cloudplay::CloudUiModel resolves_ui_model(size_t scroll, bool catalog_enabled) {
    using namespace cloudplay;
    CloudUiModel model;
    model.screen = CloudUiScreen::Resolves;
    const auto rules = psn_resolve_rules();
    const size_t first = rules.size() > 5 ? std::min(scroll, rules.size() - 5) : 0;
    for (size_t slot = 0; slot < 5 && first + slot < rules.size(); ++slot) {
        CloudUiRow row;
        row.label = rules[first + slot].first;
        row.value = rules[first + slot].second;
        model.rows.push_back(std::move(row));
    }
    model.status = std::to_string(rules.size()) + " DNS resolves active";
    model.controls.clear();
    model.catalog_enabled = catalog_enabled;
    return model;
}

cloudplay::CloudUiModel catalog_ui_model(
    const CatalogView &view, const cloudplay::CredentialHttpServer &server,
    const cloudplay::CloudCoverDownloader &cover_downloader,
    bool catalog_loading) {
    using namespace cloudplay;
    CloudUiModel model;
    model.screen = CloudUiScreen::Catalog;
    static const char *category_labels[] = {"Recently played", "All games", "Playable",
                                            "Owned", "Streamable", "Purchasable"};
    static const char *platform_labels[] = {"All systems", "PS5", "PS4", "PS3"};
    static const char *service_labels[] = {"All services", "PS5 Cloud", "PS Now"};
    static const char *sort_labels[] = {"Playable first", "A–Z", "Z–A"};
    model.filters = {view.search.empty() ? "Search games" : view.search,
                     category_labels[view.category], platform_labels[view.platform],
                     service_labels[view.service],
                     view.category == 0 ? "Recent first" : sort_labels[view.sort]};
    model.catalog_loading = catalog_loading;
    model.catalog_controls_enabled = !catalog_loading && !view.games.empty();
    model.focused_filter = model.catalog_controls_enabled && view.focus < 5 ? view.focus : -1;
    model.count = std::to_string(view.visible.size()) + " / " +
                  std::to_string(view.games.size());
    const size_t first_row = view.selected / catalog_columns > 1 ?
        view.selected / catalog_columns - 1 : 0;
    const size_t first = first_row * catalog_columns;
    for (size_t slot = 0; slot < catalog_slots && first + slot < view.visible.size(); ++slot) {
        const CloudGame &game = view.games[view.visible[first + slot]];
        const std::string id = game.product_id.empty() ? game.identifier : game.product_id;
        CloudUiCard card;
        card.title = display_game_name(game.name);
        card.platform = game.platform;
        card.availability = game.is_owned ? (game.is_playable ? "Owned · Cloud" : "Owned") :
                            (game.is_playable ? "Cloud" : "Store");
        if (!id.empty()) {
            const std::string key = cloud_cover_key(id);
            card.cover_image = cover_downloader.image(key);
            if (card.cover_image) card.cover_source = "memory-cover://" + key;
        }
        card.selected = view.focus == 5 && first + slot == view.selected;
        model.cards.push_back(std::move(card));
    }
    model.choosing_variant = view.choosing_variant && !view.visible.empty();
    model.showing_purchase = view.showing_purchase && !view.visible.empty();
    model.selected_variant = static_cast<int>(view.variant_choice);
    if (model.choosing_variant) {
        const CloudGame &game = view.games[view.visible[view.selected]];
        model.selected_game = display_game_name(game.name);
        for (size_t index = 0; index < game.variants.size() && index < 4; ++index) {
            const CloudVariant &variant = game.variants[index];
            model.variants.push_back(variant.platform +
                (variant.service_type.empty() ? std::string() : " · " + variant.service_type));
        }
    }
    if (model.showing_purchase) {
        const CloudGame &game = view.games[view.visible[view.selected]];
        model.purchase_game = display_game_name(game.name);
        model.purchase_url = view.purchase_url;
    }
    model.status = server.access_state() == CloudAccessState::Expired ?
        server.status() : (view.status.empty() ? server.status() : view.status);
    model.controls = "X - Launch  •  OPTIONS - Refresh";
    model.catalog_empty_message = view.games.empty() ?
        "Catalog could not be loaded. Press OPTIONS to try again." :
        view.category == 0 ? "Games you launch will appear here." :
                             "No games match the selected filters.";
    return model;
}
#endif

} // namespace

int main(int argc, char **argv) {
    using namespace cloudplay;
    debug_event("boot");
#if CLOUDPLAY_PS5
    app_log_allocator("app", "allocator_ready");
#endif
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        debug_event("sdl_runtime_failed");
        return 1;
    }
    debug_event("sdl_runtime_ready");
#if CLOUDPLAY_PS5
    debug_event("paper_ui_construct_start");
    CloudUiView cloud_ui;
    debug_event("paper_ui_construct_done");
    debug_event("paper_ui_initialize_start");
    const bool paper_ui_ready = cloud_ui.initialize();
    debug_event("paper_ui_ready", paper_ui_ready);
    if (!paper_ui_ready) {
        SDL_Quit();
        return 1;
    }
#else
    debug_event("paper_ui_required");
    SDL_Quit();
    return 1;
#endif
#if CLOUDPLAY_PS5
    const char *settings_path = "/download0/settings.json";
#else
    const char *settings_path = "./chiaki-cloud-settings.json";
#endif
    CloudSettingsStore settings_store(settings_path);
    CloudSettings build_defaults;
    CloudSettings settings = build_defaults;
    CloudSettings stored_settings;
    const bool settings_loaded = settings_store.load(stored_settings);
    if (settings_loaded) settings = std::move(stored_settings);
    debug_event("settings_loaded", settings_loaded);
#if CLOUDPLAY_PS5
    psn_set_resolve_rules(settings.dns_resolves);
#endif
    const char *catalog_path =
#if CLOUDPLAY_PS5
        "/download0/catalog.json";
#else
        "./chiaki-cloud-catalog.json";
#endif
    CloudCatalogStore catalog_store(catalog_path);
    const char *recent_path =
#if CLOUDPLAY_PS5
        "/download0/recent.json";
#else
        "./recent.json";
#endif
    RecentLaunchStore recent_store(recent_path);
    debug_event("cloud_access_ready", cloud_access_ready(settings));
    CredentialHttpServer server{settings_store, settings};
    debug_event("credentials_loaded", !server.credentials().npsso.empty());
#if CLOUDPLAY_PS5
    debug_event("psn_auth_present", !server.credentials().psn_access_token.empty());
    debug_event("psn_refresh_present", !server.credentials().psn_refresh_token.empty());
    debug_event("psn_expiry_present", !server.credentials().psn_auth_token_expiry.empty());
    debug_event("psn_account_present", !server.credentials().psn_account_id.empty());
#endif
    CatalogView catalog;
    catalog.focus = 5;
    debug_event("recent_loaded", recent_store.load(catalog.recent_launches));
    const bool catalog_loaded = catalog_store.load(catalog.games, &catalog.context);
    debug_event("catalog_loaded", catalog_loaded);
    catalog.category = has_recent_games(catalog) ? 0 : 1;
    filter_catalog(catalog);
    (void)argc;
    (void)argv;
    debug_event("http_server", server.start(9055));
    debug_event("http_error_step", server.error_step());
    debug_event("http_errno", server.error_number());
#if CLOUDPLAY_PS5
    CloudCatalogClient online_catalog(catalog_store);
    const bool streaming_backend_ready = chiaki_lib_init() == CHIAKI_ERR_SUCCESS;
    debug_event("streaming_backend_ready", streaming_backend_ready);
    auto stream = std::make_unique<CloudStreamClient>();
    auto cover_downloader = std::make_unique<CloudCoverDownloader>();
    bool pending_catalog_refresh = false;
    bool catalog_opened_once = false;
    bool default_catalog_mode_pending = catalog.games.empty();
    unsigned loaded_online_catalog_generation = 0;
    std::string last_online_status;
    const bool pad_ready = radio_input_init();
    const bool ime_ready = pad_ready && radio_ime_init();
    debug_event("pad_ready", pad_ready);
    debug_event("ime_ready", ime_ready);
#endif
    enum class View {
        Catalog,
        Settings,
        Tokens,
#if CLOUDPLAY_PS5
        Resolves,
#endif
        Stream
    };
    const CloudAccessState startup_access_state = cloud_access_state(settings);
    View view = startup_access_state != CloudAccessState::Missing ?
        View::Catalog : View::Settings;
    int focused = 0;
#if CLOUDPLAY_PS5
    size_t resolve_scroll = 0;
    int catalog_repeat_direction = 0;
    uint64_t catalog_repeat_at = 0;
#endif
    bool running = true;
    uint64_t last_draw = 0;
    unsigned loaded_settings_generation = 0;
    CloudAccessState observed_access_state = startup_access_state;
    bool first_frame = true;
#if CLOUDPLAY_PS5
    bool ui_video_active = true;
    auto close_ui_video = [&]() {
        cloud_ui.shutdown();
        ui_video_active = false;
        debug_event("ui_video_released_for_agc");
    };
    auto open_ui_video = [&]() {
        if (ui_video_active) return true;
        SDL_Delay(100);
        const bool restored = cloud_ui.initialize();
        debug_event("paper_ui_restored", restored);
        if (!restored)
            return false;
        ui_video_active = true;
        last_draw = 0;
        return true;
    };
    auto start_native_stream = [&]() {
        pending_catalog_refresh = false;
        close_ui_video();
        SDL_Delay(100);
        if (!stream->start(catalog.launch_game, catalog.launch_variant, settings,
                           catalog.context, server.credentials().npsso)) {
            debug_event("stream_start_failed");
            catalog.status = stream->status();
            (void)open_ui_video();
            return false;
        }
        record_recent_launch(catalog.recent_launches, catalog.launch_game,
                             catalog.launch_variant);
        debug_event("recent_saved", recent_store.save(catalog.recent_launches));
        if (catalog.category == 0) {
            catalog.selected = 0;
            filter_catalog(catalog);
        }
        view = View::Stream;
        debug_event("stream_started_native");
        return true;
    };
    auto stop_native_stream = [&]() {
        stream->stop();
        catalog.status = stream->status();
        SDL_Delay(100);
        if (!open_ui_video()) running = false;
        view = View::Catalog;
        debug_event("stream_stopped_native");
    };
#endif
    auto catalog_input_enabled = [&]() {
#if CLOUDPLAY_PS5
        return cloud_access_state(settings) != CloudAccessState::Missing &&
            !pending_catalog_refresh &&
            !online_catalog.busy() && !catalog.games.empty();
#else
        return !catalog.games.empty();
#endif
    };
    auto catalog_tab_enabled = [&]() {
        return cloud_access_state(settings) != CloudAccessState::Missing;
    };
    auto navigate_page = [&](int direction) {
#if CLOUDPLAY_PS5
        constexpr int last_page = static_cast<int>(View::Resolves);
#else
        constexpr int last_page = static_cast<int>(View::Tokens);
#endif
        const int target = static_cast<int>(view) + direction;
        if (target < static_cast<int>(View::Catalog) || target > last_page)
            return;
        const View destination = static_cast<View>(target);
        if (destination == View::Catalog && !catalog_tab_enabled())
            return;
        view = destination;
    };
    while (running) {
        SDL_Event event{};
        while (
#if CLOUDPLAY_PS5
               view != View::Stream &&
#endif
               SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN) {
                const auto key = event.key.keysym.sym;
                if (key == SDLK_ESCAPE) {
                    if (view == View::Stream) {
#if CLOUDPLAY_PS5
                        stop_native_stream();
#endif
                    }
                    else if (view == View::Catalog && catalog.choosing_variant)
                        catalog.choosing_variant = false;
                    else if (view == View::Catalog && catalog.showing_purchase)
                        catalog.showing_purchase = false;
                    else if (view == View::Catalog && catalog.focus == 5)
                        catalog.focus = 0;
                    else if (view == View::Catalog)
                        reset_focused_catalog_filter(catalog);
                    else if (view != View::Settings)
                        navigate_page(-1);
                }
                if (key == SDLK_PAGEUP)
                    navigate_page(-1);
                if (key == SDLK_PAGEDOWN)
                    navigate_page(1);
                if (view == View::Settings) {
                    if (key == SDLK_UP)
                        focused = (focused + settings_row_count - 1) % settings_row_count;
                    if (key == SDLK_DOWN)
                        focused = (focused + 1) % settings_row_count;
                    if (key == SDLK_LEFT || key == SDLK_RIGHT) {
                        change(settings, focused, key == SDLK_LEFT ? -1 : 1);
                        debug_event("settings_saved", settings_store.save(settings));
                    }
                }
#if CLOUDPLAY_PS5
                else if (view == View::Resolves) {
                    const size_t count = psn_resolve_rules().size();
                    if (key == SDLK_UP && resolve_scroll > 0) --resolve_scroll;
                    if (key == SDLK_DOWN && resolve_scroll + 5 < count) ++resolve_scroll;
                }
#endif
                else if (view == View::Catalog) {
                    const bool enabled = catalog_input_enabled();
                    if (enabled && key == SDLK_LEFT) move_catalog(catalog, -1);
                    if (enabled && key == SDLK_RIGHT) move_catalog(catalog, 1);
                    if (enabled && key == SDLK_UP)
                        move_catalog(catalog, -static_cast<int>(catalog_columns));
                    if (enabled && key == SDLK_DOWN)
                        move_catalog(catalog, static_cast<int>(catalog_columns));
                    if (enabled && key == SDLK_RETURN && !cloud_access_ready(settings)) {
                        catalog.status = server.status();
                        debug_event("stream_blocked_settings_missing");
                    } else if (enabled && key == SDLK_RETURN && activate_catalog(catalog
#if CLOUDPLAY_PS5
                            , server.credentials().npsso
#endif
                            )) {
#if CLOUDPLAY_PS5
                        (void)start_native_stream();
#endif
                    }
                    if (enabled && key == SDLK_BACKSPACE && catalog.focus == 0 &&
                        !catalog.search.empty()) {
                        catalog.search.pop_back();
                        filter_catalog(catalog);
                    }
                }
            }
            if (view == View::Catalog && event.type == SDL_TEXTINPUT && catalog.focus == 0 &&
                catalog_input_enabled()) {
                catalog.search += event.text.text;
                filter_catalog(catalog);
            }
        }
#if CLOUDPLAY_PS5
        if (pad_ready) {
            radio_input_poll();
            if (ime_ready) radio_ime_poll();
            const bool handling_stream_input = view == View::Stream;
            if (handling_stream_input) {
                radio_input_pad_state_t pad{};
                if (radio_input_pad_state(&pad)) stream->send_pad(pad);
                if (radio_input_pressed(RADIO_INPUT_OPTIONS) &&
                    radio_input_pressed(RADIO_INPUT_CIRCLE)) {
                    stop_native_stream();
                }
            }
            radio_input_event_t input{};
            while (radio_input_next(&input)) {
                // Discard the whole input batch collected for the stream. This also
                // prevents OPTIONS from the OPTIONS + O stop chord from becoming a
                // catalog refresh after stop_native_stream() changes the view.
                if (handling_stream_input || view == View::Stream) continue;
                if (!input.pressed) continue;
                if (input.key == RADIO_INPUT_CIRCLE) {
                    if (view == View::Catalog && catalog.choosing_variant)
                        catalog.choosing_variant = false;
                    else if (view == View::Catalog && catalog.showing_purchase)
                        catalog.showing_purchase = false;
                    else if (view == View::Catalog && catalog.focus == 5)
                        catalog.focus = 0;
                    else if (view == View::Catalog)
                        reset_focused_catalog_filter(catalog);
                }
                const bool catalog_modal = view == View::Catalog &&
                    (catalog.choosing_variant || catalog.showing_purchase);
                if (input.key == RADIO_INPUT_L1 && !catalog_modal)
                    navigate_page(-1);
                if (input.key == RADIO_INPUT_R1 && !catalog_modal)
                    navigate_page(1);
                if (view == View::Settings) {
                    if (input.key == RADIO_INPUT_UP)
                        focused = (focused + settings_row_count - 1) % settings_row_count;
                    if (input.key == RADIO_INPUT_DOWN)
                        focused = (focused + 1) % settings_row_count;
                    if (input.key == RADIO_INPUT_LEFT || input.key == RADIO_INPUT_RIGHT ||
                        input.key == RADIO_INPUT_CROSS) {
                        change(settings, focused, input.key == RADIO_INPUT_LEFT ? -1 : 1);
                        debug_event("settings_saved", settings_store.save(settings));
                    }
                } else if (view == View::Resolves) {
                    const size_t count = psn_resolve_rules().size();
                    if (input.key == RADIO_INPUT_UP && resolve_scroll > 0) --resolve_scroll;
                    if (input.key == RADIO_INPUT_DOWN && resolve_scroll + 5 < count)
                        ++resolve_scroll;
                } else if (view == View::Catalog) {
                    const bool enabled = catalog_input_enabled();
                    if (enabled && input.key == RADIO_INPUT_LEFT) move_catalog(catalog, -1);
                    if (enabled && input.key == RADIO_INPUT_RIGHT) move_catalog(catalog, 1);
                    if (enabled && input.key == RADIO_INPUT_UP)
                        move_catalog(catalog, -static_cast<int>(catalog_columns));
                    if (enabled && input.key == RADIO_INPUT_DOWN)
                        move_catalog(catalog, static_cast<int>(catalog_columns));
                    if (enabled && input.key == RADIO_INPUT_CROSS) {
                        if (!cloud_access_ready(settings)) {
                            catalog.status = server.status();
                            debug_event("stream_blocked_settings_missing");
                        } else if (online_catalog.busy()) {
                            catalog.status = "Wait for catalog refresh to finish";
                            debug_event("stream_blocked_catalog_busy");
                        } else if (activate_catalog(catalog,
                                                    server.credentials().npsso)) {
                            (void)start_native_stream();
                        }
                    }
                    if (input.key == RADIO_INPUT_OPTIONS && !catalog.choosing_variant &&
                        !catalog.showing_purchase &&
                        !pending_catalog_refresh && !online_catalog.busy()) {
                        if (!cloud_access_ready(settings)) {
                            pending_catalog_refresh = false;
                            catalog.status = server.status();
                            debug_event("catalog_refresh_blocked_settings_missing");
                        } else {
                            pending_catalog_refresh = true;
                            debug_event("catalog_refresh_requested");
                            catalog.status = "Refreshing catalog via HTTPS";
                        }
                    }
                }
            }
            const int held_catalog_direction =
                radio_input_pressed(RADIO_INPUT_DOWN) ? 1 :
                radio_input_pressed(RADIO_INPUT_UP) ? -1 : 0;
            const bool repeat_catalog_navigation = !handling_stream_input &&
                view == View::Catalog &&
                catalog.focus == 5 && !catalog.choosing_variant && !catalog.showing_purchase &&
                catalog_input_enabled() &&
                held_catalog_direction != 0;
            const uint64_t repeat_now = SDL_GetTicks64();
            if (!repeat_catalog_navigation) {
                catalog_repeat_direction = 0;
                catalog_repeat_at = 0;
            } else if (catalog_repeat_direction != held_catalog_direction) {
                catalog_repeat_direction = held_catalog_direction;
                catalog_repeat_at = repeat_now + catalog_repeat_delay_ms;
            } else if (repeat_now >= catalog_repeat_at) {
                move_catalog(catalog, catalog_repeat_direction *
                             static_cast<int>(catalog_columns));
                catalog_repeat_at = repeat_now + catalog_repeat_interval_ms;
            }
        }
#endif
#if CLOUDPLAY_PS5
        if (!catalog_opened_once) {
            catalog_opened_once = true;
            if (cloud_access_state(settings) == CloudAccessState::Missing) {
                pending_catalog_refresh = false;
                catalog.status = server.status();
                debug_event("catalog_auto_blocked_settings_missing");
            } else if (catalog.games.empty()) {
                pending_catalog_refresh = true;
                catalog.status = "Loading Cloud Play catalog via HTTPS";
                debug_event("catalog_auto_requested");
            } else {
                debug_event("catalog_cache_ready", static_cast<int>(catalog.games.size()));
            }
        }
#endif
        server.poll();
#if CLOUDPLAY_PS5
        if (loaded_settings_generation != server.settings_generation()) {
            loaded_settings_generation = server.settings_generation();
            const CloudAccessState current_access_state = cloud_access_state(settings);
            const bool access_became_available =
                observed_access_state == CloudAccessState::Missing &&
                current_access_state != CloudAccessState::Missing;
            observed_access_state = current_access_state;
            psn_set_resolve_rules(settings.dns_resolves);
            resolve_scroll = 0;
            catalog.status.clear();
            debug_event("settings_post_applied",
                        static_cast<int>(settings.dns_resolves.size()));
            if (access_became_available && view != View::Stream) {
                view = View::Catalog;
                debug_event("catalog_opened_after_setup");
            }
            if (access_became_available || catalog.games.empty()) {
                pending_catalog_refresh = true;
                catalog.status = "Loading Cloud Play catalog via HTTPS";
                debug_event("catalog_auto_after_settings_post");
            }
        }
#endif
#if CLOUDPLAY_PS5
        if (view == View::Stream && !stream->active()) {
            stop_native_stream();
        }
        std::string datacenter_service, datacenter_json;
        if (stream->take_datacenters(datacenter_service, datacenter_json)) {
            ServiceSettings &service = datacenter_service == "pscloud" ?
                settings.ps5_cloud : settings.ps_now;
            service.datacenters_json = datacenter_json;
            debug_event("datacenters_saved", settings_store.save(settings));
        }
        online_catalog.poll();
        cover_downloader->poll();
        if (pending_catalog_refresh && !online_catalog.busy()) {
            pending_catalog_refresh = false;
            if (!streaming_backend_ready) {
                catalog.status = "Streaming backend initialization failed";
                debug_event("catalog_refresh_skipped", 1);
            } else if (!cloud_access_ready(settings)) {
                catalog.status = server.status();
                debug_event("catalog_refresh_skipped", 2);
            } else {
                debug_event("catalog_refresh_launch");
                const bool started = online_catalog.refresh(server.credentials().npsso,
                    settings.selected().game_language.empty() ? "en-US" :
                        settings.selected().game_language);
                debug_event("catalog_refresh_started", started);
            }
        }
        if (last_online_status != online_catalog.status()) {
            last_online_status = online_catalog.status();
            if (!last_online_status.empty()) catalog.status = last_online_status;
            debug_event("catalog_status_changed", online_catalog.generation());
        }
        if (loaded_online_catalog_generation != online_catalog.generation()) {
            loaded_online_catalog_generation = online_catalog.generation();
            debug_event("catalog_reload", catalog_store.load(catalog.games, &catalog.context));
            if (default_catalog_mode_pending && !catalog.games.empty()) {
                catalog.category = has_recent_games(catalog) ? 0 : 1;
                default_catalog_mode_pending = false;
            }
            filter_catalog(catalog);
            cover_downloader->reset();
        }
#endif
#if CLOUDPLAY_PS5
        if (view == View::Catalog)
            cover_downloader->schedule(catalog.games, catalog.visible, catalog.selected);
#endif
        const uint64_t now = SDL_GetTicks64();
        if (
#if CLOUDPLAY_PS5
            view != View::Stream && ui_video_active &&
#endif
            now - last_draw >= 33) {
#if CLOUDPLAY_PS5
            const bool catalog_enabled = catalog_tab_enabled();
            if (view == View::Settings)
                cloud_ui.present(settings_ui_model(settings, build_defaults, server, focused,
                                                   catalog_enabled));
            else if (view == View::Tokens)
                cloud_ui.present(tokens_ui_model(server, catalog_enabled));
            else if (view == View::Resolves)
                cloud_ui.present(resolves_ui_model(resolve_scroll, catalog_enabled));
            else if (view == View::Catalog)
                cloud_ui.present(catalog_ui_model(
                    catalog, server, *cover_downloader,
                    pending_catalog_refresh || online_catalog.busy()));
#endif
            last_draw = now;
            if (first_frame) { debug_event("first_frame"); first_frame = false; }
        }
        SDL_Delay(2);
    }
    server.stop();
    debug_event("shutdown");
#if CLOUDPLAY_PS5
    stream->stop();
    stream.reset();
    cover_downloader.reset();
    if (ime_ready) radio_ime_shutdown();
    if (pad_ready) radio_input_shutdown();
    cloud_ui.shutdown();
#endif
    SDL_Quit();
    return 0;
}
