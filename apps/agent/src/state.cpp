#include "state.h"

#include "zfleet/core/uuid.h"

#include <toml++/toml.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace zfleet::agent {
namespace {

toml::table ParseTomlFileOrThrow(const std::filesystem::path& path) {
#if TOML_EXCEPTIONS
  return toml::parse_file(path.string());
#else
  const auto parsed = toml::parse_file(path.string());
  if (!parsed) {
    throw std::runtime_error("failed to parse state: " +
                             std::string(parsed.error().description()));
  }
  return parsed.table();
#endif
}

AgentState parse_state(const std::filesystem::path& state_path) {
  const auto table = ParseTomlFileOrThrow(state_path);
  const auto* state = table.get_as<toml::table>("state");
  if (state == nullptr) {
    throw std::runtime_error("missing [state] table");
  }

  const auto value = state->get("agent_id");
  if (value == nullptr) {
    throw std::runtime_error("missing state.agent_id");
  }

  const auto agent_id = value->value<std::string>();
  if (!agent_id.has_value() || agent_id->empty()) {
    throw std::runtime_error("invalid state.agent_id");
  }

  return AgentState{.agent_id = *agent_id};
}

void write_state(const std::filesystem::path& state_path,
                 const AgentState& state) {
  toml::table root;
  root.insert("state",
              toml::table{{"agent_id", state.agent_id}});

  std::filesystem::create_directories(state_path.parent_path());

  std::ofstream stream(state_path);
  if (!stream) {
    throw std::runtime_error("failed to open state file for writing");
  }

  stream << root;
  if (!stream) {
    throw std::runtime_error("failed to write state file");
  }
}

} // namespace

AgentState LoadOrCreateState(const std::filesystem::path& state_path) {
  if (std::filesystem::exists(state_path)) {
    return parse_state(state_path);
  }

  AgentState state{.agent_id = zfleet::core::GenerateUuid()};
  write_state(state_path, state);
  return state;
}

} // namespace zfleet::agent
