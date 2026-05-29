#include "installer.h"

#include "test_util.h"

#include <zfleet/crypto/sha256.h>
#include <zfleet/package/archive.h>
#include <zfleet/package/manifest.h>
#include <zfleet/platform/file_permissions.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using zfleet::test::ReadTextFile;
using zfleet::test::WriteTextFile;

struct PackageFileSpec {
  std::string source_relative_path;
  std::string target;
  std::string content;
  bool executable = false;
};

std::string BuildManifestJson(
    const std::string& component,
    const std::string& version,
    const std::vector<zfleet::package::ManifestFile>& files,
    const std::string& min_installer_version = "0.1.0") {
  return zfleet::package::SerializeManifestJson(zfleet::package::Manifest{
      .schema_version = 1,
      .component = component,
      .version = version,
      .platform = "linux",
      .arch = "x86_64",
      .build_type = "debug",
      .min_installer_version = min_installer_version,
      .files = files,
  });
}

std::string BuildManifest(const std::string& component,
                          const std::string& version,
                          const std::vector<PackageFileSpec>& files,
                          const fs::path& package_dir,
                          const std::string& min_installer_version = "0.1.0") {
  std::vector<zfleet::package::ManifestFile> manifest_files;

  for (const auto& file : files) {
    const auto payload_path =
        package_dir / "payload" / file.source_relative_path;
    WriteTextFile(payload_path, file.content);
    zfleet::platform::SetExecutable(payload_path, file.executable);

    manifest_files.push_back(zfleet::package::ManifestFile{
        .source = "payload/" + file.source_relative_path,
        .target = file.target,
        .size = static_cast<std::uint64_t>(fs::file_size(payload_path)),
        .sha256 = zfleet::crypto::Sha256FileHex(payload_path),
        .executable = file.executable,
    });
  }

  return BuildManifestJson(component, version, manifest_files,
                           min_installer_version);
}

fs::path CreatePackage(const fs::path& root,
                       const std::string& component,
                       const std::string& version,
                       const std::vector<PackageFileSpec>& files,
                       const std::string* manifest_override = nullptr,
                       const std::string& min_installer_version = "0.1.0") {
  const auto package_dir = root / "package";
  fs::create_directories(package_dir / "META");
  const auto manifest =
      manifest_override == nullptr
          ? BuildManifest(component, version, files, package_dir,
                          min_installer_version)
          : *manifest_override;
  WriteTextFile(package_dir / "META" / "manifest.json", manifest);
  return package_dir;
}

fs::path CreateArchivePackage(const fs::path& package_dir,
                              const fs::path& archive_path) {
  zfleet::package::CreateArchive({package_dir, archive_path, true});
  return archive_path;
}

fs::path ComponentRoot(const fs::path& root, const std::string& component) {
  return root / component;
}

fs::path ActiveVersionPath(const fs::path& root, const std::string& component) {
  return ComponentRoot(root, component) / "var" / "active-version";
}

fs::path PreviousVersionPath(const fs::path& root,
                             const std::string& component) {
  return ComponentRoot(root, component) / "var" / "previous-version";
}

fs::path ReleaseBinaryPath(const fs::path& root,
                           const std::string& component,
                           const std::string& version) {
  const auto binary_name = "zfleet_" + component;
  return ComponentRoot(root, component) / "releases" / version / "bin" /
         binary_name;
}

fs::path LauncherStubPath(const fs::path& root, const std::string& component) {
  const auto binary_name = "zfleet_" + component;
  return ComponentRoot(root, component) / "bin" / binary_name;
}

fs::path CreateInstallerPackage(const fs::path& root,
                                const std::string& version,
                                const std::string& launcher_tag = "launcher") {
  return CreatePackage(
      root, "installer", version,
      {
          PackageFileSpec{.source_relative_path = "bin/zfleet_installer",
                          .target = "bin/zfleet_installer",
                          .content = "installer-binary-" + version,
                          .executable = true},
          PackageFileSpec{
              .source_relative_path = "bin/zfleet_launcher",
              .target = "bin/zfleet_launcher",
              .content = launcher_tag + "-template-" + version,
                          .executable = true},
      });
}

void InstallInstallerRelease(const fs::path& root, const std::string& version = "0.1.0",
                             const std::string& launcher_tag = "launcher") {
  const auto package_dir =
      CreateInstallerPackage(root / ("installer-" + version), version,
                             launcher_tag);
  REQUIRE(zfleet::installer::ApplyPackage(root, package_dir).ok);
}

} // namespace

TEST_CASE("apply writes release content and active-version") {
  const zfleet::test::ScopedTestDir test_root("installer");
  InstallInstallerRelease(test_root);

  const auto package_dir =
      CreatePackage(test_root, "agent", "0.1.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                     .target = "bin/zfleet_agent",
                                     .content = "agent-binary",
                                     .executable = true}});

  const auto result = zfleet::installer::ApplyPackage(test_root, package_dir);

  REQUIRE(result.ok);
  const auto release_root = test_root / "agent" / "releases" / "0.1.0";
  REQUIRE(fs::exists(release_root / "bin" / "zfleet_agent"));
  REQUIRE(fs::exists(release_root / "META" / "manifest.json"));
  REQUIRE(ReadTextFile(release_root / "bin" / "zfleet_agent") == "agent-binary");
  REQUIRE(zfleet::platform::IsExecutableFile(release_root / "bin" /
                                             "zfleet_agent"));
  REQUIRE(fs::exists(LauncherStubPath(test_root, "agent")));
  REQUIRE(ReadTextFile(LauncherStubPath(test_root, "agent")) ==
          "launcher-template-0.1.0");
  REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "agent")) == "0.1.0\n");
  REQUIRE_FALSE(fs::exists(PreviousVersionPath(test_root, "agent")));

}

TEST_CASE("apply accepts a .zip archive and preserves directory install checks") {
  const zfleet::test::ScopedTestDir test_root("installer");
  InstallInstallerRelease(test_root);

  const auto package_dir =
      CreatePackage(test_root / "package-dir", "agent", "0.1.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                     .target = "bin/zfleet_agent",
                                     .content = "agent-binary",
                                     .executable = true}});
  const auto archive_path = test_root / "package.zip";
  CreateArchivePackage(package_dir, archive_path);

  const auto result = zfleet::installer::ApplyPackage(test_root, archive_path);

  REQUIRE(result.ok);
  REQUIRE(result.component == "agent");
  REQUIRE(result.version == "0.1.0");
  const auto release_root =
      test_root / "agent" / "releases" / "0.1.0";
  REQUIRE(fs::exists(release_root / "bin" / "zfleet_agent"));
  REQUIRE(fs::exists(release_root / "META" / "manifest.json"));
  REQUIRE(ReadTextFile(release_root / "bin" / "zfleet_agent") == "agent-binary");
  REQUIRE(zfleet::platform::IsExecutableFile(release_root / "bin" /
                                             "zfleet_agent"));
  REQUIRE(fs::exists(LauncherStubPath(test_root, "agent")));
  REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "agent")) == "0.1.0\n");

}

TEST_CASE("apply fails on payload integrity mismatch without writing active-version") {
  const zfleet::test::ScopedTestDir test_root("installer");
  InstallInstallerRelease(test_root);

  SECTION("sha mismatch") {
    const auto package_dir =
        CreatePackage(test_root, "agent", "0.1.0",
                      {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                       .target = "bin/zfleet_agent",
                                       .content = "agent-binary",
                                       .executable = true}});
    WriteTextFile(package_dir / "payload" / "bin" / "zfleet_agent",
                  "tampered-binary");

    const auto result = zfleet::installer::ApplyPackage(test_root, package_dir);
    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(
        fs::exists(test_root / "agent" / "var" / "active-version"));
  }

  SECTION("size mismatch") {
    const auto package_dir = CreatePackage(test_root / "size-case", "agent",
                                           "0.1.0",
                                           {PackageFileSpec{
                                               .source_relative_path =
                                                   "bin/zfleet_agent",
                                               .target = "bin/zfleet_agent",
                                               .content = "agent-binary",
                                               .executable = true}});
    const auto payload_path =
        package_dir / "payload" / "bin" / "zfleet_agent";
    const auto manifest = BuildManifestJson(
        "agent", "0.1.0",
        {zfleet::package::ManifestFile{
            .source = "payload/bin/zfleet_agent",
            .target = "bin/zfleet_agent",
            .size = 999U,
            .sha256 = zfleet::crypto::Sha256FileHex(payload_path),
            .executable = true,
        }});
    WriteTextFile(package_dir / "META" / "manifest.json", manifest);

    const auto result = zfleet::installer::ApplyPackage(test_root, package_dir);
    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(
        fs::exists(test_root / "agent" / "var" / "active-version"));
  }

  SECTION("payload directory symlink") {
    const auto package_dir = test_root / "symlink-case" / "package";
    const auto outside_dir = test_root / "outside-payload";
    const auto outside_file = outside_dir / "bin" / "zfleet_agent";
    WriteTextFile(outside_file, "agent-binary");

    fs::create_directories(package_dir / "META");
    std::error_code symlink_error;
    fs::create_directory_symlink(outside_dir, package_dir / "payload",
                                 symlink_error);
    if (!symlink_error) {
      const auto manifest = BuildManifestJson(
          "agent", "0.1.0",
          {zfleet::package::ManifestFile{
              .source = "payload/bin/zfleet_agent",
              .target = "bin/zfleet_agent",
              .size = static_cast<std::uint64_t>(fs::file_size(outside_file)),
              .sha256 = zfleet::crypto::Sha256FileHex(outside_file),
              .executable = false,
          }});
      WriteTextFile(package_dir / "META" / "manifest.json", manifest);

      const auto result =
          zfleet::installer::ApplyPackage(test_root, package_dir);
      REQUIRE_FALSE(result.ok);
      REQUIRE_FALSE(
          fs::exists(test_root / "agent" / "var" / "active-version"));
    }
  }

}

TEST_CASE("apply rejects component packages when active installer is missing or too old") {
  const zfleet::test::ScopedTestDir test_root("installer");

  const auto agent_package =
      CreatePackage(test_root / "agent-pkg", "agent", "0.2.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                     .target = "bin/zfleet_agent",
                                     .content = "agent-binary",
                                     .executable = true}});

  SECTION("missing installer") {
    const auto result = zfleet::installer::ApplyPackage(test_root, agent_package);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.message == "active installer release is not installed");
  }

  SECTION("installer below min_installer_version") {
    InstallInstallerRelease(test_root, "0.1.0");
    const auto package_dir =
        CreatePackage(test_root / "agent-pkg-min", "agent", "0.2.0",
                      {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                       .target = "bin/zfleet_agent",
                                       .content = "agent-binary",
                                       .executable = true}},
                      nullptr, "0.2.0");

    const auto result = zfleet::installer::ApplyPackage(test_root, package_dir);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.message ==
            "active installer version 0.1.0 does not satisfy min_installer_version 0.2.0");
  }
}

TEST_CASE("installer package must carry launcher assets and deploy stubs") {
  const zfleet::test::ScopedTestDir test_root("installer");

  SECTION("missing launcher asset is rejected") {
    const auto package_dir = CreatePackage(
        test_root / "missing-launcher", "installer", "0.1.0",
        {PackageFileSpec{.source_relative_path = "bin/zfleet_installer",
                         .target = "bin/zfleet_installer",
                         .content = "installer-binary",
                         .executable = true}});

    const auto result = zfleet::installer::ApplyPackage(test_root, package_dir);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.message.find("launcher asset missing:") == 0);
  }

  SECTION("installer apply and rollback refresh fixed launcher stubs") {
    InstallInstallerRelease(test_root, "0.1.0", "stub-v1");
    const auto installer_v2 =
        CreateInstallerPackage(test_root / "installer-v2", "0.2.0", "stub-v2");

    REQUIRE(zfleet::installer::ApplyPackage(test_root, installer_v2).ok);
    REQUIRE(ReadTextFile(LauncherStubPath(test_root, "installer")) ==
            "stub-v2-template-0.2.0");
    REQUIRE(ReadTextFile(LauncherStubPath(test_root, "agent")) ==
            "stub-v2-template-0.2.0");
    REQUIRE(ReadTextFile(LauncherStubPath(test_root, "server")) ==
            "stub-v2-template-0.2.0");

    const auto rollback =
        zfleet::installer::RollbackComponent(test_root, "installer");
    REQUIRE(rollback.ok);
    REQUIRE(ReadTextFile(LauncherStubPath(test_root, "installer")) ==
            "stub-v1-template-0.1.0");
    REQUIRE(ReadTextFile(LauncherStubPath(test_root, "agent")) ==
            "stub-v1-template-0.1.0");
    REQUIRE(ReadTextFile(LauncherStubPath(test_root, "server")) ==
            "stub-v1-template-0.1.0");
  }
}

TEST_CASE("same healthy version can be applied repeatedly") {
  const zfleet::test::ScopedTestDir test_root("installer");
  InstallInstallerRelease(test_root);

  const auto package_dir =
      CreatePackage(test_root, "agent", "0.1.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                     .target = "bin/zfleet_agent",
                                     .content = "agent-binary",
                                     .executable = true}});

  REQUIRE(zfleet::installer::ApplyPackage(test_root, package_dir).ok);
  const auto second_result =
      zfleet::installer::ApplyPackage(test_root, package_dir);

  REQUIRE(second_result.ok);
  REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "agent")) == "0.1.0\n");
  REQUIRE_FALSE(fs::exists(PreviousVersionPath(test_root, "agent")));

}

TEST_CASE("new version apply switches active-version and preserves old release") {
  const zfleet::test::ScopedTestDir test_root("installer");
  InstallInstallerRelease(test_root);

  const auto package_v1 =
      CreatePackage(test_root / "v1", "agent", "0.1.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                     .target = "bin/zfleet_agent",
                                     .content = "agent-binary-v1",
                                     .executable = true}});
  const auto package_v2 =
      CreatePackage(test_root / "v2", "agent", "0.2.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                     .target = "bin/zfleet_agent",
                                     .content = "agent-binary-v2",
                                     .executable = true}});

  REQUIRE(zfleet::installer::ApplyPackage(test_root, package_v1).ok);
  REQUIRE(zfleet::installer::ApplyPackage(test_root, package_v2).ok);

  REQUIRE(fs::exists(test_root / "agent" / "releases" / "0.1.0"));
  REQUIRE(fs::exists(test_root / "agent" / "releases" / "0.2.0"));
  REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "agent")) == "0.2.0\n");
  REQUIRE(ReadTextFile(PreviousVersionPath(test_root, "agent")) == "0.1.0\n");
  REQUIRE(ReadTextFile(test_root / "agent" / "releases" / "0.1.0" / "bin" /
                       "zfleet_agent") == "agent-binary-v1");

}

TEST_CASE("same active version does not rewrite previous-version to self") {
  const zfleet::test::ScopedTestDir test_root("installer");
  InstallInstallerRelease(test_root);

  const auto package_v1 =
      CreatePackage(test_root / "v1", "agent", "0.1.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                     .target = "bin/zfleet_agent",
                                     .content = "agent-binary-v1",
                                     .executable = true}});
  const auto package_v2 =
      CreatePackage(test_root / "v2", "agent", "0.2.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                     .target = "bin/zfleet_agent",
                                     .content = "agent-binary-v2",
                                     .executable = true}});

  REQUIRE(zfleet::installer::ApplyPackage(test_root, package_v1).ok);
  REQUIRE(zfleet::installer::ApplyPackage(test_root, package_v2).ok);
  REQUIRE(zfleet::installer::ApplyPackage(test_root, package_v2).ok);

  REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "agent")) == "0.2.0\n");
  REQUIRE(ReadTextFile(PreviousVersionPath(test_root, "agent")) == "0.1.0\n");

}

TEST_CASE("apply from corrupt active to new version does not record corrupt previous") {
  const zfleet::test::ScopedTestDir test_root("installer");
  InstallInstallerRelease(test_root);

  const auto package_v1 =
      CreatePackage(test_root / "v1", "agent", "0.1.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                     .target = "bin/zfleet_agent",
                                     .content = "agent-binary-v1",
                                     .executable = true}});
  const auto package_v2 =
      CreatePackage(test_root / "v2", "agent", "0.2.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                     .target = "bin/zfleet_agent",
                                     .content = "agent-binary-v2",
                                     .executable = true}});

  REQUIRE(zfleet::installer::ApplyPackage(test_root, package_v1).ok);
  WriteTextFile(ReleaseBinaryPath(test_root, "agent", "0.1.0"),
                "tampered-binary");

  const auto result = zfleet::installer::ApplyPackage(test_root, package_v2);

  REQUIRE(result.ok);
  REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "agent")) == "0.2.0\n");
  REQUIRE_FALSE(fs::exists(PreviousVersionPath(test_root, "agent")));

}

TEST_CASE("rollback swaps active and previous versions") {
  const zfleet::test::ScopedTestDir test_root("installer");
  InstallInstallerRelease(test_root);

  const auto package_v1 =
      CreatePackage(test_root / "v1", "agent", "0.1.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                     .target = "bin/zfleet_agent",
                                     .content = "agent-binary-v1",
                                     .executable = true}});
  const auto package_v2 =
      CreatePackage(test_root / "v2", "agent", "0.2.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                     .target = "bin/zfleet_agent",
                                     .content = "agent-binary-v2",
                                     .executable = true}});

  REQUIRE(zfleet::installer::ApplyPackage(test_root, package_v1).ok);
  REQUIRE(zfleet::installer::ApplyPackage(test_root, package_v2).ok);

  const auto rollback = zfleet::installer::RollbackComponent(test_root, "agent");

  REQUIRE(rollback.ok);
  REQUIRE(rollback.component == "agent");
  REQUIRE(rollback.from_version == "0.2.0");
  REQUIRE(rollback.to_version == "0.1.0");
  REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "agent")) == "0.1.0\n");
  REQUIRE(ReadTextFile(PreviousVersionPath(test_root, "agent")) == "0.2.0\n");

}

TEST_CASE("rollback can switch between adjacent versions repeatedly") {
  const zfleet::test::ScopedTestDir test_root("installer");
  InstallInstallerRelease(test_root);

  const auto package_v1 =
      CreatePackage(test_root / "v1", "agent", "0.1.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                     .target = "bin/zfleet_agent",
                                     .content = "agent-binary-v1",
                                     .executable = true}});
  const auto package_v2 =
      CreatePackage(test_root / "v2", "agent", "0.2.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                     .target = "bin/zfleet_agent",
                                     .content = "agent-binary-v2",
                                     .executable = true}});

  REQUIRE(zfleet::installer::ApplyPackage(test_root, package_v1).ok);
  REQUIRE(zfleet::installer::ApplyPackage(test_root, package_v2).ok);
  REQUIRE(zfleet::installer::RollbackComponent(test_root, "agent").ok);

  const auto second_rollback =
      zfleet::installer::RollbackComponent(test_root, "agent");

  REQUIRE(second_rollback.ok);
  REQUIRE(second_rollback.from_version == "0.1.0");
  REQUIRE(second_rollback.to_version == "0.2.0");
  REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "agent")) == "0.2.0\n");
  REQUIRE(ReadTextFile(PreviousVersionPath(test_root, "agent")) == "0.1.0\n");

}

TEST_CASE("rollback failures keep active-version unchanged") {
  const zfleet::test::ScopedTestDir test_root("installer");
  InstallInstallerRelease(test_root);

  SECTION("missing previous-version") {
    const auto package_dir =
        CreatePackage(test_root, "agent", "0.1.0",
                      {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                       .target = "bin/zfleet_agent",
                                       .content = "agent-binary",
                                       .executable = true}});
    REQUIRE(zfleet::installer::ApplyPackage(test_root, package_dir).ok);

    const auto rollback = zfleet::installer::RollbackComponent(test_root, "agent");

    REQUIRE_FALSE(rollback.ok);
    REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "agent")) == "0.1.0\n");
    REQUIRE_FALSE(fs::exists(PreviousVersionPath(test_root, "agent")));
  }

  SECTION("invalid previous-version") {
    const auto package_v1 =
        CreatePackage(test_root / "v1", "agent", "0.1.0",
                      {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                       .target = "bin/zfleet_agent",
                                       .content = "agent-binary-v1",
                                       .executable = true}});
    const auto package_v2 =
        CreatePackage(test_root / "v2", "agent", "0.2.0",
                      {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                       .target = "bin/zfleet_agent",
                                       .content = "agent-binary-v2",
                                       .executable = true}});
    REQUIRE(zfleet::installer::ApplyPackage(test_root, package_v1).ok);
    REQUIRE(zfleet::installer::ApplyPackage(test_root, package_v2).ok);
    WriteTextFile(PreviousVersionPath(test_root, "agent"), "../bad\n");

    const auto rollback = zfleet::installer::RollbackComponent(test_root, "agent");

    REQUIRE_FALSE(rollback.ok);
    REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "agent")) == "0.2.0\n");
    REQUIRE(ReadTextFile(PreviousVersionPath(test_root, "agent")) == "../bad\n");
  }

  SECTION("previous-version matches active-version") {
    const auto package_dir =
        CreatePackage(test_root, "agent", "0.2.0",
                      {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                       .target = "bin/zfleet_agent",
                                       .content = "agent-binary-v2",
                                       .executable = true}});
    REQUIRE(zfleet::installer::ApplyPackage(test_root, package_dir).ok);
    WriteTextFile(PreviousVersionPath(test_root, "agent"), "0.2.0\n");

    const auto rollback = zfleet::installer::RollbackComponent(test_root, "agent");

    REQUIRE_FALSE(rollback.ok);
    REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "agent")) == "0.2.0\n");
    REQUIRE(ReadTextFile(PreviousVersionPath(test_root, "agent")) == "0.2.0\n");
  }

  SECTION("previous release missing") {
    const auto package_v1 =
        CreatePackage(test_root / "v1", "agent", "0.1.0",
                      {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                       .target = "bin/zfleet_agent",
                                       .content = "agent-binary-v1",
                                       .executable = true}});
    const auto package_v2 =
        CreatePackage(test_root / "v2", "agent", "0.2.0",
                      {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                       .target = "bin/zfleet_agent",
                                       .content = "agent-binary-v2",
                                       .executable = true}});
    REQUIRE(zfleet::installer::ApplyPackage(test_root, package_v1).ok);
    REQUIRE(zfleet::installer::ApplyPackage(test_root, package_v2).ok);
    fs::remove_all(ComponentRoot(test_root, "agent") / "releases" / "0.1.0");

    const auto rollback = zfleet::installer::RollbackComponent(test_root, "agent");

    REQUIRE_FALSE(rollback.ok);
    REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "agent")) == "0.2.0\n");
    REQUIRE(ReadTextFile(PreviousVersionPath(test_root, "agent")) == "0.1.0\n");
  }

  SECTION("previous release tampered") {
    const auto package_v1 =
        CreatePackage(test_root / "v1", "agent", "0.1.0",
                      {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                       .target = "bin/zfleet_agent",
                                       .content = "agent-binary-v1",
                                       .executable = true}});
    const auto package_v2 =
        CreatePackage(test_root / "v2", "agent", "0.2.0",
                      {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                       .target = "bin/zfleet_agent",
                                       .content = "agent-binary-v2",
                                       .executable = true}});
    REQUIRE(zfleet::installer::ApplyPackage(test_root, package_v1).ok);
    REQUIRE(zfleet::installer::ApplyPackage(test_root, package_v2).ok);
    WriteTextFile(ReleaseBinaryPath(test_root, "agent", "0.1.0"),
                  "tampered-binary");

    const auto rollback = zfleet::installer::RollbackComponent(test_root, "agent");

    REQUIRE_FALSE(rollback.ok);
    REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "agent")) == "0.2.0\n");
    REQUIRE(ReadTextFile(PreviousVersionPath(test_root, "agent")) == "0.1.0\n");
  }

  SECTION("active release corrupt") {
    const auto package_v1 =
        CreatePackage(test_root / "v1", "agent", "0.1.0",
                      {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                       .target = "bin/zfleet_agent",
                                       .content = "agent-binary-v1",
                                       .executable = true}});
    const auto package_v2 =
        CreatePackage(test_root / "v2", "agent", "0.2.0",
                      {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                       .target = "bin/zfleet_agent",
                                       .content = "agent-binary-v2",
                                       .executable = true}});
    REQUIRE(zfleet::installer::ApplyPackage(test_root, package_v1).ok);
    REQUIRE(zfleet::installer::ApplyPackage(test_root, package_v2).ok);
    WriteTextFile(ReleaseBinaryPath(test_root, "agent", "0.2.0"),
                  "tampered-binary");

    const auto rollback = zfleet::installer::RollbackComponent(test_root, "agent");

    REQUIRE_FALSE(rollback.ok);
    REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "agent")) == "0.2.0\n");
    REQUIRE(ReadTextFile(PreviousVersionPath(test_root, "agent")) == "0.1.0\n");
  }

}

TEST_CASE("status reports installation health") {
  const zfleet::test::ScopedTestDir test_root("installer");

  SECTION("not installed") {
    const auto status = zfleet::installer::GetStatus(test_root, "agent");

    REQUIRE(status.state == "not_installed");
    REQUIRE(status.component == "agent");
    REQUIRE_FALSE(status.version.has_value());
  }

  SECTION("installed") {
    InstallInstallerRelease(test_root);
    const auto package_dir =
        CreatePackage(test_root, "agent", "0.1.0",
                      {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                       .target = "bin/zfleet_agent",
                                       .content = "agent-binary",
                                       .executable = true}});
    REQUIRE(zfleet::installer::ApplyPackage(test_root, package_dir).ok);

    const auto status = zfleet::installer::GetStatus(test_root, "agent");

    REQUIRE(status.state == "installed");
    REQUIRE(status.component == "agent");
    REQUIRE(status.version == std::optional<std::string>{"0.1.0"});
  }

  SECTION("active release missing") {
    WriteTextFile(test_root / "agent" / "var" / "active-version", "0.1.0\n");

    const auto status = zfleet::installer::GetStatus(test_root, "agent");

    REQUIRE(status.state == "corrupt");
    REQUIRE(status.version == std::optional<std::string>{"0.1.0"});
    REQUIRE(status.message.has_value());
  }

  SECTION("active release file tampered") {
    InstallInstallerRelease(test_root);
    const auto package_dir =
        CreatePackage(test_root, "agent", "0.1.0",
                      {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                       .target = "bin/zfleet_agent",
                                       .content = "agent-binary",
                                       .executable = true}});
    REQUIRE(zfleet::installer::ApplyPackage(test_root, package_dir).ok);
    WriteTextFile(test_root / "agent" / "releases" / "0.1.0" / "bin" /
                      "zfleet_agent",
                  "tampered-binary");

    const auto status = zfleet::installer::GetStatus(test_root, "agent");

    REQUIRE(status.state == "corrupt");
    REQUIRE(status.version == std::optional<std::string>{"0.1.0"});
    REQUIRE(status.message.has_value());
  }

}

TEST_CASE("component installs stay isolated under the shared root") {
  const zfleet::test::ScopedTestDir test_root("installer");
  InstallInstallerRelease(test_root, "0.1.0", "stub-v1");

  const auto agent_package =
      CreatePackage(test_root / "agent-pkg", "agent", "0.1.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                     .target = "bin/zfleet_agent",
                                     .content = "agent-binary",
                                     .executable = true}});
  const auto server_package =
      CreatePackage(test_root / "server-pkg", "server", "0.1.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_server",
                                     .target = "bin/zfleet_server",
                                     .content = "server-binary",
                                     .executable = true}});
  REQUIRE(zfleet::installer::ApplyPackage(test_root, agent_package).ok);
  REQUIRE(zfleet::installer::ApplyPackage(test_root, server_package).ok);
  const auto agent_package_v2 =
      CreatePackage(test_root / "agent-pkg-v2", "agent", "0.2.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                     .target = "bin/zfleet_agent",
                                     .content = "agent-binary-v2",
                                     .executable = true}});
  REQUIRE(zfleet::installer::ApplyPackage(test_root, agent_package_v2).ok);
  REQUIRE(zfleet::installer::RollbackComponent(test_root, "agent").ok);

  REQUIRE(fs::exists(test_root / "agent" / "releases" / "0.1.0" / "bin" /
                     "zfleet_agent"));
  REQUIRE(fs::exists(test_root / "server" / "releases" / "0.1.0" / "bin" /
                     "zfleet_server"));
  REQUIRE(fs::exists(test_root / "installer" / "releases" / "0.1.0" / "bin" /
                     "zfleet_installer"));
  REQUIRE(ReadTextFile(LauncherStubPath(test_root, "agent")) ==
          "stub-v1-template-0.1.0");
  REQUIRE(ReadTextFile(LauncherStubPath(test_root, "server")) ==
          "stub-v1-template-0.1.0");
  REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "agent")) == "0.1.0\n");
  REQUIRE(ReadTextFile(PreviousVersionPath(test_root, "agent")) == "0.2.0\n");
  REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "server")) == "0.1.0\n");
  REQUIRE_FALSE(fs::exists(PreviousVersionPath(test_root, "server")));
  REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "installer")) == "0.1.0\n");
  REQUIRE_FALSE(fs::exists(PreviousVersionPath(test_root, "installer")));

}
