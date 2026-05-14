#include "zfleet/platform/system.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

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

std::string architecture_name() {
#if defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#elif defined(__arm__) || defined(_M_ARM)
  return "arm";
#else
  return "unknown";
#endif
}

std::string hostname() {
  char buffer[256] = {};

#if defined(_WIN32)
  DWORD size = static_cast<DWORD>(std::size(buffer));
  if (GetComputerNameA(buffer, &size) != 0) {
    return std::string(buffer, size);
  }
#else
  if (gethostname(buffer, sizeof(buffer)) == 0) {
    buffer[sizeof(buffer) - 1] = '\0';
    return buffer;
  }
#endif

  return "unknown";
}

} // namespace zfleet::platform
