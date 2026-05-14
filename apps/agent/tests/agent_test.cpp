#include "config.h"
#include "state.h"

#include <filesystem>
#include <fstream>
#include <string>

int main() {
  namespace fs = std::filesystem;

  const auto test_root =
      fs::temp_directory_path() / "zfleet-agent-tests" / "identity-config";
  fs::remove_all(test_root);
  fs::create_directories(test_root);

  const auto config_path = test_root / "agent.toml";
  {
    std::ofstream config_stream(config_path);
    if (!config_stream) {
      return 1;
    }

    config_stream << "[agent]\n";
    config_stream << "server_url = \"http://127.0.0.1:18080\"\n";
    config_stream << "data_dir = \"" << (test_root / "data").string() << "\"\n";
    config_stream << "state_file = \"agent-state.toml\"\n";
  }

  const auto config = zfleet::agent::LoadConfig(config_path);
  if (config.server_url != "http://127.0.0.1:18080") {
    return 1;
  }

  const auto state_path = zfleet::agent::StatePathFor(config);
  if (state_path != test_root / "data" / "agent-state.toml") {
    return 1;
  }

  const auto first_state =
      zfleet::agent::LoadOrCreateState(state_path);
  if (first_state.agent_id.empty()) {
    return 1;
  }

  const auto second_state =
      zfleet::agent::LoadOrCreateState(state_path);
  if (second_state.agent_id != first_state.agent_id) {
    return 1;
  }

  fs::remove_all(test_root);
  return 0;
}
