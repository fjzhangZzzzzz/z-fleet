#include "zfleet/core/component.h"
#include "zfleet/core/path.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace zfleet::core {
namespace {

bool EndsWithCaseInsensitive(std::string_view value, std::string_view suffix) {
  if (value.size() < suffix.size()) {
    return false;
  }

  const auto offset = value.size() - suffix.size();
  for (std::size_t index = 0; index < suffix.size(); ++index) {
    const auto left = static_cast<unsigned char>(value[offset + index]);
    const auto right = static_cast<unsigned char>(suffix[index]);
    if (std::tolower(left) != std::tolower(right)) {
      return false;
    }
  }
  return true;
}

std::string BinaryLookupName(std::string_view binary_name) {
  std::string lookup_name(binary_name);
#ifdef _WIN32
  if (EndsWithCaseInsensitive(lookup_name, ".exe")) {
    lookup_name.resize(lookup_name.size() - 4);
  }
#endif
  return lookup_name;
}

}  // namespace

bool IsKnownComponent(std::string_view component) noexcept {
  return component == "agent" || component == "server" ||
         component == "installer";
}

ValidationResult ValidateComponent(std::string_view component) {
  if (IsKnownComponent(component)) {
    return ValidationResult{.ok = true, .message = {}};
  }
  return ValidationResult{
      .ok = false,
      .message = "expected " + std::string(kKnownComponentsDescription),
  };
}

std::string BinaryNameForComponent(std::string_view component) {
  const auto validation = ValidateComponent(component);
  if (!validation.ok) {
    throw std::invalid_argument("invalid component: " + validation.message);
  }

#ifdef _WIN32
  return "zfleet_" + std::string(component) + ".exe";
#else
  return "zfleet_" + std::string(component);
#endif
}

std::string BinaryNameForExecutableStem(std::string_view stem) {
  const auto validation = zfleet::core::ValidatePathSegment(stem);
  if (!validation.ok) {
    throw std::invalid_argument("invalid executable stem: " +
                                validation.message);
  }

#ifdef _WIN32
  return "zfleet_" + std::string(stem) + ".exe";
#else
  return "zfleet_" + std::string(stem);
#endif
}

std::string BinaryNameForArtifact(BinaryArtifact artifact) {
  switch (artifact) {
    case BinaryArtifact::kLauncher:
      return BinaryNameForExecutableStem("launcher");
  }
  throw std::invalid_argument("unknown binary artifact");
}

std::optional<std::string> ComponentForBinaryName(
    std::string_view binary_name) {
  const auto lookup_name = BinaryLookupName(binary_name);
  if (lookup_name == "zfleet_agent") {
    return "agent";
  }
  if (lookup_name == "zfleet_server") {
    return "server";
  }
  if (lookup_name == "zfleet_installer") {
    return "installer";
  }
  return std::nullopt;
}

std::optional<BinaryArtifact> ArtifactForBinaryName(
    std::string_view binary_name) {
  const auto lookup_name = BinaryLookupName(binary_name);
  if (lookup_name == "zfleet_launcher") {
    return BinaryArtifact::kLauncher;
  }
  return std::nullopt;
}

}  // namespace zfleet::core
