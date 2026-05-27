#pragma once

#include <optional>
#include <string_view>

#include "database.h"

namespace zfleet::server {

std::optional<AgentPackageRecord> FindCompatibleInstallerPackage(
    ServerDatabase* database, const AgentPackageRecord& package,
    std::string_view min_installer_version);

}  // namespace zfleet::server
