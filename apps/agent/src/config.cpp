#include "config.h"

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <toml++/toml.hpp>

namespace zfleet::agent {
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

AgentConfig LoadConfig(
    const std::optional<std::filesystem::path>& config_path) {
  AgentConfig config;

  if (!config_path.has_value()) {
    return config;
  }

  const auto table = ParseTomlFileOrThrow(*config_path);

  LoadLogConfig(table, &config.log);

  const auto* agent = table.get_as<toml::table>("agent");
  if (agent == nullptr) {
    return config;
  }

  if (const auto* node = agent->get("control_url"); node != nullptr) {
    if (const auto value = node->value<std::string>(); value.has_value()) {
      config.control_url = *value;
    }
  }
  if (const auto* node = agent->get("registration_token"); node != nullptr) {
    if (const auto value = node->value<std::string>(); value.has_value()) {
      config.registration_token = *value;
    }
  }

  if (const auto* node = agent->get("data_dir"); node != nullptr) {
    if (const auto value = node->value<std::string>(); value.has_value()) {
      config.data_dir = *value;
    }
  }

  if (const auto node = agent->get("state_path"); node != nullptr) {
    if (const auto value = node->value<std::string>(); value.has_value()) {
      config.state_path = *value;
    }
  }

  if (const auto* node = agent->get("heartbeat_interval_seconds");
      node != nullptr) {
    if (const auto value = node->value<std::uint32_t>(); value.has_value()) {
      config.heartbeat_interval_seconds = *value;
    }
  }

  if (const auto* node = agent->get("reconnect_initial_delay_seconds");
      node != nullptr) {
    if (const auto value = node->value<std::uint32_t>(); value.has_value()) {
      config.reconnect_initial_delay_seconds = *value;
    }
  }

  if (const auto* node = agent->get("reconnect_max_delay_seconds");
      node != nullptr) {
    if (const auto value = node->value<std::uint32_t>(); value.has_value()) {
      config.reconnect_max_delay_seconds = *value;
    }
  }

  return config;
}

std::filesystem::path DefaultConfigPath(
    const std::optional<std::filesystem::path>& install_dir) {
  if (install_dir.has_value()) {
    return *install_dir / "etc" / "agent.toml";
  }
  return "agent.toml";
}

void SaveConfig(const AgentConfig& config,
                const std::filesystem::path& config_path) {
  if (config_path.has_parent_path()) {
    std::filesystem::create_directories(config_path.parent_path());
  }

  toml::table root;
  root.insert(
      "agent",
      toml::table{
          {"control_url", config.control_url},
          {"registration_token", config.registration_token},
          {"data_dir", PathToConfigString(config.data_dir)},
          {"state_path", PathToConfigString(config.state_path)},
          {"heartbeat_interval_seconds",
           static_cast<std::int64_t>(config.heartbeat_interval_seconds)},
          {"reconnect_initial_delay_seconds",
           static_cast<std::int64_t>(config.reconnect_initial_delay_seconds)},
          {"reconnect_max_delay_seconds",
           static_cast<std::int64_t>(config.reconnect_max_delay_seconds)},
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

void ResolveConfigPaths(AgentConfig* config) {
  config->data_dir = ResolvePath(config->install_dir, config->data_dir);
  config->state_path = ResolvePath(config->install_dir, config->state_path);
  config->log.file_path = ResolvePath(config->install_dir,
                                      config->log.file_path);
}

std::filesystem::path StatePathFor(const AgentConfig& config) {
  if (config.state_path.is_absolute()) {
    return config.state_path;
  }

  return config.data_dir / config.state_path;
}

}  // namespace zfleet::agent
