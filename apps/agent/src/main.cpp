#include "config.h"
#include "state.h"

#include "zfleet/core/version.h"
#include "zfleet/platform/system.h"
#include "zfleet/protocol/message.h"

#include <CLI/CLI.hpp>

#include <filesystem>
#include <iostream>
#include <optional>

int main(int argc, char** argv) {
  CLI::App app{"z-fleet agent"};

  std::string config_path_arg;
  std::string data_dir_arg;
  app.add_option("--config", config_path_arg, "Path to agent config file");
  app.add_option("--data-dir", data_dir_arg, "Override agent data directory");

  CLI11_PARSE(app, argc, argv);

  try {
    const auto config_path =
        config_path_arg.empty()
            ? std::optional<std::filesystem::path>{}
            : std::optional<std::filesystem::path>{config_path_arg};
    auto config = zfleet::agent::LoadConfig(config_path);
    if (!data_dir_arg.empty()) {
      config.data_dir = data_dir_arg;
    }

    const auto state_path = zfleet::agent::StatePathFor(config);
    const auto state = zfleet::agent::LoadOrCreateState(state_path);

    std::cout << zfleet::core::project_name() << " agent "
              << zfleet::core::version() << " protocol "
              << zfleet::protocol::protocol_version() << " on "
              << zfleet::platform::os_name() << '\n'
              << "agent_id=" << state.agent_id << '\n'
              << "server_url=" << config.server_url << '\n'
              << "data_dir=" << config.data_dir.string() << '\n'
              << "hostname=" << zfleet::platform::hostname() << '\n'
              << "arch=" << zfleet::platform::architecture_name() << '\n';
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "agent startup failed: " << ex.what() << '\n';
    return 1;
  }
}
