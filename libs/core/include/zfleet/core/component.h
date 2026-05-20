#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace zfleet::core {

constexpr std::string_view kKnownComponentsDescription =
    "agent, server, or installer";

struct ValidationResult {
  bool ok;
  std::string message;
};

bool IsKnownComponent(std::string_view component) noexcept;
ValidationResult ValidateComponent(std::string_view component);
std::string BinaryNameForComponent(std::string_view component);
std::optional<std::string> ComponentForBinaryName(std::string_view binary_name);

}  // namespace zfleet::core
