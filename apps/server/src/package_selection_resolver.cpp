#include "package_selection_resolver.h"

namespace zfleet::server {

InstallPackageSelection ResolveInstallPackageSelection(
    ServerDatabase* database, std::string_view component,
    std::string_view channel, std::string_view platform, std::string_view arch,
    std::string_view build_type) {
  InstallPackageSelection selection;
  selection.agent = database->FindDefaultPublishedAgentPackage(
      std::string(component), std::string(channel), std::string(platform),
      std::string(arch), std::string(build_type));
  if (!selection.agent.has_value()) {
    return selection;
  }
  selection.installer = database->FindDefaultPublishedAgentPackage(
      "installer", std::string(channel), std::string(platform),
      std::string(arch), std::string(build_type));
  return selection;
}

}  // namespace zfleet::server
