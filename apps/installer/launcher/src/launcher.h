#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace zfleet::launcher {

constexpr const char* kComponentRootEnvVar = "ZFLEET_COMPONENT_ROOT";

struct LaunchTarget {
  std::string component;
  std::filesystem::path component_root;
  std::filesystem::path active_version_path;
  std::string version;
  std::filesystem::path executable_path;
};

struct ResolveResult {
  bool ok;
  LaunchTarget target;
  std::string message;
};

ResolveResult ResolveLaunchTarget(const std::filesystem::path& launcher_path);
std::vector<std::string> BuildForwardedArgv(
    const std::filesystem::path& executable_path, int argc, char* const argv[]);
int RunLauncher(const std::filesystem::path& launcher_path, int argc,
                char* argv[]);

}  // namespace zfleet::launcher
