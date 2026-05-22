#include "config.h"
#include "state.h"

#include "test_util.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

TEST_CASE("agent config loads values from toml and resolves state path") {
  const zfleet::test::ScopedTestDir test_dir("agent");
  const auto test_root = test_dir.path();

  const auto config_path = test_root / "agent.toml";
  {
    std::ofstream config_stream(config_path);
    REQUIRE(config_stream);
    config_stream << "[agent]\n";
    config_stream << "server_url = \"http://127.0.0.1:18080\"\n";
    config_stream << "control_url = \"http://127.0.0.1:18081\"\n";
    config_stream << "heartbeat_interval_seconds = 5\n";
    config_stream << "reconnect_initial_delay_seconds = 1\n";
    config_stream << "reconnect_max_delay_seconds = 3\n";
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
  REQUIRE(config.control_url == "http://127.0.0.1:18081");
  REQUIRE(config.heartbeat_interval_seconds == 5);
  REQUIRE(config.reconnect_initial_delay_seconds == 1);
  REQUIRE(config.reconnect_max_delay_seconds == 3);
  REQUIRE(config.log.level == zfleet::core::log::Level::kError);
  REQUIRE(config.log.file_path == test_root / "agent.log");
  REQUIRE_FALSE(config.log.enable_console);
  REQUIRE(state_path == test_root / "data" / "agent-state.toml");
}

TEST_CASE("agent state is persisted and reused across restarts") {
  const zfleet::test::ScopedTestDir test_dir("agent");
  const auto test_root = test_dir.path();

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
}
