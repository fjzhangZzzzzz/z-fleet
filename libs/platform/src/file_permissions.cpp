#include "zfleet/platform/file_permissions.h"

#include <stdexcept>
#include <string>

namespace zfleet::platform {

namespace fs = std::filesystem;

bool HasExecutablePermission(fs::perms permissions) noexcept {
#ifdef _WIN32
  (void)permissions;
  return false;
#else
  return (permissions & fs::perms::owner_exec) != fs::perms::none ||
         (permissions & fs::perms::group_exec) != fs::perms::none ||
         (permissions & fs::perms::others_exec) != fs::perms::none;
#endif
}

bool IsLaunchableProgram(const fs::path& path) {
  const auto target_status = fs::symlink_status(path);
  if (!fs::exists(target_status) || !fs::is_regular_file(target_status)) {
    return false;
  }

#ifdef _WIN32
  const auto extension = path.extension().string();
  return extension == ".exe" || extension == ".EXE";
#else
  return HasExecutablePermission(fs::status(path).permissions());
#endif
}

void SetExecutable(const fs::path& path, bool executable) {
#ifdef _WIN32
  (void)path;
  (void)executable;
#else
  const auto mask = fs::perms::owner_exec | fs::perms::group_exec |
                    fs::perms::others_exec;
  std::error_code error;
  fs::permissions(path, mask,
                  executable ? fs::perm_options::add
                             : fs::perm_options::remove,
                  error);
  if (error) {
    throw std::runtime_error("failed to update executable permission for: " +
                             path.string());
  }
#endif
}

}  // namespace zfleet::platform
