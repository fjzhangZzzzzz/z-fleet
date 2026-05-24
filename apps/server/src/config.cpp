#include "config.h"

#include <fstream>
#include <stdexcept>
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

std::string PathToConfigString(const std::filesystem::path& path) {
  return path.generic_string();
}

toml::table ParseTomlFileOrThrow(const std::filesystem::path& path) {
#if TOML_EXCEPTIONS
  return toml::parse_file(path.string());
#else
  const auto parsed = toml::parse_file(path.string());
  if (!parsed) {
    throw std::runtime_error("failed to parse config: " +
                             std::string(parsed.error().description()));
  }
  return parsed.table();
#endif
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

  const auto table = ParseTomlFileOrThrow(*config_path);

  LoadLogConfig(table, &config.log);

  const auto* server = table.get_as<toml::table>("server");
  if (server == nullptr) {
    return config;
  }

  if (const auto* node = server->get("control_listen"); node != nullptr) {
    if (const auto value = node->value<std::string>(); value.has_value()) {
      config.control_listen = *value;
    }
  }

  if (const auto* node = server->get("management_listen"); node != nullptr) {
    if (const auto value = node->value<std::string>(); value.has_value()) {
      config.management_listen = *value;
    }
  }

  if (const auto* node = server->get("database_path"); node != nullptr) {
    if (const auto value = node->value<std::string>(); value.has_value()) {
      config.database_path = *value;
    }
  }

  if (const auto* node = server->get("package_repository"); node != nullptr) {
    if (const auto value = node->value<std::string>(); value.has_value()) {
      config.package_repository = *value;
    }
  }

  if (const auto* node = server->get("web_static_dir"); node != nullptr) {
    if (const auto value = node->value<std::string>(); value.has_value()) {
      config.web_static_dir = *value;
    }
  }

  return config;
}

std::filesystem::path DefaultConfigPath(
    const std::optional<std::filesystem::path>& install_dir) {
  if (install_dir.has_value()) {
    return *install_dir / "etc" / "server.toml";
  }
  return "server.toml";
}

void SaveConfig(const ServerConfig& config,
                const std::filesystem::path& config_path) {
  if (config_path.has_parent_path()) {
    std::filesystem::create_directories(config_path.parent_path());
  }

  toml::table root;
  root.insert(
      "server",
      toml::table{
          {"control_listen", config.control_listen},
          {"management_listen", config.management_listen},
          {"database_path", PathToConfigString(config.database_path)},
          {"package_repository", PathToConfigString(config.package_repository)},
          {"web_static_dir", PathToConfigString(config.web_static_dir)},
      });
  root.insert("log",
              toml::table{
                  {"level", std::string(zfleet::core::log::ToString(
                                config.log.level))},
                  {"file", PathToConfigString(config.log.file_path)},
                  {"enable_console", config.log.enable_console},
              });

  std::ofstream stream(config_path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("failed to open config for writing: " +
                             config_path.string());
  }
  stream << root << '\n';
  if (!stream) {
    throw std::runtime_error("failed to write config: " + config_path.string());
  }
}

void ResolveConfigPaths(ServerConfig* config) {
  config->database_path = ResolvePath(config->install_dir,
                                      config->database_path);
  config->package_repository = ResolvePath(config->install_dir,
                                           config->package_repository);
  config->web_static_dir = ResolvePath(config->install_dir,
                                       config->web_static_dir);
  config->log.file_path = ResolvePath(config->install_dir,
                                      config->log.file_path);
}

}  // namespace zfleet::server
