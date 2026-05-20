#include "installer.h"

#include "manifest.h"

#include <zfleet/package/archive.h>
#include <zfleet/package/temp_dir.h>

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace zfleet::installer {
namespace {

namespace fs = std::filesystem;

struct ComponentPaths {
  fs::path component_root;
  fs::path releases_root;
  fs::path staging_root;
  fs::path var_root;
  fs::path active_version_path;
  fs::path previous_version_path;
};

struct ReleaseValidation {
  bool ok;
  std::string version;
  std::string message;
};

enum class ActiveReleaseState {
  kMissing,
  kHealthy,
  kCorrupt,
};

struct ActiveReleaseInfo {
  ActiveReleaseState state;
  std::string version;
  std::string message;
};

ComponentPaths BuildPaths(const fs::path& root, const std::string& component) {
  const auto component_root = root / "zfleet" / component;
  return ComponentPaths{
      .component_root = component_root,
      .releases_root = component_root / "releases",
      .staging_root = component_root / ".staging",
      .var_root = component_root / "var",
      .active_version_path = component_root / "var" / "active-version",
      .previous_version_path = component_root / "var" / "previous-version",
  };
}

std::string ReadFileTrimmed(const fs::path& path) {
  std::ifstream stream(path);
  if (!stream) {
    throw std::runtime_error("failed to open file: " + path.string());
  }

  std::string value{std::istreambuf_iterator<char>(stream),
                    std::istreambuf_iterator<char>()};
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  return value;
}

void WriteTextFile(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("failed to open file for write: " +
                             path.string());
  }
  stream << content;
  if (!stream) {
    throw std::runtime_error("failed to write file: " + path.string());
  }
}

bool IsSafeVersionString(const std::string& version) {
  return !version.empty() && version != "." && version != ".." &&
         version.find('/') == std::string::npos &&
         version.find('\\') == std::string::npos;
}

bool HasSymlinkInPackagePath(const fs::path& package_dir,
                             const std::string& relative_path) {
  fs::path current = package_dir;
  for (const auto& part : fs::path(relative_path)) {
    current /= part;
    if (fs::is_symlink(fs::symlink_status(current))) {
      return true;
    }
  }
  return false;
}

ReleaseValidation ValidateReleaseDirectory(const fs::path& release_dir,
                                           const std::string& component) {
  const auto manifest_path = release_dir / "META" / "manifest.json";
  if (!fs::exists(manifest_path)) {
    return ReleaseValidation{.ok = false,
                             .version = release_dir.filename().string(),
                             .message = "manifest missing"};
  }

  Manifest manifest;
  try {
    manifest = LoadManifest(manifest_path);
  } catch (const std::exception& ex) {
    return ReleaseValidation{.ok = false,
                             .version = release_dir.filename().string(),
                             .message = ex.what()};
  }

  if (manifest.component != component) {
    return ReleaseValidation{.ok = false,
                             .version = manifest.version,
                             .message = "manifest component mismatch"};
  }
  if (manifest.version != release_dir.filename().string()) {
    return ReleaseValidation{.ok = false,
                             .version = manifest.version,
                             .message = "manifest version mismatch"};
  }

  for (const auto& file : manifest.files) {
    const auto file_path = release_dir / file.target;
    const auto status = fs::symlink_status(file_path);
    if (fs::is_symlink(status)) {
      return ReleaseValidation{.ok = false,
                               .version = manifest.version,
                               .message = "release file is symlink: " +
                                              file.target};
    }
    if (!fs::exists(status) || !fs::is_regular_file(status)) {
      return ReleaseValidation{.ok = false,
                               .version = manifest.version,
                               .message = "release file missing: " +
                                              file.target};
    }

    const auto actual_size = fs::file_size(file_path);
    if (actual_size != file.size) {
      return ReleaseValidation{.ok = false,
                               .version = manifest.version,
                               .message = "size mismatch: " + file.target};
    }
    if (ComputeSha256Hex(file_path) != file.sha256) {
      return ReleaseValidation{.ok = false,
                               .version = manifest.version,
                               .message = "sha256 mismatch: " + file.target};
    }
    if (IsExecutable(file_path) != file.executable) {
      return ReleaseValidation{.ok = false,
                               .version = manifest.version,
                               .message = "executable mismatch: " +
                                              file.target};
    }
  }

  return ReleaseValidation{.ok = true,
                           .version = manifest.version,
                           .message = {}};
}

void WriteVersionFile(const fs::path& path, const std::string& version) {
  fs::create_directories(path.parent_path());
  const auto temp_path = path.string() + ".tmp";
  WriteTextFile(temp_path, version + "\n");

  std::error_code rename_error;
  fs::rename(temp_path, path, rename_error);
  if (rename_error) {
    std::error_code remove_error;
    fs::remove(path, remove_error);
    fs::rename(temp_path, path);
  }
}

void StageRelease(const fs::path& package_dir,
                  const fs::path& staging_dir,
                  const Manifest& manifest) {
  fs::remove_all(staging_dir);
  fs::create_directories(staging_dir);

  for (const auto& file : manifest.files) {
    const auto source_path = package_dir / file.source;
    if (HasSymlinkInPackagePath(package_dir, file.source)) {
      throw std::runtime_error("source path uses symlink: " + file.source);
    }

    const auto source_status = fs::symlink_status(source_path);
    if (fs::is_symlink(source_status)) {
      throw std::runtime_error("source is symlink: " + file.source);
    }
    if (!fs::exists(source_status) || !fs::is_regular_file(source_status)) {
      throw std::runtime_error("source file missing: " + file.source);
    }
    if (fs::file_size(source_path) != file.size) {
      throw std::runtime_error("size mismatch: " + file.source);
    }
    if (ComputeSha256Hex(source_path) != file.sha256) {
      throw std::runtime_error("sha256 mismatch: " + file.source);
    }

    const auto destination = staging_dir / file.target;
    fs::create_directories(destination.parent_path());
    fs::copy_file(source_path, destination, fs::copy_options::overwrite_existing);
    SetExecutable(destination, file.executable);
  }

  const auto manifest_source = package_dir / "META" / "manifest.json";
  const auto manifest_destination = staging_dir / "META" / "manifest.json";
  fs::create_directories(manifest_destination.parent_path());
  fs::copy_file(manifest_source, manifest_destination,
                fs::copy_options::overwrite_existing);
}

ActiveReleaseInfo InspectActiveRelease(const ComponentPaths& paths,
                                       const std::string& component) {
  if (!fs::exists(paths.active_version_path)) {
    return ActiveReleaseInfo{
        .state = ActiveReleaseState::kMissing,
        .version = {},
        .message = {},
    };
  }

  std::string version;
  try {
    version = ReadFileTrimmed(paths.active_version_path);
  } catch (const std::exception& ex) {
    return ActiveReleaseInfo{
        .state = ActiveReleaseState::kCorrupt,
        .version = {},
        .message = ex.what(),
    };
  }

  if (!IsSafeVersionString(version)) {
    return ActiveReleaseInfo{
        .state = ActiveReleaseState::kCorrupt,
        .version = {},
        .message = "active-version is invalid",
    };
  }

  const auto release_dir = paths.releases_root / version;
  if (!fs::exists(release_dir) || !fs::is_directory(release_dir)) {
    return ActiveReleaseInfo{
        .state = ActiveReleaseState::kCorrupt,
        .version = version,
        .message = "active release missing",
    };
  }

  const auto validation = ValidateReleaseDirectory(release_dir, component);
  if (!validation.ok) {
    return ActiveReleaseInfo{
        .state = ActiveReleaseState::kCorrupt,
        .version = validation.version,
        .message = validation.message,
    };
  }

  return ActiveReleaseInfo{
      .state = ActiveReleaseState::kHealthy,
      .version = validation.version,
      .message = {},
  };
}

bool ShouldRecordPreviousVersion(const ActiveReleaseInfo& active_release,
                                 const std::string& next_version) {
  return active_release.state == ActiveReleaseState::kHealthy &&
         active_release.version != next_version;
}

void SwitchActiveVersion(const ComponentPaths& paths,
                         const ActiveReleaseInfo& active_release,
                         const std::string& next_version) {
  if (ShouldRecordPreviousVersion(active_release, next_version)) {
    WriteVersionFile(paths.previous_version_path, active_release.version);
  }
  WriteVersionFile(paths.active_version_path, next_version);
}

RollbackResult MakeRollbackFailure(const std::string& component,
                                   const std::string& from_version,
                                   const std::string& to_version,
                                   const std::string& message) {
  return RollbackResult{
      .ok = false,
      .component = component,
      .from_version = from_version,
      .to_version = to_version,
      .message = message,
  };
}

} // namespace

bool IsKnownComponent(const std::string& component) {
  return component == "agent" || component == "server" ||
         component == "installer";
}

ApplyResult ApplyPackageDirectory(const fs::path& root,
                                  const fs::path& package_dir) {
  try {
    if (!fs::exists(package_dir) || !fs::is_directory(package_dir)) {
      throw std::runtime_error("package must be a directory");
    }

    const auto manifest = LoadManifest(package_dir / "META" / "manifest.json");
    const auto paths = BuildPaths(root, manifest.component);
    const auto active_release = InspectActiveRelease(paths, manifest.component);
    const auto release_dir = paths.releases_root / manifest.version;

    if (fs::exists(release_dir)) {
      const auto validation =
          ValidateReleaseDirectory(release_dir, manifest.component);
      if (!validation.ok) {
        return ApplyResult{.ok = false,
                           .component = manifest.component,
                           .version = manifest.version,
                           .message =
                               "existing release is unhealthy: " +
                               validation.message};
      }

      SwitchActiveVersion(paths, active_release, manifest.version);
      return ApplyResult{.ok = true,
                         .component = manifest.component,
                         .version = manifest.version,
                         .message = "release already installed"};
    }

    const auto staging_dir = paths.staging_root / manifest.version;
    try {
      StageRelease(package_dir, staging_dir, manifest);
      fs::create_directories(paths.releases_root);
      fs::rename(staging_dir, release_dir);
      SwitchActiveVersion(paths, active_release, manifest.version);
    } catch (...) {
      std::error_code cleanup_error;
      fs::remove_all(staging_dir, cleanup_error);
      throw;
    }

    return ApplyResult{.ok = true,
                       .component = manifest.component,
                       .version = manifest.version,
                       .message = "applied"};
  } catch (const std::exception& ex) {
    return ApplyResult{.ok = false,
                       .component = {},
                       .version = {},
                       .message = ex.what()};
  }
}

ApplyResult ApplyPackage(const fs::path& root, const fs::path& package_path) {
  if (fs::exists(package_path) && fs::is_directory(package_path)) {
    return ApplyPackageDirectory(root, package_path);
  }

  if (!zfleet::package::IsArchivePath(package_path)) {
    return ApplyResult{.ok = false,
                       .component = {},
                       .version = {},
                       .message = "package must be a directory or .zip archive"};
  }

  try {
    const zfleet::package::ScopedTempDir temp_dir("zfleet-installer");

    zfleet::package::ExtractArchive(
        {.archive_path = package_path,
         .output_dir = temp_dir.path(),
         .force = true});

    return ApplyPackageDirectory(root, temp_dir.path());
  } catch (const std::exception& ex) {
    return ApplyResult{.ok = false,
                       .component = {},
                       .version = {},
                       .message = ex.what()};
  }
}

RollbackResult RollbackComponent(const fs::path& root,
                                 const std::string& component) {
  if (!IsKnownComponent(component)) {
    throw std::invalid_argument("unknown component: " + component);
  }

  const auto paths = BuildPaths(root, component);
  const auto active_release = InspectActiveRelease(paths, component);
  if (active_release.state == ActiveReleaseState::kMissing) {
    return MakeRollbackFailure(component, {}, {}, "active release is not installed");
  }
  if (active_release.state == ActiveReleaseState::kCorrupt) {
    return MakeRollbackFailure(component, active_release.version, {},
                               "active release is corrupt: " +
                                   active_release.message);
  }

  if (!fs::exists(paths.previous_version_path)) {
    return MakeRollbackFailure(component, active_release.version, {},
                               "previous-version is missing");
  }

  std::string previous_version;
  try {
    previous_version = ReadFileTrimmed(paths.previous_version_path);
  } catch (const std::exception& ex) {
    return MakeRollbackFailure(component, active_release.version, {},
                               ex.what());
  }

  if (!IsSafeVersionString(previous_version)) {
    return MakeRollbackFailure(component, active_release.version,
                               previous_version,
                               "previous-version is invalid");
  }
  if (previous_version == active_release.version) {
    return MakeRollbackFailure(component, active_release.version,
                               previous_version,
                               "previous-version matches active-version");
  }

  const auto previous_release_dir = paths.releases_root / previous_version;
  if (!fs::exists(previous_release_dir) || !fs::is_directory(previous_release_dir)) {
    return MakeRollbackFailure(component, active_release.version,
                               previous_version,
                               "previous release missing");
  }

  const auto validation =
      ValidateReleaseDirectory(previous_release_dir, component);
  if (!validation.ok) {
    return MakeRollbackFailure(component, active_release.version,
                               previous_version,
                               "previous release is unhealthy: " +
                                   validation.message);
  }

  try {
    WriteVersionFile(paths.previous_version_path, active_release.version);
    WriteVersionFile(paths.active_version_path, previous_version);
  } catch (const std::exception& ex) {
    return MakeRollbackFailure(component, active_release.version,
                               previous_version, ex.what());
  }

  return RollbackResult{
      .ok = true,
      .component = component,
      .from_version = active_release.version,
      .to_version = previous_version,
      .message = "rolled back",
  };
}

StatusResult GetStatus(const fs::path& root, const std::string& component) {
  if (!IsKnownComponent(component)) {
    throw std::invalid_argument("unknown component: " + component);
  }

  const auto paths = BuildPaths(root, component);
  const auto active_release = InspectActiveRelease(paths, component);
  if (active_release.state == ActiveReleaseState::kMissing) {
    return StatusResult{
        .component = component,
        .state = "not_installed",
        .version = std::nullopt,
        .message = std::nullopt,
    };
  }
  if (active_release.state == ActiveReleaseState::kCorrupt) {
    return StatusResult{
        .component = component,
        .state = "corrupt",
        .version = active_release.version.empty()
                       ? std::nullopt
                       : std::optional<std::string>{active_release.version},
        .message = active_release.message,
    };
  }

  return StatusResult{
      .component = component,
      .state = "installed",
      .version = active_release.version,
      .message = std::nullopt,
  };
}

} // namespace zfleet::installer
