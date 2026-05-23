#pragma once

#include "zfleet/core/log.h"

#include <filesystem>
#include <optional>
#include <string>

namespace zfleet::server {

struct ServerConfig {
  std::optional<std::filesystem::path> install_dir = std::nullopt;
  std::string control_listen = "127.0.0.1:8081";
  std::filesystem::path database_path = "data/zfleet.db";
  zfleet::core::log::Config log{
      .level = zfleet::core::log::Level::kInfo,
      .enable_console = true,
      .file_path = "logs/zfleet-server.log",
  };
};

ServerConfig LoadConfig(const std::optional<std::filesystem::path>& config_path);
std::filesystem::path DefaultConfigPath(
    const std::optional<std::filesystem::path>& install_dir);
void SaveConfig(const ServerConfig& config,
                const std::filesystem::path& config_path);
void ResolveConfigPaths(ServerConfig* config);

} // namespace zfleet::server
