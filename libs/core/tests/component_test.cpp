#include "zfleet/core/component.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("component helpers validate names and derive binary names") {
  REQUIRE(zfleet::core::IsKnownComponent("agent"));
  REQUIRE(zfleet::core::IsKnownComponent("server"));
  REQUIRE(zfleet::core::IsKnownComponent("installer"));
  REQUIRE_FALSE(zfleet::core::IsKnownComponent("worker"));
  REQUIRE(zfleet::core::ValidateComponent("agent").ok);
  REQUIRE_FALSE(zfleet::core::ValidateComponent("worker").ok);
  REQUIRE(zfleet::core::ValidateComponent("worker").message ==
          "expected agent, server, or installer");

#ifdef _WIN32
  REQUIRE(zfleet::core::BinaryNameForComponent("agent") == "zfleet_agent.exe");
#else
  REQUIRE(zfleet::core::BinaryNameForComponent("agent") == "zfleet_agent");
#endif
  REQUIRE_THROWS_AS(zfleet::core::BinaryNameForComponent("worker"),
                    std::invalid_argument);
}

TEST_CASE("component helpers map launcher binary names back to components") {
  REQUIRE(zfleet::core::ComponentForBinaryName("zfleet_agent") ==
          std::optional<std::string>{"agent"});
  REQUIRE(zfleet::core::ComponentForBinaryName("zfleet_server") ==
          std::optional<std::string>{"server"});
  REQUIRE(zfleet::core::ComponentForBinaryName("zfleet_installer") ==
          std::optional<std::string>{"installer"});
  REQUIRE_FALSE(zfleet::core::ComponentForBinaryName("zfleet_worker").has_value());
}
