#include "config.h"

#include <toml++/toml.hpp>

namespace zfleet::server {

ServerConfig LoadConfig(const std::optional<std::filesystem::path>& config_path) {
  ServerConfig config;

  if (!config_path.has_value()) {
    return config;
  }

  const auto table = toml::parse_file(config_path->string());
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

} // namespace zfleet::server
