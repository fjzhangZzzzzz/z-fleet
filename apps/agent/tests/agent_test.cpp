#include "config.h"
#include "command_decoder.h"
#include "state.h"

#include "test_util.h"

#include "zfleet/protocol/v1/agent_control.pb.h"
#include "zfleet/transport/frame_codec.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

TEST_CASE("agent config loads values from toml and resolves state path") {
  const zfleet::test::ScopedTestDir test_dir("agent");
  const auto test_root = test_dir.path();

  const auto config_path = test_root / "agent.toml";
  {
    std::ofstream config_stream(config_path);
    REQUIRE(config_stream);
    config_stream << "[agent]\n";
    config_stream << "control_url = \"http://127.0.0.1:18081\"\n";
    config_stream << "heartbeat_interval_seconds = 5\n";
    config_stream << "reconnect_initial_delay_seconds = 1\n";
    config_stream << "reconnect_max_delay_seconds = 3\n";
    config_stream << "data_dir = \"data\"\n";
    config_stream << "state_path = \"state/agent-state.toml\"\n";
    config_stream << "\n[log]\n";
    config_stream << "level = \"error\"\n";
    config_stream << "file = \"logs/agent.log\"\n";
    config_stream << "enable_console = false\n";
  }

  auto config = zfleet::agent::LoadConfig(config_path);
  config.install_dir = test_root;
  zfleet::agent::ResolveConfigPaths(&config);
  const auto state_path = zfleet::agent::StatePathFor(config);

  REQUIRE(config.install_dir == test_root);
  REQUIRE(config.control_url == "http://127.0.0.1:18081");
  REQUIRE(config.data_dir == test_root / "data");
  REQUIRE(config.heartbeat_interval_seconds == 5);
  REQUIRE(config.reconnect_initial_delay_seconds == 1);
  REQUIRE(config.reconnect_max_delay_seconds == 3);
  REQUIRE(config.log.level == zfleet::core::log::Level::kError);
  REQUIRE(config.log.file_path == test_root / "logs" / "agent.log");
  REQUIRE_FALSE(config.log.enable_console);
  REQUIRE(state_path == test_root / "state" / "agent-state.toml");
}

TEST_CASE("agent config persists defaults and CLI overrides without install dir") {
  const zfleet::test::ScopedTestDir test_dir("agent");
  const auto test_root = test_dir.path();
  const auto config_path = zfleet::agent::DefaultConfigPath(
      std::optional<std::filesystem::path>{test_root});

  zfleet::agent::AgentConfig config;
  config.install_dir = test_root;
  config.control_url = "http://127.0.0.1:18081";
  config.log.level = zfleet::core::log::Level::kDebug;

  zfleet::agent::SaveConfig(config, config_path);

  REQUIRE(config_path == test_root / "etc" / "agent.toml");
  const auto saved = zfleet::test::ReadTextFile(config_path);
  REQUIRE(saved.find("install_dir") == std::string::npos);
  REQUIRE(saved.find("control_url") != std::string::npos);
  REQUIRE(saved.find("http://127.0.0.1:18081") != std::string::npos);
  REQUIRE(saved.find("level") != std::string::npos);
  REQUIRE(saved.find("debug") != std::string::npos);

  auto loaded = zfleet::agent::LoadConfig(config_path);
  loaded.install_dir = test_root;
  zfleet::agent::ResolveConfigPaths(&loaded);

  REQUIRE(loaded.install_dir == test_root);
  REQUIRE(loaded.control_url == "http://127.0.0.1:18081");
  REQUIRE(loaded.data_dir == test_root / "data" / "agent");
  REQUIRE(loaded.log.level == zfleet::core::log::Level::kDebug);
}

TEST_CASE("agent state is persisted and reused across restarts") {
  const zfleet::test::ScopedTestDir test_dir("agent");
  const auto test_root = test_dir.path();

  const zfleet::agent::AgentConfig config{
      .data_dir = test_root / "data",
      .state_path = "agent-state.toml",
  };
  const auto state_path = zfleet::agent::StatePathFor(config);

  const auto first_state = zfleet::agent::LoadOrCreateState(state_path);
  const auto second_state = zfleet::agent::LoadOrCreateState(state_path);

  REQUIRE_FALSE(first_state.agent_id.empty());
  REQUIRE(second_state.agent_id == first_state.agent_id);
}

TEST_CASE("agent command decoder rejects invalid protobuf frames") {
  zfleet::transport::FrameDecoder decoder;
  const std::vector<std::uint8_t> invalid_payload{0xff, 0xff, 0xff};
  const auto invalid_frame = zfleet::transport::EncodeFrame(
      std::span<const std::uint8_t>{invalid_payload.data(),
                                    invalid_payload.size()});

  REQUIRE_THROWS_AS(
      zfleet::agent::DecodeServerCommands(
          &decoder, std::span<const std::uint8_t>{invalid_frame.data(),
                                                  invalid_frame.size()}),
      std::runtime_error);
}

TEST_CASE("agent command decoder waits for truncated frames") {
  zfleet::protocol::v1::ServerCommand command;
  command.set_protocol_version("v1");
  command.set_message_id("cmd-1");
  command.set_agent_id("agent-1");
  command.set_occurred_at("2026-05-22T10:00:00Z");
  command.mutable_error()->set_code(
      zfleet::protocol::v1::ERROR_CODE_INTERNAL_ERROR);
  command.mutable_error()->set_message("test");
  command.mutable_error()->set_retryable(true);

  std::string payload;
  REQUIRE(command.SerializeToString(&payload));
  const auto frame = zfleet::transport::EncodeFrame(
      std::span<const std::uint8_t>{
          reinterpret_cast<const std::uint8_t*>(payload.data()),
          payload.size()});

  zfleet::transport::FrameDecoder decoder;
  const auto partial = zfleet::agent::DecodeServerCommands(
      &decoder, std::span<const std::uint8_t>{frame.data(), frame.size() - 1});
  REQUIRE(partial.empty());

  const auto complete = zfleet::agent::DecodeServerCommands(
      &decoder,
      std::span<const std::uint8_t>{frame.data() + frame.size() - 1, 1});
  REQUIRE(complete.size() == 1);
  REQUIRE(complete.front().payload_case() ==
          zfleet::protocol::v1::ServerCommand::kError);
}
