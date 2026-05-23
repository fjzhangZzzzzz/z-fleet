#include "config.h"
#include "runtime.h"

#include "zfleet/core/log.h"

#include <CLI/CLI.hpp>

#include <chrono>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <optional>
#include <thread>

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;
constexpr char kComponentRootEnvVar[] = "ZFLEET_COMPONENT_ROOT";

void HandleStopSignal(int /*signal*/) {
  g_stop_requested = 1;
}

std::filesystem::path DetectInstallDir() {
  if (const char* value = std::getenv(kComponentRootEnvVar);
      value != nullptr && value[0] != '\0') {
    return std::filesystem::path(value);
  }
  return std::filesystem::current_path();
}

} // namespace

int main(int argc, char** argv) {
  CLI::App app{"z-fleet agent"};

  std::string config_path_arg;
  std::string control_url_arg;
  std::string data_dir_arg;
  std::string state_path_arg;
  std::string log_level_arg;
  app.add_option("-c,--config", config_path_arg, "Path to agent config file");
  app.add_option("--control-url", control_url_arg,
                 "Override server HTTP/2 control URL");
  app.add_option("--data-dir", data_dir_arg, "Override agent data directory");
  app.add_option("--state-path", state_path_arg, "Override agent state path");
  app.add_option("--log-level", log_level_arg, "Override agent log level");

  CLI11_PARSE(app, argc, argv);

  try {
    const auto install_dir = DetectInstallDir();
    const auto config_path =
        config_path_arg.empty()
            ? zfleet::agent::DefaultConfigPath(
                  std::optional<std::filesystem::path>{install_dir})
            : std::filesystem::path{config_path_arg};
    auto config = std::filesystem::exists(config_path)
                      ? zfleet::agent::LoadConfig(
                            std::optional<std::filesystem::path>{config_path})
                      : zfleet::agent::AgentConfig{};
    config.install_dir = install_dir;
    if (!data_dir_arg.empty()) {
      config.data_dir = data_dir_arg;
    }
    if (!state_path_arg.empty()) {
      config.state_path = state_path_arg;
    }
    if (!control_url_arg.empty()) {
      config.control_url = control_url_arg;
    }
    if (!log_level_arg.empty()) {
      config.log.level = zfleet::core::log::ParseLevel(log_level_arg);
    }
    zfleet::agent::SaveConfig(config, config_path);
    zfleet::agent::ResolveConfigPaths(&config);

    zfleet::core::log::Init(config.log);
    std::signal(SIGINT, HandleStopSignal);
    std::signal(SIGTERM, HandleStopSignal);
    zfleet::agent::AgentRuntime runtime(config);
    std::thread signal_monitor([&runtime]() {
      while (!runtime.stop_requested()) {
        if (g_stop_requested != 0) {
          runtime.RequestStop();
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    });
    try {
      runtime.Run();
      runtime.RequestStop();
      if (signal_monitor.joinable()) {
        signal_monitor.join();
      }
    } catch (...) {
      runtime.RequestStop();
      if (signal_monitor.joinable()) {
        signal_monitor.join();
      }
      throw;
    }
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
