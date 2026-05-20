#include "json_codec.h"

#include "zfleet/core/component.h"
#include "zfleet/core/path.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zfleet::installer {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

template <typename T>
T Required(const json& value, const char* key) {
  return value.at(key).get<T>();
}

int RequiredInt(const json& value, const char* key) {
  const auto& field = value.at(key);
  if (!field.is_number_integer()) {
    throw std::invalid_argument(std::string(key) + " must be an integer");
  }

  return field.get<int>();
}

std::uint64_t RequiredSize(const json& value, const char* key) {
  const auto& field = value.at(key);
  if (!field.is_number_integer()) {
    throw std::invalid_argument(std::string(key) + " must be an integer");
  }

  const auto parsed = field.get<long long>();
  if (parsed < 0) {
    throw std::invalid_argument(std::string(key) + " must be non-negative");
  }

  return static_cast<std::uint64_t>(parsed);
}

bool IsLowerHexSha256(std::string_view value) {
  if (value.size() != 64) {
    return false;
  }

  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isdigit(ch) || (ch >= 'a' && ch <= 'f');
  });
}

std::string ValidateRelativePath(std::string_view raw_path,
                                 const char* field_name) {
  const auto normalized = zfleet::core::ValidateRelativePath(raw_path);
  if (!normalized.ok) {
    throw std::invalid_argument(std::string(field_name) + " " +
                                normalized.message);
  }
  return normalized.value;
}

std::string ValidateVersion(std::string_view version) {
  const auto validation = zfleet::core::ValidatePathSegment(version);
  if (!validation.ok) {
    throw std::invalid_argument("version " + validation.message);
  }
  return validation.value;
}

} // namespace

Manifest ParseManifestJson(std::string_view manifest_json) {
  const auto parsed = json::parse(manifest_json);
  if (!parsed.is_object()) {
    throw std::invalid_argument("manifest must be a JSON object");
  }

  Manifest manifest{
      .schema_version = RequiredInt(parsed, "schema_version"),
      .component = Required<std::string>(parsed, "component"),
      .version = Required<std::string>(parsed, "version"),
      .min_installer_version =
          Required<std::string>(parsed, "min_installer_version"),
      .files = {},
  };

  if (manifest.schema_version != 1) {
    throw std::invalid_argument("schema_version must be 1");
  }
  if (manifest.min_installer_version.empty()) {
    throw std::invalid_argument("min_installer_version must not be empty");
  }
  manifest.version = ValidateVersion(manifest.version);

  const auto component_validation =
      zfleet::core::ValidateComponent(manifest.component);
  if (!component_validation.ok) {
    throw std::invalid_argument("component " + component_validation.message);
  }

  if (!parsed.contains("files") || !parsed.at("files").is_array()) {
    throw std::invalid_argument("files must be an array");
  }
  if (parsed.contains("signatures") && !parsed.at("signatures").is_array()) {
    throw std::invalid_argument("signatures must be an array");
  }

  std::unordered_set<std::string> targets;
  for (const auto& file_json : parsed.at("files")) {
    ManifestFile file{
        .source = ValidateRelativePath(Required<std::string>(file_json, "source"),
                                       "source"),
        .target = ValidateRelativePath(Required<std::string>(file_json, "target"),
                                       "target"),
        .size = RequiredSize(file_json, "size"),
        .sha256 = Required<std::string>(file_json, "sha256"),
        .executable = Required<bool>(file_json, "executable"),
    };

    if (!file.source.starts_with("payload/")) {
      throw std::invalid_argument("source must start with payload/");
    }
    if (file.target == "META" || file.target.starts_with("META/")) {
      throw std::invalid_argument("target must not write under META/");
    }
    if (!IsLowerHexSha256(file.sha256)) {
      throw std::invalid_argument("sha256 must be 64 lowercase hex chars");
    }
    if (!targets.insert(file.target).second) {
      throw std::invalid_argument("duplicate target: " + file.target);
    }

    manifest.files.push_back(std::move(file));
  }

  return manifest;
}

std::string SerializeStatusResult(const StatusResult& result) {
  json payload = {
      {"component", result.component},
      {"state", result.state},
  };
  if (result.version.has_value()) {
    payload["version"] = *result.version;
  }
  if (result.message.has_value()) {
    payload["message"] = *result.message;
  }
  return payload.dump();
}

} // namespace zfleet::installer
