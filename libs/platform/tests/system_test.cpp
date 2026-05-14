#include "zfleet/platform/system.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("platform system metadata is available") {
  REQUIRE_FALSE(zfleet::platform::os_name().empty());
  REQUIRE_FALSE(zfleet::platform::architecture_name().empty());
  REQUIRE_FALSE(zfleet::platform::hostname().empty());
}
