#include "zfleet/core/path.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <vector>

namespace zfleet::core {

bool HasWindowsDrivePrefix(std::string_view value) noexcept {
  return value.size() >= 2 &&
         std::isalpha(static_cast<unsigned char>(value[0])) != 0 &&
         value[1] == ':';
}

bool IsSafePathSegment(std::string_view value) noexcept {
  if (value.empty() || value == "." || value == "..") {
    return false;
  }

  for (const unsigned char ch : value) {
    if (!(std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-')) {
      return false;
    }
  }
  return true;
}

ValidatedString ValidatePathSegment(std::string_view value) {
  if (value.empty()) {
    return ValidatedString{.ok = false,
                           .value = {},
                           .message = "must not be empty"};
  }
  if (value == "." || value == "..") {
    return ValidatedString{.ok = false,
                           .value = {},
                           .message = "must not be '.' or '..'"};
  }
  if (!IsSafePathSegment(value)) {
    return ValidatedString{
        .ok = false,
        .value = {},
        .message = "expected " + std::string(kSafePathSegmentDescription),
    };
  }
  return ValidatedString{.ok = true, .value = std::string(value), .message = {}};
}

std::string NormalizePathSeparators(std::string_view value) {
  std::string normalized(value);
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  return normalized;
}

std::optional<std::string> NormalizeSafeRelativePath(std::string_view value) {
  const auto validation = ValidateRelativePath(value);
  if (!validation.ok) {
    return std::nullopt;
  }
  return validation.value;
}

ValidatedString ValidateRelativePath(std::string_view value) {
  const auto normalized = NormalizePathSeparators(value);
  if (normalized.empty()) {
    return ValidatedString{.ok = false,
                           .value = {},
                           .message = "must not be empty"};
  }
  if (normalized == "." || normalized == "..") {
    return ValidatedString{.ok = false,
                           .value = {},
                           .message = "must not be '.' or '..'"};
  }
  if (normalized.starts_with('/') || normalized.starts_with("//")) {
    return ValidatedString{.ok = false,
                           .value = {},
                           .message = "must be a relative path"};
  }
  if (HasWindowsDrivePrefix(normalized)) {
    return ValidatedString{.ok = false,
                           .value = {},
                           .message = "must not use a Windows drive path"};
  }

  const std::filesystem::path path(normalized);
  if (path.is_absolute()) {
    return ValidatedString{.ok = false,
                           .value = {},
                           .message = "must be a relative path"};
  }

  std::vector<std::string> components;
  for (const auto& part : path) {
    const auto piece = part.generic_string();
    if (piece.empty()) {
      continue;
    }
    if (piece == "." || piece == "..") {
      return ValidatedString{.ok = false,
                             .value = {},
                             .message = "must not contain '.' or '..'"};
    }
    components.push_back(piece);
  }

  if (components.empty()) {
    return ValidatedString{.ok = false,
                           .value = {},
                           .message = "must not be empty"};
  }

  std::string collapsed = components.front();
  for (std::size_t index = 1; index < components.size(); ++index) {
    collapsed += "/";
    collapsed += components[index];
  }
  return ValidatedString{.ok = true, .value = collapsed, .message = {}};
}

bool IsSafeRelativePath(std::string_view value) {
  return NormalizeSafeRelativePath(value).has_value();
}

}  // namespace zfleet::core
