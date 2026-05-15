#include "app.h"
#include "config.h"

#include "zfleet/core/log.h"

#include <CLI/CLI.hpp>

#include <filesystem>
#include <optional>

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
    zfleet::agent::RunOnce(config);
    zfleet::core::log::Shutdown();
    return 0;
  } catch (const std::exception& ex) {
    ZFLOG_ERROR(zfleet::core::log::Component("agent"),
                "agent startup failed: {}",
                ex.what());
    zfleet::core::log::Shutdown();
    return 1;
  }
}
