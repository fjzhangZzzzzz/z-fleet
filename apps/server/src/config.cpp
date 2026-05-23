#include "config.h"

#include <toml++/toml.hpp>

namespace zfleet::server {
namespace {

std::filesystem::path ResolvePath(
    const std::optional<std::filesystem::path>& install_dir,
    std::filesystem::path path) {
  if (path.empty() || path.is_absolute() || !install_dir.has_value()) {
    return path;
  }
  return *install_dir / path;
}

void LoadLogConfig(const toml::table& table,
                   zfleet::core::log::Config* config) {
  const auto* log = table.get_as<toml::table>("log");
  if (log == nullptr) {
    return;
  }

  if (const auto* node = log->get("level"); node != nullptr) {
    if (const auto value = node->value<std::string>(); value.has_value()) {
      config->level = zfleet::core::log::ParseLevel(*value);
    }
  }

  if (const auto* node = log->get("file"); node != nullptr) {
    if (const auto value = node->value<std::string>(); value.has_value()) {
      config->file_path = *value;
    }
  }

  if (const auto* node = log->get("enable_console"); node != nullptr) {
    if (const auto value = node->value<bool>(); value.has_value()) {
      config->enable_console = *value;
    }
  }
}

}  // namespace

ServerConfig LoadConfig(
    const std::optional<std::filesystem::path>& config_path) {
  ServerConfig config;

  if (!config_path.has_value()) {
    return config;
  }

  const auto table = toml::parse_file(config_path->string());

  LoadLogConfig(table, &config.log);

  const auto* server = table.get_as<toml::table>("server");
  if (server == nullptr) {
    return config;
  }

  if (const auto* node = server->get("install_dir"); node != nullptr) {
    if (const auto value = node->value<std::string>(); value.has_value()) {
      config.install_dir = std::filesystem::path(*value);
    }
  }

  if (const auto* node = server->get("control_listen"); node != nullptr) {
    if (const auto value = node->value<std::string>(); value.has_value()) {
      config.control_listen = *value;
    }
  }

  if (const auto* node = server->get("database_path"); node != nullptr) {
    if (const auto value = node->value<std::string>(); value.has_value()) {
      config.database_path = *value;
    }
  }

  return config;
}

void ResolveConfigPaths(ServerConfig* config) {
  config->database_path = ResolvePath(config->install_dir,
                                      config->database_path);
  config->log.file_path = ResolvePath(config->install_dir,
                                      config->log.file_path);
}

}  // namespace zfleet::server
