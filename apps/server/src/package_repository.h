#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace zfleet::server {

struct AgentPackageMetadata {
  std::string component;
  std::string version;
  std::string min_installer_version;
  std::uint64_t size_bytes = 0;
  std::string sha256;
  std::string manifest_json;
};

AgentPackageMetadata ValidateAgentPackageUpload(
    const std::filesystem::path& staged_package_path);

}  // namespace zfleet::server
