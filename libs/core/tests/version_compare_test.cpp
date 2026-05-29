#include "zfleet/core/version_compare.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>

namespace {

using zfleet::core::ParseSemver3;
using zfleet::core::Semver3;

}  // namespace

TEST_CASE("semver3 parser accepts strict x.y.z versions") {
  REQUIRE(ParseSemver3("0.1.0") ==
          std::optional<Semver3>{Semver3{.major = 0, .minor = 1, .patch = 0}});
  REQUIRE(
      ParseSemver3("12.34.56") ==
      std::optional<Semver3>{Semver3{.major = 12, .minor = 34, .patch = 56}});
}

TEST_CASE("semver3 parser rejects malformed versions") {
  for (const auto* value :
       {"", "1", "1.2", "1.2.3.4", "01.2.3", "1.02.3", "1.2.03", "1..3", "1.2.",
        ".2.3", "a.2.3", "1.b.3", "1.2.c", "v1.2.3", "1.2.3 ", " 1.2.3",
        "+1.2.3", "-1.2.3"}) {
    REQUIRE_FALSE(ParseSemver3(value).has_value());
  }
}

TEST_CASE("semver3 supports direct comparisons") {
  const auto v100 = ParseSemver3("1.0.0");
  const auto v123 = ParseSemver3("1.2.3");
  const auto v124 = ParseSemver3("1.2.4");
  const auto v200 = ParseSemver3("2.0.0");

  REQUIRE(v100.has_value());
  REQUIRE(v123.has_value());
  REQUIRE(v124.has_value());
  REQUIRE(v200.has_value());

  REQUIRE(*v123 == Semver3{.major = 1, .minor = 2, .patch = 3});
  REQUIRE(*v123 >= *v100);
  REQUIRE(*v123 <= *v124);
  REQUIRE(*v124 > *v123);
  REQUIRE(*v200 > *v124);
}
