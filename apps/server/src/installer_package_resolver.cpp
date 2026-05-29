#include "installer_package_resolver.h"

#include "zfleet/core/version_compare.h"
#include "zfleet/package/manifest.h"

namespace zfleet::server {
namespace {

}  // namespace

std::optional<AgentPackageRecord> FindCompatibleInstallerPackage(
    ServerDatabase* database, const AgentPackageRecord& package,
    std::string_view min_installer_version) {
  if (min_installer_version.empty()) {
    return std::nullopt;
  }

  const auto minimum_version = zfleet::core::ParseSemver3(min_installer_version);
  if (!minimum_version.has_value()) {
    return std::nullopt;
  }

  std::optional<AgentPackageRecord> installer;
  for (const auto& candidate : database->ListAgentPackages()) {
    const auto candidate_version = zfleet::core::ParseSemver3(candidate.version);
    if (candidate.component != "installer" || candidate.status != "published" ||
        candidate.platform != package.platform ||
        candidate.arch != package.arch ||
        candidate.build_type != package.build_type ||
        !candidate_version.has_value() ||
        *candidate_version < *minimum_version) {
      continue;
    }
    if (!installer.has_value()) {
      installer = candidate;
      continue;
    }

    const auto installer_version =
        zfleet::core::ParseSemver3(installer->version);
    if (!installer_version.has_value() ||
        *candidate_version >= *installer_version) {
      installer = candidate;
    }
  }
  return installer;
}

}  // namespace zfleet::server
