#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace zfleet::package {

struct ManifestFile {
  std::string source;
  std::string target;
  std::uint64_t size;
  std::string sha256;
  bool executable;
};

struct Manifest {
  int schema_version;
  std::string component;
  std::string version;
  std::string min_installer_version;
  std::vector<ManifestFile> files;
};

Manifest ParseManifestJson(std::string_view manifest_json);
std::string SerializeManifestJson(const Manifest& manifest);
Manifest LoadManifest(const std::filesystem::path& manifest_path);

}  // namespace zfleet::package
