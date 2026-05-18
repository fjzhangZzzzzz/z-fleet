#include "installer.h"

#include "manifest.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
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
};

struct ReleaseValidation {
  bool ok;
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

void WriteActiveVersion(const ComponentPaths& paths, const std::string& version) {
  fs::create_directories(paths.var_root);
  const auto temp_path = paths.active_version_path.string() + ".tmp";
  WriteTextFile(temp_path, version + "\n");

  std::error_code rename_error;
  fs::rename(temp_path, paths.active_version_path, rename_error);
  if (rename_error) {
    std::error_code remove_error;
    fs::remove(paths.active_version_path, remove_error);
    fs::rename(temp_path, paths.active_version_path);
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

} // namespace

bool IsKnownComponent(const std::string& component) {
  return component == "agent" || component == "server" ||
         component == "installer";
}

ApplyResult ApplyPackage(const fs::path& root, const fs::path& package_dir) {
  try {
    if (!fs::exists(package_dir) || !fs::is_directory(package_dir)) {
      throw std::runtime_error("package must be a directory");
    }

    const auto manifest = LoadManifest(package_dir / "META" / "manifest.json");
    const auto paths = BuildPaths(root, manifest.component);
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

      WriteActiveVersion(paths, manifest.version);
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
      WriteActiveVersion(paths, manifest.version);
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

StatusResult GetStatus(const fs::path& root, const std::string& component) {
  if (!IsKnownComponent(component)) {
    throw std::invalid_argument("unknown component: " + component);
  }

  const auto paths = BuildPaths(root, component);
  if (!fs::exists(paths.active_version_path)) {
    return StatusResult{
        .component = component,
        .state = "not_installed",
        .version = std::nullopt,
        .message = std::nullopt,
    };
  }

  std::string version;
  try {
    version = ReadFileTrimmed(paths.active_version_path);
  } catch (const std::exception& ex) {
    return StatusResult{
        .component = component,
        .state = "corrupt",
        .version = std::nullopt,
        .message = ex.what(),
    };
  }

  if (!IsSafeVersionString(version)) {
    return StatusResult{
        .component = component,
        .state = "corrupt",
        .version = std::nullopt,
        .message = "active-version is invalid",
    };
  }

  const auto release_dir = paths.releases_root / version;
  if (!fs::exists(release_dir) || !fs::is_directory(release_dir)) {
    return StatusResult{
        .component = component,
        .state = "corrupt",
        .version = version,
        .message = "active release missing",
    };
  }

  const auto validation = ValidateReleaseDirectory(release_dir, component);
  if (!validation.ok) {
    return StatusResult{
        .component = component,
        .state = "corrupt",
        .version = validation.version,
        .message = validation.message,
    };
  }

  return StatusResult{
      .component = component,
      .state = "installed",
      .version = validation.version,
      .message = std::nullopt,
  };
}

} // namespace zfleet::installer
