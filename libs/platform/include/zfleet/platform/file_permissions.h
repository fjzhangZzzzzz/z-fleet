#pragma once

#include <filesystem>

namespace zfleet::platform {

bool HasExecutablePermission(std::filesystem::perms permissions) noexcept;
bool IsExecutableFile(const std::filesystem::path& path);
void SetExecutable(const std::filesystem::path& path, bool executable);

}  // namespace zfleet::platform
