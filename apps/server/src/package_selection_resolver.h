#pragma once

#include <optional>
#include <string_view>

#include "database.h"

namespace zfleet::server {

struct InstallPackageSelection {
  std::optional<AgentPackageRecord> agent;
  std::optional<AgentPackageRecord> installer;
};

InstallPackageSelection ResolveInstallPackageSelection(
    ServerDatabase* database, std::string_view component,
    std::string_view channel, std::string_view platform, std::string_view arch,
    std::string_view build_type);

}  // namespace zfleet::server
