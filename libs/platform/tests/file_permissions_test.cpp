#include "zfleet/platform/file_permissions.h"

#include "test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace fs = std::filesystem;

TEST_CASE("platform file permissions update executable bit") {
  const zfleet::test::ScopedTestDir test_dir("platform");
  const auto file_path = test_dir / "tool";
  zfleet::test::WriteTextFile(file_path, "binary");

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
}
