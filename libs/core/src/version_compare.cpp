#include "zfleet/core/version_compare.h"

#include <array>
#include <charconv>

namespace zfleet::core {
namespace {

bool ParseSemverPart(std::string_view part, int* output) {
  if (part.empty()) {
    return false;
  }
  if (part.size() > 1 && part.front() == '0') {
    return false;
  }

  int parsed = 0;
  const auto* begin = part.data();
  const auto* end = begin + part.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end || parsed < 0) {
    return false;
  }

  *output = parsed;
  return true;
}

}  // namespace

std::optional<Semver3> ParseSemver3(std::string_view value) {
  std::array<int, 3> parts{};
  std::size_t begin = 0;

  for (std::size_t index = 0; index < parts.size(); ++index) {
    const auto delimiter = value.find('.', begin);
    if (index + 1 == parts.size()) {
      if (delimiter != std::string_view::npos) {
        return std::nullopt;
      }
    } else if (delimiter == std::string_view::npos) {
      return std::nullopt;
    }

    const auto token = value.substr(begin, delimiter == std::string_view::npos
                                               ? std::string_view::npos
                                               : delimiter - begin);
    if (!ParseSemverPart(token, &parts[index])) {
      return std::nullopt;
    }

    if (delimiter == std::string_view::npos) {
      begin = value.size();
    } else {
      begin = delimiter + 1;
    }
  }

  if (begin != value.size()) {
    return std::nullopt;
  }

  return Semver3{
      .major = parts[0],
      .minor = parts[1],
      .patch = parts[2],
  };
}

}  // namespace zfleet::core
