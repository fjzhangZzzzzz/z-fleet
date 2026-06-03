#include "config.h"
#include "command_decoder.h"
#include "package_updater.h"
#include "state.h"

#include "test_util.h"

#include "zfleet/protocol/v1/agent_control.pb.h"
#include "zfleet/crypto/sha256.h"
#include "zfleet/package/archive.h"
#include "zfleet/platform/file_permissions.h"
#include "zfleet/transport/frame_codec.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

class SingleResponseServer {
 public:
  explicit SingleResponseServer(std::string body)
      : acceptor_(io_context_,
                  {boost::asio::ip::make_address("127.0.0.1"), 0}),
        body_(std::move(body)),
        thread_([this]() { Serve(); }) {}

  ~SingleResponseServer() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  std::string url() const {
    return "http://127.0.0.1:" +
           std::to_string(acceptor_.local_endpoint().port()) + "/package.zip";
  }

 private:
  void Serve() {
    boost::asio::ip::tcp::socket socket(io_context_);
    acceptor_.accept(socket);
    std::array<char, 4096> request{};
    boost::system::error_code ignored;
    socket.read_some(boost::asio::buffer(request), ignored);
    const auto response =
        "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nContent-Length: " +
        std::to_string(body_.size()) + "\r\nConnection: close\r\n\r\n" + body_;
    boost::asio::write(socket, boost::asio::buffer(response));
  }

  boost::asio::io_context io_context_;
  boost::asio::ip::tcp::acceptor acceptor_;
  std::string body_;
  std::thread thread_;
};

std::string ReadBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

std::filesystem::path MakePackageArchive(const std::filesystem::path& root) {
  const auto package_dir = root / "package";
  zfleet::test::WriteTextFile(package_dir / "META" / "manifest.json",
                              "{\"test\":true}\n");
  zfleet::test::WriteTextFile(package_dir / "payload" / "bin" / "payload",
                              "payload");
  const auto archive = root / "installer.zip";
  zfleet::package::CreateArchive({
      .package_dir = package_dir,
      .archive_path = archive,
      .force = true,
  });
  return archive;
}

} // namespace

TEST_CASE("agent config loads values from toml and resolves state path") {
  const zfleet::test::ScopedTestDir test_dir("agent");
  const auto test_root = test_dir.path();

  const auto config_path = test_root / "agent.toml";
  {
    std::ofstream config_stream(config_path);
    REQUIRE(config_stream);
    config_stream << "[agent]\n";
    config_stream << "control_url = \"http://127.0.0.1:18081\"\n";
    config_stream << "registration_token = \"register-once\"\n";
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
  REQUIRE(config.registration_token == "register-once");
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
  config.registration_token = "register-once";
  config.log.level = zfleet::core::log::Level::kDebug;

  zfleet::agent::SaveConfig(config, config_path);

  REQUIRE(config_path == test_root / "etc" / "agent.toml");
  const auto saved = zfleet::test::ReadTextFile(config_path);
  REQUIRE(saved.find("install_dir") == std::string::npos);
  REQUIRE(saved.find("control_url") != std::string::npos);
  REQUIRE(saved.find("http://127.0.0.1:18081") != std::string::npos);
  REQUIRE(saved.find("register-once") != std::string::npos);
  REQUIRE(saved.find("level") != std::string::npos);
  REQUIRE(saved.find("debug") != std::string::npos);

  auto loaded = zfleet::agent::LoadConfig(config_path);
  loaded.install_dir = test_root;
  zfleet::agent::ResolveConfigPaths(&loaded);

  REQUIRE(loaded.install_dir == test_root);
  REQUIRE(loaded.control_url == "http://127.0.0.1:18081");
  REQUIRE(loaded.registration_token == "register-once");
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
  REQUIRE(std::holds_alternative<zfleet::protocol::ServerError>(
      complete.front().payload));
}

#ifndef _WIN32
TEST_CASE("package updater downloads verifies and invokes active installer") {
  const zfleet::test::ScopedTestDir test_dir("agent-updater");
  const auto root = test_dir.path();
  const auto archive = MakePackageArchive(root);
  const auto body = ReadBytes(archive);
  SingleResponseServer server(body);
  const auto marker = root / "applied";
  const auto installer = root / "installer" / "bin" / "zfleet_installer";
  zfleet::test::WriteTextFile(installer,
                              "#!/bin/sh\ntouch \"" + marker.string() +
                                  "\"\nexit 0\n");
  zfleet::platform::SetExecutable(installer, true);
  zfleet::test::WriteTextFile(root / "installer" / "var" / "active-version",
                              "0.1.0\n");
  const auto manifest =
      zfleet::package::ReadArchiveFile(archive, "META/manifest.json");

  zfleet::agent::AgentConfig config{
      .install_dir = root / "agent",
      .data_dir = root / "agent-data",
  };
  const auto result = zfleet::agent::ExecutePackageUpdate(
      config, zfleet::protocol::PackageUpdateInput{
                  .component = "installer",
                  .package_id = "installer-1",
                  .version = "0.1.0",
                  .package_url = server.url(),
                  .package_sha256 = zfleet::crypto::Sha256BytesHex(body),
                  .manifest_sha256 = zfleet::crypto::Sha256BytesHex(
                      std::string_view(
                          reinterpret_cast<const char*>(manifest.data()),
                          manifest.size())),
              });

  REQUIRE(result.ok);
  REQUIRE_FALSE(result.stop_agent);
  REQUIRE(std::filesystem::exists(marker));
}

TEST_CASE("package updater rejects checksum mismatch before apply") {
  const zfleet::test::ScopedTestDir test_dir("agent-updater");
  const auto root = test_dir.path();
  const auto archive = MakePackageArchive(root);
  SingleResponseServer server(ReadBytes(archive));
  const auto marker = root / "applied";
  const auto installer = root / "installer" / "bin" / "zfleet_installer";
  zfleet::test::WriteTextFile(installer,
                              "#!/bin/sh\ntouch \"" + marker.string() +
                                  "\"\nexit 0\n");
  zfleet::platform::SetExecutable(installer, true);
  zfleet::test::WriteTextFile(root / "installer" / "var" / "active-version",
                              "0.1.0\n");

  const auto result = zfleet::agent::ExecutePackageUpdate(
      zfleet::agent::AgentConfig{
          .install_dir = root / "agent",
          .data_dir = root / "agent-data",
      },
      zfleet::protocol::PackageUpdateInput{
          .component = "installer",
          .package_id = "installer-bad",
          .version = "0.1.0",
          .package_url = server.url(),
          .package_sha256 = std::string(64, '0'),
      });

  REQUIRE_FALSE(result.ok);
  REQUIRE(result.error_code == zfleet::protocol::ErrorCode::checksum_mismatch);
  REQUIRE_FALSE(std::filesystem::exists(marker));
}

TEST_CASE("package updater rolls back agent and starts restored release") {
  const zfleet::test::ScopedTestDir test_dir("agent-updater");
  const auto root = test_dir.path();
  const auto started = root / "started";
  const auto installer = root / "installer" / "bin" / "zfleet_installer";
  zfleet::test::WriteTextFile(
      installer, "#!/bin/sh\nmkdir -p \"" +
                     (root / "agent" / "var").string() + "\"\nprintf '0.0.9\\n' > \"" +
                     (root / "agent" / "var" / "active-version").string() +
                      "\"\nexit 0\n");
  zfleet::platform::SetExecutable(installer, true);
  zfleet::test::WriteTextFile(root / "installer" / "var" / "active-version",
                              "0.1.0\n");
  const auto restored_agent = root / "agent" / "bin" / "zfleet_agent";
  zfleet::test::WriteTextFile(restored_agent,
                              "#!/bin/sh\ntouch \"" + started.string() +
                                  "\"\nexit 0\n");
  zfleet::platform::SetExecutable(restored_agent, true);

  const auto result = zfleet::agent::ExecutePackageUpdate(
      zfleet::agent::AgentConfig{
          .install_dir = root / "agent",
          .data_dir = root / "agent-data",
      },
      zfleet::protocol::PackageUpdateInput{
          .action = "rollback",
          .component = "agent",
      });

  REQUIRE(result.ok);
  REQUIRE(result.stop_agent);
  for (int attempt = 0; attempt < 20 && !std::filesystem::exists(started);
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(std::filesystem::exists(started));
}
#endif
