// SPDX-License-Identifier: GPL-3.0-or-later
#include "credentials.hpp"

#include <cstdint>
#include <ctime>
#include <string_view>

namespace cloudplay {
namespace {

bool valid_token(const std::string &token) {
    if (token.size() > 2048) return false;
    for (unsigned char ch : token)
        if (ch < 0x21 || ch > 0x7e) return false;
    return true;
}

bool valid_expiry(const std::string &expiry) {
    if (expiry.size() > 2048) return false;
    for (unsigned char ch : expiry)
        if (ch < 0x20 || ch > 0x7e) return false;
    return true;
}

bool digits(std::string_view value, size_t offset, size_t count, int &result) {
    if (offset + count > value.size()) return false;
    result = 0;
    for (size_t index = 0; index < count; ++index) {
        const char ch = value[offset + index];
        if (ch < '0' || ch > '9') return false;
        result = result * 10 + ch - '0';
    }
    return true;
}

bool leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int days_in_month(int year, int month) {
    static constexpr int days[] = {31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31};
    return month == 2 && leap_year(year) ? 29 : days[month - 1];
}

// Days since 1970-01-01. This avoids timegm(), which is not available in the
// PS5 libc while still interpreting the stored timestamp independently of the
// console's local timezone.
int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
    const unsigned day_of_year =
        (153 * adjusted_month + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 -
        year_of_era / 100 + day_of_year;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(day_of_era) - 719468;
}

bool expiry_epoch(std::string_view value, int64_t &result) {
    // Accepted format: YYYY-MM-DD HH:MM:SS GMT+H, with an optional :MM.
    if (value.size() < 25 || value[4] != '-' || value[7] != '-' || value[10] != ' ' ||
        value[13] != ':' || value[16] != ':' || value.substr(19, 4) != " GMT" ||
        (value[23] != '+' && value[23] != '-')) return false;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!digits(value, 0, 4, year) || !digits(value, 5, 2, month) ||
        !digits(value, 8, 2, day) || !digits(value, 11, 2, hour) ||
        !digits(value, 14, 2, minute) || !digits(value, 17, 2, second) ||
        year < 1970 || month < 1 || month > 12 || day < 1 ||
        day > days_in_month(year, month) || hour > 23 || minute > 59 || second > 60)
        return false;

    const size_t colon = value.find(':', 24);
    const size_t hour_digits = (colon == std::string_view::npos ? value.size() : colon) - 24;
    if (hour_digits < 1 || hour_digits > 2) return false;
    int offset_hour = 0, offset_minute = 0;
    if (!digits(value, 24, hour_digits, offset_hour) || offset_hour > 23) return false;
    if (colon != std::string_view::npos) {
        if (colon + 3 != value.size() || !digits(value, colon + 1, 2, offset_minute) ||
            offset_minute > 59) return false;
    }
    const int offset = (offset_hour * 60 + offset_minute) * 60 *
        (value[23] == '+' ? 1 : -1);
    result = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) *
        86400 + hour * 3600 + minute * 60 + second - offset;
    return true;
}

} // namespace

bool valid_credentials(const Credentials &credentials) {
    return valid_token(credentials.npsso) &&
           valid_token(credentials.psn_access_token) &&
           valid_token(credentials.psn_refresh_token) &&
           valid_expiry(credentials.psn_auth_token_expiry) &&
           valid_token(credentials.psn_account_id);
}

PsnTokenExpiryState psn_token_expiry_state(const Credentials &credentials) {
    if (credentials.psn_auth_token_expiry.empty()) return PsnTokenExpiryState::Missing;
    int64_t expiry = 0;
    if (!expiry_epoch(credentials.psn_auth_token_expiry, expiry))
        return PsnTokenExpiryState::Invalid;
    return static_cast<int64_t>(std::time(nullptr)) >= expiry ?
        PsnTokenExpiryState::Expired : PsnTokenExpiryState::Active;
}

} // namespace cloudplay
