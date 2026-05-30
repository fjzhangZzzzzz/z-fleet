#include <zfleet/package/manifest.h>

#include "test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

namespace {

constexpr char kEmptySha256[] =
    "e3b0c44298fc1c149afbf4c8996fb924"
    "27ae41e4649b934ca495991b7852b855";

}  // namespace

TEST_CASE("manifest json supports serialization and parsing") {
  const zfleet::package::Manifest manifest{
      .schema_version = 2,
      .component = "agent",
      .version = "0.1.0",
      .platform = "linux",
      .arch = "x86_64",
      .build_type = "release",
      .min_installer_version = "0.1.0",
      .files = {zfleet::package::ManifestFile{
          .source = "payload/bin/zfleet_agent",
          .target = "bin/zfleet_agent",
          .size = 0,
          .sha256 = kEmptySha256,
          .launchable = true,
      }},
  };

  const auto serialized = zfleet::package::SerializeManifestJson(manifest);
  const auto parsed = zfleet::package::ParseManifestJson(serialized);

  REQUIRE(parsed.schema_version == 2);
  REQUIRE(parsed.component == "agent");
  REQUIRE(parsed.version == "0.1.0");
  REQUIRE(parsed.platform == "linux");
  REQUIRE(parsed.arch == "x86_64");
  REQUIRE(parsed.build_type == "release");
  REQUIRE(parsed.min_installer_version == "0.1.0");
  REQUIRE(parsed.files.size() == 1);
  REQUIRE(parsed.files.front().source == "payload/bin/zfleet_agent");
  REQUIRE(parsed.files.front().target == "bin/zfleet_agent");
  REQUIRE(parsed.files.front().size == 0);
  REQUIRE(parsed.files.front().sha256 == kEmptySha256);
  REQUIRE(parsed.files.front().launchable);
}

TEST_CASE("manifest json loader reads from disk") {
  const zfleet::test::ScopedTestDir test_dir("package");
  const auto manifest_path = test_dir / "META" / "manifest.json";
  zfleet::test::WriteTextFile(
      manifest_path,
      zfleet::package::SerializeManifestJson(zfleet::package::Manifest{
          .schema_version = 2,
          .component = "server",
          .version = "1.2.3",
          .platform = "linux",
          .arch = "x86_64",
          .build_type = "debug",
          .min_installer_version = "0.1.0",
          .files = {},
      }));

  const auto manifest = zfleet::package::LoadManifest(manifest_path);

  REQUIRE(manifest.component == "server");
  REQUIRE(manifest.version == "1.2.3");
  REQUIRE(manifest.build_type == "debug");
}

TEST_CASE("manifest json rejects malformed package metadata") {
  SECTION("missing required field") {
    REQUIRE_THROWS(zfleet::package::ParseManifestJson(R"({
      "schema_version": 2,
      "component": "agent",
      "version": "0.1.0",
      "files": []
    })"));
  }

  SECTION("unknown component") {
    REQUIRE_THROWS(zfleet::package::ParseManifestJson(R"({
      "schema_version": 2,
      "component": "worker",
      "version": "0.1.0",
      "platform": "linux",
      "arch": "x86_64",
      "build_type": "release",
      "min_installer_version": "0.1.0",
      "files": [],
      "signatures": []
    })"));
  }

  SECTION("invalid sha256") {
    REQUIRE_THROWS(zfleet::package::ParseManifestJson(R"({
      "schema_version": 2,
      "component": "agent",
      "version": "0.1.0",
      "platform": "linux",
      "arch": "x86_64",
      "build_type": "release",
      "min_installer_version": "0.1.0",
      "files": [
        {
          "source": "payload/bin/zfleet_agent",
          "target": "bin/zfleet_agent",
          "size": 1,
          "sha256": "not-a-valid-sha",
          "launchable": true
        }
      ],
      "signatures": []
    })"));
  }

  SECTION("unsafe paths") {
    REQUIRE_THROWS(zfleet::package::ParseManifestJson(R"({
      "schema_version": 2,
      "component": "agent",
      "version": "0.1.0",
      "platform": "linux",
      "arch": "x86_64",
      "build_type": "release",
      "min_installer_version": "0.1.0",
      "files": [
        {
          "source": "../payload/bin/zfleet_agent",
          "target": "bin/zfleet_agent",
          "size": 1,
          "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
          "launchable": true
        }
      ],
      "signatures": []
    })"));
  }

  SECTION("target under META") {
    REQUIRE_THROWS(zfleet::package::ParseManifestJson(R"({
      "schema_version": 2,
      "component": "agent",
      "version": "0.1.0",
      "platform": "linux",
      "arch": "x86_64",
      "build_type": "release",
      "min_installer_version": "0.1.0",
      "files": [
        {
          "source": "payload/bin/zfleet_agent",
          "target": "META/manifest.json",
          "size": 1,
          "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
          "launchable": true
        }
      ],
      "signatures": []
    })"));
  }

  SECTION("duplicate target") {
    REQUIRE_THROWS(zfleet::package::ParseManifestJson(R"({
      "schema_version": 2,
      "component": "agent",
      "version": "0.1.0",
      "platform": "linux",
      "arch": "x86_64",
      "build_type": "release",
      "min_installer_version": "0.1.0",
      "files": [
        {
          "source": "payload/bin/a",
          "target": "bin/zfleet_agent",
          "size": 1,
          "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
          "launchable": true
        },
        {
          "source": "payload/bin/b",
          "target": "bin/zfleet_agent",
          "size": 1,
          "sha256": "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
          "launchable": true
        }
      ],
      "signatures": []
    })"));
  }

  SECTION("missing build type") {
    REQUIRE_THROWS(zfleet::package::ParseManifestJson(R"({
      "schema_version": 2,
      "component": "agent",
      "version": "0.1.0",
      "platform": "linux",
      "arch": "x86_64",
      "min_installer_version": "0.1.0",
      "files": []
    })"));
  }

  SECTION("invalid build type") {
    REQUIRE_THROWS(zfleet::package::ParseManifestJson(R"({
      "schema_version": 2,
      "component": "agent",
      "version": "0.1.0",
      "platform": "linux",
      "arch": "x86_64",
      "build_type": "profile",
      "min_installer_version": "0.1.0",
      "files": []
    })"));
  }
}
