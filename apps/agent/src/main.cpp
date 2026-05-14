#include "config.h"
#include "state.h"

#include "zfleet/core/log.h"
#include "zfleet/core/version.h"
#include "zfleet/platform/system.h"
#include "zfleet/protocol/message.h"

#include <CLI/CLI.hpp>

#include <filesystem>
#include <optional>
#include <sstream>

int main(int argc, char** argv) {
  CLI::App app{"z-fleet agent"};

  std::string config_path_arg;
  std::string data_dir_arg;
  std::string log_level_arg;
  app.add_option("--config", config_path_arg, "Path to agent config file");
  app.add_option("--data-dir", data_dir_arg, "Override agent data directory");
  app.add_option("--log-level", log_level_arg, "Override agent log level");

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
    if (!log_level_arg.empty()) {
      config.log.level = zfleet::core::log::ParseLevel(log_level_arg);
    }

    zfleet::core::log::Init(config.log);

    const auto state_path = zfleet::agent::StatePathFor(config);
    const auto state = zfleet::agent::LoadOrCreateState(state_path);
    const auto logger = zfleet::core::log::Component("agent").With(
        {{"agent_id", state.agent_id},
         {"server_url", config.server_url},
         {"data_dir", config.data_dir.string()}});

    std::ostringstream message;
    message << zfleet::core::project_name() << " agent "
            << zfleet::core::version() << " protocol "
            << zfleet::protocol::protocol_version() << " on "
            << zfleet::platform::os_name() << " hostname="
            << zfleet::platform::hostname() << " arch="
            << zfleet::platform::architecture_name();
    zfleet::core::log::Write(
        zfleet::core::log::Level::kInfo, logger, message.str());
    zfleet::core::log::Shutdown();
    return 0;
  } catch (const std::exception& ex) {
    zfleet::core::log::Write(zfleet::core::log::Level::kError,
                             zfleet::core::log::Component("agent"),
                             std::string("agent startup failed: ") + ex.what());
    zfleet::core::log::Shutdown();
    return 1;
  }
}
