#include "config.h"

#include <stdexcept>
#include <toml++/toml.hpp>

namespace zfleet::agent {
namespace {

std::filesystem::path resolve_data_dir(const toml::table& agent) {
  if (const auto value = agent["data_dir"].value<std::string>();
      value.has_value()) {
    return *value;
  }

  return AgentConfig{}.data_dir;
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

  const auto table = toml::parse_file(config_path->string());

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

  config.data_dir = resolve_data_dir(*agent);

  if (const auto node = agent->get("state_file"); node != nullptr) {
    if (const auto value = node->value<std::string>(); value.has_value()) {
      config.state_file = *value;
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

std::filesystem::path StatePathFor(const AgentConfig& config) {
  const std::filesystem::path state_file(config.state_file);
  if (state_file.is_absolute()) {
    return state_file;
  }

  return config.data_dir / state_file;
}

}  // namespace zfleet::agent
