// ps5-homebrew-ui - Baked SDF font: loading, measuring and glyph layout.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx/font.hpp"

#include <algorithm>
#include <cstring>

namespace hui::gfx
{

namespace ff = font_format;

namespace
{
std::uint32_t fallback_codepoint(std::uint32_t codepoint)
{
    switch (codepoint)
    {
    case 0x02bc: // modifier apostrophe
    case 0x2018: // left single quotation mark
    case 0x2019: // right single quotation mark
    case 0x201a:
    case 0x201b:
    case 0x2032:
        return '\'';
    case 0x201c:
    case 0x201d:
    case 0x201e:
    case 0x201f:
    case 0x2033:
        return '"';
    case 0x00a0:
        return ' ';
    // The compact bundled UI fonts only contain the ASCII Latin alphabet.
    // Preserve readable Store titles by falling back accented Latin letters
    // to their base glyph instead of rendering the missing-glyph question mark.
    case 0x00c0: case 0x00c1: case 0x00c2: case 0x00c3: case 0x00c4: case 0x00c5:
    case 0x0100: case 0x0102: case 0x0104: case 0x00c6:
        return 'A';
    case 0x00e0: case 0x00e1: case 0x00e2: case 0x00e3: case 0x00e4: case 0x00e5:
    case 0x0101: case 0x0103: case 0x0105: case 0x00e6:
        return 'a';
    case 0x00c7: case 0x0106: case 0x0108: case 0x010a: case 0x010c:
        return 'C';
    case 0x00e7: case 0x0107: case 0x0109: case 0x010b: case 0x010d:
        return 'c';
    case 0x00d0: case 0x010e: case 0x0110:
        return 'D';
    case 0x00f0: case 0x010f: case 0x0111:
        return 'd';
    case 0x00c8: case 0x00c9: case 0x00ca: case 0x00cb:
    case 0x0112: case 0x0114: case 0x0116: case 0x0118: case 0x011a:
        return 'E';
    case 0x00e8: case 0x00e9: case 0x00ea: case 0x00eb:
    case 0x0113: case 0x0115: case 0x0117: case 0x0119: case 0x011b:
        return 'e';
    case 0x011c: case 0x011e: case 0x0120: case 0x0122:
        return 'G';
    case 0x011d: case 0x011f: case 0x0121: case 0x0123:
        return 'g';
    case 0x0124: case 0x0126:
        return 'H';
    case 0x0125: case 0x0127:
        return 'h';
    case 0x00cc: case 0x00cd: case 0x00ce: case 0x00cf:
    case 0x0128: case 0x012a: case 0x012c: case 0x012e: case 0x0130:
        return 'I';
    case 0x00ec: case 0x00ed: case 0x00ee: case 0x00ef:
    case 0x0129: case 0x012b: case 0x012d: case 0x012f: case 0x0131:
        return 'i';
    case 0x0134:
        return 'J';
    case 0x0135:
        return 'j';
    case 0x0136:
        return 'K';
    case 0x0137:
        return 'k';
    case 0x0139: case 0x013b: case 0x013d: case 0x013f: case 0x0141:
        return 'L';
    case 0x013a: case 0x013c: case 0x013e: case 0x0140: case 0x0142:
        return 'l';
    case 0x00d1: case 0x0143: case 0x0145: case 0x0147: case 0x014a:
        return 'N';
    case 0x00f1: case 0x0144: case 0x0146: case 0x0148: case 0x0149: case 0x014b:
        return 'n';
    case 0x00d2: case 0x00d3: case 0x00d4: case 0x00d5: case 0x00d6: case 0x00d8:
    case 0x014c: case 0x014e: case 0x0150: case 0x0152:
        return 'O';
    case 0x00f2: case 0x00f3: case 0x00f4: case 0x00f5: case 0x00f6: case 0x00f8:
    case 0x014d: case 0x014f: case 0x0151: case 0x0153:
        return 'o';
    case 0x0154: case 0x0156: case 0x0158:
        return 'R';
    case 0x0155: case 0x0157: case 0x0159:
        return 'r';
    case 0x015a: case 0x015c: case 0x015e: case 0x0160:
        return 'S';
    case 0x00df: case 0x015b: case 0x015d: case 0x015f: case 0x0161:
        return 's';
    case 0x00de: case 0x0162: case 0x0164: case 0x0166:
        return 'T';
    case 0x00fe: case 0x0163: case 0x0165: case 0x0167:
        return 't';
    case 0x00d9: case 0x00da: case 0x00db: case 0x00dc:
    case 0x0168: case 0x016a: case 0x016c: case 0x016e: case 0x0170: case 0x0172:
        return 'U';
    case 0x00f9: case 0x00fa: case 0x00fb: case 0x00fc:
    case 0x0169: case 0x016b: case 0x016d: case 0x016f: case 0x0171: case 0x0173:
        return 'u';
    case 0x0174:
        return 'W';
    case 0x0175:
        return 'w';
    case 0x00dd: case 0x0176: case 0x0178:
        return 'Y';
    case 0x00fd: case 0x00ff: case 0x0177:
        return 'y';
    case 0x0179: case 0x017b: case 0x017d:
        return 'Z';
    case 0x017a: case 0x017c: case 0x017e:
        return 'z';
    default:
        return codepoint;
    }
}
} // namespace

std::uint32_t next_codepoint(std::string_view text, std::size_t *index)
{
    const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(text[at]); };
    const std::size_t i = *index;
    const unsigned char lead = byte(i);
    int length = 1;
    std::uint32_t value = lead;
    if (lead >= 0xf0 && lead < 0xf8)
    {
        length = 4;
        value = lead & 0x07u;
    }
    else if (lead >= 0xe0)
    {
        length = 3;
        value = lead & 0x0fu;
    }
    else if (lead >= 0xc0)
    {
        length = 2;
        value = lead & 0x1fu;
    }
    else if (lead >= 0x80)
    {
        *index = i + 1;
        return 0xfffd;
    }
    if (i + static_cast<std::size_t>(length) > text.size())
    {
        *index = text.size();
        return 0xfffd;
    }
    for (int k = 1; k < length; ++k)
    {
        const unsigned char continuation = byte(i + static_cast<std::size_t>(k));
        if ((continuation & 0xc0u) != 0x80u)
        {
            *index = i + 1;
            return 0xfffd;
        }
        value = (value << 6) | (continuation & 0x3fu);
    }
    *index = i + static_cast<std::size_t>(length);
    return value;
}

bool Font::load(std::string_view data)
{
    error_.clear();
    if (data.size() < sizeof(ff::Header))
    {
        error_ = "font too small";
        return false;
    }
    std::memcpy(&header_, data.data(), sizeof(header_));
    if (header_.magic != ff::kMagic || header_.version != ff::kVersion ||
        header_.pixel_size <= 0.0f)
    {
        error_ = "not a huifont v1";
        return false;
    }
    const std::size_t glyph_bytes =
        static_cast<std::size_t>(header_.glyph_count) * sizeof(ff::Glyph);
    const std::size_t kern_bytes = static_cast<std::size_t>(header_.kern_count) * sizeof(ff::Kern);
    const std::size_t atlas_bytes = static_cast<std::size_t>(header_.atlas_width) *
                                    static_cast<std::size_t>(header_.atlas_height);
    if (data.size() != sizeof(ff::Header) + glyph_bytes + kern_bytes + atlas_bytes)
    {
        error_ = "font size mismatch";
        return false;
    }
    const char *cursor = data.data() + sizeof(ff::Header);
    // A font may have no glyphs or (monospaced faces) no kerning pairs.
    glyphs_.resize(header_.glyph_count);
    if (glyph_bytes != 0)
        std::memcpy(glyphs_.data(), cursor, glyph_bytes);
    cursor += glyph_bytes;
    kerns_.resize(header_.kern_count);
    if (kern_bytes != 0)
        std::memcpy(kerns_.data(), cursor, kern_bytes);
    cursor += kern_bytes;
    atlas_.assign(reinterpret_cast<const std::uint8_t *>(cursor),
                  reinterpret_cast<const std::uint8_t *>(cursor) + atlas_bytes);
    for (const ff::Glyph &glyph : glyphs_)
    {
        if (glyph.x + glyph.w > header_.atlas_width || glyph.y + glyph.h > header_.atlas_height)
        {
            error_ = "glyph outside atlas";
            return false;
        }
    }
    return true;
}

const ff::Glyph *Font::find(std::uint32_t codepoint) const
{
    const auto it =
        std::lower_bound(glyphs_.begin(), glyphs_.end(), codepoint,
                         [](const ff::Glyph &g, std::uint32_t c) { return g.codepoint < c; });
    return it != glyphs_.end() && it->codepoint == codepoint ? &*it : nullptr;
}

float Font::kern(std::uint32_t first, std::uint32_t second) const
{
    const auto it = std::lower_bound(
        kerns_.begin(), kerns_.end(), std::make_pair(first, second),
        [](const ff::Kern &k, const std::pair<std::uint32_t, std::uint32_t> &key)
        { return k.first != key.first ? k.first < key.first : k.second < key.second; });
    return it != kerns_.end() && it->first == first && it->second == second ? it->amount : 0.0f;
}

std::string Font::fit(std::string_view text, float size, float max_width, float tracking) const
{
    if (measure(text, size, tracking) <= max_width)
        return std::string(text);
    constexpr std::string_view kEllipsis = "\xE2\x80\xA6";
    const std::string_view mark = has_glyph(0x2026) ? kEllipsis : std::string_view("...");
    std::string best;
    for (std::size_t index = 0; index < text.size();)
    {
        const std::size_t start = index;
        next_codepoint(text, &index);
        std::string candidate(text.substr(0, start));
        while (!candidate.empty() && candidate.back() == ' ')
            candidate.pop_back();
        candidate.append(mark);
        if (measure(candidate, size, tracking) > max_width)
            break;
        best = std::move(candidate);
    }
    return best;
}

float Font::measure(std::string_view text, float size, float tracking) const
{
    const float scale = size / header_.pixel_size;
    float width = 0.0f;
    std::uint32_t previous = 0;
    int glyphs = 0;
    for (std::size_t index = 0; index < text.size();)
    {
        std::uint32_t codepoint = next_codepoint(text, &index);
        const ff::Glyph *glyph = find(codepoint);
        if (glyph == nullptr)
        {
            codepoint = fallback_codepoint(codepoint);
            glyph = find(codepoint);
        }
        if (glyph == nullptr)
        {
            codepoint = '?';
            glyph = find(codepoint);
            if (glyph == nullptr)
                continue;
        }
        if (previous != 0)
            width += kern(previous, codepoint) * scale;
        width += glyph->advance * scale;
        previous = codepoint;
        ++glyphs;
    }
    return glyphs > 1 ? width + tracking * static_cast<float>(glyphs - 1) : width;
}

float Font::layout(std::string_view text, float x, float y, float size, Align align,
                   std::vector<GlyphQuad> &quads, float tracking) const
{
    const float scale = size / header_.pixel_size;
    const float width = measure(text, size, tracking);
    float pen = x;
    if (align == Align::center)
        pen -= width * 0.5f;
    else if (align == Align::right)
        pen -= width;
    const float inverse_w = 1.0f / static_cast<float>(header_.atlas_width);
    const float inverse_h = 1.0f / static_cast<float>(header_.atlas_height);
    std::uint32_t previous = 0;
    for (std::size_t index = 0; index < text.size();)
    {
        std::uint32_t codepoint = next_codepoint(text, &index);
        const ff::Glyph *glyph = find(codepoint);
        if (glyph == nullptr)
        {
            codepoint = fallback_codepoint(codepoint);
            glyph = find(codepoint);
        }
        if (glyph == nullptr)
        {
            codepoint = '?';
            glyph = find(codepoint);
            if (glyph == nullptr)
                continue;
        }
        if (previous != 0)
            pen += kern(previous, codepoint) * scale;
        if (glyph->w > 0 && glyph->h > 0)
        {
            GlyphQuad quad;
            quad.x0 = pen + glyph->offset_x * scale;
            quad.y0 = y + glyph->offset_y * scale;
            quad.x1 = quad.x0 + static_cast<float>(glyph->w) * scale;
            quad.y1 = quad.y0 + static_cast<float>(glyph->h) * scale;
            quad.u0 = static_cast<float>(glyph->x) * inverse_w;
            quad.v0 = static_cast<float>(glyph->y) * inverse_h;
            quad.u1 = static_cast<float>(glyph->x + glyph->w) * inverse_w;
            quad.v1 = static_cast<float>(glyph->y + glyph->h) * inverse_h;
            quads.push_back(quad);
        }
        pen += glyph->advance * scale + tracking;
        previous = codepoint;
    }
    return width;
}

std::vector<std::string> Font::wrap(std::string_view text, float size, float max_width) const
{
    std::vector<std::string> lines;
    std::string line;
    std::size_t index = 0;
    while (index <= text.size())
    {
        const std::size_t newline = text.find('\n', index);
        const std::string_view paragraph = text.substr(
            index, newline == std::string_view::npos ? std::string_view::npos : newline - index);
        std::size_t word_start = 0;
        line.clear();
        while (word_start <= paragraph.size())
        {
            std::size_t word_end = paragraph.find(' ', word_start);
            if (word_end == std::string_view::npos)
                word_end = paragraph.size();
            const std::string_view word = paragraph.substr(word_start, word_end - word_start);
            const std::string candidate =
                line.empty() ? std::string(word) : line + " " + std::string(word);
            if (!line.empty() && measure(candidate, size) > max_width)
            {
                lines.push_back(line);
                line.assign(word);
            }
            else
            {
                line = candidate;
            }
            word_start = word_end + 1;
        }
        lines.push_back(line);
        if (newline == std::string_view::npos)
            break;
        index = newline + 1;
    }
    return lines;
}

} // namespace hui::gfx
