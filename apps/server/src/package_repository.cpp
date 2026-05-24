#include "package_repository.h"

#include <zfleet/crypto/sha256.h>
#include <zfleet/package/archive.h>
#include <zfleet/package/manifest.h>

#include <exception>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zfleet::server {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kManifestPath = "META/manifest.json";
constexpr std::string_view kAgentComponent = "agent";

std::string BytesToString(const std::vector<std::uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

std::vector<std::uint8_t> ReadArchiveBytes(
    const fs::path& archive_path,
    std::string_view archive_file_path) {
  try {
    return zfleet::package::ReadArchiveFile(archive_path, archive_file_path);
  } catch (const std::exception&) {
    throw std::runtime_error("failed to read archive file: " +
                             std::string(archive_file_path));
  }
}

std::string ReadArchiveText(const fs::path& archive_path,
                            std::string_view archive_file_path) {
  return BytesToString(ReadArchiveBytes(archive_path, archive_file_path));
}

std::unordered_map<std::string, zfleet::package::ArchiveEntry> IndexEntries(
    const std::vector<zfleet::package::ArchiveEntry>& entries) {
  std::unordered_map<std::string, zfleet::package::ArchiveEntry> indexed;
  indexed.reserve(entries.size());
  for (const auto& entry : entries) {
    indexed.emplace(entry.path, entry);
  }
  return indexed;
}

std::uint64_t FileSizeBytes(const fs::path& path) {
  try {
    return static_cast<std::uint64_t>(fs::file_size(path));
  } catch (...) {
    throw std::runtime_error("failed to determine package size: " +
                             path.string());
  }
}

void ValidateManifestFile(const fs::path& archive_path,
                          const std::unordered_map<std::string,
                                                   zfleet::package::ArchiveEntry>&
                              archive_entries,
                          const zfleet::package::ManifestFile& file) {
  if (archive_entries.find(file.source) == archive_entries.end()) {
    throw std::invalid_argument("manifest file missing from archive: " +
                                file.source);
  }

  const auto file_bytes = ReadArchiveBytes(archive_path, file.source);
  if (file_bytes.size() != file.size) {
    throw std::invalid_argument("manifest file size mismatch: " + file.source);
  }

  const auto file_sha256 = zfleet::crypto::Sha256BytesHex(
      std::string_view(reinterpret_cast<const char*>(file_bytes.data()),
                       file_bytes.size()));
  if (file_sha256 != file.sha256) {
    throw std::invalid_argument("manifest file sha256 mismatch: " + file.source);
  }
}

}  // namespace

AgentPackageMetadata ValidateAgentPackageUpload(
    const fs::path& staged_package_path) {
  if (!fs::exists(staged_package_path)) {
    throw std::runtime_error("package archive does not exist: " +
                             staged_package_path.string());
  }
  if (!fs::is_regular_file(staged_package_path)) {
    throw std::runtime_error("package archive is not a file: " +
                             staged_package_path.string());
  }

  std::unordered_map<std::string, zfleet::package::ArchiveEntry> archive_entries;
  try {
    archive_entries =
        IndexEntries(zfleet::package::ListArchiveEntries(staged_package_path));
  } catch (const std::exception&) {
    throw std::runtime_error("failed to list archive entries: " +
                             staged_package_path.string());
  }
  if (archive_entries.find(std::string(kManifestPath)) ==
      archive_entries.end()) {
    throw std::invalid_argument("package archive missing META/manifest.json");
  }

  const auto manifest_json = ReadArchiveText(staged_package_path, kManifestPath);
  const auto manifest = zfleet::package::ParseManifestJson(manifest_json);
  if (manifest.component != kAgentComponent) {
    throw std::invalid_argument("manifest component must be agent");
  }

  for (const auto& file : manifest.files) {
    ValidateManifestFile(staged_package_path, archive_entries, file);
  }

  AgentPackageMetadata metadata{
      .component = std::move(manifest.component),
      .version = std::move(manifest.version),
      .min_installer_version = std::move(manifest.min_installer_version),
      .size_bytes = FileSizeBytes(staged_package_path),
      .sha256 = zfleet::crypto::Sha256FileHex(staged_package_path),
      .manifest_json = std::move(manifest_json),
  };
  return metadata;
}

}  // namespace zfleet::server
