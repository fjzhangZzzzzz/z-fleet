#include "zfleet/core/version.h"
#include "zfleet/platform/system.h"
#include "zfleet/protocol/message.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("integration scaffold links core platform and protocol modules") {
  REQUIRE_FALSE(zfleet::core::project_name().empty());
  REQUIRE_FALSE(zfleet::core::version().empty());
  REQUIRE_FALSE(zfleet::platform::os_name().empty());
  REQUIRE_FALSE(zfleet::protocol::protocol_version().empty());
}

TEST_CASE("integration scaffold reserves space for future agent server flow") {
  SUCCEED("Replace this placeholder with register/heartbeat/assets integration flow in v0.1.");
}
