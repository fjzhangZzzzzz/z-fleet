#include "packager.h"

#include "test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <openssl/evp.h>

#include <zfleet/package/archive.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using zfleet::test::SetExecutable;

std::string ComputeSha256Hex(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  REQUIRE(stream);

  using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  MdCtxPtr context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  REQUIRE(context);
  REQUIRE(EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) == 1);

  std::array<char, 8192> buffer{};
  while (stream.good()) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto bytes_read = stream.gcount();
    if (bytes_read <= 0) {
      break;
    }
    REQUIRE(EVP_DigestUpdate(context.get(), buffer.data(),
                             static_cast<std::size_t>(bytes_read)) == 1);
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_length = 0;
  REQUIRE(EVP_DigestFinal_ex(context.get(), digest.data(), &digest_length) ==
          1);

  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(digest_length * 2);
  for (unsigned int index = 0; index < digest_length; ++index) {
    const auto value = digest[index];
    hex.push_back(kHexDigits[(value >> 4U) & 0x0fU]);
    hex.push_back(kHexDigits[value & 0x0fU]);
  }
  return hex;
}

void ExtractArchiveTo(const fs::path& archive_path, const fs::path& output_dir) {
  zfleet::package::ExtractArchive(
      {.archive_path = archive_path, .output_dir = output_dir, .force = true});
}

struct ExpectedFile {
  std::string relative_path;
  fs::path path;
  bool executable = false;
};

std::string ExpectedManifest(const std::string& component,
                             const std::string& version,
                             const std::string& min_installer_version,
                             const std::vector<ExpectedFile>& files) {
  std::ostringstream stream;
  stream << "{\n";
  stream << "  \"schema_version\": 1,\n";
  stream << "  \"component\": \"" << component << "\",\n";
  stream << "  \"version\": \"" << version << "\",\n";
  stream << "  \"min_installer_version\": \"" << min_installer_version
         << "\",\n";
  stream << "  \"files\": [\n";
  for (std::size_t index = 0; index < files.size(); ++index) {
    const auto& file = files[index];
    stream << "    {\n";
    stream << "      \"source\": \"payload/" << file.relative_path << "\",\n";
    stream << "      \"target\": \"" << file.relative_path << "\",\n";
    stream << "      \"size\": " << fs::file_size(file.path) << ",\n";
    stream << "      \"sha256\": \"" << ComputeSha256Hex(file.path) << "\",\n";
    stream << "      \"executable\": "
           << (file.executable ? "true" : "false") << "\n";
    stream << "    }" << (index + 1 == files.size() ? "\n" : ",\n");
  }
  stream << "  ],\n";
  stream << "  \"signatures\": []\n";
  stream << "}\n";
  return stream.str();
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
  SetExecutable(binary_path);

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
          ExpectedManifest("agent", "1.2.3", "0.1.0",
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
  REQUIRE((fs::status(package_dir / "payload" / "bin" / "zfleet_agent")
               .permissions() &
           fs::perms::owner_exec) != fs::perms::none);
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
  SetExecutable(binary_path);

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
          ExpectedManifest("server", "3.4.5", "0.1.0",
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
  SetExecutable(binary_path);

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
  SetExecutable(binary_path);

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
  SetExecutable(binary_path);

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
