#include "packager.h"

#include <zfleet/core/component.h>
#include <zfleet/core/path.h>
#include <zfleet/crypto/sha256.h>
#include <zfleet/package/archive.h>
#include <zfleet/package/manifest.h>
#include <zfleet/package/temp_dir.h>
#include <zfleet/platform/file_permissions.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zfleet::packager {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kManifestRelativePath = "META/manifest.json";

struct PackagePaths {
  fs::path package_dir;
  fs::path manifest_dir;
  fs::path payload_dir;
  fs::path manifest_path;
};

struct PayloadFile {
  std::string relative_path;
  fs::path source_path;
  std::uintmax_t size_bytes = 0;
  std::string sha256_hex;
  bool executable = false;
};

std::string BuildManifestJson(const std::string& component,
                              const std::string& version,
                              const std::string& platform,
                              const std::string& arch,
                              const std::string& min_installer_version,
                              const std::vector<PayloadFile>& files) {
  zfleet::package::Manifest manifest{
      .schema_version = 1,
      .component = component,
      .version = version,
      .platform = platform,
      .arch = arch,
      .min_installer_version = min_installer_version,
      .files = {},
  };
  manifest.files.reserve(files.size());
  for (const auto& file : files) {
    manifest.files.push_back(zfleet::package::ManifestFile{
        .source = std::string("payload/") + file.relative_path,
        .target = file.relative_path,
        .size = static_cast<std::uint64_t>(file.size_bytes),
        .sha256 = file.sha256_hex,
        .executable = file.executable,
    });
  }
  return zfleet::package::SerializeManifestJson(manifest);
}

std::string ValidateComponent(std::string_view component) {
  const auto validation = zfleet::core::ValidateComponent(component);
  if (validation.ok) {
    return std::string(component);
  }
  throw std::invalid_argument("invalid --component: " + validation.message);
}

fs::path NormalizePath(const fs::path& path) {
  return fs::absolute(path).lexically_normal();
}

PackagePaths BuildPackagePaths(const std::string& component,
                               const std::string& version,
                               const fs::path& output_dir) {
  const auto package_dir = output_dir / component / version;
  return PackagePaths{
      .package_dir = package_dir,
      .manifest_dir = package_dir / "META",
      .payload_dir = package_dir / "payload",
      .manifest_path = package_dir / fs::path(kManifestRelativePath),
  };
}

fs::path ArchivePathForPackageDir(const fs::path& package_dir) {
  auto archive_name = package_dir.filename();
  archive_name += ".zip";
  return package_dir.parent_path() / archive_name;
}

void ValidatePayloadDir(const fs::path& payload_dir) {
  const auto status = fs::symlink_status(payload_dir);
  if (fs::is_symlink(status)) {
    throw std::runtime_error("payload directory cannot be a symlink: " +
                             payload_dir.string());
  }
  if (!fs::exists(status) || !fs::is_directory(status)) {
    throw std::runtime_error("payload directory must be an existing directory: " +
                             payload_dir.string());
  }
}

std::vector<PayloadFile> CollectPayloadFiles(const fs::path& payload_dir,
                                             std::string_view entry_path) {
  std::vector<PayloadFile> files;
  std::unordered_set<std::string> seen_paths;
  bool found_entry = false;

  for (fs::recursive_directory_iterator it(payload_dir), end; it != end; ++it) {
    const auto status = it->symlink_status();
    if (fs::is_symlink(status)) {
      throw std::runtime_error("symlink is not allowed in payload directory: " +
                               it->path().string());
    }
    if (fs::is_directory(status)) {
      continue;
    }
    if (!fs::is_regular_file(status)) {
      throw std::runtime_error("unsupported file type in payload directory: " +
                               it->path().string());
    }

    const auto relative_path = it->path().lexically_relative(payload_dir);
    const auto relative_raw = relative_path.generic_string();
    const auto normalized = zfleet::core::ValidateRelativePath(relative_raw);
    if (!normalized.ok) {
      throw std::runtime_error("payload path is not safe: " +
                               normalized.message + ": " + relative_raw);
    }
    const auto& relative_string = normalized.value;
    if (relative_string == "META" || relative_string.starts_with("META/")) {
      throw std::runtime_error("payload target must not write META: " +
                               relative_string);
    }
    if (!seen_paths.insert(relative_string).second) {
      throw std::runtime_error("duplicate payload path: " + relative_string);
    }

    PayloadFile file;
    file.relative_path = relative_string;
    file.source_path = it->path();
    file.size_bytes = fs::file_size(it->path());
    file.sha256_hex = zfleet::crypto::Sha256FileHex(it->path());
    file.executable = relative_string == entry_path ||
                      zfleet::platform::HasExecutablePermission(
                          fs::status(it->path()).permissions());
    if (relative_string == entry_path) {
      found_entry = true;
    }
    files.push_back(std::move(file));
  }

  std::sort(files.begin(), files.end(),
            [](const PayloadFile& lhs, const PayloadFile& rhs) {
              return lhs.relative_path < rhs.relative_path;
            });

  if (files.empty()) {
    throw std::runtime_error("payload directory must contain at least one file");
  }
  if (!found_entry) {
    throw std::runtime_error("entry file is missing from payload directory: " +
                             std::string(entry_path));
  }
  return files;
}

void CopyPayloadFiles(const fs::path& package_payload_dir,
                      const std::vector<PayloadFile>& files) {
  for (const auto& file : files) {
    const auto target_path = package_payload_dir / fs::path(file.relative_path);
    if (const auto parent = target_path.parent_path(); !parent.empty()) {
      fs::create_directories(parent);
    }
    if (!fs::copy_file(file.source_path, target_path,
                       fs::copy_options::overwrite_existing)) {
      throw std::runtime_error("failed to copy payload file: " +
                               file.source_path.string());
    }
#ifndef _WIN32
    if (file.executable) {
      zfleet::platform::SetExecutable(target_path, true);
    }
#endif
  }
}

PackResult PackToDirectory(const PackOptions& options,
                           const fs::path& output_dir_abs) {
  const auto component = ValidateComponent(options.component);
  const auto version = zfleet::core::ValidatePathSegment(options.version);
  if (!version.ok) {
    throw std::invalid_argument("invalid --version: " + version.message);
  }
  const auto min_installer_version =
      zfleet::core::ValidatePathSegment(options.min_installer_version);
  if (!min_installer_version.ok) {
    throw std::invalid_argument(
        "invalid --min-installer-version: " + min_installer_version.message);
  }
  const auto platform = zfleet::core::ValidatePathSegment(options.platform);
  const auto arch = zfleet::core::ValidatePathSegment(options.arch);
  if (!platform.ok || !arch.ok) {
    throw std::invalid_argument("platform and arch must be safe non-empty values");
  }

  const auto payload_dir = NormalizePath(options.payload_dir);
  ValidatePayloadDir(payload_dir);

  const auto normalized_entry =
      zfleet::core::ValidateRelativePath(options.entry_path.generic_string());
  if (!normalized_entry.ok) {
    throw std::invalid_argument("invalid --entry: " + normalized_entry.message);
  }

  const auto files = CollectPayloadFiles(payload_dir, normalized_entry.value);
  const auto paths = BuildPackagePaths(component, options.version, output_dir_abs);

  if (fs::exists(paths.package_dir)) {
    if (!options.force) {
      throw std::runtime_error("package already exists: " +
                               paths.package_dir.string() +
                               " (use --force to overwrite)");
    }
    fs::remove_all(paths.package_dir);
  }

  fs::create_directories(paths.manifest_dir);
  fs::create_directories(paths.payload_dir);
  CopyPayloadFiles(paths.payload_dir, files);

  const auto manifest_text =
      BuildManifestJson(component, options.version, options.platform, options.arch,
                        options.min_installer_version, files);
  std::ofstream manifest_stream(paths.manifest_path, std::ios::binary);
  if (!manifest_stream) {
    throw std::runtime_error("failed to open manifest for write: " +
                             paths.manifest_path.string());
  }
  manifest_stream << manifest_text;
  if (!manifest_stream) {
    throw std::runtime_error("failed to write manifest: " +
                             paths.manifest_path.string());
  }

  return PackResult{.package_path = paths.package_dir, .archive = false};
}

} // namespace

PackResult Pack(const PackOptions& options) {
  const auto output_dir_abs = NormalizePath(options.output_dir);
  if (!options.archive) {
    return PackToDirectory(options, output_dir_abs);
  }

  const auto component = ValidateComponent(options.component);
  const auto version = zfleet::core::ValidatePathSegment(options.version);
  if (!version.ok) {
    throw std::invalid_argument("invalid --version: " + version.message);
  }

  const auto archive_path =
      ArchivePathForPackageDir(output_dir_abs / component / options.version);
  fs::create_directories(archive_path.parent_path());
  if (fs::exists(archive_path) && !options.force) {
    throw std::runtime_error("archive already exists: " + archive_path.string() +
                             " (use --force to overwrite)");
  }

  const zfleet::package::ScopedTempDir temp_root("zfleet-packager");
  const auto pack_result =
      PackToDirectory(PackOptions{
                          .component = options.component,
                          .version = options.version,
                          .platform = options.platform,
                          .arch = options.arch,
                          .payload_dir = options.payload_dir,
                          .entry_path = options.entry_path,
                          .output_dir = temp_root.path(),
                          .min_installer_version =
                              options.min_installer_version,
                          .archive = false,
                          .force = true,
                      },
                      temp_root.path());

  zfleet::package::CreateArchive(zfleet::package::CreateArchiveOptions{
      .package_dir = pack_result.package_path,
      .archive_path = archive_path,
      .force = options.force,
  });
  return PackResult{.package_path = NormalizePath(archive_path), .archive = true};
}

} // namespace zfleet::packager
