// SPDX-License-Identifier: GPL-3.0-or-later
#include "cloud_ui_view.hpp"

#if CLOUDPLAY_PS5

#include "app_log.hpp"
#include "core/tween.hpp"
#include "gfx/backdrop_spec.hpp"
#include "gfx/renderer.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/system.hpp"
#include "qr/qrcodegen.h"
#include "storage_io.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

#include <GL/glcorearb.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>

extern "C" void native_agc_mark_initialized(void);

namespace cloudplay {
namespace {

using hui::gfx::Color;
using hui::gfx::Rect;

const Color kNightTop = Color::rgb(0x0c1330);
const Color kNightBottom = Color::rgb(0x1d1240);
const Color kPaper = Color::rgb(0xf4f1ea);
const Color kPaperShade = Color::rgb(0xe6e1d6);
const Color kPrint = Color::rgb(0xfcfbf7);
const Color kInk = Color::rgb(0x1b1d2b);
const Color kInkMuted = Color::rgb(0x6b6f82);
const Color kOnDark = Color::rgb(0xf5f3ff);
const Color kGold = Color::rgb(0xffd166);
const Color kBlack = Color::rgb(0x000000);
const Color kWhite = Color::rgb(0xffffff);
const Color kClear = Color::rgb(0x000000, 0.0f);

constexpr float kMargin = 96.0f;
constexpr float kRight = hui::gfx::kVirtualWidth - kMargin;
constexpr float kCardW = 268.0f;
constexpr float kCardH = 330.0f;
constexpr float kCardGap = 24.0f;
constexpr float kGridTop = 250.0f;
constexpr float kCardRadius = 14.0f;
constexpr float kCoverInset = 12.0f;
constexpr int kColumns = 6;

Rect card_rect(int slot) {
    return {kMargin + static_cast<float>(slot % kColumns) * (kCardW + kCardGap),
            kGridTop + static_cast<float>(slot / kColumns) * (kCardH + kCardGap),
            kCardW, kCardH};
}

Rect scaled(const Rect &rect, float scale) {
    return {rect.cx() - rect.w * scale * 0.5f, rect.cy() - rect.h * scale * 0.5f,
            rect.w * scale, rect.h * scale};
}

int screen_index(CloudUiScreen screen) {
    switch (screen) {
    case CloudUiScreen::Catalog: return 0;
    case CloudUiScreen::Settings: return 1;
    case CloudUiScreen::Tokens: return 2;
    case CloudUiScreen::Resolves: return 3;
    }
    return 0;
}

} // namespace

struct CloudUiView::Impl {
    struct CoverTexture {
        GLuint name = 0;
        const CloudCoverImage *source = nullptr;
        std::uint64_t seen_frame = 0;
    };

    hui::ps5::Display display;
    hui::gfx::Renderer renderer;
    hui::gfx::Font regular_font;
    hui::gfx::Font semibold_font;
    hui::gfx::Font display_font;
    hui::ui::Fonts fonts;
    hui::gfx::DrawList scene;
    hui::gfx::DrawList overlay;
    hui::ui::SpringRect focus_ring;
    std::unordered_map<std::string, CoverTexture> covers;
    CloudUiScreen previous_screen = CloudUiScreen::Settings;
    std::string catalog_signature;
    std::int64_t previous_time_us = 0;
    float clock = 0.0f;
    float screen_age = 0.0f;
    float catalog_age = 0.0f;
    float selected_age = 0.0f;
    std::string selected_signature;
    std::string purchase_qr_url;
    std::array<std::uint8_t, qrcodegen_BUFFER_LEN_MAX> purchase_qr{};
    int purchase_qr_size = 0;
    std::uint64_t frame = 0;
    bool focus_ring_ready = false;
    bool initialized = false;
    bool splash_hidden = false;

    bool load_font(const char *path, hui::gfx::Font &font, hui::ui::FontRef &ref) {
        app_log_text("paper", "font_read_start", std::string(" path=") + path);
        std::string bytes;
        if (!storage_read_file(path, 4u * 1024u * 1024u, bytes) || !font.load(bytes)) {
            hui::sys::log("[CloudPlay] failed to load UI font %s", path);
            return false;
        }
        app_log("paper", "font_parsed", static_cast<int>(bytes.size()));
        ref = {&font, renderer.batch().create_font_texture(font)};
        app_log("paper", "font_texture", static_cast<int>(ref.texture));
        return ref.texture != 0;
    }

    GLuint cover_texture(const CloudUiCard &card) {
        if (!card.cover_image || card.cover_source.empty() || card.cover_image->width <= 0 ||
            card.cover_image->height <= 0 || card.cover_image->rgba.empty())
            return 0;
        CoverTexture &entry = covers[card.cover_source];
        if (entry.name == 0 || entry.source != card.cover_image.get()) {
            if (entry.name != 0)
                glDeleteTextures(1, &entry.name);
            entry.name = renderer.batch().create_texture(card.cover_image->width,
                                                         card.cover_image->height,
                                                         card.cover_image->rgba.data());
            entry.source = card.cover_image.get();
        }
        entry.seen_frame = frame;
        return entry.name;
    }

    void collect_cover_textures() {
        for (auto it = covers.begin(); it != covers.end();) {
            if (frame > it->second.seen_frame + 180) {
                if (it->second.name != 0)
                    glDeleteTextures(1, &it->second.name);
                it = covers.erase(it);
            } else {
                ++it;
            }
        }
    }

    void draw_page_nav(const CloudUiModel &model) {
        static constexpr const char *labels[] = {"Games", "Settings", "PSN Tokens",
                                                 "DNS Resolves"};
        const int active = screen_index(model.screen);
        hui::ui::text(scene, fonts.display, "Cloud Play", kMargin, 74, 34, kOnDark);
        const float width = 196.0f;
        const float gap = 12.0f;
        const float start = kRight - 4.0f * width - 3.0f * gap;
        for (int index = 0; index < 4; ++index) {
            const Rect tab{start + static_cast<float>(index) * (width + gap), 30, width, 60};
            const bool selected = index == active;
            const bool enabled = index != 0 || model.catalog_enabled || selected;
            if (selected) {
                scene.shadow({tab.x, tab.y + 6, tab.w, tab.h}, 20, 14,
                             kBlack.with_alpha(0.45f));
                scene.gradient_rect(tab, 20, kPaper, kPaperShade);
            } else {
                scene.bordered_rect(tab, 20, kOnDark.with_alpha(enabled ? 0.055f : 0.025f),
                                    1.5f, kOnDark.with_alpha(enabled ? 0.18f : 0.08f));
            }
            hui::ui::text(scene, fonts.semibold, labels[index], tab.cx(), tab.y + 39, 21,
                          selected ? kInk : kOnDark.with_alpha(enabled ? 0.72f : 0.28f),
                          hui::gfx::Align::center);
        }
        const hui::ui::GlyphStyle glyphs = hui::ui::GlyphStyle::dark();
        hui::ui::draw_button(scene, fonts, glyphs, hui::ui::Button::l1, start - 78, 60, 31);
        hui::ui::draw_button(scene, fonts, glyphs, hui::ui::Button::r1, kRight + 24, 60, 31);
    }

    void draw_status(const CloudUiModel &model) {
        if (!model.status.empty()) {
            scene.circle(kMargin + 7, 1024, 5, kGold);
            const std::string status = regular_font.fit(model.status, 20, 1110);
            hui::ui::text(scene, fonts.regular, status, kMargin + 24, 1031, 20,
                          kOnDark.with_alpha(0.64f));
        }
    }

    void draw_hints(const CloudUiModel &model, hui::gfx::DrawList &list) {
        std::array<hui::ui::Hint, 2> hints{};
        int count = 0;
        if (model.screen == CloudUiScreen::Settings) {
            hints[count++] = {hui::ui::Button::cross, "Change value"};
        } else if (model.screen == CloudUiScreen::Catalog && model.choosing_variant) {
            hints[count++] = {hui::ui::Button::cross, "Select"};
            hints[count++] = {hui::ui::Button::circle, "Cancel"};
        } else if (model.screen == CloudUiScreen::Catalog && model.catalog_controls_enabled) {
            hints[count++] = {hui::ui::Button::cross, "Launch"};
            hints[count++] = {hui::ui::Button::options, "Refresh"};
        } else if (model.screen == CloudUiScreen::Catalog && !model.catalog_loading) {
            hints[count++] = {hui::ui::Button::options, "Refresh"};
        }
        if (count == 0)
            return;
        hui::ui::HintLayout layout;
        layout.size = 32;
        layout.text_size = 20;
        layout.cy = 1024;
        layout.item_gap = 34;
        hui::ui::draw_hints(list, fonts, hui::ui::GlyphStyle::dark(), hints.data(), count,
                            kRight, true, layout);
    }

    void draw_sheet_header(const char *eyebrow, const char *title, const char *subtitle) {
        hui::ui::text(scene, fonts.semibold, eyebrow, kMargin, 150, 16, kGold,
                      hui::gfx::Align::left, 3.0f);
        hui::ui::text(scene, fonts.display, title, kMargin, 204, 48, kOnDark);
        hui::ui::text(scene, fonts.regular, subtitle, kMargin, 240, 22,
                      kOnDark.with_alpha(0.58f));
    }

    void draw_rows(const CloudUiModel &model) {
        const bool settings = model.screen == CloudUiScreen::Settings;
        draw_sheet_header(settings ? "CLOUD PLAY" : "ACCOUNT",
                          settings ? "Streaming settings" :
                          model.screen == CloudUiScreen::Tokens ? "PSN tokens" : "DNS resolves",
                          settings ? "Choose separate streaming values for each cloud service." :
                          model.screen == CloudUiScreen::Tokens ?
                              "Credentials are received through the local HTTP setup endpoint." :
                              "Hostnames are resolved by the setup tool and received through HTTP.");

        const Rect sheet{150, 276, 1620, 664};
        scene.shadow({sheet.x, sheet.y + 10, sheet.w, sheet.h}, 28, 34,
                     kBlack.with_alpha(0.42f));
        scene.gradient_rect(sheet, 28, kPaper, kPaperShade);

        const float pad = 34.0f;
        const float gap = settings ? 14.0f : 12.0f;
        const float usable = sheet.h - 2.0f * pad - gap *
            static_cast<float>(model.rows.empty() ? 0 : model.rows.size() - 1);
        const float row_h = model.rows.empty() ? 0.0f : usable / static_cast<float>(model.rows.size());
        for (std::size_t index = 0; index < model.rows.size(); ++index) {
            const CloudUiRow &row = model.rows[index];
            const Rect rect{sheet.x + pad, sheet.y + pad + static_cast<float>(index) * (row_h + gap),
                            sheet.w - 2.0f * pad, row_h};
            if (row.focused) {
                scene.shadow({rect.x, rect.y + 5, rect.w, rect.h}, 18, 14,
                             kBlack.with_alpha(0.28f));
                scene.gradient_rect(rect, 18, kNightTop, kNightBottom);
                scene.bordered_rect(rect.inset(-2), 20, kClear, 3, kGold);
            } else {
                scene.bordered_rect(rect, 18, kPrint.with_alpha(0.55f), 1.5f,
                                    kInk.with_alpha(0.13f));
            }
            const Color primary = row.focused ? kOnDark : kInk;
            const Color secondary = row.focused ? kOnDark.with_alpha(0.56f) : kInkMuted;
            const float middle = rect.cy();
            const bool resolves = model.screen == CloudUiScreen::Resolves;
            const float value_x = rect.x + (resolves ? 720.0f : 390.0f);
            const float label_width = value_x - rect.x - 52.0f;
            const float value_width = rect.x + rect.w - 26.0f - value_x;
            const std::string label = regular_font.fit(row.label, 22, label_width);
            hui::ui::text(scene, fonts.regular, label, rect.x + 26, middle + 8, 22,
                          secondary);
            const std::string value = semibold_font.fit(row.value, 26, value_width);
            hui::ui::text(scene, fonts.semibold, value, value_x, middle + 9, 26, primary);
            if (!row.default_value.empty()) {
                const std::string default_value = regular_font.fit(row.default_value, 20, 300);
                hui::ui::text(scene, fonts.regular, default_value, rect.x + rect.w - 24,
                              middle + 7, 20, secondary, hui::gfx::Align::right);
            }
            if (row.slider_percent >= 0) {
                const Rect bar{rect.x + 390, rect.y + rect.h - 18, 710, 6};
                scene.rounded_rect(bar, 3, secondary.with_alpha(0.25f));
                const float amount = std::clamp(row.slider_percent / 100.0f, 0.0f, 1.0f);
                if (amount > 0.0f)
                    scene.rounded_rect({bar.x, bar.y, std::max(6.0f, bar.w * amount), bar.h}, 3,
                                       kGold);
            }
        }
        draw_status(model);
        draw_hints(model, scene);
    }

    void draw_filter(const std::string &label, const Rect &rect, bool focused, bool search,
                     bool enabled) {
        focused = focused && enabled;
        if (focused) {
            scene.shadow({rect.x, rect.y + 5, rect.w, rect.h}, 18, 13,
                         kBlack.with_alpha(0.42f));
            scene.gradient_rect(rect, 18, kPaper, kPaperShade);
        } else {
            scene.bordered_rect(rect, 18, kOnDark.with_alpha(enabled ? 0.055f : 0.025f), 1.5f,
                                kOnDark.with_alpha(enabled ? 0.18f : 0.08f));
        }
        const std::string fitted = regular_font.fit(label, 21, rect.w - (search ? 72 : 36));
        hui::ui::text(scene, focused ? fonts.semibold : fonts.regular, fitted,
                      rect.x + (search ? 48 : 20), rect.y + 36, 21,
                      focused ? kInk : kOnDark.with_alpha(enabled ? 0.7f : 0.28f));
        if (search) {
            const Color color = focused ? kInkMuted :
                kOnDark.with_alpha(enabled ? 0.42f : 0.18f);
            scene.ring(rect.x + 25, rect.cy() - 2, 8, 2, color);
            scene.line(rect.x + 31, rect.cy() + 4, rect.x + 38, rect.cy() + 11, 2, color);
        }
    }

    void draw_card(const CloudUiCard &card, int slot, bool focused, float in) {
        Rect rect = card_rect(slot);
        rect.y += 24.0f * (1.0f - in);
        const float lift = focused ? 1.065f : 1.0f;
        const Rect paper = scaled(rect, lift);
        scene.push_opacity(in);
        scene.shadow({paper.x, paper.y + (focused ? 18 : 7), paper.w, paper.h}, kCardRadius,
                     focused ? 36 : 16, kBlack.with_alpha(focused ? 0.62f : 0.42f));
        if (focused)
            scene.rotated_rect(paper, kCardRadius, slot % 2 == 0 ? 0.025f : -0.025f,
                               kPaperShade);
        scene.gradient_rect(paper, kCardRadius, kPaper, kPaperShade);

        const Rect art_box{paper.x + kCoverInset, paper.y + kCoverInset,
                           paper.w - 2.0f * kCoverInset, 226.0f * lift};
        scene.rounded_rect(art_box, 7, Color::rgb(0xc8c5be));
        const GLuint texture = cover_texture(card);
        if (texture != 0 && card.cover_image) {
            scene.image(texture, art_box, hui::gfx::kFullUv, kWhite, 6);
        } else {
            const float shimmer = 0.08f + 0.05f * hui::ui::breathe(clock, 1.8f);
            scene.gradient_rect(art_box, 7, kInkMuted.with_alpha(shimmer),
                                kOnDark.with_alpha(shimmer * 0.55f));
        }
        scene.bordered_rect(art_box, 7, kClear, 1, kInk.with_alpha(0.16f));

        const float text_x = paper.x + kCoverInset + 2;
        const float text_right = paper.x + paper.w - kCoverInset - 2;
        const float title_width = text_right - text_x;
        const float measured_title = semibold_font.measure(card.title, 22);
        if (focused && measured_title > title_width) {
            constexpr float pause = 0.7f;
            constexpr float speed = 42.0f;
            const float overflow = measured_title - title_width;
            const float travel = overflow / speed;
            const float cycle = 2.0f * (pause + travel);
            const float phase = std::fmod(selected_age, cycle);
            float offset = 0.0f;
            if (phase > pause && phase < pause + travel)
                offset = -(phase - pause) * speed;
            else if (phase >= pause + travel && phase <= 2.0f * pause + travel)
                offset = -overflow;
            else if (phase > 2.0f * pause + travel)
                offset = -overflow + (phase - 2.0f * pause - travel) * speed;
            scene.push_clip({text_x, paper.y + paper.h - 82, title_width, 36});
            hui::ui::text(scene, fonts.semibold, card.title, text_x + offset,
                          paper.y + paper.h - 55, 22, kInk);
            scene.pop_clip();
        } else {
            const std::string title = semibold_font.fit(card.title, 22, title_width);
            hui::ui::text(scene, fonts.semibold, title, text_x, paper.y + paper.h - 55, 22, kInk);
        }
        hui::ui::text(scene, fonts.semibold, card.platform, text_x, paper.y + paper.h - 22, 19,
                      Color::rgb(0x188fc1));
        const std::string availability = regular_font.fit(card.availability, 18, 154);
        hui::ui::text(scene, fonts.regular, availability, text_right, paper.y + paper.h - 22, 18,
                      kInkMuted, hui::gfx::Align::right);
        scene.pop_opacity();
    }

    void draw_loading(const CloudUiModel &model) {
        const Rect note{620, 365, 680, 300};
        scene.shadow({note.x, note.y + 12, note.w, note.h}, 28, 38, kBlack.with_alpha(0.5f));
        scene.rotated_rect(note, 28, -0.012f, kPaperShade);
        scene.gradient_rect(note, 28, kPaper, kPaperShade);
        const float phase = std::fmod(clock * 1.4f, 1.0f) * 6.2831853f;
        for (int index = 0; index < 8; ++index) {
            const float angle = phase + static_cast<float>(index) * 0.78539816f;
            const float alpha = 0.18f + 0.1f * static_cast<float>(index);
            scene.circle(note.cx() + std::cos(angle) * 42, note.y + 86 + std::sin(angle) * 42,
                         7, kInk.with_alpha(alpha));
        }
        hui::ui::text(scene, fonts.display, "Loading catalog", note.cx(), note.y + 183, 38, kInk,
                      hui::gfx::Align::center);
        const std::string status = regular_font.fit(model.status, 21, note.w - 80);
        hui::ui::text(scene, fonts.regular, status, note.cx(), note.y + 226, 21, kInkMuted,
                      hui::gfx::Align::center);
    }

    void draw_empty(const CloudUiModel &model) {
        const Rect note{580, 380, 760, 250};
        scene.shadow({note.x, note.y + 12, note.w, note.h}, 28, 36, kBlack.with_alpha(0.5f));
        scene.rotated_rect(note, 28, 0.012f, kPaperShade);
        scene.gradient_rect(note, 28, kPaper, kPaperShade);
        hui::ui::text(scene, fonts.display, "Nothing here yet", note.cx(), note.y + 91, 38, kInk,
                      hui::gfx::Align::center);
        hui::ui::paragraph(scene, fonts.regular, model.catalog_empty_message, note.cx(),
                           note.y + 141, 22, note.w - 140, 31, kInkMuted, 3,
                           hui::gfx::Align::center);
    }

    void draw_variant_dialog(const CloudUiModel &model) {
        overlay.rounded_rect({0, 0, hui::gfx::kVirtualWidth, hui::gfx::kVirtualHeight}, 0,
                             kBlack.with_alpha(0.58f));
        const float option_count = static_cast<float>(std::max<std::size_t>(2,
                                                                           model.variants.size()));
        const float sheet_w = std::min(1000.0f, 152.0f + option_count * 300.0f);
        const Rect sheet{(hui::gfx::kVirtualWidth - sheet_w) * 0.5f, 288, sheet_w, 504};
        overlay.shadow({sheet.x, sheet.y + 15, sheet.w, sheet.h}, 30, 50,
                       kBlack.with_alpha(0.65f));
        overlay.gradient_rect(sheet, 30, kPaper, kPaperShade);
        hui::ui::text(overlay, fonts.semibold, "CHOOSE VERSION", sheet.x + 56, sheet.y + 70, 17,
                      kInkMuted, hui::gfx::Align::left, 3.0f);
        const std::string title = display_font.fit(model.selected_game, 34, sheet.w - 112);
        hui::ui::text(overlay, fonts.display, title, sheet.x + 56, sheet.y + 122, 34, kInk);
        const float gap = 22.0f;
        const float option_w = (sheet.w - 112.0f - gap *
            static_cast<float>(model.variants.empty() ? 0 : model.variants.size() - 1)) /
            std::max<std::size_t>(1, model.variants.size());
        for (std::size_t index = 0; index < model.variants.size(); ++index) {
            const Rect option{sheet.x + 56 + static_cast<float>(index) * (option_w + gap),
                              sheet.y + 174, option_w, 132};
            const bool selected = static_cast<int>(index) == model.selected_variant;
            if (selected) {
                overlay.shadow({option.x, option.y + 7, option.w, option.h}, 18, 18,
                               kBlack.with_alpha(0.35f));
                overlay.gradient_rect(option, 18, kNightTop, kNightBottom);
                overlay.bordered_rect(option.inset(-3), 21, kClear, 3, kGold);
            } else {
                overlay.bordered_rect(option, 18, kPrint.with_alpha(0.78f), 1.5f,
                                      kInk.with_alpha(0.15f));
            }
            const std::string variant = semibold_font.fit(model.variants[index], 23, option.w - 28);
            hui::ui::text(overlay, fonts.semibold, variant, option.cx(), option.cy() + 8, 23,
                          selected ? kOnDark : kInk, hui::gfx::Align::center);
        }
        std::array<hui::ui::Hint, 2> hints{{{hui::ui::Button::cross, "Select"},
                                             {hui::ui::Button::circle, "Cancel"}}};
        hui::ui::HintLayout layout;
        layout.size = 34;
        layout.text_size = 22;
        layout.cy = sheet.y + sheet.h - 62;
        layout.item_gap = 42;
        hui::ui::draw_hints(overlay, fonts, hui::ui::GlyphStyle::light(), hints.data(),
                            static_cast<int>(hints.size()), sheet.x + 56, false, layout);
    }

    bool prepare_purchase_qr(const std::string &url) {
        if (url == purchase_qr_url)
            return purchase_qr_size > 0;
        purchase_qr_url = url;
        purchase_qr_size = 0;
        if (url.empty()) return false;
        std::array<std::uint8_t, qrcodegen_BUFFER_LEN_MAX> temp{};
        if (!qrcodegen_encodeText(url.c_str(), temp.data(), purchase_qr.data(),
                                  qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                                  qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
            return false;
        purchase_qr_size = qrcodegen_getSize(purchase_qr.data());
        return purchase_qr_size > 0;
    }

    void draw_purchase_dialog(const CloudUiModel &model) {
        overlay.rounded_rect({0, 0, hui::gfx::kVirtualWidth, hui::gfx::kVirtualHeight}, 0,
                             kBlack.with_alpha(0.62f));
        const Rect sheet{570, 112, 780, 856};
        overlay.shadow({sheet.x, sheet.y + 16, sheet.w, sheet.h}, 32, 54,
                       kBlack.with_alpha(0.7f));
        overlay.gradient_rect(sheet, 32, kPaper, kPaperShade);
        hui::ui::text(overlay, fonts.semibold, "PURCHASE", sheet.cx(), sheet.y + 65, 16,
                      kInkMuted, hui::gfx::Align::center, 3.0f);
        hui::ui::text(overlay, fonts.display, "Scan to purchase", sheet.cx(), sheet.y + 122, 38,
                      kInk, hui::gfx::Align::center);
        const std::string title = semibold_font.fit(model.purchase_game, 22, sheet.w - 112);
        hui::ui::text(overlay, fonts.semibold, title, sheet.cx(), sheet.y + 164, 22,
                      kInkMuted, hui::gfx::Align::center);

        if (prepare_purchase_qr(model.purchase_url)) {
            constexpr int quiet = 4;
            const int total = purchase_qr_size + quiet * 2;
            const float module = std::floor(430.0f / static_cast<float>(total));
            const float qr_w = module * static_cast<float>(total);
            const Rect qr{sheet.cx() - qr_w * 0.5f, sheet.y + 205, qr_w, qr_w};
            overlay.shadow({qr.x, qr.y + 8, qr.w, qr.h}, 12, 18, kBlack.with_alpha(0.2f));
            overlay.rounded_rect(qr, 12, kWhite);
            const float origin_x = qr.x + module * quiet;
            const float origin_y = qr.y + module * quiet;
            for (int y = 0; y < purchase_qr_size; ++y) {
                int x = 0;
                while (x < purchase_qr_size) {
                    while (x < purchase_qr_size &&
                           !qrcodegen_getModule(purchase_qr.data(), x, y)) ++x;
                    const int first = x;
                    while (x < purchase_qr_size &&
                           qrcodegen_getModule(purchase_qr.data(), x, y)) ++x;
                    if (first < x)
                        overlay.rounded_rect({origin_x + module * first,
                                              origin_y + module * y,
                                              module * (x - first), module}, 0, kInk);
                }
            }
        } else {
            hui::ui::text(overlay, fonts.regular, "Store link is unavailable", sheet.cx(),
                          sheet.y + 430, 22, kInkMuted, hui::gfx::Align::center);
        }

        std::array<hui::ui::Hint, 1> hints{{{hui::ui::Button::circle, "Close"}}};
        hui::ui::HintLayout layout;
        layout.size = 34;
        layout.text_size = 22;
        layout.cy = sheet.y + sheet.h - 56;
        hui::ui::draw_hints(overlay, fonts, hui::ui::GlyphStyle::light(), hints.data(), 1,
                            sheet.cx() - 66, false, layout);
    }

    void draw_catalog(const CloudUiModel &model, float dt) {
        hui::ui::text(scene, fonts.display, "Library", kMargin, 148, 52, kOnDark);
        hui::ui::text(scene, fonts.regular, model.count, kRight, 143, 23,
                      kOnDark.with_alpha(0.58f), hui::gfx::Align::right);
        const float filter_y = 166.0f;
        const float filter_h = 58.0f;
        const float gap = 12.0f;
        const float search_w = 600.0f;
        const float other_w = (kRight - kMargin - search_w - 4.0f * gap) / 4.0f;
        for (std::size_t index = 0; index < model.filters.size() && index < 5; ++index) {
            const float x = index == 0 ? kMargin :
                kMargin + search_w + gap + static_cast<float>(index - 1) * (other_w + gap);
            draw_filter(model.filters[index], {x, filter_y, index == 0 ? search_w : other_w,
                                               filter_h},
                        model.focused_filter == static_cast<int>(index), index == 0,
                        model.catalog_controls_enabled);
        }

        if (model.catalog_loading) {
            focus_ring_ready = false;
            draw_loading(model);
        } else if (model.cards.empty()) {
            focus_ring_ready = false;
            draw_empty(model);
        } else {
            int focused_slot = -1;
            for (std::size_t index = 0; index < model.cards.size() && index < 12; ++index)
                if (model.cards[index].selected)
                    focused_slot = static_cast<int>(index);
            if (focused_slot >= 0) {
                const Rect target = scaled(card_rect(focused_slot), 1.065f).inset(-10);
                if (!focus_ring_ready) {
                    focus_ring.snap(target);
                    focus_ring_ready = true;
                } else {
                    focus_ring.target(target);
                    focus_ring.update(dt, 20.0f);
                }
            } else {
                focus_ring_ready = false;
            }
            scene.push_clip({0, kGridTop - 24, hui::gfx::kVirtualWidth, 738});
            for (std::size_t index = 0; index < model.cards.size() && index < 12; ++index) {
                if (static_cast<int>(index) == focused_slot)
                    continue;
                draw_card(model.cards[index], static_cast<int>(index), false,
                          hui::tween::stagger(catalog_age, static_cast<int>(index), 0.025f, 0.34f));
            }
            if (focus_ring_ready) {
                const Rect ring = focus_ring.value();
                scene.glow(ring, 22, 28,
                           kGold.with_alpha(0.28f + 0.16f * hui::ui::breathe(clock)));
            }
            if (focused_slot >= 0)
                draw_card(model.cards[static_cast<std::size_t>(focused_slot)], focused_slot, true,
                          hui::tween::stagger(catalog_age, focused_slot, 0.025f, 0.34f));
            if (focus_ring_ready)
                scene.bordered_rect(focus_ring.value(), 22, kClear, 4, kGold);
            scene.pop_clip();
        }
        draw_status(model);
        if (!model.choosing_variant && !model.showing_purchase)
            draw_hints(model, scene);
        if (model.choosing_variant)
            draw_variant_dialog(model);
        if (model.showing_purchase)
            draw_purchase_dialog(model);
    }

    void draw(const CloudUiModel &model) {
        const std::int64_t now = hui::sys::monotonic_us();
        float dt = previous_time_us == 0 ? 1.0f / 30.0f :
            static_cast<float>(now - previous_time_us) / 1000000.0f;
        previous_time_us = now;
        dt = std::clamp(dt, 0.0f, 0.05f);
        clock += dt;
        screen_age += dt;
        catalog_age += dt;
        selected_age += dt;
        ++frame;

        if (model.screen != previous_screen) {
            previous_screen = model.screen;
            screen_age = 0.0f;
            focus_ring_ready = false;
        }
        if (model.screen == CloudUiScreen::Catalog) {
            std::string signature = model.count;
            for (const std::string &filter : model.filters) signature += "\n" + filter;
            if (signature != catalog_signature) {
                catalog_signature = std::move(signature);
                catalog_age = 0.0f;
            }
            std::string selection;
            for (std::size_t index = 0; index < model.cards.size(); ++index) {
                if (model.cards[index].selected) {
                    selection = std::to_string(index) + ":" + model.cards[index].title;
                    break;
                }
            }
            if (selection != selected_signature) {
                selected_signature = std::move(selection);
                selected_age = 0.0f;
            }
        } else if (!selected_signature.empty()) {
            selected_signature.clear();
            selected_age = 0.0f;
        }

        scene.clear();
        overlay.clear();
        draw_page_nav(model);
        if (model.screen == CloudUiScreen::Catalog)
            draw_catalog(model, dt);
        else
            draw_rows(model);

        hui::gfx::BackdropSpec backdrop;
        backdrop.mode = hui::gfx::BackdropMode::bokeh;
        backdrop.colors[0] = kNightTop;
        backdrop.colors[1] = kNightBottom;
        backdrop.colors[2] = Color::rgb(0xffc978);
        backdrop.colors[3] = Color::rgb(0x7d6bff);
        backdrop.time = clock;

        renderer.begin();
        renderer.backdrop(backdrop);
        renderer.draw(scene);
        if (!overlay.empty()) {
            renderer.glass();
            renderer.draw(overlay);
        }
        renderer.present(0, display.width(), display.height());
        display.swap();
        collect_cover_textures();
        if (!splash_hidden) {
            splash_hidden = hui::sys::hide_splash_screen();
            hui::sys::log("[CloudPlay] first Paper UI frame presented");
        }
    }
};

CloudUiView::CloudUiView() : impl_(new Impl) {}

CloudUiView::~CloudUiView() {
    shutdown();
    delete impl_;
}

bool CloudUiView::initialize() {
    if (impl_->initialized)
        return true;
    app_log("paper", "initialize_enter");
    hui::sys::log("[CloudPlay] Paper UI initialize");
    app_log("paper", "display_open_start");
    if (!impl_->display.open(1920, 1080)) {
        app_log("paper", "display_open_failed");
        shutdown();
        return false;
    }
    app_log("paper", "display_open_done");
    app_log("paper", "renderer_init_start");
    if (!impl_->renderer.init()) {
        app_log("paper", "renderer_init_failed");
        shutdown();
        return false;
    }
    app_log("paper", "renderer_init_done");
    if (!impl_->load_font("/app0/assets/fonts/inter-regular.huifont", impl_->regular_font,
                          impl_->fonts.regular) ||
        !impl_->load_font("/app0/assets/fonts/inter-semibold.huifont", impl_->semibold_font,
                          impl_->fonts.semibold) ||
        !impl_->load_font("/app0/assets/fonts/montserrat-medium.huifont", impl_->display_font,
                          impl_->fonts.display)) {
        shutdown();
        return false;
    }
    impl_->fonts.mono = impl_->fonts.regular;
    impl_->fonts.pixel = impl_->fonts.regular;
    impl_->fonts.hand = impl_->fonts.regular;
    impl_->previous_time_us = hui::sys::monotonic_us();
    impl_->initialized = true;
    impl_->splash_hidden = false;
    impl_->focus_ring_ready = false;
    native_agc_mark_initialized();
    app_log("paper", "initialize_done", 1);
    return true;
}

void CloudUiView::shutdown() {
    if (!impl_)
        return;
    if (impl_->initialized) {
        for (auto &[key, cover] : impl_->covers) {
            (void)key;
            if (cover.name != 0)
                glDeleteTextures(1, &cover.name);
        }
    }
    impl_->covers.clear();
    impl_->renderer.release();
    impl_->display.close();
    impl_->initialized = false;
    impl_->focus_ring_ready = false;
    impl_->previous_time_us = 0;
}

bool CloudUiView::ready() const {
    return impl_ && impl_->initialized;
}

void CloudUiView::present(const CloudUiModel &model) {
    if (ready())
        impl_->draw(model);
}

} // namespace cloudplay

#endif
