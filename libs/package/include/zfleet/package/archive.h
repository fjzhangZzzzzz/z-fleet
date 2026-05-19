#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace zfleet::package {

struct ArchiveEntry {
  std::string path;
  bool executable = false;
  std::uint64_t compressed_size = 0;
  std::uint64_t uncompressed_size = 0;
};

struct CreateArchiveOptions {
  std::filesystem::path package_dir;
  std::filesystem::path archive_path;
  bool force = false;
};

struct ExtractArchiveOptions {
  std::filesystem::path archive_path;
  std::filesystem::path output_dir;
  bool force = false;
};

void CreateArchive(const CreateArchiveOptions& options);
void ExtractArchive(const ExtractArchiveOptions& options);
bool IsArchivePath(const std::filesystem::path& path);
std::vector<ArchiveEntry> ListArchiveEntries(const std::filesystem::path& archive_path);
std::vector<std::uint8_t> ReadArchiveFile(const std::filesystem::path& archive_path,
                                          std::string_view path);

} // namespace zfleet::package
