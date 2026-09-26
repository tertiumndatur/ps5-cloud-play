// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if CLOUDPLAY_PS5

#include "cloud_cover_image.hpp"

#include <memory>
#include <string>
#include <vector>

namespace cloudplay {

enum class CloudUiScreen { Settings, Tokens, Resolves, Catalog };

struct CloudUiRow {
    std::string label;
    std::string value;
    std::string default_value;
    bool focused = false;
    int slider_percent = -1;
};

struct CloudUiCard {
    std::string title;
    std::string platform;
    std::string availability;
    std::string cover_source;
    std::shared_ptr<const CloudCoverImage> cover_image;
    bool selected = false;
};

struct CloudUiModel {
    CloudUiScreen screen = CloudUiScreen::Settings;
    std::vector<CloudUiRow> rows;
    std::vector<CloudUiCard> cards;
    std::vector<std::string> filters;
    std::vector<std::string> variants;
    std::string count;
    std::string status;
    std::string controls;
    std::string selected_game;
    std::string purchase_game;
    std::string purchase_url;
    int focused_filter = -1;
    int selected_variant = -1;
    bool choosing_variant = false;
    bool showing_purchase = false;
    bool catalog_enabled = true;
    bool catalog_loading = false;
    bool catalog_controls_enabled = false;
    std::string catalog_empty_message;
};

class CloudUiView {
public:
    CloudUiView();
    ~CloudUiView();
    CloudUiView(const CloudUiView &) = delete;
    CloudUiView &operator=(const CloudUiView &) = delete;

    bool initialize();
    void shutdown();
    bool ready() const;
    void present(const CloudUiModel &model);

private:
    struct Impl;
    Impl *impl_ = nullptr;
};

} // namespace cloudplay

#endif
