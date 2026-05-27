#include "package_component_resolver.h"

namespace zfleet::server {
namespace {

constexpr std::string_view kSupportedPackageComponents[] = {"agent",
                                                            "installer"};

}  // namespace

bool IsSupportedPackageComponent(std::string_view component) noexcept {
  for (const auto supported : kSupportedPackageComponents) {
    if (component == supported) {
      return true;
    }
  }
  return false;
}

}  // namespace zfleet::server
