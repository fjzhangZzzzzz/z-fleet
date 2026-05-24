#include <zfleet/package/manifest.h>

#include <zfleet/core/component.h>
#include <zfleet/core/path.h>
#include <zfleet/crypto/sha256.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace zfleet::package {
namespace {

using nlohmann::json;
using nlohmann::ordered_json;
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

std::string ReadFile(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("failed to open file: " + path.string());
  }

  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
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

std::string ValidatePackageTarget(std::string_view value, const char* field_name) {
  const auto validation = zfleet::core::ValidatePathSegment(value);
  if (!validation.ok) {
    throw std::invalid_argument(std::string(field_name) + " " +
                                validation.message);
  }
  return validation.value;
}

}  // namespace

Manifest ParseManifestJson(std::string_view manifest_json) {
  const auto parsed = json::parse(manifest_json);
  if (!parsed.is_object()) {
    throw std::invalid_argument("manifest must be a JSON object");
  }

  Manifest manifest{
      .schema_version = RequiredInt(parsed, "schema_version"),
      .component = Required<std::string>(parsed, "component"),
      .version = Required<std::string>(parsed, "version"),
      .platform = Required<std::string>(parsed, "platform"),
      .arch = Required<std::string>(parsed, "arch"),
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
  manifest.platform = ValidatePackageTarget(manifest.platform, "platform");
  manifest.arch = ValidatePackageTarget(manifest.arch, "arch");

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
    if (!zfleet::crypto::IsLowerHexSha256(file.sha256)) {
      throw std::invalid_argument("sha256 must be 64 lowercase hex chars");
    }
    if (!targets.insert(file.target).second) {
      throw std::invalid_argument("duplicate target: " + file.target);
    }

    manifest.files.push_back(std::move(file));
  }

  return manifest;
}

std::string SerializeManifestJson(const Manifest& manifest) {
  ordered_json payload;
  payload["schema_version"] = manifest.schema_version;
  payload["component"] = manifest.component;
  payload["version"] = manifest.version;
  payload["platform"] = manifest.platform;
  payload["arch"] = manifest.arch;
  payload["min_installer_version"] = manifest.min_installer_version;

  ordered_json files = ordered_json::array();
  for (const auto& file : manifest.files) {
    ordered_json file_json;
    file_json["source"] = file.source;
    file_json["target"] = file.target;
    file_json["size"] = file.size;
    file_json["sha256"] = file.sha256;
    file_json["executable"] = file.executable;
    files.push_back(std::move(file_json));
  }
  payload["files"] = std::move(files);
  payload["signatures"] = ordered_json::array();

  return payload.dump(2) + "\n";
}

Manifest LoadManifest(const fs::path& manifest_path) {
  return ParseManifestJson(ReadFile(manifest_path));
}

}  // namespace zfleet::package
