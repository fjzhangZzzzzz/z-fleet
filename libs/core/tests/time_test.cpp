#include "zfleet/core/time.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("utc time helper returns a non-empty timestamp") {
  REQUIRE_FALSE(zfleet::core::NowUtcRfc3339().empty());
}
