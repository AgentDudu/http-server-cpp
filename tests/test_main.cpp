#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

TEST_CASE("toolchain produces a working test binary") {
    CHECK(1 + 1 == 2);
}
