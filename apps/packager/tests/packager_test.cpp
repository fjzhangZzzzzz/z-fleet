#include "packager.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <stdexcept>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

std::atomic<int> g_test_sequence = 0;

fs::path MakeTestRoot() {
#ifndef _WIN32
  const auto process_id = getpid();
#else
  const auto process_id = 0;
#endif
  return fs::temp_directory_path() / "zfleet-packager-tests" /
         (std::to_string(process_id) + "-" +
          std::to_string(g_test_sequence.fetch_add(1)));
}

void WriteTextFile(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary);
  REQUIRE(stream);
  stream << content;
  REQUIRE(stream.good());
}

std::string ReadTextFile(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  REQUIRE(stream);
  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
}

void SetExecutable(const fs::path& path) {
  const auto mask = fs::perms::owner_exec | fs::perms::group_exec |
                    fs::perms::others_exec;
  fs::permissions(path, mask, fs::perm_options::add);
}

std::string ExpectedManifest(const std::string& component,
                             const std::string& version,
                             const std::string& min_installer_version,
                             const std::string& binary_name,
                             std::uintmax_t size_bytes,
                             const std::string& sha256) {
  std::ostringstream stream;
  stream << "{\n";
  stream << "  \"schema_version\": 1,\n";
  stream << "  \"component\": \"" << component << "\",\n";
  stream << "  \"version\": \"" << version << "\",\n";
  stream << "  \"min_installer_version\": \"" << min_installer_version
         << "\",\n";
  stream << "  \"files\": [\n";
  stream << "    {\n";
  stream << "      \"source\": \"payload/bin/" << binary_name << "\",\n";
  stream << "      \"target\": \"bin/" << binary_name << "\",\n";
  stream << "      \"size\": " << size_bytes << ",\n";
  stream << "      \"sha256\": \"" << sha256 << "\",\n";
  stream << "      \"executable\": true\n";
  stream << "    }\n";
  stream << "  ],\n";
  stream << "  \"signatures\": []\n";
  stream << "}\n";
  return stream.str();
}

} // namespace

TEST_CASE("pack-dir creates the package layout and manifest") {
  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);

  const auto binary_path = test_root / "bin" / "zfleet_agent";
  WriteTextFile(binary_path, "packager-binary");
  SetExecutable(binary_path);

  const auto output_dir = test_root / "packages";
  const auto result = zfleet::packager::PackDir(zfleet::packager::PackDirOptions{
      .component = "agent",
      .version = "1.2.3",
      .binary_path = binary_path,
      .output_dir = output_dir,
      .force = false,
  });

  const auto package_dir =
      fs::absolute(output_dir).lexically_normal() / "agent" / "1.2.3";
  const auto binary_name = zfleet::packager::BinaryNameForComponent("agent");
  const auto payload_binary = package_dir / "payload" / "bin" / binary_name;
  const auto manifest_path = package_dir / "META" / "manifest.json";

  REQUIRE(result.package_dir == package_dir);
  REQUIRE(fs::exists(payload_binary));
  REQUIRE(ReadTextFile(payload_binary) == "packager-binary");
  REQUIRE(ReadTextFile(manifest_path) == ExpectedManifest(
                                           "agent", "1.2.3", "0.1.0",
                                           binary_name, fs::file_size(payload_binary),
                                           "ffab0922a45fa94919c3d8de95f6d924aa75cb78898e41bbe7109536ad925e9d"));
#ifndef _WIN32
  REQUIRE((fs::status(payload_binary).permissions() & fs::perms::owner_exec) !=
          fs::perms::none);
#endif

  fs::remove_all(test_root);
}

TEST_CASE("pack-dir rejects existing packages unless force is set") {
  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);

  const auto binary_path = test_root / "bin" / "zfleet_server";
  WriteTextFile(binary_path, "server-binary");
  SetExecutable(binary_path);

  const auto output_dir = test_root / "packages";
  const auto package_dir =
      fs::absolute(output_dir).lexically_normal() / "server" / "2.0.0";
  fs::create_directories(package_dir / "META");
  WriteTextFile(package_dir / "META" / "stale.txt", "stale");
  WriteTextFile(output_dir / "keep.txt", "keep");

  REQUIRE_THROWS_AS(zfleet::packager::PackDir(zfleet::packager::PackDirOptions{
                        .component = "server",
                        .version = "2.0.0",
                        .binary_path = binary_path,
                        .output_dir = output_dir,
                    }),
                    std::runtime_error);

  const auto result = zfleet::packager::PackDir(zfleet::packager::PackDirOptions{
      .component = "server",
      .version = "2.0.0",
      .binary_path = binary_path,
      .output_dir = output_dir,
      .force = true,
  });

  const auto binary_name = zfleet::packager::BinaryNameForComponent("server");
  const auto payload_binary = package_dir / "payload" / "bin" / binary_name;
  REQUIRE(result.package_dir == package_dir);
  REQUIRE_FALSE(fs::exists(package_dir / "META" / "stale.txt"));
  REQUIRE(fs::exists(output_dir / "keep.txt"));
  REQUIRE(fs::exists(payload_binary));

  fs::remove_all(test_root);
}

TEST_CASE("pack-dir rejects invalid component and version values") {
  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);

  const auto binary_path = test_root / "bin" / "zfleet_installer";
  WriteTextFile(binary_path, "installer-binary");
  SetExecutable(binary_path);

  SECTION("invalid component") {
    REQUIRE_THROWS_AS(zfleet::packager::PackDir(zfleet::packager::PackDirOptions{
                          .component = "bad-component",
                          .version = "1.0.0",
                          .binary_path = binary_path,
                          .output_dir = test_root / "packages",
                      }),
                      std::invalid_argument);
  }

  SECTION("invalid version") {
    REQUIRE_THROWS_AS(zfleet::packager::PackDir(zfleet::packager::PackDirOptions{
                          .component = "installer",
                          .version = "../bad",
                          .binary_path = binary_path,
                          .output_dir = test_root / "packages",
                      }),
                      std::invalid_argument);
  }

  fs::remove_all(test_root);
}
