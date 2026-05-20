#pragma once

#include <filesystem>

namespace zfleet::installer {

bool IsExecutable(const std::filesystem::path& path);
void SetExecutable(const std::filesystem::path& path, bool executable);

}  // namespace zfleet::installer
