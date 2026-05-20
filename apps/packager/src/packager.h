#pragma once

#include <filesystem>
#include <string>

namespace zfleet::packager {

struct PackOptions {
  std::string component;
  std::string version;
  std::filesystem::path payload_dir;
  std::filesystem::path entry_path;
  std::filesystem::path output_dir;
  std::string min_installer_version = "0.1.0";
  bool archive = false;
  bool force = false;
};

struct PackResult {
  std::filesystem::path package_path;
  bool archive = false;
};

PackResult Pack(const PackOptions& options);

} // namespace zfleet::packager
