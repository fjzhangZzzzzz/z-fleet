#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace zfleet::test {

std::uint64_t CurrentProcessId();

class ScopedTestDir {
 public:
  explicit ScopedTestDir(std::string_view suite_name);
  ~ScopedTestDir();

  ScopedTestDir(const ScopedTestDir&) = delete;
  ScopedTestDir& operator=(const ScopedTestDir&) = delete;
  ScopedTestDir(ScopedTestDir&&) = delete;
  ScopedTestDir& operator=(ScopedTestDir&&) = delete;

  const std::filesystem::path& path() const noexcept;
  operator const std::filesystem::path&() const noexcept;

 private:
  std::filesystem::path path_;
};

std::filesystem::path operator/(const ScopedTestDir& directory,
                                const std::filesystem::path& child);

void WriteTextFile(const std::filesystem::path& path,
                   std::string_view content);
std::string ReadTextFile(const std::filesystem::path& path);

}  // namespace zfleet::test
