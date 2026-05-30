#include "installer.h"

#include "test_util.h"

#include <zfleet/core/component.h>
#include <zfleet/crypto/sha256.h>
#include <zfleet/package/archive.h>
#include <zfleet/package/manifest.h>
#include <zfleet/platform/file_permissions.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct PackageFileSpec {
  std::string source_relative_path;
  std::string target;
  std::string content;
  bool launchable = false;
};

std::string BuildManifestJson(
    const std::string& component, const std::string& version,
    const std::vector<zfleet::package::ManifestFile>& files,
    const std::string& min_installer_version = "0.1.0") {
  return zfleet::package::SerializeManifestJson(zfleet::package::Manifest{
      .schema_version = 2,
      .component = component,
      .version = version,
      .platform = "linux",
      .arch = "x86_64",
      .build_type = "debug",
      .min_installer_version = min_installer_version,
      .files = files,
  });
}

fs::path CreatePackageArchive(
    const fs::path& root, const std::string& component,
    const std::string& version, const std::vector<PackageFileSpec>& files,
    const std::string& min_installer_version = "0.1.0") {
  const auto package_dir = root / "package";
  fs::create_directories(package_dir / "META");

  std::vector<zfleet::package::ManifestFile> manifest_files;
  for (const auto& file : files) {
    const auto payload_path =
        package_dir / "payload" / file.source_relative_path;
    zfleet::test::WriteTextFile(payload_path, file.content);
    zfleet::platform::SetExecutable(payload_path, file.launchable);

    manifest_files.push_back(zfleet::package::ManifestFile{
        .source = "payload/" + file.source_relative_path,
        .target = file.target,
        .size = static_cast<std::uint64_t>(fs::file_size(payload_path)),
        .sha256 = zfleet::crypto::Sha256FileHex(payload_path),
        .launchable = file.launchable,
    });
  }

  zfleet::test::WriteTextFile(
      package_dir / "META" / "manifest.json",
      BuildManifestJson(component, version, manifest_files,
                        min_installer_version));
  const auto archive_path = root / (component + "-" + version + ".zip");
  zfleet::package::CreateArchive({package_dir, archive_path, true});
  return archive_path;
}

fs::path CreateInstallerArchive(const fs::path& root,
                                const std::string& version,
                                const std::string& launcher_tag) {
  const auto installer_binary_name =
      zfleet::core::BinaryNameForComponent("installer");
  const auto launcher_binary_name = zfleet::core::BinaryNameForArtifact(
      zfleet::core::BinaryArtifact::kLauncher);
  return CreatePackageArchive(
      root, "installer", version,
      {
          PackageFileSpec{
              .source_relative_path = "bin/" + installer_binary_name,
              .target = "bin/" + installer_binary_name,
              .content = "installer-binary-" + version,
              .launchable = true},
          PackageFileSpec{.source_relative_path = "bin/" + launcher_binary_name,
                          .target = "bin/" + launcher_binary_name,
                          .content = launcher_tag + "-template-" + version,
                          .launchable = true},
      });
}

fs::path CreateComponentArchive(
    const fs::path& root, const std::string& component,
    const std::string& version,
    const std::string& min_installer_version = "0.1.0") {
  return CreatePackageArchive(
      root, component, version,
      {PackageFileSpec{
          .source_relative_path = "bin/zfleet_" + component,
          .target = "bin/zfleet_" + component,
          .content = component + "-binary-" + version,
          .launchable = true,
      }},
      min_installer_version);
}

std::string ReadFile(const fs::path& path) {
  return zfleet::test::ReadTextFile(path);
}

fs::path StubPath(const fs::path& root, const std::string& component) {
  return root / component / "bin" /
         zfleet::core::BinaryNameForComponent(component);
}

}  // namespace

TEST_CASE(
    "installer integration flow refreshes stubs across apply and rollback") {
  const zfleet::test::ScopedTestDir test_dir("installer-integration");
  const auto root = test_dir.path();

  const auto installer_v1 =
      CreateInstallerArchive(root / "installer-v1", "0.1.0", "stub-v1");
  const auto installer_v2 =
      CreateInstallerArchive(root / "installer-v2", "0.2.0", "stub-v2");
  const auto agent_v1 =
      CreateComponentArchive(root / "agent-v1", "agent", "0.1.0");
  const auto server_v1 =
      CreateComponentArchive(root / "server-v1", "server", "0.1.0");

  const auto install_v1 = zfleet::installer::ApplyPackage(root, installer_v1);
  if (!install_v1.ok) {
    FAIL(install_v1.message);
  }
  REQUIRE(fs::exists(StubPath(root, "installer")));
  REQUIRE(fs::exists(StubPath(root, "agent")));
  REQUIRE(fs::exists(StubPath(root, "server")));
  REQUIRE(ReadFile(StubPath(root, "installer")) == "stub-v1-template-0.1.0");

  REQUIRE(zfleet::installer::ApplyPackage(root, agent_v1).ok);
  REQUIRE(zfleet::installer::ApplyPackage(root, server_v1).ok);
  REQUIRE(ReadFile(StubPath(root, "agent")) == "stub-v1-template-0.1.0");
  REQUIRE(ReadFile(StubPath(root, "server")) == "stub-v1-template-0.1.0");

  REQUIRE(zfleet::installer::ApplyPackage(root, installer_v2).ok);
  REQUIRE(ReadFile(StubPath(root, "installer")) == "stub-v2-template-0.2.0");
  REQUIRE(ReadFile(StubPath(root, "agent")) == "stub-v2-template-0.2.0");
  REQUIRE(ReadFile(StubPath(root, "server")) == "stub-v2-template-0.2.0");

  const auto rollback = zfleet::installer::RollbackComponent(root, "installer");
  REQUIRE(rollback.ok);
  REQUIRE(ReadFile(StubPath(root, "installer")) == "stub-v1-template-0.1.0");
  REQUIRE(ReadFile(StubPath(root, "agent")) == "stub-v1-template-0.1.0");
  REQUIRE(ReadFile(StubPath(root, "server")) == "stub-v1-template-0.1.0");
}

TEST_CASE(
    "installer integration flow rejects component packages above active "
    "installer version") {
  const zfleet::test::ScopedTestDir test_dir("installer-integration");
  const auto root = test_dir.path();

  const auto installer_v1 =
      CreateInstallerArchive(root / "installer-v1", "0.1.0", "stub-v1");
  const auto agent_v2 =
      CreateComponentArchive(root / "agent-v2", "agent", "0.2.0", "0.2.0");

  const auto install_v1 = zfleet::installer::ApplyPackage(root, installer_v1);
  if (!install_v1.ok) {
    FAIL(install_v1.message);
  }
  const auto result = zfleet::installer::ApplyPackage(root, agent_v2);
  REQUIRE_FALSE(result.ok);
  REQUIRE(result.message ==
          "active installer version 0.1.0 does not satisfy "
          "min_installer_version 0.2.0");
}

