#include "zfleet/platform/file_permissions.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

TEST_CASE("platform file permissions update executable bit") {
  const auto test_dir =
      fs::temp_directory_path() / "zfleet-platform-file-permissions-test";
  fs::remove_all(test_dir);
  fs::create_directories(test_dir);
  struct Cleanup {
    fs::path path;
    ~Cleanup() { fs::remove_all(path); }
  } cleanup{test_dir};

  const auto file_path = test_dir / "tool";
  std::ofstream stream(file_path, std::ios::binary);
  stream << "binary";
  stream.close();
  REQUIRE(stream);

  REQUIRE_FALSE(zfleet::platform::IsExecutableFile(test_dir / "missing"));

#ifdef _WIN32
  REQUIRE(zfleet::platform::IsExecutableFile(file_path));
#else
  zfleet::platform::SetExecutable(file_path, false);
  REQUIRE_FALSE(zfleet::platform::HasExecutablePermission(
      fs::status(file_path).permissions()));

  zfleet::platform::SetExecutable(file_path, true);
  REQUIRE(zfleet::platform::HasExecutablePermission(
      fs::status(file_path).permissions()));
  REQUIRE(zfleet::platform::IsExecutableFile(file_path));
#endif

  (void)cleanup;
}
