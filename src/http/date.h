#pragma once

#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace http {

// RFC 7231 IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT".
std::string formatHttpDate(std::time_t epochSeconds);

// Accepts the three dates HTTP recipients MUST parse: IMF-fixdate,
// RFC 850 ("Sunday, 06-Nov-94 08:49:37 GMT", two-digit year pivoted at 1970)
// and asctime ("Sun Nov  6 08:49:37 1994"). nullopt for anything else,
// including missing timezone or out-of-range fields.
std::optional<std::time_t> parseHttpDate(std::string_view value);

} // namespace http
