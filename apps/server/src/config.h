#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace zfleet::server {

struct ServerConfig {
  std::string listen = "127.0.0.1:8080";
  std::filesystem::path database_path = "data/zfleet.db";
};

ServerConfig LoadConfig(const std::optional<std::filesystem::path>& config_path);

} // namespace zfleet::server
