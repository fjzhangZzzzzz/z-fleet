#include "zfleet/package/archive.h"
#include "zfleet/package/temp_dir.h"
#include "zfleet/platform/file_permissions.h"

#include "test_util.h"

#include <catch2/catch_test_macros.hpp>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using zfleet::test::ReadTextFile;
using zfleet::test::WriteTextFile;

struct StoredZipEntry {
  std::string path;
  std::string contents;
  std::uint32_t unix_mode = 0100644U;
};

void WriteU16(std::ostream& stream, std::uint16_t value) {
  stream.put(static_cast<char>(value & 0xffU));
  stream.put(static_cast<char>((value >> 8) & 0xffU));
}

void WriteU32(std::ostream& stream, std::uint32_t value) {
  stream.put(static_cast<char>(value & 0xffU));
  stream.put(static_cast<char>((value >> 8) & 0xffU));
  stream.put(static_cast<char>((value >> 16) & 0xffU));
  stream.put(static_cast<char>((value >> 24) & 0xffU));
}

void WriteStoredZip(const fs::path& archive_path,
                    const std::vector<StoredZipEntry>& entries) {
  fs::create_directories(archive_path.parent_path());
  std::ofstream stream(archive_path, std::ios::binary | std::ios::trunc);
  REQUIRE(stream);

  struct CentralRecord {
    StoredZipEntry entry;
    std::uint32_t crc32_value = 0;
    std::uint32_t local_header_offset = 0;
  };
  std::vector<CentralRecord> central_records;

  for (const auto& entry : entries) {
    const auto header_offset = stream.tellp();
    REQUIRE(header_offset != std::streampos(-1));
    const auto crc32_value = static_cast<std::uint32_t>(
        crc32(0L, reinterpret_cast<const Bytef*>(entry.contents.data()),
              static_cast<uInt>(entry.contents.size())));

    WriteU32(stream, 0x04034b50U);
    WriteU16(stream, 20U);
    WriteU16(stream, 0x0800U);
    WriteU16(stream, 0U);
    WriteU16(stream, 0U);
    WriteU16(stream, 0U);
    WriteU32(stream, crc32_value);
    WriteU32(stream, static_cast<std::uint32_t>(entry.contents.size()));
    WriteU32(stream, static_cast<std::uint32_t>(entry.contents.size()));
    WriteU16(stream, static_cast<std::uint16_t>(entry.path.size()));
    WriteU16(stream, 0U);
    stream.write(entry.path.data(),
                 static_cast<std::streamsize>(entry.path.size()));
    stream.write(entry.contents.data(),
                 static_cast<std::streamsize>(entry.contents.size()));
    REQUIRE(stream);

    central_records.push_back(CentralRecord{
        .entry = entry,
        .crc32_value = crc32_value,
        .local_header_offset = static_cast<std::uint32_t>(header_offset),
    });
  }

  const auto central_directory_offset = stream.tellp();
  REQUIRE(central_directory_offset != std::streampos(-1));

  for (const auto& record : central_records) {
    const auto& entry = record.entry;
    WriteU32(stream, 0x02014b50U);
    WriteU16(stream, static_cast<std::uint16_t>((3U << 8U) | 20U));
    WriteU16(stream, 20U);
    WriteU16(stream, 0x0800U);
    WriteU16(stream, 0U);
    WriteU16(stream, 0U);
    WriteU16(stream, 0U);
    WriteU32(stream, record.crc32_value);
    WriteU32(stream, static_cast<std::uint32_t>(entry.contents.size()));
    WriteU32(stream, static_cast<std::uint32_t>(entry.contents.size()));
    WriteU16(stream, static_cast<std::uint16_t>(entry.path.size()));
    WriteU16(stream, 0U);
    WriteU16(stream, 0U);
    WriteU16(stream, 0U);
    WriteU16(stream, 0U);
    WriteU32(stream, entry.unix_mode << 16U);
    WriteU32(stream, record.local_header_offset);
    stream.write(entry.path.data(),
                 static_cast<std::streamsize>(entry.path.size()));
    REQUIRE(stream);
  }

  const auto central_directory_end = stream.tellp();
  REQUIRE(central_directory_end != std::streampos(-1));
  const auto central_directory_size = static_cast<std::uint32_t>(
      central_directory_end - central_directory_offset);

  WriteU32(stream, 0x06054b50U);
  WriteU16(stream, 0U);
  WriteU16(stream, 0U);
  WriteU16(stream, static_cast<std::uint16_t>(entries.size()));
  WriteU16(stream, static_cast<std::uint16_t>(entries.size()));
  WriteU32(stream, central_directory_size);
  WriteU32(stream, static_cast<std::uint32_t>(central_directory_offset));
  WriteU16(stream, 0U);
  REQUIRE(stream);
}

}  // namespace

TEST_CASE("create list read and extract zip archive") {
  const zfleet::test::ScopedTestDir test_dir("package");
  const auto test_root = test_dir.path();

  const auto package_dir = test_root / "package";
  WriteTextFile(package_dir / "META" / "manifest.json",
                R"({"component":"agent"})");
  WriteTextFile(package_dir / "payload" / "bin" / "zfleet_agent",
                "agent-binary");
#ifndef _WIN32
  zfleet::platform::SetExecutable(
      package_dir / "payload" / "bin" / "zfleet_agent", true);
#endif

  const auto archive_path = test_root / "package.zip";
  REQUIRE(zfleet::package::IsArchivePath(archive_path));
  REQUIRE_FALSE(zfleet::package::IsArchivePath(test_root / "package.tar"));

  zfleet::package::CreateArchive({.package_dir = package_dir,
                                  .archive_path = archive_path,
                                  .force = false});

  const auto entries = zfleet::package::ListArchiveEntries(archive_path);
  REQUIRE(entries.size() == 2);
  REQUIRE(std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
    return entry.path == "META/manifest.json" && entry.uncompressed_size > 0U;
  }));
#ifndef _WIN32
  REQUIRE(std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
    return entry.path == "payload/bin/zfleet_agent" && entry.executable;
  }));
#else
  REQUIRE(std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
    return entry.path == "payload/bin/zfleet_agent" && !entry.executable;
  }));
#endif

  const auto manifest =
      zfleet::package::ReadArchiveFile(archive_path, "META/manifest.json");
  REQUIRE(std::string(manifest.begin(), manifest.end()) ==
          R"({"component":"agent"})");
  REQUIRE_THROWS_AS(
      zfleet::package::ReadArchiveFile(archive_path, "missing.txt"),
      std::runtime_error);

  const auto output_dir = test_root / "extracted";
  zfleet::package::ExtractArchive(
      {.archive_path = archive_path, .output_dir = output_dir, .force = false});
  REQUIRE(ReadTextFile(output_dir / "META" / "manifest.json") ==
          R"({"component":"agent"})");
  REQUIRE(ReadTextFile(output_dir / "payload" / "bin" / "zfleet_agent") ==
          "agent-binary");
  REQUIRE(zfleet::platform::IsLaunchableProgram(output_dir / "payload" / "bin" /
                                                "zfleet_agent"));
}

TEST_CASE("archive create and extract honor force overwrite") {
  const zfleet::test::ScopedTestDir test_dir("package");
  const auto test_root = test_dir.path();

  const auto package_dir = test_root / "package";
  WriteTextFile(package_dir / "META" / "manifest.json", "{}");
  WriteTextFile(package_dir / "payload" / "file.txt", "payload");

  const auto archive_path = test_root / "package.zip";
  zfleet::package::CreateArchive({.package_dir = package_dir,
                                  .archive_path = archive_path,
                                  .force = false});
  REQUIRE_THROWS_AS(
      zfleet::package::CreateArchive({.package_dir = package_dir,
                                      .archive_path = archive_path,
                                      .force = false}),
      std::runtime_error);
  zfleet::package::CreateArchive({.package_dir = package_dir,
                                  .archive_path = archive_path,
                                  .force = true});

  const auto output_dir = test_root / "output";
  zfleet::package::ExtractArchive(
      {.archive_path = archive_path, .output_dir = output_dir, .force = false});
  REQUIRE_THROWS_AS(
      zfleet::package::ExtractArchive({.archive_path = archive_path,
                                       .output_dir = output_dir,
                                       .force = false}),
      std::runtime_error);
  zfleet::package::ExtractArchive(
      {.archive_path = archive_path, .output_dir = output_dir, .force = true});
}

TEST_CASE("create archive rejects output inside the package tree") {
  const zfleet::test::ScopedTestDir test_dir("package");
  const auto test_root = test_dir.path();

  const auto package_dir = test_root / "package";
  WriteTextFile(package_dir / "META" / "manifest.json", "{}");
  WriteTextFile(package_dir / "payload" / "file.txt", "payload");

  REQUIRE_THROWS_AS(
      zfleet::package::CreateArchive(
          {.package_dir = package_dir,
           .archive_path = package_dir / "payload" / "package.zip",
           .force = false}),
      std::runtime_error);
}

TEST_CASE("zip reader rejects unsafe duplicate and symlink entries") {
  const zfleet::test::ScopedTestDir test_dir("package");
  const auto test_root = test_dir.path();

  const auto unsafe_archive = test_root / "unsafe.zip";
  WriteStoredZip(unsafe_archive, {StoredZipEntry{.path = "../escape.txt",
                                                 .contents = "escape"}});
  REQUIRE_THROWS_AS(zfleet::package::ListArchiveEntries(unsafe_archive),
                    std::runtime_error);

  const auto duplicate_archive = test_root / "duplicate.zip";
  WriteStoredZip(
      duplicate_archive,
      {StoredZipEntry{.path = "payload/file.txt", .contents = "one"},
       StoredZipEntry{.path = "payload/file.txt", .contents = "two"}});
  REQUIRE_THROWS_AS(zfleet::package::ListArchiveEntries(duplicate_archive),
                    std::runtime_error);

  const auto symlink_archive = test_root / "symlink.zip";
  WriteStoredZip(symlink_archive, {StoredZipEntry{.path = "payload/link",
                                                  .contents = "target",
                                                  .unix_mode = 0120777U}});
  REQUIRE_THROWS_AS(zfleet::package::ListArchiveEntries(symlink_archive),
                    std::runtime_error);
}

TEST_CASE("create archive rejects symlinks in package tree") {
  const zfleet::test::ScopedTestDir test_dir("package");
  const auto test_root = test_dir.path();

  const auto package_dir = test_root / "package";
  WriteTextFile(package_dir / "META" / "manifest.json", "{}");
  WriteTextFile(package_dir / "payload" / "file.txt", "payload");

#ifdef _WIN32
#else
  std::error_code error;
  fs::create_symlink(package_dir / "payload" / "file.txt",
                     package_dir / "payload" / "link.txt", error);
  if (error) {
    SUCCEED("filesystem does not permit symlink creation in this environment");
    return;
  }

  REQUIRE_THROWS_AS(
      zfleet::package::CreateArchive({.package_dir = package_dir,
                                      .archive_path = test_root / "package.zip",
                                      .force = false}),
      std::runtime_error);
#endif
}

TEST_CASE("scoped temp dir removes child and empty base directory") {
  fs::path temp_path;
  fs::path base_path;
  {
    const zfleet::package::ScopedTempDir temp_dir(
        "zfleet-package-temp-dir-test");
    temp_path = temp_dir.path();
    base_path = temp_path.parent_path();

    REQUIRE(fs::exists(temp_path));
    WriteTextFile(temp_path / "file.txt", "temporary");
  }

  REQUIRE_FALSE(fs::exists(temp_path));
  REQUIRE_FALSE(fs::exists(base_path));
}
