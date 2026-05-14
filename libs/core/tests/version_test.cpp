#include "zfleet/core/version.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("core version metadata is available") {
  REQUIRE_FALSE(zfleet::core::project_name().empty());
  REQUIRE_FALSE(zfleet::core::version().empty());
}
