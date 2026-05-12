#include "zfleet/platform/system.h"

namespace zfleet::platform {

std::string_view os_name() noexcept {
#if defined(_WIN32)
  return "windows";
#elif defined(__linux__)
  return "linux";
#elif defined(__APPLE__)
  return "macos";
#else
  return "unknown";
#endif
}

} // namespace zfleet::platform
