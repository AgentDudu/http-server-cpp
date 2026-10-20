#include "http/date.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <vector>

namespace http {

namespace {

constexpr const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

bool isAlpha3(std::string_view t) {
    return t.size() == 3 && std::isalpha(static_cast<unsigned char>(t[0])) &&
           std::isalpha(static_cast<unsigned char>(t[1])) &&
           std::isalpha(static_cast<unsigned char>(t[2]));
}

bool allDigits(std::string_view t) {
    if (t.empty() || t.size() > 4) {
        return false;
    }
    for (char c : t) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

long toLong(std::string_view t) {
    long v = 0;
    for (char c : t) {
        v = v * 10 + (c - '0');
    }
    return v;
}

// Days since 1970-01-01 (Howard Hinnant's civil calendar algorithm).
long long daysFromCivil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

void civilFromDays(long long z0, long long& y, unsigned& m, unsigned& d) {
    const long long z = z0 + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long yy = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = yy + (m <= 2 ? 1 : 0);
}

bool daysInMonth(unsigned y, unsigned m, unsigned d) {
    static const unsigned kLen[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12 || d < 1) {
        return false;
    }
    unsigned len = kLen[m - 1];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) {
        len = 29;
    }
    return d <= len;
}

int monthFromName(std::string_view t) {
    for (int i = 0; i < 12; ++i) {
        bool same = true;
        for (int j = 0; j < 3; ++j) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(t[j])));
            const char b =
                static_cast<char>(std::tolower(static_cast<unsigned char>(kMonths[i][j])));
            if (a != b) {
                same = false;
                break;
            }
        }
        if (same) {
            return i + 1;
        }
    }
    return -1;
}

bool parseHms(std::string_view t, long& h, long& m, long& s) {
    if (t.size() != 8 || t[2] != ':' || t[5] != ':') {
        return false;
    }
    const auto part = [&](std::string_view p, long& out) {
        if (!allDigits(p) || p.size() != 2) {
            return false;
        }
        out = toLong(p);
        return true;
    };
    return part(t.substr(0, 2), h) && part(t.substr(3, 2), m) && part(t.substr(6, 2), s);
}

} // namespace

std::string formatHttpDate(std::time_t epochSeconds) {
    const long long secs = static_cast<long long>(epochSeconds);
    long long days = secs / 86400;
    long long rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    long long y;
    unsigned mo, d;
    civilFromDays(days, y, mo, d);
    char buf[64]; // fits worst-case wide-year expansion gcc likes to warn about
    std::snprintf(buf, sizeof(buf), "%s, %02u %s %04lld %02lld:%02lld:%02lld GMT",
                  kDays[(((days + 4) % 7) + 7) % 7], d, kMonths[mo - 1], y, rem / 3600,
                  (rem / 60) % 60, rem % 60);
    return buf;
}

std::optional<std::time_t> parseHttpDate(std::string_view value) {
    // Split on separators; "dd-Mon-yy" folds into the same token stream.
    std::vector<std::string_view> tok;
    {
        std::string_view rest = value;
        while (!rest.empty()) {
            const auto cut = rest.find_first_of(" ,-\t");
            const auto piece = cut == std::string_view::npos ? rest : rest.substr(0, cut);
            if (!piece.empty()) { // asctime's padded day doubles up separators
                tok.push_back(piece);
            }
            rest = cut == std::string_view::npos ? std::string_view{} : rest.substr(cut + 1);
        }
    }

    long y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    std::string_view timeTok;
    if (tok.size() == 5 && isAlpha3(tok[0]) && isAlpha3(tok[1]) && allDigits(tok[2])) {
        // asctime: "Sun Nov  6 08:49:37 1994" (day space-padded -> its separator
        // produced an empty token, already dropped, so tok[1] is the month).
        mo = monthFromName(tok[1]);
        if (mo < 0 || !allDigits(tok[4])) {
            return std::nullopt;
        }
        d = toLong(tok[2]);
        y = toLong(tok[4]);
        timeTok = tok[3];
    } else if (tok.size() == 6 && allDigits(tok[1]) && isAlpha3(tok[2]) && allDigits(tok[3])) {
        // "06 Nov 1994 ..." and "06 Nov 94 ..." (RFC 850 with '-' separators).
        if (tok[5].size() != 3 || std::tolower(static_cast<unsigned char>(tok[5][0])) != 'g') {
            return std::nullopt; // timezone (GMT/UTC/EST...) must be present
        }
        d = toLong(tok[1]);
        mo = monthFromName(tok[2]);
        if (mo < 0) {
            return std::nullopt;
        }
        y = toLong(tok[3]);
        if (tok[3].size() == 2) {
            y += y < 70 ? 2000 : 1900;
        }
        timeTok = tok[4];
    } else {
        return std::nullopt;
    }

    if (!parseHms(timeTok, h, mi, s) || mi > 59 || s > 60 || h > 23 ||
        !daysInMonth(static_cast<unsigned>(y), static_cast<unsigned>(mo),
                     static_cast<unsigned>(d))) {
        return std::nullopt;
    }
    const long long days = daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d));
    return static_cast<std::time_t>(days * 86400 + h * 3600 + mi * 60 + s);
}

} // namespace http
