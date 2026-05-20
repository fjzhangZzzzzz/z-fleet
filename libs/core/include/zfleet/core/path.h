#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace zfleet::core {

constexpr std::string_view kSafePathSegmentDescription =
    "non-empty single segment using only letters, digits, '.', '_', or '-'";

struct ValidatedString {
  bool ok;
  std::string value;
  std::string message;
};

bool HasWindowsDrivePrefix(std::string_view value) noexcept;
bool IsSafePathSegment(std::string_view value) noexcept;
ValidatedString ValidatePathSegment(std::string_view value);
std::string NormalizePathSeparators(std::string_view value);
std::optional<std::string> NormalizeSafeRelativePath(std::string_view value);
ValidatedString ValidateRelativePath(std::string_view value);
bool IsSafeRelativePath(std::string_view value);

}  // namespace zfleet::core
