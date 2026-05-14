#include "state.h"

#include "zfleet/core/uuid.h"

#include <toml++/toml.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace zfleet::agent {
namespace {

AgentState parse_state(const std::filesystem::path& state_path) {
  const auto table = toml::parse_file(state_path.string());
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
