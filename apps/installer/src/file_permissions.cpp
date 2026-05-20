#include "file_permissions.h"

namespace zfleet::installer {

namespace fs = std::filesystem;

bool IsExecutable(const fs::path& path) {
  const auto permissions = fs::status(path).permissions();
  return (permissions & fs::perms::owner_exec) != fs::perms::none ||
         (permissions & fs::perms::group_exec) != fs::perms::none ||
         (permissions & fs::perms::others_exec) != fs::perms::none;
}

void SetExecutable(const fs::path& path, bool executable) {
  const auto mask = fs::perms::owner_exec | fs::perms::group_exec |
                    fs::perms::others_exec;
  fs::permissions(path, mask,
                  executable ? fs::perm_options::add
                             : fs::perm_options::remove);
}

}  // namespace zfleet::installer
