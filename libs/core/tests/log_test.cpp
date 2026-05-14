#include "zfleet/core/log.h"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

TEST_CASE("log context appends structured fields") {
  const auto context = zfleet::core::log::Component("server").With(
      {{"request_id", "req-1"}, {"agent_id", "agent-1"}});

  REQUIRE(context.component() == "server");
  REQUIRE(context.Render() == "server request_id=req-1 agent_id=agent-1");
}

TEST_CASE("log level parser accepts known values") {
  REQUIRE(zfleet::core::log::ParseLevel("debug") ==
          zfleet::core::log::Level::kDebug);
  REQUIRE(zfleet::core::log::ParseLevel("WARNING") ==
          zfleet::core::log::Level::kWarn);
  REQUIRE(zfleet::core::log::ToString(zfleet::core::log::Level::kCritical) ==
          "critical");
}

TEST_CASE("log level parser rejects unknown values") {
  REQUIRE_THROWS_AS(zfleet::core::log::ParseLevel("verbose"),
                    std::invalid_argument);
}
