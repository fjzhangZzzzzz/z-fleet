#include "config.h"
#include "state.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path MakeTestRoot() {
  return std::filesystem::temp_directory_path() / "zfleet-agent-tests" /
         "identity-config";
}

} // namespace

TEST_CASE("agent config loads values from toml and resolves state path") {
  namespace fs = std::filesystem;

  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);

  const auto config_path = test_root / "agent.toml";
  {
    std::ofstream config_stream(config_path);
    REQUIRE(config_stream);
    config_stream << "[agent]\n";
    config_stream << "server_url = \"http://127.0.0.1:18080\"\n";
    config_stream << "data_dir = \"" << (test_root / "data").string() << "\"\n";
    config_stream << "state_file = \"agent-state.toml\"\n";
    config_stream << "\n[log]\n";
    config_stream << "level = \"error\"\n";
    config_stream << "file = \"" << (test_root / "agent.log").string()
                  << "\"\n";
    config_stream << "enable_console = false\n";
  }

  const auto config = zfleet::agent::LoadConfig(config_path);
  const auto state_path = zfleet::agent::StatePathFor(config);

  REQUIRE(config.server_url == "http://127.0.0.1:18080");
  REQUIRE(config.log.level == zfleet::core::log::Level::kError);
  REQUIRE(config.log.file_path == test_root / "agent.log");
  REQUIRE_FALSE(config.log.enable_console);
  REQUIRE(state_path == test_root / "data" / "agent-state.toml");

  fs::remove_all(test_root);
}

TEST_CASE("agent state is persisted and reused across restarts") {
  namespace fs = std::filesystem;

  const auto test_root = MakeTestRoot();
  fs::remove_all(test_root);
  fs::create_directories(test_root);

  const zfleet::agent::AgentConfig config{
      .server_url = "http://127.0.0.1:8080",
      .data_dir = test_root / "data",
      .state_file = "agent-state.toml",
  };
  const auto state_path = zfleet::agent::StatePathFor(config);

  const auto first_state = zfleet::agent::LoadOrCreateState(state_path);
  const auto second_state = zfleet::agent::LoadOrCreateState(state_path);

  REQUIRE_FALSE(first_state.agent_id.empty());
  REQUIRE(second_state.agent_id == first_state.agent_id);

  fs::remove_all(test_root);
}
