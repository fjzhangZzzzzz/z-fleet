#include "config.h"

#include <toml++/toml.hpp>

namespace zfleet::server {
namespace {

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

  if (const auto* node = server->get("listen"); node != nullptr) {
    if (const auto value = node->value<std::string>(); value.has_value()) {
      config.listen = *value;
    }
  }

  if (const auto* node = server->get("database_path"); node != nullptr) {
    if (const auto value = node->value<std::string>(); value.has_value()) {
      config.database_path = *value;
    }
  }

  return config;
}

}  // namespace zfleet::server
