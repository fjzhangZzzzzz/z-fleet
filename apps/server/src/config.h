#pragma once

#include "zfleet/core/log.h"

#include <filesystem>
#include <optional>
#include <string>

namespace zfleet::server {

struct ServerConfig {
  std::string listen = "127.0.0.1:8080";
  std::filesystem::path database_path = "data/zfleet.db";
  zfleet::core::log::Config log{
      .level = zfleet::core::log::Level::kInfo,
      .enable_console = true,
      .file_path = "logs/zfleet-server.log",
  };
};

ServerConfig LoadConfig(const std::optional<std::filesystem::path>& config_path);

} // namespace zfleet::server
