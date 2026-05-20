#pragma once

#include <filesystem>
#include <string_view>

namespace zfleet::package {

class ScopedTempDir {
 public:
  explicit ScopedTempDir(std::string_view prefix);
  ~ScopedTempDir();

  ScopedTempDir(const ScopedTempDir&) = delete;
  ScopedTempDir& operator=(const ScopedTempDir&) = delete;
  ScopedTempDir(ScopedTempDir&&) = delete;
  ScopedTempDir& operator=(ScopedTempDir&&) = delete;

  const std::filesystem::path& path() const noexcept;

 private:
  std::filesystem::path base_dir_;
  std::filesystem::path path_;
};

}  // namespace zfleet::package
