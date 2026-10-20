#include "doctest.h"

#include "http/date.h"

#include <string>

TEST_CASE("http dates: format is IMF-fixdate") {
    CHECK(http::formatHttpDate(784111777) == "Sun, 06 Nov 1994 08:49:37 GMT");
    CHECK(http::formatHttpDate(0) == "Thu, 01 Jan 1970 00:00:00 GMT");
    CHECK(http::formatHttpDate(-1) == "Wed, 31 Dec 1969 23:59:59 GMT");
    CHECK(http::formatHttpDate(2147483648LL) == "Tue, 19 Jan 2038 03:14:08 GMT");
}

TEST_CASE("http dates: the three legacy formats agree") {
    const auto a = http::parseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT");
    const auto b = http::parseHttpDate("Sunday, 06-Nov-94 08:49:37 GMT");
    const auto c = http::parseHttpDate("Sun Nov  6 08:49:37 1994");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(c.has_value());
    CHECK(*a == 784111777);
    CHECK(*b == 784111777);
    CHECK(*c == 784111777);

    CHECK(http::parseHttpDate(http::formatHttpDate(1700000000)) == 1700000000);
    CHECK(http::parseHttpDate("Thursday, 01-Jan-70 00:00:00 GMT") ==
          0); // two-digit pivot: 70 -> 1970
    CHECK(http::parseHttpDate("Saturday, 01-Jan-99 00:00:00 GMT") ==
          http::parseHttpDate("Sat, 01 Jan 1999 00:00:00 GMT"));
    CHECK(http::parseHttpDate("Fri, 29 Feb 2000 00:00:00 GMT").has_value()); // leap
}

TEST_CASE("http dates: garbage is nullopt, never a guess") {
    const char* bad[] = {
        "",
        "garbage",
        "Sun, 06 Nov 1994 08:49:37",       // no timezone
        "Sun, 06 Foo 1994 08:49:37 GMT",   // unknown month
        "Sun, 31 Feb 1999 08:49:37 GMT",   // not a leap year
        "Sun, 00 Nov 1994 08:49:37 GMT",   // day zero
        "Sun, 06 Nov 1994 25:49:37 GMT",   // hour
        "Sun, 06 Nov 1994 08:60:37 GMT",   // minute
        "Sun, 06 Nov 1994 08:49:37 UTC+2", // offset form is not HTTP
        "1994-11-06T08:49:37Z",            // ISO is not HTTP
        "Sun, 06-11-94 08:49:37 GMT",      // numeric month in RFC850 slot
    };
    for (const char* s : bad) {
        CHECK_MESSAGE(!http::parseHttpDate(s).has_value(), s);
    }
}
