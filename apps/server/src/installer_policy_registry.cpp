#include "installer_policy_registry.h"

#include <algorithm>
#include <vector>

#include "zfleet/package/manifest.h"

namespace zfleet::server {
namespace {

std::vector<int> ParseVersion(std::string_view version) {
  std::vector<int> parts;
  std::size_t begin = 0;
  while (begin <= version.size()) {
    const auto end = version.find('.', begin);
    const auto token = version.substr(begin, end == std::string_view::npos
                                                 ? std::string_view::npos
                                                 : end - begin);
    parts.push_back(token.empty() ? 0 : std::stoi(std::string(token)));
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  return parts;
}

bool VersionAtLeast(std::string_view actual, std::string_view minimum) {
  const auto actual_parts = ParseVersion(actual);
  const auto minimum_parts = ParseVersion(minimum);
  const auto count = std::max(actual_parts.size(), minimum_parts.size());
  for (std::size_t index = 0; index < count; ++index) {
    const auto left = index < actual_parts.size() ? actual_parts[index] : 0;
    const auto right = index < minimum_parts.size() ? minimum_parts[index] : 0;
    if (left != right) {
      return left > right;
    }
  }
  return true;
}

}  // namespace

std::optional<AgentPackageRecord> FindCompatibleInstallerPackage(
    ServerDatabase* database, const AgentPackageRecord& package,
    std::string_view min_installer_version) {
  if (min_installer_version.empty()) {
    return std::nullopt;
  }

  std::optional<AgentPackageRecord> installer;
  for (const auto& candidate : database->ListAgentPackages()) {
    if (candidate.component != "installer" || candidate.status != "published" ||
        candidate.platform != package.platform ||
        candidate.arch != package.arch ||
        candidate.build_type != package.build_type ||
        !VersionAtLeast(candidate.version, min_installer_version)) {
      continue;
    }
    if (!installer.has_value() ||
        VersionAtLeast(candidate.version, installer->version)) {
      installer = candidate;
    }
  }
  return installer;
}

}  // namespace zfleet::server
