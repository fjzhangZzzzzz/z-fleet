#include "zfleet/core/version.h"

namespace zfleet::core {

std::string_view project_name() noexcept {
  return "z-fleet";
}

std::string_view version() noexcept {
  return "0.1.0";
}

} // namespace zfleet::core
