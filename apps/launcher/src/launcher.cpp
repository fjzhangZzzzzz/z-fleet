#include "launcher.h"

#include <cerrno>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace zfleet::launcher {
namespace {

namespace fs = std::filesystem;

std::string ReadFileTrimmed(const fs::path& path) {
  std::ifstream stream(path);
  if (!stream) {
    throw std::runtime_error("failed to open file: " + path.string());
  }

  std::string value{std::istreambuf_iterator<char>(stream),
                    std::istreambuf_iterator<char>()};
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  return value;
}

bool IsSafeVersionString(const std::string& version) {
  return !version.empty() && version != "." && version != ".." &&
         version.find('/') == std::string::npos &&
         version.find('\\') == std::string::npos;
}

bool EndsWithCaseInsensitive(std::string_view value, std::string_view suffix) {
  if (value.size() < suffix.size()) {
    return false;
  }

  const auto offset = value.size() - suffix.size();
  for (std::size_t index = 0; index < suffix.size(); ++index) {
    const auto left = static_cast<unsigned char>(value[offset + index]);
    const auto right = static_cast<unsigned char>(suffix[index]);
    if (std::tolower(left) != std::tolower(right)) {
      return false;
    }
  }
  return true;
}

std::string ComponentLookupName(const std::string& executable_name) {
#ifdef _WIN32
  if (EndsWithCaseInsensitive(executable_name, ".exe")) {
    return executable_name.substr(0, executable_name.size() - 4);
  }
#endif
  return executable_name;
}

bool IsExecutableFile(const fs::path& executable_path) {
  const auto target_status = fs::symlink_status(executable_path);
  if (!fs::exists(target_status) || !fs::is_regular_file(target_status)) {
    return false;
  }

#ifdef _WIN32
  return true;
#else
  const auto permissions = fs::status(executable_path).permissions();
  return (permissions & fs::perms::owner_exec) != fs::perms::none ||
         (permissions & fs::perms::group_exec) != fs::perms::none ||
         (permissions & fs::perms::others_exec) != fs::perms::none;
#endif
}

std::optional<std::string> ComponentForExecutableName(
    const std::string& executable_name) {
  const auto lookup_name = ComponentLookupName(executable_name);
  if (lookup_name == "zfleet_agent") {
    return "agent";
  }
  if (lookup_name == "zfleet_server") {
    return "server";
  }
  if (lookup_name == "zfleet_installer") {
    return "installer";
  }
  return std::nullopt;
}

} // namespace

ResolveResult ResolveLaunchTarget(const fs::path& launcher_path) {
  const auto executable_name = launcher_path.filename().string();
  const auto component = ComponentForExecutableName(executable_name);
  if (!component.has_value()) {
    return ResolveResult{
        .ok = false,
        .target = {},
        .message = "unknown launcher executable: " + executable_name,
    };
  }

  const auto bin_dir = launcher_path.parent_path();
  const auto component_root = bin_dir.parent_path();
  if (bin_dir.filename() != "bin" || component_root.filename() != *component ||
      component_root.parent_path().filename() != "zfleet") {
    return ResolveResult{
        .ok = false,
        .target = {},
        .message = "launcher path must match <root>/zfleet/" + *component +
                   "/bin/" + executable_name,
    };
  }

  const auto active_version_path = component_root / "var" / "active-version";
  if (!fs::exists(active_version_path)) {
    return ResolveResult{
        .ok = false,
        .target = {},
        .message = "active-version is missing",
    };
  }

  std::string version;
  try {
    version = ReadFileTrimmed(active_version_path);
  } catch (const std::exception& ex) {
    return ResolveResult{
        .ok = false,
        .target = {},
        .message = ex.what(),
    };
  }

  if (!IsSafeVersionString(version)) {
    return ResolveResult{
        .ok = false,
        .target = {},
        .message = "active-version is invalid",
    };
  }

  const auto executable_path =
      component_root / "releases" / version / "bin" / executable_name;
  if (!IsExecutableFile(executable_path)) {
    return ResolveResult{
        .ok = false,
        .target = {},
        .message = "target executable is missing or not executable",
    };
  }

  return ResolveResult{
      .ok = true,
      .target =
          LaunchTarget{
              .component = *component,
              .component_root = component_root,
              .active_version_path = active_version_path,
              .version = version,
              .executable_path = executable_path,
          },
      .message = {},
  };
}

std::vector<std::string> BuildForwardedArgv(const fs::path& executable_path,
                                            int argc,
                                            char* const argv[]) {
  std::vector<std::string> forwarded;
  forwarded.reserve(argc > 0 ? static_cast<std::size_t>(argc) : 1U);
  forwarded.push_back(executable_path.string());
  for (int index = 1; index < argc; ++index) {
    forwarded.emplace_back(argv[index] == nullptr ? "" : argv[index]);
  }
  return forwarded;
}

int RunLauncher(const fs::path& launcher_path, int argc, char* argv[]) {
  const auto resolved = ResolveLaunchTarget(launcher_path);
  if (!resolved.ok) {
    std::cerr << resolved.message << '\n';
    return 1;
  }

  auto forwarded = BuildForwardedArgv(resolved.target.executable_path, argc, argv);

#ifdef _WIN32
  std::vector<char*> raw_args;
  raw_args.reserve(forwarded.size() + 1);
  for (auto& value : forwarded) {
    raw_args.push_back(value.data());
  }
  raw_args.push_back(nullptr);

  const auto exit_code = _spawnv(_P_WAIT, forwarded.front().c_str(),
                                 raw_args.data());
  if (exit_code == -1) {
    std::cerr << "failed to spawn target: " << std::strerror(errno) << '\n';
    return 1;
  }
  return exit_code;
#else
  std::vector<char*> raw_args;
  raw_args.reserve(forwarded.size() + 1);
  for (auto& value : forwarded) {
    raw_args.push_back(value.data());
  }
  raw_args.push_back(nullptr);

  execv(forwarded.front().c_str(), raw_args.data());
  std::cerr << "failed to exec target: " << std::strerror(errno) << '\n';
  return 1;
#endif
}

} // namespace zfleet::launcher
