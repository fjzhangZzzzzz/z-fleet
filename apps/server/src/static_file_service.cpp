#include "static_file_service.h"

#include "zfleet/core/path.h"

#include <array>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace zfleet::server {
namespace {

namespace fs = std::filesystem;

constexpr std::array<std::string_view, 8> kRequiredFiles = {
    "index.html",
    "install.html",
    "agents.html",
    "admin/packages.html",
    "assets/admin.css",
    "assets/admin.js",
    "scripts/install/linux.sh",
    "scripts/install/windows.ps1",
};

bool IsPermittedAssetExtension(const fs::path& path) {
  const auto extension = path.extension().string();
  return extension == ".css" || extension == ".js" || extension == ".svg" ||
         extension == ".png" || extension == ".ico" ||
         extension == ".woff2";
}

std::string ContentTypeForPath(const fs::path& path) {
  const auto extension = path.extension().string();
  if (extension == ".html") {
    return "text/html; charset=utf-8";
  }
  if (extension == ".css") {
    return "text/css; charset=utf-8";
  }
  if (extension == ".js") {
    return "text/javascript; charset=utf-8";
  }
  if (extension == ".svg") {
    return "image/svg+xml";
  }
  if (extension == ".png") {
    return "image/png";
  }
  if (extension == ".ico") {
    return "image/x-icon";
  }
  if (extension == ".woff2") {
    return "font/woff2";
  }
  if (extension == ".sh") {
    return "text/plain; charset=utf-8";
  }
  if (extension == ".ps1") {
    return "text/plain; charset=utf-8";
  }
  return "application/octet-stream";
}

}  // namespace

StaticFileService::StaticFileService(fs::path root)
    : root_(fs::absolute(std::move(root)).lexically_normal()) {}

void StaticFileService::ValidateRequiredFiles() const {
  if (!fs::is_directory(root_) || fs::is_symlink(fs::symlink_status(root_))) {
    throw std::runtime_error("web static directory is missing or unsafe: " +
                             root_.string());
  }
  for (const auto required_file : kRequiredFiles) {
    const fs::path relative_file(required_file);
    if (!IsRegularFileWithoutSymlinks(relative_file)) {
      throw std::runtime_error("required web static file is missing or unsafe: " +
                               (root_ / relative_file).string());
    }
  }
}

StaticFileResponse StaticFileService::Read(std::string_view request_path) const {
  fs::path relative_file;
  if (request_path == "/") {
    relative_file = "index.html";
  } else if (request_path == "/install") {
    relative_file = "install.html";
  } else if (request_path == "/agents" ||
             request_path.starts_with("/agents/")) {
    relative_file = "agents.html";
  } else if (request_path == "/admin/packages") {
    relative_file = "admin/packages.html";
  } else if (request_path.starts_with("/assets/")) {
    const auto validation =
        zfleet::core::ValidateRelativePath(request_path.substr(1));
    if (!validation.ok) {
      return {};
    }
    relative_file = validation.value;
    if (!IsPermittedAssetExtension(relative_file)) {
      return {};
    }
  } else {
    return {};
  }
  return ReadFile(relative_file);
}

const fs::path& StaticFileService::root() const noexcept {
  return root_;
}

bool StaticFileService::IsRegularFileWithoutSymlinks(
    const fs::path& relative_path) const {
  fs::path current = root_;
  for (const auto& component : relative_path) {
    current /= component;
    const auto status = fs::symlink_status(current);
    if (!fs::exists(status) || fs::is_symlink(status)) {
      return false;
    }
  }
  return fs::is_regular_file(current);
}

StaticFileResponse StaticFileService::ReadFile(
    const fs::path& relative_path) const {
  if (!IsRegularFileWithoutSymlinks(relative_path)) {
    return {};
  }
  std::ifstream stream(root_ / relative_path, std::ios::binary);
  if (!stream) {
    return {};
  }
  std::ostringstream body;
  body << stream.rdbuf();
  return StaticFileResponse{
      .status = 200,
      .content_type = ContentTypeForPath(relative_path),
      .body = body.str(),
  };
}

}  // namespace zfleet::server
