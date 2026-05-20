#include "installer.h"
#include "manifest.h"

#include "test_util.h"

#include <zfleet/package/archive.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
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

struct ManifestEntrySpec {
  std::string source;
  std::string target;
  std::uintmax_t size;
  std::string sha256;
  bool executable;
};

std::string JsonString(const std::string& value) {
  std::string escaped = "\"";
  for (const char ch : value) {
    switch (ch) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\n':
        escaped += "\\n";
        break;
      default:
        escaped += ch;
        break;
    }
  }
  escaped += "\"";
  return escaped;
}

std::string BuildManifestText(const std::string& component,
                              const std::string& version,
                              const std::vector<ManifestEntrySpec>& files) {
  std::ostringstream stream;
  stream << "{\n";
  stream << "  \"schema_version\": 1,\n";
  stream << "  \"component\": " << JsonString(component) << ",\n";
  stream << "  \"version\": " << JsonString(version) << ",\n";
  stream << "  \"min_installer_version\": \"0.1.0\",\n";
  stream << "  \"files\": [\n";

  for (std::size_t index = 0; index < files.size(); ++index) {
    const auto& file = files[index];
    stream << "    {\n";
    stream << "      \"source\": " << JsonString(file.source) << ",\n";
    stream << "      \"target\": " << JsonString(file.target) << ",\n";
    stream << "      \"size\": " << file.size << ",\n";
    stream << "      \"sha256\": " << JsonString(file.sha256) << ",\n";
    stream << "      \"executable\": "
           << (file.executable ? "true" : "false") << "\n";
    stream << "    }";
    if (index + 1 < files.size()) {
      stream << ",";
    }
    stream << "\n";
  }

  stream << "  ],\n";
  stream << "  \"signatures\": []\n";
  stream << "}\n";
  return stream.str();
}

std::string BuildManifest(const std::string& component,
                          const std::string& version,
                          const std::vector<PackageFileSpec>& files,
                          const fs::path& package_dir) {
  std::vector<ManifestEntrySpec> manifest_files;

  for (const auto& file : files) {
    const auto payload_path = package_dir / "payload" / file.source_relative_path;
    WriteTextFile(payload_path, file.content);
    zfleet::installer::SetExecutable(payload_path, file.executable);

    manifest_files.push_back(ManifestEntrySpec{
        .source = "payload/" + file.source_relative_path,
        .target = file.target,
        .size = fs::file_size(payload_path),
        .sha256 = zfleet::installer::ComputeSha256Hex(payload_path),
        .executable = file.executable,
    });
  }

  return BuildManifestText(component, version, manifest_files);
}

fs::path CreatePackage(const fs::path& root,
                       const std::string& component,
                       const std::string& version,
                       const std::vector<PackageFileSpec>& files,
                       const std::string* manifest_override = nullptr) {
  const auto package_dir = root / "package";
  fs::create_directories(package_dir / "META");
  const auto manifest =
      manifest_override == nullptr
          ? BuildManifest(component, version, files, package_dir)
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
  return root / "zfleet" / component;
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

} // namespace

TEST_CASE("manifest parser accepts a valid manifest") {
  const zfleet::test::ScopedTestDir test_root("installer");

  const auto package_dir =
      CreatePackage(test_root, "agent", "0.1.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                     .target = "bin/zfleet_agent",
                                     .content = "agent-binary",
                                     .executable = true}});

  const auto manifest =
      zfleet::installer::LoadManifest(package_dir / "META" / "manifest.json");

  REQUIRE(manifest.schema_version == 1);
  REQUIRE(manifest.component == "agent");
  REQUIRE(manifest.version == "0.1.0");
  REQUIRE(manifest.min_installer_version == "0.1.0");
  REQUIRE(manifest.files.size() == 1);
  REQUIRE(manifest.files.front().source == "payload/bin/zfleet_agent");
  REQUIRE(manifest.files.front().target == "bin/zfleet_agent");
  REQUIRE(manifest.files.front().executable);

}

TEST_CASE("manifest parser rejects malformed manifests") {
  const zfleet::test::ScopedTestDir test_root("installer");

  SECTION("missing required field") {
    const std::string manifest = R"({
      "schema_version": 1,
      "component": "agent",
      "version": "0.1.0",
      "files": []
    })";
    const auto package_dir = CreatePackage(test_root, "agent", "0.1.0", {},
                                           &manifest);
    REQUIRE_THROWS(zfleet::installer::LoadManifest(
        package_dir / "META" / "manifest.json"));
  }

  SECTION("unknown component") {
    const std::string manifest = R"({
      "schema_version": 1,
      "component": "worker",
      "version": "0.1.0",
      "min_installer_version": "0.1.0",
      "files": [],
      "signatures": []
    })";
    const auto package_dir = CreatePackage(test_root, "agent", "0.1.0", {},
                                           &manifest);
    REQUIRE_THROWS(zfleet::installer::LoadManifest(
        package_dir / "META" / "manifest.json"));
  }

  SECTION("invalid sha256") {
    const std::string manifest = R"({
      "schema_version": 1,
      "component": "agent",
      "version": "0.1.0",
      "min_installer_version": "0.1.0",
      "files": [
        {
          "source": "payload/bin/zfleet_agent",
          "target": "bin/zfleet_agent",
          "size": 1,
          "sha256": "not-a-valid-sha",
          "executable": true
        }
      ],
      "signatures": []
    })";
    const auto package_dir = CreatePackage(test_root, "agent", "0.1.0", {},
                                           &manifest);
    REQUIRE_THROWS(zfleet::installer::LoadManifest(
        package_dir / "META" / "manifest.json"));
  }

  SECTION("illegal paths") {
    const std::string manifest = R"({
      "schema_version": 1,
      "component": "agent",
      "version": "0.1.0",
      "min_installer_version": "0.1.0",
      "files": [
        {
          "source": "../payload/bin/zfleet_agent",
          "target": "bin/zfleet_agent",
          "size": 1,
          "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
          "executable": true
        },
        {
          "source": "payload/bin/zfleet_agent",
          "target": "META/manifest.json",
          "size": 1,
          "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
          "executable": true
        }
      ],
      "signatures": []
    })";
    const auto package_dir = CreatePackage(test_root, "agent", "0.1.0", {},
                                           &manifest);
    REQUIRE_THROWS(zfleet::installer::LoadManifest(
        package_dir / "META" / "manifest.json"));
  }

  SECTION("duplicate target") {
    const std::string manifest = R"({
      "schema_version": 1,
      "component": "agent",
      "version": "0.1.0",
      "min_installer_version": "0.1.0",
      "files": [
        {
          "source": "payload/bin/a",
          "target": "bin/zfleet_agent",
          "size": 1,
          "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
          "executable": true
        },
        {
          "source": "payload/bin/b",
          "target": "bin/zfleet_agent",
          "size": 1,
          "sha256": "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
          "executable": true
        }
      ],
      "signatures": []
    })";
    const auto package_dir = CreatePackage(test_root, "agent", "0.1.0", {},
                                           &manifest);
    REQUIRE_THROWS(zfleet::installer::LoadManifest(
        package_dir / "META" / "manifest.json"));
  }

}

TEST_CASE("apply writes release content and active-version") {
  const zfleet::test::ScopedTestDir test_root("installer");

  const auto package_dir =
      CreatePackage(test_root, "agent", "0.1.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                     .target = "bin/zfleet_agent",
                                     .content = "agent-binary",
                                     .executable = true}});

  const auto result = zfleet::installer::ApplyPackage(test_root, package_dir);

  REQUIRE(result.ok);
  const auto release_root =
      test_root / "zfleet" / "agent" / "releases" / "0.1.0";
  REQUIRE(fs::exists(release_root / "bin" / "zfleet_agent"));
  REQUIRE(fs::exists(release_root / "META" / "manifest.json"));
  REQUIRE(ReadTextFile(release_root / "bin" / "zfleet_agent") == "agent-binary");
  REQUIRE(zfleet::installer::IsExecutable(release_root / "bin" / "zfleet_agent"));
  REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "agent")) == "0.1.0\n");
  REQUIRE_FALSE(fs::exists(PreviousVersionPath(test_root, "agent")));

}

TEST_CASE("apply accepts a .zip archive and preserves directory install checks") {
  const zfleet::test::ScopedTestDir test_root("installer");

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
      test_root / "zfleet" / "agent" / "releases" / "0.1.0";
  REQUIRE(fs::exists(release_root / "bin" / "zfleet_agent"));
  REQUIRE(fs::exists(release_root / "META" / "manifest.json"));
  REQUIRE(ReadTextFile(release_root / "bin" / "zfleet_agent") == "agent-binary");
  REQUIRE(zfleet::installer::IsExecutable(release_root / "bin" / "zfleet_agent"));
  REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "agent")) == "0.1.0\n");

}

TEST_CASE("apply fails on payload integrity mismatch without writing active-version") {
  const zfleet::test::ScopedTestDir test_root("installer");

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
        fs::exists(test_root / "zfleet" / "agent" / "var" / "active-version"));
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
    const auto manifest = BuildManifestText(
        "agent", "0.1.0",
        std::vector<ManifestEntrySpec>{ManifestEntrySpec{
            .source = "payload/bin/zfleet_agent",
            .target = "bin/zfleet_agent",
            .size = 999,
            .sha256 = zfleet::installer::ComputeSha256Hex(payload_path),
            .executable = true,
        }});
    WriteTextFile(package_dir / "META" / "manifest.json", manifest);

    const auto result = zfleet::installer::ApplyPackage(test_root, package_dir);
    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(
        fs::exists(test_root / "zfleet" / "agent" / "var" / "active-version"));
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
      const auto manifest = BuildManifestText(
          "agent", "0.1.0",
          std::vector<ManifestEntrySpec>{ManifestEntrySpec{
              .source = "payload/bin/zfleet_agent",
              .target = "bin/zfleet_agent",
              .size = fs::file_size(outside_file),
              .sha256 = zfleet::installer::ComputeSha256Hex(outside_file),
              .executable = false,
          }});
      WriteTextFile(package_dir / "META" / "manifest.json", manifest);

      const auto result =
          zfleet::installer::ApplyPackage(test_root, package_dir);
      REQUIRE_FALSE(result.ok);
      REQUIRE_FALSE(fs::exists(test_root / "zfleet" / "agent" / "var" /
                               "active-version"));
    }
  }

}

TEST_CASE("same healthy version can be applied repeatedly") {
  const zfleet::test::ScopedTestDir test_root("installer");

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

  REQUIRE(fs::exists(test_root / "zfleet" / "agent" / "releases" / "0.1.0"));
  REQUIRE(fs::exists(test_root / "zfleet" / "agent" / "releases" / "0.2.0"));
  REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "agent")) == "0.2.0\n");
  REQUIRE(ReadTextFile(PreviousVersionPath(test_root, "agent")) == "0.1.0\n");
  REQUIRE(ReadTextFile(test_root / "zfleet" / "agent" / "releases" / "0.1.0" /
                       "bin" / "zfleet_agent") == "agent-binary-v1");

}

TEST_CASE("same active version does not rewrite previous-version to self") {
  const zfleet::test::ScopedTestDir test_root("installer");

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
    WriteTextFile(test_root / "zfleet" / "agent" / "var" / "active-version",
                  "0.1.0\n");

    const auto status = zfleet::installer::GetStatus(test_root, "agent");

    REQUIRE(status.state == "corrupt");
    REQUIRE(status.version == std::optional<std::string>{"0.1.0"});
    REQUIRE(status.message.has_value());
  }

  SECTION("active release file tampered") {
    const auto package_dir =
        CreatePackage(test_root, "agent", "0.1.0",
                      {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                       .target = "bin/zfleet_agent",
                                       .content = "agent-binary",
                                       .executable = true}});
    REQUIRE(zfleet::installer::ApplyPackage(test_root, package_dir).ok);
    WriteTextFile(test_root / "zfleet" / "agent" / "releases" / "0.1.0" /
                      "bin" / "zfleet_agent",
                  "tampered-binary");

    const auto status = zfleet::installer::GetStatus(test_root, "agent");

    REQUIRE(status.state == "corrupt");
    REQUIRE(status.version == std::optional<std::string>{"0.1.0"});
    REQUIRE(status.message.has_value());
  }

}

TEST_CASE("component installs stay isolated under the shared root") {
  const zfleet::test::ScopedTestDir test_root("installer");

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
  const auto installer_package = CreatePackage(
      test_root / "installer-pkg", "installer", "0.1.0",
      {PackageFileSpec{.source_relative_path = "bin/zfleet_installer",
                       .target = "bin/zfleet_installer",
                       .content = "installer-binary",
                       .executable = true}});

  REQUIRE(zfleet::installer::ApplyPackage(test_root, agent_package).ok);
  REQUIRE(zfleet::installer::ApplyPackage(test_root, server_package).ok);
  REQUIRE(zfleet::installer::ApplyPackage(test_root, installer_package).ok);
  const auto agent_package_v2 =
      CreatePackage(test_root / "agent-pkg-v2", "agent", "0.2.0",
                    {PackageFileSpec{.source_relative_path = "bin/zfleet_agent",
                                     .target = "bin/zfleet_agent",
                                     .content = "agent-binary-v2",
                                     .executable = true}});
  REQUIRE(zfleet::installer::ApplyPackage(test_root, agent_package_v2).ok);
  REQUIRE(zfleet::installer::RollbackComponent(test_root, "agent").ok);

  REQUIRE(fs::exists(test_root / "zfleet" / "agent" / "releases" / "0.1.0" /
                     "bin" / "zfleet_agent"));
  REQUIRE(fs::exists(test_root / "zfleet" / "server" / "releases" / "0.1.0" /
                     "bin" / "zfleet_server"));
  REQUIRE(fs::exists(test_root / "zfleet" / "installer" / "releases" /
                     "0.1.0" / "bin" / "zfleet_installer"));
  REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "agent")) == "0.1.0\n");
  REQUIRE(ReadTextFile(PreviousVersionPath(test_root, "agent")) == "0.2.0\n");
  REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "server")) == "0.1.0\n");
  REQUIRE_FALSE(fs::exists(PreviousVersionPath(test_root, "server")));
  REQUIRE(ReadTextFile(ActiveVersionPath(test_root, "installer")) == "0.1.0\n");
  REQUIRE_FALSE(fs::exists(PreviousVersionPath(test_root, "installer")));

}
