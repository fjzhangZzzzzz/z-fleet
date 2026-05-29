#pragma once

#include <compare>
#include <optional>
#include <string_view>

namespace zfleet::core {

struct Semver3 {
  int major = 0;
  int minor = 0;
  int patch = 0;

  auto operator<=>(const Semver3&) const = default;
};

std::optional<Semver3> ParseSemver3(std::string_view value);

}  // namespace zfleet::core
