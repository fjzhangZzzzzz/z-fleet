#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace zfleet::installer {

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

Manifest LoadManifest(const std::filesystem::path& manifest_path);
std::string ComputeSha256Hex(const std::filesystem::path& path);
bool IsExecutable(const std::filesystem::path& path);
void SetExecutable(const std::filesystem::path& path, bool executable);

} // namespace zfleet::installer
