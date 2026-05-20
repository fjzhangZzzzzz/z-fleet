#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace zfleet::installer {

struct ApplyResult {
  bool ok;
  std::string component;
  std::string version;
  std::string message;
};

struct StatusResult {
  std::string component;
  std::string state;
  std::optional<std::string> version;
  std::optional<std::string> message;
};

struct RollbackResult {
  bool ok;
  std::string component;
  std::string from_version;
  std::string to_version;
  std::string message;
};

ApplyResult ApplyPackage(const std::filesystem::path& root,
                         const std::filesystem::path& package_dir);
RollbackResult RollbackComponent(const std::filesystem::path& root,
                                 const std::string& component);
StatusResult GetStatus(const std::filesystem::path& root,
                       const std::string& component);

} // namespace zfleet::installer
