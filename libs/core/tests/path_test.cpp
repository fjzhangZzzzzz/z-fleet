#include "zfleet/core/path.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("path helpers validate safe path segments") {
  REQUIRE(zfleet::core::IsSafePathSegment("0.1.0"));
  REQUIRE(zfleet::core::IsSafePathSegment("linux_debug-1"));
  REQUIRE_FALSE(zfleet::core::IsSafePathSegment(""));
  REQUIRE_FALSE(zfleet::core::IsSafePathSegment("."));
  REQUIRE_FALSE(zfleet::core::IsSafePathSegment(".."));
  REQUIRE_FALSE(zfleet::core::IsSafePathSegment("bad/value"));
  REQUIRE_FALSE(zfleet::core::IsSafePathSegment("bad value"));
  REQUIRE(zfleet::core::ValidatePathSegment("0.1.0").ok);
  REQUIRE_FALSE(zfleet::core::ValidatePathSegment("bad/value").ok);
  REQUIRE(zfleet::core::ValidatePathSegment("bad/value").message ==
          "expected non-empty single segment using only letters, digits, '.', '_', or '-'");
}

TEST_CASE("path helpers normalize safe relative paths") {
  REQUIRE(zfleet::core::NormalizeSafeRelativePath("payload/bin/zfleet_agent") ==
          std::optional<std::string>{"payload/bin/zfleet_agent"});
  REQUIRE(zfleet::core::NormalizeSafeRelativePath("payload\\bin\\zfleet_agent") ==
          std::optional<std::string>{"payload/bin/zfleet_agent"});
  REQUIRE_FALSE(zfleet::core::NormalizeSafeRelativePath("").has_value());
  REQUIRE_FALSE(zfleet::core::NormalizeSafeRelativePath("/absolute").has_value());
  REQUIRE_FALSE(zfleet::core::NormalizeSafeRelativePath("C:/absolute").has_value());
  REQUIRE_FALSE(zfleet::core::NormalizeSafeRelativePath("../escape").has_value());
  REQUIRE_FALSE(zfleet::core::NormalizeSafeRelativePath("payload/../escape")
                    .has_value());
  REQUIRE(zfleet::core::ValidateRelativePath("payload\\bin\\zfleet_agent").ok);
  REQUIRE(zfleet::core::ValidateRelativePath("payload\\bin\\zfleet_agent")
              .value == "payload/bin/zfleet_agent");
  REQUIRE_FALSE(zfleet::core::ValidateRelativePath("../escape").ok);
  REQUIRE(zfleet::core::ValidateRelativePath("../escape").message ==
          "must not contain '.' or '..'");
}
