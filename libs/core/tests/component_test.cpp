#include "zfleet/core/component.h"

#include <catch2/catch_test_macros.hpp>

namespace {

std::string ExecutableSuffix() {
#ifdef _WIN32
  return ".exe";
#else
  return "";
#endif
}

}  // namespace

TEST_CASE("component helpers validate names and derive binary names") {
  const auto suffix = ExecutableSuffix();

  REQUIRE(zfleet::core::IsKnownComponent("agent"));
  REQUIRE(zfleet::core::IsKnownComponent("server"));
  REQUIRE(zfleet::core::IsKnownComponent("installer"));
  REQUIRE_FALSE(zfleet::core::IsKnownComponent("worker"));
  REQUIRE(zfleet::core::ValidateComponent("agent").ok);
  REQUIRE_FALSE(zfleet::core::ValidateComponent("worker").ok);
  REQUIRE(zfleet::core::ValidateComponent("worker").message ==
          "expected agent, server, or installer");

  REQUIRE(zfleet::core::BinaryNameForComponent("agent") ==
          std::string("zfleet_agent") + suffix);
  REQUIRE(zfleet::core::BinaryNameForComponent("server") ==
          std::string("zfleet_server") + suffix);
  REQUIRE(zfleet::core::BinaryNameForComponent("installer") ==
          std::string("zfleet_installer") + suffix);
  REQUIRE_THROWS_AS(zfleet::core::BinaryNameForComponent("worker"),
                    std::invalid_argument);
}

TEST_CASE("component helpers map binary names and launcher artifacts") {
  const auto suffix = ExecutableSuffix();
  REQUIRE(zfleet::core::ComponentForBinaryName("zfleet_agent") ==
          std::optional<std::string>{"agent"});
  REQUIRE(zfleet::core::ComponentForBinaryName("zfleet_server") ==
          std::optional<std::string>{"server"});
  REQUIRE(zfleet::core::ComponentForBinaryName("zfleet_installer") ==
          std::optional<std::string>{"installer"});
#ifdef _WIN32
  REQUIRE(zfleet::core::ComponentForBinaryName("zfleet_agent.exe") ==
          std::optional<std::string>{"agent"});
  REQUIRE(zfleet::core::ComponentForBinaryName("zfleet_server.exe") ==
          std::optional<std::string>{"server"});
  REQUIRE(zfleet::core::ComponentForBinaryName("zfleet_installer.exe") ==
          std::optional<std::string>{"installer"});
#endif
  REQUIRE(zfleet::core::BinaryNameForExecutableStem("launcher") ==
          std::string("zfleet_launcher") + suffix);
#ifdef _WIN32
  REQUIRE(zfleet::core::ArtifactForBinaryName("zfleet_launcher.exe") ==
          std::optional<zfleet::core::BinaryArtifact>{
              zfleet::core::BinaryArtifact::kLauncher});
#else
  REQUIRE(zfleet::core::ArtifactForBinaryName("zfleet_launcher") ==
          std::optional<zfleet::core::BinaryArtifact>{
              zfleet::core::BinaryArtifact::kLauncher});
#endif
  REQUIRE_FALSE(
      zfleet::core::ComponentForBinaryName("zfleet_worker").has_value());
}

TEST_CASE("executable stem helpers validate unknown stems") {
  REQUIRE_THROWS_AS(zfleet::core::BinaryNameForExecutableStem(""),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(zfleet::core::BinaryNameForExecutableStem("bad/stem"),
                    std::invalid_argument);
}
