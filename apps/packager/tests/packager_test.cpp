#include "packager.h"

#include "test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <zfleet/crypto/sha256.h>
#include <zfleet/package/archive.h>
#include <zfleet/package/manifest.h>
#include <zfleet/platform/file_permissions.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

void ExtractArchiveTo(const fs::path& archive_path, const fs::path& output_dir) {
  zfleet::package::ExtractArchive(
      {.archive_path = archive_path, .output_dir = output_dir, .force = true});
}

struct ExpectedFile {
  std::string relative_path;
  fs::path path;
  bool executable = false;
};

std::string ExpectedManifestJson(const std::string& component,
                                 const std::string& version,
                                 const std::string& min_installer_version,
                                 const std::vector<ExpectedFile>& files) {
  zfleet::package::Manifest manifest{
      .schema_version = 1,
      .component = component,
      .version = version,
      .min_installer_version = min_installer_version,
      .files = {},
  };
  manifest.files.reserve(files.size());
  for (const auto& file : files) {
    manifest.files.push_back(zfleet::package::ManifestFile{
        .source = "payload/" + file.relative_path,
        .target = file.relative_path,
        .size = static_cast<std::uint64_t>(fs::file_size(file.path)),
        .sha256 = zfleet::crypto::Sha256FileHex(file.path),
        .executable = file.executable,
    });
  }
  return zfleet::package::SerializeManifestJson(manifest);
}

} // namespace

TEST_CASE("pack creates package layout from payload directory") {
  const zfleet::test::ScopedTestDir test_dir("packager");
  const auto test_root = test_dir.path();

  const auto payload_dir = test_root / "payload-src";
  const auto binary_path = payload_dir / "bin" / "zfleet_agent";
  const auto library_path = payload_dir / "lib" / "libagent_support.so";
  const auto config_path = payload_dir / "share" / "agent.conf";
  zfleet::test::WriteTextFile(binary_path, "agent-binary");
  zfleet::test::WriteTextFile(library_path, "library");
  zfleet::test::WriteTextFile(config_path, "config");
  zfleet::platform::SetExecutable(binary_path, true);

  const auto output_dir = test_root / "packages";
  const auto result = zfleet::packager::Pack(zfleet::packager::PackOptions{
      .component = "agent",
      .version = "1.2.3",
      .payload_dir = payload_dir,
      .entry_path = "bin/zfleet_agent",
      .output_dir = output_dir,
      .force = false,
  });

  const auto package_dir =
      fs::absolute(output_dir).lexically_normal() / "agent" / "1.2.3";
  REQUIRE_FALSE(result.archive);
  REQUIRE(result.package_path == package_dir);
  REQUIRE(zfleet::test::ReadTextFile(package_dir / "payload" / "bin" /
                                     "zfleet_agent") ==
          "agent-binary");
  REQUIRE(zfleet::test::ReadTextFile(package_dir / "payload" / "lib" /
                                     "libagent_support.so") ==
          "library");
  REQUIRE(zfleet::test::ReadTextFile(package_dir / "payload" / "share" /
                                     "agent.conf") ==
          "config");
  REQUIRE(zfleet::test::ReadTextFile(package_dir / "META" / "manifest.json") ==
          ExpectedManifestJson(
              "agent", "1.2.3", "0.1.0",
              {ExpectedFile{.relative_path = "bin/zfleet_agent",
                            .path = binary_path,
                            .executable = true},
               ExpectedFile{.relative_path = "lib/libagent_support.so",
                            .path = library_path,
                            .executable = false},
               ExpectedFile{.relative_path = "share/agent.conf",
                            .path = config_path,
                            .executable = false}}));
#ifndef _WIN32
  REQUIRE(zfleet::platform::IsExecutableFile(package_dir / "payload" / "bin" /
                                             "zfleet_agent"));
#endif
}

TEST_CASE("pack creates a zip archive from payload directory") {
  const zfleet::test::ScopedTestDir test_dir("packager");
  const auto test_root = test_dir.path();

  const auto payload_dir = test_root / "payload-src";
  const auto binary_path = payload_dir / "bin" / "zfleet_server";
  const auto library_path = payload_dir / "lib" / "libserver_support.so";
  zfleet::test::WriteTextFile(binary_path, "server-binary");
  zfleet::test::WriteTextFile(library_path, "server-library");
  zfleet::platform::SetExecutable(binary_path, true);

  const auto output_dir = test_root / "archives";
  const auto result = zfleet::packager::Pack(zfleet::packager::PackOptions{
      .component = "server",
      .version = "3.4.5",
      .payload_dir = payload_dir,
      .entry_path = "bin/zfleet_server",
      .output_dir = output_dir,
      .archive = true,
      .force = false,
  });

  const auto archive_path =
      fs::absolute(output_dir).lexically_normal() / "server" / "3.4.5.zip";
  REQUIRE(result.archive);
  REQUIRE(result.package_path == archive_path);
  REQUIRE(fs::exists(archive_path));

  const auto extracted_dir = test_root / "extracted";
  ExtractArchiveTo(archive_path, extracted_dir);
  REQUIRE(zfleet::test::ReadTextFile(extracted_dir / "payload" / "bin" /
                                     "zfleet_server") ==
          "server-binary");
  REQUIRE(zfleet::test::ReadTextFile(extracted_dir / "payload" / "lib" /
                                     "libserver_support.so") ==
          "server-library");
  REQUIRE(zfleet::test::ReadTextFile(extracted_dir / "META" / "manifest.json") ==
          ExpectedManifestJson(
              "server", "3.4.5", "0.1.0",
              {ExpectedFile{.relative_path = "bin/zfleet_server",
                            .path = binary_path,
                            .executable = true},
               ExpectedFile{.relative_path = "lib/libserver_support.so",
                            .path = library_path,
                            .executable = false}}));
}

TEST_CASE("pack rejects existing output unless force is set") {
  const zfleet::test::ScopedTestDir test_dir("packager");
  const auto test_root = test_dir.path();

  const auto payload_dir = test_root / "payload-src";
  const auto binary_path = payload_dir / "bin" / "zfleet_installer";
  zfleet::test::WriteTextFile(binary_path, "installer-binary");
  zfleet::platform::SetExecutable(binary_path, true);

  const auto output_dir = test_root / "packages";
  const auto package_dir =
      fs::absolute(output_dir).lexically_normal() / "installer" / "2.0.0";
  fs::create_directories(package_dir / "META");
  zfleet::test::WriteTextFile(package_dir / "META" / "stale.txt", "stale");
  zfleet::test::WriteTextFile(output_dir / "keep.txt", "keep");

  REQUIRE_THROWS_AS(zfleet::packager::Pack(zfleet::packager::PackOptions{
                        .component = "installer",
                        .version = "2.0.0",
                        .payload_dir = payload_dir,
                        .entry_path = "bin/zfleet_installer",
                        .output_dir = output_dir,
                    }),
                    std::runtime_error);

  const auto result = zfleet::packager::Pack(zfleet::packager::PackOptions{
      .component = "installer",
      .version = "2.0.0",
      .payload_dir = payload_dir,
      .entry_path = "bin/zfleet_installer",
      .output_dir = output_dir,
      .force = true,
  });

  REQUIRE(result.package_path == package_dir);
  REQUIRE_FALSE(fs::exists(package_dir / "META" / "stale.txt"));
  REQUIRE(fs::exists(output_dir / "keep.txt"));
  REQUIRE(fs::exists(package_dir / "payload" / "bin" / "zfleet_installer"));
}

TEST_CASE("pack rejects invalid inputs") {
  const zfleet::test::ScopedTestDir test_dir("packager");
  const auto test_root = test_dir.path();

  const auto payload_dir = test_root / "payload-src";
  const auto binary_path = payload_dir / "bin" / "zfleet_agent";
  zfleet::test::WriteTextFile(binary_path, "agent-binary");
  zfleet::platform::SetExecutable(binary_path, true);

  SECTION("invalid component") {
    REQUIRE_THROWS_AS(zfleet::packager::Pack(zfleet::packager::PackOptions{
                          .component = "bad-component",
                          .version = "1.0.0",
                          .payload_dir = payload_dir,
                          .entry_path = "bin/zfleet_agent",
                          .output_dir = test_root / "packages",
                      }),
                      std::invalid_argument);
  }

  SECTION("invalid version") {
    REQUIRE_THROWS_AS(zfleet::packager::Pack(zfleet::packager::PackOptions{
                          .component = "agent",
                          .version = "../bad",
                          .payload_dir = payload_dir,
                          .entry_path = "bin/zfleet_agent",
                          .output_dir = test_root / "packages",
                      }),
                      std::invalid_argument);
  }

  SECTION("unsafe entry path") {
    REQUIRE_THROWS_AS(zfleet::packager::Pack(zfleet::packager::PackOptions{
                          .component = "agent",
                          .version = "1.0.0",
                          .payload_dir = payload_dir,
                          .entry_path = "../zfleet_agent",
                          .output_dir = test_root / "packages",
                      }),
                      std::invalid_argument);
  }

  SECTION("missing entry") {
    REQUIRE_THROWS_AS(zfleet::packager::Pack(zfleet::packager::PackOptions{
                          .component = "agent",
                          .version = "1.0.0",
                          .payload_dir = payload_dir,
                          .entry_path = "bin/missing",
                          .output_dir = test_root / "packages",
                      }),
                      std::runtime_error);
  }

  SECTION("payload target under META") {
    zfleet::test::WriteTextFile(payload_dir / "META" / "bad.txt", "bad");
    REQUIRE_THROWS_AS(zfleet::packager::Pack(zfleet::packager::PackOptions{
                          .component = "agent",
                          .version = "1.0.0",
                          .payload_dir = payload_dir,
                          .entry_path = "bin/zfleet_agent",
                          .output_dir = test_root / "packages",
                      }),
                      std::runtime_error);
  }
}

TEST_CASE("pack rejects symlinks in payload directory") {
  const zfleet::test::ScopedTestDir test_dir("packager");
  const auto test_root = test_dir.path();

  const auto payload_dir = test_root / "payload-src";
  const auto binary_path = payload_dir / "bin" / "zfleet_agent";
  zfleet::test::WriteTextFile(binary_path, "agent-binary");
  zfleet::platform::SetExecutable(binary_path, true);

#ifdef _WIN32
#else
  std::error_code error;
  fs::create_symlink(binary_path, payload_dir / "bin" / "link", error);
  if (error) {
    SUCCEED("filesystem does not permit symlink creation in this environment");
    return;
  }

  REQUIRE_THROWS_AS(zfleet::packager::Pack(zfleet::packager::PackOptions{
                        .component = "agent",
                        .version = "1.0.0",
                        .payload_dir = payload_dir,
                        .entry_path = "bin/zfleet_agent",
                        .output_dir = test_root / "packages",
                    }),
                    std::runtime_error);
#endif
}
