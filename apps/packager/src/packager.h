#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace zfleet::packager {

struct PackDirOptions {
  std::string component;
  std::string version;
  std::filesystem::path binary_path;
  std::filesystem::path output_dir;
  std::string min_installer_version = "0.1.0";
  bool force = false;
};

struct PackDirResult {
  std::filesystem::path package_dir;
};

struct PackArchiveOptions {
  std::string component;
  std::string version;
  std::filesystem::path binary_path;
  std::filesystem::path output_dir;
  std::string min_installer_version = "0.1.0";
  bool force = false;
};

struct PackArchiveResult {
  std::filesystem::path archive_path;
};

struct ArchiveDirOptions {
  std::filesystem::path package_dir;
  std::filesystem::path archive_path;
  bool force = false;
};

struct ArchiveDirResult {
  std::filesystem::path archive_path;
};

PackDirResult PackDir(const PackDirOptions& options);
PackArchiveResult PackArchive(const PackArchiveOptions& options);
ArchiveDirResult ArchiveDir(const ArchiveDirOptions& options);

bool IsSafeSegment(std::string_view value);
bool IsKnownComponent(std::string_view component);
std::string BinaryNameForComponent(std::string_view component);

} // namespace zfleet::packager
